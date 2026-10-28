// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "props".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_props {


// ---------------------------------------------------------------------------------------------
// --camera-selftest  (Stage H / H.12, the third-person camera)
// ---------------------------------------------------------------------------------------------
// Headless: the GL shim keeps the ES1 matrix stack in software, so findWorldCoords' unproject and
// the frustum both work with no window. The camera fields are set directly before each probe (the
// next tick's Player::update would overwrite them), on a pivot in open air looking +x, with a target
// block T 13 ahead (inside first-person reach, OUTSIDE reach if the third-person ray started at the
// camera 4 back) and, for the middle legs, a block B 2 behind (between the camera and the player).
extern "C" void eden_cycle_camera_view(void);
extern "C" void eden_set_camera_view(int view);
extern "C" int  eden_get_camera_view(void);
extern "C" float eden_settings_get(int i);
extern "C" void eden_settings_set(int i, float v);
extern int fwc_result;
int run_camera_selftest() {
    g_selftestTag = "eden-camera";
    const char* name = g_opt.world.empty() ? "camera-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();
    eden_set_fly_mode(1);   // hold still between the ticks below
    tick(120);
    char d[256];
    Terrain* ter = World::getWorld->terrain;
    Camera* cam = World::getWorld->cam;
    const Vector pp = World::getWorld->player->pos;

    // A pivot with air from 8 behind to 16 ahead (and a block either side), well above the ground.
    int ax = -1, az = -1, ay = -1;
    for (int k = 0; k < 64 && ay < 0; ++k) {
        const int x = (int)pp.x + (k % 8) * 5, z = (int)pp.z + (k / 8) * 5;
        int g = -1;
        for (int y = T_HEIGHT - 1; y >= 0 && g < 0; --y) if (ter->getLand(x, z, y) > 0) g = y;
        const int y = (g < 0 ? 30 : g) + 6;
        if (y + 3 >= T_HEIGHT) continue;
        bool clear = true;
        for (int dx = -8; dx <= 16 && clear; ++dx)
            for (int dz = -1; dz <= 1 && clear; ++dz)
                for (int dy = -1; dy <= 1 && clear; ++dy)
                    if (ter->getLand(x + dx, z + dz, y + dy) != 0) clear = false;
        if (clear) { ax = x; az = z; ay = y; }
    }
    if (ay < 0) { check(false, "an open-air pivot near the player", ""); return 1; }
    const float px = ax + 0.5f, py = ay + 0.5f, pz = az + 0.5f;
    auto aim = [&](int mode, float dist) {
        cam->px = px; cam->py = py; cam->pz = pz;
        cam->yaw = 0; cam->pitch = 0; cam->mode = mode; cam->dist = dist;
    };
    auto pick = [&](int mode) -> int {   // the block x hit at screen centre, or -1
        aim(mode, 4.0f);
        const Point3D h = findWorldCoords((int)(SCREEN_HEIGHT / 2), (int)(SCREEN_WIDTH / 2), FC_DESTROY);
        return h.x < 0 ? -1 : (int)h.x;
    };

    ter->updateChunks(ax + 13, az, ay, TYPE_STONE);   // T
    tick(4);
    const int h0 = pick(0), h1 = pick(1), h2 = pick(2);
    std::snprintf(d, sizeof(d), "pivot %d,%d,%d; T at x %d; hit x view0 %d view1 %d view2 %d", ax, az, ay, ax + 13, h0, h1, h2);
    check(h0 == ax + 13, "first person reaches T 13 blocks ahead", d);
    check(h1 == ax + 13, "third person behind aims and reaches the same block", d);
    check(h2 == ax + 13, "third person front picks along the player's look", d);

    ter->updateChunks(ax - 2, az, ay, TYPE_STONE);    // B, between the camera and the player
    tick(4);
    const int hb = pick(1);
    std::snprintf(d, sizeof(d), "hit x %d (B at %d)", hb, ax - 2);
    check(hb == ax + 13, "a block behind the player neither picks nor blocks", d);

    aim(1, 4.0f);
    cam->updateThirdPerson(1.0f / 60);
    const float pulled = cam->dist;
    // B's near face is 1.5 behind the pivot; the 0.25 march first lands in it at 1.75, less 0.3.
    std::snprintf(d, sizeof(d), "dist %.2f (B 1.5 behind the pivot: 1.45 expected)", pulled);
    check(std::fabs(pulled - 1.45f) < 0.01f, "the camera pulls in in front of a wall", d);

    // Culling: the frustum must be the one the frame is drawn with.
    auto inside = [&](float x) {
        const float b[6] = { x - 0.1f, py - 0.1f, pz - 0.1f, x + 0.1f, py + 0.1f, pz + 0.1f };
        return !(ViewTestAABB(b, 0) & VT_OUTSIDE);
    };
    R::mv().push();
    R::mv().loadIdentity();
    aim(1, 4.0f);
    cam->render();
    const bool behindIn = inside(px - 1.5f), aheadIn = inside(px + 3.0f), pastEyeIn = inside(px - 6.0f);
    R::mv().loadIdentity();
    aim(0, 4.0f);
    cam->render();
    const bool behindIn0 = inside(px - 1.5f);
    R::mv().pop();
    std::snprintf(d, sizeof(d), "view1: 1.5 behind %d, 3 ahead %d, 2 behind the eye %d; view0: 1.5 behind %d",
                  behindIn, aheadIn, pastEyeIn, behindIn0);
    check(behindIn && aheadIn && !pastEyeIn, "third-person culling uses the third-person frustum", d);
    check(!behindIn0, "first-person culling is unchanged", d);

    ter->updateChunks(ax - 2, az, ay, TYPE_NONE);
    tick(4);
    aim(1, pulled);
    for (int k = 0; k < 60; ++k) cam->updateThirdPerson(1.0f / 60);
    std::snprintf(d, sizeof(d), "dist %.2f after 1 s", cam->dist);
    check(std::fabs(cam->dist - 4.0f) < 0.01f, "the camera eases back out in the open", d);
    ter->updateChunks(ax + 13, az, ay, TYPE_NONE);

    // The mode through the real setting and key path, and the body slot.
    int si = -1;
    for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), "camera_view")) si = i;
    if (si < 0) { check(false, "the camera_view setting exists", ""); return 1; }
    eden_settings_set(si, 0);
    int seen[4];
    for (int k = 0; k < 3; ++k) { eden_cycle_camera_view(); seen[k] = eden_get_camera_view() * 10 + (int)eden_settings_get(si); }
    std::snprintf(d, sizeof(d), "view*10+setting: %d %d %d", seen[0], seen[1], seen[2]);
    check(seen[0] == 11 && seen[1] == 22 && seen[2] == 0, "F5 cycles 0->1->2->0 and the setting tracks it", d);

    SpawnCreatureAt(M_MOOF, MakeVector(pp.x + 6, pp.y, pp.z + 6));   // so the count below is not 0 == 0
    eden_settings_set(si, 1);
    tick(10);
    Vector bp;
    const bool avail = PlayerBodyAvailable(), shown = PlayerBodyShown(&bp);
    std::snprintf(d, sizeof(d), "cam mode %d; PODs %s; slot %s", cam->mode, avail ? "loaded" : "not loaded (fallback box)",
                  shown ? "posed" : "empty");
    check(cam->mode == 1 && shown == avail, "view 1 drives the camera and the body slot", d);
    if (shown) {
        const Vector feet = World::getWorld->player->pos;
        const int hit = PointTestModels(feet.x, feet.y + 0.5f, feet.z);
        const int n1 = CountActiveCreatures();
        HidePlayerBody();
        const int n0 = CountActiveCreatures();
        std::snprintf(d, sizeof(d), "PointTestModels at the player %d; creatures %d posed, %d hidden", hit, n1, n0);
        check(hit == -1 && n1 == n0, "the body is not a creature (no pick, no count)", d);
    }
    eden_settings_set(si, 0);
    tick(4);
    std::snprintf(d, sizeof(d), "cam mode %d, slot %s", cam->mode, PlayerBodyShown(nullptr) ? "posed" : "empty");
    check(cam->mode == 0 && !PlayerBodyShown(nullptr), "view 0 is first person with no body", d);

    std::printf("[eden-camera] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --props-selftest  (Stage H / H.15, World Properties)
// ---------------------------------------------------------------------------------------------
// world-properties-plan.md §6, gates 1-5, on worlds written into --docs and driven through the
// real screen (taps on Properties, the Terrain tab, Save, the confirm's buttons):
//   1. Round trip: a Classic world with one edited column; Caves Off -> Deep through the screen
//      (Cancel on the confirm first: nothing written); reopened, untouched columns are the new
//      settings' pure function and differ from the old, the edited column is byte-equal; setting
//      Caves back restores the untouched land. --props-mutate=nogen (skip the GENERATOR append)
//      must FAIL it.
//   2. Legacy: the H.4 1.x fixture, no record: detected caves shown; a name-only save raises no
//      dialog and writes exactly one GENERATOR (profile 1, detected flag kept).
//   3. Lift: home in an unsaved column lands on the new surface (solid under, two air above);
//      pos in a saved column keeps its y.
//   4. Cost: every open's time is printed; --props-world=FILE (a world in --docs) times one more.
//   5. UI: Scale read-only on a 256z 1.x world and a slider on a new one; Flat shows its type only.

namespace props_test {

// g_world::classic_test::write_world at either height: header, empty creature slots, an optional record.
bool write_bare(const std::string& path, const char* name, int seed, int bands, int hx, int hz,
                const uint8_t* genRecord, std::string& err) {
    emod::io::remove_file(path);
    emod::EdenWorldStore st;
    if (!st.create(path.c_str(), bands, 0)) { err = st.error(); return false; }
    st.beginBatch();
    WorldFileHeader h;
    std::memset(&h, 0, sizeof h);
    h.level_seed = seed;
    h.pos.x = h.home.x = (float)hx; h.pos.z = h.home.z = (float)hz; h.pos.y = h.home.y = 60;
    std::snprintf(h.name, sizeof h.name, "%s", name);
    h.version = bands == 16 ? 5 : 4;
    h.goldencubes = 10;
    for (int i = 0; i < 16; ++i) h.skycolors[i] = COLOR_NORMAL_BLUE;
    const int slots = bands == 16 ? 400 : 200;
    std::vector<uint8_t> cr(slots * sizeof(EntityData), 0);
    for (size_t o = 0; o < cr.size(); o += sizeof(EntityData)) { int32_t t = -1; std::memcpy(&cr[o + 28], &t, 4); }
    st.putRecord(emod::T_WORLD_HEADER, &h, sizeof h);
    st.putRecord(emod::T_CREATURES, &cr[0], cr.size());
    if (genRecord && !st.putRecord(emod::T_GENERATOR, genRecord, CLASSIC_RECORD_BYTES)) { err = st.error(); return false; }
    if (!st.commitBatch()) { err = st.error(); return false; }
    st.close();
    return true;
}

Menu* menu() { return World::getWorld->menu; }


// Selects `file` in the list and taps Properties.
bool open_props(const char* file) {
    menu()->loadWorlds();
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_file(i), file) == 0) idx = i;
    if (idx < 0) { std::fprintf(stderr, "[eden-props] %s is not listed\n", file); return false; }
    eden_menu_select(idx);
    ui_menu_to(false);
    tick(2);
    ui_tap_rect(menu()->controlRect("properties"));
    tick(3);
    WorldPropertiesMenu* wp = menu()->props;
    if (menu()->showprops && wp->isOpen())
        std::printf("[eden-props] open %s: %.1f ms (%d columns)\n", file, wp->props().openMs, wp->props().columns);
    return menu()->showprops && wp->isOpen();
}
void close_props() {
    if (menu()->showprops) { menu()->props->back(); menu()->showprops = false; }
    tick(2);
}
void tap_choice(const char* key, int option) {
    CGRect r;
    if (menu()->props->choiceRect(key, option, &r)) ui_tap_rect(r);
    tick(2);
}
void tap_dialog(int j) {
    CGRect r;
    if (GLDialog::buttonRect(j, &r)) ui_tap_rect(r);
    tick(3);
}
long long file_size(const std::string& p) {
    FILE* f = emod::io::fopen_utf8(p.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long long n = std::ftell(f);
    std::fclose(f);
    return n;
}
bool read_header(const std::string& p, WorldFileHeader* h) {
    emod::EdenWorldStore st;
    std::vector<uint8_t> raw;
    if (!st.open(p.c_str(), false) || st.readRecord(emod::T_WORLD_HEADER, raw) != emod::EdenWorldStore::READ_OK ||
        raw.size() != sizeof *h) return false;
    std::memcpy(h, &raw[0], sizeof *h);
    return true;
}
bool read_gen(const std::string& p, ClassicGen* g) {
    emod::EdenWorldStore st;
    std::vector<uint8_t> rec;
    return st.open(p.c_str(), false) && st.readRecord(emod::T_GENERATOR, rec) == emod::EdenWorldStore::READ_OK &&
           classic_decode_record(&rec[0], rec.size(), g);
}
// Resident cells of column (cx,cz) against an `.emod` column's type bytes (band-major, y fastest).
int column_vs_bytes(int cx, int cz, const std::vector<uint8_t>& col, int height) {
    if (!isColumnResident(cx * 16, cz * 16)) return -1;
    int bad = 0;
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
            for (int y = 0; y < height; ++y)
                bad += getLandc(cx * 16 + x, cz * 16 + z, y) != (block8)col[(size_t)(y / 16) * emod::BAND + (x * 16 + z) * 16 + y % 16];
    return bad;
}

}  // namespace props_test

int run_props_selftest() {
    using namespace props_test;
    using g_world::classic_test::column_vs_pure;
    using g_world::classic_test::count_records;
    using g_world::classic_test::play_file;
    g_selftestTag = "eden-props";
    g_tickInput = true;            // the name field is typed into (headless pump_events drains SDL's queue only with this)
    char d[512];
    std::string err;
    const std::string docs = eden_platform_documents_root();
    FileManager* fm = World::getWorld->fm;
    const bool mut = g_opt.propsMutate == "nogen";
    g_wp_test_skip_generator = mut;
    const int S = 4096 * 16 + 8;

    // ---- 1. round trip through the screen
    ClassicGen g0;
    classic_default_params(&g0, 4242);
    uint8_t rec[CLASSIC_RECORD_BYTES];
    classic_encode_record(g0, rec);
    const std::string rp = docs + "/props-round.emod";
    bool ok = write_bare(rp, "props round", 4242, 4, S, S, rec, err) && play_file("props-round.emod");
    check(ok, "a Classic world plays", err.c_str());
    if (!ok) return 1;
    tick(30);
    const int ecx = 4098, ecz = 4096;                      // the edited column
    std::vector<std::pair<int, int> > untouched;           // resident, never edited
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -2; dz <= 0; ++dz) untouched.push_back(std::make_pair(4096 + dx, 4096 + dz));
    int bad = 0, tested = 0;
    for (size_t i = 0; i < untouched.size(); ++i) {
        const int b = column_vs_pure(g0, untouched[i].first, untouched[i].second, 64);
        if (b >= 0) { ++tested; bad += b; }
    }
    std::snprintf(d, sizeof d, "%d columns, %d cells differ", tested, bad);
    check(tested == (int)untouched.size() && bad == 0, "its columns are the record's pure function", d);
    Terrain* ter = World::getWorld->terrain;
    for (int y = 1; y < 63; ++y) ter->updateChunks(ecx * 16 + 5, ecz * 16 + 5, y, TYPE_WOOD);
    tick(5);
    save_world();
    quit_to_menu();
    tick(10);
    std::vector<uint8_t> edited(4 * emod::BAND);
    {
        emod::EdenWorldStore st;
        ok = st.open(rp.c_str(), false) && st.hasColumn(ecx, ecz) &&
             st.readColumn(ecx, ecz, &edited[0]) == emod::EdenWorldStore::READ_OK;
        check(ok, "the edited column is saved", "");
    }

    ok = open_props("props-round.emod");
    WorldPropertiesMenu* wp = menu()->props;
    std::snprintf(d, sizeof d, "type %d source %d seed \"%s\" photo %d", wp->props().type, wp->props().genSource,
                  wp->value("seed"), wp->rowVisible("photo") ? 1 : 0);
    check(ok && wp->props().type == WP_TYPE_CLASSIC && wp->props().genSource == WP_GEN_RECORD &&
          std::strcmp(wp->value("seed"), "4242") == 0 && !wp->rowEditable("seed"),
          "Properties opens on a Classic world, the seed read-only", d);
    ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_TERRAIN));
    tick(2);
    const bool rows = wp->rowEditable("hills") && wp->rowEditable("caves") && wp->rowEditable("trees") &&
                      wp->rowEditable("flowers") && wp->rowEditable("clouds") && wp->rowEditable("size") &&
                      !wp->rowVisible("scale") && !wp->rowVisible("seed");
    check(wp->tab() == WorldPropertiesMenu::TAB_TERRAIN && rows, "the Terrain tab has the Classic rows (no Scale at 64)", "");
    tap_choice("caves", 2);
    std::snprintf(d, sizeof d, "caves %d changed %d save %d", wp->choice("caves"), wp->terrainChanged() ? 1 : 0,
                  wp->saveEnabled() ? 1 : 0);
    check(wp->choice("caves") == CLASSIC_CAVES_DEEP && wp->terrainChanged() && wp->saveEnabled(), "Caves -> Deep enables Save", d);
    const long long size0 = file_size(rp);
    ui_tap_rect(wp->controlRect("save"));
    tick(3);
    check(GLDialog::active() && wp->isOpen(), "a terrain save raises the confirm", "");
    tap_dialog(1);   // Cancel
    std::snprintf(d, sizeof d, "dialog %d open %d bytes %lld -> %lld", GLDialog::active() ? 1 : 0, wp->isOpen() ? 1 : 0,
                  size0, file_size(rp));
    check(!GLDialog::active() && wp->isOpen() && file_size(rp) == size0, "Cancel writes nothing and stays on the screen", d);
    ui_tap_rect(wp->controlRect("save"));
    tick(3);
    tap_dialog(0);   // Change terrain
    const int n9 = count_records(rp, emod::T_GENERATOR);
    std::snprintf(d, sizeof d, "open %d result %d, %d GENERATOR record(s)", wp->isOpen() ? 1 : 0, wp->lastResult(), n9);
    check(!wp->isOpen() && !menu()->showprops && wp->lastResult() >= 0 && (wp->lastResult() & WP_WROTE_GENERATOR) && n9 == 2,
          "Change terrain appends a newer GENERATOR and closes", d);

    ClassicGen g1 = g0;
    g1.caves = CLASSIC_CAVES_DEEP;
    ok = play_file("props-round.emod");
    check(ok, "it reopens", "");
    if (ok) {
        tick(30);
        TerrainGenerator* tg = World::getWorld->terrain->tgen;
        int newBad = 0, oldDiff = 0;
        tested = 0;
        for (size_t i = 0; i < untouched.size(); ++i) {
            const int b = column_vs_pure(g1, untouched[i].first, untouched[i].second, 64);
            if (b < 0) continue;
            ++tested; newBad += b;
            oldDiff += column_vs_pure(g0, untouched[i].first, untouched[i].second, 64);
        }
        std::snprintf(d, sizeof d, "caves %d; %d columns: %d cells off the new settings, %d differ from the old",
                      tg->classic.caves, tested, newBad, oldDiff);
        check(tg->classic.caves == CLASSIC_CAVES_DEEP && tested == (int)untouched.size() && newBad == 0 && oldDiff > 0,
              "untouched land is regenerated with the new settings", d);
        const int eb = column_vs_bytes(ecx, ecz, edited, 64);
        std::snprintf(d, sizeof d, "%d cells differ from the saved column", eb);
        check(eb == 0, "the edited column is byte-equal", d);
        quit_to_menu();
        tick(10);
    }
    if (mut) {
        std::printf("[eden-props] %s (%d failure(s)) [mutation nogen]\n", g_selftestFailures ? "FAILURES" : "ALL PASS",
                    g_selftestFailures);
        return g_selftestFailures ? 1 : 0;
    }
    {
        WorldProps p;
        ok = fm->readWorldProperties("props-round.emod", &p);
        p.gen.caves = CLASSIC_CAVES_OFF;
        const int r = ok ? fm->applyWorldProperties("props-round.emod", p) : -1;
        ok = r >= 0 && (r & WP_WROTE_GENERATOR) && play_file("props-round.emod");
        int back = 0;
        tested = 0;
        if (ok) {
            tick(30);
            for (size_t i = 0; i < untouched.size(); ++i) {
                const int b = column_vs_pure(g0, untouched[i].first, untouched[i].second, 64);
                if (b >= 0) { ++tested; back += b; }
            }
            quit_to_menu();
            tick(10);
        }
        std::snprintf(d, sizeof d, "result %d, %d columns, %d cells differ from the original", r, tested, back);
        check(ok && tested == (int)untouched.size() && back == 0, "setting Caves back brings the original land back", d);
        emod::EdenWorldStore st;
        check(st.open(rp.c_str(), false) && !st.damaged() && count_records(rp, emod::T_GENERATOR) == 3,
              "the file reads undamaged, three GENERATOR versions", st.error().c_str());
    }

    // ---- 3. lift: home in an unsaved column, pos in the saved (edited) one
    {
        emod::EdenWorldStore st;
        std::vector<uint8_t> raw;
        WorldFileHeader h;
        ok = st.open(rp.c_str(), true) && st.readRecord(emod::T_WORLD_HEADER, raw) == emod::EdenWorldStore::READ_OK;
        if (ok) {
            std::memcpy(&h, &raw[0], sizeof h);
            h.home.x = 4100 * 16 + 3; h.home.z = 4100 * 16 + 3; h.home.y = 50;
            h.pos.x = ecx * 16 + 5.5f; h.pos.z = ecz * 16 + 5.5f; h.pos.y = 63;
            st.beginBatch();
            st.putRecord(emod::T_WORLD_HEADER, &h, sizeof h);
            ok = st.commitBatch();
        }
        st.close();
        WorldProps p;
        ok = ok && fm->readWorldProperties("props-round.emod", &p);
        p.gen.hilliness = 4;   // Wild: a different surface
        const int r = ok ? fm->applyWorldProperties("props-round.emod", p) : -1;
        WorldFileHeader h2;
        ok = r >= 0 && read_header(rp, &h2);
        ClassicGen g2 = g0;
        g2.hilliness = 4;
        int under = -1, at = -1, above = -1;
        if (ok) {
            static block8 t[256 * 64];
            static color8 c[256 * 64];
            const int hx = (int)h2.home.x, hz = (int)h2.home.z, hy = (int)h2.home.y;
            classic_generate_window(g2, hx & ~15, hz & ~15, 16, 16, 64, t, c, true);
            const block8* col = &t[((hx & 15) * 16 + (hz & 15)) * 64];
            if (hy >= 1 && hy < 63) { under = col[hy - 1]; at = col[hy]; above = col[hy + 1]; }
        }
        std::snprintf(d, sizeof d, "result %d home.y %.0f (cells %d/%d/%d) pos.y %.1f", r, ok ? h2.home.y : -1.0f,
                      under, at, above, ok ? h2.pos.y : -1.0f);
        check(ok && (r & WP_LIFTED_HOME) && under > 0 && at == 0 && above == 0,
              "home in unsaved land is lifted to the new surface", d);
        check(ok && !(r & WP_LIFTED_POS) && h2.pos.y == 63, "a position on saved land keeps its y", d);
    }

    // ---- 2. legacy (the H.4 fixture: a 1.x world with no record)
    g_world::classic_test::Fixture fx;
    const bool haveFx = g_world::classic_test::load_fixture(g_opt.classicFixture, fx, err);
    check(haveFx, "the 1.x fixture loads", haveFx ? g_opt.classicFixture.c_str() : err.c_str());
    if (haveFx) {
        const std::string lp = docs + "/props-legacy.emod";
        const int hx = fx.keys[0].first * 16 + 8, hz = fx.keys[0].second * 16 + 8;
        ok = g_world::classic_test::write_world(lp, "props legacy", fx.seed, hx, hz, &fx, NULL, NULL, err) && open_props("props-legacy.emod");
        check(ok, "a 1.x world opens in Properties", err.c_str());
        if (ok) {
            const WorldProps& p = wp->props();
            ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_TERRAIN));
            tick(2);
            std::snprintf(d, sizeof d, "source %d profile %d flags %d caves %d (shown %d)", p.genSource, p.gen.profile,
                          p.gen.flags, p.gen.caves, wp->choice("caves"));
            check(p.genSource == WP_GEN_LEGACY && p.gen.profile == CLASSIC_PROFILE_LEGACY && (p.gen.flags & 1) &&
                  wp->choice("caves") == p.gen.caves, "it shows the detected caves", d);
            ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_WORLD));
            tick(2);
            ui_tap_rect(wp->controlRect("name"));
            tick(2);
            for (int i = 0; i < 64; ++i) push_tap_key(SDL_SCANCODE_BACKSPACE);
            tick(2);                         // the keys drain before the text (two queues)
            push_text("Legacy renamed");
            tick(2);
            ui_tap_rect(wp->controlRect("save"));
            tick(3);
            const bool noDialog = !GLDialog::active();
            if (!noDialog) tap_dialog(1);
            ClassicGen lg;
            const bool rd = read_gen(lp, &lg);
            WorldFileHeader lh;
            read_header(lp, &lh);
            std::snprintf(d, sizeof d, "dialog %d result %d, %d record(s), profile %d flags %d caves %d, name \"%.49s\"",
                          noDialog ? 0 : 1, wp->lastResult(), count_records(lp, emod::T_GENERATOR), rd ? lg.profile : -1,
                          rd ? lg.flags : -1, rd ? lg.caves : -1, lh.name);
            check(noDialog && !wp->isOpen() && count_records(lp, emod::T_GENERATOR) == 1 && rd &&
                  lg.profile == CLASSIC_PROFILE_LEGACY && (lg.flags & 1) && lg.caves == p.gen.caves &&
                  std::strcmp(lh.name, "Legacy renamed") == 0,
                  "a name-only save raises no dialog and records the legacy generator once", d);
            close_props();
        }
    }

    // ---- 5. the Terrain tab's other shapes
    {
        const std::string a = docs + "/props-legacy256.emod", b = docs + "/props-new256.emod", f = docs + "/props-flat.emod";
        ClassicGen gn;
        classic_default_params(&gn, 777);
        gn.scale4 = 8;
        uint8_t rn[CLASSIC_RECORD_BYTES];
        classic_encode_record(gn, rn);
        ok = write_bare(a, "props legacy 256", 777777, 16, S, S, NULL, err) && write_bare(b, "props new 256", 777, 16, S, S, rn, err) &&
             write_bare(f, "props flat", 0, 4, S, S, NULL, err);
        check(ok, "the 256z and flat worlds are written", err.c_str());
        if (ok && open_props("props-legacy256.emod")) {
            wp->showTab(WorldPropertiesMenu::TAB_TERRAIN);
            std::snprintf(d, sizeof d, "scale shown %d editable %d \"%s\"", wp->rowVisible("scale") ? 1 : 0,
                          wp->rowEditable("scale") ? 1 : 0, wp->value("scale"));
            check(wp->rowVisible("scale") && !wp->rowEditable("scale"), "Scale is read-only on a 1.x world", d);
            close_props();
        }
        if (ok && open_props("props-new256.emod")) {
            wp->showTab(WorldPropertiesMenu::TAB_TERRAIN);
            CGRect r;
            check(wp->rowEditable("scale") && wp->sliderRect("scale", &r), "Scale is a slider on a new 256z world", "");
            close_props();
        }
        if (ok && open_props("props-flat.emod")) {
            wp->showTab(WorldPropertiesMenu::TAB_TERRAIN);
            std::snprintf(d, sizeof d, "type \"%s\" hills %d", wp->value("type"), wp->rowVisible("hills") ? 1 : 0);
            check(wp->rowVisible("type") && !wp->rowEditable("type") && !wp->rowVisible("hills") && !wp->rowVisible("caves"),
                  "a Flat world's Terrain tab is its type only", d);
            close_props();
        }
    }

    // ---- 4. cost on a real world
    if (!g_opt.propsWorld.empty()) {
        WorldProps p;
        const bool rd = fm->readWorldProperties(g_opt.propsWorld, &p);
        std::printf("[eden-props] cost %s: %.1f ms, %d columns, %s\n", g_opt.propsWorld.c_str(), rd ? p.openMs : -1.0,
                    rd ? p.columns : -1, rd ? "read" : "UNREADABLE");
        check(rd, "--props-world reads", g_opt.propsWorld.c_str());
    }

    std::printf("[eden-props] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --signs-selftest / --sign-shot  (Stage D / D.3b + D.3c, drawing and placing signs)
// ---------------------------------------------------------------------------------------------

namespace signs_test {

std::string toast() {
    const std::string* t = World::getWorld->hud->sb->current();
    return t ? *t : std::string();
}
// The tool, answered through the REAL prompt: typed text (a literal: SDL keeps the pointer), the
// field's prefill erased first when `erase` > 0, then Return.
int sign_with(int x, int z, int y, int dx, int dy, int dz, float yaw, const char* typed, int erase) {
    const int r = SignTool::use(x, z, y, dx, dy, dz, yaw);
    tick(2);
    if (GLDialog::active()) {
        for (int i = 0; i < erase; ++i) push_tap_key(SDL_SCANCODE_BACKSPACE);
        if (typed && *typed) push_text(typed);
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
    }
    return r;
}
std::string text_of(const TrailerSign& s) { char b[96]; std::memcpy(b, s.text, 95); b[95] = 0; return b; }
// The top solid block of column (x, z), or -1.
int ground(int x, int z) {
    for (int y = T_HEIGHT - 1; y >= 0; --y) if (World::getWorld->terrain->getLand(x, z, y) > 0) return y;
    return -1;
}
// A floating stone with air all around: (x, z, y) in Terrain order, near (px, pz).
bool floating_anchor(int px, int pz, int* ox, int* oz, int* oy) {
    Terrain* t = World::getWorld->terrain;
    for (int k = 0; k < 40; ++k) {
        const int x = px + 4 + (k % 8) * 3, z = pz + (k / 8) * 3;
        const int g = ground(x, z);
        const int y = (g < 0 ? 40 : g) + 4;
        if (y + 2 >= T_HEIGHT) continue;
        bool clear = true;
        for (int dx = -1; dx <= 1 && clear; ++dx)
            for (int dz = -1; dz <= 1 && clear; ++dz)
                for (int dy = -1; dy <= 1 && clear; ++dy)
                    if (t->getLand(x + dx, z + dz, y + dy) != 0) clear = false;
        if (!clear) continue;
        t->updateChunks(x, z, y, TYPE_STONE);
        *ox = x; *oz = z; *oy = y;
        return true;
    }
    return false;
}

}  // namespace signs_test

int run_signs_selftest() {
    using namespace signs_test;
    g_selftestTag = "eden-signs";
    g_tickInput = true;   // the prompt's typed keys reach the field only through the input tick
    SignTool::testAllowBottom = g_opt.signsMutate == "bottom";
    SignTool::testNoAnchorHook = g_opt.signsMutate == "nohook";
    char d[512];
    if (g_opt.world.empty()) { check(false, "--world=NAME (a world with signs) is given", ""); return 1; }
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 4);
    // --world-format=emod converts the .eden on first play and asks "World converted": the tools
    // (rightly, since D.4b) do nothing under a modal, so it goes first. Keep the original.
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }
    WorldTrailer* t = (WorldTrailer*)eden_debug_world_trailer();
    Hud* hud = World::getWorld->hud;
    Terrain* ter = World::getWorld->terrain;
    PlayerState ps = player_state();
    std::printf("[eden-signs] world %s: %s; player %.1f,%.1f,%.1f\n", eden_debug_world_file(),
                t ? t->describe().c_str() : "no trailer", ps.x, ps.y, ps.z);

    // ---- D.3b: the pass's probe. inRange is compared with emod.py's count in the results file.
    const SignRenderer::Stats s0 = SignRenderer::stats();
    std::printf("[eden-signs] probe %s\n", SignRenderer::describe());
    std::snprintf(d, sizeof(d), "%s", SignRenderer::describe());
    check(t && s0.records == (int)t->signCount() && s0.inRange > 0 &&
          s0.drawn + s0.hiddenAir + s0.hiddenFront == s0.inRange && s0.drawn > 0,
          "the sign pass sees the trailer's signs in range and draws them", d);

    // ---- D.3c: the tool on a floating anchor A.
    hud->block_paintcolor = 0;
    int ax, az, ay;
    if (!floating_anchor((int)ps.x, (int)ps.z, &ax, &az, &ay)) { check(false, "a floating anchor near the player", ""); return 1; }
    tick(10);
    const TrailerPos A = TrailerPos::fromTerrainArgs(ax, az, ay);
    const size_t n0 = t->signCount();
    struct Face { int dx, dy, dz, a, c; const char* text; };
    const Face faces[] = { {-1, 0, 0, 0, 0, "west"}, {1, 0, 0, 1, 2, "east"}, {0, 0, -1, 4, 1, "north"},
                           {0, 0, 1, 5, 3, "south"}, {0, 1, 0, 3, 1, "top sign, longer text that wraps over lines"} };
    for (const Face& f : faces) {
        const int r = sign_with(ax, az, ay, f.dx, f.dy, f.dz, 90.0f, f.text, 0);   // yaw 90: looking +z
        const int i = t->findSign(A, f.a);
        std::snprintf(d, sizeof(d), "face a=%d: use %d, record %d%s", f.a, r, i,
                      i >= 0 ? (" b=" + std::to_string(t->sign(i).b) + " c=" + std::to_string(t->sign(i).c) +
                                " '" + text_of(t->sign(i)) + "'").c_str() : "");
        check(r == SignTool::USE_PROMPT_NEW && i >= 0 && text_of(t->sign(i)) == f.text && t->sign(i).b == 45 &&
              t->sign(i).c == f.c, "a sign placed through the prompt: a, b = 45 (no paint), c, text", d);
    }
    hud->sb->clear();
    {
        const int r = sign_with(ax, az, ay, 0, -1, 0, 90.0f, "under", 0);
        std::snprintf(d, sizeof(d), "use %d, record %d, toast '%s'", r, t->findSign(A, 2), toast().c_str());
        check(r == SignTool::USE_REFUSED_BOTTOM && t->findSign(A, 2) < 0 && !GLDialog::active() &&
              toast() == "Can't place a sign under the block", "the bottom face is refused with the toast", d);
    }
    tick(4);
    const SignRenderer::Stats s1 = SignRenderer::stats();
    std::snprintf(d, sizeof(d), "drawn %d -> %d, records %d", s0.drawn, s1.drawn, s1.records);
    check(t->signCount() == n0 + 5 && s1.drawn == s0.drawn + 5, "the five new signs are drawn", d);

    // Edit (prefilled, erased, retyped), keeping its colour; edit with a paint picked recolours.
    {
        const int r = sign_with(ax, az, ay, -1, 0, 0, 90.0f, "WEST edited", 4);
        const int i = t->findSign(A, 0);
        std::snprintf(d, sizeof(d), "use %d, '%s' b=%d", r, i >= 0 ? text_of(t->sign(i)).c_str() : "-", i >= 0 ? t->sign(i).b : -1);
        check(r == SignTool::USE_PROMPT_EDIT && i >= 0 && text_of(t->sign(i)) == "WEST edited" && t->sign(i).b == 45,
              "tapping a sign again edits it (prefilled), colour kept", d);
        hud->block_paintcolor = 19;
        sign_with(ax, az, ay, 1, 0, 0, 90.0f, "", 0);   // Return on the prefill: same text, new paint
        const int j = t->findSign(A, 1);
        std::snprintf(d, sizeof(d), "'%s' b=%d", j >= 0 ? text_of(t->sign(j)).c_str() : "-", j >= 0 ? t->sign(j).b : -1);
        check(j >= 0 && text_of(t->sign(j)) == "east" && t->sign(j).b == 19, "an edit with a paint picked takes its colour", d);
        hud->block_paintcolor = 0;
        const int r2 = sign_with(ax, az, ay, 0, 0, -1, 90.0f, "", 12);
        std::snprintf(d, sizeof(d), "use %d, record %d, signs %zu", r2, t->findSign(A, 4), t->signCount());
        check(r2 == SignTool::USE_PROMPT_EDIT && t->findSign(A, 4) < 0 && t->signCount() == n0 + 4,
              "an empty edit deletes the sign", d);
    }
    // A block in front of a sign hides it; the record stays.
    {
        ter->updateChunks(ax, az + 1, ay, TYPE_STONE);   // in front of "south"
        tick(3);
        const SignRenderer::Stats h = SignRenderer::stats();
        ter->updateChunks(ax, az + 1, ay, TYPE_NONE);
        tick(3);
        std::snprintf(d, sizeof(d), "hiddenFront %d -> %d, south record %d, drawn back %d", s1.hiddenFront, h.hiddenFront,
                      t->findSign(A, 5), SignRenderer::stats().drawn);
        check(h.hiddenFront == s1.hiddenFront + 1 && t->findSign(A, 5) >= 0, "a block in front hides a sign, never deletes it", d);
    }

    // The real path: the picker's SIGN cell arms the tool, a screen tap at the crosshair places one.
    {
        tap_hud(1);
        tick(10);
        hud->pickerPageStep(1);
        tick(4);
        int cell = -1;
        for (int i = 0; i < Hud::pickerCellCount(1); ++i) if (Hud::pickerCell(1, i) == HUD_TOOL_SIGN) cell = i;
        if (cell >= 0) ui_tap_rect(hud->blockBounds[cell]);
        tick(10);
        std::snprintf(d, sizeof(d), "mode %d blocktype %d", hud->mode, hud->blocktype);
        check(hud->mode == MODE_BUILD && hud->blocktype == HUD_TOOL_SIGN, "the picker's SIGN cell arms the tool", d);
        // Aim at A's free bottom-less -z face... it was just deleted: aim the crosshair at its centre.
        Player* pl = World::getWorld->player;
        Camera* cam = World::getWorld->cam;
        const float fx = ax + 0.5f, fy = ay + 0.5f, fz = (float)az;
        pl->pos.x = fx; pl->pos.z = fz - 3.0f; pl->pos.y = fy;
        pl->vel.x = pl->vel.y = pl->vel.z = 0;
        for (int k = 0; k < 4; ++k) {
            tick(1);
            const float dx = fx - cam->px, dy = fy - cam->py, dz = fz - cam->pz;
            pl->yaw = std::atan2(dz, dx) * 180.0f / (float)M_PI;
            pl->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0f / (float)M_PI;
        }
        tick(1);
        ui_pointer(0, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(1);
        ui_pointer(2, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(2);
        const bool prompted = GLDialog::active();
        if (prompted) { push_text("tapped"); tick(2); push_tap_key(SDL_SCANCODE_RETURN); tick(3); }
        const int i = t->findSign(A, 4);
        std::snprintf(d, sizeof(d), "prompt %d, -z record %d%s, cam %.2f,%.2f,%.2f", prompted, i,
                      i >= 0 ? (" '" + text_of(t->sign(i)) + "'").c_str() : "", cam->px, cam->py, cam->pz);
        check(prompted && i >= 0 && text_of(t->sign(i)) == "tapped" && t->sign(i).c == 1,
              "a screen tap with the tool places a sign on the face it hits", d);
        // Back to an ordinary block, so nothing below builds a sign by accident.
        hud->blocktype = TYPE_STONE;
    }

    // The user's Mac bug (2026-10-10): a RIGHT-CLICK that opened the sign prompt left the hold-to-
    // build pulse running (its release arrived with the cursor freed and went to the menu path), so
    // the prompt re-opened every 200 ms; a key held at the time kept walking the player after it
    // closed. Real SDL events through the input translator, real wall-clock time. Twice: the click
    // alone (the prompt must not come back), then with S held as well (the player must not walk).
    for (int withKey = 0; withKey < 2; ++withKey) {
        int qx, qz, qy;
        floating_anchor(ax - 8 - 8 * withKey, az - 8, &qx, &qz, &qy);
        tick(4);
        hud->mode = MODE_BUILD;
        hud->blocktype = HUD_TOOL_SIGN;
        Player* pl = World::getWorld->player;
        Camera* cam = World::getWorld->cam;
        const float fx = qx + 0.5f, fy = qy + 0.5f, fz = (float)qz;
        pl->pos.x = fx; pl->pos.z = fz - 3.0f; pl->pos.y = fy;
        pl->vel.x = pl->vel.y = pl->vel.z = 0;
        for (int k = 0; k < 4; ++k) {
            tick(1);
            const float dx = fx - cam->px, dy = fy - cam->py, dz = fz - cam->pz;
            pl->yaw = std::atan2(dz, dx) * 180.0f / (float)M_PI;
            pl->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0f / (float)M_PI;
        }
        if (withKey) push_key(SDL_SCANCODE_S, true);     // walking backwards, as the user was
        push_mouse_button(SDL_BUTTON_RIGHT, true);
        bool opened = false;
        for (int k = 0; k < 60 && !opened; ++k) { tick(1); opened = GLDialog::active(); }
        tick(6);   // a human click outlasts the frame that opened the prompt: the release must
                   // arrive AFTER the dialog freed the cursor, which is what routed it astray
        if (withKey) push_key(SDL_SCANCODE_S, false);
        push_mouse_button(SDL_BUTTON_RIGHT, false);
        tick(2);
        push_tap_key(SDL_SCANCODE_ESCAPE);               // cancel: no sign
        tick(3);
        pl->vel.x = pl->vel.y = pl->vel.z = 0;           // in the air nothing brakes the steps taken
        const PlayerState p0 = player_state();           // before the prompt; a held key re-accelerates
        int reopened = 0;
        for (int k = 0; k < 40; ++k) {
            SDL_Delay(20);
            tick(1);
            if (GLDialog::active()) { ++reopened; push_tap_key(SDL_SCANCODE_ESCAPE); }
        }
        if (GLDialog::active()) GLDialog::dismiss();
        const PlayerState p1 = player_state();
        const float moved = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
        std::snprintf(d, sizeof(d), "%s: prompt opened %d, frames with a prompt in 0.8 s after cancel %d, walked %.2f",
                      withKey ? "click + S" : "click", opened, reopened, moved);
        check(opened && reopened == 0 && moved < 0.3f,
              withKey ? "a right-click + held key that open the prompt leave no key held behind"
                      : "a right-click that opens the prompt leaves no build pulse behind (it never re-opens)", d);
        hud->blocktype = TYPE_STONE;
    }

    // ---- D.4b: the CMD tool. Placing (steel + an empty CMB1 record), the skin, editing through the
    // multi-line prompt (cap 511, empty clears), the running button, the per-world cap, a sign on
    // its face, the picker cell + a real screen tap, mining.
    TrailerPos keptCmd = {0, 0, 0};
    std::string keptScript;
    {
        int ex, ez, ey;
        floating_anchor(ax + 16, az - 8, &ex, &ez, &ey);
        {   // stand by it: the anchor sits at its column's ground + 4, which can be far above or below
            Player* pl = World::getWorld->player;
            pl->pos.x = ex + 0.5f; pl->pos.y = ey + 0.5f; pl->pos.z = ez - 3.0f;
            pl->vel.x = pl->vel.y = pl->vel.z = 0;
        }
        tick(4);
        const SignRenderer::Stats k0 = SignRenderer::stats();
        hud->mode = MODE_BUILD;
        hud->blocktype = HUD_TOOL_CMD;
        hud->block_paintcolor = 19;
        const size_t c0 = t->cmdCount();
        const int r = CmdTool::use(ex, ez, ey, 1, 0, 0);          // on the stone's +x face
        const TrailerPos P = TrailerPos::fromTerrainArgs(ex + 1, ez, ey);
        const int i = t->findCmd(P);
        tick(3);
        const SignRenderer::Stats k1 = SignRenderer::stats();
        std::snprintf(d, sizeof(d), "use %d, voxel %d colour %d, record %d of %zu, script '%s'; hud type %d paint %d; "
                      "skinned %d -> %d, faces +%d", r, ter->getLand(ex + 1, ez, ey), ter->getColor(ex + 1, ez, ey),
                      i, t->cmdCount(), i >= 0 ? t->cmd(i).script : "-", hud->blocktype, hud->block_paintcolor,
                      k0.cmdInRange, k1.cmdInRange, k1.cmdFaces - k0.cmdFaces);
        check(r == CmdTool::USE_PLACED && ter->getLand(ex + 1, ez, ey) == TYPE_STEEL && ter->getColor(ex + 1, ez, ey) == 0 &&
              i >= 0 && t->cmdCount() == c0 + 1 && t->cmd(i).script[0] == 0 && t->cmd(i).flags == 0 &&
              hud->blocktype == HUD_TOOL_CMD && hud->block_paintcolor == 19,
              "the CMD tool places unpainted steel + an empty CMB1 record, the tool stays armed", d);
        {
            const PlayerState pq = player_state();
            char more[160];
            std::snprintf(more, sizeof(more), "; block %d,%d,%d (file x,y,z) player %.1f,%.1f,%.1f", P.x, P.y, P.z, pq.x, pq.y, pq.z);
            std::strncat(d, more, sizeof(d) - std::strlen(d) - 1);
        }
        check(k1.cmdInRange == k0.cmdInRange + 1 && k1.cmdFaces - k0.cmdFaces == 5,
              "the new command block is skinned on its 5 exposed faces (not the one against the stone)", d);
        hud->block_paintcolor = 0;
        // A second tap in the cell it filled: refused (occupied), nothing added.
        const int rr = CmdTool::use(ex, ez, ey, 1, 0, 0);
        // ...whereas tapping the command block itself is an edit.
        // (rr: the stone's +x face now leads into the steel, which is not air.)
        std::snprintf(d, sizeof(d), "use %d, cmds %zu", rr, t->cmdCount());
        check(rr == CmdTool::USE_REFUSED_CELL && t->cmdCount() == c0 + 1, "a CMD tap into an occupied cell is refused", d);

        // Edit through the real prompt.
        const int re = CmdTool::use(ex + 1, ez, ey, 0, 1, 0);
        tick(2);
        const bool up = GLDialog::active();
        push_text("say hello; wait 1; say bye");
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
        int j = t->findCmd(P);
        std::snprintf(d, sizeof(d), "use %d, prompt %d, script '%s'", re, up, j >= 0 ? t->cmd(j).script : "-");
        check(re == CmdTool::USE_PROMPT_EDIT && up && j >= 0 && std::string(t->cmd(j).script) == "say hello; wait 1; say bye",
              "tapping a command block edits its script through the prompt", d);
        // Over-long: the field stops at 511 bytes.
        CmdTool::use(ex + 1, ez, ey, -1, 0, 0);
        tick(2);
        push_text("fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; "
                  "fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; "
                  "fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; "
                  "fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; "
                  "fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; "
                  "fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; fill ~1 ~0 ~1 ~4 ~4 ~4 stone; ");
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
        j = t->findCmd(P);
        const size_t longLen = j >= 0 ? std::strlen(t->cmd(j).script) : 0;
        std::snprintf(d, sizeof(d), "script length %zu", longLen);
        check(longLen == CmdTool::MAX_SCRIPT && std::string(t->cmd(j).script).compare(0, 26, "say hello; wait 1; say bye") == 0,
              "a script stops at 511 bytes (the prefill kept, the rest cut)", d);
        // Empty: the block stays, its script is cleared (cmd.html).
        CmdTool::use(ex + 1, ez, ey, 0, 0, 1);
        tick(2);
        for (int k = 0; k < CmdTool::MAX_SCRIPT + 4; ++k) push_key(SDL_SCANCODE_BACKSPACE, true);
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
        j = t->findCmd(P);
        std::snprintf(d, sizeof(d), "voxel %d, record %d, script '%s'", ter->getLand(ex + 1, ez, ey), j,
                      j >= 0 ? t->cmd(j).script : "-");
        check(ter->getLand(ex + 1, ez, ey) == TYPE_STEEL && j >= 0 && t->cmd(j).script[0] == 0,
              "an empty edit keeps the command block and clears its script", d);
        if (GLDialog::active()) GLDialog::dismiss();

        // The button: green while running (D.4c's hook), red otherwise.
        // X.3: cmdRunning counts EVERY in-range running command block, so a world that already has
        // some running (the adder has 3) made the old absolute `1 -> 0` check fail on a correct
        // engine. Compare against this block's own idle baseline instead.
        CmdTool::setRunning(P, false);
        tick(2);
        const int runBase = SignRenderer::stats().cmdRunning;
        CmdTool::setRunning(P, true);
        tick(2);
        const int runOn = SignRenderer::stats().cmdRunning;
        CmdTool::setRunning(P, false);
        tick(2);
        const int runOff = SignRenderer::stats().cmdRunning;
        std::snprintf(d, sizeof(d), "running %d -> %d -> %d (idle baseline first)", runBase, runOn, runOff);
        check(runOn == runBase + 1 && runOff == runBase, "a running command block draws the green button, idle the red", d);

        // A sign on its face sits over the panel (the D.3b board).
        const int onCmd0 = SignRenderer::stats().onCmd;
        sign_with(ex + 1, ez, ey, 1, 0, 0, 0.0f, "GO", 0);
        tick(3);
        std::snprintf(d, sizeof(d), "onCmd %d -> %d", onCmd0, SignRenderer::stats().onCmd);
        check(t->findSign(P, 1) >= 0 && SignRenderer::stats().onCmd == onCmd0 + 1, "a sign on a command block's face is its panel board", d);

        // The per-world cap: filler records far away up to 512, then the tool refuses with a toast.
        {
            std::vector<TrailerPos> filler;
            for (int k = 0; (int)t->cmdCount() < CmdTool::MAX_PER_WORLD; ++k) {
                const TrailerPos F = {P.x + 1000 + k, P.y + 1000, 1};
                if (t->addCmd(F, "") != WorldTrailer::TR_OK) break;
                filler.push_back(F);
            }
            hud->sb->clear();
            int fx2, fz2, fy2;
            floating_anchor(ax + 24, az, &fx2, &fz2, &fy2);
            const int rf = CmdTool::use(fx2, fz2, fy2, 0, 1, 0);
            std::snprintf(d, sizeof(d), "cmds %zu, use %d, cell %d, toast '%s'", t->cmdCount(), rf,
                          ter->getLand(fx2, fz2, fy2 + 1), toast().c_str());
            check(t->cmdCount() == CmdTool::MAX_PER_WORLD && rf == CmdTool::USE_REFUSED_FULL &&
                  ter->getLand(fx2, fz2, fy2 + 1) == 0 && toast() == "This world has 512 command blocks already",
                  "the 513th command block is refused with a toast, nothing built", d);
            for (const TrailerPos& F : filler) { const int k = t->findCmd(F); if (k >= 0) t->removeCmd((size_t)k); }
            ter->updateChunks(fx2, fz2, fy2, TYPE_NONE);
        }

        // The real path: the picker's CMD cell arms the tool; a screen tap on a face places one.
        {
            hud->blocktype = TYPE_STONE;
            tap_hud(1);
            tick(10);
            hud->pickerPageStep(1);
            tick(4);
            int cell = -1;
            for (int k = 0; k < Hud::pickerCellCount(1); ++k) if (Hud::pickerCell(1, k) == HUD_TOOL_CMD) cell = k;
            if (cell >= 0) ui_tap_rect(hud->blockBounds[cell]);
            tick(10);
            const bool armed = hud->mode == MODE_BUILD && hud->blocktype == HUD_TOOL_CMD;
            int gx, gz, gy;
            floating_anchor(ax - 24, az + 8, &gx, &gz, &gy);
            tick(4);
            Player* pl = World::getWorld->player;
            Camera* cam = World::getWorld->cam;
            const float fx = gx + 0.5f, fy = gy + 0.5f, fz = (float)gz;   // its -z face
            pl->pos.x = fx; pl->pos.z = fz - 3.0f; pl->pos.y = fy;
            pl->vel.x = pl->vel.y = pl->vel.z = 0;
            for (int k = 0; k < 4; ++k) {
                tick(1);
                const float dx = fx - cam->px, dy = fy - cam->py, dz = fz - cam->pz;
                pl->yaw = std::atan2(dz, dx) * 180.0f / (float)M_PI;
                pl->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0f / (float)M_PI;
            }
            tick(1);
            ui_pointer(0, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(1);
            ui_pointer(2, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(2);
            const TrailerPos G = TrailerPos::fromTerrainArgs(gx, gz - 1, gy);
            std::snprintf(d, sizeof(d), "armed %d, cell %d, record %d, prompt %d", armed, ter->getLand(gx, gz - 1, gy),
                          t->findCmd(G), GLDialog::active());
            check(armed && ter->getLand(gx, gz - 1, gy) == TYPE_STEEL && t->findCmd(G) >= 0 && !GLDialog::active(),
                  "the picker's CMD cell arms the tool and a screen tap places a command block (no prompt)", d);
            // Kept for the reload check, with a script.
            CmdTool::use(gx, gz - 1, gy, 0, 1, 0);
            tick(2);
            push_text("say kept");
            tick(2);
            push_tap_key(SDL_SCANCODE_RETURN);
            tick(3);
            keptCmd = G;
            keptScript = t->findCmd(G) >= 0 ? t->cmd(t->findCmd(G)).script : "";
            hud->blocktype = TYPE_STONE;
        }

        // Mining the command block takes the record, its sign and its running flag.
        CmdTool::setRunning(P, true);
        ter->destroyBlock(P.x, P.y, P.z);
        tick(3);
        std::snprintf(d, sizeof(d), "voxel %d, record %d, signs %zu, running %d", ter->getLand(P.x, P.y, P.z), t->findCmd(P),
                      t->signsOn(P).size(), CmdTool::running(P));
        check(ter->getLand(P.x, P.y, P.z) == 0 && t->findCmd(P) < 0 && t->signsOn(P).empty() && !CmdTool::running(P),
              "mining a placed command block removes its record, its sign and its running flag", d);
    }

    // Mining an anchor and blowing one up take their signs.
    {
        int bx, bz, by, cx, cz, cy;
        floating_anchor(ax + 8, az + 8, &bx, &bz, &by);
        floating_anchor(ax - 16, az + 16, &cx, &cz, &cy);
        tick(4);
        sign_with(bx, bz, by, 0, 1, 0, 0.0f, "mine me", 0);
        sign_with(cx, cz, cy, 1, 0, 0, 0.0f, "blow me up", 0);
        const TrailerPos B = TrailerPos::fromTerrainArgs(bx, bz, by), C = TrailerPos::fromTerrainArgs(cx, cz, cy);
        const bool placed = t->findSign(B, 3) >= 0 && t->findSign(C, 1) >= 0;
        ter->destroyBlock(bx, bz, by);
        tick(3);
        std::snprintf(d, sizeof(d), "placed %d, B block %d, B signs %zu", placed, ter->getLand(bx, bz, by), t->signsOn(B).size());
        check(placed && ter->getLand(bx, bz, by) == 0 && t->signsOn(B).empty(), "mining the anchor removes its sign", d);
        ter->updateChunks(cx - 1, cz, cy, TYPE_TNT);
        ter->burnBlock(cx - 1, cz, cy, true);   // lit as a blast lights it: a 0.5-0.8 s fuse,
        tick(420);                              // which Terrain::update runs at a quarter speed
        std::snprintf(d, sizeof(d), "C block %d, C signs %zu", ter->getLand(cx, cz, cy), t->signsOn(C).size());
        check(ter->getLand(cx, cz, cy) == 0 && t->signsOn(C).empty(), "TNT next to the anchor removes its sign", d);
    }

    // A command block's steel mined: its CMB1 record (and its signs) go, every other record stays.
    TrailerPos minedCmd = {0, 0, 0};
    size_t cmdsAfter = t->cmdCount();
    size_t firstOld = 0;   // not the one D.4b's leg placed and keeps for the reload check
    while (firstOld < t->cmdCount() && t->cmd(firstOld).pos() == keptCmd) ++firstOld;
    if (firstOld < t->cmdCount()) {
        const TrailerCmd c0 = t->cmd(firstOld);
        const TrailerPos P = c0.pos();
        std::vector<TrailerCmd> otherCmds;
        std::vector<TrailerSign> otherSigns;
        for (size_t i = 0; i < t->cmdCount(); ++i) if (i != firstOld) otherCmds.push_back(t->cmd(i));
        for (size_t i = 0; i < t->signCount(); ++i) if (!(t->sign(i).pos() == P)) otherSigns.push_back(t->sign(i));
        const size_t onIt = t->signsOn(P).size();
        ter->destroyBlock(P.x, P.y, P.z);
        tick(3);
        bool same = t->cmdCount() == otherCmds.size() && t->signCount() == otherSigns.size();
        for (size_t i = 0; same && i < otherCmds.size(); ++i) same = std::memcmp(&t->cmd(i), &otherCmds[i], sizeof(TrailerCmd)) == 0;
        for (size_t i = 0; same && i < otherSigns.size(); ++i) same = std::memcmp(&t->sign(i), &otherSigns[i], sizeof(TrailerSign)) == 0;
        std::snprintf(d, sizeof(d), "steel at %d,%d,%d -> %d; cmds %zu, signs on it %zu -> %zu, the rest identical %d",
                      P.x, P.y, P.z, ter->getLand(P.x, P.y, P.z), t->cmdCount(), onIt, t->signsOn(P).size(), same);
        check(ter->getLand(P.x, P.y, P.z) == 0 && t->findCmd(P) < 0 && t->signsOn(P).empty() && same,
              "mining a command block removes only its CMB1 record (and its signs)", d);
        minedCmd = P;
        cmdsAfter = t->cmdCount();
    }

    // Save, reload: A's four signs with their a/b/c and text, the removals kept.
    std::vector<TrailerSign> want;
    for (int i : t->signsOn(A)) want.push_back(t->sign(i));
    const size_t signsAfter = t->signCount();
    save_world();
    if (!quit_to_menu()) return 1;
    tick(60);
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(30);
    t = (WorldTrailer*)eden_debug_world_trailer();
    {
        bool ok = t && t->signCount() == signsAfter && t->cmdCount() == cmdsAfter && t->signsOn(A).size() == want.size();
        for (size_t k = 0; ok && k < want.size(); ++k) {
            const int i = t->findSign(A, want[k].a);
            ok = i >= 0 && std::memcmp(&t->sign(i), &want[k], sizeof(TrailerSign)) == 0;
        }
        if (ok && cmdsAfter) ok = t->findCmd(minedCmd) < 0;
        if (ok) {   // D.4b: the command block placed by a screen tap, with its script
            const int k = t->findCmd(keptCmd);
            ok = k >= 0 && keptScript == "say kept" && std::string(t->cmd(k).script) == keptScript;
        }
        std::snprintf(d, sizeof(d), "%s; A has %zu (want %zu)", t ? t->describe().c_str() : "no trailer",
                      t ? t->signsOn(A).size() : 0, want.size());
        check(ok, "after save + reload: the placed/edited signs byte-identical, the removed ones gone", d);
    }
    std::printf("[eden-signs] file %s anchor A %d,%d,%d\n", eden_debug_world_file(), A.x, A.y, A.z);
    std::printf("[eden-signs] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// A real window at --at looking --look=YAW,PITCH: PREFIX-signs.bmp and the probe line. Frames are
// ticked until every sign in range has its text (textures are built a few per frame).
int run_sign_shot() {
    SDL_GL_SetSwapInterval(0);
    if (g_opt.world.empty()) { std::fprintf(stderr, "[eden-shot] --sign-shot needs --world\n"); return 1; }
    tick(30);
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    Player* pl = World::getWorld->player;
    for (int k = 0; k < g_opt.frames; ++k) {
        if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
        if (g_opt.haveLook) { pl->yaw = g_opt.look[0]; pl->pitch = g_opt.look[1]; }
        tick(1);
    }
    const SignRenderer::Stats& s = SignRenderer::stats();
    for (int k = 0; k < 120 && s.withText < s.drawn; ++k) tick(1);
    std::printf("[eden-shot] signs %s\n", SignRenderer::describe());
    eden_mem_trace("signs");
    capture("signs");
    return 0;
}

}  // namespace g_props
}  // namespace harness
