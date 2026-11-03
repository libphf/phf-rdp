/*
 * macOS backend for capture.h
 *
 *   Screen + system audio : ScreenCaptureKit (SCStream, macOS 13+). The stream is configured at
 *                           the output resolution, so the GPU does the downscale.
 *   Mouse + keyboard      : CGEventPost (needs the Accessibility permission).
 *   Clipboard             : NSPasteboard (UTF-8 text).
 *
 * Permissions: the app that launches the server (Terminal, iTerm, VS Code...) must be allowed
 * in System Settings > Privacy & Security > Screen & System Audio Recording, and in
 * Accessibility for remote control. Without Accessibility the stream works but input is ignored.
 *
 * Key mapping (the web client sends Windows virtual-key codes):
 *   Meta/Win (0x5B/0x5C) -> Command, Ctrl (0x11) -> Control, Alt (0x12) -> Option.
 * A Mac client therefore gets native shortcuts (Cmd+C on the client = Cmd+C on the host).
 */
#include "capture.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Shared state
 * ============================================================================ */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    CGDirectDisplayID id;
    int pixel_w, pixel_h;   /* native pixels */
    CGRect bounds;          /* global coordinates, points */
    int is_primary;
    char name[128];
} mac_display_t;

static mac_display_t s_displays[PHF_MAX_MONITORS];
static int s_display_count = 0;
static int s_active = 0;

static int s_req_w = 0, s_req_h = 0;   /* client request (0 = native) */
static int s_target_w = 0, s_target_h = 0;
static int s_fps = 60;
static atomic_int s_draw_cursor = 1;
static atomic_int s_res_dirty = 0;
static atomic_int s_mon_dirty = 0;
static atomic_int s_cursor_dirty = 0;

static CVPixelBufferRef s_latest_pb = NULL;   /* guarded by s_lock */
static int s_latest_is_new = 0;               /* guarded by s_lock */
static int64_t s_last_delivered_us = 0;

static uint8_t *s_rgb = NULL;
static size_t s_rgb_cap = 0;

static SCStream *s_stream = nil;
@class PHFStreamOutput;
static PHFStreamOutput *s_output = nil;
static dispatch_queue_t s_video_queue = NULL;
static dispatch_queue_t s_audio_queue = NULL;

/* Audio ring: 24 kHz stereo int16 (48 kHz input, 2:1 decimation, like the Windows backend). */
#define AUD_RING_FRAMES (24000 * 2)
static int16_t s_aud_ring[AUD_RING_FRAMES * 2];
static size_t s_aud_head = 0, s_aud_count = 0;   /* guarded by s_lock */
static int s_aud_have_carry = 0;
static float s_aud_carry_l = 0.f, s_aud_carry_r = 0.f;
static atomic_int s_audio_wanted = 0;

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ============================================================================
 * Resolution policy (same rules as the Windows backend: never upscale, keep aspect)
 * ============================================================================ */

static void compute_smart_resolution(int screen_w, int screen_h, int req_w, int req_h,
                                     int *out_w, int *out_h) {
    if (screen_w <= 0 || screen_h <= 0) { screen_w = 1920; screen_h = 1080; }
    if (req_w <= 0 || req_h <= 0 || (req_w >= screen_w && req_h >= screen_h)) {
        *out_w = screen_w; *out_h = screen_h; return;
    }
    double sx = (double)req_w / screen_w, sy = (double)req_h / screen_h;
    double scale = sx < sy ? sx : sy;
    if (scale >= 1.0) { *out_w = screen_w; *out_h = screen_h; return; }
    int w = ((int)(screen_w * scale + 0.5) + 1) & ~1;
    int h = ((int)(screen_h * scale + 0.5) + 1) & ~1;
    if (w < 320) w = 320;
    if (h < 180) h = 180;
    if (w > screen_w) w = screen_w;
    if (h > screen_h) h = screen_h;
    *out_w = w; *out_h = h;
}

/* ============================================================================
 * Display enumeration
 * ============================================================================ */

static SCShareableContent *fetch_shareable_content(void) {
    __block SCShareableContent *result = nil;
    __block NSError *err = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *c, NSError *e) {
        result = c;
        err = e;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (!result) {
        fprintf(stderr, "[Capture] ScreenCaptureKit: no shareable content (%s).\n"
                        "          Allow your terminal in System Settings > Privacy & Security >\n"
                        "          Screen & System Audio Recording, then restart it.\n",
                err ? err.localizedDescription.UTF8String : "timeout");
    }
    return result;
}

static void refresh_displays(void) {
    CGDirectDisplayID ids[PHF_MAX_MONITORS];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(PHF_MAX_MONITORS, ids, &n) != kCGErrorSuccess) n = 0;
    CGDirectDisplayID main_id = CGMainDisplayID();

    /* Main display first, so monitor 0 is the primary one. */
    int count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n && count < PHF_MAX_MONITORS; i++) {
            int is_main = (ids[i] == main_id);
            if ((pass == 0) != is_main) continue;
            mac_display_t *d = &s_displays[count++];
            memset(d, 0, sizeof(*d));
            d->id = ids[i];
            d->bounds = CGDisplayBounds(ids[i]);
            d->is_primary = is_main;
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
            if (mode) {
                d->pixel_w = (int)CGDisplayModeGetPixelWidth(mode);
                d->pixel_h = (int)CGDisplayModeGetPixelHeight(mode);
                CGDisplayModeRelease(mode);
            }
            if (d->pixel_w <= 0) d->pixel_w = (int)CGDisplayPixelsWide(ids[i]);
            if (d->pixel_h <= 0) d->pixel_h = (int)CGDisplayPixelsHigh(ids[i]);

            NSString *name = nil;
            for (NSScreen *scr in [NSScreen screens]) {
                NSNumber *sid = scr.deviceDescription[@"NSScreenNumber"];
                if (sid && sid.unsignedIntValue == ids[i]) { name = scr.localizedName; break; }
            }
            snprintf(d->name, sizeof(d->name), "%s%s (%dx%d)",
                     name ? name.UTF8String : "Display",
                     CGDisplayIsBuiltin(ids[i]) ? " [built-in]" : "",
                     d->pixel_w, d->pixel_h);
        }
    }
    s_display_count = count;
    if (s_active >= s_display_count) s_active = 0;
}

/* ============================================================================
 * ScreenCaptureKit stream
 * ============================================================================ */

static void audio_push(CMSampleBufferRef sb);

@interface PHFStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation PHFStreamOutput
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type {
    if (!CMSampleBufferIsValid(sb)) return;
    if (type == SCStreamOutputTypeAudio) {
        audio_push(sb);
        return;
    }
    if (type != SCStreamOutputTypeScreen) return;

    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!atts || CFArrayGetCount(atts) == 0) return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(atts, 0);
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;   /* idle/blank: no change */

    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) return;
    CVPixelBufferRetain(pb);
    pthread_mutex_lock(&s_lock);
    if (s_latest_pb) CVPixelBufferRelease(s_latest_pb);
    s_latest_pb = pb;
    s_latest_is_new = 1;
    pthread_mutex_unlock(&s_lock);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    fprintf(stderr, "[Capture] Stream stopped: %s\n", error.localizedDescription.UTF8String);
    atomic_store(&s_mon_dirty, 1);   /* restart on the next acquire */
}
@end

static SCStreamConfiguration *make_config(void) {
    SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
    cfg.width = (size_t)s_target_w;
    cfg.height = (size_t)s_target_h;
    cfg.pixelFormat = kCVPixelFormatType_32BGRA;
    cfg.minimumFrameInterval = CMTimeMake(1, s_fps);
    cfg.queueDepth = 4;
    cfg.showsCursor = atomic_load(&s_draw_cursor) ? YES : NO;
    cfg.scalesToFit = YES;
    cfg.capturesAudio = YES;
    cfg.sampleRate = 48000;
    cfg.channelCount = 2;
    cfg.excludesCurrentProcessAudio = YES;
    return cfg;
}

static void stop_stream(void) {
    if (!s_stream) return;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [s_stream stopCaptureWithCompletionHandler:^(NSError *e) { (void)e; dispatch_semaphore_signal(sem); }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    s_stream = nil;
    pthread_mutex_lock(&s_lock);
    if (s_latest_pb) { CVPixelBufferRelease(s_latest_pb); s_latest_pb = NULL; }
    s_latest_is_new = 0;
    pthread_mutex_unlock(&s_lock);
}

static int start_stream(void) {
    @autoreleasepool {
        stop_stream();
        refresh_displays();
        if (s_display_count == 0) return -1;
        mac_display_t *d = &s_displays[s_active];
        compute_smart_resolution(d->pixel_w, d->pixel_h, s_req_w, s_req_h, &s_target_w, &s_target_h);

        SCShareableContent *content = fetch_shareable_content();
        if (!content) return -1;
        SCDisplay *display = nil;
        for (SCDisplay *sd in content.displays) {
            if (sd.displayID == d->id) { display = sd; break; }
        }
        if (!display) display = content.displays.firstObject;
        if (!display) return -1;

        /* Exclude nothing: the server is a console process without windows. */
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        if (!s_output) s_output = [[PHFStreamOutput alloc] init];
        if (!s_video_queue) s_video_queue = dispatch_queue_create("phf.rdp.video", DISPATCH_QUEUE_SERIAL);
        if (!s_audio_queue) s_audio_queue = dispatch_queue_create("phf.rdp.audio", DISPATCH_QUEUE_SERIAL);

        s_stream = [[SCStream alloc] initWithFilter:filter configuration:make_config() delegate:s_output];
        NSError *err = nil;
        if (![s_stream addStreamOutput:s_output type:SCStreamOutputTypeScreen sampleHandlerQueue:s_video_queue error:&err]) {
            fprintf(stderr, "[Capture] addStreamOutput(screen) failed: %s\n", err.localizedDescription.UTF8String);
            s_stream = nil;
            return -1;
        }
        if (![s_stream addStreamOutput:s_output type:SCStreamOutputTypeAudio sampleHandlerQueue:s_audio_queue error:&err]) {
            fprintf(stderr, "[Capture] addStreamOutput(audio) failed: %s (continuing without audio)\n",
                    err.localizedDescription.UTF8String);
        }

        __block NSError *start_err = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [s_stream startCaptureWithCompletionHandler:^(NSError *e) { start_err = e; dispatch_semaphore_signal(sem); }];
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || start_err) {
            fprintf(stderr, "[Capture] startCapture failed: %s\n",
                    start_err ? start_err.localizedDescription.UTF8String : "timeout");
            s_stream = nil;
            return -1;
        }
        printf("[Capture] ScreenCaptureKit: %s -> %dx%d @ %d fps\n", d->name, s_target_w, s_target_h, s_fps);
        return 0;
    }
}

static void update_stream_config(void) {
    if (!s_stream) return;
    @autoreleasepool {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [s_stream updateConfiguration:make_config() completionHandler:^(NSError *e) {
            if (e) fprintf(stderr, "[Capture] updateConfiguration: %s\n", e.localizedDescription.UTF8String);
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    }
}

/* ============================================================================
 * capture.h: screen
 * ============================================================================ */

int capture_init(const capture_config_t *config) {
    if (config) {
        s_req_w = config->target_w;
        s_req_h = config->target_h;
        if (config->fps > 0) s_fps = config->fps;
    }
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
        fprintf(stderr, "[Capture] Screen Recording permission is missing. Grant it to the app that runs\n"
                        "          the server (System Settings > Privacy & Security > Screen & System\n"
                        "          Audio Recording) and restart that app.\n");
    }
    NSDictionary *opts = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
    if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts)) {
        fprintf(stderr, "[Input] Accessibility permission is missing: remote mouse/keyboard will be ignored\n"
                        "        until it is granted (System Settings > Privacy & Security > Accessibility).\n");
    }
    return start_stream();
}

int capture_list_monitors(capture_monitor_info_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    if (s_display_count == 0) refresh_displays();   /* normally refreshed by the capture thread */
    int n = s_display_count < max_count ? s_display_count : max_count;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].id = i;
        snprintf(out[i].name, sizeof(out[i].name), "%s", s_displays[i].name);
        out[i].width = s_displays[i].pixel_w;
        out[i].height = s_displays[i].pixel_h;
        out[i].left = (int)s_displays[i].bounds.origin.x;
        out[i].top = (int)s_displays[i].bounds.origin.y;
        out[i].is_primary = s_displays[i].is_primary;
    }
    return n;
}

int capture_set_monitor(int monitor_id) {
    if (monitor_id < 0 || monitor_id >= s_display_count) return -1;
    s_active = monitor_id;
    atomic_store(&s_mon_dirty, 1);
    return 0;
}

int capture_get_active_monitor(void) { return s_active; }

int capture_set_resolution(int req_w, int req_h) {
    s_req_w = req_w;
    s_req_h = req_h;
    atomic_store(&s_res_dirty, 1);
    return 0;
}

void capture_set_draw_cursor(int enabled) {
    int v = enabled ? 1 : 0;
    if (atomic_exchange(&s_draw_cursor, v) != v) atomic_store(&s_cursor_dirty, 1);
}

void capture_get_geometry(int *screen_w, int *screen_h, int *stream_w, int *stream_h) {
    int sw = 0, sh = 0;
    if (s_display_count > 0) { sw = s_displays[s_active].pixel_w; sh = s_displays[s_active].pixel_h; }
    int tw = 0, th = 0;
    compute_smart_resolution(sw, sh, s_req_w, s_req_h, &tw, &th);
    if (screen_w) *screen_w = sw;
    if (screen_h) *screen_h = sh;
    if (stream_w) *stream_w = tw;
    if (stream_h) *stream_h = th;
}

static void bgra_to_rgb(const uint8_t *src, size_t pitch, int w, int h, uint8_t *dst) {
    const int chunk = 32;
    dispatch_apply((size_t)((h + chunk - 1) / chunk), DISPATCH_APPLY_AUTO, ^(size_t blk) {
        int y0 = (int)blk * chunk, y1 = y0 + chunk < h ? y0 + chunk : h;
        for (int y = y0; y < y1; y++) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint8_t *d = dst + (size_t)y * (size_t)w * 3;
            for (int x = 0; x < w; x++) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
                s += 4; d += 3;
            }
        }
    });
}

/* 0 = frame ready, 1 = nothing new (caller retries), <0 = error. */
int capture_acquire_frame(capture_frame_t *frame, int timeout_ms) {
    (void)timeout_ms;
    if (!frame) return -1;

    if (atomic_exchange(&s_mon_dirty, 0)) {
        if (start_stream() != 0) return -1;
    } else if (atomic_exchange(&s_res_dirty, 0)) {
        int w = 0, h = 0;
        if (s_display_count > 0)
            compute_smart_resolution(s_displays[s_active].pixel_w, s_displays[s_active].pixel_h,
                                     s_req_w, s_req_h, &w, &h);
        if (w != s_target_w || h != s_target_h) {
            s_target_w = w; s_target_h = h;
            atomic_store(&s_cursor_dirty, 0);
            update_stream_config();
        }
    }
    if (atomic_exchange(&s_cursor_dirty, 0)) update_stream_config();
    if (!s_stream) return -1;

    /* ScreenCaptureKit only delivers changed frames. Re-deliver the last one every 250 ms so a
     * client that just connected (or asked for a keyframe) gets a picture on a static screen;
     * an unchanged frame costs the server only a dirty-rect comparison. */
    int64_t t = now_us();
    pthread_mutex_lock(&s_lock);
    CVPixelBufferRef pb = s_latest_pb;
    int is_new = s_latest_is_new;
    if (!pb || (!is_new && t - s_last_delivered_us < 250000)) {
        pthread_mutex_unlock(&s_lock);
        return 1;
    }
    CVPixelBufferRetain(pb);
    s_latest_is_new = 0;
    pthread_mutex_unlock(&s_lock);

    int w = (int)CVPixelBufferGetWidth(pb);
    int h = (int)CVPixelBufferGetHeight(pb);
    size_t need = (size_t)w * (size_t)h * 3;
    if (need > s_rgb_cap) {
        free(s_rgb);
        s_rgb = (uint8_t *)malloc(need + 64);
        s_rgb_cap = s_rgb ? need : 0;
        if (!s_rgb) { CVPixelBufferRelease(pb); return -1; }
    }
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    bgra_to_rgb((const uint8_t *)CVPixelBufferGetBaseAddress(pb), CVPixelBufferGetBytesPerRow(pb), w, h, s_rgb);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pb);

    s_last_delivered_us = t;
    frame->rgb_data = s_rgb;
    frame->width = w;
    frame->height = h;
    frame->stride = w * 3;
    frame->timestamp_us = t;
    return 0;
}

void capture_release_frame(void) {}

void capture_close(void) {
    stop_stream();
    free(s_rgb);
    s_rgb = NULL;
    s_rgb_cap = 0;
}

/* ============================================================================
 * capture.h: input (CGEvent)
 * ============================================================================ */

static CGEventSourceRef s_src = NULL;
static CGEventFlags s_flags = 0;           /* modifiers currently held by the client */
static int s_buttons_down = 0;             /* bit 0 left, 1 middle, 2 right */
static CGPoint s_last_pos;
static int s_click_count = 0, s_click_button = -1;
static int64_t s_last_click_us = 0;
static CGPoint s_last_click_pos;

static CGEventSourceRef event_source(void) {
    if (!s_src) s_src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    return s_src;
}

static CGPoint norm_to_point(uint16_t xn, uint16_t yn) {
    CGRect b = (s_display_count > 0) ? s_displays[s_active].bounds : CGDisplayBounds(CGMainDisplayID());
    double fx = (double)xn / 65535.0, fy = (double)yn / 65535.0;
    return CGPointMake(b.origin.x + fx * (b.size.width - 1), b.origin.y + fy * (b.size.height - 1));
}

static void post(CGEventRef ev) {
    if (!ev) return;
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

void input_inject_mouse_move(uint16_t x_norm, uint16_t y_norm) {
    CGPoint p = norm_to_point(x_norm, y_norm);
    s_last_pos = p;
    CGEventType type = kCGEventMouseMoved;
    CGMouseButton btn = kCGMouseButtonLeft;
    if (s_buttons_down & 1)      { type = kCGEventLeftMouseDragged; }
    else if (s_buttons_down & 4) { type = kCGEventRightMouseDragged; btn = kCGMouseButtonRight; }
    else if (s_buttons_down & 2) { type = kCGEventOtherMouseDragged; btn = kCGMouseButtonCenter; }
    CGEventRef ev = CGEventCreateMouseEvent(event_source(), type, p, btn);
    if (ev) CGEventSetFlags(ev, s_flags);
    post(ev);
}

void input_inject_mouse_button(uint8_t button, uint8_t down, uint16_t x_norm, uint16_t y_norm) {
    CGPoint p = norm_to_point(x_norm, y_norm);
    s_last_pos = p;
    CGEventType type;
    CGMouseButton btn;
    int bit;
    /* Browser buttons: 0 left, 1 middle, 2 right. */
    if (button == 0)      { btn = kCGMouseButtonLeft;   bit = 1; type = down ? kCGEventLeftMouseDown  : kCGEventLeftMouseUp; }
    else if (button == 2) { btn = kCGMouseButtonRight;  bit = 4; type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp; }
    else if (button == 1) { btn = kCGMouseButtonCenter; bit = 2; type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; }
    else return;

    if (down) {
        /* macOS needs an explicit click count for double/triple clicks. */
        int64_t t = now_us();
        double interval = [NSEvent doubleClickInterval];
        double dx = p.x - s_last_click_pos.x, dy = p.y - s_last_click_pos.y;
        if (s_click_button == button && (t - s_last_click_us) < (int64_t)(interval * 1e6) &&
            dx * dx + dy * dy < 25.0) {
            s_click_count++;
        } else {
            s_click_count = 1;
        }
        s_click_button = button;
        s_last_click_us = t;
        s_last_click_pos = p;
        s_buttons_down |= bit;
    } else {
        s_buttons_down &= ~bit;
    }

    CGEventRef ev = CGEventCreateMouseEvent(event_source(), type, p, btn);
    if (ev) {
        CGEventSetIntegerValueField(ev, kCGMouseEventClickState, s_click_count > 0 ? s_click_count : 1);
        CGEventSetFlags(ev, s_flags);
    }
    post(ev);
}

void input_inject_mouse_wheel(int16_t delta_x, int16_t delta_y) {
    /* The client sends Windows wheel units scaled from browser pixels (-deltaY * 1.2);
     * positive y = scroll up on both systems. Windows positive x = right, macOS = left. */
    int32_t py = (int32_t)(delta_y / 1.2);
    int32_t px = (int32_t)(-delta_x / 1.2);
    if (py == 0 && delta_y != 0) py = delta_y > 0 ? 1 : -1;
    if (px == 0 && delta_x != 0) px = delta_x > 0 ? -1 : 1;
    CGEventRef ev = CGEventCreateScrollWheelEvent2(event_source(), kCGScrollEventUnitPixel, 2, py, px, 0);
    if (ev) CGEventSetFlags(ev, s_flags);
    post(ev);
}

/* Windows VK -> macOS virtual key code (kVK_*), 0xFFFF = unmapped. */
static uint16_t vk_to_mac(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        static const uint16_t letters[26] = {
            kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F, kVK_ANSI_G,
            kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N,
            kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U,
            kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y, kVK_ANSI_Z };
        return letters[vk - 'A'];
    }
    if (vk >= '0' && vk <= '9') {
        static const uint16_t digits[10] = {
            kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
            kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9 };
        return digits[vk - '0'];
    }
    if (vk >= 0x60 && vk <= 0x69) {
        static const uint16_t pad[10] = {
            kVK_ANSI_Keypad0, kVK_ANSI_Keypad1, kVK_ANSI_Keypad2, kVK_ANSI_Keypad3, kVK_ANSI_Keypad4,
            kVK_ANSI_Keypad5, kVK_ANSI_Keypad6, kVK_ANSI_Keypad7, kVK_ANSI_Keypad8, kVK_ANSI_Keypad9 };
        return pad[vk - 0x60];
    }
    if (vk >= 0x70 && vk <= 0x7B) {
        static const uint16_t fkeys[12] = {
            kVK_F1, kVK_F2, kVK_F3, kVK_F4, kVK_F5, kVK_F6,
            kVK_F7, kVK_F8, kVK_F9, kVK_F10, kVK_F11, kVK_F12 };
        return fkeys[vk - 0x70];
    }
    switch (vk) {
    case 0x08: return kVK_Delete;            /* Backspace */
    case 0x09: return kVK_Tab;
    case 0x0D: return kVK_Return;
    case 0x10: case 0xA0: return kVK_Shift;
    case 0xA1: return kVK_RightShift;
    case 0x11: case 0xA2: return kVK_Control;
    case 0xA3: return kVK_RightControl;
    case 0x12: case 0xA4: return kVK_Option;
    case 0xA5: return kVK_RightOption;
    case 0x5B: return kVK_Command;
    case 0x5C: return kVK_RightCommand;
    case 0x14: return kVK_CapsLock;
    case 0x1B: return kVK_Escape;
    case 0x20: return kVK_Space;
    case 0x21: return kVK_PageUp;
    case 0x22: return kVK_PageDown;
    case 0x23: return kVK_End;
    case 0x24: return kVK_Home;
    case 0x25: return kVK_LeftArrow;
    case 0x26: return kVK_UpArrow;
    case 0x27: return kVK_RightArrow;
    case 0x28: return kVK_DownArrow;
    case 0x2D: return kVK_Help;              /* Insert */
    case 0x2E: return kVK_ForwardDelete;
    case 0x6A: return kVK_ANSI_KeypadMultiply;
    case 0x6B: return kVK_ANSI_KeypadPlus;
    case 0x6D: return kVK_ANSI_KeypadMinus;
    case 0x6E: return kVK_ANSI_KeypadDecimal;
    case 0x6F: return kVK_ANSI_KeypadDivide;
    case 0x90: return kVK_ANSI_KeypadClear;
    case 0xBA: return kVK_ANSI_Semicolon;
    case 0xBB: return kVK_ANSI_Equal;
    case 0xBC: return kVK_ANSI_Comma;
    case 0xBD: return kVK_ANSI_Minus;
    case 0xBE: return kVK_ANSI_Period;
    case 0xBF: return kVK_ANSI_Slash;
    case 0xC0: return kVK_ANSI_Grave;
    case 0xDB: return kVK_ANSI_LeftBracket;
    case 0xDC: return kVK_ANSI_Backslash;
    case 0xDD: return kVK_ANSI_RightBracket;
    case 0xDE: return kVK_ANSI_Quote;
    default:   return 0xFFFF;
    }
}

static CGEventFlags modifier_flag(uint16_t mac_key) {
    switch (mac_key) {
    case kVK_Shift: case kVK_RightShift:     return kCGEventFlagMaskShift;
    case kVK_Control: case kVK_RightControl: return kCGEventFlagMaskControl;
    case kVK_Option: case kVK_RightOption:   return kCGEventFlagMaskAlternate;
    case kVK_Command: case kVK_RightCommand: return kCGEventFlagMaskCommand;
    default: return 0;
    }
}

static void post_key(uint16_t mac_key, bool down) {
    CGEventRef ev = CGEventCreateKeyboardEvent(event_source(), (CGKeyCode)mac_key, down);
    if (ev) CGEventSetFlags(ev, s_flags);
    post(ev);
}

void input_inject_key(uint16_t vk_code, uint8_t down) {
    uint16_t k = vk_to_mac(vk_code);
    if (k == 0xFFFF) return;
    CGEventFlags m = modifier_flag(k);
    if (m) {
        if (down) s_flags |= m; else s_flags &= ~m;
    }
    post_key(k, down != 0);
}

void input_inject_unicode(uint16_t utf16_char) {
    if (!utf16_char) return;
    UniChar ch = utf16_char;
    for (int i = 0; i < 2; i++) {
        CGEventRef ev = CGEventCreateKeyboardEvent(event_source(), 0, i == 0);
        if (!ev) return;
        CGEventSetFlags(ev, 0);   /* the character is already final; no modifiers */
        CGEventKeyboardSetUnicodeString(ev, 1, &ch);
        post(ev);
    }
}

void input_release_all_modifiers(void) {
    static const uint16_t mods[] = { kVK_Shift, kVK_RightShift, kVK_Control, kVK_RightControl,
                                     kVK_Option, kVK_RightOption, kVK_Command, kVK_RightCommand };
    s_flags = 0;
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) post_key(mods[i], false);
}

/* ============================================================================
 * capture.h: clipboard (NSPasteboard)
 * ============================================================================ */

static NSInteger s_last_change = -1;

int clipboard_set_utf8(const char *utf8_text, size_t utf8_len, int trigger_paste) {
    if (!utf8_text || utf8_len == 0) return -1;
    @autoreleasepool {
        NSString *s = [[NSString alloc] initWithBytes:utf8_text length:utf8_len encoding:NSUTF8StringEncoding];
        if (!s) return -1;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        if (![pb setString:s forType:NSPasteboardTypeString]) return -1;
        s_last_change = pb.changeCount;   /* do not echo it back to the client */
        printf("[Clipboard] Set macOS clipboard from client (%zu bytes, trigger_paste=%d)\n", utf8_len, trigger_paste);
    }
    if (trigger_paste) {
        usleep(10000);
        CGEventFlags saved = s_flags;
        s_flags = kCGEventFlagMaskCommand;
        post_key(kVK_Command, true);
        post_key(kVK_ANSI_V, true);
        post_key(kVK_ANSI_V, false);
        s_flags = 0;
        post_key(kVK_Command, false);
        s_flags = saved;
    }
    return 0;
}

char *clipboard_poll_utf8_change(size_t *out_len) {
    if (out_len) *out_len = 0;
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSInteger c = pb.changeCount;
        if (s_last_change < 0) { s_last_change = c; return NULL; }
        if (c == s_last_change) return NULL;
        s_last_change = c;
        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (!s) return NULL;
        const char *u = s.UTF8String;
        size_t n = u ? strlen(u) : 0;
        if (n == 0 || n > 4 * 1024 * 1024) return NULL;
        char *out = (char *)malloc(n + 1);
        if (!out) return NULL;
        memcpy(out, u, n + 1);
        if (out_len) *out_len = n;
        return out;
    }
}

/* ============================================================================
 * capture.h: system audio (ScreenCaptureKit audio output)
 * ============================================================================ */

static inline int16_t to_i16(float v) {
    if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
    return (int16_t)(v * 32767.f);
}

static void ring_put(int16_t l, int16_t r) {
    size_t tail = (s_aud_head + s_aud_count) % AUD_RING_FRAMES;
    s_aud_ring[tail * 2] = l;
    s_aud_ring[tail * 2 + 1] = r;
    if (s_aud_count < AUD_RING_FRAMES) s_aud_count++;
    else s_aud_head = (s_aud_head + 1) % AUD_RING_FRAMES;   /* drop oldest */
}

static void audio_push(CMSampleBufferRef sb) {
    if (!atomic_load(&s_audio_wanted)) return;
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
    const AudioStreamBasicDescription *asbd = fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt) : NULL;
    if (!asbd || !(asbd->mFormatFlags & kAudioFormatFlagIsFloat) || asbd->mBitsPerChannel != 32) return;

    size_t list_size = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, &list_size, NULL, 0, NULL, NULL, 0, NULL);
    if (list_size == 0) return;
    AudioBufferList *abl = (AudioBufferList *)malloc(list_size);
    if (!abl) return;
    CMBlockBufferRef block = NULL;
    OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sb, NULL, abl, list_size, NULL, NULL, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (st != noErr) { free(abl); return; }

    int frames = (int)CMSampleBufferGetNumSamples(sb);
    int non_interleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    int ch = (int)asbd->mChannelsPerFrame;
    const float *L, *R;
    int step;
    if (non_interleaved) {
        L = (const float *)abl->mBuffers[0].mData;
        R = abl->mNumberBuffers > 1 ? (const float *)abl->mBuffers[1].mData : L;
        step = 1;
    } else {
        L = (const float *)abl->mBuffers[0].mData;
        R = ch > 1 ? L + 1 : L;
        step = ch > 0 ? ch : 1;
    }

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < frames; i++) {
        float l = L[(size_t)i * step], r = R[(size_t)i * step];
        if (s_aud_have_carry) {   /* 2:1 decimation by averaging pairs */
            ring_put(to_i16((s_aud_carry_l + l) * 0.5f), to_i16((s_aud_carry_r + r) * 0.5f));
            s_aud_have_carry = 0;
        } else {
            s_aud_carry_l = l; s_aud_carry_r = r; s_aud_have_carry = 1;
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (block) CFRelease(block);
    free(abl);
}

int audio_loopback_init(uint32_t *out_sample_rate, uint16_t *out_channels) {
    atomic_store(&s_audio_wanted, 1);
    if (out_sample_rate) *out_sample_rate = 24000;
    if (out_channels) *out_channels = 2;
    printf("[Audio] ScreenCaptureKit system audio (48000 Hz in -> 24000 Hz out, stereo)\n");
    return 0;
}

int audio_loopback_read(int16_t *out_pcm, int max_frames, uint32_t *out_sample_rate, uint16_t *out_channels) {
    if (out_sample_rate) *out_sample_rate = 24000;
    if (out_channels) *out_channels = 2;
    if (!out_pcm || max_frames <= 0) return 0;
    int n = 0, non_silent = 0;
    pthread_mutex_lock(&s_lock);
    while (s_aud_count > 0 && n < max_frames) {
        int16_t l = s_aud_ring[s_aud_head * 2], r = s_aud_ring[s_aud_head * 2 + 1];
        out_pcm[n * 2] = l;
        out_pcm[n * 2 + 1] = r;
        if (l | r) non_silent = 1;
        s_aud_head = (s_aud_head + 1) % AUD_RING_FRAMES;
        s_aud_count--;
        n++;
    }
    pthread_mutex_unlock(&s_lock);
    return non_silent ? n : 0;
}

void audio_loopback_close(void) {
    atomic_store(&s_audio_wanted, 0);
}
