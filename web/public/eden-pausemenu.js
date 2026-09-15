// eden-pausemenu.js — the in-game pause menu (pass 30; restyled onto the Eden: Community Edition
// design system).
// Requires: window.EdenUI, window.EdenSettings, window.EdenAssets. Publishes: window.EdenPauseMenu.
// See docs/ui.md's dependency graph (audit I2).
//
// Replaces the engine's own tiny 4-icon in-game menu (Hud::renderMenuScreen — Save/Warp
// home/Take photo/Save & exit, opened by tapping the corner icon while playing,
// Classes/Hud.mm:1537-1570) with a DOM panel.
//
// UNLIKE the settings panel, this one does NOT --wrap anything on the C++ side (there is no
// standalone PauseMenu class to wrap — the old menu is a few branches inline inside Hud::update/
// Hud::render, which are cross-TU-called as a whole and far too central to intercept piecemeal).
// Instead this is a thin DOM overlay that tracks the engine's REAL `hud->inmenu` flag (polled via
// `eden_hud_in_menu()`, web/src/seam/Input_web.mm) and, for each button, synthesizes the exact
// same tap the old GL icons used to receive (`eden_tap_hud_button_begin/end`, extended with cases
// 3-6 for rsave/rhome/rcam/rexit) — so every action (save, the existing warp-home confirm dialog,
// photo mode, save-and-quit) runs through the SAME engine code Hud::handlePickMenu always has.
//
// EXACTLY ONE IN-GAME MENU IS EVER ON SCREEN. This used to be true by accident — the panel was a
// full-canvas opaque overlay, so the GL icons rendering underneath were simply never visible. Once
// it became a shrink-to-fit dialog they showed through the scrim, so it is now explicit and keyed
// off the `legacy_menu` setting: `Hud::renderMenuScreen` is suppressed via
// `eden_hud_draw_menu_screen_hook` (installed in Menu_web.mm) unless the player opted into the
// legacy GL UI, and in that case tick() below keeps THIS panel closed instead.
//
// DESIGN-SYSTEM PASS: the panel is now built from public/eden-ui.js's window + button primitives
// and carries no CSS of its own. There is no pause-menu mockup in the Figma source, so rather
// than invent a fourth surface this composes the existing ones — a beveled window, a white
// content box, and a left-aligned two-column grid of standard buttons, shrink-wrapped to its
// content (`.eden-window--fit`). Resume keeps a vector glyph; the other five carry the game's own
// raster icon art out of media/ui via eden-assets.js.
// The engine-facing half of this file is untouched.
(function () {
  'use strict';

  var S = {
    open: false,
    root: null,
    releaseFocus: null,
    settingsWasOpenedFromHere: false
  };

  function M() { return window.Module; }
  function ready() {
    return window.__edenModuleReady && M() && typeof M()._eden_hud_in_menu === 'function';
  }

  // which: see eden_tap_hud_button_begin/end's switch (Input_web.mm). A synthetic tap needs a
  // begin on one frame and an end on the next — Player::processInput-style tap logic requires the
  // down/up to straddle a tick (same requirement eden_click_begin/end's own comment documents).
  function tapHudButton(which) {
    M()._eden_tap_hud_button_begin(which);
    requestAnimationFrame(function () { M()._eden_tap_hud_button_end(which); });
  }

  function resume() { tapHudButton(0); }          // rmenu again — same as the old "tap corner icon to close"
  function saveGame() { tapHudButton(3); close(); }
  function warpHome() { tapHudButton(4); close(); } // hands off to the existing warp-home confirm dialog
  function takePhoto() { tapHudButton(5); close(); }
  function quitToMenu() { tapHudButton(6); close(); }

  function openSettingsFromHere(group) {
    if (!window.EdenSettings) return;
    // Unlike Resume/Save/Warp Home/Take Photo/Quit above (all real taps on an engine HUD rect,
    // which already got S_MENU_BUTTON_PRESS/RELEASE in pass 39 — Classes/Hud.mm), this button has
    // no engine-side counterpart at all, so it needs its own sound.
    if (ready()) M()._eden_play_menu_button_sound(1);
    S.settingsWasOpenedFromHere = true;
    hide();
    S.open = false;   // so tick()'s (engineWants && !S.open) re-opens us once Settings closes
    window.EdenSettings.open(group);
  }

  // Audit row G1 (control discoverability): jumps straight to the Keys tab instead of whatever
  // tab Settings last had open, and — since that tab lists key->action only, nothing about mouse
  // look or touch chrome — the panel's Keys tab already carries the keybind list; this button just
  // gets the player there in one click instead of Settings -> tab-click.
  function openControls() { openSettingsFromHere('Keys'); }

  // Audit row 17/G1 (touch half — "on-screen controls being draggable"): puts the joystick pad
  // into an explicit, opt-in "move controls" mode (window.EdenJoystickCustomize, eden-input.js)
  // rather than a long-press during normal play, so there is no ambiguity with actually walking.
  // A floating "Done" button (independent of this panel, which is already closed by the time the
  // player is dragging) is the only way out, mirroring eden-st.html's fullscreenBtn pattern for a
  // page-chrome control that isn't part of the design-system window stack.
  var moveControlsDoneBtn = null;
  function endMoveControls() {
    window.EdenJoystickCustomize.stop();
    if (moveControlsDoneBtn) {
      moveControlsDoneBtn.parentNode.removeChild(moveControlsDoneBtn);
      moveControlsDoneBtn = null;
    }
  }
  function openMoveControls() {
    if (ready()) M()._eden_play_menu_button_sound(1);
    hide();
    S.open = false;
    window.EdenJoystickCustomize.start();
    showToast('Drag the joystick to move it');
    moveControlsDoneBtn = window.EdenUI.button({
      size: 'md', tone: 'positive', label: 'Done', onClick: endMoveControls,
    });
    moveControlsDoneBtn.style.cssText =
      'position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:12;';
    document.body.appendChild(moveControlsDoneBtn);
  }

  function build() {
    var UI = window.EdenUI;
    var A = window.EdenAssets;
    UI.ensureCSS();

    var scrim = UI.scrim({ id: 'eden-pause-backdrop', onDismiss: resume });
    // "Menu", not "Paused": opening this does NOT pause the engine — World::update keeps running
    // behind it (hud->inmenu only gates input and swaps what Hud::render draws), so creatures move,
    // fire spreads and the sun keeps travelling while it is up. Calling it "Paused" was a promise
    // the port doesn't keep.
    var win = UI.window({
      title: 'Menu',
      variant: 'dialog',
      className: 'eden-window--fit',
      scrollbar: false,
    });

    // No close button in the title bar: Resume is the first row of the stack, the scrim dismisses,
    // and Escape resumes — a fourth affordance for the same action was just noise in a panel this
    // small (and it was the one thing forcing the title bar wider than the content).
    //
    // Resume keeps the vector play glyph; the rest use the game's own icon art (media/ui), which is
    // the same art the engine's 4-icon GL menu drew for save/home/camera. Row-major order across two
    // columns puts Resume top-left and Quit bottom-right, which is the order they were in when this
    // was one tall column.
    var stack = UI.el('div', 'eden-stack eden-stack--left eden-stack--grid');
    stack.appendChild(UI.button({ size: 'md', tone: 'positive', icon: 'play', label: 'Resume', onClick: resume }));
    stack.appendChild(UI.button({ size: 'md', iconImg: A.NAMES.iconSave, label: 'Save Game', onClick: saveGame }));
    stack.appendChild(UI.button({ size: 'md', iconImg: A.NAMES.iconHome, label: 'Warp Home', onClick: warpHome }));
    stack.appendChild(UI.button({ size: 'md', iconImg: A.NAMES.iconCamera, label: 'Take Photo', onClick: takePhoto }));
    stack.appendChild(UI.button({ size: 'md', iconImg: A.NAMES.iconSettings, label: 'Settings', onClick: function () { openSettingsFromHere(); } }));
    stack.appendChild(UI.button({ size: 'md', icon: 'keyboard', label: 'Controls', onClick: openControls }));
    // Touch-only: there is no "on-screen joystick" to reposition on the desktop profile.
    if (ready() && M()._eden_effective_input_is_touch()) {
      stack.appendChild(UI.button({ size: 'md', icon: 'hand', label: 'Move Controls', onClick: openMoveControls }));
    }
    stack.appendChild(UI.button({ size: 'md', tone: 'danger', iconImg: A.NAMES.iconQuit, label: 'Quit to Menu', onClick: quitToMenu }));
    win.content.appendChild(stack);

    scrim.appendChild(win.root);
    UI.bindButtonSounds(scrim);
    S.root = scrim;
    return scrim;
  }

  function show() {
    if (S.root) return;
    document.body.appendChild(build());
    S.releaseFocus = window.EdenUI.trapFocus(S.root);
    if (document.pointerLockElement) document.exitPointerLock();
  }

  function hide() {
    if (!S.root) return;
    if (S.releaseFocus) { S.releaseFocus(); S.releaseFocus = null; }
    if (S.root.parentNode) S.root.parentNode.removeChild(S.root);
    S.root = null;
  }

  // Not a real "close" — the engine's inmenu flag is the source of truth; this just makes the
  // resume/save/etc. actions above feel synchronous (hide immediately) instead of waiting a full
  // frame for the next tick() poll to notice inmenu went false.
  function close() { hide(); }

  // Polled once per frame from eden-st.html's rAF loop, same pattern as EdenSettings.tick(): keep
  // the DOM mirror in lockstep with hud->inmenu, however it changed (our own buttons, the ESC key
  // via eden_tap_hud_button_*(0), or — while this panel isn't up — a direct tap on the GL corner
  // icon, which still works exactly as before).
  function tick() {
    if (!ready()) return;
    // While the Settings panel we opened from here is up, don't fight over the DOM: wait for it
    // to close, then reopen ourselves (inmenu is still true the whole time — we never touched it).
    if (S.settingsWasOpenedFromHere) {
      if (window.EdenSettings && window.EdenSettings.isOpen()) return;
      S.settingsWasOpenedFromHere = false;
    }
    // Legacy UI on = the engine draws its own 4-icon GL panel (the Hud hook in Menu_web.mm lets it
    // through) and this one stays away entirely, which is the other half of "exactly one in-game
    // menu is ever on screen". Checked every tick rather than once at load, so toggling the setting
    // takes effect immediately — including closing a panel that is already up.
    var legacy = M()._eden_legacy_ui_active && M()._eden_legacy_ui_active() !== 0;
    var engineWants = !legacy && M()._eden_hud_in_menu() !== 0;
    if (engineWants && !S.open) { S.open = true; show(); }
    if (!engineWants && S.open) { S.open = false; hide(); }
  }

  window.EdenPauseMenu = {
    tick: tick,
    isOpen: function () { return S.open; }
  };
})();
