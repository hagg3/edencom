// seam_link_stubs_native.mm — the definitions the native target's excluded seam files owe the
// linker (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md). Native twin of
// web/src/seam/seam_link_stubs.mm; read that file's header first — the reasoning for each stub is
// there and is not repeated, and the two exclude lists are deliberately kept identical so the
// files stay diffable.
//
// *** SCAFFOLDING WITH AN EXPIRY DATE, same as the web original. *** Every section below belongs
// to a subsystem with a stage assigned to it (audio, dialogs and input are Stage 2; networking is
// later or never). As each lands, DELETE that section rather than let a stub outlive its
// replacement.
//
// WHAT DIFFERS FROM THE WEB TWIN:
//   * the three dialogs are console/auto-answer rather than DOM overlays. Stage 2 gives them the
//     engine's own GL UI, which is where the plan puts native's UI;
//   * SimpleAudioEngine is REPLACED on both targets now, but by different backends:
//     web/src/seam/SimpleAudioEngine_web.mm (Web Audio) and src/seam/SimpleAudioEngine_native.mm
//     (SDL_AudioStream + AVAudioPlayer). Neither file appears in the exclude list.
// Everything else is the web file's behaviour verbatim.

#import "../../../web/src/shim/foundation/uikit_stubs.h"

#include "../../../Classes/FileManager.h"
#include "../../../Classes/Alert.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Menu.h"
#include "../../../Classes/Hud.h"
#include "../../../Classes/GLDialog.h"
#include "../../../Classes/VKeyboard.h"
#include "../../../Classes/SharedList.h"
#include "../../../Classes/ShareMenu.h"

#include <cstdio>

// =============================================================================================
// Alert  (Classes/Alert.mm, excluded) — Stage 2 gives these the engine's GL UI
// =============================================================================================
void alert_init() {}

// Defined in web/src/seam/Menu_web.mm, which the native target also compiles: the New World
// screen's pending choices (one-shot "take" semantics, -1 = none).
extern "C" int  eden_menu_take_pending_world_type(void);
extern "C" int  eden_menu_take_pending_world_height(void);
extern "C" void eden_menu_set_pending_world_height(int height);

extern "C" void eden_world_type_choice(int flat) {
    // Same call Alert.mm's PAlert delegate made for buttonIndex 0 ("Flat") / 1 ("Normal"):
    // Menu::a_genFlat(BOOL) sets fm->genflat and does loading++ (3->4), unblocking loadWorld().
    World::getWorld->menu->a_genFlat(flat ? TRUE : FALSE);
}

// --- the world-type + height prompt, as a chained pair of GLDialogs (Phase N Stage 2.5) --------
// Menu::render() parks `loading` at 3 waiting for eden_world_type_choice(); nothing else advances
// it, so BOTH dialogs must always terminate in that call — a dialog that never answers is an
// unrecoverable "loading forever" hang.
static int  s_worldFlat = 0;
static bool s_heightAlreadyChosen = false;

// S.5e (the user, 2026-10-10): New Dawn 256 is the default, so it is button 0, the first cell of the
// grid; Classic 64 (the old default) is second.
static void wt_height_cb(int j) {
    if (!s_heightAlreadyChosen)
        eden_menu_set_pending_world_height(j == 0 ? 256 : 64);  // consumed by probeWorldHeight()
    eden_world_type_choice(s_worldFlat);
}

static void wt_type_cb(int i) {
    s_worldFlat = (i == 0) ? 1 : 0;                 // button 0 = "Flat", 1 = "Normal"
    if (s_heightAlreadyChosen) { wt_height_cb(0); return; }
    static const char* const kHeight[] = { "New Dawn 256", "Classic 64" };
    GLDialog::show("Choose height format",
                   "How tall the world is. Classic matches the shipped game; New Dawn is taller "
                   "and its saves are bigger.", kHeight, 2, wt_height_cb);
}

void showAlertWorldType() {
    int pendingType = eden_menu_take_pending_world_type();

    // Peek the pending height (take() clears it, so put it back for probeWorldHeight()).
    int pendingHeight = eden_menu_take_pending_world_height();
    s_heightAlreadyChosen = (pendingHeight >= 0);
    if (s_heightAlreadyChosen) eden_menu_set_pending_world_height(pendingHeight);

    if (pendingType >= 0) { s_worldFlat = pendingType; wt_type_cb(pendingType ? 0 : 1); return; }

    static const char* const kType[] = { "Flat", "Normal" };
    GLDialog::show("Choose world type",
                   "Normal is the generated landscape. Flat is an empty building slab.",
                   kType, 2, wt_type_cb);
}

// The in-game ESC menu's "home" button (Classes/Hud.mm). Nothing in the engine waits on the
// answer, so the async GL dialog is a faithful stand-in for the old UIAlertView.
// Cancel is LAST because GLDialog lays buttons out in two columns and spans an odd final one
// across both — which is exactly the mockup's full-width Cancel row (design-system.md,
// "Alert-2Stack"). Reordering these three is therefore a layout change, not just a copy change.
static void home_cb(int i) {
    if      (i == 0) World::getWorld->hud->asetHome();   // "Set Current Location as Home"
    else if (i == 1) World::getWorld->hud->awarpHome();  // "Warp home"
    // i == 2 ("Cancel"): nothing.
}

void showAlertWarpHome() {
    static const char* const kHome[] = { "Set Home Here", "Warp Home", "Cancel" };
    GLDialog::show("Home menu", "Both options save the world file first.", kHome, 3, home_cb);
}

// Stage 5.6: a real confirmation. It was a no-op ("not confirming a delete is the non-destructive
// default"), which made the main menu's Delete do nothing on native at all. The callback is
// Alert.mm's delegate for this alert: button 0 runs Menu::a_deleteConfirm() on the selected
// world, Cancel runs a_deleteCancel(). Nothing waits on the answer (no `loading` is parked).
// The body names the world; "Delete" first and Cancel LAST, the full-width row (see above).
static void delete_cb(int i) {
    Menu* m = World::getWorld ? World::getWorld->menu : NULL;
    if (!m) return;
    if (i == 0 && m->selected_world) m->a_deleteConfirm();
    else m->a_deleteCancel();
}

void showAlertDeleteConfirm(NSString *name) {
    static const char* const kDelete[] = { "Delete", "Cancel" };
    static char body[160];
    std::snprintf(body, sizeof(body), "\"%s\" will be deleted from this device. This can't be undone.",
                  name ? [name UTF8String] : "This world");
    GLDialog::show("Delete world?", body, kDelete, 2, delete_cb);
}

// Stage S / S.5: the question after a `.eden` was converted to an `.emod`. "Delete" removes the
// original (FileManager::resolveOriginal(TRUE)); "Keep" leaves it, where the world list hides it
// because it is paired with the `.emod`. Cancel/Escape is the LAST button, so it keeps (the safe
// answer). Nothing waits on it.
static void delete_original_cb(int i) {
    if (World::getWorld && World::getWorld->fm) World::getWorld->fm->resolveOriginal(i == 0 ? TRUE : FALSE);
}

void showAlertDeleteOriginal(NSString *edenName) {
    static const char* const kButtons[] = { "Delete original", "Keep it" };
    static char body[240];
    std::snprintf(body, sizeof(body),
                  "\"%s\" was converted to the smaller .emod format. The original .eden is still on "
                  "this device. Delete it?", edenName ? [edenName UTF8String] : "This world");
    GLDialog::show("World converted", body, kButtons, 2, delete_original_cb);
}

// Stage S / S.5e: before a conversion, with Settings' "Upgrade 64z worlds to 256z when converting"
// on. Three buttons, Cancel LAST (the full-width row): World::loadWorld waits on
// FileManager::answerConvertHeight and backs out to the menu on Cancel. A 256z source never asks.
// dismiss() (no callback) would leave it waiting, as it would any loading-screen dialog.
static void convert_height_cb(int i) {
    if (World::getWorld && World::getWorld->fm) World::getWorld->fm->answerConvertHeight(i == 0 ? 1 : (i == 1 ? 0 : -1));
}

void showAlertConvertHeight(NSString *edenName, BOOL known64) {
    static const char* const kButtons[] = { "Upgrade to 256", "Keep 64", "Cancel" };
    static char body[320];
    std::snprintf(body, sizeof(body),
                  known64 ? "\"%s\" is a Classic 64-tall world. Converting it to .emod can also make it New Dawn "
                            "256-tall: the same blocks, with room to build up to y 255. Uses more memory while it is open."
                          : "If \"%s\" is a Classic 64-tall world, converting it to .emod can also make it New Dawn "
                            "256-tall: the same blocks, with room to build up to y 255. Uses more memory while it is open.",
                  edenName ? [edenName UTF8String] : "This world");
    GLDialog::show("Convert world", body, kButtons, 3, convert_height_cb);
}

// =============================================================================================
// VKeyboard  (Classes/VKeyboard.mm, excluded) — Stage 2
// =============================================================================================
// The original overlays a native UITextField on the GL view for world renaming. Native's answer
// is SDL_StartTextInput plus the engine's own GL text field; Stage 2's row.
void vkeyboard_init() {}

// =============================================================================================
// SharedList / ShareMenu  (networking cluster, excluded)
// =============================================================================================
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
// CocosDenshion::SimpleAudioEngine — NO LONGER STUBBED (Phase N Stage 2)
// =============================================================================================
// The whole class now lives in src/seam/SimpleAudioEngine_native.mm (SDL_AudioStream mixing for
// effects, AVAudioPlayer for the five streaming channels). The stub that was here is deleted
// rather than #if'd out, per this file's own rule: a stub that outlives its replacement is a
// duplicate symbol at best and a silently-silent game at worst.

// =============================================================================================
// SUPPORTS_OGL2  (Classes/EAGLView.mm, excluded)
// =============================================================================================
// FALSE for the same reason as on web, and it is a choice rather than caution: the shim presents
// ES 1.1 fixed-function, so from the engine's point of view there is no ES2 path. TRUE would send
// Hud.mm/Resources.mm down code the shim does not implement.
BOOL SUPPORTS_OGL2 = FALSE;

// =============================================================================================
// ShareUtil  (Classes/ShareUtil.mm, excluded)
// =============================================================================================
// *** A DIFFERENT KIND OF STUB, and the reason is worth reading before excluding another .mm. ***
// Excluding an Objective-C .mm removes its @implementation, which the LINKER does not miss at all
// — a bracket send resolves by name through the runtime's class table at RUN time. So the build
// stays green and `[[ShareUtil alloc] init]` in Menu::Menu() dispatches against a class the
// runtime never registered. On web that surfaced as a bare "function signature mismatch" inside
// Menu::Menu() with nothing naming ShareUtil. Any excluded .mm that carried an @implementation
// needs a stub like this one.
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
