// eden-settings.js — the port's settings panel (pass 28; restyled onto the Eden: Community
// Edition design system).
// Requires: window.EdenUI, window.EdenStorage, window.EdenKeybinds, window.EdenConsole
// (feature-detected only), Module._eden_settings_schema. Publishes: window.EdenSettings. See
// docs/ui.md's dependency graph (audit I2).
//
// Replaces the engine's GL settings screen (Classes/SettingsMenu.mm, whose update()/render() are
// --wrap'd to no-ops — see web/src/seam/Settings_web.mm for why the data half of that class is
// kept and only the UI is replaced).
//
// It is deliberately a small, self-contained DOM layer with ONE data source: the C schema returned
// by `Module._eden_settings_schema()`. Nothing here hard-codes a setting — adding a row is a line
// in `kSettings[]` on the C side and nothing at all here. That is the property the old GL panel
// lacked (every row there was a hand-placed texture plus a hard-coded `if` in two functions).
//
// WHAT CHANGED IN THE DESIGN-SYSTEM PASS. The panel used to be a "Dark Glass Chrome" card with a
// HORIZONTAL scrolling text tab strip and ~200 lines of CSS injected from this file as a JS
// string. It is now the mockups' Gameplay Settings screen: a beveled window with a VERTICAL ICON
// TAB RAIL on the left, a flat white content box, and the chunky scrollbar on the right. All the
// styling moved to public/eden-ui.css and all the element construction to public/eden-ui.js —
// this file is now only "read the C schema, build rows, write back".
//
// Everything about the DATA path is unchanged: same schema call, same _eden_settings_get/set,
// same toggleByKey, same engine-menu polling in tick(), same sounds, same onChange listeners.
//
// TOUCH IS A FIRST-CLASS TARGET, not an afterthought. The design system handles this now — see
// eden-ui.css's "Density + accessibility overrides": `--u` scales the whole UI with the viewport
// and a `pointer: coarse` block raises hit boxes to the 44px platform floor where the scale
// bottoms out. The panel still scrolls internally rather than depending on page scroll, and is
// still a plain overlay ABOVE the canvas, so no touch reaches the engine while it is up.
(function () {
  'use strict';

  var S = {
    open: false,
    schema: null,
    group: null,
    root: null,
    win: null,          // the EdenUI.window() bundle: {root, title, rail, content, ...}
    releaseFocus: null,
    fromEngineMenu: false   // true when the engine's own Options button opened us
  };

  // Q2 (dazzling-munching-bengio.md perf audit): a way for the host page to react to a specific
  // setting changing (e.g. display_mode) without polling every rAF frame. Fired after every write
  // this panel makes to the C settings model, keyed by the setting's string key.
  var changeListeners = [];
  function notifyChange(key) {
    for (var i = 0; i < changeListeners.length; i++) changeListeners[i](key);
  }

  function M() { return window.Module; }
  function ready() {
    return window.__edenModuleReady && M() && typeof M()._eden_settings_schema === 'function';
  }

  // THE CANONICAL COPY of this helper — eden-menu.js, eden-storage.js, eden-loaderror.js and
  // eden-console.js each carry an identical one and point back here for the reasoning.
  //
  // `new Uint8Array(...)` around the subarray is NOT redundant, and removing it breaks the
  // threaded build in a way that looks nothing like a string bug (audit row 36/C1, pass 63).
  // In an EDEN_THREADED build the wasm memory is a SharedArrayBuffer, so `HEAPU8` is a view onto
  // shared memory — and `TextDecoder.decode()` REFUSES those outright:
  //     TypeError: Failed to execute 'decode' on 'TextDecoder':
  //               The provided ArrayBufferView value must not be shared.
  // (The spec forbids it because the decoder could otherwise observe another thread mutating the
  // bytes mid-decode.) The constructor copies the bytes into a fresh, non-shared buffer first,
  // which is both allowed and what we want anyway.
  //
  // WHY THIS MATTERED SO MUCH MORE THAN IT LOOKS: the throw happened inside `getSetting`, which
  // eden-gamepad.js polls from `trackCursorNeed`, which eden_main.cpp calls at the tail of EVERY
  // `eden_frame_tick` via `Module.__edenFramePost`. So the exception unwound out of the engine's
  // own frame callback and the main loop stopped after tick 0 — presenting as a black canvas with
  // a live WebGL2 context and a healthy-looking DOM, i.e. exactly the failure signature
  // web/CLAUDE.md warns is "a renderer bug and is not one". Found by counting engine frames.
  //
  // Cost in the single-threaded build: one small copy per C-string read. These are UI-rate reads
  // of short strings (setting keys, world names), not per-vertex work — kept unconditional so
  // both builds run the same code path rather than diverging on a `crossOriginIsolated` check.
  function utf8(ptr) {
    var H = M().HEAPU8, end = ptr;
    while (H[end]) end++;
    return new TextDecoder().decode(new Uint8Array(H.subarray(ptr, end)));
  }

  // The schema is the key -> index map, so nothing here ever passes a string into wasm (which
  // would mean putting _malloc/_free on the export list). Loaded lazily rather than at open(),
  // because the B/F shortcuts can fire before the panel has ever been shown.
  function ensureSchema() {
    if (S.schema || !ready()) return S.schema;
    M()._eden_settings_init();
    S.schema = JSON.parse(utf8(M()._eden_settings_schema()));
    return S.schema;
  }

  function indexOf(key) {
    var sc = ensureSchema();
    if (!sc) return -1;
    for (var i = 0; i < sc.length; i++) if (sc[i].key === key) return i;
    return -1;
  }

  // ---------------------------------------------------------------------------------------------
  // Tab icons.
  //
  // The mockup's rail is seven SF Symbols glyphs; the design system substituted the open Lucide
  // set (see eden-icons.js). The port's group list is not identical to the mockup's — it has no
  // "Touch" group and does have JS-owned "Keys" and "Storage" tabs — so this maps by MEANING
  // rather than by rail position. A group with no entry here still gets a tab, drawn with the
  // generic gear, so adding a group to kSettings[] never breaks this screen.
  // ---------------------------------------------------------------------------------------------
  var GROUP_ICONS = {
    Gameplay: 'wrench',
    Audio: 'volume-2',
    Controls: 'gamepad-2',
    Video: 'monitor',
    Interface: 'sliders-horizontal',
    Experiments: 'flask-conical',
    Keys: 'keyboard',
    Storage: 'hard-drive',
    Reset: 'rotate-ccw',
  };
  function iconForGroup(g) { return GROUP_ICONS[g] || 'settings'; }

  function injectCSS() { window.EdenUI.ensureCSS(); }

  // ---------------------------------------------------------------------------------------------
  // Rows
  // ---------------------------------------------------------------------------------------------
  function formatValue(item, v) {
    if (item.key === 'fov') return Math.round(v) + '°';
    if (item.max === 1 && item.step < 1) return Math.round(v * 100) + '%';
    return (Math.round(v * 100) / 100).toFixed(2) + '×';
  }

  // eden_settings_schema() (Settings_web.mm) emits KIND_ENUM's `options` as a plain
  // comma-separated string (no nested-array JSON encoding needed on the C side for a handful of
  // short labels) — split it here, once per row.
  function enumOptions(item) {
    return item.options ? item.options.split(',') : [];
  }

  function makeRow(item) {
    var UI = window.EdenUI;
    var current = M()._eden_settings_get(item.i);
    var ctl;

    if (item.kind === 0) {
      ctl = UI.toggle({
        checked: current !== 0,
        ariaLabel: item.label,
        onChange: function (on) {
          M()._eden_settings_set(item.i, on ? 1 : 0);
          // The one control menu_button_press/release_01.mp3 is reserved for (see
          // eden_play_switch_toggle_sound's header comment, Settings_web.mm) — deliberately NOT
          // eden_play_menu_button_sound, and this is the only call site for that sound.
          if (ready()) M()._eden_play_switch_toggle_sound(on ? 1 : 0);
          notifyChange(item.key);
        },
      });
    } else if (item.kind === 2) {
      // Segmented control. In this design system "selected" and "pressed" are the same visual, so
      // a segment is literally a row of buttons with aria-pressed — no new component needed.
      ctl = UI.el('div', 'eden-seg');
      ctl.setAttribute('role', 'radiogroup');
      ctl.setAttribute('aria-label', item.label);
      enumOptions(item).forEach(function (label, idx) {
        var b = UI.button({
          size: 'sm', label: label,
          onClick: function () {
            M()._eden_settings_set(item.i, idx);
            var kids = ctl.children;
            for (var k = 0; k < kids.length; k++) {
              kids[k].setAttribute('aria-checked', k === idx ? 'true' : 'false');
              kids[k].classList.toggle('is-active', k === idx);
            }
            notifyChange(item.key);
          },
        });
        b.setAttribute('role', 'radio');
        b.setAttribute('aria-checked', idx === current ? 'true' : 'false');
        if (idx === current) b.classList.add('is-active');
        ctl.appendChild(b);
      });
    } else {
      ctl = UI.el('div', 'eden-listrow__actions');
      var range = UI.el('input', 'eden-slider');
      range.type = 'range';
      range.min = item.min; range.max = item.max; range.step = item.step;
      range.value = current;
      range.setAttribute('aria-label', item.label);
      var out = UI.el('span', 'eden-value', formatValue(item, current));
      // `input` (not `change`): apply live so FOV and sensitivity can be judged while dragging,
      // which is the whole reason they are sliders. Each write also persists, which is cheap here
      // (one localStorage set) and means a drag that ends off-screen is not lost.
      range.addEventListener('input', function () {
        var v = parseFloat(range.value);
        M()._eden_settings_set(item.i, v);
        out.textContent = formatValue(item, v);
        notifyChange(item.key);
      });
      ctl.appendChild(range);
      ctl.appendChild(out);
    }

    return window.EdenUI.listRow({
      title: item.label,
      desc: item.hint || null,
      actions: ctl,
    });
  }

  function groupsOf(schema) {
    var seen = [], out = [];
    for (var i = 0; i < schema.length; i++) {
      if (seen.indexOf(schema[i].group) === -1) { seen.push(schema[i].group); out.push(schema[i].group); }
    }
    return out;
  }

  function allGroups() {
    var gs = groupsOf(S.schema);
    gs.push('Keys');      // not schema-driven — see renderKeysBody (Phase 5, JS-owned keybinds)
    gs.push('Storage');   // not schema-driven — see renderStorageBody
    gs.push('Reset');     // not schema-driven — see renderResetBody (audit row 20/G2)
    return gs;
  }

  function renderBody() {
    var content = S.win.content;
    content.textContent = '';
    var pad = window.EdenUI.el('div', 'eden-content__pad');
    content.appendChild(pad);

    if (S.group === 'Storage') {
      renderStorageBody(pad);
    } else if (S.group === 'Keys') {
      renderKeysBody(pad);
    } else if (S.group === 'Reset') {
      renderResetBody(pad);
    } else {
      for (var i = 0; i < S.schema.length; i++) {
        if (S.schema[i].group !== S.group) continue;
        pad.appendChild(makeRow(S.schema[i]));
      }
    }

    // The mockup titles the window after the active tab ("Gameplay Settings"), which doubles as
    // the label for a rail of otherwise-unlabelled icons.
    S.win.title.textContent = S.group + ' Settings';
    S.win.root.setAttribute('aria-label', S.group + ' Settings');

    var kids = S.win.rail.children;
    for (var k = 0; k < kids.length; k++) {
      kids[k].setAttribute('aria-selected', kids[k].dataset.group === S.group ? 'true' : 'false');
    }
    window.EdenUI.syncRailTabIndex(S.win.rail);
    content.scrollTop = 0;
    if (S.win.scrollbar && S.win.scrollbar.sync) requestAnimationFrame(S.win.scrollbar.sync);
  }

  function renderTabs() {
    var UI = window.EdenUI;
    S.win.rail.textContent = '';
    allGroups().forEach(function (g) {
      var t = UI.button({
        size: 'tab', icon: iconForGroup(g), ariaLabel: g, title: g,
        onClick: function () { S.group = g; renderBody(); },
      });
      t.setAttribute('role', 'tab');
      t.dataset.group = g;
      S.win.rail.appendChild(t);
    });
    UI.railKeyNav(S.win.rail, function (index) {
      S.group = allGroups()[index];
      renderBody();
    });
  }

  // ---------------------------------------------------------------------------------------------
  // Keys tab (Phase 5, PC controls audit) — not schema-driven, unlike every other tab: the
  // key->action map is JS-owned localStorage state (window.EdenKeybinds, eden-st.html), NOT part
  // of the C settings model — see that file's header for why. Only the PRIMARY binding per action
  // is rebindable here; secondary bindings (arrow keys) are fixed.
  // ---------------------------------------------------------------------------------------------
  function keyLabel(code) {
    if (!code) return '—';
    if (code.indexOf('Key') === 0) return code.slice(3);
    if (code.indexOf('Digit') === 0) return code.slice(5);
    if (code.indexOf('Arrow') === 0) return code.slice(5);
    return { ShiftLeft: 'Shift', ShiftRight: 'R Shift', AltLeft: 'Alt', AltRight: 'R Alt',
      ControlLeft: 'Ctrl', ControlRight: 'R Ctrl', Space: 'Space', Escape: 'Esc' }[code] || code;
  }

  function renderKeysBody(pad) {
    var UI = window.EdenUI;
    var KB = window.EdenKeybinds;
    if (!KB) {
      pad.appendChild(UI.listRow({ title: 'Keybinds unavailable' }));
      return;
    }
    // Audit row G1: this tab is the only controls reference in the game, and pointer lock has no
    // other affordance telling a first-time desktop player it exists.
    pad.appendChild(UI.el('div', 'eden-section__desc',
      'Click the game world to look around with the mouse. Press Esc to release it.'));
    // Audit row F5 follow-up: the dev console has no other discoverability affordance — it's a
    // hardcoded key, not part of the KB.actions rebind table below, and its own feature-detect
    // means it's silently absent on a non-diagnostics build, so only mention it when it's real.
    if (window.EdenConsole && window.EdenConsole.available()) {
      pad.appendChild(UI.el('div', 'eden-section__desc',
        'Press ` (backtick) to open the dev console.'));
    }
    KB.actions.forEach(function (action) {
      var btn = UI.button({ size: 'sm', label: keyLabel(KB.primaryCode(action)) });
      btn.addEventListener('click', function () {
        btn.textContent = 'Press a key…';
        btn.classList.add('is-active');
        KB.startCapture(function (code) {
          KB.rebind(action, code);
          btn.textContent = keyLabel(code);
          btn.classList.remove('is-active');
        });
      });
      btn.setAttribute('aria-label', KB.labelFor(action) + ': ' + keyLabel(KB.primaryCode(action)) +
        '. Activate to rebind.');
      pad.appendChild(UI.listRow({ title: KB.labelFor(action), actions: btn }));
    });

    var reset = UI.button({
      size: 'sm', label: 'Reset to defaults',
      onClick: function () { KB.resetDefaults(); pad.textContent = ''; renderKeysBody(pad); },
    });
    var resetWrap = UI.el('div', 'eden-section__body');
    resetWrap.appendChild(reset);
    pad.appendChild(resetWrap);
  }

  // ---------------------------------------------------------------------------------------------
  // Reset tab (audit row 20/G2 — "unify settings + keybinds"). The storage split (C table for
  // engine-backed rows, JS localStorage blob for keybinds — see the Keys tab's own header comment
  // for why that split is real and staying) was never the complaint; the missing SHARED reset was.
  // The Keys tab already had its own local reset, every other row had none at all, so "Reset
  // settings" meant something different depending which tab you were on. One button here resets
  // both halves together, in the same unified shell every other tab already lives in.
  // ---------------------------------------------------------------------------------------------
  function renderResetBody(pad) {
    var UI = window.EdenUI;
    pad.appendChild(UI.el('div', 'eden-section__desc',
      'Resets every setting on every tab, including key bindings, back to defaults. ' +
      'Saved worlds are not affected.'));
    var confirming = false, confirmTimer = null;
    var btn = UI.button({
      size: 'sm', label: 'Reset all settings & keybinds',
      onClick: function () {
        if (!confirming) {
          confirming = true;
          btn.textContent = 'Confirm reset?';
          btn.classList.add('eden-btn--danger');
          confirmTimer = setTimeout(function () {
            confirming = false;
            btn.textContent = 'Reset all settings & keybinds';
            btn.classList.remove('eden-btn--danger');
          }, 4000);
          return;
        }
        clearTimeout(confirmTimer);
        if (ready()) M()._eden_settings_reset_all();
        if (window.EdenKeybinds) window.EdenKeybinds.resetDefaults();
        pad.textContent = '';
        renderResetBody(pad);
      },
    });
    var wrap = UI.el('div', 'eden-section__body');
    wrap.appendChild(btn);
    pad.appendChild(wrap);
  }

  // ---------------------------------------------------------------------------------------------
  // Storage tab (pass 29) — not schema-driven. Lists every world in Documents (the REAL save
  // directory FileManager uses — docs/save-load.md) with its size and last-modified time, plus
  // whether this device is actually persisting them (window.EdenStorage — public/eden-storage.js,
  // src/seam/Storage_web.mm), and lets the player delete one to reclaim space.
  // ---------------------------------------------------------------------------------------------
  function renderStorageBody(pad) {
    var UI = window.EdenUI;
    var ES = window.EdenStorage;
    var worlds = ES ? ES.listWorlds() : [];

    var totalBytes = worlds.reduce(function (n, w) { return n + (w.bytes || 0); }, 0);
    var persistText;
    if (!ES) {
      persistText = 'Storage info unavailable.';
    } else if (ES.isPersistent()) {
      persistText = 'Saved locally in this browser. Worlds survive a reload but live only on ' +
        'this device — clearing site data removes them.';
    } else if (ES.backend && ES.backend() === 'readonly') {
      // ROADMAP C2: the OPFS sync access handle is an exclusive lock, so a second tab can read
      // these worlds but must not write them. Say so here as well as in the one-shot dialog —
      // this panel is where a player goes to ask "are my worlds safe".
      persistText = 'Eden is open in another tab, so this tab is read-only: your worlds are ' +
        'listed and playable here, but changes made in THIS tab will not be saved. Close the ' +
        'other tab and reload to play normally.';
    } else {
      persistText = 'Not persistent this session. Worlds will be lost on reload (no browser ' +
        'storage available — e.g. private browsing on some browsers).';
    }
    var summary = UI.section({ title: 'This device', desc: persistText });
    var totals = UI.el('div', 'eden-listrow__sub',
      worlds.length + (worlds.length === 1 ? ' world, ' : ' worlds, ') +
      (ES ? ES.formatBytes(totalBytes) : totalBytes + ' B') + ' total.');
    summary.appendChild(totals);
    var quotaLine = UI.el('div', 'eden-listrow__sub');
    summary.appendChild(quotaLine);
    pad.appendChild(summary);

    // Row #18 (perf-audit §6): import a .eden file from disk (button, or drag-and-drop anywhere
    // on this tab) so it shows up in the world picker without going through the engine's own
    // save/share path. No format validation on the way in — a bad file just fails to load like
    // any other corrupt save (docs/eden-file-format.md is the source of truth if that ever
    // matters) — but this is the documented, supported way to move a world in, paired with the
    // per-world "Export" button below for moving one out.
    if (ES) {
      var runImport = function (f) {
        if (!f) return;
        importBtn.disabled = true;
        importBtn.textContent = 'Importing…';
        ES.importFile(f, function (ok, err) {
          importBtn.disabled = false;
          importBtn.textContent = 'Choose file…';
          if (!ok) { window.alert('Import failed: ' + err); return; }
          pad.textContent = '';
          renderStorageBody(pad);
        });
      };
      var importBtn = UI.button({ size: 'sm', label: 'Choose file…' });
      var fileInput = UI.el('input');
      fileInput.type = 'file';
      fileInput.accept = '.eden,.gz';
      fileInput.style.display = 'none';
      fileInput.addEventListener('change', function () {
        var f = fileInput.files && fileInput.files[0];
        fileInput.value = '';
        runImport(f);
      });
      importBtn.addEventListener('click', function () { fileInput.click(); });
      var importRow = UI.listRow({
        title: 'Import .eden file',
        sub: 'Choose a .eden or .eden.gz file, or drag and drop one anywhere on this tab.',
        actions: importBtn,
      });
      importRow.appendChild(fileInput);
      pad.appendChild(importRow);

      // Drag-and-drop on the whole tab body, not just the row — a bigger, more forgiving target.
      // dragover must preventDefault or the browser's own "open this file" navigation wins.
      pad.addEventListener('dragover', function (ev) { ev.preventDefault(); });
      pad.addEventListener('drop', function (ev) {
        ev.preventDefault();
        var f = ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files[0];
        runImport(f);
      });
    }
    if (ES) {
      ES.estimateQuota(function (est) {
        if (!est || !est.quota || !document.contains(quotaLine)) return;
        var pct = est.usage / est.quota * 100;
        quotaLine.textContent = 'This site is using ' + ES.formatBytes(est.usage) + ' of ' +
          ES.formatBytes(est.quota) + ' available on this device (' + pct.toFixed(1) + '%).';
      });
    }

    if (worlds.length === 0) {
      pad.appendChild(UI.listRow({ title: 'No saved worlds yet' }));
      return;
    }

    worlds.forEach(function (w, index) {
      var exportBtn = UI.button({ size: 'sm', label: 'Export' });
      exportBtn.addEventListener('click', function () {
        if (!ES || !ES.exportWorldAt(index)) window.alert('Export failed.');
      });

      var del = UI.button({ size: 'sm', label: 'Delete' });
      var confirming = false, resetTimer = null;
      del.addEventListener('click', function () {
        if (!confirming) {
          confirming = true;
          del.textContent = 'Confirm?';
          del.classList.add('eden-btn--danger');
          resetTimer = setTimeout(function () {
            confirming = false;
            del.textContent = 'Delete';
            del.classList.remove('eden-btn--danger');
          }, 4000);
          return;
        }
        clearTimeout(resetTimer);
        if (ES && ES.deleteWorldAt(index)) { pad.textContent = ''; renderStorageBody(pad); }
      });

      // 256z Stage 3 item 5: the space-reclaim action, only offered for a 256z world (converting
      // a 64z world is a no-op the engine would refuse anyway). Destructive -- discards anything
      // above y=63, so it gets the same two-click confirm as Delete, then shows the FULL report
      // (blocks discarded, creatures dropped, etc.) rather than a bare success toast, because a
      // silent "it worked" would hide exactly the information this warning exists to surface.
      var convertBtn = null;
      if (w.height === 256) {
        convertBtn = UI.button({ size: 'sm', label: 'Convert to 64z' });
        var convertConfirming = false, convertResetTimer = null;
        convertBtn.addEventListener('click', function () {
          if (!convertConfirming) {
            convertConfirming = true;
            convertBtn.textContent = 'Confirm?';
            convertBtn.classList.add('eden-btn--danger');
            convertResetTimer = setTimeout(function () {
              convertConfirming = false;
              convertBtn.textContent = 'Convert to 64z';
              convertBtn.classList.remove('eden-btn--danger');
            }, 4000);
            return;
          }
          clearTimeout(convertResetTimer);
          convertBtn.disabled = true;
          convertBtn.textContent = 'Converting…';
          var r = ES ? ES.convertTo64zAt(index) : { ok: false, error: 'not available' };
          if (!r.ok) {
            window.alert('Convert failed: ' + (r.error || 'unknown error'));
            pad.textContent = '';
            renderStorageBody(pad);
            return;
          }
          var lines = [r.columns + ' columns converted.'];
          if (r.blocksDiscarded) lines.push(r.blocksDiscarded + ' blocks above y=63 discarded (' + r.columnsAffected + ' columns affected).');
          if (r.doorsOrphaned) lines.push(r.doorsOrphaned + ' door/portal halves at the cut cleared.');
          if (r.creaturesDropped) lines.push(r.creaturesDropped + ' creatures above y=63 dropped.');
          if (r.creaturesRelocated) lines.push(r.creaturesRelocated + ' creatures relocated to a lower slot.');
          if (r.creaturesOverflow) lines.push(r.creaturesOverflow + ' creatures could not be relocated (no free slot).');
          if (r.posClamped) lines.push('Player position clamped into the 64z ceiling.');
          if (r.homeClamped) lines.push('Home point clamped into the 64z ceiling.');
          window.alert('Converted to 64z.\n\n' + lines.join('\n'));
          pad.textContent = '';
          renderStorageBody(pad);
        });
      }

      // w.height (Storage_web.mm) is 64 or 256 -- a 256z ("New Dawn") world costs roughly 4x the
      // memory of a 64z one once loaded (~413 MB of wasm heap vs. ~128 MB, measured pass 67), which
      // is real risk on a memory-constrained device (the iOS Safari ceiling that produced audit row
      // A11), so it's called out here rather than only at the moment of loading it.
      pad.appendChild(UI.listRow({
        title: w.name,
        sub: (ES ? ES.formatBytes(w.bytes) : w.bytes + ' B') + ' · edited ' +
          (ES ? ES.formatDate(w.mtime) : w.mtime) +
          (w.height === 256 ? ' · 256z (uses much more memory to load)' : ''),
        actions: convertBtn ? [exportBtn, convertBtn, del] : [exportBtn, del],
      }));
    });
  }

  // ---------------------------------------------------------------------------------------------
  // Open / close
  // ---------------------------------------------------------------------------------------------
  function buildShell() {
    var UI = window.EdenUI;
    var scrim = UI.scrim({ id: 'eden-settings-backdrop', onDismiss: close });
    var win = UI.window({
      title: 'Settings',
      variant: 'narrow',
      rail: true,
      railLabel: 'Settings sections',
      onBack: close,
    });
    scrim.appendChild(win.root);

    // Pass 39 wired the engine's own S_MENU_BUTTON_PRESS/RELEASE into the GL HUD's menu icons
    // (Classes/Hud.mm) — this panel is pure DOM with no engine tap underneath any of its buttons,
    // so it never got that sound. Delegated on the scrim rather than per-button, so every current
    // AND future control gets it for free. Toggles are excluded: they own a DIFFERENT sound
    // (eden_play_switch_toggle_sound, fired in makeRow) and would otherwise play both.
    UI.bindButtonSounds(scrim, '.eden-toggle');

    S.root = scrim;
    S.win = win;
    return scrim;
  }

  // group: optional — jump straight to a tab (e.g. 'Keys') instead of remembering the last one
  // open. Added for the pause menu's "Controls" shortcut (audit row G1); every other caller keeps
  // passing nothing, which preserves the old "reopens on whatever tab you left" behaviour.
  function open(group) {
    if (S.open || !ready()) return;
    injectCSS();
    if (!ensureSchema()) return;
    if (group) S.group = group;
    else if (!S.group) S.group = S.schema.length ? S.schema[0].group : null;
    document.body.appendChild(buildShell());
    renderTabs();
    renderBody();
    S.open = true;
    S.releaseFocus = window.EdenUI.trapFocus(S.root);
    // A panel you point at cannot coexist with a locked pointer. eden-st.html's cursor loop only
    // re-grabs on a picker-close EDGE, so releasing here is not undone behind our back.
    if (document.pointerLockElement) document.exitPointerLock();
  }

  function close() {
    if (!S.open) return;
    if (S.releaseFocus) { S.releaseFocus(); S.releaseFocus = null; }
    if (S.root && S.root.parentNode) S.root.parentNode.removeChild(S.root);
    S.root = null;
    S.win = null;
    S.open = false;
    // If the engine's own Options button put us here, hand its state machine back its `false` —
    // that is what un-freezes Menu::update/render (both early-return while showsettings is set).
    if (S.fromEngineMenu && ready()) M()._eden_settings_menu_close();
    S.fromEngineMenu = false;
  }

  // ---------------------------------------------------------------------------------------------
  // Engine polling
  // ---------------------------------------------------------------------------------------------
  // Polled once per frame from eden-st.html's existing rAF loop. Two jobs: keep the C settings
  // model initialised (it needs the World to exist, which is later than module init), and mirror
  // the engine's own `showsettings` flag so the main menu's Options button opens this panel.
  function tick() {
    if (!ready()) return;
    M()._eden_settings_init();
    var engineWants = M()._eden_settings_menu_open() !== 0;
    if (engineWants && !S.open) { S.fromEngineMenu = true; open(); }
    if (!engineWants && S.open && S.fromEngineMenu) { close(); }
  }

  // Keyboard shortcuts route THROUGH the model rather than keeping their own copy of the state,
  // so the key and the panel can never disagree.
  function toggleByKey(key) {
    if (!ready()) return null;
    var i = indexOf(key);
    if (i < 0) return null;
    var v = M()._eden_settings_toggle(i);
    // If the panel happens to be open on this row, redraw so the switch does not lie.
    if (S.open) renderBody();
    notifyChange(key);
    return v;
  }

  /** Read one setting by key without opening the panel. Used by the DOM menu's legacy-GL check. */
  function getByKey(key) {
    if (!ready()) return null;
    var i = indexOf(key);
    if (i < 0) return null;
    return M()._eden_settings_get(i);
  }

  window.EdenSettings = {
    open: open,
    close: close,
    tick: tick,
    isOpen: function () { return S.open; },
    toggleByKey: toggleByKey,
    getByKey: getByKey,
    injectCSS: injectCSS,  // pass 30: kept as the shared "make sure the stylesheet exists" hook
    // Q2 perf audit: subscribe to setting writes instead of polling every rAF frame (e.g.
    // display_mode -> applyDisplayMode() in eden-st.html).
    onChange: function (fn) { changeListeners.push(fn); }
  };
})();
