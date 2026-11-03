#if defined(_WIN32)
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

#include "tray_win.h"

#define WM_TRAY_NOTIFY (WM_USER + 201)

#define IDM_STATUS_TITLE   3001
#define IDM_STATUS_RES     3002
#define IDM_STATUS_CLIENTS 3003
#define IDM_CLIENT_FIRST   3100
#define IDM_OPEN_BROWSER   4001
#define IDM_COPY_URL       4002
#define IDM_TOGGLE_CONSOLE 4003
#define IDM_TOGGLE_PAUSE   4004
#define IDM_EXIT           4005

static HWND g_tray_hwnd = NULL;
static HANDLE g_tray_thread = NULL;
static DWORD g_tray_thread_id = 0;
static NOTIFYICONDATAW g_nid = {0};
static tray_action_cb_t g_tray_cb = NULL;
static tray_status_info_t g_current_status = {0};
static CRITICAL_SECTION g_tray_lock;
static HICON g_hicon_active = NULL;
static HICON g_hicon_paused = NULL;
static bool g_console_hidden = false;
static volatile bool g_tray_running = false;

/* Create an antialiased 16x16 32-bit ARGB badge icon */
static HICON create_tray_icon(bool is_paused) {
    int w = 16, h = 16;
    BITMAPV5HEADER bi = {0};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = w;
    bi.bV5Height = -h; /* top-down */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(NULL);
    uint32_t *pixels = NULL;
    HBITMAP hbmColor = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    HBITMAP hbmMask = CreateBitmap(w, h, 1, 1, NULL);
    ReleaseDC(NULL, hdc);

    if (!hbmColor || !pixels) return LoadIcon(NULL, IDI_APPLICATION);

    /* Vibrant cyan for active, warm amber for paused */
    uint32_t col_badge = is_paused ? 0xFFF59E0B : 0xFF0284C7;
    uint32_t col_glow  = is_paused ? 0xFFFBBF24 : 0xFF38BDF8;
    uint32_t col_ring  = 0xFF0F172A;
    uint32_t col_white = 0xFFFFFFFF;

    float cx = 7.5f, cy = 7.5f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist <= 6.8f) {
                if (dist > 5.4f) {
                    pixels[y * w + x] = col_ring;
                } else if (dist > 4.2f) {
                    pixels[y * w + x] = col_badge;
                } else if (dist > 2.4f) {
                    pixels[y * w + x] = col_glow;
                } else {
                    pixels[y * w + x] = col_white;
                }
            } else if (dist <= 7.8f) {
                uint32_t alpha = (uint32_t)((7.8f - dist) * 180.0f);
                pixels[y * w + x] = (alpha << 24) | (col_ring & 0x00FFFFFF);
            } else {
                pixels[y * w + x] = 0;
            }
        }
    }

    ICONINFO ii = {0};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return hIcon;
}

static void copy_string_to_clipboard(const char *text) {
    if (!text || !OpenClipboard(g_tray_hwnd)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) {
        memcpy(GlobalLock(hMem), text, len);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

static void get_local_access_url(char *out_url, size_t max_len) {
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent *he = gethostbyname(hostname);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            struct in_addr addr;
            memcpy(&addr, he->h_addr_list[0], sizeof(struct in_addr));
            snprintf(out_url, max_len, "http://%s:8080", inet_ntoa(addr));
            return;
        }
    }
    snprintf(out_url, max_len, "http://localhost:8080");
}

static void append_menu_utf8(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, const char *utf8_text) {
    if (!utf8_text) {
        AppendMenuW(hMenu, uFlags, uIDNewItem, NULL);
        return;
    }
    wchar_t wbuf[256];
    MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, wbuf, sizeof(wbuf) / sizeof(wbuf[0]));
    AppendMenuW(hMenu, uFlags, uIDNewItem, wbuf);
}

static void show_tray_context_menu(void) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    EnterCriticalSection(&g_tray_lock);
    tray_status_info_t st = g_current_status;
    LeaveCriticalSection(&g_tray_lock);

    /* 1. Header & Status */
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "● PHF-RDP Server v1.3 [%s]",
             st.is_paused ? "PAUSADO" : "ATIVO");
    append_menu_utf8(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_STATUS_TITLE, title_buf);

    char res_buf[128];
    if (st.source_w > 0 && st.stream_w > 0) {
        snprintf(res_buf, sizeof(res_buf), "  Tela: %dx%d -> %dx%d @ %d FPS",
                 st.source_w, st.source_h,
                 st.stream_w, st.stream_h,
                 st.fps);
    } else {
        snprintf(res_buf, sizeof(res_buf), "  Transmissão: 60 FPS Target");
    }
    append_menu_utf8(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_STATUS_RES, res_buf);

    /* 2. Connected Clients info */
    char clients_hdr[128];
    if (st.client_count <= 0) {
        snprintf(clients_hdr, sizeof(clients_hdr), "  Clientes conectados: Nenhum (Ocioso)");
        append_menu_utf8(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_STATUS_CLIENTS, clients_hdr);
    } else {
        snprintf(clients_hdr, sizeof(clients_hdr), "  Clientes conectados (%d):", st.client_count);
        append_menu_utf8(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_STATUS_CLIENTS, clients_hdr);

        for (int i = 0; i < st.clients_len && i < TRAY_MAX_CLIENT_ENTRIES; i++) {
            char client_row[128];
            snprintf(client_row, sizeof(client_row), "    • %s%s",
                     st.clients[i].ip_port,
                     st.clients[i].is_audio ? " [Áudio]" : " [Vídeo/RDP]");
            append_menu_utf8(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_CLIENT_FIRST + i, client_row);
        }
    }

    append_menu_utf8(hMenu, MF_SEPARATOR, 0, NULL);

    /* 3. Actions */
    append_menu_utf8(hMenu, MF_STRING, IDM_OPEN_BROWSER, "🌐 Abrir Dashboard (http://localhost:8080)");

    char url_buf[128];
    get_local_access_url(url_buf, sizeof(url_buf));
    char copy_item[160];
    snprintf(copy_item, sizeof(copy_item), "📋 Copiar Link de Acesso (%s)", url_buf);
    append_menu_utf8(hMenu, MF_STRING, IDM_COPY_URL, copy_item);

    HWND hConsole = GetConsoleWindow();
    if (hConsole) {
        append_menu_utf8(hMenu, MF_STRING, IDM_TOGGLE_CONSOLE,
                    g_console_hidden ? "👁 Exibir Janela do Console" : "👁 Ocultar Janela do Console");
    }

    append_menu_utf8(hMenu, MF_SEPARATOR, 0, NULL);

    /* 4. Pause / Resume */
    if (st.is_paused) {
        append_menu_utf8(hMenu, MF_STRING, IDM_TOGGLE_PAUSE, "▶ Retomar Servidor");
    } else {
        append_menu_utf8(hMenu, MF_STRING, IDM_TOGGLE_PAUSE, "⏸ Pausar Servidor");
    }

    append_menu_utf8(hMenu, MF_SEPARATOR, 0, NULL);

    /* 5. Exit */
    append_menu_utf8(hMenu, MF_STRING, IDM_EXIT, "❌ Fechar Servidor");

    /* Track menu */
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_tray_hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_tray_hwnd, NULL);
    PostMessageA(g_tray_hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TRAY_NOTIFY) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            show_tray_context_menu();
            return 0;
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShellExecuteA(NULL, "open", "http://localhost:8080", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
    } else if (uMsg == WM_COMMAND) {
        int id = LOWORD(wParam);
        if (id == IDM_OPEN_BROWSER) {
            ShellExecuteA(NULL, "open", "http://localhost:8080", NULL, NULL, SW_SHOWNORMAL);
        } else if (id == IDM_COPY_URL) {
            char url[128];
            get_local_access_url(url, sizeof(url));
            copy_string_to_clipboard(url);
        } else if (id == IDM_TOGGLE_CONSOLE) {
            HWND hConsole = GetConsoleWindow();
            if (hConsole) {
                g_console_hidden = !g_console_hidden;
                ShowWindow(hConsole, g_console_hidden ? SW_HIDE : SW_SHOW);
            }
        } else if (id == IDM_TOGGLE_PAUSE) {
            if (g_tray_cb) g_tray_cb(TRAY_ACTION_TOGGLE_PAUSE);
        } else if (id == IDM_EXIT) {
            if (g_tray_cb) g_tray_cb(TRAY_ACTION_EXIT);
        }
        return 0;
    } else if (uMsg == WM_DESTROY) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

static DWORD WINAPI tray_thread_func(LPVOID lpParam) {
    (void)lpParam;

    /* Attach dedicated tray thread to the interactive user desktop */
    HDESK hInput = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (hInput) {
        SetThreadDesktop(hInput);
        CloseDesktop(hInput);
    } else {
        HDESK hDef = OpenDesktopA("Default", 0, FALSE, GENERIC_ALL);
        if (hDef) {
            SetThreadDesktop(hDef);
            CloseDesktop(hDef);
        }
    }

    HINSTANCE hInstance = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = tray_wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "PHF_RDP_TrayClass";
    RegisterClassA(&wc);

    /* Use a top-level invisible WS_POPUP window with WS_EX_TOOLWINDOW (NOT HWND_MESSAGE)
     * so it receives all Shell_NotifyIcon notifications and context menu events properly */
    g_tray_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        "PHF_RDP_TrayClass", "PHF_RDP_TrayWindow",
        WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    if (!g_tray_hwnd) {
        return 1;
    }

    g_hicon_active = create_tray_icon(false);
    g_hicon_paused = create_tray_icon(true);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_tray_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAY_NOTIFY;
    g_nid.hIcon = g_hicon_active;

    MultiByteToWideChar(CP_UTF8, 0, "PHF-RDP Server (Ativo)\nhttp://localhost:8080", -1,
                        g_nid.szTip, sizeof(g_nid.szTip) / sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, "PHF-RDP Server Ativo", -1,
                        g_nid.szInfoTitle, sizeof(g_nid.szInfoTitle) / sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, "Streaming 60 FPS pronto em http://localhost:8080", -1,
                        g_nid.szInfo, sizeof(g_nid.szInfo) / sizeof(wchar_t));
    g_nid.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_tray_running = true;

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hicon_active) DestroyIcon(g_hicon_active);
    if (g_hicon_paused) DestroyIcon(g_hicon_paused);
    g_hicon_active = NULL;
    g_hicon_paused = NULL;
    g_tray_running = false;
    return 0;
}

int tray_init(HINSTANCE hInstance, tray_action_cb_t cb) {
    (void)hInstance;
    g_tray_cb = cb;
    InitializeCriticalSection(&g_tray_lock);

    g_tray_thread = CreateThread(NULL, 0, tray_thread_func, NULL, 0, &g_tray_thread_id);
    if (!g_tray_thread) return -1;
    return 0;
}

void tray_poll(void) {
    /* No-op: handled in dedicated tray thread without blocking main server loop */
}

void tray_update(const tray_status_info_t *info) {
    if (!info || !g_tray_running || !g_tray_hwnd) return;

    EnterCriticalSection(&g_tray_lock);
    g_current_status = *info;
    LeaveCriticalSection(&g_tray_lock);

    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = info->is_paused ? g_hicon_paused : g_hicon_active;

    char tip_buf[256];
    if (info->is_paused) {
        snprintf(tip_buf, sizeof(tip_buf),
                 "PHF-RDP Server (Pausado)\nClientes: %d\nhttp://localhost:8080",
                 info->client_count);
    } else {
        snprintf(tip_buf, sizeof(tip_buf),
                 "PHF-RDP Server (%s)\n%dx%d @ %d FPS • Clientes: %d",
                 info->status_str ? info->status_str : "Ativo",
                 info->stream_w, info->stream_h, info->fps,
                 info->client_count);
    }
    MultiByteToWideChar(CP_UTF8, 0, tip_buf, -1,
                        g_nid.szTip, sizeof(g_nid.szTip) / sizeof(wchar_t));

    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void tray_close(void) {
    if (g_tray_hwnd) {
        PostMessageA(g_tray_hwnd, WM_DESTROY, 0, 0);
    }
    if (g_tray_thread) {
        WaitForSingleObject(g_tray_thread, 1000);
        CloseHandle(g_tray_thread);
        g_tray_thread = NULL;
    }
    DeleteCriticalSection(&g_tray_lock);
}

#endif /* _WIN32 */
