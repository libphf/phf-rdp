#ifndef TRAY_WIN_H
#define TRAY_WIN_H

#if defined(_WIN32)
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#define TRAY_MAX_CLIENT_ENTRIES 16

typedef struct {
    char ip_port[64];
    bool is_audio;
} tray_client_entry_t;

typedef struct {
    const char *status_str;     /* e.g. "Ativo (60 FPS)", "Pausado", "Ocioso" */
    int client_count;           /* total active clients */
    int source_w;
    int source_h;
    int stream_w;
    int stream_h;
    int fps;
    bool is_paused;
    tray_client_entry_t clients[TRAY_MAX_CLIENT_ENTRIES];
    int clients_len;
} tray_status_info_t;

/* Tray action callbacks */
#define TRAY_ACTION_TOGGLE_PAUSE   1
#define TRAY_ACTION_EXIT           2
#define TRAY_ACTION_OPEN_BROWSER   3
#define TRAY_ACTION_COPY_URL       4
#define TRAY_ACTION_TOGGLE_CONSOLE 5

typedef void (*tray_action_cb_t)(int action_id);

/* Initialize tray window and notification icon */
int tray_init(HINSTANCE hInstance, tray_action_cb_t cb);

/* Process Windows message queue for tray icon (call in main loop) */
void tray_poll(void);

/* Update tray icon status data and tooltip */
void tray_update(const tray_status_info_t *info);

/* Cleanly remove tray icon and destroy hidden window */
void tray_close(void);

#endif /* _WIN32 */
#endif /* TRAY_WIN_H */
