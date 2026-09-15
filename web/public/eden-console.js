// eden-console.js — project-audit-2026-07-30 row F5: a dev console (teleport/spawn/world-stats),
// requested from play rather than analysis (audit rows 31/33). The three wasm exports it calls
// (web/src/seam/DevConsole_web.mm) only exist in an EDEN_DIAGNOSTICS build — this file
// feature-detects them rather than checking a build flag, so it silently does nothing on a
// shipped build without either side needing to know the other's CMake configuration.
//
// Same shape as eden-loaderror.js: no engine state of its own, built from the shared design
// system, self-contained. Unlike the pause menu/settings panel, nothing here mirrors a per-frame
// engine flag, so there is no tick() to wire into eden-st.html's rAF loop — this is purely a
// keypress-toggled overlay.
//
// Requires: window.EdenUI. Publishes: window.EdenConsole. See docs/ui.md's dependency graph
// (audit I2).
(function () {
  'use strict';

  var S = { open: false, root: null, releaseFocus: null, input: null, log: null };

  function M() { return window.Module; }
  function available() {
    return window.__edenModuleReady && M() && typeof M()._eden_console_teleport === 'function';
  }

  // Same static-buffer C-string convention as eden-loaderror.js's utf8()/eden-settings.js's.
  function utf8(ptr) {
    var H = M().HEAPU8, end = ptr;
    while (H[end]) end++;
    // The Uint8Array copy is load-bearing in the EDEN_THREADED build (shared memory cannot be
    // handed to TextDecoder) — full reasoning on the canonical copy in eden-settings.js.
    return new TextDecoder().decode(new Uint8Array(H.subarray(ptr, end)));
  }

  function appendLine(text, isError) {
    var line = window.EdenUI.el('div', 'eden-stack__text', text);
    if (isError) line.style.color = 'var(--eden-danger, #d33)';
    S.log.appendChild(line);
    S.log.scrollTop = S.log.scrollHeight;
  }

  // Command grammar is deliberately tiny — this is a debugging tool, not a scripting language.
  //   tp <x> <y> <z>   — teleport, Vector convention (y is UP; NOT Terrain's x,z,y order)
  //   spawn <type>     — spawns a creature; <type> is the raw TYPE_*/M_* engine constant from
  //                      Classes/Constants.h (no name table exposed to JS yet — see audit row I4)
  //   stats            — player pos, chunk offset, active creature count, game mode
  //   help             — this list
  function runCommand(raw) {
    var line = raw.trim();
    if (!line) return;
    appendLine('> ' + line);
    var parts = line.split(/\s+/);
    var cmd = parts[0].toLowerCase();
    if (cmd === 'help') {
      appendLine('tp <x> <y> <z>          teleport (y is up)');
      appendLine('spawn <type>            spawn a creature at your position (numeric TYPE_*/M_* id)');
      appendLine('setblock <x> <z> <y> <t> set a block (Terrain arg order — y is still up, but last)');
      appendLine('stats                   world/player snapshot');
      appendLine('mem                     wasm heap use vs the -sMAXIMUM_MEMORY cap');
      return;
    }
    if (cmd === 'tp') {
      var x = parseFloat(parts[1]), y = parseFloat(parts[2]), z = parseFloat(parts[3]);
      if ([x, y, z].some(function (n) { return !isFinite(n); })) {
        appendLine('usage: tp <x> <y> <z>', true);
        return;
      }
      M()._eden_console_teleport(x, y, z);
      appendLine('teleported to (' + x + ', ' + y + ', ' + z + ')');
      return;
    }
    if (cmd === 'spawn') {
      var type = parseInt(parts[1], 10);
      if (!isFinite(type)) { appendLine('usage: spawn <type>', true); return; }
      var ok = M()._eden_console_spawn(type) !== 0;
      appendLine(ok ? ('spawned type ' + type) : 'spawn failed (bad type, or no free creature slot)', !ok);
      return;
    }
    if (cmd === 'setblock') {
      var bx = parseInt(parts[1], 10), bz = parseInt(parts[2], 10),
          by = parseInt(parts[3], 10), bt = parseInt(parts[4], 10);
      if ([bx, bz, by, bt].some(function (n) { return !isFinite(n); })) {
        appendLine('usage: setblock <x> <z> <y> <type>', true);
        return;
      }
      var setOk = M()._eden_console_setblock(bx, bz, by, bt) !== 0;
      appendLine(setOk ? ('set (' + bx + ',' + bz + ',' + by + ') = type ' + bt) : 'setblock failed', !setOk);
      return;
    }
    if (cmd === 'stats') {
      appendLine(utf8(M()._eden_console_world_stats()));
      return;
    }
    // ROADMAP Phase M / M1 step 5: the cap is a hard abort, so it needs a surface a player or a
    // bug report can read on demand, not only the one-shot 80%/90% warnings the frame loop emits.
    if (cmd === 'mem') {
      var h = JSON.parse(utf8(M()._eden_debug_heap()));
      var mb = function (n) { return (n / 1048576).toFixed(1) + ' MB'; };
      appendLine('heap in use   ' + mb(h.sbrkTop) + '  (peak this session ' + mb(h.peakSbrkTop) + ')');
      appendLine('heap reserved ' + mb(h.heapSize) + '  of a ' + mb(h.heapMax) + ' cap  ->  ' + h.usedPct + '% used');
      appendLine('note: heap use grows ~22 MB per world load and is not reclaimed (Phase M / M6).');
      return;
    }
    appendLine('unknown command: ' + cmd + ' (try "help")', true);
  }

  function build() {
    var UI = window.EdenUI;
    UI.ensureCSS();

    var scrim = UI.scrim({ id: 'eden-console-backdrop' });
    var win = UI.window({ title: 'Dev console', variant: 'dialog', scrollbar: false });

    var stack = UI.el('div', 'eden-stack');
    var log = UI.el('div', 'eden-stack');
    log.style.cssText = 'max-height:calc(220 * var(--u));overflow-y:auto;font-family:monospace;' +
      'font-size:calc(13 * var(--u));white-space:pre-wrap;';
    stack.appendChild(log);

    var input = UI.el('input', 'eden-field');
    input.type = 'text';
    input.placeholder = 'help / tp x y z / spawn <type> / setblock x z y t / stats / mem';
    input.autocomplete = 'off';
    input.spellcheck = false;
    input.addEventListener('keydown', function (e) {
      e.stopPropagation();  // do not let Enter/Escape/letters reach eden-st.html's game handlers
      if (e.key === 'Enter') {
        runCommand(input.value);
        input.value = '';
      } else if (e.key === 'Escape') {
        hide();
      }
    });
    stack.appendChild(input);

    win.content.appendChild(stack);
    scrim.appendChild(win.root);
    S.root = scrim;
    S.input = input;
    S.log = log;
    return scrim;
  }

  function show() {
    if (S.open) return;
    if (!S.root) document.body.appendChild(build());
    else document.body.appendChild(S.root);
    S.open = true;
    S.releaseFocus = window.EdenUI.trapFocus(S.root);
    if (document.pointerLockElement) document.exitPointerLock();
    S.input.focus();
    appendLine('type "help" for commands');
  }

  function hide() {
    if (!S.open) return;
    S.open = false;
    if (S.releaseFocus) { S.releaseFocus(); S.releaseFocus = null; }
    if (S.root && S.root.parentNode) S.root.parentNode.removeChild(S.root);
  }

  window.addEventListener('keydown', function (e) {
    if (e.code !== 'Backquote') return;
    if (S.open) return;  // the console's own input's keydown handler owns Backquote while open
    if (!available()) return;
    // Same guard eden-st.html's own action-key handler uses: a focused text field elsewhere
    // (e.g. the New World name box) must get its own keystrokes, not have this steal Backquote.
    var el = document.activeElement;
    if (el && (el.isContentEditable || el.tagName === 'INPUT' || el.tagName === 'TEXTAREA')) return;
    e.preventDefault();
    show();
  });

  window.EdenConsole = { show: show, hide: hide, isOpen: function () { return S.open; }, available: available };
})();
