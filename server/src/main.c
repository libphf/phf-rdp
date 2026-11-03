#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mongoose.h"
#include "capture.h"
#include "phf.h"

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
static CRITICAL_SECTION g_pkt_lock;
#define PKT_LOCK()   EnterCriticalSection(&g_pkt_lock)
#define PKT_UNLOCK() LeaveCriticalSection(&g_pkt_lock)
#else
#include <unistd.h>
#include <pthread.h>
#include <time.h>
static pthread_mutex_t g_pkt_lock = PTHREAD_MUTEX_INITIALIZER;
#define PKT_LOCK()   pthread_mutex_lock(&g_pkt_lock)
#define PKT_UNLOCK() pthread_mutex_unlock(&g_pkt_lock)
#endif

#if defined(_WIN32)
#include "tray_win.h"
static volatile bool g_server_paused = false;
#endif

#define HTTP_URL "http://0.0.0.0:8080"
#define WS_URL   "ws://0.0.0.0:8765"

static struct mg_mgr g_mgr;
static volatile bool g_running = true;
static volatile int g_target_w = 1280;
static volatile int g_target_h = 720;
static volatile int g_target_fps = 60;
static volatile int g_ws_client_count = 0;
static volatile LONG g_input_pending = 0;
static volatile LONG g_force_keyframe = 1;
static volatile LONG g_audio_enabled = 0;

#if defined(_WIN32)
static void on_tray_action(int action_id) {
    if (action_id == TRAY_ACTION_TOGGLE_PAUSE) {
        g_server_paused = !g_server_paused;
        if (!g_server_paused) {
            InterlockedExchange(&g_force_keyframe, 1);
        }
        printf("[Server] %s via Tray Icon.\n", g_server_paused ? "Pausado" : "Retomado");
    } else if (action_id == TRAY_ACTION_EXIT) {
        printf("[Server] Encerrando servidor via Tray Icon...\n");
        g_running = false;
    }
}

static void update_tray_status(void) {
    tray_status_info_t info = {0};
    info.is_paused = g_server_paused;
    info.status_str = g_server_paused ? "Pausado" : (g_ws_client_count > 0 ? "Ativo" : "Ocioso");
    info.client_count = g_ws_client_count;
    info.fps = g_target_fps;

    int sw = 0, sh = 0, tw = 0, th = 0;
    capture_get_geometry(&sw, &sh, &tw, &th);
    info.source_w = sw;
    info.source_h = sh;
    info.stream_w = tw;
    info.stream_h = th;

    int count = 0;
    for (struct mg_connection *c = g_mgr.conns; c != NULL && count < TRAY_MAX_CLIENT_ENTRIES; c = c->next) {
        if (c->is_websocket) {
            mg_snprintf(info.clients[count].ip_port, sizeof(info.clients[count].ip_port), "%M", mg_print_ip_port, &c->rem);
            info.clients[count].is_audio = (c->fn_data != NULL);
            count++;
        }
    }
    info.clients_len = count;
    tray_update(&info);
}
#endif

/* Latest encoded packet mailbox (single-slot, zero-queue latency) */
static uint8_t *g_latest_pkt = NULL;
static size_t   g_latest_pkt_size = 0;
static uint64_t g_latest_pkt_seq = 0;

static double get_time_seconds(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* Disable Nagle's algorithm (TCP_NODELAY) for instant WebSocket input & frame delivery */
static void set_tcp_nodelay(struct mg_connection *c) {
#if defined(_WIN32)
    BOOL nodelay = TRUE;
    setsockopt((SOCKET)(uintptr_t)c->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
#endif
}

/* Publish newest full PHF frame (0x01) to the mailbox */
static void publish_phf_keyframe(const uint8_t *phf_data, size_t phf_size, double timestamp, float enc_ms) {
    if (!phf_data || phf_size == 0) return;

    /* Packet header (13 bytes): [0x01 (1B)][ts_double (8B)][enc_ms_float (4B)] */
    size_t total_size = 13 + phf_size;
    uint8_t *packet = (uint8_t*)malloc(total_size);
    if (!packet) return;

    packet[0] = 0x01; /* PKT_VIDEO (Full Frame) */
    memcpy(packet + 1, &timestamp, sizeof(double));
    memcpy(packet + 9, &enc_ms, sizeof(float));
    memcpy(packet + 13, phf_data, phf_size);

    uint8_t *old_pkt = NULL;
    PKT_LOCK();
    old_pkt = g_latest_pkt;
    g_latest_pkt = packet;
    g_latest_pkt_size = total_size;
    g_latest_pkt_seq++;
    PKT_UNLOCK();

    if (old_pkt) {
        free(old_pkt);
    }
}

/* Publish delta sub-frame (0x06) to the mailbox */
static void publish_phf_delta(
    const uint8_t *phf_data, size_t phf_size,
    double timestamp, float enc_ms,
    uint16_t rx, uint16_t ry, uint16_t full_w, uint16_t full_h)
{
    if (!phf_data || phf_size == 0) return;

    /* Packet header (21 bytes):
     * [0x06 (1B)][ts_double (8B)][enc_ms_float (4B)][rx_u16][ry_u16][full_w_u16][full_h_u16] */
    size_t total_size = 21 + phf_size;
    uint8_t *packet = (uint8_t*)malloc(total_size);
    if (!packet) return;

    packet[0] = 0x06; /* PKT_VIDEO_DELTA */
    memcpy(packet + 1, &timestamp, sizeof(double));
    memcpy(packet + 9, &enc_ms, sizeof(float));
    memcpy(packet + 13, &rx, sizeof(uint16_t));
    memcpy(packet + 15, &ry, sizeof(uint16_t));
    memcpy(packet + 17, &full_w, sizeof(uint16_t));
    memcpy(packet + 19, &full_h, sizeof(uint16_t));
    memcpy(packet + 21, phf_data, phf_size);

    uint8_t *old_pkt = NULL;
    PKT_LOCK();
    old_pkt = g_latest_pkt;
    g_latest_pkt = packet;
    g_latest_pkt_size = total_size;
    g_latest_pkt_seq++;
    PKT_UNLOCK();

    if (old_pkt) {
        InterlockedExchange(&g_force_keyframe, 1);
        free(old_pkt);
    }
}

/* Flush latest frame from mailbox to all connected WebSocket clients on Mongoose thread */
static void flush_latest_frame_to_clients(uint64_t *last_sent_seq) {
    uint8_t *pkt_to_send = NULL;
    size_t pkt_size = 0;

    PKT_LOCK();
    if (g_latest_pkt && g_latest_pkt_seq > *last_sent_seq) {
        pkt_to_send = g_latest_pkt;
        pkt_size = g_latest_pkt_size;
        g_latest_pkt = NULL;
        g_latest_pkt_size = 0;
        *last_sent_seq = g_latest_pkt_seq;
    }
    PKT_UNLOCK();

    if (!pkt_to_send) return;

    for (struct mg_connection *c = g_mgr.conns; c != NULL; c = c->next) {
        if (c->is_websocket) {
            /* Skip dedicated audio websocket connections (c->fn_data != NULL) */
            if (c->fn_data != NULL) {
                continue;
            }
            if (c->send.len > 256 * 1024) {
                InterlockedExchange(&g_force_keyframe, 1);
                continue;
            }
            mg_ws_send(c, (const char*)pkt_to_send, pkt_size, WEBSOCKET_OP_BINARY);
        }
    }

    free(pkt_to_send);
}

/* Broadcast host clipboard change (0x05) to all connected WebSocket clients */
static void broadcast_host_clipboard(const char *utf8_str, size_t utf8_len) {
    if (!utf8_str || utf8_len == 0) return;
    size_t pkt_size = 2 + utf8_len;
    uint8_t *pkt = (uint8_t*)malloc(pkt_size);
    if (!pkt) return;

    pkt[0] = 0x05; /* PKT_CLIPBOARD */
    pkt[1] = 0x00; /* From server */
    memcpy(pkt + 2, utf8_str, utf8_len);

    int count = 0;
    for (struct mg_connection *c = g_mgr.conns; c != NULL; c = c->next) {
        /* Send clipboard to video/main sockets only */
        if (c->is_websocket && c->fn_data == NULL) {
            mg_ws_send(c, (const char*)pkt, pkt_size, WEBSOCKET_OP_BINARY);
            count++;
        }
    }
    printf("[Clipboard] Host clipboard changed (%zu bytes) broadcast to %d client(s)\n", utf8_len, count);
    free(pkt);
}

/* Broadcast WASAPI PCM stereo audio chunk (0x02) to connected WebSocket clients */
static void broadcast_audio_pcm(const int16_t *pcm, int num_frames, uint32_t sample_rate, uint16_t channels) {
    if (!pcm || num_frames <= 0) return;
    size_t pcm_bytes = (size_t)num_frames * (size_t)channels * sizeof(int16_t);
    /* Header (8 bytes, 2-byte aligned): [0x02][0x00][sample_rate_u32 (4B)][channels_u16 (2B)] */
    size_t pkt_size = 8 + pcm_bytes;
    uint8_t *pkt = (uint8_t*)malloc(pkt_size);
    if (!pkt) return;

    pkt[0] = 0x02; /* PKT_AUDIO */
    pkt[1] = 0x00; /* Alignment pad */
    memcpy(pkt + 2, &sample_rate, sizeof(uint32_t));
    memcpy(pkt + 6, &channels, sizeof(uint16_t));
    memcpy(pkt + 8, pcm, pcm_bytes);

    for (struct mg_connection *c = g_mgr.conns; c != NULL; c = c->next) {
        /* Send audio exclusively to dedicated audio sockets (c->fn_data != NULL)
         * or fallback to main sockets if no dedicated audio socket is connected */
        if (c->is_websocket && c->fn_data != NULL && c->send.len < 128 * 1024) {
            mg_ws_send(c, (const char*)pkt, pkt_size, WEBSOCKET_OP_BINARY);
        }
    }
    free(pkt);
}

static void send_server_state(struct mg_connection *target_c) {
    int screen_w = 0, screen_h = 0, stream_w = 0, stream_h = 0;
    capture_get_geometry(&screen_w, &screen_h, &stream_w, &stream_h);

    capture_monitor_info_t mons[PHF_MAX_MONITORS];
    int mon_count = capture_list_monitors(mons, PHF_MAX_MONITORS);
    int active_mon = capture_get_active_monitor();

    char mon_json[1024] = "[";
    size_t pos = 1;
    for (int i = 0; i < mon_count; i++) {
        int written = snprintf(
            mon_json + pos, sizeof(mon_json) - pos,
            "%s{\"id\":%d,\"name\":\"%s\",\"w\":%d,\"h\":%d}",
            (i > 0) ? "," : "",
            mons[i].id, mons[i].name, mons[i].width, mons[i].height
        );
        if (written > 0 && pos + (size_t)written < sizeof(mon_json) - 2) {
            pos += (size_t)written;
        }
    }
    mon_json[pos++] = ']';
    mon_json[pos] = '\0';

    char json[1536];
    snprintf(json, sizeof(json),
             "{\"type\":\"state\",\"screen_w\":%d,\"screen_h\":%d,\"stream_w\":%d,\"stream_h\":%d,\"fps\":%d,\"active_monitor\":%d,\"monitors\":%s}",
             screen_w, screen_h, stream_w, stream_h, g_target_fps, active_mon, mon_json);

    size_t jlen = strlen(json);
    uint8_t *resp = (uint8_t*)malloc(1 + jlen);
    if (!resp) return;
    resp[0] = 0x03; /* PKT_JSON */
    memcpy(resp + 1, json, jlen);

    if (target_c) {
        mg_ws_send(target_c, (const char*)resp, 1 + jlen, WEBSOCKET_OP_BINARY);
    } else {
        for (struct mg_connection *c = g_mgr.conns; c != NULL; c = c->next) {
            if (c->is_websocket) {
                mg_ws_send(c, (const char*)resp, 1 + jlen, WEBSOCKET_OP_BINARY);
            }
        }
    }
    free(resp);
}

static const char* get_web_root(void) {
    static char root_buf[MAX_PATH] = {0};
    if (root_buf[0] != '\0') return root_buf;

    const char *candidates[] = {
        "web",
        "../web",
        "phf-rdp/web",
        "../../phf-rdp/web"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        DWORD attr = GetFileAttributesA(candidates[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            strcpy_s(root_buf, sizeof(root_buf), candidates[i]);
            return root_buf;
        }
    }
    strcpy_s(root_buf, sizeof(root_buf), "web");
    return root_buf;
}

static void handle_input_packet(const uint8_t *buf, size_t len) {
    if (len < 2) return;
    uint8_t sub = buf[1];
    if (sub == 0x01 && len >= 6) {
        /* MOUSE_MOVE: [0x04][0x01][x_u16][y_u16] */
        uint16_t x_norm = (uint16_t)(buf[2] | (buf[3] << 8));
        uint16_t y_norm = (uint16_t)(buf[4] | (buf[5] << 8));
        input_inject_mouse_move(x_norm, y_norm);
        InterlockedExchange(&g_input_pending, 1);
    } else if (sub == 0x02 && len >= 8) {
        /* MOUSE_BUTTON: [0x04][0x02][btn][down][x_u16][y_u16] */
        uint8_t btn  = buf[2];
        uint8_t down = buf[3];
        uint16_t x_norm = (uint16_t)(buf[4] | (buf[5] << 8));
        uint16_t y_norm = (uint16_t)(buf[6] | (buf[7] << 8));
        input_inject_mouse_button(btn, down, x_norm, y_norm);
        InterlockedExchange(&g_input_pending, 1);
    } else if (sub == 0x03 && len >= 6) {
        /* MOUSE_WHEEL: [0x04][0x03][dx_i16][dy_i16] */
        int16_t dx = (int16_t)(buf[2] | (buf[3] << 8));
        int16_t dy = (int16_t)(buf[4] | (buf[5] << 8));
        input_inject_mouse_wheel(dx, dy);
        InterlockedExchange(&g_input_pending, 1);
    } else if (sub == 0x04 && len >= 5) {
        /* KEY (VK): [0x04][0x04][vk_u16][down] */
        uint16_t vk  = (uint16_t)(buf[2] | (buf[3] << 8));
        uint8_t down = buf[4];
        input_inject_key(vk, down);
        InterlockedExchange(&g_input_pending, 1);
    } else if (sub == 0x05 && len >= 4) {
        /* UNICODE CHAR: [0x04][0x05][utf16_u16] */
        uint16_t ch = (uint16_t)(buf[2] | (buf[3] << 8));
        input_inject_unicode(ch);
        InterlockedExchange(&g_input_pending, 1);
    } else if (sub == 0x06 && len >= 3) {
        /* CURSOR MODE: [0x04][0x06][draw_server_cursor (0 or 1)] */
        capture_set_draw_cursor(buf[2]);
    } else if (sub == 0x07 && len >= 3) {
        /* AUDIO MODE: [0x04][0x07][audio_enabled (0 or 1)] */
        InterlockedExchange(&g_audio_enabled, buf[2] ? 1 : 0);
    } else if (sub == 0x08) {
        /* RESET MODIFIERS: [0x04][0x08] */
        input_release_all_modifiers();
    }
}

/* Handle WebSocket and HTTP events */
static void fn_http_ws(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_ACCEPT) {
        set_tcp_nodelay(c);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        struct mg_str *upgrade = mg_http_get_header(hm, "Upgrade");
        if (upgrade != NULL || mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else {
            struct mg_http_serve_opts opts = {
                .root_dir = get_web_root(),
                .mime_types = "wasm=application/wasm,js=text/javascript,css=text/css",
                .extra_headers = "Cache-Control: no-cache\r\n"
            };
            mg_http_serve_dir(c, hm, &opts);
        }
    } else if (ev == MG_EV_WS_OPEN) {
        set_tcp_nodelay(c);
        c->fn_data = NULL; /* Defaults to video/main client */
        g_ws_client_count++;
        InterlockedExchange(&g_force_keyframe, 1);
        input_release_all_modifiers();
        send_server_state(c);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        if (wm->data.len > 0) {
            const uint8_t *buf = (const uint8_t*)wm->data.buf;
            uint8_t opcode = buf[0];
            if (opcode == 0x04) {
                if (wm->data.len >= 3 && buf[1] == 0x07) {
                    /* Tag connection: if buf[2] == 1, this socket is a dedicated audio stream */
                    c->fn_data = (buf[2] != 0) ? (void*)(uintptr_t)1 : NULL;
                }
                handle_input_packet(buf, wm->data.len);
            } else if (opcode == 0x05 && wm->data.len > 2) {
                int trigger_paste = (buf[1] != 0) ? 1 : 0;
                printf("[Clipboard] Received %zu bytes from client (trigger_paste=%d)\n", wm->data.len - 2, trigger_paste);
                clipboard_set_utf8((const char*)(buf + 2), wm->data.len - 2, trigger_paste);
                InterlockedExchange(&g_input_pending, 1);
            } else if (opcode == 0x03 && wm->data.len > 1) {
                char *cmd_str = (char*)malloc(wm->data.len);
                if (cmd_str) {
                    memcpy(cmd_str, buf + 1, wm->data.len - 1);
                    cmd_str[wm->data.len - 1] = '\0';

                    int req_w = 0, req_h = 0, mon_id = -1;
                    if (sscanf(cmd_str, "{\"cmd\":\"resolution\",\"w\":%d,\"h\":%d}", &req_w, &req_h) == 2) {
                        if ((req_w >= 320 && req_h >= 180) || (req_w == 0 && req_h == 0)) {
                            g_target_w = req_w;
                            g_target_h = req_h;
                            capture_set_resolution(g_target_w, g_target_h);
                            InterlockedExchange(&g_force_keyframe, 1);
                            send_server_state(NULL);
                        }
                    } else if (sscanf(cmd_str, "{\"cmd\":\"monitor\",\"id\":%d}", &mon_id) == 1) {
                        if (mon_id >= 0 && mon_id < PHF_MAX_MONITORS) {
                            capture_set_monitor(mon_id);
                            InterlockedExchange(&g_force_keyframe, 1);
                            send_server_state(NULL);
                        }
                    }
                    free(cmd_str);
                }
            }
        }
    } else if (ev == MG_EV_CLOSE) {
        if (c->is_websocket && g_ws_client_count > 0) {
            g_ws_client_count--;
        }
    }
}

/**
 * Find the dirty bounding rectangle between curr_rgb and prev_rgb (W x H x 3).
 * Returns:
 *   0 if the two frames are 100% identical.
 *   1 if differences were found, filling (*out_x, *out_y, *out_w, *out_h).
 */
static int find_dirty_rect_rgb(
    const uint8_t *curr_rgb, const uint8_t *prev_rgb,
    int w, int h,
    int *out_x, int *out_y, int *out_w, int *out_h)
{
    size_t row_bytes = (size_t)w * 3;
    int min_y = h, max_y = -1;
    int min_x = w, max_x = -1;

    for (int y = 0; y < h; y++) {
        const uint8_t *c_row = curr_rgb + (size_t)y * row_bytes;
        const uint8_t *p_row = prev_rgb + (size_t)y * row_bytes;

        if (memcmp(c_row, p_row, row_bytes) == 0) {
            continue;
        }

        if (y < min_y) min_y = y;
        max_y = y;

        int x_limit = (min_x < w) ? min_x : w;
        for (int x = 0; x < x_limit; x++) {
            const uint8_t *cp = c_row + x * 3;
            const uint8_t *pp = p_row + x * 3;
            if (cp[0] != pp[0] || cp[1] != pp[1] || cp[2] != pp[2]) {
                min_x = x;
                break;
            }
        }

        int x_floor = (max_x >= 0) ? max_x : min_x;
        for (int x = w - 1; x >= x_floor; x--) {
            const uint8_t *cp = c_row + x * 3;
            const uint8_t *pp = p_row + x * 3;
            if (cp[0] != pp[0] || cp[1] != pp[1] || cp[2] != pp[2]) {
                if (x > max_x) max_x = x;
                break;
            }
        }
    }

    if (max_y < min_y || max_x < min_x) {
        return 0;
    }

    int rx = (min_x > 2) ? ((min_x - 2) & ~1) : 0;
    int ry = (min_y > 2) ? ((min_y - 2) & ~1) : 0;
    int rx2 = (max_x + 3 < w) ? ((max_x + 3) & ~1) : w;
    int ry2 = (max_y + 3 < h) ? ((max_y + 3) & ~1) : h;

    int rw = rx2 - rx;
    int rh = ry2 - ry;
    if (rw < 16) {
        rw = 16;
        if (rx + rw > w) rx = (w >= 16) ? (w - 16) : 0;
    }
    if (rh < 16) {
        rh = 16;
        if (ry + rh > h) ry = (h >= 16) ? (h - 16) : 0;
    }

    *out_x = rx;
    *out_y = ry;
    *out_w = rw;
    *out_h = rh;
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI capture_thread_func(LPVOID arg)
#else
static void* capture_thread_func(void *arg)
#endif
{
    (void)arg;
    capture_config_t config = {
        .target_w = g_target_w,
        .target_h = g_target_h,
        .fps = g_target_fps
    };

    if (capture_init(&config) != 0) {
        fprintf(stderr, "[Server] Failed to initialize screen capture!\n");
        return 0;
    }

    phf_encode_options_t phf_opts;
    phf_opts.effort = PHF_EFFORT_FAST;
    phf_opts.mode = PHF_MODE_MED;

    int sw = 0, sh = 0, tw = 0, th = 0;
    capture_get_geometry(&sw, &sh, &tw, &th);
    printf("[Server] Screen capture engine ready (Source %dx%d -> Stream %dx%d @ %d FPS target)\n",
           sw, sh, tw, th, g_target_fps);

    uint8_t *prev_rgb = NULL;
    uint8_t *sub_rgb  = NULL;
    size_t   prev_cap = 0;
    int      prev_w = 0, prev_h = 0;
    int      frames_since_keyframe = 0;

    capture_frame_t frame;
    double target_interval_sec = 1.0 / (double)g_target_fps;
    double next_frame_time = get_time_seconds();

    while (g_running) {
#if defined(_WIN32)
        if (g_server_paused) {
            Sleep(50);
            next_frame_time = get_time_seconds();
            continue;
        }
#endif
        if (g_ws_client_count <= 0) {
#if defined(_WIN32)
            Sleep(30);
#else
            usleep(30000);
#endif
            next_frame_time = get_time_seconds();
            continue;
        }

        if (g_latest_pkt != NULL) {
#if defined(_WIN32)
            Sleep(1);
#endif
            continue;
        }

        int res = capture_acquire_frame(&frame, 0);
        if (res == 0) {
            int w = frame.width;
            int h = frame.height;
            size_t full_bytes = (size_t)w * (size_t)h * 3;

            if (full_bytes > prev_cap || !prev_rgb || !sub_rgb) {
                free(prev_rgb);
                free(sub_rgb);
                prev_rgb = (uint8_t*)malloc(full_bytes + 64);
                sub_rgb  = (uint8_t*)malloc(full_bytes + 64);
                prev_cap = full_bytes;
                prev_w = 0;
                prev_h = 0;
            }

            bool need_keyframe = (InterlockedCompareExchange(&g_force_keyframe, 0, 1) == 1) ||
                                 (w != prev_w || h != prev_h) ||
                                 (frames_since_keyframe >= 240);

            int rx = 0, ry = 0, rw = w, rh = h;
            int has_changes = 1;

            if (!need_keyframe && prev_rgb) {
                has_changes = find_dirty_rect_rgb(frame.rgb_data, prev_rgb, w, h, &rx, &ry, &rw, &rh);
                if (has_changes && ((int64_t)rw * (int64_t)rh >= ((int64_t)w * (int64_t)h * 65) / 100)) {
                    need_keyframe = true;
                }
            }

            if (need_keyframe) {
                double t_enc_start = get_time_seconds();
                size_t phf_size = 0;
                uint8_t *phf_data = phf_encode_memory(
                    frame.rgb_data,
                    (uint32_t)w,
                    (uint32_t)h,
                    3,
                    &phf_opts,
                    &phf_size
                );
                double t_enc_end = get_time_seconds();
                float enc_ms = (float)((t_enc_end - t_enc_start) * 1000.0);

                if (prev_rgb) {
                    memcpy(prev_rgb, frame.rgb_data, full_bytes);
                    prev_w = w;
                    prev_h = h;
                }
                capture_release_frame();

                if (phf_data && phf_size > 0) {
                    publish_phf_keyframe(phf_data, phf_size, t_enc_start, enc_ms);
                    phf_free(phf_data);
                    frames_since_keyframe = 0;
                }
            } else if (has_changes && sub_rgb && prev_rgb) {
                double t_enc_start = get_time_seconds();
                size_t src_stride = (size_t)w * 3;
                size_t sub_stride = (size_t)rw * 3;
                for (int y = 0; y < rh; y++) {
                    const uint8_t *src_line = frame.rgb_data + (size_t)(ry + y) * src_stride + (size_t)rx * 3;
                    uint8_t *prev_line = prev_rgb + (size_t)(ry + y) * src_stride + (size_t)rx * 3;
                    uint8_t *dst_line = sub_rgb + (size_t)y * sub_stride;
                    memcpy(dst_line, src_line, sub_stride);
                    memcpy(prev_line, src_line, sub_stride);
                }
                capture_release_frame();

                size_t phf_size = 0;
                uint8_t *phf_data = phf_encode_memory(
                    sub_rgb,
                    (uint32_t)rw,
                    (uint32_t)rh,
                    3,
                    &phf_opts,
                    &phf_size
                );
                double t_enc_end = get_time_seconds();
                float enc_ms = (float)((t_enc_end - t_enc_start) * 1000.0);

                if (phf_data && phf_size > 0) {
                    publish_phf_delta(
                        phf_data, phf_size,
                        t_enc_start, enc_ms,
                        (uint16_t)rx, (uint16_t)ry,
                        (uint16_t)w, (uint16_t)h
                    );
                    phf_free(phf_data);
                    frames_since_keyframe++;
                }
            } else {
                capture_release_frame();
            }
        } else if (res == 1) {
#if defined(_WIN32)
            Sleep(1);
#else
            usleep(1000);
#endif
            next_frame_time = get_time_seconds();
            continue;
        } else {
#if defined(_WIN32)
            Sleep(2);
#else
            usleep(2000);
#endif
            next_frame_time = get_time_seconds();
            continue;
        }

        /* High-precision 60 FPS frame pacing */
        next_frame_time += target_interval_sec;
        double now = get_time_seconds();
        if (next_frame_time < now) {
            next_frame_time = now;
        } else {
            while ((next_frame_time - now) > 0.0018) {
                if (InterlockedCompareExchange(&g_input_pending, 0, 1) == 1) {
#if defined(_WIN32)
                    Sleep(1);
#endif
                    next_frame_time = get_time_seconds();
                    break;
                }
#if defined(_WIN32)
                Sleep(1);
#else
                usleep(1000);
#endif
                now = get_time_seconds();
            }
            while (get_time_seconds() < next_frame_time) {
#if defined(_WIN32)
                SwitchToThread();
#endif
            }
        }
    }

    free(prev_rgb);
    free(sub_rgb);
    capture_close();
    return 0;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#if defined(_WIN32)
    SetProcessDPIAware();
    timeBeginPeriod(1); /* Enable 1ms OS scheduler timer precision for 60 FPS */
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif

    mg_log_set(MG_LL_ERROR);

    printf("====================================================\n");
    printf("  PHF-RDP Server (Native C Screen Stream Engine)    \n");
    printf("====================================================\n");

    if (argc >= 3) {
        g_target_w = atoi(argv[1]);
        g_target_h = atoi(argv[2]);
    }
    if (argc >= 4) {
        g_target_fps = atoi(argv[3]);
    }

#if defined(_WIN32)
    InitializeCriticalSection(&g_pkt_lock);
#endif

    mg_mgr_init(&g_mgr);

    if (mg_http_listen(&g_mgr, HTTP_URL, fn_http_ws, NULL) == NULL) {
        fprintf(stderr, "Failed to listen on %s\n", HTTP_URL);
        return 1;
    }
    if (mg_http_listen(&g_mgr, WS_URL, fn_http_ws, NULL) == NULL) {
        fprintf(stderr, "Failed to listen on %s\n", WS_URL);
        return 1;
    }

    printf("[Server] HTTP Dashboard: %s\n", HTTP_URL);
    printf("[Server] WebSocket:      %s\n", WS_URL);
    printf("[Server] Initial Res:    %dx%d @ %d FPS\n", g_target_w, g_target_h, g_target_fps);

    uint32_t aud_rate = 48000;
    uint16_t aud_ch = 2;
    int aud_ok = (audio_loopback_init(&aud_rate, &aud_ch) == 0);
    int16_t *aud_pcm_buf = aud_ok ? (int16_t*)malloc(48000 * 2 * sizeof(int16_t)) : NULL;

#if defined(_WIN32)
    HANDLE hThread = CreateThread(NULL, 0, capture_thread_func, NULL, 0, NULL);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, capture_thread_func, NULL);
#endif

    uint64_t last_sent_seq = 0;
    double last_clip_poll = get_time_seconds();
    double last_aud_poll = get_time_seconds();
#if defined(_WIN32)
    tray_init(GetModuleHandleA(NULL), on_tray_action);
    update_tray_status();
    double last_tray_poll = get_time_seconds();
#endif

    while (g_running) {
        mg_mgr_poll(&g_mgr, 1);
        flush_latest_frame_to_clients(&last_sent_seq);

        double now = get_time_seconds();
#if defined(_WIN32)
        tray_poll();
        if (now - last_tray_poll >= 0.5) {
            last_tray_poll = now;
            update_tray_status();
        }
#endif

        if (g_ws_client_count > 0) {

            /* Poll WASAPI loopback audio in 40ms batches (25 pkt/s instead of 125 pkt/s)
             * so audio never contends with video frames or main-thread WebSocket events */
            if (aud_ok && aud_pcm_buf && (now - last_aud_poll >= 0.040)) {
                last_aud_poll = now;
                int frames_read = audio_loopback_read(aud_pcm_buf, 16384, &aud_rate, &aud_ch);
                if (frames_read > 0 && g_audio_enabled) {
                    broadcast_audio_pcm(aud_pcm_buf, frames_read, aud_rate, aud_ch);
                }
            }

            /* Poll host clipboard sequence number every 150ms */
            if (now - last_clip_poll >= 0.15) {
                last_clip_poll = now;
                size_t clip_len = 0;
                char *clip_utf8 = clipboard_poll_utf8_change(&clip_len);
                if (clip_utf8) {
                    broadcast_host_clipboard(clip_utf8, clip_len);
                    free(clip_utf8);
                }
            }
        }
    }

#if defined(_WIN32)
    tray_close();
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
    timeEndPeriod(1);
#else
    pthread_join(thread, NULL);
#endif

    free(aud_pcm_buf);
    audio_loopback_close();
    mg_mgr_free(&g_mgr);
    printf("[Server] Stopped.\n");
    return 0;
}
