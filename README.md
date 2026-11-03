# PHF-RDP — Ultra-Low Latency 60 FPS Lossless Remote Desktop

Real-time remote desktop (RDP) and screen streaming engine powered by the **Pure High-Fidelity Format (PHF)**, DirectX 11 DXGI Desktop Duplication, WebAssembly SIMD128, and native WASAPI system audio loopback.

Unlike traditional lossy remote desktop software (RDP with lossy codecs, VNC, AnyDesk, TeamViewer) that introduce 4:2:0 chroma blur, banding, and compression artifacts, **PHF-RDP streams every desktop frame in 100% mathematically lossless RGB quality** at 60 FPS with sub-millisecond compression latency.

---

## ⚡ Features (v1.3)

1. **100% Lossless 60 FPS Desktop Streaming**:
   - Full 4:4:4 RGB color fidelity: crystal-clear subpixel text rendering, crisp UI lines, and perfect color accuracy.
   - Ultra-low latency single-pass compression (`~0.2–1.2 ms` for delta frames, `~2.2 ms` for 720p, `~7.9 ms` for 1080p).
2. **Delta Sub-Frames (`Dirty Sub-Frame` — Opcode `0x06`)**:
   - Fast vector dirty-rect detection (`find_dirty_rect_rgb` in `~0.18 ms`).
   - Encodes and transmits only the modified bounding rectangle `(rx, ry, rw, rh)` and performs partial Canvas uploads (`putImageData`), cutting bandwidth and decoding load by up to 90% during standard desktop workflows.
   - **Zero-Flicker DXGI Idle Engine**: Automatically sleeps at 0% CPU on static screens without redundant frame transmission; gracefully falls back to GDI upon physical monitor disconnects or KVM switches.
3. **Smart Geometry & Resolution Scaling (`Auto`, `720p`, `1080p`, `Native`)**:
   - **Zero Server-Side Upscaling**: The server never compresses above the native display resolution.
   - **Windowed Mode**: High-speed `16.16` fixed-point bilinear downscaler with OpenMP when client view is smaller than the host monitor.
   - **Fullscreen / Larger Monitors**: Browser GPU handles display upscaling with aspect ratio preservation at zero CPU overhead.
4. **Low-Latency Remote Control (`TCP_NODELAY` + `SendInput`)**:
   - **Zero-Offset Mouse**: Precise coordinate mapping with automatic software cursor suppression (`capture_set_draw_cursor(0)`) so the client only sees their native hardware cursor.
   - **Layout-Independent Keyboard**: Printable characters and dead-key accents (`~`, `´`, `^`, `` ` ``, `¨`, `ç`) are composed on the client and injected via `KEYEVENTF_UNICODE`, working independently of host keyboard layout.
   - **Clean Modifier Separation**: Standard Windows `Ctrl` shortcuts (`Ctrl+C`, `Ctrl+V`, `Ctrl+X`, `Ctrl+A`, `Ctrl+Z`, `Ctrl+S`) map to physical Control with hardware scan code `0x1D`.
5. **Bidirectional Clipboard Synchronization (Opcode `0x05`)**:
   - Seamless shared clipboard between client (macOS, Linux, Windows) and host PC.
   - Works over standard HTTP LAN connections via native DOM paste event passthrough.
   - HUD clipboard viewer with one-click **"⬆ Send to PC"** and **"⬇ Copy to Mac/Client"** actions.
6. **Multi-Monitor & Virtual Display (IddCx) Support**:
   - Automatically detects and enumerates physical monitors and virtual display drivers (e.g. Parsec VDD, MTT VDD). Switch displays on-the-fly directly from the HUD.
7. **Native WASAPI Loopback System Audio (Opcode `0x02`)**:
   - Captures system mix in C via `IAudioCaptureClient` (`AUDCLNT_STREAMFLAGS_LOOPBACK`), streams 16-bit PCM stereo, and suppresses silent buffers (`0 kbps` during silence).
8. **Windows System Tray Icon & Draggable HUD**:
   - Dedicated background thread with Windows notification badge, status display, client list, pause/resume, and clean shutdown.
   - Minimalist half-circle HUD edge tab docked to the right edge of the screen, vertically draggable with position persistence (`localStorage`).

---

## 🏗️ Architecture

```text
phf-rdp/
├── server/
│   ├── include/
│   │   ├── capture.h         # Cross-platform screen capture & input interface
│   │   ├── phf.h             # Core libphf header
│   │   ├── tray_win.h        # Windows System Tray interface
│   │   └── mongoose.h        # Single-header HTTP & WebSocket server
│   ├── src/
│   │   ├── main.c            # Server loop, delta detector, and WebSocket hub
│   │   ├── capture_win_dxgi.c# DirectX 11 DXGI Desktop Duplication, SendInput & WASAPI
│   │   ├── capture_mac.m     # ScreenCaptureKit, CGEvent & NSPasteboard (macOS)
│   │   ├── capture_linux_x11.c# MIT-SHM, XTest & XFixes native capture (Linux)
│   │   ├── capture_stub.c    # Synthetic test pattern fallback
│   │   ├── tray_win.c        # Dedicated Win32 tray thread with Unicode support
│   │   ├── phf.c             # Pure High-Fidelity Format encoder/decoder
│   │   ├── huff.c            # N-Huffman entropy codec
│   │   └── mongoose.c        # Mongoose networking engine
│   └── build.bat             # MSVC compilation script
├── web/
│   ├── index.html            # Minimalist viewport & floating glassmorphic HUD
│   ├── style.css             # Dark glassmorphism styling
│   ├── client.js             # WebSocket client, WASM SIMD decoder, and input bridge
│   ├── hud.js                # HUD controller (metrics, draggable tab, monitor selector)
│   ├── phf.js                # Emscripten WebAssembly runtime
│   └── phf.wasm              # SIMD128 accelerated PHF decoder
├── run.bat                   # Single-command launcher
└── README.md
```

---

## 🚀 How to Build and Run

### 1. Build Server (Windows MSVC)
Open Developer Command Prompt (or run from standard shell with Visual Studio installed):
```cmd
cd server
build.bat
```

### 1b. Build and Run Server (macOS 13+)
Native backend `src/capture_mac.m`: ScreenCaptureKit for the screen (scaled on the GPU to the
stream size) and system audio, `CGEventPost` for mouse and keyboard, `NSPasteboard` for the
clipboard.
```bash
cd server
cmake -S . -B build -DOpenMP_ROOT=$(brew --prefix libomp)   # OpenMP flag optional
cmake --build build
cd .. && ./server/phf-rdp-server [width height [fps]]       # run from the repo root
```
Permissions are granted to the app that launches the server (Terminal, iTerm, VS Code...), in
System Settings > Privacy & Security; restart that app after granting them:
- **Screen & System Audio Recording**: required (no frames without it).
- **Accessibility**: required for remote mouse and keyboard.

Keys: the client sends Windows virtual-key codes; on macOS, Meta/Win maps to Command, Ctrl to
Control and Alt to Option, so a Mac client keeps its native shortcuts.

Measured on an Apple M3 Pro with a full-screen animation (every frame is a keyframe, the worst
case; ordinary desktop use sends small delta frames):

| Display | Stream | FPS | Encode p50 | Bandwidth |
| :--- | :--- | ---: | ---: | ---: |
| External 2560x1440 @ 60 Hz | 1920x1080 | 60.0 (display-bound) | 12.9 ms | ~560 Mbps |
| External 2560x1440 @ 60 Hz | 2560x1440 | 47.6 | 19.2 ms | ~870 Mbps |
| Built-in Retina @ 120 Hz | 1672x1080 | 74.4 | 10.9 ms | ~530 Mbps |
| Built-in Retina @ 120 Hz | 3456x2234 | 27.0 | 33.6 ms | ~900 Mbps |

The default cap is 60 FPS; pass e.g. `1280 720 120` to allow 120.

### 1c. Build and Run Server (Linux X11)
Native backend `src/capture_linux_x11.c`: MIT-SHM (`XShmGetImage`) for zero-copy 60 FPS screen capture,
`XTest` extension for remote mouse and keyboard event injection, and `XFixes` for software cursor overlay.
```bash
# Ubuntu / Debian / Raspberry Pi OS dependencies:
sudo apt-get install -y cmake build-essential libx11-dev libxext-dev libxtst-dev libxfixes-dev libxrandr-dev

cd server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd .. && ./server/phf-rdp-server [width height [fps]]
```
To run with the synthetic test pattern stub instead of native capture, pass `-DPHF_RDP_STUB=ON` to CMake.

### 2. Launch Server
```cmd
run.bat
```
The server will start listening on:
- **HTTP Dashboard:** `http://localhost:8080` (or `http://<your-lan-ip>:8080`)
- **WebSocket Video/Input:** `ws://localhost:8765` (or `ws://<your-lan-ip>:8765`)

### 3. Open in Any Modern Browser
Navigate to `http://localhost:8080` on your client machine (Chrome, Safari, Edge, Firefox):
- **Alt + H** (or **H**): Show / Hide floating HUD
- **F** (or **F11**): Toggle Fullscreen
- Click the half-circle tab on the right edge of the screen to open the control panel. Drag the tab vertically to reposition it.

---

## 📄 License

MIT License. Part of the [PHF (libphf)](https://github.com/libphf/phf) ecosystem.
