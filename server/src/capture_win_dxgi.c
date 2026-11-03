#if defined(_WIN32)

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "capture.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <tmmintrin.h> /* SSSE3 _mm_shuffle_epi8 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

typedef enum {
    CAPTURE_BACKEND_DXGI = 0,
    CAPTURE_BACKEND_GDI  = 1
} capture_backend_t;

typedef struct {
    capture_backend_t       backend;
    HDESK                   hdesk;
    /* DXGI resources */
    ID3D11Device           *device;
    ID3D11DeviceContext    *context;
    IDXGIOutputDuplication *duplication;
    ID3D11Texture2D        *staging_texture;
    int                     frame_acquired;
    int                     dxgi_has_frame;
    int                     idle_ticks;

    /* GDI fallback resources */
    HDC                     hdc_screen;
    HDC                     hdc_mem;
    HBITMAP                 hbm_screen;
    uint8_t                *gdi_bits;

    /* Monitor & Geometry state */
    int                     active_monitor_id;
    int                     monitor_left;
    int                     monitor_top;
    int                     screen_w;
    int                     screen_h;
    int                     req_w;
    int                     req_h;
    int                     target_w;
    int                     target_h;
    volatile LONG           res_dirty;
    volatile LONG           mon_dirty;
    volatile LONG           draw_cursor;
    uint8_t                *rgb_buffer;
    size_t                  rgb_buffer_size;
} capture_state_t;

static capture_state_t g_cap = { .draw_cursor = 1, .active_monitor_id = 0 };
__declspec(thread) static int t_desktop_attached = 0;

static void ensure_thread_desktop(void) {
    if (!t_desktop_attached) {
        HDESK hInput = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (hInput) {
            SetThreadDesktop(hInput);
            CloseDesktop(hInput);
        } else {
            if (!g_cap.hdesk) {
                g_cap.hdesk = OpenDesktopA("Default", 0, FALSE, GENERIC_ALL);
            }
            if (g_cap.hdesk) {
                SetThreadDesktop(g_cap.hdesk);
            }
        }
        t_desktop_attached = 1;
    }
}

static void compute_smart_resolution(int screen_w, int screen_h, int req_w, int req_h, int *out_w, int *out_h) {
    if (screen_w <= 0 || screen_h <= 0) {
        screen_w = GetSystemMetrics(SM_CXSCREEN);
        screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0) screen_w = 1920;
        if (screen_h <= 0) screen_h = 1080;
    }

    if (req_w <= 0 || req_h <= 0 || (req_w >= screen_w && req_h >= screen_h)) {
        *out_w = screen_w;
        *out_h = screen_h;
        return;
    }

    double scale_x = (double)req_w / (double)screen_w;
    double scale_y = (double)req_h / (double)screen_h;
    double scale = (scale_x < scale_y) ? scale_x : scale_y;

    if (scale >= 1.0) {
        *out_w = screen_w;
        *out_h = screen_h;
        return;
    }

    int w = (int)((double)screen_w * scale + 0.5);
    int h = (int)((double)screen_h * scale + 0.5);

    w = (w + 1) & ~1;
    h = (h + 1) & ~1;

    if (w < 320) w = 320;
    if (h < 180) h = 180;
    if (w > screen_w) w = screen_w;
    if (h > screen_h) h = screen_h;

    *out_w = w;
    *out_h = h;
}

static void ensure_rgb_buffer(int w, int h) {
    size_t req_bytes = (size_t)w * (size_t)h * 3;
    if (req_bytes > g_cap.rgb_buffer_size || !g_cap.rgb_buffer) {
        free(g_cap.rgb_buffer);
        g_cap.rgb_buffer = (uint8_t*)malloc(req_bytes + 64);
        g_cap.rgb_buffer_size = req_bytes;
    }
}

static void cleanup_resources(void) {
    if (g_cap.duplication) {
        if (g_cap.frame_acquired) {
            g_cap.duplication->lpVtbl->ReleaseFrame(g_cap.duplication);
            g_cap.frame_acquired = 0;
        }
        g_cap.duplication->lpVtbl->Release(g_cap.duplication);
        g_cap.duplication = NULL;
    }
    if (g_cap.staging_texture) {
        g_cap.staging_texture->lpVtbl->Release(g_cap.staging_texture);
        g_cap.staging_texture = NULL;
    }
    if (g_cap.context) {
        g_cap.context->lpVtbl->Release(g_cap.context);
        g_cap.context = NULL;
    }
    if (g_cap.device) {
        g_cap.device->lpVtbl->Release(g_cap.device);
        g_cap.device = NULL;
    }
    if (g_cap.hbm_screen) {
        DeleteObject(g_cap.hbm_screen);
        g_cap.hbm_screen = NULL;
    }
    if (g_cap.gdi_bits) {
        free(g_cap.gdi_bits);
        g_cap.gdi_bits = NULL;
    }
    if (g_cap.hdc_mem) {
        DeleteDC(g_cap.hdc_mem);
        g_cap.hdc_mem = NULL;
    }
    if (g_cap.hdc_screen) {
        ReleaseDC(NULL, g_cap.hdc_screen);
        g_cap.hdc_screen = NULL;
    }
    g_cap.dxgi_has_frame = 0;
}

/* Enumerate all physical and virtual (IddCx) monitors attached to desktop */
int capture_list_monitors(capture_monitor_info_t *out_monitors, int max_count) {
    if (!out_monitors || max_count <= 0) return 0;
    ensure_thread_desktop();

    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory))) {
        /* Fallback to primary GDI screen if DXGI unavailable */
        out_monitors[0].id = 0;
        snprintf(out_monitors[0].name, sizeof(out_monitors[0].name), "Monitor 1 (GDI)");
        out_monitors[0].width = GetSystemMetrics(SM_CXSCREEN);
        out_monitors[0].height = GetSystemMetrics(SM_CYSCREEN);
        out_monitors[0].left = 0;
        out_monitors[0].top = 0;
        out_monitors[0].is_primary = 1;
        return 1;
    }

    int count = 0;
    for (UINT adapter_idx = 0; count < max_count; adapter_idx++) {
        IDXGIAdapter1 *adapter = NULL;
        if (factory->lpVtbl->EnumAdapters1(factory, adapter_idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 adesc;
        adapter->lpVtbl->GetDesc1(adapter, &adesc);
        char gpu_name[64] = {0};
        WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, gpu_name, sizeof(gpu_name) - 1, NULL, NULL);

        for (UINT output_idx = 0; count < max_count; output_idx++) {
            IDXGIOutput *output = NULL;
            if (adapter->lpVtbl->EnumOutputs(adapter, output_idx, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_OUTPUT_DESC odesc;
            output->lpVtbl->GetDesc(output, &odesc);
            output->lpVtbl->Release(output);

            if (!odesc.AttachedToDesktop) continue;

            int w = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
            int h = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
            if (w <= 0 || h <= 0) continue;

            capture_monitor_info_t *m = &out_monitors[count];
            m->id = count;
            m->width = w;
            m->height = h;
            m->left = odesc.DesktopCoordinates.left;
            m->top = odesc.DesktopCoordinates.top;
            m->is_primary = (m->left == 0 && m->top == 0) ? 1 : 0;
            snprintf(m->name, sizeof(m->name), "Display %d: %dx%d (%s)", count + 1, w, h, gpu_name);
            count++;
        }
        adapter->lpVtbl->Release(adapter);
    }

    factory->lpVtbl->Release(factory);

    if (count == 0) {
        out_monitors[0].id = 0;
        snprintf(out_monitors[0].name, sizeof(out_monitors[0].name), "Monitor 1 (Default)");
        out_monitors[0].width = GetSystemMetrics(SM_CXSCREEN);
        out_monitors[0].height = GetSystemMetrics(SM_CYSCREEN);
        out_monitors[0].left = 0;
        out_monitors[0].top = 0;
        out_monitors[0].is_primary = 1;
        return 1;
    }
    return count;
}

int capture_set_monitor(int monitor_id) {
    if (monitor_id < 0 || monitor_id >= PHF_MAX_MONITORS) return -1;
    if (monitor_id != g_cap.active_monitor_id) {
        g_cap.active_monitor_id = monitor_id;
        InterlockedExchange(&g_cap.mon_dirty, 1);
    }
    return 0;
}

int capture_get_active_monitor(void) {
    return g_cap.active_monitor_id;
}

/* SSSE3 + OpenMP accelerated 1:1 BGRA -> RGB conversion */
static void convert_bgra_to_rgb_same_size(
    const uint8_t *src_bgra, int src_pitch,
    uint8_t *dst_rgb, int w, int h)
{
    const __m128i shuf_mask = _mm_setr_epi8(
        2, 1, 0,   6, 5, 4,   10, 9, 8,   14, 13, 12,
        -1, -1, -1, -1
    );
    int y;
    #pragma omp parallel for schedule(static)
    for (y = 0; y < h; y++) {
        const uint8_t *src_row = src_bgra + (size_t)y * src_pitch;
        uint8_t *dst_row = dst_rgb + (size_t)y * w * 3;
        int x = 0;
        for (; x <= w - 6; x += 4) {
            __m128i bgra = _mm_loadu_si128((const __m128i*)(src_row + x * 4));
            __m128i rgb  = _mm_shuffle_epi8(bgra, shuf_mask);
            _mm_storeu_si128((__m128i*)(dst_row + x * 3), rgb);
        }
        for (; x < w; x++) {
            const uint8_t *p = src_row + (x * 4);
            uint8_t *d = dst_row + (x * 3);
            d[0] = p[2];
            d[1] = p[1];
            d[2] = p[0];
        }
    }
}

/* Fast 16.16 fixed-point integer bilinear BGRA -> RGB downscaler */
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

        const uint8_t *row_low = src_bgra + (size_t)y_low * src_pitch;
        const uint8_t *row_high = src_bgra + (size_t)y_high * src_pitch;
        uint8_t *dst_row = dst_rgb + (size_t)y * dw * 3;

        uint32_t sx_fp = 0;
        for (int x = 0; x < dw; x++) {
            int x_low = (int)(sx_fp >> 16);
            int x_high = (x_low + 1 < sw) ? x_low + 1 : x_low;
            uint32_t wx = (sx_fp >> 8) & 0xFFu;
            uint32_t iwx = 256u - wx;
            sx_fp += x_step;

            uint32_t w00 = iwx * iwy;
            uint32_t w01 = wx  * iwy;
            uint32_t w10 = iwx * wy;
            uint32_t w11 = wx  * wy;

            const uint8_t *p00 = row_low + (x_low * 4);
            const uint8_t *p01 = row_low + (x_high * 4);
            const uint8_t *p10 = row_high + (x_low * 4);
            const uint8_t *p11 = row_high + (x_high * 4);

            uint8_t *d = dst_row + (x * 3);
            d[0] = (uint8_t)((p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 16);
            d[1] = (uint8_t)((p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 16);
            d[2] = (uint8_t)((p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 16);
        }
    }
}

/* Software cursor overlay for DXGI RGB buffer (only used in view-only mode) */
static void draw_cursor_rgb(uint8_t *rgb, int w, int h, int screen_w, int screen_h) {
    if (!g_cap.draw_cursor) return;

    CURSORINFO ci = {0};
    ci.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;

    int cx = ci.ptScreenPos.x - g_cap.monitor_left;
    int cy = ci.ptScreenPos.y - g_cap.monitor_top;
    if (cx < 0 || cy < 0 || cx >= screen_w || cy >= screen_h) return;

    int mx = (cx * w) / screen_w;
    int my = (cy * h) / screen_h;

    static const uint8_t arrow[16][12] = {
        {2,0,0,0,0,0,0,0,0,0,0,0},
        {2,2,0,0,0,0,0,0,0,0,0,0},
        {2,1,2,0,0,0,0,0,0,0,0,0},
        {2,1,1,2,0,0,0,0,0,0,0,0},
        {2,1,1,1,2,0,0,0,0,0,0,0},
        {2,1,1,1,1,2,0,0,0,0,0,0},
        {2,1,1,1,1,1,2,0,0,0,0,0},
        {2,1,1,1,1,1,1,2,0,0,0,0},
        {2,1,1,1,1,1,1,1,2,0,0,0},
        {2,1,1,1,1,1,2,2,2,2,0,0},
        {2,1,1,2,1,1,2,0,0,0,0,0},
        {2,1,2,0,2,1,1,2,0,0,0,0},
        {2,2,0,0,2,1,1,2,0,0,0,0},
        {2,0,0,0,0,2,1,1,2,0,0,0},
        {0,0,0,0,0,2,1,1,2,0,0,0},
        {0,0,0,0,0,0,2,2,0,0,0,0}
    };

    for (int dy = 0; dy < 16; dy++) {
        int py = my + dy;
        if (py < 0 || py >= h) continue;
        uint8_t *row = rgb + (size_t)py * w * 3;
        for (int dx = 0; dx < 12; dx++) {
            uint8_t v = arrow[dy][dx];
            if (!v) continue;
            int px = mx + dx;
            if (px < 0 || px >= w) continue;
            uint8_t *p = row + px * 3;
            if (v == 1) {
                p[0] = 255; p[1] = 255; p[2] = 255;
            } else {
                p[0] = 0;   p[1] = 0;   p[2] = 0;
            }
        }
    }
}

static int init_gdi_fallback(void) {
    g_cap.backend = CAPTURE_BACKEND_GDI;
    g_cap.hdc_screen = GetDC(NULL);
    g_cap.hdc_mem = CreateCompatibleDC(g_cap.hdc_screen);

    g_cap.monitor_left = 0;
    g_cap.monitor_top = 0;
    g_cap.screen_w = GetSystemMetrics(SM_CXSCREEN);
    g_cap.screen_h = GetSystemMetrics(SM_CYSCREEN);

    compute_smart_resolution(g_cap.screen_w, g_cap.screen_h, g_cap.req_w, g_cap.req_h, &g_cap.target_w, &g_cap.target_h);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_cap.screen_w;
    bmi.bmiHeader.biHeight = -g_cap.screen_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_cap.hbm_screen = CreateDIBSection(g_cap.hdc_screen, &bmi, DIB_RGB_COLORS, (void**)&g_cap.gdi_bits, NULL, 0);
    SelectObject(g_cap.hdc_mem, g_cap.hbm_screen);

    ensure_rgb_buffer(g_cap.target_w, g_cap.target_h);

    printf("[Capture] GDI BitBlt engine active (%dx%d -> %dx%d)\n",
           g_cap.screen_w, g_cap.screen_h, g_cap.target_w, g_cap.target_h);
    return 0;
}

/* Seed initial frame via GDI so DXGI never starts empty on a static desktop */
static void seed_initial_frame_gdi(void) {
    HDC hdc_s = GetDC(NULL);
    HDC hdc_m = CreateCompatibleDC(hdc_s);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_cap.screen_w;
    bmi.bmiHeader.biHeight = -g_cap.screen_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint8_t *bits = NULL;
    HBITMAP hbm = CreateDIBSection(hdc_s, &bmi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (hbm && bits) {
        SelectObject(hdc_m, hbm);
        BitBlt(hdc_m, 0, 0, g_cap.screen_w, g_cap.screen_h, hdc_s, g_cap.monitor_left, g_cap.monitor_top, SRCCOPY);
        GdiFlush();
        if (g_cap.target_w == g_cap.screen_w && g_cap.target_h == g_cap.screen_h) {
            convert_bgra_to_rgb_same_size(bits, g_cap.screen_w * 4, g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h);
        } else {
            convert_bgra_to_rgb_bilinear(bits, g_cap.screen_w * 4, g_cap.screen_w, g_cap.screen_h, g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h);
        }
        g_cap.dxgi_has_frame = 1;
        DeleteObject(hbm);
    }
    DeleteDC(hdc_m);
    ReleaseDC(NULL, hdc_s);
}

static int try_init_dxgi(void) {
    HRESULT hr;
    IDXGIFactory1 *factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory);
    if (FAILED(hr)) return -1;

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL fl;

    int current_mon_idx = 0;

    for (UINT adapter_idx = 0; ; adapter_idx++) {
        IDXGIAdapter1 *adapter = NULL;
        if (factory->lpVtbl->EnumAdapters1(factory, adapter_idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 adesc;
        adapter->lpVtbl->GetDesc1(adapter, &adesc);

        for (UINT output_idx = 0; ; output_idx++) {
            IDXGIOutput *output = NULL;
            if (adapter->lpVtbl->EnumOutputs(adapter, output_idx, &output) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            IDXGIOutput1 *output1 = NULL;
            hr = output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput1, (void**)&output1);
            output->lpVtbl->Release(output);
            if (FAILED(hr)) continue;

            DXGI_OUTPUT_DESC odesc;
            output1->lpVtbl->GetDesc(output1, &odesc);
            if (!odesc.AttachedToDesktop) {
                output1->lpVtbl->Release(output1);
                continue;
            }

            int match = (current_mon_idx == g_cap.active_monitor_id);
            current_mon_idx++;
            if (!match) {
                output1->lpVtbl->Release(output1);
                continue;
            }

            g_cap.monitor_left = odesc.DesktopCoordinates.left;
            g_cap.monitor_top  = odesc.DesktopCoordinates.top;
            g_cap.screen_w = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
            g_cap.screen_h = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;

            compute_smart_resolution(g_cap.screen_w, g_cap.screen_h, g_cap.req_w, g_cap.req_h, &g_cap.target_w, &g_cap.target_h);

            ID3D11Device *dev = NULL;
            ID3D11DeviceContext *ctx = NULL;
            hr = D3D11CreateDevice(
                (IDXGIAdapter*)adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                NULL,
                0,
                feature_levels,
                sizeof(feature_levels) / sizeof(feature_levels[0]),
                D3D11_SDK_VERSION,
                &dev,
                &fl,
                &ctx
            );

            if (SUCCEEDED(hr)) {
                IDXGIOutputDuplication *dupl = NULL;
                hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown*)dev, &dupl);
                if (SUCCEEDED(hr)) {
                    D3D11_TEXTURE2D_DESC staging_desc = {0};
                    staging_desc.Width = g_cap.screen_w;
                    staging_desc.Height = g_cap.screen_h;
                    staging_desc.MipLevels = 1;
                    staging_desc.ArraySize = 1;
                    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    staging_desc.SampleDesc.Count = 1;
                    staging_desc.Usage = D3D11_USAGE_STAGING;
                    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                    ID3D11Texture2D *staging = NULL;
                    hr = dev->lpVtbl->CreateTexture2D(dev, &staging_desc, NULL, &staging);
                    if (SUCCEEDED(hr)) {
                        g_cap.backend = CAPTURE_BACKEND_DXGI;
                        g_cap.device = dev;
                        g_cap.context = ctx;
                        g_cap.duplication = dupl;
                        g_cap.staging_texture = staging;

                        ensure_rgb_buffer(g_cap.target_w, g_cap.target_h);
                        seed_initial_frame_gdi();

                        wprintf(L"[DXGI] Hardware Duplication active on Display %d - %s (%dx%d -> %dx%d)\n",
                                g_cap.active_monitor_id + 1, adesc.Description,
                                g_cap.screen_w, g_cap.screen_h, g_cap.target_w, g_cap.target_h);

                        output1->lpVtbl->Release(output1);
                        adapter->lpVtbl->Release(adapter);
                        factory->lpVtbl->Release(factory);
                        return 0;
                    }
                    dupl->lpVtbl->Release(dupl);
                }
                dev->lpVtbl->Release(dev);
                ctx->lpVtbl->Release(ctx);
            }
            output1->lpVtbl->Release(output1);
        }
        adapter->lpVtbl->Release(adapter);
    }

    factory->lpVtbl->Release(factory);
    return -1;
}

static int init_capture(void) {
    cleanup_resources();
    ensure_thread_desktop();
    if (try_init_dxgi() == 0) {
        return 0;
    }
    /* If selected monitor index didn't match, reset to primary monitor 0 and retry once */
    if (g_cap.active_monitor_id != 0) {
        g_cap.active_monitor_id = 0;
        if (try_init_dxgi() == 0) {
            return 0;
        }
    }
    return init_gdi_fallback();
}

int capture_init(const capture_config_t *config) {
    if (config) {
        g_cap.req_w = config->target_w;
        g_cap.req_h = config->target_h;
    }
    return init_capture();
}

int capture_set_resolution(int req_w, int req_h) {
    g_cap.req_w = req_w;
    g_cap.req_h = req_h;
    InterlockedExchange(&g_cap.res_dirty, 1);
    return 0;
}

void capture_set_draw_cursor(int enabled) {
    InterlockedExchange(&g_cap.draw_cursor, enabled ? 1 : 0);
}

void capture_get_geometry(int *screen_w, int *screen_h, int *stream_w, int *stream_h) {
    int sw = g_cap.screen_w;
    int sh = g_cap.screen_h;
    if (sw <= 0 || sh <= 0) {
        sw = GetSystemMetrics(SM_CXSCREEN);
        sh = GetSystemMetrics(SM_CYSCREEN);
    }
    int tw = g_cap.target_w;
    int th = g_cap.target_h;
    compute_smart_resolution(sw, sh, g_cap.req_w, g_cap.req_h, &tw, &th);

    if (screen_w) *screen_w = sw;
    if (screen_h) *screen_h = sh;
    if (stream_w) *stream_w = tw;
    if (stream_h) *stream_h = th;
}

int capture_acquire_frame(capture_frame_t *frame, int timeout_ms) {
    (void)timeout_ms;
    ensure_thread_desktop();

    int force_refresh = 0;

    /* Apply any pending monitor switch */
    if (InterlockedCompareExchange(&g_cap.mon_dirty, 0, 1) == 1) {
        init_capture();
        force_refresh = 1;
    }

    /* Apply any pending smart resolution change safely on the capture thread */
    if (InterlockedCompareExchange(&g_cap.res_dirty, 0, 1) == 1) {
        int new_w = g_cap.target_w;
        int new_h = g_cap.target_h;
        compute_smart_resolution(g_cap.screen_w, g_cap.screen_h, g_cap.req_w, g_cap.req_h, &new_w, &new_h);
        if (new_w != g_cap.target_w || new_h != g_cap.target_h) {
            g_cap.target_w = new_w;
            g_cap.target_h = new_h;
            ensure_rgb_buffer(g_cap.target_w, g_cap.target_h);
            force_refresh = 1;
            if (g_cap.backend == CAPTURE_BACKEND_DXGI) {
                seed_initial_frame_gdi();
            }
        }
    }

    if (g_cap.backend == CAPTURE_BACKEND_GDI) {
        if (!g_cap.hdc_screen || !g_cap.hdc_mem) {
            if (init_gdi_fallback() != 0) return -1;
        }

        BitBlt(g_cap.hdc_mem, 0, 0, g_cap.screen_w, g_cap.screen_h, g_cap.hdc_screen, g_cap.monitor_left, g_cap.monitor_top, SRCCOPY);

        if (g_cap.draw_cursor) {
            CURSORINFO ci = {0};
            ci.cbSize = sizeof(CURSORINFO);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii = {0};
                int cx = ci.ptScreenPos.x - g_cap.monitor_left;
                int cy = ci.ptScreenPos.y - g_cap.monitor_top;
                if (GetIconInfo((HICON)ci.hCursor, &ii)) {
                    cx -= (int)ii.xHotspot;
                    cy -= (int)ii.yHotspot;
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
                DrawIconEx(g_cap.hdc_mem, cx, cy, (HICON)ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
            }
        }

        GdiFlush();

        if (g_cap.target_w == g_cap.screen_w && g_cap.target_h == g_cap.screen_h) {
            convert_bgra_to_rgb_same_size(
                g_cap.gdi_bits, g_cap.screen_w * 4,
                g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h
            );
        } else {
            convert_bgra_to_rgb_bilinear(
                g_cap.gdi_bits, g_cap.screen_w * 4, g_cap.screen_w, g_cap.screen_h,
                g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h
            );
        }

        frame->rgb_data = g_cap.rgb_buffer;
        frame->width = g_cap.target_w;
        frame->height = g_cap.target_h;
        frame->stride = g_cap.target_w * 3;

        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        frame->timestamp_us = (counter.QuadPart * 1000000) / freq.QuadPart;
        return 0;
    }

    /* DXGI backend */
    if (!g_cap.duplication) {
        if (init_capture() != 0) return -1;
    }

    if (g_cap.frame_acquired) {
        g_cap.duplication->lpVtbl->ReleaseFrame(g_cap.duplication);
        g_cap.frame_acquired = 0;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    IDXGIResource *desktop_resource = NULL;
    int new_dxgi_frame = 0;

    HRESULT hr = g_cap.duplication->lpVtbl->AcquireNextFrame(
        g_cap.duplication,
        0,
        &frame_info,
        &desktop_resource
    );

    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        if (init_capture() != 0) {
            init_gdi_fallback();
        }
        return -2;
    }

    if (SUCCEEDED(hr)) {
        g_cap.frame_acquired = 1;
        if (frame_info.LastPresentTime.QuadPart != 0) {
            ID3D11Texture2D *acquired_texture = NULL;
            hr = desktop_resource->lpVtbl->QueryInterface(desktop_resource, &IID_ID3D11Texture2D, (void**)&acquired_texture);
            if (SUCCEEDED(hr)) {
                g_cap.context->lpVtbl->CopyResource(
                    g_cap.context,
                    (ID3D11Resource*)g_cap.staging_texture,
                    (ID3D11Resource*)acquired_texture
                );
                acquired_texture->lpVtbl->Release(acquired_texture);
                g_cap.dxgi_has_frame = 1;
                new_dxgi_frame = 1;
            }
        }
        desktop_resource->lpVtbl->Release(desktop_resource);
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
        /* Any other unexpected DXGI failure: fallback to GDI immediately */
        init_gdi_fallback();
        return -3;
    }

    if (!g_cap.dxgi_has_frame) {
        return 1;
    }

    if (!new_dxgi_frame && !force_refresh) {
        g_cap.idle_ticks++;
        if (g_cap.idle_ticks < 6) {
            return 1;
        }
        /* If DXGI has been silent for a while (e.g. static desktop or monitor turned off),
         * test-refresh via GDI to ensure we never get stuck on a black screen */
        if (g_cap.idle_ticks >= 60) {
            seed_initial_frame_gdi();
        }
    }
    g_cap.idle_ticks = 0;

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_cap.context->lpVtbl->Map(
        g_cap.context,
        (ID3D11Resource*)g_cap.staging_texture,
        0,
        D3D11_MAP_READ,
        0,
        &mapped
    );
    if (SUCCEEDED(hr)) {
        if (g_cap.target_w == g_cap.screen_w && g_cap.target_h == g_cap.screen_h) {
            convert_bgra_to_rgb_same_size(
                (const uint8_t*)mapped.pData, mapped.RowPitch,
                g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h
            );
        } else {
            convert_bgra_to_rgb_bilinear(
                (const uint8_t*)mapped.pData, mapped.RowPitch,
                g_cap.screen_w, g_cap.screen_h,
                g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h
            );
        }
        g_cap.context->lpVtbl->Unmap(g_cap.context, (ID3D11Resource*)g_cap.staging_texture, 0);
    }

    draw_cursor_rgb(g_cap.rgb_buffer, g_cap.target_w, g_cap.target_h, g_cap.screen_w, g_cap.screen_h);

    frame->rgb_data = g_cap.rgb_buffer;
    frame->width = g_cap.target_w;
    frame->height = g_cap.target_h;
    frame->stride = g_cap.target_w * 3;

    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    frame->timestamp_us = (counter.QuadPart * 1000000) / freq.QuadPart;
    return 0;
}

void capture_release_frame(void) {
    if (g_cap.backend == CAPTURE_BACKEND_DXGI && g_cap.duplication && g_cap.frame_acquired) {
        g_cap.duplication->lpVtbl->ReleaseFrame(g_cap.duplication);
        g_cap.frame_acquired = 0;
    }
}

void capture_close(void) {
    cleanup_resources();
    if (g_cap.rgb_buffer) {
        free(g_cap.rgb_buffer);
        g_cap.rgb_buffer = NULL;
        g_cap.rgb_buffer_size = 0;
    }
}

/* ============================================================================
 * RDP Input Injection (Mouse & Keyboard via Windows SetCursorPos + SendInput)
 * ============================================================================ */

static void move_cursor_exact(uint16_t x_norm, uint16_t y_norm) {
    int sw = g_cap.screen_w > 0 ? g_cap.screen_w : GetSystemMetrics(SM_CXSCREEN);
    int sh = g_cap.screen_h > 0 ? g_cap.screen_h : GetSystemMetrics(SM_CYSCREEN);
    if (sw > 1 && sh > 1) {
        int px = g_cap.monitor_left + (int)(((uint32_t)x_norm * (uint32_t)(sw - 1) + 32767u) / 65535u);
        int py = g_cap.monitor_top  + (int)(((uint32_t)y_norm * (uint32_t)(sh - 1) + 32767u) / 65535u);
        SetCursorPos(px, py);
    }
}

void input_inject_mouse_move(uint16_t x_norm, uint16_t y_norm) {
    ensure_thread_desktop();
    move_cursor_exact(x_norm, y_norm);
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)x_norm;
    in.mi.dy = (LONG)y_norm;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(INPUT));
}

void input_inject_mouse_button(uint8_t button, uint8_t down, uint16_t x_norm, uint16_t y_norm) {
    ensure_thread_desktop();
    move_cursor_exact(x_norm, y_norm);
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)x_norm;
    in.mi.dy = (LONG)y_norm;
    DWORD flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    if (button == 0) {
        flags |= down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (button == 1) {
        flags |= down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    } else if (button == 2) {
        flags |= down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else {
        return;
    }
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(INPUT));
}

void input_inject_mouse_wheel(int16_t delta_x, int16_t delta_y) {
    ensure_thread_desktop();
    if (delta_y != 0) {
        INPUT in = {0};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)delta_y;
        SendInput(1, &in, sizeof(INPUT));
    }
    if (delta_x != 0) {
        INPUT in = {0};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = (DWORD)delta_x;
        SendInput(1, &in, sizeof(INPUT));
    }
}

void input_release_all_modifiers(void) {
    ensure_thread_desktop();
    INPUT ins[12];
    memset(ins, 0, sizeof(ins));
    WORD vks[12] = {
        VK_LSHIFT, VK_RSHIFT, VK_SHIFT,
        VK_LCONTROL, VK_RCONTROL, VK_CONTROL,
        VK_LMENU, VK_RMENU, VK_MENU,
        VK_LWIN, VK_RWIN,
        VK_CAPITAL
    };
    for (int i = 0; i < 11; i++) {
        ins[i].type = INPUT_KEYBOARD;
        ins[i].ki.wVk = vks[i];
        ins[i].ki.wScan = 0;
        ins[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(11, ins, sizeof(INPUT));
}

void input_inject_key(uint16_t vk_code, uint8_t down) {
    if (vk_code == 0 || vk_code > 255) return;
    ensure_thread_desktop();

    /* For editing and navigation keys that should never get stuck down: */
    if (vk_code == VK_BACK || vk_code == VK_RETURN || vk_code == VK_TAB || vk_code == VK_ESCAPE) {
        if (!down) return;
        INPUT ins[2];
        memset(ins, 0, sizeof(ins));
        ins[0].type = INPUT_KEYBOARD;
        ins[0].ki.wVk = vk_code;
        ins[0].ki.wScan = (WORD)MapVirtualKeyA(vk_code, MAPVK_VK_TO_VSC);

        ins[1].type = INPUT_KEYBOARD;
        ins[1].ki.wVk = vk_code;
        ins[1].ki.wScan = ins[0].ki.wScan;
        ins[1].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(2, ins, sizeof(INPUT));
        return;
    }

    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk_code;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk_code, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if ((vk_code >= VK_PRIOR && vk_code <= VK_DOWN) ||
        vk_code == VK_INSERT || vk_code == VK_DELETE ||
        vk_code == VK_LWIN   || vk_code == VK_RWIN   ||
        vk_code == VK_RMENU  || vk_code == VK_RCONTROL) {
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &in, sizeof(INPUT));
}

void input_inject_unicode(uint16_t utf16_char) {
    if (utf16_char == 0) return;
    ensure_thread_desktop();
    INPUT ins[2];
    memset(ins, 0, sizeof(ins));

    ins[0].type = INPUT_KEYBOARD;
    ins[0].ki.wVk = 0;
    ins[0].ki.wScan = utf16_char;
    ins[0].ki.dwFlags = KEYEVENTF_UNICODE;

    ins[1].type = INPUT_KEYBOARD;
    ins[1].ki.wVk = 0;
    ins[1].ki.wScan = utf16_char;
    ins[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SendInput(2, ins, sizeof(INPUT));
}

/* ============================================================================
 * Bidirectional Clipboard Synchronization (UTF-8 <-> Windows CF_UNICODETEXT)
 * ============================================================================ */

static DWORD g_last_clip_seq = 0;

int clipboard_set_utf8(const char *utf8_text, size_t utf8_len, int trigger_paste) {
    if (!utf8_text || utf8_len == 0) return -1;
    ensure_thread_desktop();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text, (int)utf8_len, NULL, 0);
    if (wlen <= 0) return -1;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(WCHAR));
    if (!hMem) return -1;

    WCHAR *wbuf = (WCHAR*)GlobalLock(hMem);
    if (!wbuf) {
        GlobalFree(hMem);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8_text, (int)utf8_len, wbuf, wlen);
    wbuf[wlen] = L'\0';
    GlobalUnlock(hMem);

    int ok = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, hMem) != NULL) {
                ok = 1;
            }
            CloseClipboard();
            break;
        }
        Sleep(2);
    }

    if (!ok) {
        GlobalFree(hMem);
        return -1;
    }

    g_last_clip_seq = GetClipboardSequenceNumber();

    if (trigger_paste) {
        INPUT ins[4];
        memset(ins, 0, sizeof(ins));
        ins[0].type = INPUT_KEYBOARD;
        ins[0].ki.wVk = VK_CONTROL;
        ins[0].ki.wScan = (WORD)MapVirtualKeyA(VK_CONTROL, MAPVK_VK_TO_VSC);

        ins[1].type = INPUT_KEYBOARD;
        ins[1].ki.wVk = 'V';
        ins[1].ki.wScan = (WORD)MapVirtualKeyA('V', MAPVK_VK_TO_VSC);

        ins[2].type = INPUT_KEYBOARD;
        ins[2].ki.wVk = 'V';
        ins[2].ki.wScan = (WORD)MapVirtualKeyA('V', MAPVK_VK_TO_VSC);
        ins[2].ki.dwFlags = KEYEVENTF_KEYUP;

        ins[3].type = INPUT_KEYBOARD;
        ins[3].ki.wVk = VK_CONTROL;
        ins[3].ki.wScan = (WORD)MapVirtualKeyA(VK_CONTROL, MAPVK_VK_TO_VSC);
        ins[3].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(4, ins, sizeof(INPUT));
    }

    return 0;
}

char* clipboard_poll_utf8_change(size_t *out_len) {
    if (out_len) *out_len = 0;
    DWORD seq = GetClipboardSequenceNumber();
    if (g_last_clip_seq == 0) {
        g_last_clip_seq = seq;
        return NULL;
    }
    if (seq == g_last_clip_seq) {
        return NULL;
    }
    g_last_clip_seq = seq;

    ensure_thread_desktop();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return NULL;
    }
    if (!OpenClipboard(NULL)) {
        return NULL;
    }

    char *utf8_out = NULL;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const WCHAR *wstr = (const WCHAR*)GlobalLock(hData);
        if (wstr) {
            int wlen = (int)wcslen(wstr);
            if (wlen > 0 && wlen <= 1024 * 1024) {
                int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, NULL, 0, NULL, NULL);
                if (ulen > 0) {
                    utf8_out = (char*)malloc((size_t)ulen + 1);
                    if (utf8_out) {
                        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, utf8_out, ulen, NULL, NULL);
                        utf8_out[ulen] = '\0';
                        if (out_len) *out_len = (size_t)ulen;
                    }
                }
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return utf8_out;
}

/* ============================================================================
 * Native Windows WASAPI Loopback System Audio Capture
 * ============================================================================ */

static const GUID PHF_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID PHF_IID_IMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID PHF_IID_IAudioClient         = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID PHF_IID_IAudioCaptureClient  = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

static IMMDeviceEnumerator *g_aud_enum = NULL;
static IMMDevice           *g_aud_dev  = NULL;
static IAudioClient        *g_aud_client = NULL;
static IAudioCaptureClient *g_aud_capture = NULL;
static uint32_t             g_aud_rate = 48000;
static uint16_t             g_aud_channels = 2;
static int                  g_aud_is_float = 1;
static int                  g_aud_bps = 32;

int audio_loopback_init(uint32_t *out_sample_rate, uint16_t *out_channels) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    hr = CoCreateInstance(
        &PHF_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &PHF_IID_IMMDeviceEnumerator, (void**)&g_aud_enum
    );
    if (FAILED(hr) || !g_aud_enum) return -1;

    hr = g_aud_enum->lpVtbl->GetDefaultAudioEndpoint(g_aud_enum, eRender, eConsole, &g_aud_dev);
    if (FAILED(hr) || !g_aud_dev) {
        audio_loopback_close();
        return -2;
    }

    hr = g_aud_dev->lpVtbl->Activate(g_aud_dev, &PHF_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&g_aud_client);
    if (FAILED(hr) || !g_aud_client) {
        audio_loopback_close();
        return -3;
    }

    WAVEFORMATEX *pwfx = NULL;
    hr = g_aud_client->lpVtbl->GetMixFormat(g_aud_client, &pwfx);
    if (FAILED(hr) || !pwfx) {
        audio_loopback_close();
        return -4;
    }

    g_aud_rate = pwfx->nSamplesPerSec;
    g_aud_channels = pwfx->nChannels;
    g_aud_bps = pwfx->wBitsPerSample;
    g_aud_is_float = (pwfx->wFormatTag == 3 /* WAVE_FORMAT_IEEE_FLOAT */ || pwfx->wFormatTag == 0xFFFE /* WAVE_FORMAT_EXTENSIBLE */) ? 1 : 0;

    /* 100ms ring buffer duration (1,000,000 * 100ns) to prevent WASAPI underruns */
    hr = g_aud_client->lpVtbl->Initialize(
        g_aud_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        1000000,
        0,
        pwfx,
        NULL
    );
    CoTaskMemFree(pwfx);

    if (FAILED(hr)) {
        audio_loopback_close();
        return -5;
    }

    hr = g_aud_client->lpVtbl->GetService(g_aud_client, &PHF_IID_IAudioCaptureClient, (void**)&g_aud_capture);
    if (FAILED(hr) || !g_aud_capture) {
        audio_loopback_close();
        return -6;
    }

    hr = g_aud_client->lpVtbl->Start(g_aud_client);
    if (FAILED(hr)) {
        audio_loopback_close();
        return -7;
    }

    /* Report 24000 Hz downsampled rate to clients */
    if (out_sample_rate) *out_sample_rate = g_aud_rate / 2;
    if (out_channels) *out_channels = 2; /* Always output stereo int16 */
    printf("[Audio] WASAPI System Loopback active (%u Hz in -> %u Hz out, %u ch)\n",
           g_aud_rate, g_aud_rate / 2, g_aud_channels);
    return 0;
}

int audio_loopback_read(int16_t *out_pcm, int max_frames, uint32_t *out_sample_rate, uint16_t *out_channels) {
    if (!g_aud_capture || !out_pcm || max_frames <= 0) return 0;

    if (out_sample_rate) *out_sample_rate = g_aud_rate / 2;
    if (out_channels) *out_channels = 2;

    UINT32 packet_len = 0;
    HRESULT hr_pkt = g_aud_capture->lpVtbl->GetNextPacketSize(g_aud_capture, &packet_len);
    if (FAILED(hr_pkt)) {
        /* Device might have been invalidated (e.g. HDMI monitor turned off) */
        audio_loopback_close();
        audio_loopback_init(NULL, NULL);
        return 0;
    }
    if (packet_len == 0) {
        return 0;
    }

    int total_out_frames = 0;
    int non_silent = 0;

    while (packet_len > 0) {
        BYTE *data = NULL;
        UINT32 num_frames = 0;
        DWORD flags = 0;

        HRESULT hr = g_aud_capture->lpVtbl->GetBuffer(g_aud_capture, &data, &num_frames, &flags, NULL, NULL);
        if (FAILED(hr)) {
            audio_loopback_close();
            audio_loopback_init(NULL, NULL);
            break;
        }

        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && num_frames > 1) {
            int ch = (int)g_aud_channels;
            /* Downsample 2:1 from g_aud_rate (e.g. 48kHz) to g_aud_rate/2 (24kHz) */
            UINT32 pairs = num_frames / 2;
            if (total_out_frames + (int)pairs <= max_frames) {
                if (g_aud_is_float && g_aud_bps == 32) {
                    const float *fsrc = (const float*)data;
                    for (UINT32 i = 0; i < pairs; i++) {
                        UINT32 i0 = (i * 2) * ch;
                        UINT32 i1 = (i * 2 + 1) * ch;

                        float l = (fsrc[i0] + fsrc[i1]) * 0.5f;
                        float r = (ch > 1) ? ((fsrc[i0 + 1] + fsrc[i1 + 1]) * 0.5f) : l;

                        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;

                        int16_t il = (int16_t)(l * 32767.0f);
                        int16_t ir = (int16_t)(r * 32767.0f);

                        if (il != 0 || ir != 0) non_silent = 1;
                        out_pcm[(total_out_frames + i) * 2]     = il;
                        out_pcm[(total_out_frames + i) * 2 + 1] = ir;
                    }
                } else if (g_aud_bps == 16) {
                    const int16_t *isrc = (const int16_t*)data;
                    for (UINT32 i = 0; i < pairs; i++) {
                        UINT32 i0 = (i * 2) * ch;
                        UINT32 i1 = (i * 2 + 1) * ch;

                        int32_t l = ((int32_t)isrc[i0] + (int32_t)isrc[i1]) / 2;
                        int32_t r = (ch > 1) ? (((int32_t)isrc[i0 + 1] + (int32_t)isrc[i1 + 1]) / 2) : l;

                        int16_t il = (int16_t)l;
                        int16_t ir = (int16_t)r;

                        if (il != 0 || ir != 0) non_silent = 1;
                        out_pcm[(total_out_frames + i) * 2]     = il;
                        out_pcm[(total_out_frames + i) * 2 + 1] = ir;
                    }
                }
                total_out_frames += (int)pairs;
            }
        }

        g_aud_capture->lpVtbl->ReleaseBuffer(g_aud_capture, num_frames);
        if (FAILED(g_aud_capture->lpVtbl->GetNextPacketSize(g_aud_capture, &packet_len))) {
            audio_loopback_close();
            audio_loopback_init(NULL, NULL);
            break;
        }
    }

    return non_silent ? total_out_frames : 0;
}

void audio_loopback_close(void) {
    if (g_aud_client) {
        g_aud_client->lpVtbl->Stop(g_aud_client);
    }
    if (g_aud_capture) {
        g_aud_capture->lpVtbl->Release(g_aud_capture);
        g_aud_capture = NULL;
    }
    if (g_aud_client) {
        g_aud_client->lpVtbl->Release(g_aud_client);
        g_aud_client = NULL;
    }
    if (g_aud_dev) {
        g_aud_dev->lpVtbl->Release(g_aud_dev);
        g_aud_dev = NULL;
    }
    if (g_aud_enum) {
        g_aud_enum->lpVtbl->Release(g_aud_enum);
        g_aud_enum = NULL;
    }
}

#endif /* _WIN32 */
