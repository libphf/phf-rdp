#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHF_MAX_MONITORS 8

typedef struct {
    int id;
    char name[128];
    int width;
    int height;
    int left;
    int top;
    int is_primary;
} capture_monitor_info_t;

typedef struct {
    int screen_w;       /* Native monitor width */
    int screen_h;       /* Native monitor height */
    int target_w;       /* Requested max width (0 = native) */
    int target_h;       /* Requested max height (0 = native) */
    int fps;            /* Desired capture frame rate (e.g., 30, 60) */
} capture_config_t;

typedef struct {
    uint8_t *rgb_data;  /* Tightly packed RGB (width * height * 3) */
    int width;
    int height;
    int stride;         /* Stride in bytes */
    int64_t timestamp_us;
} capture_frame_t;

/**
 * Initialize screen capture system.
 */
int capture_init(const capture_config_t *config);

/**
 * Enumerate available physical and virtual (IddCx) monitors.
 * Returns number of monitors found (up to max_count).
 */
int capture_list_monitors(capture_monitor_info_t *out_monitors, int max_count);

/**
 * Select which monitor index (0 .. count-1) to capture.
 */
int capture_set_monitor(int monitor_id);
int capture_get_active_monitor(void);

/**
 * Change target output resolution dynamically based on client bounds.
 * Never upscales above native source resolution and always preserves source aspect ratio.
 */
int capture_set_resolution(int req_w, int req_h);

/**
 * Query current native screen dimensions and active output stream dimensions.
 */
void capture_get_geometry(int *screen_w, int *screen_h, int *stream_w, int *stream_h);

/**
 * Enable or disable drawing the server's software cursor onto the video stream.
 */
void capture_set_draw_cursor(int enabled);

/**
 * Acquire next screen frame.
 */
int capture_acquire_frame(capture_frame_t *frame, int timeout_ms);

/**
 * Release currently acquired frame.
 */
void capture_release_frame(void);

/**
 * Close and free capture resources.
 */
void capture_close(void);

/**
 * Inject remote mouse and keyboard events into the host OS.
 * x_norm, y_norm: normalized [0..65535] coordinates across the active captured monitor.
 */
void input_inject_mouse_move(uint16_t x_norm, uint16_t y_norm);
void input_inject_mouse_button(uint8_t button, uint8_t down, uint16_t x_norm, uint16_t y_norm);
void input_inject_mouse_wheel(int16_t delta_x, int16_t delta_y);
void input_inject_key(uint16_t vk_code, uint8_t down);
void input_inject_unicode(uint16_t utf16_char);
void input_release_all_modifiers(void);

/**
 * Clipboard synchronization (UTF-8).
 */
int clipboard_set_utf8(const char *utf8_text, size_t utf8_len, int trigger_paste);
char* clipboard_poll_utf8_change(size_t *out_len);

/**
 * System Audio Loopback Capture (WASAPI on Windows).
 * Outputs 16-bit signed PCM stereo samples.
 * Returns number of stereo frames (>0) read when non-silent audio is playing, or 0 if silent/empty.
 */
int audio_loopback_init(uint32_t *out_sample_rate, uint16_t *out_channels);
int audio_loopback_read(int16_t *out_pcm, int max_frames, uint32_t *out_sample_rate, uint16_t *out_channels);
void audio_loopback_close(void);

#ifdef __cplusplus
}
#endif

#endif /* CAPTURE_H */
