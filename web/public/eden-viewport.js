// eden-viewport.js — audit row 28/C5 split: the engine's derived POINT space, the canvas's CSS
// display box and pixel backing store, the crosshair overlay, the WebGL-context-loss recovery
// panel, and adaptive resolution scaling. Everything here answers "what size/shape is the world
// rendered at, and where on screen is it" — coordinate TRANSLATION of a given point
// (client X/Y -> engine point space) also lives here since it is purely a function of this same
// geometry; consuming that translation to decide what a touch/click DOES is eden-input.js's job.
//
// Depends on (must load after): eden-host.js (canvasEl, moduleReady, callIfReady), eden-ui.js
// (window.EdenUI), eden-settings.js (window.EdenSettings). Declares, for later files/the boot
// script in eden-st.html: refreshEngineMetrics, toEnginePoint, applyDisplayMode, applyDrawableSize,
// adaptiveResolutionTick, crosshairEl, fullscreenBtn (eden-input.js's 'fullscreen' keybind action
// calls `fullscreenBtn.onclick()`) — all read only from callbacks/tick functions that run after
// boot, per eden-host.js's header note on cross-file timing.
'use strict';

// The engine's POINT space — its layout coordinate system, not the canvas's retina PIXEL backing
// store and not its CSS display size (CLAUDE.md #3). NO LONGER A CONSTANT as of audit rows D1 + D4:
// src/seam/DisplayProfile_web.mm derives it from the window aspect and the `ui_scale` /
// `display_layout` settings, so this is a cache of whatever it last chose, refreshed by
// refreshEngineMetrics() below. The literals are the pre-boot fallback and are still the exact
// values the touch profile resolves to (Classic 16:9 + ui_scale 200%).
let ENGINE_WIDTH = 568, ENGINE_HEIGHT = 320;
let ENGINE_ASPECT = ENGINE_WIDTH / ENGINE_HEIGHT;
// Pulls the three cached numbers back out of the engine. Cheap (three plain wasm getters) and
// called only from applyDisplayMode(), i.e. on boot / resize / a settings change — NOT per pointer
// event, which is why toEnginePoint() can read the cache instead of the module.
function refreshEngineMetrics() {
  if (!moduleReady || !Module._eden_display_point_width) return;
  ENGINE_WIDTH = Module._eden_display_point_width();
  ENGINE_HEIGHT = Module._eden_display_point_height();
  ENGINE_ASPECT = Module._eden_display_aspect_x1000() / 1000;
}

function toEnginePoint(clientX, clientY) {
  const rect = canvasEl.getBoundingClientRect();
  return {
    x: (clientX - rect.left) * (ENGINE_WIDTH / rect.width),
    y: (clientY - rect.top) * (ENGINE_HEIGHT / rect.height),
  };
}

// --- PC controls audit F3: crosshair overlay --------------------------------------------
const crosshairEl = document.createElement('div');
crosshairEl.id = 'eden-crosshair';
['top', 'bottom', 'left', 'right'].forEach((dir) => {
  const arm = document.createElement('div');
  arm.className = 'arm ' + dir;
  crosshairEl.appendChild(arm);
});
document.body.appendChild(crosshairEl);
function updateCrosshairPosition() {
  const rect = canvasEl.getBoundingClientRect();
  crosshairEl.style.left = (rect.left + rect.width / 2) + 'px';
  crosshairEl.style.top = (rect.top + rect.height / 2) + 'px';
}
window.addEventListener('resize', updateCrosshairPosition);
document.addEventListener('fullscreenchange', updateCrosshairPosition);
updateCrosshairPosition();

// --- Perf audit item #5 (C3): WebGL context loss --------------------------------------------
// The shim PUSHES both events here (src/shim/gl/gl_es1_shim.cpp's context-loss block calls
// window.EdenRenderer.onContextLost/onContextRestored) rather than the page polling for them, and
// it has already paused the engine's main loop by the time this runs.
//
// Why this offers a reload rather than resuming: the shim rebuilds its OWN GL objects on a
// restore, but every engine texture was uploaded once during load by Texture2D_web.mm, and
// re-uploading them means re-running load code that lives in Classes/ — untouchable. So a resumed
// frame would be untextured garbage. Reload is the honest recovery, and world saves survive it
// (IDBFS), so the cost is a load screen, not progress.
let rendererPanelEl = null;
function showRendererPanel(title, detail) {
  if (!rendererPanelEl) {
    // Built from the design system rather than hand-styled inline (design-system pass). It is an
    // alertdialog, so it sits at the same z-layer as the load-failure dialog — both are
    // "something broke and only you can decide what happens next" surfaces.
    const UI = window.EdenUI;
    rendererPanelEl = UI.scrim({ id: 'eden-renderer-lost' });
    rendererPanelEl.style.zIndex = 'var(--eden-z-alert)';
    const win = UI.window({ title: 'Renderer lost', variant: 'dialog', scrollbar: false,
                            role: 'alertdialog' });
    const stack = UI.el('div', 'eden-stack');
    const p = UI.el('p', 'eden-stack__text');
    stack.appendChild(p);
    stack.appendChild(UI.button({
      size: 'md', tone: 'positive', icon: 'rotate-ccw', label: 'Reload',
      onClick: () => location.reload(),
    }));
    win.content.appendChild(stack);
    rendererPanelEl.appendChild(win.root);
    document.body.appendChild(rendererPanelEl);
    rendererPanelEl._h = win.title; rendererPanelEl._p = p;
  }
  rendererPanelEl._h.textContent = title;
  rendererPanelEl._p.textContent = detail;
  rendererPanelEl.style.display = 'flex';
}
window.EdenRenderer = {
  onContextLost: function () {
    // Pointer lock would keep the cursor hidden over a panel the player has to click.
    if (document.pointerLockElement === canvasEl) document.exitPointerLock();
    showRendererPanel('Renderer lost',
      'The browser dropped this page\'s WebGL context — usually memory pressure, a driver ' +
      'reset, or the tab being backgrounded for a long time. The game is paused. Reload to ' +
      'continue; your worlds are saved in browser storage and are not affected.');
  },
  onContextRestored: function () {
    showRendererPanel('Renderer restored',
      'A new WebGL context is available, but the game\'s textures were lost with the old one ' +
      'and can only be reloaded from scratch. Reload to continue.');
  },
};

// --- PC controls audit F9d: display mode (Fixed / Fit window / Fullscreen-on-demand) --------
// Where the canvas's CSS DISPLAY size comes from. Since audit D1 this also decides the engine's
// point space, because applyDisplayMode() hands the available box to eden_display_set_viewport()
// before it letterboxes to the aspect the engine picked — see that function. toEnginePoint()
// derives from getBoundingClientRect() and ENGINE_WIDTH/HEIGHT, both of which follow, so there is
// still no coordinate-space work to do here.

// --- Perf audit item #6 (§4c.1-2): the DYNAMIC drawable ------------------------------------
// Before this, the backing store was pinned to 1136x640 (retina 2x of the engine's 568x320
// points) and devicePixelRatio was never read, so in Fit/Fullscreen mode a 1136x640 buffer was
// upscaled to whatever the display was — the game was permanently blurry on any modern screen.
//
// Now: backing store = the canvas's real CSS box x min(devicePixelRatio, dpr_cap) x render_scale.
// The engine keeps projecting in its 568x320 POINT space and SCREEN_WIDTH/HEIGHT/SCALE_* are
// NEVER touched (that is the port's most-bitten failure class — see EAGLView_web.mm); only the
// pixel buffer and the GL viewport change. toEnginePoint() already derives from
// getBoundingClientRect(), so input coordinates need no work at all.
//
// Uses the measured box rather than the engine aspect on purpose: in real fullscreen the UA
// stylesheet owns the element's size, and matching whatever the box actually is keeps the backing
// store and the display area in step (the previous fixed buffer was stretched to that same box,
// so this introduces no distortion that was not already there).
// --- Audit row F1: adaptive resolution scaling ---------------------------------------------
// The plumbing this row asked for a controller on top of (applyDrawableSize, render_scale)
// already existed; this is that controller. Frame time is sampled from the cadence of
// Module.__edenFramePost itself (see eden-st.html's trackCursorNeed) — the same per-frame hook
// audit row A8 already unified everything onto, so this adds no second timer. A rolling window
// smooths out one-off hitches (a GC pause, a texture decode) so the controller reacts to a
// sustained trend, not a single frame, and a cooldown plus a gap between the down/up thresholds
// (hysteresis) is what keeps it from oscillating once it settles near a boundary.
//
// Deliberately layered ON TOP of render_scale rather than driving that setting directly:
// render_scale is a persisted preference (Settings_web.mm's kSettings[]), and writing to it every
// time the frame rate dips would both spam IndexedDB/localStorage and silently overwrite a choice
// the player made on purpose. adaptiveScaleFactor is pure runtime state — never persisted, always
// <= 1.0 (it only ever makes frames CHEAPER than the user's own setting, never more expensive),
// and resets to 1.0 on reload, so the worst case of it misbehaving is "back to what render_scale
// alone would have produced," not a corrupted preference.
let adaptiveScaleFactor = 1.0;      // multiplies into the factor below; always in [MIN, 1.0]
const ADAPTIVE_MIN_SCALE = 0.5;     // matches render_scale's own lowest enum step (50%) — floor
const ADAPTIVE_STEP = 0.1;
const ADAPTIVE_WINDOW = 30;         // frame samples averaged before a decision (~0.5s at 60Hz)
const ADAPTIVE_COOLDOWN_MS = 2000;  // minimum time between adjustments, either direction
const ADAPTIVE_FPS_LOW = 45;        // sustained average below this -> scale down
const ADAPTIVE_FPS_HIGH = 55;       // sustained average above this -> scale up (hysteresis gap)
let adaptiveFrameTimes = [];
let adaptiveLastFrameAt = 0;
let adaptiveLastAdjustAt = 0;
function adaptiveResolutionTick() {
  const now = performance.now();
  if (adaptiveLastFrameAt) {
    adaptiveFrameTimes.push(now - adaptiveLastFrameAt);
    if (adaptiveFrameTimes.length > ADAPTIVE_WINDOW) adaptiveFrameTimes.shift();
  }
  adaptiveLastFrameAt = now;
  if (adaptiveFrameTimes.length < ADAPTIVE_WINDOW) return;         // not enough samples yet
  if (now - adaptiveLastAdjustAt < ADAPTIVE_COOLDOWN_MS) return;   // cooldown

  const avgMs = adaptiveFrameTimes.reduce((a, b) => a + b, 0) / adaptiveFrameTimes.length;
  const avgFps = 1000 / avgMs;
  if (avgFps < ADAPTIVE_FPS_LOW && adaptiveScaleFactor > ADAPTIVE_MIN_SCALE) {
    adaptiveScaleFactor = Math.max(ADAPTIVE_MIN_SCALE, adaptiveScaleFactor - ADAPTIVE_STEP);
    adaptiveLastAdjustAt = now;
    applyDrawableSize();
  } else if (avgFps > ADAPTIVE_FPS_HIGH && adaptiveScaleFactor < 1.0) {
    adaptiveScaleFactor = Math.min(1.0, adaptiveScaleFactor + ADAPTIVE_STEP);
    adaptiveLastAdjustAt = now;
    applyDrawableSize();
  }
}

function applyDrawableSize() {
  if (!moduleReady) return;
  const rect = canvasEl.getBoundingClientRect();
  if (rect.width < 1 || rect.height < 1) return;   // display:none / not laid out yet
  const cap = Module._eden_get_dpr_cap_x100() / 100;
  const scale = Module._eden_get_render_scale_pct() / 100;
  const factor = Math.min(window.devicePixelRatio || 1, cap) * scale * adaptiveScaleFactor;
  const w = Math.max(1, Math.round(rect.width * factor));
  const h = Math.max(1, Math.round(rect.height * factor));
  // The dedupe compares against the CANVAS's real attributes, not a JS-side memo of what was last
  // requested. That distinction is load-bearing and cost a debugging round to find:
  // Module.onRuntimeInitialized fires BEFORE main(), so the first applyDisplayMode() runs before
  // EAGLView_web's establishScreenMetrics asserts its own 1136x640 boot size — a remembered
  // "already 1704x960" would then make every later resize a no-op against a drawable the engine
  // had since overwritten. The canvas attributes cannot lie about that. (The boot race itself is
  // handled by the deferred applyDisplayMode() in onRuntimeInitialized.)
  if (canvasEl.width === w && canvasEl.height === h) return;
  Module._eden_set_drawable_size(w, h);
}

function applyDisplayMode() {
  if (!moduleReady) return;
  const mode = Module._eden_get_display_mode(); // 0=Fixed, 1=Fit, 2=Fullscreen-on-demand
  const vw = window.innerWidth, vh = window.innerHeight;
  // The box the canvas is ALLOWED to occupy, before any aspect fitting.
  // "Fixed" means at most 852x480 (1.5x the classic 568x320 point space) — a deliberately small,
  // stable window-in-a-page. Fit/Fullscreen get the whole viewport.
  let availW = vw, availH = vh;
  if (mode === 0 && document.fullscreenElement !== canvasEl) {
    availW = Math.min(852, vw);
    availH = Math.min(480, vh);
  }
  // Audit D1: hand the engine the SHAPE of that box first. In Adaptive layout it derives its point
  // space (and therefore P_ASPECT_RATIO) from it, so the aspect read back on the next line IS the
  // available box's aspect and the fit below is a no-op — that is the whole point, a wider window
  // shows more world instead of being letterboxed. It can still differ: Classic layout pins 16:9,
  // and the engine clamps extreme aspects (portrait phones, 32:9 monitors) rather than hand its
  // absolutely-sized HUD rects a shape they cannot fill. Whenever it does differ, the fit below
  // letterboxes — which is what keeps the picture undistorted, since findWorldCoords's raycast
  // (Util.mm) and gluPerspective(..., P_ASPECT_RATIO, ...) both assume the rendered box IS the
  // engine's aspect. A distorted box stretches the scene AND misaims every click.
  if (Module._eden_display_set_viewport) {
    Module._eden_display_set_viewport(Math.max(1, Math.round(availW)), Math.max(1, Math.round(availH)));
  }
  refreshEngineMetrics();
  let w = availW, h = availW / ENGINE_ASPECT;
  if (h > availH) { h = availH; w = availH * ENGINE_ASPECT; }
  canvasEl.style.width = Math.round(w) + 'px';
  canvasEl.style.height = Math.round(h) + 'px';
  updateCrosshairPosition();
  // The CSS box just changed, so the pixel buffer that backs it has to follow (item #6).
  applyDrawableSize();
  fullscreenBtn.style.display = (mode === 2 && document.fullscreenElement !== canvasEl) ? '' : 'none';
}
window.addEventListener('resize', applyDisplayMode);
document.addEventListener('fullscreenchange', applyDisplayMode);
// Q2 (dazzling-munching-bengio.md perf audit): the settings panel is the only other place
// display_mode can change; react to that write instead of recomputing layout every rAF frame.
// Item #6 adds render_scale/dpr_cap on the same hook — they change the backing store only, so they
// do not need the full layout pass.
window.EdenSettings.onChange(function (key) {
  // ui_scale/display_layout (audit D1/D4) change the engine's POINT space, which changes the
  // aspect the canvas has to be letterboxed to — so they need the full layout pass, not just a
  // backing-store resize. The C side has already re-derived and re-laid-out by the time this
  // fires; applyDisplayMode() re-reads the result.
  // `input_mode` is in this list because it selects the PROFILE, and the profile is what Auto
  // resolves ui_scale/display_layout against (audit D4).
  if (key === 'display_mode' || key === 'ui_scale' || key === 'display_layout' ||
      key === 'input_mode') applyDisplayMode();
  else if (key === 'render_scale' || key === 'dpr_cap') applyDrawableSize();
});

// requestFullscreen needs a real user gesture, so "Fullscreen-on-demand" mode surfaces a small
// button rather than trying to enter fullscreen automatically on selecting the setting.
// eden-input.js's 'fullscreen' keybind action calls `fullscreenBtn.onclick()` — a cross-file read
// of this const, safe per eden-host.js's header (only ever happens inside a later keydown handler).
const fullscreenBtn = window.EdenUI.button({
  size: 'sm', label: 'Fullscreen', icon: 'monitor',
  onClick: () => { canvasEl.requestFullscreen().catch(() => {}); },
});
fullscreenBtn.id = 'eden-fullscreen-btn';
// Position is page chrome, not a design-system concern — the button itself is a stock
// .eden-btn--sm, only where it floats is set here.
fullscreenBtn.style.cssText = 'position:fixed;top:10px;right:10px;z-index:12;display:none;';
document.body.appendChild(fullscreenBtn);
