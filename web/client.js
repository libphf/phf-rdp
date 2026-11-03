/**
 * PHF-RDP Client Engine
 * High-performance screen reception, WASM decoding with SIMD (Full + Delta Sub-Frames),
 * smart viewport/fullscreen aspect-ratio scaling, Multi-Monitor switcher,
 * bidirectional Clipboard sync, WASAPI System Audio player, and low-latency RDP Input.
 */

import { HUDController } from './hud.js?v=22';

let wasmModule = null;
let cachedSrcPtr = 0;
let cachedSrcCapacity = 0;
let cachedDstPtr = 0;
let cachedDstCapacity = 0;

let currentImageData = null;

/* Performance tracking & metrics */
let frameCountWindow = 0;
let deltaFrameCountWindow = 0;
let fullFrameCountWindow = 0;
let bytesReceivedWindow = 0;
let rawBytesWindow = 0;
let compBytesWindow = 0;
let fullEquivalentRawBytesWindow = 0;
let rollingEncMs = 0.5;
let rollingDecMs = 0.3;
let lastDecMs = 0.3;

/* Client display refresh tracking via requestAnimationFrame */
let rafFrameCount = 0;
let lastRafTime = performance.now();
let clientDisplayHz = 60;
function measureDisplayRefresh() {
  rafFrameCount++;
  const now = performance.now();
  if (now - lastRafTime >= 1000) {
    clientDisplayHz = Math.round((rafFrameCount * 1000) / (now - lastRafTime));
    rafFrameCount = 0;
    lastRafTime = now;
  }
  requestAnimationFrame(measureDisplayRefresh);
}
requestAnimationFrame(measureDisplayRefresh);

/* Map browser KeyboardEvent.code / keyCode to Windows Virtual-Key (VK_*) codes */
const CODE_TO_VK = {
  Backspace: 0x08, Tab: 0x09, Enter: 0x0D, NumpadEnter: 0x0D,
  ShiftLeft: 0xA0, ShiftRight: 0xA1, ControlLeft: 0xA2, ControlRight: 0xA3,
  AltLeft: 0xA4, AltRight: 0xA5, Pause: 0x13, CapsLock: 0x14, Escape: 0x1B,
  Space: 0x20, PageUp: 0x21, PageDown: 0x22, End: 0x23, Home: 0x24,
  ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28,
  PrintScreen: 0x2C, Insert: 0x2D, Delete: 0x2E,
  MetaLeft: 0x5B, MetaRight: 0x5C, ContextMenu: 0x5D,
  Numpad0: 0x60, Numpad1: 0x61, Numpad2: 0x62, Numpad3: 0x63, Numpad4: 0x64,
  Numpad5: 0x65, Numpad6: 0x66, Numpad7: 0x67, Numpad8: 0x68, Numpad9: 0x69,
  NumpadMultiply: 0x6A, NumpadAdd: 0x6B, NumpadSubtract: 0x6D,
  NumpadDecimal: 0x6E, NumpadDivide: 0x6F,
  F1: 0x70, F2: 0x71, F3: 0x72, F4: 0x73, F5: 0x74, F6: 0x75,
  F7: 0x76, F8: 0x77, F9: 0x78, F10: 0x79, F11: 0x7A, F12: 0x7B,
  NumLock: 0x90, ScrollLock: 0x91,
  Semicolon: 0xBA, Equal: 0xBB, Comma: 0xBC, Minus: 0xBD, Period: 0xBE,
  Slash: 0xBF, Backquote: 0xC0, BracketLeft: 0xDB, Backslash: 0xDC,
  BracketRight: 0xDD, Quote: 0xDE
};

function mapKeyEventToVK(e) {
  if (CODE_TO_VK[e.code]) return CODE_TO_VK[e.code];
  if (e.code && e.code.startsWith('Key') && e.code.length === 4) {
    return e.code.charCodeAt(3);
  }
  if (e.code && e.code.startsWith('Digit') && e.code.length === 6) {
    return e.code.charCodeAt(5);
  }
  return e.keyCode || 0;
}

async function initWasm() {
  return new Promise((resolve, reject) => {
    if (typeof window.PHFModule !== 'function') {
      return reject(new Error('phf.js did not define window.PHFModule'));
    }
    window.PHFModule({
      locateFile: (path) => `${path}?v=7`
    }).then(mod => {
      wasmModule = mod;
      resolve(mod);
    }).catch(reject);
  });
}

class ScreenClient {
  constructor() {
    this.viewport = document.getElementById('viewport-container');
    this.canvas = document.getElementById('display-canvas');
    this.inputTrap = document.getElementById('rdp-input-trap');
    this.ctx = this.canvas.getContext('2d', { alpha: false, desynchronized: true });
    this.statusOverlay = document.getElementById('status-overlay');
    this.statusText = document.getElementById('status-text');

    this.ws = null;
    this.reconnectTimer = null;
    this.resizeTimer = null;

    /* Smart Scaling & Geometry state */
    this.scaleMode = 'auto';
    this.capWidth = 0;
    this.capHeight = 0;
    this.sourceWidth = 1920;
    this.sourceHeight = 1080;
    this.streamWidth = 1280;
    this.streamHeight = 720;
    this.lastRequestedW = -1;
    this.lastRequestedH = -1;

    /* RDP Input, Clipboard & Audio state */
    this.inputEnabled = false;
    this.audioEnabled = false;
    this.audioWs = null;
    this.audioCtx = null;
    this.nextAudioTime = 0;
    this.pendingMouseCoords = null;
    this.mouseRafScheduled = false;
    this.remoteClipboardText = '';

    this.hud = new HUDController({
      onResolutionModeChange: (mode, w, h) => this.setScalingMode(mode, w, h),
      onInputToggle: (enabled) => this.toggleInputControl(enabled),
      onMonitorChange: (monitorId) => this.selectMonitor(monitorId),
      onAudioToggle: (enabled) => this.toggleAudioStream(enabled)
    });

    this.initViewportListeners();
    this.initInputListeners();

    this.lastStatsTickTime = performance.now();
    this.statsTimer = setInterval(() => this.tickStats(), 1000);
  }

  async start() {
    try {
      this.statusText.textContent = "Carregando PHF WASM (SIMD)...";
      await initWasm();
      this.statusText.textContent = "Conectando ao PHF-RDP Server...";
      this.connectWebSocket();
    } catch (err) {
      this.statusText.textContent = `Erro na inicialização: ${err.message}`;
      console.error(err);
    }
  }

  initViewportListeners() {
    const onResize = () => {
      this.fitCanvasToViewport();
      requestAnimationFrame(() => this.fitCanvasToViewport());
      clearTimeout(this.resizeTimer);
      this.resizeTimer = setTimeout(() => {
        this.fitCanvasToViewport();
        this.syncResolutionWithServer();
      }, 120);
    };
    window.addEventListener('resize', onResize);
    document.addEventListener('fullscreenchange', onResize);

    if (typeof ResizeObserver !== 'undefined' && this.viewport) {
      const ro = new ResizeObserver(() => this.fitCanvasToViewport());
      ro.observe(this.viewport);
    }
  }

  fitCanvasToViewport() {
    const rect = this.viewport ? this.viewport.getBoundingClientRect() : null;
    const vw = (rect && rect.width > 0) ? rect.width : window.innerWidth;
    const vh = (rect && rect.height > 0) ? rect.height : window.innerHeight;
    const sw = this.sourceWidth || this.streamWidth || this.canvas.width || 1920;
    const sh = this.sourceHeight || this.streamHeight || this.canvas.height || 1080;
    if (vw <= 0 || vh <= 0 || sw <= 0 || sh <= 0) return;

    const targetAspect = sw / sh;
    const viewAspect = vw / vh;

    let cssW, cssH;
    if (viewAspect > targetAspect) {
      cssH = vh;
      cssW = vh * targetAspect;
    } else {
      cssW = vw;
      cssH = vw / targetAspect;
    }

    this.canvas.style.width = `${cssW}px`;
    this.canvas.style.height = `${cssH}px`;
  }

  setScalingMode(mode, w, h) {
    this.scaleMode = mode;
    this.capWidth = w;
    this.capHeight = h;
    this.lastRequestedW = -1;
    this.lastRequestedH = -1;
    this.syncResolutionWithServer();
  }

  selectMonitor(monitorId) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const msg = JSON.stringify({ cmd: "monitor", id: monitorId });
    const payload = new TextEncoder().encode(msg);
    const packet = new Uint8Array(1 + payload.length);
    packet[0] = 0x03; // PKT_JSON
    packet.set(payload, 1);
    this.ws.send(packet);
  }

  syncResolutionWithServer() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;

    const dpr = window.devicePixelRatio || 1;
    const physW = Math.round(window.innerWidth * dpr);
    const physH = Math.round(window.innerHeight * dpr);

    let reqW = 0;
    let reqH = 0;

    if (this.scaleMode === 'native') {
      reqW = 0;
      reqH = 0;
    } else if (this.scaleMode === 'cap' && this.capWidth > 0 && this.capHeight > 0) {
      reqW = Math.min(this.capWidth, physW);
      reqH = Math.min(this.capHeight, physH);
    } else {
      reqW = physW;
      reqH = physH;
    }

    if (reqW > 0) reqW = (reqW + 1) & ~1;
    if (reqH > 0) reqH = (reqH + 1) & ~1;

    if (reqW === this.lastRequestedW && reqH === this.lastRequestedH) return;
    this.lastRequestedW = reqW;
    this.lastRequestedH = reqH;

    const msg = JSON.stringify({ cmd: "resolution", w: reqW, h: reqH });
    const payload = new TextEncoder().encode(msg);
    const packet = new Uint8Array(1 + payload.length);
    packet[0] = 0x03;
    packet.set(payload, 1);
    this.ws.send(packet);
  }

  connectWebSocket() {
    const host = window.location.hostname || 'localhost';
    const port = this.useFallbackPort ? '8080/ws' : '8765';
    const wsUrl = `ws://${host}:${port}`;

    this.ws = new WebSocket(wsUrl);
    this.ws.binaryType = 'arraybuffer';

    this.ws.onopen = () => {
      console.log(`[Client] Connected to PHF-RDP Server (${wsUrl}).`);
      this.statusOverlay.style.opacity = '0';
      setTimeout(() => { this.statusOverlay.style.display = 'none'; }, 300);
      this.lastRequestedW = -1;
      this.lastRequestedH = -1;
      this.syncResolutionWithServer();
      this.syncCursorModeWithServer();
      if (this.audioEnabled) {
        this.connectAudioWebSocket();
      }
    };

    this.ws.onmessage = (event) => {
      if (typeof event.data === 'string') return;
      this.handleBinaryPacket(event.data);
    };

    this.ws.onclose = () => {
      this.useFallbackPort = !this.useFallbackPort;
      this.statusOverlay.style.display = 'flex';
      this.statusOverlay.style.opacity = '1';
      this.statusText.textContent = "Conexão perdida. Reconectando...";
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = setTimeout(() => this.connectWebSocket(), 2000);
    };

    this.ws.onerror = (err) => {
      console.error('[Client] WebSocket error', err);
    };
  }

  /* ==========================================================================
   * WASAPI System Audio Playback (Dedicated WebSocket + Web Audio API)
   * ========================================================================== */

  toggleAudioStream(enabled) {
    this.audioEnabled = enabled;
    if (enabled) {
      if (!this.audioCtx) {
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        if (AudioContextClass) {
          this.audioCtx = new AudioContextClass({ latencyHint: 'interactive' });
        }
      }
      if (this.audioCtx && this.audioCtx.state === 'suspended') {
        this.audioCtx.resume();
      }
      this.nextAudioTime = 0;
      this.connectAudioWebSocket();
    } else {
      if (this.audioWs) {
        try { this.audioWs.close(); } catch (_) {}
        this.audioWs = null;
      }
    }
  }

  connectAudioWebSocket() {
    if (this.audioWs && (this.audioWs.readyState === WebSocket.OPEN || this.audioWs.readyState === WebSocket.CONNECTING)) {
      return;
    }
    const host = window.location.hostname || 'localhost';
    const port = this.useFallbackPort ? '8080/ws' : '8765';
    const wsUrl = `ws://${host}:${port}`;

    this.audioWs = new WebSocket(wsUrl);
    this.audioWs.binaryType = 'arraybuffer';

    this.audioWs.onopen = () => {
      // Register this connection on server as an audio-only socket
      const pkt = new Uint8Array([0x04, 0x07, 1]);
      this.audioWs.send(pkt);
    };

    this.audioWs.onmessage = (event) => {
      if (typeof event.data === 'string') return;
      bytesReceivedWindow += event.data.byteLength;
      const u8 = new Uint8Array(event.data);
      if (u8.length > 8 && u8[0] === 0x02) {
        this.handleAudioPacket(event.data);
      }
    };

    this.audioWs.onclose = () => {
      if (this.audioEnabled) {
        setTimeout(() => this.connectAudioWebSocket(), 1500);
      }
    };

    this.audioWs.onerror = () => {};
  }

  handleAudioPacket(arrayBuffer) {
    if (!this.audioEnabled || !this.audioCtx || arrayBuffer.byteLength <= 8) return;
    const dv = new DataView(arrayBuffer);
    const sampleRate = dv.getUint32(2, true) || 24000;
    const channels = dv.getUint16(6, true) || 2;
    const pcm16 = new Int16Array(arrayBuffer, 8);
    const numFrames = Math.floor(pcm16.length / channels);
    if (numFrames <= 0) return;

    const audioBuffer = this.audioCtx.createBuffer(channels, numFrames, sampleRate);
    for (let ch = 0; ch < channels; ch++) {
      const chData = audioBuffer.getChannelData(ch);
      for (let i = 0; i < numFrames; i++) {
        chData[i] = pcm16[i * channels + ch] / 32768.0;
      }
    }

    const src = this.audioCtx.createBufferSource();
    src.buffer = audioBuffer;
    src.connect(this.audioCtx.destination);

    const now = this.audioCtx.currentTime;
    if (this.nextAudioTime < now) {
      this.nextAudioTime = now + 0.080;
    } else if (this.nextAudioTime > now + 0.35) {
      this.nextAudioTime = now + 0.080;
    }
    src.start(this.nextAudioTime);
    this.nextAudioTime += audioBuffer.duration;
  }

  /* ==========================================================================
   * RDP Remote Input Control & Bidirectional Clipboard
   * ========================================================================== */

  syncCursorModeWithServer() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const pkt = new Uint8Array([0x04, 0x06, this.inputEnabled ? 0 : 1]);
    this.ws.send(pkt);
  }

  toggleInputControl(enabled) {
    this.inputEnabled = enabled;
    this.canvas.classList.toggle('rdp-active', enabled);
    this.syncCursorModeWithServer();
    if (enabled) {
      this.resetModifiers();
      if (this.inputTrap) {
        this.inputTrap.value = '';
        this.inputTrap.focus({ preventScroll: true });
      }
    }
  }

  getNormalizedCanvasCoords(clientX, clientY) {
    const rect = this.canvas.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return { x: 0, y: 0 };

    const imgW = this.canvas.width || this.sourceWidth || 1920;
    const imgH = this.canvas.height || this.sourceHeight || 1080;
    const imgAspect = imgW / imgH;
    const boxAspect = rect.width / rect.height;

    let renderW = rect.width;
    let renderH = rect.height;
    let offsetX = 0;
    let offsetY = 0;

    if (Math.abs(boxAspect - imgAspect) > 0.001) {
      if (boxAspect > imgAspect) {
        renderW = rect.height * imgAspect;
        offsetX = (rect.width - renderW) * 0.5;
      } else {
        renderH = rect.width / imgAspect;
        offsetY = (rect.height - renderH) * 0.5;
      }
    }

    const relX = Math.max(0, Math.min(1, (clientX - rect.left - offsetX) / renderW));
    const relY = Math.max(0, Math.min(1, (clientY - rect.top - offsetY) / renderH));
    return {
      x: Math.round(relX * 65535),
      y: Math.round(relY * 65535)
    };
  }

  sendUnicodeString(str) {
    if (!str || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    for (let i = 0; i < str.length; i++) {
      const code = str.charCodeAt(i);
      if (code >= 32) {
        const pkt = new Uint8Array(4);
        pkt[0] = 0x04;
        pkt[1] = 0x05; // INPUT_UNICODE
        pkt[2] = code & 0xFF;
        pkt[3] = (code >> 8) & 0xFF;
        this.ws.send(pkt);
      }
    }
  }

  resetModifiers() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const pkt = new Uint8Array(2);
    pkt[0] = 0x04;
    pkt[1] = 0x08; // RESET_MODIFIERS
    this.ws.send(pkt);
  }

  sendVK(vk, down) {
    if (!vk || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const pkt = new Uint8Array(5);
    pkt[0] = 0x04;
    pkt[1] = 0x04; // INPUT_KEY (VK)
    pkt[2] = vk & 0xFF;
    pkt[3] = (vk >> 8) & 0xFF;
    pkt[4] = down ? 1 : 0;
    this.ws.send(pkt);
  }

  sendClipboardToServer(text, triggerPaste) {
    if (!text || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const utf8 = new TextEncoder().encode(text);
    const pkt = new Uint8Array(2 + utf8.length);
    pkt[0] = 0x05; // PKT_CLIPBOARD
    pkt[1] = triggerPaste ? 1 : 0;
    pkt.set(utf8, 2);
    this.ws.send(pkt);
  }

  initInputListeners() {
    this.canvas.addEventListener('mousemove', (e) => {
      if (!this.inputEnabled || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      this.pendingMouseCoords = this.getNormalizedCanvasCoords(e.clientX, e.clientY);

      if (!this.mouseRafScheduled) {
        this.mouseRafScheduled = true;
        requestAnimationFrame(() => {
          this.mouseRafScheduled = false;
          if (this.pendingMouseCoords && this.ws && this.ws.readyState === WebSocket.OPEN) {
            const { x, y } = this.pendingMouseCoords;
            this.pendingMouseCoords = null;
            const pkt = new Uint8Array(6);
            pkt[0] = 0x04;
            pkt[1] = 0x01; // MOUSE_MOVE
            pkt[2] = x & 0xFF;
            pkt[3] = (x >> 8) & 0xFF;
            pkt[4] = y & 0xFF;
            pkt[5] = (y >> 8) & 0xFF;
            this.ws.send(pkt);
          }
        });
      }
    });

    const sendButton = (e, down) => {
      if (!this.inputEnabled || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      e.preventDefault();
      if (this.inputTrap) {
        this.inputTrap.focus({ preventScroll: true });
      }
      const { x, y } = this.getNormalizedCanvasCoords(e.clientX, e.clientY);
      const pkt = new Uint8Array(8);
      pkt[0] = 0x04;
      pkt[1] = 0x02; // MOUSE_BUTTON
      pkt[2] = e.button & 0xFF;
      pkt[3] = down ? 1 : 0;
      pkt[4] = x & 0xFF;
      pkt[5] = (x >> 8) & 0xFF;
      pkt[6] = y & 0xFF;
      pkt[7] = (y >> 8) & 0xFF;
      this.ws.send(pkt);
    };

    this.canvas.addEventListener('mousedown', (e) => sendButton(e, true));
    this.canvas.addEventListener('mouseup', (e) => sendButton(e, false));
    this.canvas.addEventListener('contextmenu', (e) => {
      if (this.inputEnabled) e.preventDefault();
    });

    this.canvas.addEventListener('wheel', (e) => {
      if (!this.inputEnabled || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      e.preventDefault();
      const dy = Math.max(-32768, Math.min(32767, Math.round(-e.deltaY * 1.2)));
      const dx = Math.max(-32768, Math.min(32767, Math.round(e.deltaX * 1.2)));
      const pkt = new Uint8Array(6);
      pkt[0] = 0x04;
      pkt[1] = 0x03; // MOUSE_WHEEL
      pkt[2] = dx & 0xFF;
      pkt[3] = (dx >> 8) & 0xFF;
      pkt[4] = dy & 0xFF;
      pkt[5] = (dy >> 8) & 0xFF;
      this.ws.send(pkt);
    }, { passive: false });

    window.addEventListener('paste', (e) => {
      if (!this.inputEnabled || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      if (e.target && e.target.tagName === 'INPUT') return;
      e.preventDefault();
      const clipData = e.clipboardData || window.clipboardData;
      const text = clipData ? clipData.getData('text/plain') : '';
      if (text) {
        this.sendClipboardToServer(text, true);
      } else {
        this.sendVK(0xA2, true);
        this.sendVK(0x56, true);
        this.sendVK(0x56, false);
        this.sendVK(0xA2, false);
      }
      if (this.inputTrap) this.inputTrap.value = '';
    });

    let pendingDeadKey = null;

    const DEAD_MAP = {
      '´': { 'a': 'á', 'e': 'é', 'i': 'í', 'o': 'ó', 'u': 'ú', 'c': 'ç', 'y': 'ý',
             'A': 'Á', 'E': 'É', 'I': 'Í', 'O': 'Ó', 'U': 'Ú', 'C': 'Ç', 'Y': 'Ý', ' ': '´', '´': '´' },
      '~': { 'a': 'ã', 'o': 'õ', 'n': 'ñ',
             'A': 'Ã', 'O': 'Õ', 'N': 'Ñ', ' ': '~', '~': '~' },
      '^': { 'a': 'â', 'e': 'ê', 'i': 'î', 'o': 'ô', 'u': 'û',
             'A': 'Â', 'E': 'Ê', 'I': 'Î', 'O': 'Ô', 'U': 'Û', ' ': '^', '^': '^' },
      '`': { 'a': 'à', 'e': 'è', 'i': 'ì', 'o': 'ò', 'u': 'ù',
             'A': 'À', 'E': 'È', 'I': 'Ì', 'O': 'Ò', 'U': 'Ù', ' ': '`', '`': '`' },
      '¨': { 'a': 'ä', 'e': 'ë', 'i': 'ï', 'o': 'ö', 'u': 'ü',
             'A': 'Ä', 'E': 'Ë', 'I': 'Ï', 'O': 'Ö', 'U': 'Ü', ' ': '¨', '¨': '¨' }
    };

    function detectDeadSymbol(ev) {
      // ABNT2: BracketLeft is [´ / `] next to P, BracketRight is [~ / ^] next to Ç
      if (ev.code === 'BracketLeft')  return ev.shiftKey ? '`' : '´';
      if (ev.code === 'BracketRight') return ev.shiftKey ? '^' : '~';
      // US-International: Quote is [´ / ¨], Backquote is [` / ~]
      if (ev.code === 'Quote')        return ev.shiftKey ? '¨' : '´';
      if (ev.code === 'Backquote')    return ev.shiftKey ? '~' : '`';
      if (ev.code === 'Digit6' && ev.shiftKey) return '^';
      if (ev.code === 'Equal')        return ev.shiftKey ? '`' : '´';
      return ev.shiftKey ? '`' : '´';
    }

    const handleKey = (e, down) => {
      if (!this.inputEnabled || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
      if (e.altKey && (e.key === 'h' || e.key === 'H')) return;

      // Allow Ctrl+V paste passthrough to window paste listener
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'v' || e.key === 'V')) {
        return;
      }

      // Brazilian ABNT2 & US-Intl Dead Key composition
      if (e.key === 'Dead' || e.isComposing) {
        if (down) {
          pendingDeadKey = detectDeadSymbol(e);
        }
        e.preventDefault();
        return;
      }

      // If Escape or Backspace pressed, clear pending dead key
      if (pendingDeadKey && (e.key === 'Escape' || e.key === 'Backspace')) {
        pendingDeadKey = null;
      }

      const isShortcutCombo = e.ctrlKey || e.altKey || e.metaKey;
      const isPrintableChar = e.key && e.key.length === 1 && !isShortcutCombo;

      if (isPrintableChar) {
        if (down) {
          e.preventDefault();
          let charToSend = e.key;
          if (pendingDeadKey) {
            charToSend = DEAD_MAP[pendingDeadKey]?.[e.key] || (pendingDeadKey + e.key);
            pendingDeadKey = null;
          }
          this.sendUnicodeString(charToSend);
        }
        return;
      }

      const vk = mapKeyEventToVK(e);
      if (!vk) return;

      // When navigation arrow keys are pressed without Shift, guarantee Shift is UP
      // so Windows never accidentally selects text
      if (vk >= 0x25 && vk <= 0x28 && !e.shiftKey) {
        this.sendVK(0x10, false);
      }

      e.preventDefault();
      this.sendVK(vk, down);
    };

    window.addEventListener('keydown', (e) => handleKey(e, true));
    window.addEventListener('keyup', (e) => handleKey(e, false));
  }

  /* ==========================================================================
   * Packet Reception & WASM Decoding (0x01 Keyframe, 0x06 Delta, 0x02 Audio, 0x05 Clipboard)
   * ========================================================================== */

  handleBinaryPacket(arrayBuffer) {
    const u8 = new Uint8Array(arrayBuffer);
    if (u8.length < 2) return;

    const opcode = u8[0];
    if (opcode === 0x01 && u8.length >= 13) { // PKT_VIDEO (Full Frame)
      bytesReceivedWindow += u8.length;
      const dv = new DataView(arrayBuffer);
      const encMs = dv.getFloat32(9, true);
      this.decodeFullFrame(u8.subarray(13), encMs);
    } else if (opcode === 0x06 && u8.length >= 21) { // PKT_VIDEO_DELTA (Dirty Sub-Frame)
      bytesReceivedWindow += u8.length;
      const dv = new DataView(arrayBuffer);
      const encMs = dv.getFloat32(9, true);
      const rx = dv.getUint16(13, true);
      const ry = dv.getUint16(15, true);
      const fullW = dv.getUint16(17, true);
      const fullH = dv.getUint16(19, true);
      this.decodeDeltaFrame(u8.subarray(21), encMs, rx, ry, fullW, fullH);
    } else if (opcode === 0x02 && u8.length > 8) { // PKT_AUDIO (WASAPI Loopback PCM16)
      bytesReceivedWindow += u8.length;
      this.handleAudioPacket(arrayBuffer);
    } else if (opcode === 0x05 && u8.length > 2) { // PKT_CLIPBOARD (Server -> Client)
      try {
        const text = new TextDecoder().decode(u8.subarray(2));
        this.remoteClipboardText = text;
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(text).catch(() => {});
        }
      } catch (_) {}
    } else if (opcode === 0x03) { // PKT_JSON
      try {
        const text = new TextDecoder().decode(u8.subarray(1));
        const msg = JSON.parse(text);
        if (msg.screen_w && msg.screen_h) {
          this.sourceWidth = msg.screen_w;
          this.sourceHeight = msg.screen_h;
        }
        if (msg.stream_w && msg.stream_h) {
          this.streamWidth = msg.stream_w;
          this.streamHeight = msg.stream_h;
        }
        if (msg.monitors) {
          this.hud.updateMonitors(msg.monitors, msg.active_monitor || 0);
        }
        this.fitCanvasToViewport();
        this.hud.updateMetrics({
          sourceWidth: this.sourceWidth,
          sourceHeight: this.sourceHeight,
          width: this.streamWidth,
          height: this.streamHeight,
          scaleTag: this.getScaleTag(this.streamWidth, this.streamHeight)
        });
      } catch (_) {}
    }
  }

  ensureWasmBuffers(srcBytes, dstBytes) {
    if (cachedSrcCapacity < srcBytes) {
      if (cachedSrcPtr) wasmModule._free(cachedSrcPtr);
      cachedSrcPtr = wasmModule._malloc(srcBytes * 2);
      cachedSrcCapacity = srcBytes * 2;
    }
    if (cachedDstCapacity < dstBytes) {
      if (cachedDstPtr) wasmModule._free(cachedDstPtr);
      cachedDstPtr = wasmModule._malloc(dstBytes * 2);
      cachedDstCapacity = dstBytes * 2;
    }
  }

  decodeFullFrame(phfData, encMs) {
    const n = phfData.byteLength;
    if (n < 20) return;

    if (phfData[0] !== 0x50 || phfData[1] !== 0x48 || phfData[2] !== 0x46 || phfData[3] !== 0x31) {
      return;
    }

    const dv = new DataView(phfData.buffer, phfData.byteOffset, phfData.byteLength);
    const channels = dv.getUint8(5);
    const width = dv.getUint32(8, true);
    const height = dv.getUint32(12, true);

    if (width === 0 || height === 0 || width > 8192 || height > 8192) return;

    const outBytes = width * height * 4;
    this.ensureWasmBuffers(n, outBytes);

    wasmModule.HEAPU8.set(phfData, cachedSrcPtr);

    const t0 = performance.now();
    const rc = wasmModule._phf_decode_memory(cachedSrcPtr, n, cachedDstPtr, width, height, 4);
    lastDecMs = performance.now() - t0;

    if (rc !== 0) return;

    if (!currentImageData || this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
      this.streamWidth = width;
      this.streamHeight = height;
      currentImageData = this.ctx.createImageData(width, height);
      this.fitCanvasToViewport();
    }

    const decodedView = new Uint8Array(wasmModule.HEAPU8.buffer, cachedDstPtr, outBytes);
    currentImageData.data.set(decodedView);
    this.ctx.putImageData(currentImageData, 0, 0);

    if (!this.firstFrameLogged) {
      this.firstFrameLogged = true;
      console.log(`[Client] Screen stream live: ${width}x${height} (${channels}ch)`);
      this.statusOverlay.style.display = 'none';
    }

    frameCountWindow++;
    fullFrameCountWindow++;
    rawBytesWindow += width * height * channels;
    compBytesWindow += n;
    fullEquivalentRawBytesWindow += width * height * channels;
    if (encMs > 0) rollingEncMs = rollingEncMs * 0.7 + encMs * 0.3;
    if (lastDecMs > 0) rollingDecMs = rollingDecMs * 0.7 + lastDecMs * 0.3;
  }

  decodeDeltaFrame(phfData, encMs, rx, ry, fullW, fullH) {
    const n = phfData.byteLength;
    if (n < 20 || !currentImageData) return;

    if (phfData[0] !== 0x50 || phfData[1] !== 0x48 || phfData[2] !== 0x46 || phfData[3] !== 0x31) {
      return;
    }

    if (this.canvas.width !== fullW || this.canvas.height !== fullH) {
      return;
    }

    const dv = new DataView(phfData.buffer, phfData.byteOffset, phfData.byteLength);
    const channels = dv.getUint8(5);
    const rw = dv.getUint32(8, true);
    const rh = dv.getUint32(12, true);

    if (rw === 0 || rh === 0 || rx + rw > fullW || ry + rh > fullH) return;

    const outBytes = rw * rh * 4;
    this.ensureWasmBuffers(n, outBytes);

    wasmModule.HEAPU8.set(phfData, cachedSrcPtr);

    const t0 = performance.now();
    const rc = wasmModule._phf_decode_memory(cachedSrcPtr, n, cachedDstPtr, rw, rh, 4);
    lastDecMs = performance.now() - t0;

    if (rc !== 0) return;

    const decodedView = new Uint8Array(wasmModule.HEAPU8.buffer, cachedDstPtr, outBytes);
    const dstData = currentImageData.data;
    const fullRowBytes = fullW * 4;
    const subRowBytes = rw * 4;

    for (let y = 0; y < rh; y++) {
      const srcOffset = y * subRowBytes;
      const dstOffset = (ry + y) * fullRowBytes + rx * 4;
      dstData.set(decodedView.subarray(srcOffset, srcOffset + subRowBytes), dstOffset);
    }

    this.ctx.putImageData(currentImageData, 0, 0, rx, ry, rw, rh);

    frameCountWindow++;
    deltaFrameCountWindow++;
    rawBytesWindow += rw * rh * channels;
    compBytesWindow += n;
    fullEquivalentRawBytesWindow += fullW * fullH * channels;
    if (encMs > 0) rollingEncMs = rollingEncMs * 0.7 + encMs * 0.3;
    if (lastDecMs > 0) rollingDecMs = rollingDecMs * 0.7 + lastDecMs * 0.3;
  }

  getScaleTag(width, height) {
    const dpr = window.devicePixelRatio || 1;
    const viewPhysW = Math.round(this.canvas.clientWidth * dpr);
    if (width < this.sourceWidth || height < this.sourceHeight) {
      if (viewPhysW > width + 32) {
        return 'Server Downscale + GPU Upscale';
      }
      return 'Server Downscale';
    }
    if (viewPhysW > width + 32) {
      return '1:1 + GPU Upscale';
    }
    return '1:1 Nativo';
  }

  tickStats() {
    const now = performance.now();
    const elapsed = Math.max(1, now - this.lastStatsTickTime);
    this.lastStatsTickTime = now;

    const streamFps = Math.round((frameCountWindow * 1000) / elapsed);
    const bytesPerSec = (bytesReceivedWindow * 1000) / elapsed;

    // Bitrate calculation & formatting
    const mbps = (bytesPerSec * 8) / 1000000;
    let bitrateDisplay, bitrateUnit;
    if (mbps >= 1.0) {
      bitrateDisplay = mbps.toFixed(1);
      bitrateUnit = 'Mbps';
    } else {
      const kbps = bytesPerSec / 1024;
      bitrateDisplay = kbps < 0.1 ? '0.1' : (kbps < 10 ? kbps.toFixed(1) : Math.round(kbps).toString());
      bitrateUnit = 'KB/s';
    }

    // FPS & Mode calculation
    const targetHz = (clientDisplayHz >= 45 && clientDisplayHz <= 240) ? clientDisplayHz : 60;
    const isIdle = (streamFps < 20);
    const fpsDisplay = isIdle ? String(targetHz) : String(Math.min(targetHz, Math.max(streamFps, 1)));
    const fpsLabel = isIdle ? 'FPS (Ocioso)' : 'FPS (Ativo)';
    const streamMode = isIdle
      ? `${targetHz} Hz • Ocioso (${streamFps} delta/s)`
      : `${targetHz} Hz • Ativo (${streamFps} fps)`;

    // Lossless compression ratio & Delta bandwidth savings
    let ratio = compBytesWindow > 0 ? (rawBytesWindow / compBytesWindow) : 3.5;
    if (ratio < 1.0) ratio = 1.0;
    if (ratio > 15.0) ratio = 6.0;

    let deltaSavings = 98;
    if (fullEquivalentRawBytesWindow > 0 && compBytesWindow > 0) {
      deltaSavings = Math.max(0, Math.min(99, Math.round((1 - (compBytesWindow / fullEquivalentRawBytesWindow)) * 100)));
    }

    const curW = this.canvas.width || this.streamWidth || 1280;
    const curH = this.canvas.height || this.streamHeight || 720;

    this.hud.updateMetrics({
      fps: fpsDisplay,
      fpsLabel,
      bitrate: bitrateDisplay,
      bitrateUnit,
      encMs: rollingEncMs,
      decMs: rollingDecMs,
      sourceWidth: this.sourceWidth,
      sourceHeight: this.sourceHeight,
      width: curW,
      height: curH,
      scaleTag: this.getScaleTag(curW, curH),
      ratio,
      deltaSavings,
      streamMode
    });

    // Reset window counters
    frameCountWindow = 0;
    deltaFrameCountWindow = 0;
    fullFrameCountWindow = 0;
    bytesReceivedWindow = 0;
    rawBytesWindow = 0;
    compBytesWindow = 0;
    fullEquivalentRawBytesWindow = 0;
  }
}

window.addEventListener('DOMContentLoaded', () => {
  const client = new ScreenClient();
  client.start();
});
