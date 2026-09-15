// eden-hotbar.js — audit row 28/C5 split: the DOM hotbar strip (desktop profile only) mirroring
// the engine's 9 hotbar slots, plus its localStorage persistence.
//
// Depends on (must load after): eden-host.js (callIfReady). Declares updateHotbarStrip(visible),
// called once per frame from eden-st.html's trackCursorNeed — that caller decides visibility
// (touch profile / a picker or panel wanting the cursor both hide it), this file only renders.
'use strict';

// --- PC controls audit Phase 4: DOM hotbar strip (desktop profile only) --------------------
// Styled by the shared design system (.eden-hotbar in eden-ui.css) — one of the two surfaces that
// INVERTS the palette (dark chrome, light text) because it sits over the rendered world rather
// than on a panel. ensureCSS() so the strip is styled even if no panel is ever opened.
window.EdenUI.ensureCSS();
const HOTBAR_STORAGE_KEY = 'eden.prefs.hotbar';
const hotbarEl = document.createElement('div');
hotbarEl.id = 'eden-hotbar';
hotbarEl.className = 'eden-hotbar';
const hotbarSlots = [];
for (let i = 0; i < 9; i++) {
  const slot = document.createElement('div');
  slot.className = 'eden-hotbar__slot';
  const num = document.createElement('span');
  num.className = 'eden-hotbar__num';
  num.textContent = String(i + 1);
  const type = document.createElement('span');
  type.className = 'type';
  slot.appendChild(num);
  slot.appendChild(type);
  slot.addEventListener('click', () => callIfReady(() => Module._eden_select_hotbar_slot(i)));
  hotbarEl.appendChild(slot);
  hotbarSlots.push({ el: slot, typeEl: type });
}
document.body.appendChild(hotbarEl);

// Classes/Constants.h's BLOCK_TYPES enum: TYPE_NONE=0 (not placeable) through TYPE_BTSTEEL=111
// (the last real type as of this build). A value outside that range reaching hud->blocktype would
// ask the atlas/HUD-icon renderer for a block that does not exist — defended against here so a
// corrupted/out-of-range localStorage value from a future build (or a manual localStorage edit)
// can never do that, rather than trusting the stored JSON blindly.
const MIN_BLOCK_TYPE = 1, MAX_BLOCK_TYPE = 111;
function restoreHotbar() {
  let saved;
  try { saved = JSON.parse(localStorage.getItem(HOTBAR_STORAGE_KEY) || 'null'); } catch (e) { saved = null; }
  if (!Array.isArray(saved) || saved.length !== 9) return;
  saved.forEach((t, i) => {
    if (Number.isInteger(t) && t >= MIN_BLOCK_TYPE && t <= MAX_BLOCK_TYPE) {
      Module._eden_set_hotbar_slot_type(i, t);
    }
  });
}
function persistHotbar() {
  const types = [];
  for (let i = 0; i < 9; i++) types.push(Module._eden_get_hotbar_slot_type(i));
  try { localStorage.setItem(HOTBAR_STORAGE_KEY, JSON.stringify(types)); } catch (e) {}
}
let hotbarRestored = false;
let lastHotbarTypes = null;
// Q2 perf audit: last-rendered (types, active-slot) state, so the 9 textContent writes + 9
// classList.toggle calls below only run on an actual change, not every rAF frame — the wasm reads
// (_eden_get_hotbar_index/_eden_get_hotbar_slot_type) still happen every frame since they're what
// detects the change, but they're cheap compared to the DOM writes.
let lastRenderedActive = null;
let lastRenderedTypes = null;
let lastVisible = null;
function updateHotbarStrip(visible) {
  if (!moduleReady) return;
  if (!hotbarRestored) { restoreHotbar(); hotbarRestored = true; }
  if (visible !== lastVisible) { hotbarEl.style.display = visible ? '' : 'none'; lastVisible = visible; }
  if (!visible) return;
  const active = Module._eden_get_hotbar_index();
  const types = [];
  for (let i = 0; i < 9; i++) types.push(Module._eden_get_hotbar_slot_type(i));
  const key = types.join(',');
  if (active !== lastRenderedActive || key !== lastRenderedTypes) {
    for (let i = 0; i < 9; i++) {
      const s = hotbarSlots[i];
      s.el.classList.toggle('is-active', i === active);
      s.typeEl.textContent = String(types[i]);
    }
    lastRenderedActive = active;
    lastRenderedTypes = key;
  }
  // Persist only on an actual change (picker edits are rare; this avoids a localStorage write
  // every single frame while the strip is visible).
  if (key !== lastHotbarTypes) { lastHotbarTypes = key; persistHotbar(); }
}
