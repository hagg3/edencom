// AppLifecycle_web.mm — Phase N Stage 4.2: save-on-background.
//
// THE GAP THIS CLOSES. Stock Eden saves on streaming boundaries, warps, the Save button and exit —
// never when the app goes away (root CLAUDE.md says so in as many words, and the Phase N Stage 3+
// plan re-measured it: `Classes/Terrain.mm:454,468,2116,2343` and `Classes/FileManager.mm:2449`
// are the whole list). On the ORIGINAL iOS target that was survivable, because iOS 4-era
// backgrounding kept the process alive. On the two targets that exist today it is a live
// correctness bug: a browser tab can be discarded at any moment (and IS, under memory pressure —
// V7 watched desktop Chrome do it), and iOS 12 will kill a backgrounded app without another
// callback. `public/eden-storage.js`'s visibilitychange/pagehide handlers do NOT cover this: they
// flush the STORAGE MIRROR (FS.syncfs), i.e. they persist whatever the engine already wrote. If
// the engine never wrote, there is nothing to mirror.
//
// WHY THIS FILE IS `_web` BUT PORTABLE. Same rule as Settings_web/Menu_web/Storage_web (see
// native/CMakeLists.txt's EDEN_SHARED_SEAM list): a "_web" seam file is one the WEB build owns and
// the native build compiles unmodified when it is genuinely platform-free. This one is — it names
// no DOM, no Emscripten, no SDL. The whole point is that the policy below ("when is it safe to
// save?") has exactly one implementation, so web's `pagehide`, native's SDL_EVENT_QUIT and iOS's
// SDL_EVENT_WILL_ENTER_BACKGROUND cannot drift apart. Landing on web first, alone, is the plan's
// own instruction: web's headless suite is the cheapest possible proof that it works.
//
// SIDE EFFECT, DELIBERATELY ACCEPTED: FileManager::saveWorld() opens with
// `terrain->endDynamics(TRUE)`, which clears liquids, the burn list and all particle effects. So
// backgrounding stops flowing water and puts out fires, exactly as pressing the in-game Save
// button already does (Classes/Hud.mm:1019 — the same call, followed by the same no-op
// startDynamics()). Preserving dynamics across a save is a change to saveWorld's own contract and
// belongs nowhere near this file.
#import "../../../Classes/World.h"
#import "../../../Classes/Menu.h"
#import "../../../Classes/Terrain.h"
#import "../../../Classes/FileManager.h"
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT
#include "AppLifecycle_web.h"

// How many times eden_app_will_background() has actually written a world, and what the last call
// answered. Diagnostics only — tools/headless-save-on-background-test.js asserts on them, because
// "the return code said 0" and "a save really happened" are different claims and this project has
// been bitten by exactly that difference before (ROADMAP's "a gate that greps for a word is not a
// gate").
static int g_background_saves = 0;
static int g_last_status = EDEN_BG_NO_ENGINE;

extern "C" {

// Save the open world because the app is going away (tab hidden/unloaded, window closed, iOS
// backgrounded). Idempotent-ish and cheap when there is nothing to do; safe to call from any host
// lifecycle event, including several in a row for one departure — `pagehide` and
// `visibilitychange` both fire on a real tab close and both are wired to it.
//
// EVERY GUARD BELOW IS LOAD-BEARING, and the reason is the same in each case: saveWorld() is
// written for the in-play state machine and dereferences things that only exist there.
EDEN_EXPORT
int eden_app_will_background(void) {
    World* w = World::getWorld;
    if (!w || !w->fm || !w->terrain || !w->menu) {
        return (g_last_status = EDEN_BG_NO_ENGINE);
    }
    // A load is in flight. Worth distinguishing from "no world" purely so the answer is honest:
    // game_mode is still GAME_MODE_MENU for the whole of Menu::render's 1->4 loading ladder, so
    // the plain !PLAY test below would call a half-loaded world "nothing to save".
    if (w->game_mode == GAME_MODE_MENU && w->menu->loading) {
        return (g_last_status = EDEN_BG_BUSY);
    }
    // Not in a world: the menu is up, or the app is between worlds. Nothing to save, and this is
    // the common case for a tab that gets hidden on the main menu.
    //
    // THIS TEST MUST COME BEFORE ANY READ OF target_game_mode. `World::World()` sets game_mode
    // (World.mm:198) and never touches target_game_mode -- its only four assignments are in
    // World::update's doneLoading==2 branch and World::exitToMenu (World.mm:427-428, 475-476) --
    // so the field is INDETERMINATE until the first world is loaded or exited. Reading it earlier
    // is reading uninitialised memory, and it happens to be reachable here because this function
    // can be called at any moment from a host lifecycle event, including before the player has
    // ever pressed Play.
    if (w->game_mode != GAME_MODE_PLAY) {
        return (g_last_status = EDEN_BG_NO_WORLD);
    }
    // In a world but not settled in it. GAME_MODE_PLAY is only reached through GAME_MODE_WAIT
    // (World.mm:606, in render()); `terrain->loaded` is the terrain's own answer to the same
    // question. Saving out of either writes a header for a world that is not fully in memory.
    if (w->target_game_mode != GAME_MODE_PLAY || w->menu->loading || !w->terrain->loaded) {
        return (g_last_status = EDEN_BG_BUSY);
    }
    // saveWorld() reads menu->selected_world->display_name into the 192-byte header and
    // terrain->world_name for the file path. Neither is null in normal play; both are null in the
    // states above, and a null here means we mis-read the state machine rather than that the
    // player is unlucky — so bail rather than crash on the way out of the app.
    if (!w->menu->selected_world || !w->terrain->world_name) {
        return (g_last_status = EDEN_BG_BUSY);
    }

    w->fm->saveWorld();
    ++g_background_saves;
    return (g_last_status = EDEN_BG_SAVED);
}

// Diagnostics for the headless test and the dev console. Not on any hot path.
EDEN_EXPORT
int eden_app_background_save_count(void) { return g_background_saves; }

EDEN_EXPORT
int eden_app_background_last_status(void) { return g_last_status; }

}  // extern "C"
