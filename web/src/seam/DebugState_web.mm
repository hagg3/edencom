// DebugState_web.mm — TEMPORARY debug probe for the "menu layout is broken" investigation
// (web/docs/archive/PORT-STATUS-2026-08-13.md "Known open issue: menu layout", opened Pass 18).
//
// *** DELETE THIS FILE (and its CMakeLists.txt EDEN_SEAM_SOURCES entry) once the layout bug is
// fixed or the investigation moves on. *** It exists only so a live browser session can read
// Menu.mm's actual runtime rect/flag state directly via JS, instead of inferring state from
// screenshots (Pass 20 hit a false positive doing the latter — see archive/PORT-STATUS-2026-08-13.md "Pass 20").
// Same "temporary probe, delete after" pattern as Pass 17/18's throwaway fprintf instrumentation.
//
// Exposed as EDEN_EXPORT `eden_debug_menu_state()`, callable from JS as
// `UTF8ToString(Module._eden_debug_menu_state())` — returns a static buffer (not exported
// dynamically, single string, single frame use — no ownership/free story needed, matches this
// port's other simple debug exports).
#import "../shim/foundation/uikit_stubs.h"
#include "gl_es1_shim.h"                     // eden_gl_glGetIntegerv, for the D1 pick-viewport probe
#include "../../../Classes/World.h"
#include "../../../Classes/Globals.h"
#include "../../../Classes/Input.h"
#include "../../../Classes/Constants.h"       // NUM_CREATURES, for the pass-27 model probe
#include "../../../Classes/PVRTModelPOD.h"    // CPVRTModelPOD, ditto
#include "../../../Classes/Resources.h"       // audit row 11 (A5) recolor probe
#include "DisplayProfile_web.h"               // audit D1/D4 display-profile probe
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <cstdio>

// Globals.mm-only (not declared in Globals.h — Pass 16 already ran into this for the same reason).
extern BOOL IS_WIDESCREEN;
// Classes/Joystick.mm's on-screen pad, in point space (bottom-left origin, like every other rect
// reported here). Phase N Stage 4.3's second device pass: --touch-selftest could tap a HUD BUTTON
// but had no way to hold the STICK, and "the joystick is broken" was one of the four bugs it
// missed. Reported rather than hard-coded for the same reason `rmenu` is — a test that guesses a
// coordinate can pass by hitting the wrong thing.
extern CGRect padbounds;
// Declared in Globals.h, but with C++ linkage — so it has to be re-declared HERE, outside the
// `extern "C"` block below, not inside the one function that reads it.
extern float P_ASPECT_RATIO;

extern "C" {

// Pass 23 addition: player/hud state for headlessly verifying the new desktop keyboard/mouse
// controls (web/src/seam/Input_web.mm) — same "TEMPORARY, delete with this file" status as the
// rest of this probe.
EDEN_EXPORT
const char* eden_debug_player_state(void) {
    static char buf[512];
    if (!World::getWorld || !World::getWorld->player || !World::getWorld->hud) {
        snprintf(buf, sizeof(buf), "{\"error\":\"no World/Player/Hud yet\"}");
        return buf;
    }
    Player* p = World::getWorld->player;
    Hud* h = World::getWorld->hud;
    snprintf(buf, sizeof(buf),
        "{"
        "\"pos\":[%.3f,%.3f,%.3f],\"yaw\":%.3f,\"pitch\":%.3f,"
        "\"walk_force\":[%.3f,%.3f,%.3f],\"max_walk_speed\":%.3f,"
        "\"hud_mode\":%d,\"blocktype\":%d"
        "}",
        p->pos.x, p->pos.y, p->pos.z, p->yaw, p->pitch,
        p->walk_force.x, p->walk_force.y, p->walk_force.z, p->max_walk_speed,
        h->mode, h->blocktype);
    return buf;
}

// Pass 27 addition: did the creature PODs actually load, and is anything alive to draw?
// `models[]` and `model_render_count` are plain globals in Classes/Model.mm (not file-static), so
// they can be read from here. This is the headless proof that the GL_OES_matrix_palette emulation
// worked end to end: nNumMesh > 0 means LoadModels() got past its extension gate AND
// ReadFromFile() succeeded for that creature. Same "TEMPORARY, delete with this file" status.
EDEN_EXPORT
const char* eden_debug_model_state(void) {
    static char buf[512];
    extern CPVRTModelPOD models[];
    extern int model_render_count;
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "{\"nNumMesh\":[");
    for (int i = 0; i < NUM_CREATURES; ++i)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%u", i ? "," : "", models[i].nNumMesh);
    n += snprintf(buf + n, sizeof(buf) - n, "],\"nNumFrame\":[");
    for (int i = 0; i < NUM_CREATURES; ++i)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%u", i ? "," : "", models[i].nNumFrame);
    snprintf(buf + n, sizeof(buf) - n, "],\"render_count\":%d}", model_render_count);
    return buf;
}

// Pass 27 addition: the in-game menu's button rects, so a headless drive can tap "exit to menu"
// (the only path that reaches World::exitToMenu -> UnloadModels) through the real touch path.
// Rects are in itouch's bottom-left-origin, Y-up space. TEMPORARY, same as the rest of this file.
EDEN_EXPORT
const char* eden_debug_hud_rects(void) {
    static char buf[256];
    if (!World::getWorld || !World::getWorld->hud) { snprintf(buf, sizeof(buf), "{}"); return buf; }
    Hud* h = World::getWorld->hud;
    snprintf(buf, sizeof(buf), "{\"rexit\":[%.2f,%.2f,%.2f,%.2f],\"rhome\":[%.2f,%.2f,%.2f,%.2f]}",
             h->rexit.origin.x, h->rexit.origin.y, h->rexit.size.width, h->rexit.size.height,
             h->rhome.origin.x, h->rhome.origin.y, h->rhome.size.width, h->rhome.size.height);
    return buf;
}

// Audit D1/D4 addition: the derived point space + the HUD rects that actually move with it. This
// is what tools/headless-display-profile-test.js asserts against — the whole point of unpinning
// SCREEN_WIDTH/SCREEN_HEIGHT is that they now have more than one correct value, so "did the layout
// follow?" stopped being answerable by reading a constant out of the source. rjumphit/rmenu are the
// two extremes (right-edge-anchored and top-left-anchored), blockBounds[0] is the block picker's
// first cell (the one whose margin has the hand-tuned widescreen nudge in it), and rmine is on the
// right-hand mode column that HUDR_X drives.
EDEN_EXPORT
const char* eden_debug_display_state(void) {
    static char buf[1280];
    const char* profile = eden_profile_name();
    if (!World::getWorld || !World::getWorld->hud) {
        snprintf(buf, sizeof(buf),
                 "{\"profile\":\"%s\",\"SCREEN_WIDTH\":%.1f,\"SCREEN_HEIGHT\":%.1f,"
                 "\"P_ASPECT_RATIO\":%.4f,\"IS_WIDESCREEN\":%d,\"hud\":false}",
                 profile, SCREEN_WIDTH, SCREEN_HEIGHT, P_ASPECT_RATIO, (int)IS_WIDESCREEN);
        return buf;
    }
    Hud* h = World::getWorld->hud;
    Input* in = Input::getInput();
    // What Util.mm's findWorldCoords will unproject a tap against. The contract the shim's
    // kPickViewport exists to hold is exactly `viewport == point space x SCALE_*`; reporting it
    // here is what lets the headless test assert that instead of trusting it.
    GLint vp[4] = {0, 0, 0, 0};
    eden_gl_glGetIntegerv(GL_VIEWPORT, vp);
    snprintf(buf, sizeof(buf),
        "{\"profile\":\"%s\",\"SCREEN_WIDTH\":%.1f,\"SCREEN_HEIGHT\":%.1f,"
        "\"P_ASPECT_RATIO\":%.4f,\"IS_WIDESCREEN\":%d,\"hud\":true,"
        "\"point_w\":%d,\"point_h\":%d,\"aspect_x1000\":%d,\"touch_chrome\":%d,"
        "\"pick_viewport\":[%d,%d],"
        "\"scr\":[%d,%d],\"use_joystick\":%d,"
        "\"rjoystick\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rmine\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rjumphit\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rmenu\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rmenuframe\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rpaintframe\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rburn\":[%.1f,%.1f,%.1f,%.1f],"
        "\"rpaint\":[%.1f,%.1f,%.1f,%.1f],"
        "\"block0\":[%.1f,%.1f,%.1f,%.1f],"
        "\"color0\":[%.1f,%.1f,%.1f,%.1f]}",
        profile, SCREEN_WIDTH, SCREEN_HEIGHT, P_ASPECT_RATIO, (int)IS_WIDESCREEN,
        eden_display_point_width(), eden_display_point_height(), eden_display_aspect_x1000(),
        eden_profile_touch_chrome(),
        (int)vp[2], (int)vp[3],
        in->scr_width, in->scr_height, h->use_joystick,
        padbounds.origin.x, padbounds.origin.y, padbounds.size.width, padbounds.size.height,
        h->rmine.origin.x, h->rmine.origin.y, h->rmine.size.width, h->rmine.size.height,
        h->rjumphit.origin.x, h->rjumphit.origin.y, h->rjumphit.size.width, h->rjumphit.size.height,
        h->rmenu.origin.x, h->rmenu.origin.y, h->rmenu.size.width, h->rmenu.size.height,
        h->rmenuframe.origin.x, h->rmenuframe.origin.y,
        h->rmenuframe.size.width, h->rmenuframe.size.height,
        h->rpaintframe.origin.x, h->rpaintframe.origin.y,
        h->rpaintframe.size.width, h->rpaintframe.size.height,
        h->rburn.origin.x, h->rburn.origin.y, h->rburn.size.width, h->rburn.size.height,
        h->rpaint.origin.x, h->rpaint.origin.y, h->rpaint.size.width, h->rpaint.size.height,
        h->blockBounds[0].origin.x, h->blockBounds[0].origin.y,
        h->blockBounds[0].size.width, h->blockBounds[0].size.height,
        h->colorBounds[0].origin.x, h->colorBounds[0].origin.y,
        h->colorBounds[0].size.width, h->colorBounds[0].size.height);
    return buf;
}

EDEN_EXPORT
const char* eden_debug_menu_state(void) {
    static char buf[3072];
    if (!World::getWorld || !World::getWorld->menu) {
        snprintf(buf, sizeof(buf), "{\"error\":\"no World/Menu yet\"}");
        return buf;
    }
    Menu* m = World::getWorld->menu;
    int worldCount = 0;
    for (WorldNode* n = m->world_list; n != NULL; n = n->next) worldCount++;
    const char* selName = (m->selected_world && m->selected_world->display_name)
        ? [m->selected_world->display_name UTF8String] : "";

    itouch* touches = Input::getInput()->getTouches();
    char touchBuf[1280];
    int tn = 0;
    tn += snprintf(touchBuf + tn, sizeof(touchBuf) - tn, "[");
    for (int i = 0; i < MAX_TOUCHES; i++) {
        tn += snprintf(touchBuf + tn, sizeof(touchBuf) - tn,
            "%s{\"mx\":%d,\"my\":%d,\"inuse\":%d,\"down\":%d,\"moved\":%d,"
            // pass 27: previewtype/preview/etime are what Player::render gates the translucent
            // ghost block on — the only way to see the block-preview toggle working headlessly.
            "\"previewtype\":%d,\"preview\":[%d,%d,%d],\"etime\":%.3f}",
            i ? "," : "", touches[i].mx, touches[i].my, touches[i].inuse,
            touches[i].down, touches[i].moved, touches[i].previewtype,
            touches[i].preview.x, touches[i].preview.y, touches[i].preview.z,
            touches[i].etime);
    }
    snprintf(touchBuf + tn, sizeof(touchBuf) - tn, "]");

    snprintf(buf, sizeof(buf),
        "{"
        "\"SCREEN_WIDTH\":%.3f,\"SCREEN_HEIGHT\":%.3f,"
        "\"SCALE_WIDTH\":%.3f,\"SCALE_HEIGHT\":%.3f,"
        "\"IS_IPAD\":%d,\"IS_RETINA\":%d,\"IS_WIDESCREEN\":%d,\"SUPPORTS_RETINA\":%d,"
        "\"showsettings\":%d,\"showlistscreen\":%d,\"is_sharing\":%d,\"loading\":%d,"
        "\"loading_world_list\":%d,"
        "\"delete_mode\":%d,\"selected_world_ptr\":\"%p\",\"selected_world_name\":\"%s\","
        "\"world_count\":%d,"
        "\"rect_options\":[%.2f,%.2f,%.2f,%.2f],"
        "\"rect_create\":[%.2f,%.2f,%.2f,%.2f],"
        "\"rect_delete\":[%.2f,%.2f,%.2f,%.2f],"
        "\"rect_share\":[%.2f,%.2f,%.2f,%.2f],"
        "\"rect_loadshared\":[%.2f,%.2f,%.2f,%.2f],"
        "\"left_arrow\":[%.2f,%.2f,%.2f,%.2f],"
        "\"right_arrow\":[%.2f,%.2f,%.2f,%.2f],"
        "\"selected_world_rect\":[%.2f,%.2f,%.2f,%.2f],"
        "\"doneLoading\":%d,\"game_mode\":%d,"
        "\"touches\":%s"
        "}",
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SCALE_WIDTH, SCALE_HEIGHT,
        (int)IS_IPAD, (int)IS_RETINA, (int)IS_WIDESCREEN, (int)SUPPORTS_RETINA,
        (int)m->showsettings, (int)m->showlistscreen, m->is_sharing, m->loading,
        m->loading_world_list,
        (int)m->delete_mode, (void*)m->selected_world, selName,
        worldCount,
        m->rect_options.origin.x, m->rect_options.origin.y,
        m->rect_options.size.width, m->rect_options.size.height,
        m->rect_create.origin.x, m->rect_create.origin.y,
        m->rect_create.size.width, m->rect_create.size.height,
        m->rect_delete.origin.x, m->rect_delete.origin.y,
        m->rect_delete.size.width, m->rect_delete.size.height,
        m->rect_share.origin.x, m->rect_share.origin.y,
        m->rect_share.size.width, m->rect_share.size.height,
        m->rect_loadshared.origin.x, m->rect_loadshared.origin.y,
        m->rect_loadshared.size.width, m->rect_loadshared.size.height,
        m->left_arrow.origin.x, m->left_arrow.origin.y,
        m->left_arrow.size.width, m->left_arrow.size.height,
        m->right_arrow.origin.x, m->right_arrow.origin.y,
        m->right_arrow.size.width, m->right_arrow.size.height,
        m->selected_world ? m->selected_world->rect.origin.x : -1,
        m->selected_world ? m->selected_world->rect.origin.y : -1,
        m->selected_world ? m->selected_world->rect.size.width : -1,
        m->selected_world ? m->selected_world->rect.size.height : -1,
        World::getWorld->doneLoading, World::getWorld->game_mode,
        touchBuf);
    return buf;
}

// TEMPORARY (pass 23, delete with the rest of this probe): reports the raw engine heap pointers.
// Hunting a low-memory zero-fill that clobbers emscripten's stack cookie the instant a world
// loads: Terrain::clearBlocks / updateLightingBegin memset lightarray unconditionally, so a
// failed malloc would splatter ~16 MB of zeros starting at address 0.
EDEN_EXPORT
extern "C" const char *eden_debug_alloc_state(void) {
    extern block8 *blockarray;
    extern Vector8 *lightarray;
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"blockarray\":%u,\"lightarray\":%u,\"lightarray_bytes\":%u}",
             (unsigned)(uintptr_t)blockarray, (unsigned)(uintptr_t)lightarray,
             (unsigned)(sizeof(Vector8) * T_SIZE * T_SIZE * T_HEIGHT));
    return buf;
}

// Audit row 11 (A5) regression probe. The recolor pipeline is entirely invisible from the outside
// — its failure mode is a Texture2D whose GL `name` stayed 0, which draws as nothing and reports
// no error — so there is no way to assert it from a headless harness without asking the engine
// directly. Reports, for one paint colour, whether each input image survived load and whether the
// recolored texture actually got a GL name. `stored_*` being 0 means initFromPath's storeImage
// block didn't fire (asset missing, or the positional classification drifted); `paint_tex` being 0
// with both inputs present means ManipulateImagePixelData returned null.
//
// Colour 0 is deliberately rejected: Resources::getPaintTex short-circuits it to the plain
// ICO_PAINT atlas texture and never reaches the recolor at all, so probing it would pass whether
// or not the pipeline works. tools/headless-recolor-test.js drives this.
EDEN_EXPORT
extern "C" const char *eden_debug_recolor_state(int color) {
    static char buf[512];
    extern int realStoredSkinCounter;
    extern int storedMaskCounter;
    extern UIImage *storedPaint;
    extern UIImage *storedPaintMask;
    extern UIImage *storedDoor;
    extern UIImage *storedDoorMask;
    if (!Resources::getResources || color <= 0) {
        snprintf(buf, sizeof(buf), "{\"error\":\"no Resources, or color<=0 (see comment)\"}");
        return buf;
    }
    CGImageRef paintCG = [storedPaint CGImage];
    CGImageRef maskCG = [storedPaintMask CGImage];
    Texture2D *paintTex = Resources::getResources->getPaintTex(color);
    int doorTex = Resources::getResources->getDoorTex(color);

    // The creature half of the same pipeline. These two 5x2 tables are filled POSITIONALLY (the
    // Nth texture load since Resources::loadResources zeroed the counter), which is the part most
    // likely to drift silently if anyone reorders a texture load — a wrong count here means
    // creatures get each other's skins, with nothing else to notice it by.
    extern UIImage *storedSkins[5][2];
    extern UIImage *storedMasks[5][2];
    int skinsFilled = 0, masksFilled = 0;
    for (int m = 0; m < 5; m++) {
        for (int s = 0; s < 2; s++) {
            if (storedSkins[m][s] != nil) skinsFilled++;
            if (storedMasks[m][s] != nil) masksFilled++;
        }
    }

    snprintf(buf, sizeof(buf),
             "{\"color\":%d,\"stored_paint\":%d,\"stored_paint_mask\":%d,"
             "\"stored_door\":%d,\"stored_door_mask\":%d,"
             "\"paint_w\":%d,\"paint_h\":%d,\"mask_w\":%d,\"mask_h\":%d,"
             "\"paint_tex\":%u,\"door_tex\":%d,"
             "\"skins_filled\":%d,\"masks_filled\":%d,\"skin_counter\":%d,\"mask_counter\":%d}",
             color, storedPaint != nil, storedPaintMask != nil,
             storedDoor != nil, storedDoorMask != nil,
             paintCG ? paintCG->width : -1, paintCG ? paintCG->height : -1,
             maskCG ? maskCG->width : -1, maskCG ? maskCG->height : -1,
             paintTex ? (unsigned)paintTex->name : 0u, doorTex,
             skinsFilled, masksFilled, realStoredSkinCounter, storedMaskCounter);
    return buf;
}

// 256z ("New Dawn") runtime-height probe (Stage 2). The world height is a per-world runtime value
// now (Classes/Constants.h), and every downstream size derives from it -- so the one thing a test
// must be able to ask is "which height did this world actually open at, and did the derived
// per-file quantities follow?". creature_slots in particular is derived from the FILE, not the
// version (the sibling world editor writes v5 saves with no creature block at all), so it is the
// number most worth asserting. tools/headless-256z-test.js drives this.
EDEN_EXPORT
extern "C" const char *eden_debug_world_format(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"height\":%d,\"bands\":%d,\"column_bytes\":%d,\"creature_slots\":%d,"
             "\"t_blocks\":%d,\"xz_stride\":%d}",
             g_world_height, g_chunks_per_column, g_column_bytes, g_max_creatures_saved,
             g_t_blocks, g_xz_stride);
    return buf;
}

// B3 Stage 2 (off-thread meshing): whole-window geometry integrity + a comparable fingerprint.
// The plan doc's R1 asks for a whole-window integrity scan as the tool for chasing a count/fill
// heap overrun; this is that scan, and it doubles as the equivalence check the change actually
// needs. Vertex COUNTS depend only on block types and face visibility -- not on lighting, which is
// baked into vertex colours and is sliced across frames -- so after the same teleports over the
// same deterministic default world, `rt_vertices` must come out byte-identical whether the meshing
// ran inline or on a worker. A worker mesher that dropped, duplicated or truncated a chunk shows up
// here as a different total; `unpublished` catches the other failure mode, a chunk left holding a
// built-but-never-uploaded mesh (needsVBO still set), which is what "the chunk went invisible"
// looks like from the inside. Diagnostics-only, like everything else in this file.
EDEN_EXPORT
extern "C" const char *eden_debug_terrain_geometry(void) {
    static char buf[256];
    long long verts = 0, verts2 = 0, objects = 0;
    int meshed = 0, unpublished = 0, busy = 0, total = 0;
    extern TerrainChunk** chunkTablec;
    if (chunkTablec) {
        const int n = CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN;
        for (int i = 0; i < n; i++) {
            TerrainChunk* c = chunkTablec[i];
            if (!c) continue;
            total++;
            verts   += c->rtn_vertices;
            verts2  += c->rtn_vertices2;
            objects += c->rtnum_objects;
            if (c->rtn_vertices || c->rtn_vertices2) meshed++;
            if (c->needsVBO) unpublished++;
            if (c->meshJobState != MESH_JOB_IDLE) busy++;
        }
    }
    snprintf(buf, sizeof(buf),
             "{\"chunks\":%d,\"meshed\":%d,\"rt_vertices\":%lld,\"rt_vertices2\":%lld,"
             "\"rt_objects\":%lld,\"unpublished\":%d,\"jobs_in_flight\":%d}",
             total, meshed, verts, verts2, objects, unpublished, busy);
    return buf;
}

// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md), criterion 2: a CONTENT
// fingerprint of the meshed window, not just the counts eden_debug_terrain_geometry() reports.
// Counts prove no chunk was dropped or duplicated; they say nothing about whether the vertices are
// in the right places. This is what makes "byte-identical geometry between the web build and the
// native build" a checkable claim rather than an assertion — and, per the plan's rule 4, it exists
// on BOTH targets, which also strengthens tools/safari-threaded-mesher-check.js (it compares
// counts today).
//
// WHY THE PER-VERTEX WORK IS IN THE ENGINE AND ONLY THE FOLD IS HERE. The obvious implementation —
// walk chunkTablec and hash each chunk's verticesbg/verticesbg2 — reports zero vertices, and that
// is not a bug in the walk: TerrainChunk::prepareVBO() uploads those arrays to the chunk's VBO and
// then free()s them, so in a settled world the only copy of the vertex data is inside a GL buffer,
// which a headless run has no context to read back. The hash therefore has to be taken at publish
// time; Classes/TerrainChunk.mm does it there, into two durable per-chunk fields, and this export
// folds those in chunkTablec index order (a fixed toroidal layout, so the traversal is
// deterministic across platforms for a given player position).
//
// TWO CHECKSUMS, DELIBERATELY, because they fail for different reasons:
//   `geom`  — position + texcoord only. Pure functions of block types and face visibility, so they
//             must match EXACTLY between any two runs over the same world at the same origin, on
//             any platform. This is the number the cross-target comparison turns on.
//   `full`  — the above plus the baked vertex colours, which carry lighting. Lighting is propagated
//             incrementally and sliced across frames, so two runs agree here only after the window
//             has settled for the same number of frames. A `geom` match with a `full` mismatch
//             means "same geometry, lighting still converging" — information, not noise.
//
// `hashed` reports how many chunks actually carry a fingerprint. It is 0 unless
// eden_debug_set_mesh_checksum(1) was called BEFORE the world was meshed — the per-vertex pass is
// opt-in so it stays off prepareVBO()'s hot path — and a probe that forgets would otherwise read
// a confident, meaningless "the two targets agree".
EDEN_EXPORT
extern "C" void eden_debug_set_mesh_checksum(int on) {
    extern bool g_mesh_checksum_enabled;   // Classes/TerrainChunk.mm
    g_mesh_checksum_enabled = (on != 0);
}

EDEN_EXPORT
extern "C" const char *eden_debug_mesh_checksum(void) {
    static char buf[256];
    extern TerrainChunk** chunkTablec;

    const unsigned long long kFnvOffset = 1469598103934665603ULL;
    const unsigned long long kFnvPrime  = 1099511628211ULL;
    unsigned long long hGeom = kFnvOffset, hFull = kFnvOffset;
    long long verts = 0;
    int chunks = 0, hashed = 0;

    auto mix = [&](unsigned long long& h, unsigned long long v) {
        const unsigned char* b = (const unsigned char*)&v;
        for (size_t i = 0; i < sizeof(v); ++i) { h ^= b[i]; h *= kFnvPrime; }
    };

    if (chunkTablec) {
        const int n = CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN;
        for (int i = 0; i < n; i++) {
            TerrainChunk* c = chunkTablec[i];
            if (!c) continue;
            chunks++;
            verts += c->rtn_vertices + c->rtn_vertices2;
            if (c->rtGeomHash) hashed++;
            mix(hGeom, c->rtGeomHash);
            mix(hFull, c->rtFullHash);
        }
    }
    snprintf(buf, sizeof(buf),
             "{\"chunks\":%d,\"hashed\":%d,\"vertices\":%lld,"
             "\"geom\":\"%016llx\",\"full\":\"%016llx\"}",
             chunks, hashed, verts, hGeom, hFull);
    return buf;
}

// B5 (256z Stage 3): force the save strategy for a test. Above g_save_inplace_threshold saveWorld
// writes the world file in place behind a rollback journal; below it, it runs on a whole-file
// scratch copy. Real worlds pick a path by their size, which would make the in-place path
// untestable without a multi-hundred-megabyte fixture -- so a test sets the threshold to 0
// ("always in place") or to a huge number ("never") and drives an ordinary small world through
// both. Diagnostics-only, like everything else in this file. tools/headless-save-inplace-test.js.
EDEN_EXPORT
extern "C" void eden_debug_set_save_inplace_threshold(double bytes) {
    eden_set_save_inplace_threshold(bytes < 0 ? 0ull : (unsigned long long)bytes);
}

EDEN_EXPORT
extern "C" double eden_debug_get_save_inplace_threshold(void) {
    return (double)g_save_inplace_threshold;
}

// C3: turn the rollback journal's phase 2 (the dirty-column pre-images that make an in-place save
// fully atomic) off. Two callers: tools/headless-opfs-mirror-test.js A/Bs the per-save byte cost
// with it on and off, and tools/headless-save-inplace-test.js needs B5's old behaviour to prove
// the new records are what closes the tearing gap rather than something else having changed.
EDEN_EXPORT
extern "C" void eden_debug_set_save_journal_columns(int on) {
    eden_set_save_journal_columns(on);
}

EDEN_EXPORT
extern "C" int eden_debug_get_save_journal_columns(void) {
    return g_save_journal_columns;
}

} // extern "C"
