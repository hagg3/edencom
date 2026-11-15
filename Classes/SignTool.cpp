//
//  SignTool.mm
//  Eden — Stage D / D.3c. See SignTool.h.
//
#include "SignTool.h"
#include "Globals.h"
#include "World.h"
#include "Terrain.h"
#include "FileManager.h"
#include "Hud.h"
#include "Util.h"
#include "Resources.h"
#include "GLDialog.h"
#include "Player.h"
#include "Liquids.h"
#include "MpWorld.h"   // MP.4: signs out
#include "ModScript.h"  // K.6-3: the `sign` event
#include "SignRenderer.h"
#include "WorldTrailer.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

namespace {

// The prompt is up for this (anchor, face). Looked up again when it is answered: the trailer's
// indices move on any removal, and a TNT blast may take the anchor while the prompt is open.
struct Pending { bool on; TrailerPos pos; int face, facing; };
Pending g_pending = {false, {0, 0, 0}, 0, 0};

const int kDefaultColour = 45;   // charcoal: Emod's no-paint board (the 2026 game writes 2)

int buildPaint() {
    const int c = World::getWorld->hud->block_paintcolor;
    return (c >= 1 && c <= 54) ? c : 0;
}

// The one commit both the prompt and eden.setsign go through: add / retext (+recolour) / delete one
// sign, drop the renderer's texture for the old text, tell the server. `colour` 0 = keep an existing
// board's, charcoal for a new one. Returns the WorldTrailer result (nothing changed unless TR_OK).
int commitSign(const TrailerPos& pos, int face, int facing, const std::string& clean, bool blank, int colour, bool sound) {
    World* w = World::getWorld;
    WorldTrailer* t = w->fm->trailer();
    const int existing = t->findSign(pos, face);
    char old[96] = {0};
    if (existing >= 0) { memcpy(old, t->sign((size_t)existing).text, 95); old[95] = 0; }
    int r = WorldTrailer::TR_OK;
    if (blank) {
        if (existing >= 0) r = t->removeSign((size_t)existing);   // an empty edit deletes the sign
    } else if (existing >= 0) {
        r = t->setSignText((size_t)existing, clean.c_str());
        if (r == WorldTrailer::TR_OK && colour) r = t->setSignColour((size_t)existing, colour);
    } else {
        r = t->addSign(pos, face, colour ? colour : kDefaultColour, facing, clean.c_str());
        if (r == WorldTrailer::TR_OK && sound) Resources::getResources->playSound(S_BUILD_WOOD);
    }
    if (r != WorldTrailer::TR_OK) return r;
    if (old[0] && !SignTool::testNoCache) SignRenderer::forget(old);
    // MP.4 (engine plan §10): a placed or edited sign goes to the server as it now stands. An
    // emptied one (removed) sends nothing, as retail: the server drops signs with their block.
    const int now = blank ? -1 : t->findSign(pos, face);
    if (now >= 0) {
        const TrailerSign& sg = t->sign((size_t)now);
        char text[96];
        memcpy(text, sg.text, 95);
        text[95] = 0;
        MpWorld::intentSign(sg.x, sg.y, sg.z, sg.a, sg.b, sg.c, text);
    }
    return r;
}

}  // namespace

bool SignTool::testAllowBottom = false;
bool SignTool::testNoAnchorHook = false;
bool SignTool::testNoCache = false;

int SignTool::facingForYaw(float yaw) {
    // The camera looks along (cos yaw, sin yaw) in (x, z); the text faces back along it.
    const float lx = -cosf(yaw * (float)M_PI / 180.0f), lz = -sinf(yaw * (float)M_PI / 180.0f);
    if (fabsf(lx) >= fabsf(lz)) return lx < 0 ? 0 : 2;   // -x : +x
    return lz < 0 ? 1 : 3;                                // -z (file -y) : +z
}

void SignTool::tap(int mx, int my, float yaw) {
    const Point3D anchor = findWorldCoords(my, mx, FC_DESTROY);
    if (anchor.x == -1) return;
    const Point3D place = findWorldCoords(my, mx, FC_PLACE);
    if (place.x == -1) return;
    use(anchor.x, anchor.z, anchor.y, place.x - anchor.x, place.y - anchor.y, place.z - anchor.z, yaw);
}

int SignTool::use(int x, int z, int y, int dx, int dyUp, int dz, float yaw) {
    World* w = World::getWorld;
    if (GLDialog::active()) return USE_NONE;   // modal: a tap under the prompt never re-opens it
    if (std::abs(dx) + std::abs(dyUp) + std::abs(dz) != 1) return USE_NONE;
    if (dyUp == -1 && !testAllowBottom) {
        w->hud->sb->setStatus("Can't place a sign under the block", 2);
        return USE_REFUSED_BOTTOM;
    }
    if (w->terrain->getLand(x, z, y) <= 0) return USE_REFUSED_AIR;
    WorldTrailer* t = w->fm->trailer();
    if (!t->parsed()) {
        w->fm->reportTrailerRefusal(WorldTrailer::TR_OPAQUE);
        return USE_REFUSED_OPAQUE;
    }
    // a: 0 -x, 1 +x, 3 top, 4 -y (engine -z), 5 +y (engine +z). c for a wall sign is its face's
    // direction (0 -x, 1 -z, 2 +x, 3 +z); for a standing sign, the player's quarter.
    int face, facing;
    if (dyUp == 1)      { face = 3; facing = facingForYaw(yaw); }
    else if (dyUp == -1) { face = 2; facing = facingForYaw(yaw); }   // testAllowBottom only
    else if (dx == -1)  { face = 0; facing = 0; }
    else if (dx == 1)   { face = 1; facing = 2; }
    else if (dz == -1)  { face = 4; facing = 1; }
    else                { face = 5; facing = 3; }
    const TrailerPos pos = TrailerPos::fromTerrainArgs(x, z, y);
    const int existing = t->findSign(pos, face);
    g_pending.on = true;
    g_pending.pos = pos;
    g_pending.face = face;
    g_pending.facing = facing;
    char initial[96] = {0};
    if (existing >= 0) {
        memcpy(initial, t->sign((size_t)existing).text, 95);
        initial[95] = 0;
    }
    static const char* const kButtons[] = {"OK", "Cancel"};
    GLDialog::prompt(existing >= 0 ? "Edit sign" : "New sign",
                     existing >= 0 ? "Leave it empty to remove the sign." : NULL,
                     initial, 95, kButtons, 2, answer, 3);
    return existing >= 0 ? USE_PROMPT_EDIT : USE_PROMPT_NEW;
}

void SignTool::answer(int chosen, const char* text) {
    if (!g_pending.on) return;
    g_pending.on = false;
    if (chosen != 0 || !World::getWorld) return;
    World* w = World::getWorld;
    // Printable ASCII only (the record's rule); anything else the field let through is dropped.
    std::string clean;
    for (const char* p = text ? text : ""; *p && clean.size() < 95; ++p)
        if (*p >= 32 && *p <= 126) clean += *p;
    bool blank = true;
    for (char ch : clean) if (ch != ' ') { blank = false; break; }

    const TrailerPos& pos = g_pending.pos;
    // The anchor may have gone while the prompt was up.
    if (!blank && w->fm->trailer()->findSign(pos, g_pending.face) < 0 && w->terrain->getLand(pos.x, pos.y, pos.z) <= 0) return;
    const int r = commitSign(pos, g_pending.face, g_pending.facing, clean, blank, buildPaint(), true);
    if (r != WorldTrailer::TR_OK) { w->fm->reportTrailerRefusal(r); return; }
    if (!blank) {   // K.6-3: the `sign` event (a script's own eden.setsign is not one)
        int ex, ey, ez;
        pos.toEngine(&ex, &ey, &ez);
        ModScript::noteSign(ex, ez, ey, g_pending.face, clean.c_str());
    }
}

bool SignTool::set(int x, int z, int y, int face, const char* text, int colour, float yaw, std::string* why) {
    std::string e;
    std::string* W = why ? why : &e;
    World* w = World::getWorld;
    if (!w || !w->fm || !w->terrain) { *W = "no world is open"; return false; }
    if (!(face == 0 || face == 1 || face == 3 || face == 4 || face == 5)) {
        *W = "face must be 0 (-x), 1 (+x), 3 (top), 4 (-z) or 5 (+z)";
        return false;
    }
    if (colour != 0 && (colour < 1 || colour > 54)) { *W = "colour must be 1 to 54"; return false; }
    const size_t n = text ? strlen(text) : 0;
    if (n > 95) { *W = "a sign holds up to 95 bytes"; return false; }
    bool blank = true;
    for (size_t i = 0; i < n; ++i) {
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126) { *W = "a sign holds printable ASCII only (32 to 126)"; return false; }
        if (text[i] != ' ') blank = false;
    }
    WorldTrailer* t = w->fm->trailer();
    if (!t->parsed()) { *W = WorldTrailer::resultMessage(WorldTrailer::TR_OPAQUE); return false; }
    const TrailerPos pos = TrailerPos::fromTerrainArgs(x, z, y);
    if (!blank && t->findSign(pos, face) < 0 && w->terrain->getLand(x, z, y) <= 0) {
        *W = "no block there to hang a sign on";
        return false;
    }
    // A new standing sign faces the player, like the prompt's; a wall sign its face's direction.
    static const int kFacing[6] = {0, 2, 0, 0, 1, 3};
    const int facing = face == 3 ? facingForYaw(yaw) : kFacing[face];
    const int r = commitSign(pos, face, facing, blank ? std::string() : std::string(text, n), blank, colour, false);
    if (r != WorldTrailer::TR_OK) { *W = WorldTrailer::resultMessage(r); return false; }
    return true;
}

bool SignTool::get(int x, int z, int y, int face, std::string* text, int* colour, int* faceOut) {
    World* w = World::getWorld;
    if (!w || !w->fm) return false;
    WorldTrailer* t = w->fm->trailer();
    const TrailerPos pos = TrailerPos::fromTerrainArgs(x, z, y);
    int i = -1;
    if (face >= 0) i = t->findSign(pos, face);
    else { const std::vector<int> v = t->signsOn(pos); if (!v.empty()) i = v[0]; }
    if (i < 0) return false;
    const TrailerSign& sg = t->sign((size_t)i);
    char b[96];
    memcpy(b, sg.text, 95);
    b[95] = 0;
    *text = b;
    *colour = sg.b;
    *faceOut = sg.a;
    return true;
}

void SignTool::anchorBecameAir(int x, int z, int y) {
    if (testNoAnchorHook || !World::getWorld || !World::getWorld->fm) return;
    WorldTrailer* t = World::getWorld->fm->trailer();
    const TrailerPos pos = TrailerPos::fromTerrainArgs(x, z, y);
    if (t->hasAnchored(pos)) {
        if (t->findCmd(pos) >= 0) CmdTool::setRunning(pos, false);
        t->removeAnchored(pos);
    }
}

// ---- CmdTool (D.4b) ---------------------------------------------------------------------------

namespace {

// The edit prompt is up for this command block; looked up again on the answer, like a sign's.
bool g_cmdPending = false;
TrailerPos g_cmdPos = {0, 0, 0};

std::unordered_set<uint64_t> g_running;
uint64_t runKey(const TrailerPos& p) {
    return ((uint64_t)(uint32_t)p.x << 40) ^ ((uint64_t)(uint32_t)p.y << 16) ^ (uint64_t)(uint32_t)p.z;
}

}  // namespace

bool CmdTool::running(const TrailerPos& block) { return !g_running.empty() && g_running.count(runKey(block)); }
void CmdTool::setRunning(const TrailerPos& block, bool on) {
    if (on) g_running.insert(runKey(block));
    else g_running.erase(runKey(block));
}
void CmdTool::clearRunning() { g_running.clear(); }

void CmdTool::tap(int mx, int my) {
    const Point3D anchor = findWorldCoords(my, mx, FC_DESTROY);
    if (anchor.x == -1) return;
    const Point3D place = findWorldCoords(my, mx, FC_PLACE);
    if (place.x == -1) return;
    use(anchor.x, anchor.z, anchor.y, place.x - anchor.x, place.y - anchor.y, place.z - anchor.z);
}

int CmdTool::use(int x, int z, int y, int dx, int dyUp, int dz) {
    World* w = World::getWorld;
    if (GLDialog::active()) return USE_NONE;
    if (std::abs(dx) + std::abs(dyUp) + std::abs(dz) != 1) return USE_NONE;
    WorldTrailer* t = w->fm->trailer();
    if (!t->parsed()) {
        w->fm->reportTrailerRefusal(WorldTrailer::TR_OPAQUE);
        return USE_REFUSED_OPAQUE;
    }
    // Tapping a command block: edit its script.
    const TrailerPos at = TrailerPos::fromTerrainArgs(x, z, y);
    const int existing = w->terrain->getLand(x, z, y) > 0 ? t->findCmd(at) : -1;
    if (existing >= 0) {
        g_cmdPending = true;
        g_cmdPos = at;
        char initial[CmdTool::MAX_SCRIPT + 1];
        memcpy(initial, t->cmd((size_t)existing).script, CmdTool::MAX_SCRIPT);
        initial[CmdTool::MAX_SCRIPT] = 0;
        static const char* const kButtons[] = {"OK", "Cancel"};
        GLDialog::prompt("Command block", "Statements separated by ;  Empty clears the script.",
                         initial, CmdTool::MAX_SCRIPT, kButtons, 2, answer, PROMPT_LINES);
        return USE_PROMPT_EDIT;
    }
    // Anything else: a new command block in the cell in front of the face -- the build rules
    // (air, or a liquid below level 4, and not inside the player).
    const int px = x + dx, pz = z + dz, py = y + dyUp;
    if (py < 0 || py >= T_HEIGHT) return USE_REFUSED_CELL;
    const int cur = w->terrain->getLand(px, pz, py);
    if (!(cur == TYPE_NONE || (cur > 0 && (blockinfo[cur] & IS_LIQUID) && getLevel(cur) < 4)))
        return USE_REFUSED_CELL;
    // Player::test and buildBlock both read hud->blocktype (test indexes blockinfo[] with it, which
    // a tool sentinel must never reach): both run as a plain unpainted steel build, then put back.
    Hud* hud = w->hud;
    const int savedType = hud->blocktype, savedPaint = hud->block_paintcolor;
    hud->blocktype = TYPE_STEEL;
    const bool inPlayer = w->player->test(px, py, pz, 1);
    hud->blocktype = savedType;
    if (inPlayer) return USE_REFUSED_PLAYER;
    if ((int)t->cmdCount() >= MAX_PER_WORLD) {
        w->hud->sb->setStatus("This world has 512 command blocks already", 3);
        return USE_REFUSED_FULL;
    }
    const TrailerPos pos = TrailerPos::fromTerrainArgs(px, pz, py);
    const int r = t->addCmd(pos, "");
    if (r != WorldTrailer::TR_OK) { w->fm->reportTrailerRefusal(r); return USE_REFUSED_FULL; }
    // The voxel through the stock build path (liquid source removal, the dirty lists), as plain
    // unpainted steel: the skin is drawn over it, never baked into it.
    hud->blocktype = TYPE_STEEL;
    hud->block_paintcolor = 0;
    w->terrain->buildBlock(px, pz, py);
    hud->blocktype = savedType;
    hud->block_paintcolor = savedPaint;
    Resources::getResources->playSound(S_BUILD_METAL);
    return USE_PLACED;
}

void CmdTool::answer(int chosen, const char* text) {
    if (!g_cmdPending) return;
    g_cmdPending = false;
    if (chosen != 0 || !World::getWorld) return;
    World* w = World::getWorld;
    std::string clean;
    for (const char* p = text ? text : ""; *p && clean.size() < (size_t)MAX_SCRIPT; ++p)
        if (*p >= 32 && *p <= 126) clean += *p;
    if (clean.find_first_not_of(' ') == std::string::npos) clean.clear();   // blank = empty
    WorldTrailer* t = w->fm->trailer();
    const int i = t->findCmd(g_cmdPos);
    if (i < 0) return;   // mined while the prompt was up
    const int r = t->setCmdScript((size_t)i, clean.c_str());
    if (r != WorldTrailer::TR_OK) w->fm->reportTrailerRefusal(r);
}
