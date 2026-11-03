/*
 * Native screen capture and remote input backend for Linux (X11 / Xwayland).
 *
 * Uses MIT-SHM (XShmGetImage) for zero-copy 60 FPS screen capture,
 * XTest extension for remote mouse and keyboard event injection,
 * and XFixes for software cursor rendering.
 */
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
    Display *dpy;
    int screen;
    Window root;

    int screen_w;
    int screen_h;
    int target_w;
    int target_h;
    int fps;

    /* MIT-SHM resources */
    int has_shm;
    XShmSegmentInfo shminfo;
    XImage *shm_image;

    /* Output buffer for PHF (RGB24 tightly packed) */
    uint8_t *rgb_buffer;
    size_t rgb_size;

    int draw_cursor;
    int active_monitor;
    int64_t last_frame_us;
} linux_capture_state_t;

static linux_capture_state_t s_cap = {0};

static int64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Fast 16.16 fixed-point bilinear BGRA -> RGB downscaler */
static void convert_bgra_to_rgb_bilinear(
    const uint8_t *src_bgra, int src_pitch, int sw, int sh,
    uint8_t *dst_rgb, int dw, int dh)
{
    const uint32_t x_step = (uint32_t)(((uint64_t)(sw - 1) << 16) / (uint32_t)dw);
    const uint32_t y_step = (uint32_t)(((uint64_t)(sh - 1) << 16) / (uint32_t)dh);

    int y;
    #pragma omp parallel for schedule(static)
    for (y = 0; y < dh; y++) {
        uint32_t sy_fp = (uint32_t)y * y_step;
        int y_low = (int)(sy_fp >> 16);
        int y_high = (y_low + 1 < sh) ? y_low + 1 : y_low;
        uint32_t wy = (sy_fp >> 8) & 0xFFu;
        uint32_t iwy = 256u - wy;

        const uint8_t *row0 = src_bgra + (size_t)y_low * (size_t)src_pitch;
        const uint8_t *row1 = src_bgra + (size_t)y_high * (size_t)src_pitch;
        uint8_t *dst_row = dst_rgb + (size_t)y * (size_t)dw * 3;

        for (int x = 0; x < dw; x++) {
            uint32_t sx_fp = (uint32_t)x * x_step;
            int x_low = (int)(sx_fp >> 16);
            int x_high = (x_low + 1 < sw) ? x_low + 1 : x_low;
            uint32_t wx = (sx_fp >> 8) & 0xFFu;
            uint32_t iwx = 256u - wx;

            const uint8_t *p00 = row0 + (x_low * 4);
            const uint8_t *p01 = row0 + (x_high * 4);
            const uint8_t *p10 = row1 + (x_low * 4);
            const uint8_t *p11 = row1 + (x_high * 4);

            uint32_t w00 = (iwx * iwy) >> 8;
            uint32_t w01 = (wx  * iwy) >> 8;
            uint32_t w10 = (iwx * wy)  >> 8;
            uint32_t w11 = (wx  * wy)  >> 8;

            uint32_t b = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 8;
            uint32_t g = (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 8;
            uint32_t r = (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 8;

            uint8_t *d = dst_row + (x * 3);
            d[0] = (uint8_t)(r > 255 ? 255 : r);
            d[1] = (uint8_t)(g > 255 ? 255 : g);
            d[2] = (uint8_t)(b > 255 ? 255 : b);
        }
    }
}

/* Fast 1:1 BGRA -> RGB conversion (same resolution) */
static void convert_bgra_to_rgb_same_size(
    const uint8_t *src_bgra, int src_pitch,
    uint8_t *dst_rgb, int w, int h)
{
    int y;
    #pragma omp parallel for schedule(static)
    for (y = 0; y < h; y++) {
        const uint8_t *s = src_bgra + (size_t)y * (size_t)src_pitch;
        uint8_t *d = dst_rgb + (size_t)y * (size_t)w * 3;
        for (int x = 0; x < w; x++) {
            d[x * 3 + 0] = s[x * 4 + 2]; /* R */
            d[x * 3 + 1] = s[x * 4 + 1]; /* G */
            d[x * 3 + 2] = s[x * 4 + 0]; /* B */
        }
    }
}

static void draw_cursor_onto_rgb(uint8_t *rgb_buffer, int stream_w, int stream_h, int screen_w, int screen_h) {
    if (!s_cap.dpy || !s_cap.draw_cursor) return;

    XFixesCursorImage *cur = XFixesGetCursorImage(s_cap.dpy);
    if (!cur) return;

    int cx = cur->x - cur->xhot;
    int cy = cur->y - cur->yhot;
    int cw = cur->width;
    int ch = cur->height;

    /* Scale cursor position to stream dimensions if needed */
    if (stream_w != screen_w || stream_h != screen_h) {
        cx = (int)((int64_t)cx * stream_w / screen_w);
        cy = (int)((int64_t)cy * stream_h / screen_h);
        cw = (int)((int64_t)cw * stream_w / screen_w);
        ch = (int)((int64_t)ch * stream_h / screen_h);
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
    }

    for (int y = 0; y < ch; y++) {
        int py = cy + y;
        if (py < 0 || py >= stream_h) continue;

        int src_y = (int)((int64_t)y * cur->height / ch);
        const unsigned long *cur_row = cur->pixels + src_y * cur->width;
        uint8_t *dst_row = rgb_buffer + (size_t)py * (size_t)stream_w * 3;

        for (int x = 0; x < cw; x++) {
            int px = cx + x;
            if (px < 0 || px >= stream_w) continue;

            int src_x = (int)((int64_t)x * cur->width / cw);
            unsigned long pixel = cur_row[src_x];
            uint8_t a = (pixel >> 24) & 0xFF;
            if (a == 0) continue;

            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;

            uint8_t *d = dst_row + (px * 3);
            if (a == 255) {
                d[0] = r; d[1] = g; d[2] = b;
            } else {
                uint32_t inv_a = 255 - a;
                d[0] = (uint8_t)((r * a + d[0] * inv_a) / 255);
                d[1] = (uint8_t)((g * a + d[1] * inv_a) / 255);
                d[2] = (uint8_t)((b * a + d[2] * inv_a) / 255);
            }
        }
    }

    XFree(cur);
}

static void fit_resolution(int req_w, int req_h) {
    if (req_w <= 0 || req_h <= 0 || req_w >= s_cap.screen_w || req_h >= s_cap.screen_h) {
        s_cap.target_w = s_cap.screen_w;
        s_cap.target_h = s_cap.screen_h;
        return;
    }
    int w = req_w;
    int h = (int)((int64_t)req_w * s_cap.screen_h / s_cap.screen_w);
    if (h > req_h) {
        h = req_h;
        w = (int)((int64_t)req_h * s_cap.screen_w / s_cap.screen_h);
    }
    s_cap.target_w = w > 16 ? (w & ~1) : 16;
    s_cap.target_h = h > 16 ? (h & ~1) : 16;
}

static int alloc_buffers(void) {
    size_t needed = (size_t)s_cap.target_w * (size_t)s_cap.target_h * 3;
    if (needed > s_cap.rgb_size) {
        free(s_cap.rgb_buffer);
        s_cap.rgb_buffer = (uint8_t *)malloc(needed);
        if (!s_cap.rgb_buffer) {
            s_cap.rgb_size = 0;
            return -1;
        }
        s_cap.rgb_size = needed;
    }
    return 0;
}

int capture_init(const capture_config_t *config) {
    s_cap.dpy = XOpenDisplay(NULL);
    if (!s_cap.dpy) {
        fprintf(stderr, "[Capture-Linux] Cannot open X11 Display. Is $DISPLAY set?\n");
        return -1;
    }

    s_cap.screen = DefaultScreen(s_cap.dpy);
    s_cap.root = RootWindow(s_cap.dpy, s_cap.screen);
    s_cap.screen_w = DisplayWidth(s_cap.dpy, s_cap.screen);
    s_cap.screen_h = DisplayHeight(s_cap.dpy, s_cap.screen);
    s_cap.fps = (config && config->fps > 0) ? config->fps : 60;
    s_cap.draw_cursor = 1;

    if (config) {
        fit_resolution(config->target_w, config->target_h);
    } else {
        fit_resolution(1280, 720);
    }

    /* Initialize MIT-SHM */
    s_cap.has_shm = XShmQueryExtension(s_cap.dpy);
    if (s_cap.has_shm) {
        s_cap.shm_image = XShmCreateImage(
            s_cap.dpy, DefaultVisual(s_cap.dpy, s_cap.screen),
            DefaultDepth(s_cap.dpy, s_cap.screen), ZPixmap, NULL,
            &s_cap.shminfo, s_cap.screen_w, s_cap.screen_h
        );
        if (s_cap.shm_image) {
            s_cap.shminfo.shmid = shmget(IPC_PRIVATE,
                (size_t)s_cap.shm_image->bytes_per_line * (size_t)s_cap.shm_image->height,
                IPC_CREAT | 0777);
            if (s_cap.shminfo.shmid >= 0) {
                s_cap.shminfo.shmaddr = s_cap.shm_image->data = (char *)shmat(s_cap.shminfo.shmid, 0, 0);
                s_cap.shminfo.readOnly = False;
                if (!XShmAttach(s_cap.dpy, &s_cap.shminfo)) {
                    fprintf(stderr, "[Capture-Linux] XShmAttach failed, falling back to standard XGetImage\n");
                    s_cap.has_shm = 0;
                    XDestroyImage(s_cap.shm_image);
                    s_cap.shm_image = NULL;
                    shmdt(s_cap.shminfo.shmaddr);
                    shmctl(s_cap.shminfo.shmid, IPC_RMID, 0);
                }
            } else {
                s_cap.has_shm = 0;
                XDestroyImage(s_cap.shm_image);
                s_cap.shm_image = NULL;
            }
        } else {
            s_cap.has_shm = 0;
        }
    }

    if (alloc_buffers() != 0) {
        capture_close();
        return -1;
    }

    printf("[Capture-Linux] Native X11 capture initialized: %dx%d (Stream %dx%d @ %d FPS, SHM=%s)\n",
           s_cap.screen_w, s_cap.screen_h, s_cap.target_w, s_cap.target_h, s_cap.fps,
           s_cap.has_shm ? "Enabled" : "Fallback");
    return 0;
}

int capture_list_monitors(capture_monitor_info_t *out_monitors, int max_count) {
    if (!out_monitors || max_count < 1) return 0;
    memset(out_monitors, 0, sizeof(*out_monitors) * (size_t)max_count);

    if (s_cap.dpy) {
        int nmonitors = 0;
        XRRMonitorInfo *m = XRRGetMonitors(s_cap.dpy, s_cap.root, True, &nmonitors);
        if (m && nmonitors > 0) {
            int count = nmonitors < max_count ? nmonitors : max_count;
            for (int i = 0; i < count; i++) {
                out_monitors[i].id = i;
                char *name = XGetAtomName(s_cap.dpy, m[i].name);
                snprintf(out_monitors[i].name, sizeof(out_monitors[i].name), "%s", name ? name : "Display");
                if (name) XFree(name);
                out_monitors[i].width = m[i].width;
                out_monitors[i].height = m[i].height;
                out_monitors[i].left = m[i].x;
                out_monitors[i].top = m[i].y;
                out_monitors[i].is_primary = m[i].primary ? 1 : (i == 0);
            }
            XRRFreeMonitors(m);
            return count;
        }
    }

    out_monitors[0].id = 0;
    snprintf(out_monitors[0].name, sizeof(out_monitors[0].name), "Default Screen (:0)");
    out_monitors[0].width = s_cap.screen_w > 0 ? s_cap.screen_w : 1920;
    out_monitors[0].height = s_cap.screen_h > 0 ? s_cap.screen_h : 1080;
    out_monitors[0].is_primary = 1;
    return 1;
}

int capture_set_monitor(int monitor_id) {
    if (monitor_id == 0) {
        s_cap.active_monitor = 0;
        return 0;
    }
    return -1;
}

int capture_get_active_monitor(void) {
    return s_cap.active_monitor;
}

int capture_set_resolution(int req_w, int req_h) {
    fit_resolution(req_w, req_h);
    return alloc_buffers();
}

void capture_get_geometry(int *screen_w, int *screen_h, int *stream_w, int *stream_h) {
    if (screen_w) *screen_w = s_cap.screen_w;
    if (screen_h) *screen_h = s_cap.screen_h;
    if (stream_w) *stream_w = s_cap.target_w;
    if (stream_h) *stream_h = s_cap.target_h;
}

void capture_set_draw_cursor(int enabled) {
    s_cap.draw_cursor = enabled;
}

int capture_acquire_frame(capture_frame_t *frame, int timeout_ms) {
    (void)timeout_ms;
    if (!s_cap.dpy || !s_cap.rgb_buffer || !frame) return -1;

    int64_t t = get_time_us();
    int64_t min_interval_us = 1000000 / (s_cap.fps > 0 ? s_cap.fps : 60);
    if (t - s_cap.last_frame_us < min_interval_us) {
        return 1; /* Frame rate limiter: not ready yet */
    }
    s_cap.last_frame_us = t;

    const uint8_t *src_pixels = NULL;
    int src_pitch = 0;
    XImage *fallback_img = NULL;

    if (s_cap.has_shm && s_cap.shm_image) {
        if (!XShmGetImage(s_cap.dpy, s_cap.root, s_cap.shm_image, 0, 0, AllPlanes)) {
            return -1;
        }
        src_pixels = (const uint8_t *)s_cap.shm_image->data;
        src_pitch = s_cap.shm_image->bytes_per_line;
    } else {
        fallback_img = XGetImage(s_cap.dpy, s_cap.root, 0, 0, s_cap.screen_w, s_cap.screen_h, AllPlanes, ZPixmap);
        if (!fallback_img) return -1;
        src_pixels = (const uint8_t *)fallback_img->data;
        src_pitch = fallback_img->bytes_per_line;
    }

    if (s_cap.target_w == s_cap.screen_w && s_cap.target_h == s_cap.screen_h) {
        convert_bgra_to_rgb_same_size(src_pixels, src_pitch, s_cap.rgb_buffer, s_cap.target_w, s_cap.target_h);
    } else {
        convert_bgra_to_rgb_bilinear(src_pixels, src_pitch, s_cap.screen_w, s_cap.screen_h,
                                     s_cap.rgb_buffer, s_cap.target_w, s_cap.target_h);
    }

    if (fallback_img) {
        XDestroyImage(fallback_img);
    }

    if (s_cap.draw_cursor) {
        draw_cursor_onto_rgb(s_cap.rgb_buffer, s_cap.target_w, s_cap.target_h, s_cap.screen_w, s_cap.screen_h);
    }

    frame->rgb_data = s_cap.rgb_buffer;
    frame->width = s_cap.target_w;
    frame->height = s_cap.target_h;
    frame->stride = s_cap.target_w * 3;
    frame->timestamp_us = t;
    return 0;
}

void capture_release_frame(void) {}

void capture_close(void) {
    if (s_cap.dpy) {
        if (s_cap.has_shm && s_cap.shm_image) {
            XShmDetach(s_cap.dpy, &s_cap.shminfo);
            XDestroyImage(s_cap.shm_image);
            shmdt(s_cap.shminfo.shmaddr);
            shmctl(s_cap.shminfo.shmid, IPC_RMID, 0);
            s_cap.shm_image = NULL;
        }
        XCloseDisplay(s_cap.dpy);
        s_cap.dpy = NULL;
    }
    free(s_cap.rgb_buffer);
    s_cap.rgb_buffer = NULL;
    s_cap.rgb_size = 0;
}

/* --------------------------------------------------------------------------
 * Remote Input Injection (XTest)
 * -------------------------------------------------------------------------- */
void input_inject_mouse_move(uint16_t x_norm, uint16_t y_norm) {
    if (!s_cap.dpy || s_cap.screen_w <= 0 || s_cap.screen_h <= 0) return;
    int x = (int)((uint64_t)x_norm * (uint64_t)(s_cap.screen_w - 1) / 65535ULL);
    int y = (int)((uint64_t)y_norm * (uint64_t)(s_cap.screen_h - 1) / 65535ULL);
    XTestFakeMotionEvent(s_cap.dpy, s_cap.screen, x, y, CurrentTime);
    XFlush(s_cap.dpy);
}

void input_inject_mouse_button(uint8_t button, uint8_t down, uint16_t x_norm, uint16_t y_norm) {
    if (!s_cap.dpy) return;
    input_inject_mouse_move(x_norm, y_norm);

    unsigned int xbtn = 1; /* 1: Left, 2: Middle, 3: Right */
    if (button == 0) xbtn = 1;
    else if (button == 1) xbtn = 2;
    else if (button == 2) xbtn = 3;

    XTestFakeButtonEvent(s_cap.dpy, xbtn, down ? True : False, CurrentTime);
    XFlush(s_cap.dpy);
}

void input_inject_mouse_wheel(int16_t delta_x, int16_t delta_y) {
    if (!s_cap.dpy) return;
    if (delta_y != 0) {
        unsigned int btn = (delta_y < 0) ? 4 : 5; /* 4: Wheel Up, 5: Wheel Down */
        XTestFakeButtonEvent(s_cap.dpy, btn, True, CurrentTime);
        XTestFakeButtonEvent(s_cap.dpy, btn, False, CurrentTime);
    }
    if (delta_x != 0) {
        unsigned int btn = (delta_x < 0) ? 6 : 7;
        XTestFakeButtonEvent(s_cap.dpy, btn, True, CurrentTime);
        XTestFakeButtonEvent(s_cap.dpy, btn, False, CurrentTime);
    }
    XFlush(s_cap.dpy);
}

/* Map Windows Virtual Keys (VK_*) to standard X11 KeySym */
static KeySym vk_to_keysym(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') return XK_a + (vk - 'A');
    if (vk >= '0' && vk <= '9') return XK_0 + (vk - '0');
    if (vk >= 0x60 && vk <= 0x69) return XK_KP_0 + (vk - 0x60); /* Numpad */
    if (vk >= 0x70 && vk <= 0x7B) return XK_F1 + (vk - 0x70);   /* F1-F12 */

    switch (vk) {
        case 0x08: return XK_BackSpace;
        case 0x09: return XK_Tab;
        case 0x0D: return XK_Return;
        case 0x10: return XK_Shift_L;
        case 0x11: return XK_Control_L;
        case 0x12: return XK_Alt_L;
        case 0x13: return XK_Pause;
        case 0x14: return XK_Caps_Lock;
        case 0x1B: return XK_Escape;
        case 0x20: return XK_space;
        case 0x21: return XK_Page_Up;
        case 0x22: return XK_Page_Down;
        case 0x23: return XK_End;
        case 0x24: return XK_Home;
        case 0x25: return XK_Left;
        case 0x26: return XK_Up;
        case 0x27: return XK_Right;
        case 0x28: return XK_Down;
        case 0x2D: return XK_Insert;
        case 0x2E: return XK_Delete;
        case 0x5B: return XK_Super_L; /* Windows key */
        case 0x5C: return XK_Super_R;
        default: return NoSymbol;
    }
}

void input_inject_key(uint16_t vk_code, uint8_t down) {
    if (!s_cap.dpy) return;
    KeySym sym = vk_to_keysym(vk_code);
    if (sym != NoSymbol) {
        KeyCode kc = XKeysymToKeycode(s_cap.dpy, sym);
        if (kc != 0) {
            XTestFakeKeyEvent(s_cap.dpy, kc, down ? True : False, CurrentTime);
            XFlush(s_cap.dpy);
        }
    }
}

void input_inject_unicode(uint16_t utf16_char) {
    if (!s_cap.dpy || utf16_char == 0) return;
    KeySym sym = (utf16_char < 0x100) ? (KeySym)utf16_char : (0x01000000 | utf16_char);
    KeyCode kc = XKeysymToKeycode(s_cap.dpy, sym);
    if (kc != 0) {
        XTestFakeKeyEvent(s_cap.dpy, kc, True, CurrentTime);
        XTestFakeKeyEvent(s_cap.dpy, kc, False, CurrentTime);
        XFlush(s_cap.dpy);
    }
}

void input_release_all_modifiers(void) {
    if (!s_cap.dpy) return;
    KeySym mods[] = { XK_Shift_L, XK_Shift_R, XK_Control_L, XK_Control_R, XK_Alt_L, XK_Alt_R, XK_Super_L };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        KeyCode kc = XKeysymToKeycode(s_cap.dpy, mods[i]);
        if (kc != 0) {
            XTestFakeKeyEvent(s_cap.dpy, kc, False, CurrentTime);
        }
    }
    XFlush(s_cap.dpy);
}

/* --------------------------------------------------------------------------
 * Clipboard & Audio Stubs
 * -------------------------------------------------------------------------- */
int clipboard_set_utf8(const char *text, size_t len, int trigger_paste) {
    (void)text; (void)len; (void)trigger_paste;
    return 0;
}

char *clipboard_poll_utf8_change(size_t *out_len) {
    if (out_len) *out_len = 0;
    return NULL;
}

int audio_loopback_init(uint32_t *rate, uint16_t *ch) {
    (void)rate; (void)ch;
    return -1;
}

int audio_loopback_read(int16_t *pcm, int max_frames, uint32_t *rate, uint16_t *ch) {
    (void)pcm; (void)max_frames; (void)rate; (void)ch;
    return 0;
}

void audio_loopback_close(void) {}
