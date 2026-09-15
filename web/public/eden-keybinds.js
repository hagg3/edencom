// eden-keybinds.js — Phase N Stage 5.2: this file used to OWN the ACTION -> physical-code(s) map
// (DEFAULT_KEYMAP, ACTION_LABELS, a private `eden.prefs.keybinds` localStorage blob) as a stated,
// deliberate exception to "settings live in C, never in the JS" — the reason given was that the C
// settings model stores floats only and NSUserDefaults persists NSNumber only, so a code->action
// map had nowhere to live on the C side.
//
// That reason was true of the representation it assumed (`event.code` STRINGS) and only of that
// one. A physical key already has a numeric name every target here agrees on — its USB HID usage
// ID, which is what SDL3 scancodes ARE and what `event.code` strings are a 1:1 renaming of — so a
// binding is an int below 512, a float holds it exactly, and NSNumber persists it. The C-side model
// now lives in `web/src/seam/Settings_web.mm`'s KEYBINDS section (read that file's header comment
// for the full design rationale); THIS file is just the bridge — the C API in one direction, the
// DOM rebind-capture protocol in the other, neither of which belongs on the other side.
//
// What stayed here, because it is a DOM concern and has no C-side equivalent: the capture-phase
// `keydown` listener (preventDefault/stopPropagation so a captured key never reaches the real
// action-dispatch listener in eden-input.js or types into a field), and codeToActions/
// MOMENTARY_ACTIONS/CONTINUOUS_ACTIONS as bare top-level bindings — eden-input.js reads those by
// name, not through `window.EdenKeybinds`, so they keep the exact shape it already consumes.
//
// Self-contained: nothing here depends on any other split file, so its position in eden-st.html's
// load order relative to eden-viewport.js/eden-hotbar.js does not matter — only that it loads
// before eden-input.js, which reads codeToActions/MOMENTARY_ACTIONS/CONTINUOUS_ACTIONS, and that
// both load after the Module bootstrap in eden-host.js (window.__edenModuleReady).
//
// TIMING: this file's top-level code runs at PAGE PARSE time, long before the wasm module has
// linked — every C call here is behind `ready()`, exactly like eden-settings.js's own guard, and
// every entry point degrades to a harmless no-op/empty-result rather than throwing when the module
// is not ready yet. This matters more here than almost anywhere else in the port: a `keydown` can
// arrive (and does, if a player mashes a key while the page is still booting) before Module exists,
// and an exception thrown on that path would escape into... nothing, in this file's own case,
// since the listener below is DOM-driven and not on the engine's frame callback — but
// codeToActions/MOMENTARY_ACTIONS/CONTINUOUS_ACTIONS are also read every frame from eden-input.js's
// own keydown/keyup handlers, which is exactly the shape of path that killed the engine's main loop
// once before (see eden-settings.js's `utf8()` comment for that incident). So: guard everything,
// same discipline, same reason.
'use strict';

// Legacy blob (pre-Stage-5.2): {action: ['KeyW', 'ArrowUp'], ...}, action -> event.code strings.
// Migrated once into the C model below, then left in place (not deleted) in case the migration
// ever needs to be re-run by hand — same posture as Settings_web.mm's own touch_joystick migration.
const OLD_KEYBIND_STORAGE_KEY = 'eden.prefs.keybinds';
const KEYBIND_MIGRATED_KEY = 'eden.prefs.keybinds.migrated';

function M() { return window.Module; }
function ready() {
  return window.__edenModuleReady && M() && typeof M()._eden_keybind_count === 'function';
}

// Same shape as eden-settings.js's canonical copy (see that file's header for why the
// `new Uint8Array(...)` copy is load-bearing under the threaded build) — duplicated rather than
// imported because there is no module system here, only script load order.
function utf8(ptr) {
  const H = M().HEAPU8;
  let end = ptr;
  while (H[end]) end++;
  return new TextDecoder().decode(new Uint8Array(H.subarray(ptr, end)));
}

// ---------------------------------------------------------------------------------------------
// Lazily-loaded, cached state. `actionMeta[i]` is index-aligned with the C table (schema rows are
// emitted in table order, i is the row's own index) and holds only the IMMUTABLE fields — action
// id, label, group, dispatch class. The LIVE binding (which code an action currently points at) is
// never cached: codeToActions()/primaryCode() ask C fresh every time, which is what lets a rebind
// take effect immediately with no cache-invalidation logic anywhere in this file.
// ---------------------------------------------------------------------------------------------
let codeByName = null;   // event.code string -> HID int, e.g. codeByName['KeyW'] === 26
let nameByCode = null;   // HID int -> event.code string (the inverse), for primaryCode()'s return
let actionMeta = null;   // [{action, label, group, continuous, momentary}, ...] by C index
let actionIndex = null;  // action id -> C index

const MOMENTARY_ACTIONS = new Set();
const CONTINUOUS_ACTIONS = new Set();

function ensureLoaded() {
  if (actionMeta) return actionMeta;
  if (!ready()) return null;

  // The code<->name dictionary. Read once — eden_keybind_code_table()'s own header comment on the
  // C side says as much: "it never changes at runtime."
  const table = JSON.parse(utf8(M()._eden_keybind_code_table()));
  codeByName = {};
  nameByCode = {};
  for (const row of table) { codeByName[row.n] = row.c; nameByCode[row.c] = row.n; }

  const schema = JSON.parse(utf8(M()._eden_keybinds_schema()));
  actionMeta = schema;
  actionIndex = {};
  for (const row of schema) {
    actionIndex[row.action] = row.i;
    if (row.momentary) MOMENTARY_ACTIONS.add(row.action);
    if (row.continuous) CONTINUOUS_ACTIONS.add(row.action);
  }

  migrateOldKeymapIfNeeded();
  return actionMeta;
}

// One-shot: translate slot 0 (the primary binding) of every action in the OLD localStorage blob
// through the code table into eden_keybind_set(), then remember it ran. Follows
// Settings_web.mm's eden_settings_init() "touch_joystick" migration block for shape: read the old
// key directly (its own row is gone from the live model), only act on a real stored value, and
// never re-apply once the marker is set — a player who deliberately rebinds afterward must not be
// silently reset back to their pre-Stage-5.2 layout on the next visit.
function migrateOldKeymapIfNeeded() {
  try {
    if (localStorage.getItem(KEYBIND_MIGRATED_KEY)) return;
    const raw = localStorage.getItem(OLD_KEYBIND_STORAGE_KEY);
    if (raw) {
      const old = JSON.parse(raw);
      for (const action in old) {
        if (!Object.prototype.hasOwnProperty.call(actionIndex, action)) continue;
        const slot0 = Array.isArray(old[action]) ? old[action][0] : null;
        const hid = slot0 && codeByName[slot0];
        if (!hid) continue;
        M()._eden_keybind_set(actionIndex[action], hid);
      }
    }
  } catch (e) {
    // Malformed old blob, or localStorage unavailable (private browsing) — leave the C model at
    // its compiled defaults rather than let a parse error here take out keybind loading entirely.
  }
  try { localStorage.setItem(KEYBIND_MIGRATED_KEY, '1'); } catch (e) {}
}

// Returns every action bound to this `event.code` string — plural because a duplicate binding
// (Ctrl -> flyDown AND crouch, out of the box) is shipped configuration, not an edge case; see
// Settings_web.mm's "CONFLICTS ARE ALLOWED" note. Walks eden_keybind_action_after() rather than
// stopping at the first hit for exactly that reason.
function codeToActions(code) {
  if (!ensureLoaded()) return [];
  const hid = codeByName[code];
  if (!hid) return [];
  const out = [];
  let prev = -1;
  for (;;) {
    const i = M()._eden_keybind_action_after(prev, hid);
    if (i < 0) break;
    out.push(actionMeta[i].action);
    prev = i;
  }
  return out;
}

function primaryCode(action) {
  if (!ensureLoaded()) return '';
  const i = actionIndex[action];
  if (i === undefined) return '';
  const code = M()._eden_keybind_get(i);
  return (code && nameByCode[code]) || '';
}

// ---------------------------------------------------------------------------------------------
// Rebind capture protocol — unchanged from before Stage 5.2, and stays here on purpose: it is a
// DOM concern (which element/keystroke counts as "the next real key"), not a settings-storage one.
// eden-settings.js's Keys tab calls startCapture(cb) after a "Press a key…" prompt; the NEXT
// keydown anywhere is consumed here (capture phase + stopPropagation, so it never reaches the real
// action-dispatch listener in eden-input.js or types into anything) and handed to `cb` instead of
// being interpreted as input.
// ---------------------------------------------------------------------------------------------
let keyCaptureCallback = null;
window.addEventListener('keydown', (e) => {
  if (!keyCaptureCallback) return;
  e.preventDefault();
  e.stopPropagation();
  const cb = keyCaptureCallback;
  keyCaptureCallback = null;
  cb(e.code);
}, true);

window.EdenKeybinds = {
  get actions() {
    const m = ensureLoaded();
    return m ? m.map((row) => row.action) : [];
  },
  labelFor: (action) => {
    const m = ensureLoaded();
    const i = m && actionIndex[action];
    return (m && i !== undefined) ? m[i].label : action;
  },
  primaryCode: primaryCode,
  startCapture: (onCode) => { keyCaptureCallback = onCode; },
  cancelCapture: () => { keyCaptureCallback = null; },
  // `code` is an event.code string, same contract as before Stage 5.2. Silently refuses a code
  // this build has no HID row for — mirrors eden_keybind_set()'s own "unknown key: refuse" guard
  // on the C side (a binding nobody can display is a binding nobody can undo from the UI).
  rebind: (action, code) => {
    if (!ensureLoaded()) return;
    const i = actionIndex[action];
    const hid = codeByName[code];
    if (i === undefined || !hid) return;
    M()._eden_keybind_set(i, hid);
  },
  resetDefaults: () => { if (ready()) M()._eden_keybind_reset_all(); },
};
