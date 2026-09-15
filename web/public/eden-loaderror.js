// eden-loaderror.js — corrupt/truncated-save recovery dialog (perf-audit C4's other still-open
// item: "a load-failure recovery UI, needs a corrupt-file signal from the load path"). That signal
// now exists: Classes/FileManager.mm's loadWorld() sanity-checks a save's header/directory before
// trusting it and calls eden_report_load_failure() (web/src/seam/LoadFailure_web.mm) instead of
// reading garbage. This file just polls the resulting flag and shows a DOM panel, same shape as
// eden-pausemenu.js: no engine state of its own, built from the shared design system.
//
// Requires: window.EdenUI, window.EdenPauseMenu (its .tick is suspended while this dialog is up),
// FS.syncfs. Publishes: window.EdenLoadError. See docs/ui.md's dependency graph (audit I2).
//
// There is no "resume" out of this dialog — by the time eden_load_failed() is true, loadWorld()
// has already bailed out with the terrain cleared and no valid world loaded, so the only sane
// actions are: restore the `.bak` backup slot and retry, or go back to the menu and pick something
// else. Both funnel through a full page reload — simplest way to get every affected subsystem
// (terrain, HUD, menu's world list) back to a clean boot state after a load this abnormal.
(function () {
  'use strict';

  var S = { open: false, root: null, releaseFocus: null };

  function M() { return window.Module; }
  function ready() {
    return window.__edenModuleReady && M() && typeof M()._eden_load_failed === 'function';
  }

  // Same static-buffer C-string convention as eden-settings.js's utf8() (schema/index-based exports
  // instead of passing strings into wasm, which would need _malloc/_free on the export list).
  function utf8(ptr) {
    var H = M().HEAPU8, end = ptr;
    while (H[end]) end++;
    // The Uint8Array copy is load-bearing in the EDEN_THREADED build (shared memory cannot be
    // handed to TextDecoder) — full reasoning on the canonical copy in eden-settings.js.
    return new TextDecoder().decode(new Uint8Array(H.subarray(ptr, end)));
  }

  // Reloading is how both buttons below get back to a clean boot (main() re-mounts IDBFS, the
  // menu re-scans Documents, no live World's half-cleared state needs undoing) — but IDBFS's
  // autoPersist sync (eden-storage.js) is DEBOUNCED, not synchronous per write. Reloading
  // immediately after a write races that debounce: found live (2026-07-26) when a restore's
  // fixed bytes were still in MEMFS but lost to the reload because they hadn't reached
  // IndexedDB yet, leaving the file exactly as broken as before the button was clicked. Forcing
  // a `FS.syncfs(false, …)` and reloading only in its callback closes that race — same
  // mechanism eden-storage.js's own `flushNow()` uses on visibilitychange/pagehide, just awaited
  // here instead of fire-and-forget, since this is the one place a reload deliberately follows a
  // write moments earlier.
  function reloadAfterFlush() {
    try {
      FS.syncfs(false, function () { location.reload(); });
    } catch (e) {
      location.reload();
    }
  }

  function restoreAndRetry() {
    var ok = M()._eden_load_restore_backup() !== 0;
    if (ok) {
      reloadAfterFlush();
    } else {
      var msg = document.getElementById('eden-loaderror-msg');
      if (msg) msg.textContent = 'No backup was available for this world — nothing to restore.';
    }
  }

  function quitToMenu() {
    // No `.bak` (or restore failed) and no engine-side way to bail a half-loaded world back to
    // the menu without re-running boot — same reasoning as restoreAndRetry's reload.
    reloadAfterFlush();
  }

  // ROADMAP Phase M / M5.3: World::loadWorld reports this exact token instead of a corrupt-save
  // reason when it refuses a 256z world on a device the page flagged as low-memory. Same channel,
  // different dialog — nothing is corrupt and there is no backup to restore, the world is simply
  // too big for this device, and the remedy is the Storage tab's existing "Convert to 64z".
  function buildTallWorld(worldName) {
    var UI = window.EdenUI;
    UI.ensureCSS();

    var scrim = UI.scrim({ id: 'eden-loaderror-backdrop' });
    scrim.style.zIndex = 'var(--eden-z-alert)';

    var win = UI.window({
      title: 'World needs more memory',
      variant: 'dialog',
      scrollbar: false,
      role: 'alertdialog',
    });
    var warn = UI.icon('alert-triangle', { title: 'Warning' });
    warn.style.width = 'calc(24 * var(--u))';
    warn.style.height = 'calc(24 * var(--u))';
    win.actions.appendChild(warn);

    var stack = UI.el('div', 'eden-stack');
    stack.appendChild(UI.el('p', 'eden-stack__text',
      '"' + worldName + '" is a 256-block-tall world and needs more memory than this device has.'));
    stack.appendChild(UI.el('p', 'eden-stack__text',
      'Open it on a device with more RAM, or use Settings → Storage → “Convert to 64z” ' +
      'to make a shorter copy that will load here.'));
    stack.appendChild(UI.button({
      size: 'md', tone: 'danger', icon: 'log-out', label: 'Back to menu', onClick: quitToMenu,
    }));
    win.content.appendChild(stack);

    scrim.appendChild(win.root);
    UI.bindButtonSounds(scrim);
    S.root = scrim;
    return scrim;
  }

  function build(worldName, reason) {
    if (reason === 'TALL_WORLD_LOW_MEM') return buildTallWorld(worldName);
    var UI = window.EdenUI;
    UI.ensureCSS();

    var scrim = UI.scrim({ id: 'eden-loaderror-backdrop' });
    // Above the settings/pause layer: this is the one dialog that must not be buried, and unlike
    // those two it has no dismiss path, so a scrim click deliberately does nothing.
    scrim.style.zIndex = 'var(--eden-z-alert)';

    var win = UI.window({
      title: 'World could not be loaded',
      variant: 'dialog',
      scrollbar: false,
      role: 'alertdialog',
    });
    // A warning glyph in the title bar's right slot — the one place this system uses an icon to
    // carry severity rather than to label an action.
    var warn = UI.icon('alert-triangle', { title: 'Warning' });
    warn.style.width = 'calc(24 * var(--u))';
    warn.style.height = 'calc(24 * var(--u))';
    win.actions.appendChild(warn);

    var stack = UI.el('div', 'eden-stack');
    stack.appendChild(UI.el('p', 'eden-stack__text',
      '"' + worldName + '" appears to be corrupted or was interrupted mid-save' +
      (reason ? ' (' + reason + ').' : '.')));
    var p2 = UI.el('p', 'eden-stack__text', 'A backup from the previous save may still be available.');
    p2.id = 'eden-loaderror-msg';
    // The restore button can rewrite this line in place; announce that change rather than letting
    // it appear silently for a screen-reader user.
    p2.setAttribute('role', 'status');
    p2.setAttribute('aria-live', 'polite');
    stack.appendChild(p2);
    stack.appendChild(UI.button({
      size: 'md', tone: 'positive', icon: 'rotate-ccw',
      label: 'Restore previous save', onClick: restoreAndRetry,
    }));
    stack.appendChild(UI.button({
      size: 'md', tone: 'danger', icon: 'log-out', label: 'Back to menu', onClick: quitToMenu,
    }));
    win.content.appendChild(stack);

    scrim.appendChild(win.root);
    UI.bindButtonSounds(scrim);
    S.root = scrim;
    return scrim;
  }

  function show(worldName, reason) {
    if (S.root) return;
    document.body.appendChild(build(worldName, reason));
    S.releaseFocus = window.EdenUI.trapFocus(S.root);
    if (document.pointerLockElement) document.exitPointerLock();
  }

  // Polled once per frame from eden-st.html's rAF loop, same pattern as EdenPauseMenu.tick(). Once
  // shown, this dialog stays up until the user acts — there is nothing to "un-fail" on its own.
  function tick() {
    if (!ready() || S.open) return;
    if (M()._eden_load_failed() !== 0) {
      S.open = true;
      var worldName = utf8(M()._eden_load_failed_world());
      var reason = utf8(M()._eden_load_failed_reason());
      show(worldName, reason);
    }
  }

  // Audit row A7: a second, lighter-weight dialog for "your save may not have persisted" (quota
  // exceeded / any other syncfs error), called from eden-storage.js. Unlike the load-failure
  // dialog above this is a warning, not a fatal state — the game keeps running underneath, so it
  // gets its own non-alertdialog role and a dismiss button instead of forcing a reload.
  var Q = { open: false, root: null, releaseFocus: null };

  function dismissStorageWarning() {
    if (!Q.root) return;
    if (Q.releaseFocus) Q.releaseFocus();
    Q.root.remove();
    Q.root = null;
    Q.open = false;
  }

  function showStorageWarning(message) {
    if (Q.root) return; // one at a time; reportSyncError/checkQuotaAndWarn already dedupe upstream
    var UI = window.EdenUI;
    UI.ensureCSS();

    var scrim = UI.scrim({ id: 'eden-storagewarn-backdrop' });
    scrim.style.zIndex = 'var(--eden-z-alert)';
    // Dismissible (unlike the load-failure dialog): this is a warning the player can act on later,
    // not a state the engine is stuck in.
    scrim.addEventListener('mousedown', function (e) {
      if (e.target === scrim) dismissStorageWarning();
    });

    var win = UI.window({
      title: 'Storage warning',
      variant: 'dialog',
      scrollbar: false,
      role: 'alertdialog',
    });
    var warn = UI.icon('alert-triangle', { title: 'Warning' });
    warn.style.width = 'calc(24 * var(--u))';
    warn.style.height = 'calc(24 * var(--u))';
    win.actions.appendChild(warn);

    var stack = UI.el('div', 'eden-stack');
    stack.appendChild(UI.el('p', 'eden-stack__text', message));
    stack.appendChild(UI.button({
      size: 'md', tone: 'positive', label: 'Dismiss', onClick: dismissStorageWarning,
    }));
    win.content.appendChild(stack);

    scrim.appendChild(win.root);
    UI.bindButtonSounds(scrim);
    document.body.appendChild(scrim);
    Q.root = scrim;
    Q.open = true;
    Q.releaseFocus = window.EdenUI.trapFocus(scrim);
  }

  window.EdenLoadError = {
    tick: tick,
    isOpen: function () { return S.open; },
    showStorageWarning: showStorageWarning
  };
})();
