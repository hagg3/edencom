// Menu_web.mm — the C side of the DOM main menu (public/eden-menu.js).
//
// WHAT THIS IS NOT: a replacement for Classes/Menu.mm. Menu.mm still compiles, still runs, still
// owns every piece of state — the world list, the selection, the load state machine, the alerts.
// This file only exposes that state to JS and offers the same state transitions the GL menu's own
// touch handler performs, so the DOM screens can drive the real thing instead of reimplementing it.
//
// WHY NOT --wrap Menu::update/render, the way Settings_web.mm wraps SettingsMenu?
// Because the WORLD-LOAD STATE MACHINE LIVES INSIDE Menu::render() (Classes/Menu.mm:778-847), not
// in update(): `loading` walks 1 -> 2 -> (worldExists? 4 : showAlertWorldType + 3 -> 4) -> and only
// at 4 does it call World::loadWorld(). Wrapping render() away would silently break loading a
// world, and duplicating that ladder here would mean two copies of the port's single most
// load-bearing sequence. So the GL menu keeps rendering underneath, and public/eden-menu.js is an
// opaque full-viewport overlay on top of it — exactly the pattern public/eden-pausemenu.js already
// uses for Hud::renderMenuScreen, for exactly the same reason. Cost: a handful of wasted draw
// calls per frame on a screen that is doing nothing else. Benefit: zero risk to the load path, and
// the "use the legacy GL menu instead" setting is then just "don't show the overlay" — no second
// code path to keep working.
//
// CONVENTIONS FOLLOWED HERE (see web/docs/ui.md "Passing data across the JS/wasm boundary"):
//   * Index-based, never string-in. Every accessor takes an int index into the CURRENT world list.
//   * Strings out go through a static buffer, read with the same utf8(ptr) helper the settings
//     schema uses. Nothing here needs _malloc/_free on the export list.
//   * The one thing that genuinely needs a string INBOUND is the new world's name. Rather than
//     adding malloc to the export list for it, eden_menu_name_buffer() hands JS a pointer to a
//     static buffer this file owns; JS writes UTF-8 bytes into HEAPU8 and calls create. Same
//     "C owns the memory, JS fills it" trick, no allocator involved.
#import "../../../Classes/World.h"
#import "../../../Classes/Menu.h"
#import "../../../Classes/FileManager.h"
#import "../../../Classes/Hud.h"       // eden_hud_draw_menu_screen_hook
#import "../../../Classes/Terrain.h"   // eden_menu_load_percent reads terrain->counter
#import "../../../Classes/SettingsMenu.h"
#import "../../../Classes/Util.h"
#import "../shim/foundation/NSString.h"
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <cstring>
#include <cstdlib>

// Set from Settings_web.mm's "legacy_menu" row. One flag decides which UI the player gets, in all
// three places it matters: eden_menu_active() (the DOM main menu), eden_legacy_ui_active() (read by
// public/eden-pausemenu.js for the DOM in-game menu), and the Hud hook installed just below.
extern float eden_legacy_menu;

// Phase N Stage 2: a host with no DOM UI at all. On web `legacy_menu` is a real player choice
// between two live UIs; on native there is only one — the engine's own GL menus — so the flag has
// nothing to choose between and every DOM-shaped default below would resolve the wrong way. Most
// visibly the in-game (ESC) panel, which the hook further down suppresses by default because the
// DOM pause menu covers it: with no DOM pause menu, Escape sets `hud->inmenu` and then NOTHING
// DRAWS. That is exactly how it presented in the first native play pass — the corner icon's
// press animation played (it always draws) and no panel ever appeared.
//
// A forced override rather than a different default for the row, because this is a property of
// the BUILD and not a preference: native must never resolve it to 0, including if a stale
// NSUserDefaults value says so.
static bool g_forceLegacyUI = false;

extern "C" EDEN_EXPORT
void eden_ui_force_legacy(int on) { g_forceLegacyUI = (on != 0); }

static bool legacy_ui(void) { return g_forceLegacyUI || eden_legacy_menu != 0.0f; }

// The engine's own in-game menu panel (Hud::renderMenuScreen) draws over the world whenever
// hud->inmenu is set. That was invisible while public/eden-pausemenu.js was a full-canvas opaque
// overlay, but it is a shrink-to-fit dialog now, so the 4 GL icons showed THROUGH the scrim behind
// the DOM panel. Suppress the engine's panel unless the player asked for the legacy UI, in which
// case it draws and the DOM panel stays away instead (see eden-pausemenu.js's tick()). Exactly one
// in-game menu is ever on screen.
//
// Installed from a static ctor rather than assigned from some init function: there is no ordering
// hazard (Hud::render cannot run before main()), and it keeps the wiring next to the predicate.
static struct InstallHudMenuHook {
    InstallHudMenuHook() { eden_hud_draw_menu_screen_hook = legacy_ui; }
} g_install_hud_menu_hook;

static Menu* menu_ptr() {
    World* w = World::getWorld;
    return w ? w->menu : NULL;
}

// A single scratch buffer for every string this file returns. Safe because JS decodes the result
// immediately on return, before it can possibly call another export (the whole JS/wasm boundary
// here is synchronous) — the same contract eden_settings_schema() and eden_load_failed_world()
// already rely on.
static char g_out[512];
static const char* out(NSString* s) {
    g_out[0] = 0;
    if (s) {
        const char* c = [s UTF8String];
        if (c) { std::strncpy(g_out, c, sizeof(g_out) - 1); g_out[sizeof(g_out) - 1] = 0; }
    }
    return g_out;
}

// Walk the linked list to the nth node. O(n) per call, and the DOM list is rebuilt by calling this
// once per row — with a realistic world count (tens) that is nothing, and it keeps this file free
// of a cached snapshot that could disagree with Menu's own list after a create/delete.
static WorldNode* node_at(int index) {
    Menu* m = menu_ptr();
    if (!m || index < 0) return NULL;
    WorldNode* n = m->world_list;
    while (n && index-- > 0) n = n->next;
    return n;
}

extern "C" {

// -----------------------------------------------------------------------------------------------
// Visibility
// -----------------------------------------------------------------------------------------------

// True when the DOM menu overlay should be up: the engine is on its menu screen, the player has
// not opted into the legacy GL menu, and none of the engine's own sub-screens that this overlay
// does NOT replace (share flow, shared-world list, settings) is showing. Deliberately conservative
// — if the engine is showing something we have no DOM equivalent for, get out of the way rather
// than covering it with an opaque overlay the player cannot dismiss.
EDEN_EXPORT
int eden_menu_active(void) {
    World* w = World::getWorld;
    Menu* m = menu_ptr();
    if (!w || !m) return 0;
    if (legacy_ui()) return 0;
    if (w->game_mode != GAME_MODE_MENU) return 0;
    if (m->is_sharing || m->showlistscreen || m->showsettings) return 0;
    return 1;
}

// True when the player has opted back into the original 2010 GL UI. Every DOM surface that has a
// GL counterpart checks this, so the two can never both be on screen.
EDEN_EXPORT
int eden_legacy_ui_active(void) { return legacy_ui() ? 1 : 0; }

// Non-zero while a world load is in flight. The DOM menu uses this to show its own progress state
// and to stop accepting input — the engine is mid-`loading` ladder and a second Play tap would
// re-enter it.
EDEN_EXPORT
int eden_menu_loading(void) {
    Menu* m = menu_ptr();
    return (m && m->loading) ? 1 : 0;
}

/** 0-100 load progress, so the DOM menu can show a real bar instead of an indefinite spinner.
 *  Same expression World::loadWorld uses for its own status line (Classes/World.mm:354) — the
 *  terrain streamer counts up to 324 columns — kept in step by deriving it from the same field
 *  rather than inventing a second notion of "done". */
EDEN_EXPORT
int eden_menu_load_percent(void) {
    World* w = World::getWorld;
    if (!w || !w->terrain) return 0;
    int pct = (int)(100.0f * (float)(w->terrain->counter) / 324.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// -----------------------------------------------------------------------------------------------
// World list
// -----------------------------------------------------------------------------------------------
EDEN_EXPORT
int eden_menu_world_count(void) {
    Menu* m = menu_ptr();
    if (!m) return 0;
    int n = 0;
    for (WorldNode* p = m->world_list; p; p = p->next) ++n;
    return n;
}

/** The player-facing name (FileManager::getName's value, not the hashed filename). */
EDEN_EXPORT
const char* eden_menu_world_name(int index) {
    WorldNode* n = node_at(index);
    return out(n ? n->display_name : NULL);
}

/** The on-disk filename, e.g. "a3f9c1.eden". The DOM list joins on this to pick up size/mtime
 *  from eden_storage_list_worlds() rather than duplicating a stat() here. */
EDEN_EXPORT
const char* eden_menu_world_file(int index) {
    WorldNode* n = node_at(index);
    return out(n ? n->file_name : NULL);
}

EDEN_EXPORT
int eden_menu_selected_index(void) {
    Menu* m = menu_ptr();
    if (!m || !m->selected_world) return -1;
    int i = 0;
    for (WorldNode* p = m->world_list; p; p = p->next, ++i)
        if (p == m->selected_world) return i;
    return -1;
}

/** Select a world. Mirrors the GL menu's own "tapped a world block that wasn't selected" branch
 *  (Classes/Menu.mm:519-520), including the filename status bar it sets — that bar still renders
 *  underneath and would otherwise name the previously selected world. */
EDEN_EXPORT
void eden_menu_select(int index) {
    Menu* m = menu_ptr();
    WorldNode* n = node_at(index);
    if (!m || !n) return;
    m->selected_world = n;
    m->refreshfn();
}

// -----------------------------------------------------------------------------------------------
// Actions
// -----------------------------------------------------------------------------------------------

/** Play the selected world. `loading = 1` is exactly what the GL menu's tap handler sets
 *  (Classes/Menu.mm:513-516); Menu::render's ladder takes it from there. Guarded against
 *  re-entry the same way the original is. */
EDEN_EXPORT
int eden_menu_play(void) {
    Menu* m = menu_ptr();
    if (!m || !m->selected_world) return 0;
    if (m->loading != 0) return 0;
    m->loading = 1;
    m->sbar->setStatus(@"Loading ", 9999);
    return 1;
}

/** Pointer to the static buffer JS writes a new world's name into (UTF-8, NUL-terminated).
 *  See this file's header for why this exists instead of a malloc'd string parameter. */
static char g_name_buf[128];
EDEN_EXPORT
char* eden_menu_name_buffer(void) {
    g_name_buf[0] = 0;
    return g_name_buf;
}
EDEN_EXPORT
int eden_menu_name_buffer_size(void) { return (int)sizeof(g_name_buf); }

/** Create a world and select it — a transcription of the GL menu's rect_create branch
 *  (Classes/Menu.mm:537-550), differing only in where the name comes from: the buffer above if JS
 *  filled it, otherwise SettingsMenu::getNewWorldName() exactly as before, so an empty name field
 *  still produces the engine's own auto-generated name rather than a blank entry.
 *
 *  Note this only creates the LIST ENTRY. No file exists until the world is played and the
 *  terrain generator runs — which is why the flat/normal choice below is answered at Play time,
 *  not here. That is the engine's own design, not a shortcut.
 *
 *  Returns the new world's index, or -1. */
EDEN_EXPORT
int eden_menu_create_world(void) {
    Menu* m = menu_ptr();
    if (!m) return -1;
    WorldNode* nw = (WorldNode*)malloc(sizeof(WorldNode));
    if (!nw) return -1;
    memset(nw, 0, sizeof(WorldNode));

    NSString* name = NULL;
    if (g_name_buf[0]) {
        g_name_buf[sizeof(g_name_buf) - 1] = 0;
        name = [NSString stringWithUTF8String:g_name_buf];
    }
    if (!name) name = m->settings->getNewWorldName();
    nw->display_name = name;
    nw->file_name = [NSString stringWithFormat:@"%@.eden", genhash()];
    [nw->file_name retain];
    [nw->display_name retain];
    m->addWorld(nw);
    m->selected_world = nw;
    m->refreshfn();
    g_name_buf[0] = 0;
    Resources::getResources->playSound(S_MODE_SELECTION);
    return eden_menu_selected_index();
}

/** Delete the world at `index`.
 *
 *  This is the ONLY working delete path in the web port. The GL menu routes delete through
 *  showAlertDeleteConfirm(), which seam_link_stubs.mm implements as a deliberate no-op (not
 *  confirming a destructive prompt is the safe default when you have no dialog) — so the engine's
 *  own delete button has never actually deleted anything here. The DOM menu asks for confirmation
 *  itself and then calls this, which runs the same Menu::a_deleteConfirm() the alert delegate
 *  would have. */
EDEN_EXPORT
int eden_menu_delete_at(int index) {
    Menu* m = menu_ptr();
    WorldNode* n = node_at(index);
    if (!m || !n) return 0;
    m->selected_world = n;
    m->a_deleteConfirm();
    return 1;
}

// -----------------------------------------------------------------------------------------------
// World type (Flat / Normal)
// -----------------------------------------------------------------------------------------------
// The engine asks this question with a modal at load time, from inside Menu::render's `loading==2`
// branch (showAlertWorldType -> Menu::a_genFlat -> loading 3->4). The DOM New World screen asks it
// UP FRONT instead, as a tab rail, which is what the mockups show. So the choice is parked here and
// seam_link_stubs.mm's showAlertWorldType() consumes it instead of raising the modal.
//
// "take" semantics on purpose: the pending choice is one-shot. A second world created without
// going through the New World screen (or a world loaded from the legacy GL menu) must fall back to
// the real dialog rather than silently inheriting the last screen's answer.
static int g_pending_world_type = -1;   // -1 none, 0 normal, 1 flat

EDEN_EXPORT
void eden_menu_set_pending_world_type(int flat) { g_pending_world_type = flat ? 1 : 0; }

EDEN_EXPORT
void eden_menu_clear_pending_world_type(void) { g_pending_world_type = -1; }

/** Consumed by showAlertWorldType() in seam_link_stubs.mm. Returns -1 when nothing is pending, in
 *  which case the caller must raise the real dialog. */
EDEN_EXPORT
int eden_menu_take_pending_world_type(void) {
    int v = g_pending_world_type;
    g_pending_world_type = -1;
    return v;
}

// -----------------------------------------------------------------------------------------------
// World height (64z / 256z "New Dawn") -- 256z Stage 3 item 4
// -----------------------------------------------------------------------------------------------
// Same "park a choice up front, one-shot take" pattern as world type above, consumed from
// Classes/FileManager.mm's probeWorldHeight() instead of a seam file, because probeWorldHeight is
// the one place that already decides a not-yet-existing world's height (see its own comment).
// 64z remains the default: eden_menu_take_pending_world_height() returns -1 (nothing pending)
// unless the New World screen explicitly set 256.
static int g_pending_world_height = -1;   // -1 none, 64 or 256

EDEN_EXPORT
void eden_menu_set_pending_world_height(int height) {
    g_pending_world_height = (height == 256) ? 256 : 64;
}

EDEN_EXPORT
void eden_menu_clear_pending_world_height(void) { g_pending_world_height = -1; }

/** Consumed by FileManager::probeWorldHeight() (Classes/FileManager.mm). Returns -1 when nothing
 *  is pending, in which case the caller stays at the 64z default. */
EDEN_EXPORT
int eden_menu_take_pending_world_height(void) {
    int v = g_pending_world_height;
    g_pending_world_height = -1;
    return v;
}

/** Open the engine's settings screen (which the DOM settings panel mirrors via
 *  eden_settings_menu_open()). Same flag the GL menu's Options button sets. */
EDEN_EXPORT
void eden_menu_open_settings(void) {
    Menu* m = menu_ptr();
    if (m) m->showsettings = TRUE;
}

} // extern "C"
