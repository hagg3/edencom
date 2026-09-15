// seam_link_stubs.mm — the definitions the excluded seam files owe the linker, so the target can
// LINK before any of those subsystems is actually implemented.
//
// *** THIS FILE IS SCAFFOLDING WITH AN EXPIRY DATE. *** Every symbol below belongs to a seam
// subsystem with a stage already assigned to it (P2 raster, P3 keyboard, P4 persistence,
// P5 audio, P6 networking). As each stage lands, DELETE that section rather than leaving it to
// shadow the real implementation — a stub that silently outlives its replacement is worse than
// no stub. When this file is empty, the port's seam work is done.
//
// WHY IT EXISTS: web-port-plan.md's blocker #4 called for exactly this ("writing throwaway
// failing stubs first is the fastest way to surface the NEXT layer of real errors"). The measured
// cost of five excluded subsystems is 53 undefined symbols, listed below grouped by owner.
//
// THE STUBS ARE INERT-BUT-SAFE, NOT ABORTING, and that is a deliberate choice per subsystem
// rather than a blanket one. The Stage P1 milestone is a single headless `world->update(etime)`
// tick, which requires the world to finish constructing and loading; an abort inside a draw call
// or a texture constructor would stop the run before the thing under test executes. Where doing
// nothing is genuinely risky (FileManager's save path), that is called out at the definition.
#import "../shim/foundation/uikit_stubs.h"
#include <emscripten/emscripten.h>

#include "../../../Classes/FileManager.h"
#include "../../../Classes/Alert.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Menu.h"
#include "../../../Classes/VKeyboard.h"
#include "../../../Classes/SharedList.h"
#include "../../../Classes/ShareMenu.h"
#include "../../../Classes/SimpleAudioEngine.h"

// Texture2D (Classes/Texture2D.mm, excluded) is REPLACED, not stubbed — see Texture2D_web.mm.
// Nothing in this file owes it anything anymore.

// =============================================================================================
// FileManager  (Classes/FileManager.mm + FileManagerHelper.mm) — STAGE P4: NO LONGER STUBBED.
// =============================================================================================
// P4 landed by UN-EXCLUDING the real engine files (see CMakeLists.txt EDEN_SEAM_EXCLUDE). The
// real FileManager::{FileManager,saveWorld,loadWorld,readColumn,worldExists,getName,...} and the
// real `regionSkyColors`/`defaultRegionSkyColors` tables now come from Classes/FileManager.mm,
// so every former stub here would be a DUPLICATE SYMBOL. All of it (ctor, save/load, readColumn,
// worldExists, and the zeroed regionSkyColors table) is intentionally gone. FileArchive.mm stays
// excluded but is referenced only in commented-out code, so nothing here owes it a symbol.

// =============================================================================================
// Alert  (Classes/Alert.mm, excluded) — TODO P6 (delete-confirm/report); world-type + warp-home DONE
// =============================================================================================
// UIAlertView prompts. docs/ui.md notes this game's UI is otherwise entirely custom GL with no
// UIKit, so a from-scratch replacement could have been an in-engine GL prompt instead of a DOM
// dialog — but showAlertWorldType is on a hard deadline (see below) and needs real user choice,
// not an auto-answer, so pass 25 gave it a plain DOM `<dialog>` instead. Doing nothing for the
// remaining two (delete-confirm/report) means those confirmations simply never appear and
// the action is not taken — see each site below for why that's the safe default.
//
// *** showAlertWorldType is NOT a no-op, and must not be. *** It is on the critical path for
// loading a NEW world. When creating/opening a world (worldExists() above always returns NO until
// P4), Menu::render() reaches `loading==2`, calls showAlertWorldType(), and bumps `loading` to 3.
// The ORIGINAL iOS flow then waits on the modal: tapping "Normal"/"Flat" calls
// Menu::a_genFlat(BOOL), which does `loading++` (3->4), and only `loading==4` calls loadWorld().
// Pass 25: replaced the old "auto-answer Normal" stub with a real DOM dialog so a new world can
// actually be Flat. `loading` sitting at 3 while the dialog is open is CORRECT — it mirrors the
// original modal wait; nothing else advances it. `eden_world_type_choice` (below) is the async
// callback the dialog's buttons invoke; it does exactly what `Alert.mm`'s delegate did, just later.
// NOTE: this EM_ASM touches `document` directly, which only exists because the sole runnable host
// today is eden-st.html's single-threaded, main-thread build (see STATUS.md) — `public/
// index.html`'s worker/OffscreenCanvas path (D1) is still unimplemented scaffolding. If/when that
// path is finished, this call needs to become a postMessage to the main thread instead.
void alert_init() {}

// ---------------------------------------------------------------------------------------------
// Generic DOM replacement for UIAlertView (pass 27). One JS helper + one C callback, so the
// remaining prompts (delete-confirm, report) can be wired up later by adding a `dialogId` case
// rather than another copy of the overlay markup.
//
// Button INDEXES match Alert.mm's UIAlertView ordering exactly — cancelButtonTitle is index 0 and
// otherButtonTitles follow — because `eden_alert_choice` below is a transcription of
// `PAlert -alertView:clickedButtonAtIndex:` and must stay index-compatible with it.
//
// Same two caveats as showAlertWorldType() below: (1) it touches `document` directly, which is
// only valid because the sole runnable host today is eden-st.html's single-threaded main-thread
// build — index.html's worker path would need a postMessage bridge; (2) EM_JS bodies go through
// the C PREPROCESSOR, whose macro-argument splitting tracks PARENS ONLY, so a top-level comma
// inside a `[...]` or `{...}` literal here silently becomes "another macro argument" and produces
// errors pointing at the macro, not at the code. Everything below is written procedurally for
// that reason — do not "tidy" it into array/object literals.
EM_JS(void, eden_js_alert_dialog, (int dialogId, const char* titleC, const char* b0,
                                   const char* b1, const char* b2), {
  // Headless (`node eden.js`) has no DOM. Dropping the prompt there means the action is simply
  // not taken, which is the safe default for every dialog this serves.
  if (typeof document === 'undefined') return;
  // A dialog is a thing you point at: release the pointer lock so the cursor is visible. The
  // page's trackCursorNeed() loop only re-grabs on a picker-close EDGE, so it will not fight this.
  if (document.exitPointerLock && document.pointerLockElement) document.exitPointerLock();
  var overlay = document.createElement('div');
  // z-index MUST sit above --eden-z-menu (25) and --eden-z-panel (30) — see the z-index scale in
  // public/eden-ui.css. This was a literal 20 until 2026-08-06, i.e. UNDER the DOM menu, and that
  // was not cosmetic: the engine parks `loading` on the answer to this dialog, so a modal the
  // player cannot see or click is an unrecoverable "loading forever" hang. Verified in real Safari
  // (elementFromPoint over both buttons returned DIV.eden-stack, the menu, not the button).
  overlay.style.cssText = 'position:fixed;inset:0;z-index:var(--eden-z-alert,40);display:flex;' +
    'align-items:center;justify-content:center;background:rgba(0,0,0,.55);' +
    'font:14px/1.4 monospace;';
  var box = document.createElement('div');
  box.style.cssText = 'background:#222;color:#eee;border:1px solid #555;border-radius:6px;' +
    'padding:16px 20px;min-width:240px;text-align:center;';
  var title = document.createElement('div');
  title.textContent = UTF8ToString(titleC);
  title.style.cssText = 'margin-bottom:12px;font-weight:bold;';
  box.appendChild(title);
  var row = document.createElement('div');
  row.style.cssText = 'display:flex;gap:10px;justify-content:center;flex-wrap:wrap;';
  box.appendChild(row);
  overlay.appendChild(box);
  document.body.appendChild(overlay);
  function addButton(labelPtr, idx) {
    if (!labelPtr) return;
    var btn = document.createElement('button');
    btn.textContent = UTF8ToString(labelPtr);
    btn.style.cssText = 'padding:6px 16px;font:inherit;cursor:pointer;';
    btn.onclick = function () {
      document.body.removeChild(overlay);
      Module._eden_alert_choice(dialogId, idx);
    };
    row.appendChild(btn);
  }
  addButton(b0, 0);
  addButton(b1, 1);
  addButton(b2, 2);
});

// dialogId 1 = warp-home. Transcribed from Alert.mm's PAlert delegate, alertWarpHome branch.
extern "C" EMSCRIPTEN_KEEPALIVE void eden_alert_choice(int dialogId, int buttonIndex) {
  if (!World::getWorld || !World::getWorld->hud) return;
  if (dialogId == 1) {
    switch (buttonIndex) {
      case 0: break;                                     // Cancel — Alert.mm does nothing here too
      case 1: World::getWorld->hud->asetHome();  break;   // "Set Current Location as Home"
      case 2: World::getWorld->hud->awarpHome(); break;   // "Warp home"
      default: break;
    }
  }
}

// Reached from the in-game ESC menu's "home" button (Classes/Hud.mm:903). Nothing in the engine
// waits on the answer — unlike showAlertWorldType, no `loading` counter is parked on it — so the
// async DOM round-trip is a faithful stand-in for the modal.
void showAlertWarpHome() {
  eden_js_alert_dialog(1, "Home Menu", "Cancel", "Set Current Location as Home", "Warp home");
}

extern "C" EMSCRIPTEN_KEEPALIVE void eden_world_type_choice(int flat) {
  // Same call Alert.mm's PAlert delegate made for buttonIndex 0 ("Flat")/1 ("Normal"):
  // Menu::a_genFlat(BOOL) sets fm->genflat and does loading++ (3->4), unblocking loadWorld().
  World::getWorld->menu->a_genFlat(flat ? TRUE : FALSE);
}
// Defined in Menu_web.mm. Returns the flat/normal choice the DOM New World screen already made
// (and clears it), or -1 if nothing is pending.
extern "C" int eden_menu_take_pending_world_type(void);

void showAlertWorldType() {
  // The rebuilt DOM menu (public/eden-menu.js) asks this question UP FRONT, on the New World
  // screen's generator-type rail, because that is what the mockups show. When it has, answering
  // the modal is not the player's job any more — take the parked choice and continue. Falls
  // through to the real dialog whenever nothing is pending (legacy GL menu, or a world reached by
  // any path that didn't go through the New World screen), so this is additive, not a takeover.
  {
    int pending = eden_menu_take_pending_world_type();
    if (pending >= 0) { eden_world_type_choice(pending); return; }
  }
  EM_ASM({
    // Headless (`node eden.js`, see archive/PORT-STATUS-2026-08-13.md "Headless driving") has no `document` — fall
    // back to the old auto-answer-Normal behavior so a scripted drive through the create-world
    // path doesn't ReferenceError; a real browser always takes the dialog path below.
    if (typeof document === 'undefined') { Module._eden_world_type_choice(0); return; }
    var overlay = document.createElement('div');
    // Above --eden-z-menu (25): this dialog is the one thing standing between the player and a
    // world load, and at z-index 20 it rendered UNDERNEATH the DOM menu. See the note on the
    // alert overlay above; this one is the instance that was actually reported as a hang.
    overlay.style.cssText = 'position:fixed;inset:0;z-index:var(--eden-z-alert,40);display:flex;' +
      'align-items:center;justify-content:center;background:rgba(0,0,0,.55);' +
      'font:14px/1.4 monospace;';
    var box = document.createElement('div');
    box.style.cssText = 'background:#222;color:#eee;border:1px solid #555;border-radius:6px;' +
      'padding:16px 20px;min-width:220px;text-align:center;';
    var title = document.createElement('div');
    title.textContent = 'Pick world type';
    title.style.cssText = 'margin-bottom:12px;font-weight:bold;';
    box.appendChild(title);
    var row = document.createElement('div');
    row.style.cssText = 'display:flex;gap:10px;justify-content:center;';
    box.appendChild(row);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    // Matches Alert.mm's alertWorldType button order: "Flat", "Normal" (buttonIndex 0/1).
    // NOTE: EM_ASM stringifies its argument via the C PREPROCESSOR, which only tracks paren
    // balance (not brackets/braces) when splitting macro arguments — a top-level `,` inside a
    // `[...]` array literal here would be misread as another EM_ASM argument. Two explicit
    // addButton() calls (no array/forEach) sidestep that trap.
    function addButton(label, flat) {
      var btn = document.createElement('button');
      btn.textContent = label;
      btn.style.cssText = 'padding:6px 16px;font:inherit;cursor:pointer;';
      btn.onclick = function () {
        document.body.removeChild(overlay);
        Module._eden_world_type_choice(flat);
      };
      row.appendChild(btn);
    }
    addButton('Flat', 1);
    addButton('Normal', 0);
  });
}
// Safe as a no-op: not confirming a delete is the non-destructive default (see the warning above
// about NOT auto-answering destructive prompts).
void showAlertDeleteConfirm(NSString *name) { (void)name; }

// =============================================================================================
// VKeyboard  (Classes/VKeyboard.mm, excluded) — TODO P3
// =============================================================================================
// The original overlays a native UITextField on the GL view for world renaming (this is why it
// was reclassified into the seam — see the EDEN_SEAM_EXCLUDE note in CMakeLists.txt). The web
// equivalent is a real DOM <input> positioned over the canvas.
void vkeyboard_init() {}

// =============================================================================================
// SharedList / ShareMenu  (networking cluster, excluded) — TODO P6
// =============================================================================================
// The world-browsing UI. Stage P6 may legitimately ship these disabled — the plan explicitly
// allows "feature-flag Shared Worlds off if the endpoint can't be reached cross-origin", and the
// eden-world-editor notes TLS failures on app2.edengame.net. In that case these stubs stop being
// scaffolding and become the shipped behavior, at which point the menu entry should be hidden
// rather than left leading to an empty screen.
SharedList::SharedList() {}
void SharedList::activate() {}
void SharedList::deactivate() {}
void SharedList::update(float etime) { (void)etime; }
void SharedList::render() {}

ShareMenu::ShareMenu() {}
void ShareMenu::activate() {}
void ShareMenu::deactivate() {}
void ShareMenu::update(float etime) { (void)etime; }
void ShareMenu::render() {}
void ShareMenu::beginShare(WorldNode *world) { (void)world; }

// =============================================================================================
// CocosDenshion::SimpleAudioEngine  (Classes/SimpleAudioEngine.mm, excluded) — STAGE P5: REPLACED
// =============================================================================================
// Audio is no longer stubbed. The real implementation (Web Audio for effects, a streaming <audio>
// element for music, plus a hand-rolled LPCM .caf decoder) lives in src/seam/SimpleAudioEngine_web.mm.
// Everything that used to be here would now be a duplicate symbol.

// =============================================================================================
// SUPPORTS_OGL2  (Classes/EAGLView.mm, excluded) — TODO P2
// =============================================================================================
// Set from the device's ES2 capability check on iOS; read by Hud.mm and Resources.mm to choose
// cheaper art paths.
//
// FALSE is a deliberate choice, not just caution: the D2 shim emulates ES 1.1 fixed-function over
// WebGL, so from the engine's point of view there is no ES2 path available. TRUE would send
// Hud/Resources down code the shim does not implement. Stage P2 owns the real value; Stage R7
// (the WebGL2 renderer) is when TRUE becomes the right answer.
BOOL SUPPORTS_OGL2 = FALSE;

// =============================================================================================
// ShareUtil  (Classes/ShareUtil.mm, excluded) — TODO P6
// =============================================================================================
// *** A DIFFERENT KIND OF STUB FROM EVERY OTHER ONE IN THIS FILE, and the reason is worth
// reading before you exclude another Objective-C .mm. ***
//
// Everything above is a C++ definition the LINKER demanded. `ShareUtil` is an Objective-C class,
// and excluding `ShareUtil.mm` removed its `@implementation` — which the linker does not miss at
// all, because under the GNU ABI a bracket send resolves by name through the runtime's class
// table at run time. So the build stays green and `[[ShareUtil alloc] init]` in `Menu::Menu()`
// (Classes/Menu.mm:132) instead dispatches against a class the runtime has never registered.
// That surfaces as `RuntimeError: function signature mismatch` attributed to `Menu::Menu()` —
// no symbol name, no mention of ShareUtil, nothing pointing here. **Any excluded .mm that
// carried an @implementation needs a stub like this one, and only a runtime trace will tell
// you.** Same open risk today: `Classes/FileDownload.mm` (also excluded, also an ObjC class);
// it is not stubbed here because nothing has been observed instantiating it yet — the only
// owner is ShareUtil's `dlmanager`, which this stub never allocates.
//
// Behaviour: inert. Every method is a no-op and `getSharedWorldList`/`searchSharedWorlds`
// report "nothing shared", which is the truthful answer with no endpoint wired up — and matches
// the plan's sanctioned P6 outcome ("feature-flag Shared Worlds off if the endpoint can't be
// reached"). P6 deletes this whole section.
#import "../../../Classes/ShareUtil.h"

@implementation ShareUtil
@synthesize listresult;
- (void)canceldl {}
- (void)loadShared:(NSString *)file_name { (void)file_name; }
- (void)reportWorld:(NSString *)file_name { (void)file_name; }
- (void)loadSharedPreview:(NSString *)file_name { (void)file_name; }
- (void)shareWorld:(NSString *)file_name { (void)file_name; }
- (void)getSharedWorldList {}
- (NSData *)gzipInflate:(NSData *)data { (void)data; return nil; }
- (void)uploadSuccess:(id)obj { (void)obj; }
- (void)uploadError:(id)obj { (void)obj; }
- (void)downloadSuccess:(id)obj { (void)obj; }
- (void)reportSuccess:(id)obj { (void)obj; }
- (void)reportError:(id)obj { (void)obj; }
- (void)downloadError:(id)obj { (void)obj; }
- (NSString *)searchSharedWorlds:(NSString *)query { (void)query; return nil; }
@end
