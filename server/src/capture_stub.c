/*
 * Synthetic capture backend for platforms without a native one yet (macOS, Linux).
 *
 * Produces a test pattern (static gradient + a moving block, so the dirty-rect delta path
 * is exercised) and accepts input/clipboard/audio calls as no-ops. It lets the server and
 * the web client be built and tested end to end without screen-capture permissions.
 */
#include "capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STUB_W 1920
#define STUB_H 1080

static uint8_t *s_rgb = NULL;
static int s_stream_w = 1280;
static int s_stream_h = 720;
static int s_fps = 60;
static int64_t s_last_frame_us = 0;
static unsigned s_tick = 0;

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Fit the request inside the source while keeping its aspect ratio (never upscale). */
static void fit_resolution(int req_w, int req_h) {
    if (req_w <= 0 || req_h <= 0 || req_w >= STUB_W || req_h >= STUB_H) {
        s_stream_w = STUB_W;
        s_stream_h = STUB_H;
        return;
    }
    int w = req_w;
    int h = (int)((int64_t)req_w * STUB_H / STUB_W);
    if (h > req_h) {
        h = req_h;
        w = (int)((int64_t)req_h * STUB_W / STUB_H);
    }
    s_stream_w = w > 16 ? (w & ~1) : 16;
    s_stream_h = h > 16 ? (h & ~1) : 16;
}

static int ensure_buffer(void) {
    free(s_rgb);
    s_rgb = (uint8_t *)malloc((size_t)s_stream_w * (size_t)s_stream_h * 3);
    return s_rgb ? 0 : -1;
}

static void draw_background(void) {
    for (int y = 0; y < s_stream_h; y++) {
        uint8_t *row = s_rgb + (size_t)y * (size_t)s_stream_w * 3;
        for (int x = 0; x < s_stream_w; x++) {
            row[x * 3 + 0] = (uint8_t)(x * 255 / s_stream_w);
            row[x * 3 + 1] = (uint8_t)(y * 255 / s_stream_h);
            row[x * 3 + 2] = (uint8_t)(((x >> 5) ^ (y >> 5)) & 1 ? 200 : 60);
        }
    }
}

static void draw_block(int bx, int by, int bw, int bh, int erase) {
    for (int y = by; y < by + bh && y < s_stream_h; y++) {
        for (int x = bx; x < bx + bw && x < s_stream_w; x++) {
            uint8_t *p = s_rgb + ((size_t)y * (size_t)s_stream_w + (size_t)x) * 3;
            if (erase) {
                p[0] = (uint8_t)(x * 255 / s_stream_w);
                p[1] = (uint8_t)(y * 255 / s_stream_h);
                p[2] = (uint8_t)(((x >> 5) ^ (y >> 5)) & 1 ? 200 : 60);
            } else {
                p[0] = 255;
                p[1] = (uint8_t)(s_tick * 3);
                p[2] = 32;
            }
        }
    }
}

int capture_init(const capture_config_t *config) {
    if (config) {
        if (config->fps > 0) s_fps = config->fps;
        fit_resolution(config->target_w, config->target_h);
    }
    if (ensure_buffer() != 0) return -1;
    draw_background();
    printf("[Capture] Synthetic test-pattern backend (no native capture on this platform yet).\n");
    return 0;
}

int capture_list_monitors(capture_monitor_info_t *out, int max_count) {
    if (!out || max_count < 1) return 0;
    memset(out, 0, sizeof(*out));
    out->id = 0;
    snprintf(out->name, sizeof(out->name), "Synthetic test pattern");
    out->width = STUB_W;
    out->height = STUB_H;
    out->is_primary = 1;
    return 1;
}

int capture_set_monitor(int monitor_id) { return monitor_id == 0 ? 0 : -1; }
int capture_get_active_monitor(void) { return 0; }

int capture_set_resolution(int req_w, int req_h) {
    fit_resolution(req_w, req_h);
    if (ensure_buffer() != 0) return -1;
    draw_background();
    return 0;
}

void capture_get_geometry(int *screen_w, int *screen_h, int *stream_w, int *stream_h) {
    if (screen_w) *screen_w = STUB_W;
    if (screen_h) *screen_h = STUB_H;
    if (stream_w) *stream_w = s_stream_w;
    if (stream_h) *stream_h = s_stream_h;
}

void capture_set_draw_cursor(int enabled) { (void)enabled; }

/* 0 = new frame, 1 = no new frame yet (caller retries). */
int capture_acquire_frame(capture_frame_t *frame, int timeout_ms) {
    (void)timeout_ms;
    if (!s_rgb || !frame) return -1;
    int64_t t = now_us();
    if (t - s_last_frame_us < 1000000 / s_fps) return 1;
    s_last_frame_us = t;

    int bw = s_stream_w / 12, bh = s_stream_h / 12;
    int span = s_stream_w - bw;
    int old_x = span > 0 ? (int)((s_tick * 8u) % (unsigned)span) : 0;
    s_tick++;
    int new_x = span > 0 ? (int)((s_tick * 8u) % (unsigned)span) : 0;
    int by = s_stream_h / 2 - bh / 2;
    draw_block(old_x, by, bw, bh, 1);
    draw_block(new_x, by, bw, bh, 0);

    frame->rgb_data = s_rgb;
    frame->width = s_stream_w;
    frame->height = s_stream_h;
    frame->stride = s_stream_w * 3;
    frame->timestamp_us = t;
    return 0;
}

void capture_release_frame(void) {}

void capture_close(void) {
    free(s_rgb);
    s_rgb = NULL;
}

void input_inject_mouse_move(uint16_t x, uint16_t y) { (void)x; (void)y; }
void input_inject_mouse_button(uint8_t b, uint8_t d, uint16_t x, uint16_t y) {
    (void)b; (void)d; (void)x; (void)y;
}
void input_inject_mouse_wheel(int16_t dx, int16_t dy) { (void)dx; (void)dy; }
void input_inject_key(uint16_t vk, uint8_t down) { (void)vk; (void)down; }
void input_inject_unicode(uint16_t ch) { (void)ch; }
void input_release_all_modifiers(void) {}

int clipboard_set_utf8(const char *text, size_t len, int trigger_paste) {
    (void)text; (void)len; (void)trigger_paste;
    return 0;
}
char *clipboard_poll_utf8_change(size_t *out_len) {
    if (out_len) *out_len = 0;
    return NULL;
}

int audio_loopback_init(uint32_t *rate, uint16_t *ch) { (void)rate; (void)ch; return -1; }
int audio_loopback_read(int16_t *pcm, int max_frames, uint32_t *rate, uint16_t *ch) {
    (void)pcm; (void)max_frames; (void)rate; (void)ch;
    return 0;
}
void audio_loopback_close(void) {}
