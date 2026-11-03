/**
 * PHF-RDP HUD Controller
 * Manages draggable floating HUD, toggling visibility, monitor selection,
 * smart scaling selection, fullscreen, RDP input control & WASAPI system audio toggle.
 */

export class HUDController {
  constructor(callbacks) {
    this.onResolutionModeChange = callbacks.onResolutionModeChange;
    this.onInputToggle = callbacks.onInputToggle;
    this.onMonitorChange = callbacks.onMonitorChange;
    this.onAudioToggle = callbacks.onAudioToggle;

    this.panel = document.getElementById('floating-hud');
    this.trigger = document.getElementById('hud-trigger');
    this.dragHandle = document.getElementById('hud-drag-handle');
    this.btnMinimize = document.getElementById('btn-minimize-hud');
    this.btnFullscreen = document.getElementById('btn-fullscreen');
    this.monitorSelect = document.getElementById('monitor-select');
    this.toggleInput = document.getElementById('toggle-input-control');
    this.toggleAudio = document.getElementById('toggle-audio-stream');
    this.inputHint = document.getElementById('input-hint-text');
    this.resButtons = document.querySelectorAll('#res-selector .pill-btn');

    /* Metrics elements */
    this.valFps = document.getElementById('val-fps');
    this.lblFps = document.getElementById('lbl-fps');
    this.valBitrate = document.getElementById('val-bitrate');
    this.lblBitrate = document.getElementById('lbl-bitrate');
    this.valEnc = document.getElementById('val-enc');
    this.valDec = document.getElementById('val-dec');
    this.valSourceRes = document.getElementById('val-source-res');
    this.valRes = document.getElementById('val-res');
    this.valRatio = document.getElementById('val-ratio');
    this.valStreamMode = document.getElementById('val-stream-mode');

    this.isDragging = false;
    this.dragOffsetX = 0;
    this.dragOffsetY = 0;
    this.inputEnabled = false;

    // Restore saved vertical position of trigger tab
    const savedTop = localStorage.getItem('phf_hud_tab_top');
    if (savedTop !== null) {
      this.trigger.style.top = `${savedTop}px`;
      this.trigger.style.transform = 'none';
    }

    this.initEvents();
  }

  initEvents() {
    // Vertical dragging & click handling for edge tab trigger
    let isTabDragging = false;
    let tabStartY = 0;
    let tabStartTop = 0;
    let tabMoved = false;

    this.trigger.addEventListener('pointerdown', (e) => {
      isTabDragging = true;
      tabStartY = e.clientY;
      const rect = this.trigger.getBoundingClientRect();
      tabStartTop = rect.top;
      tabMoved = false;
      this.trigger.setPointerCapture(e.pointerId);
      this.trigger.classList.add('tab-dragging');
    });

    this.trigger.addEventListener('pointermove', (e) => {
      if (!isTabDragging) return;
      const dy = e.clientY - tabStartY;
      if (Math.abs(dy) > 3) {
        tabMoved = true;
      }
      let newTop = tabStartTop + dy;
      const h = this.trigger.offsetHeight || 56;
      newTop = Math.max(10, Math.min(window.innerHeight - h - 10, newTop));
      this.trigger.style.top = `${newTop}px`;
      this.trigger.style.transform = 'none';
    });

    const finishTabDrag = (e) => {
      if (!isTabDragging) return;
      isTabDragging = false;
      this.trigger.classList.remove('tab-dragging');
      try { this.trigger.releasePointerCapture(e.pointerId); } catch (_) {}
      if (tabMoved) {
        const rect = this.trigger.getBoundingClientRect();
        localStorage.setItem('phf_hud_tab_top', String(Math.round(rect.top)));
      } else {
        this.toggleHUD();
      }
    };

    this.trigger.addEventListener('pointerup', finishTabDrag);
    this.trigger.addEventListener('pointercancel', finishTabDrag);
    this.btnMinimize.addEventListener('click', () => this.hideHUD());

    window.addEventListener('keydown', (e) => {
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;

      if (e.altKey && (e.key === 'h' || e.key === 'H')) {
        e.preventDefault();
        this.toggleHUD();
        return;
      }

      if (!this.inputEnabled) {
        if (e.key === 'h' || e.key === 'H') {
          this.toggleHUD();
        } else if (e.key === 'f' || e.key === 'F') {
          this.toggleFullscreen();
        } else if (e.key === 'Escape' && !this.panel.classList.contains('hud-hidden')) {
          this.hideHUD();
        }
      }
    });

    this.dragHandle.addEventListener('mousedown', (e) => this.startDrag(e));
    window.addEventListener('mousemove', (e) => this.onDrag(e));
    window.addEventListener('mouseup', () => this.stopDrag());

    this.btnFullscreen.addEventListener('click', () => this.toggleFullscreen());

    if (this.monitorSelect) {
      this.monitorSelect.addEventListener('change', (e) => {
        const id = parseInt(e.target.value, 10) || 0;
        if (this.onMonitorChange) {
          this.onMonitorChange(id);
        }
      });
    }

    this.resButtons.forEach(btn => {
      btn.addEventListener('click', () => {
        this.resButtons.forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        const mode = btn.dataset.mode || 'auto';
        const w = parseInt(btn.dataset.w, 10) || 0;
        const h = parseInt(btn.dataset.h, 10) || 0;
        if (this.onResolutionModeChange) {
          this.onResolutionModeChange(mode, w, h);
        }
      });
    });

    this.toggleInput.addEventListener('change', (e) => {
      this.inputEnabled = e.target.checked;
      if (this.inputHint) {
        this.inputHint.textContent = this.inputEnabled
          ? 'Controle RDP ATIVO: cliques, teclado e Ctrl+C/Ctrl+V sincronizados (Alt+H abre/fecha HUD).'
          : 'Ative para controlar mouse, teclado e Ctrl+C/Ctrl+V (Alt+H abre/fecha HUD).';
      }
      if (this.onInputToggle) {
        this.onInputToggle(this.inputEnabled);
      }
    });

    if (this.toggleAudio) {
      this.toggleAudio.addEventListener('change', (e) => {
        if (this.onAudioToggle) {
          this.onAudioToggle(e.target.checked);
        }
      });
    }
  }

  updateMonitors(monitors, activeMonitorId) {
    if (!this.monitorSelect || !Array.isArray(monitors) || monitors.length === 0) return;
    this.monitorSelect.innerHTML = '';
    monitors.forEach(m => {
      const opt = document.createElement('option');
      opt.value = String(m.id);
      opt.textContent = m.name || `Display ${m.id + 1}: ${m.w}x${m.h}`;
      if (m.id === activeMonitorId) opt.selected = true;
      this.monitorSelect.appendChild(opt);
    });
  }

  toggleHUD() {
    if (this.panel.classList.contains('hud-hidden')) {
      this.showHUD();
    } else {
      this.hideHUD();
    }
  }

  showHUD() {
    this.panel.classList.remove('hud-hidden');
    this.trigger.classList.add('hud-open');
    this.trigger.style.opacity = '0.2';
  }

  hideHUD() {
    this.panel.classList.add('hud-hidden');
    this.trigger.classList.remove('hud-open');
    this.trigger.style.opacity = '';
  }

  startDrag(e) {
    this.isDragging = true;
    const rect = this.panel.getBoundingClientRect();
    this.dragOffsetX = e.clientX - rect.left;
    this.dragOffsetY = e.clientY - rect.top;
  }

  onDrag(e) {
    if (!this.isDragging) return;

    let x = e.clientX - this.dragOffsetX;
    let y = e.clientY - this.dragOffsetY;

    const maxX = window.innerWidth - this.panel.offsetWidth - 10;
    const maxY = window.innerHeight - this.panel.offsetHeight - 10;

    x = Math.max(10, Math.min(x, maxX));
    y = Math.max(10, Math.min(y, maxY));

    this.panel.style.left = `${x}px`;
    this.panel.style.top = `${y}px`;
    this.panel.style.right = 'auto';
  }

  stopDrag() {
    this.isDragging = false;
  }

  toggleFullscreen() {
    if (!document.fullscreenElement) {
      document.documentElement.requestFullscreen().catch(err => {
        console.warn(`Fullscreen error: ${err.message}`);
      });
    } else {
      if (document.exitFullscreen) {
        document.exitFullscreen();
      }
    }
  }

  updateMetrics(stats) {
    if (stats.fps !== undefined && this.valFps) {
      this.valFps.textContent = stats.fps;
    }
    if (stats.fpsLabel !== undefined && this.lblFps) {
      this.lblFps.textContent = stats.fpsLabel;
    }
    if (stats.bitrate !== undefined && this.valBitrate) {
      this.valBitrate.textContent = stats.bitrate;
    }
    if (stats.bitrateUnit !== undefined && this.lblBitrate) {
      this.lblBitrate.textContent = stats.bitrateUnit;
    }
    if (stats.encMs !== undefined && this.valEnc) {
      this.valEnc.textContent = `${stats.encMs.toFixed(1)} ms`;
    }
    if (stats.decMs !== undefined && this.valDec) {
      this.valDec.textContent = `${stats.decMs.toFixed(1)} ms`;
    }
    if (stats.sourceWidth && stats.sourceHeight && this.valSourceRes) {
      this.valSourceRes.textContent = `${stats.sourceWidth}x${stats.sourceHeight}`;
    }
    if (stats.width && stats.height && this.valRes) {
      const label = stats.scaleTag ? ` (${stats.scaleTag})` : '';
      this.valRes.textContent = `${stats.width}x${stats.height}${label}`;
    }
    if (stats.ratio !== undefined && this.valRatio) {
      const deltaStr = stats.deltaSavings !== undefined ? ` (${stats.deltaSavings}% Delta)` : '';
      this.valRatio.textContent = `${stats.ratio.toFixed(1)}x${deltaStr}`;
    }
    if (stats.streamMode !== undefined && this.valStreamMode) {
      this.valStreamMode.textContent = stats.streamMode;
    }
  }
}
