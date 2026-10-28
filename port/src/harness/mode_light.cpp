// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "light".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_light {


// ---------------------------------------------------------------------------------------------
// --light-selftest  (Stage R / R.3, the light store)
// ---------------------------------------------------------------------------------------------
// --stage1's `.light` hash covers the sweep on load. This covers what the sweep never does: the
// edit paths (buildBlock's +1 splat, destroyBlock's and paintBlock's -1, which subtracts and
// clamps at 0), saturation at 255 (five overlapping lightboxes), the y clip at the top and bottom
// of the window, and the toroidal seam (a light whose radius wraps from index T_SIZE-1 to 0 on both
// axes). Every step prints the light hash, so a run against the old dense array is the reference
// for a run against anything else; the run is also self-checking: after everything is removed
// and a fresh sweep runs, the light must be exactly what the world had before the test touched it.
int run_light_selftest() {
    g_selftestTag = "eden-light";
    const char* name = g_opt.world.empty() ? "light-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    auto field = [](const char* js, const char* key) -> long long {
        const char* k = std::strstr(js, key);
        return k ? atoll(k + std::strlen(key)) : -1;
    };
    auto hashOf = [](const char* js) {
        const char* k = std::strstr(js, "\"hash\":\"");
        return k ? std::string(k + 8, 16) : std::string();
    };
    auto settle = [&] {
        for (int i = 0; i < 600 && field(eden_debug_light_state(), "\"pending\":") != 0; i++) tick(1);
    };
    int step = 0;
    auto show = [&](const char* what) {
        const char* js = eden_debug_light_state();
        std::printf("[eden-light] step %d %-22s %s\n", step++, what, js);
        std::fflush(stdout);
        char d[64];
        std::snprintf(d, sizeof(d), "mismatch %lld", field(js, "\"mismatch\":"));
        if (std::strstr(js, "\"mismatch\":"))
            check(field(js, "\"mismatch\":") == 0, "self-check: the store matches the dense mirror", d);
        return std::string(js);
    };

    settle();
    const std::string s0 = show("baseline");
    // Anchors off the player's REAL position (see --empty-bit-selftest for why not --at).
    const PlayerState ps = player_state();
    const int px = (int)ps.x, pz = (int)ps.z, H = g_opt.height;
    // The seam: world x with x % T_SIZE == T_SIZE-2 (g_offcx is a multiple of T_SIZE), nearest the
    // player, so x-5..x+5 covers toroidal indices T_SIZE-7..T_SIZE-1 and 0..3. Same for z with 1.
    int xs = px - px % T_SIZE + T_SIZE - 2; if (xs - px > T_SIZE / 2) xs -= T_SIZE;
    int zs = pz - pz % T_SIZE + 1;          if (zs - pz > T_SIZE / 2) zs -= T_SIZE;
    if (pz - zs > T_SIZE / 2) zs += T_SIZE;
    const int yt = H - 3;
    struct P { int x, z, y; };
    const P A{xs, zs, yt}, B{xs + 1, zs, yt}, C{xs - 1, zs, yt}, D{xs, zs + 1, yt}, E{xs, zs, yt - 1};
    const P F{px + 3, pz + 3, 2};

    eden_debug_light_edit(0, A.x, A.z, A.y, 0);
    const std::string s1 = show("place A (seam, top)");
    check(hashOf(s1.c_str()) != hashOf(s0.c_str()) && field(s1.c_str(), "\"lit\":") > field(s0.c_str(), "\"lit\":"),
          "placing a lightbox adds light", s1.c_str());
    for (const P& p : {B, C, D, E}) eden_debug_light_edit(0, p.x, p.z, p.y, 0);
    show("place B-E (saturate)");
    eden_debug_light_edit(0, F.x, F.z, F.y, 5);
    show("place F (bottom clip)");
    eden_debug_light_edit(2, A.x, A.z, A.y, 12);
    show("repaint A");
    eden_debug_light_edit(1, B.x, B.z, B.y, 0);
    eden_debug_light_edit(1, C.x, C.z, C.y, 0);
    show("break B, C");
    for (const P& p : {A, D, E, F}) eden_debug_light_edit(1, p.x, p.z, p.y, 0);
    show("break A, D, E, F");
    // B.5b-1c: the window EDGE (not the toroidal seam above, which is mid-window). G on the window's
    // last resident block in +x: its light past the edge would wrap, through the toroidal index, into
    // the -x edge column. It must light its own side and leave the wrapped box exactly as it was.
    {
        const int ox = windowOriginChunk(World::getWorld->player->pos.x);
        const int xe = (ox + CHUNKS_PER_SIDE) * CHUNK_SIZE - 1;
        const P G{xe, pz, yt - 2};
        auto box = [&](int x1, int x2) {   // light summed over x in [x1,x2], z/y within 5 of G
            long long sum = 0;
            for (int x = x1; x <= x2; ++x)
                for (int z = G.z - 5; z <= G.z + 5; ++z)
                    for (int y = G.y - 5; y <= G.y + 5; ++y) {
                        if (y < 0 || y >= H) continue;
                        const Vector8 v = lightAtToroidal((x + g_offcx) % T_SIZE, (z + g_offcz) % T_SIZE, y);
                        sum += v.x + v.y + v.z;
                    }
            return sum;
        };
        const long long in0 = box(xe - 5, xe), out0 = box(xe + 1, xe + 5);
        eden_debug_light_edit(0, G.x, G.z, G.y, 7);
        const long long in1 = box(xe - 5, xe), out1 = box(xe + 1, xe + 5);
        show("place G (window edge)");
        char d[160];
        std::snprintf(d, sizeof(d), "edge column resident %d, next %d; inside %lld -> %lld, wrapped %lld -> %lld",
                      (int)isColumnResident(xe, G.z), (int)isColumnResident(xe + 1, G.z), in0, in1, out0, out1);
        check(isColumnResident(xe, G.z) && !isColumnResident(xe + 1, G.z) && in1 > in0 && out1 == out0,
              "a lightbox at the window edge lights its side and nothing past it (no wrap)", d);
        eden_debug_light_edit(1, G.x, G.z, G.y, 0);
        show("break G");
    }
    tick(30);
    eden_debug_light_edit(3, 0, 0, 0, 0);
    settle();
    const std::string s7 = show("fresh sweep");
    check(hashOf(s7.c_str()) == hashOf(s0.c_str()), "after a fresh sweep the light is the pre-test light", s7.c_str());
#if 1   // R.3: the sparse store's promises (would fail against the dense array, by design)
    {
        const long long dense = 3LL * T_SIZE * T_SIZE * H;
        const long long b = field(s7.c_str(), "\"bytes\":");
        char d[96];
        std::snprintf(d, sizeof(d), "%lld bytes held vs %lld dense", b, dense);
        check(b >= 0 && b * 8 < dense, "the store holds well under 1/8 of the dense array", d);
    }
#endif

    std::printf("[eden-light] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --ring-light-selftest  (B.5b-1c: light per landed column)
// ---------------------------------------------------------------------------------------------
// Plan §3.4 gate 1's light leg, in one process. A walk lands columns one a frame and re-lights the
// 3x3 of slots around each (light_syncLandedColumns); the reference is the full sweep a load or warp
// runs, plus a re-mesh of every chunk, against the SAME window -- so any difference is the per-column
// path, nothing else. At night (the lattice prep paints it), because by day the mesher never reads
// the light and a stale mesh would hide. The cross-process form (fresh load at the end point, also
// chunk state) is WORKING/b5b1/ring-light-gate.sh.
int run_ring_light_selftest() {
    g_selftestTag = "eden-ring-light";
    const char* name = g_opt.world.empty() ? "ring-light-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (!g_opt.haveAt) { g_opt.at[0] = 65536.5f; g_opt.at[1] = 60.0f; g_opt.at[2] = 65536.5f; }
    const int placed = place_light_lattice(23, "eden-ring-light");
    const float X0 = g_opt.at[0], Y0 = g_opt.at[1], Z0 = g_opt.at[2];
    eden_console_teleport(X0, Y0, Z0);
    tick(g_opt.frames);
    auto field = [](const char* js, const char* key) -> long long {
        const char* k = std::strstr(js, key);
        return k ? atoll(k + std::strlen(key)) : -1;
    };
    auto str = [](const char* js, const char* key) {
        const char* k = std::strstr(js, key);
        if (!k) return std::string();
        k += std::strlen(key);
        const char* e = std::strchr(k, '"');
        return e ? std::string(k, e - k) : std::string();
    };
    auto settle = [&] {
        for (int i = 0; i < 1200 && field(eden_debug_light_state(), "\"pending\":") != 0; i++) tick(1);
        tick(120);
    };
    settle();
    const int bulk0 = g_bulk_reloads, ring0 = g_ring_columns_landed;
    const long long reb0 = field(eden_debug_light_state(), "\"rebuilds\":");
    float wx = X0, wz = Z0;
    auto walk = [&](float dx, float dz) {   // 0.25 block a frame, the F3 driver's walk
        const int steps = (int)std::ceil(std::sqrt(dx * dx + dz * dz) / 0.25f);
        for (int k = 1; k <= steps; ++k) {
            const float f = (float)k / (float)steps;
            eden_console_teleport(wx + dx * f, Y0, wz + dz * f);
            tick(1);
        }
        wx += dx; wz += dz;
    };
    walk(160, 0);
    walk(0, 160);
    // P.10: a lightbox built in a column remembered as lightbox-free must make it be scanned again.
    // The column is the last resident one in +x, so the next chunk of +x walking lands the column
    // beside it and re-lights its 3x3 (which holds this one) from scratch; a column still trusted as
    // free would lose the new lightbox's light there, and the sweep below would not.
    settle();
    {
        const int ex = windowOriginChunk(wx) + CHUNKS_PER_SIDE - 1, pcz = (int)wz / CHUNK_SIZE;
        int lcz = INT_MIN;
        for (int dz = 0; dz <= CHUNKS_PER_SIDE / 2 && lcz == INT_MIN; ++dz)
            for (int cz : {pcz + dz, pcz - dz})
                if (lcz == INT_MIN && light_columnKnownFree(ex, cz)) lcz = cz;
        char d2[160];
        bool built = false, forgot = false;
        if (lcz != INT_MIN) {
            const int x = ex * CHUNK_SIZE + 8, z = lcz * CHUNK_SIZE + 8, H = g_world_height;
            int y = H - 1;
            while (y >= 0 && getLandc(x, z, y) == TYPE_NONE) --y;
            y = std::min(y + 1, H - 2);
            eden_debug_light_edit(0, x, z, y, 20);
            built = getLandc(x, z, y) == TYPE_LIGHTBOX;
            forgot = !light_columnKnownFree(ex, lcz);
            std::snprintf(d2, sizeof(d2), "column (%d,%d), lightbox at (%d,%d,%d) %s, still trusted free %d",
                          ex, lcz, x, z, y, built ? "built" : "NOT built", (int)!forgot);
        } else std::snprintf(d2, sizeof(d2), "no remembered-free column in chunk x %d", ex);
        check(built && forgot, "a lightbox built in a remembered-free column un-forgets it (P.10)", d2);
    }
    walk(32, 0);
    settle();
    const std::string lw = eden_debug_light_state(), cw = eden_debug_mesh_checksum();
    const long long reb1 = field(lw.c_str(), "\"rebuilds\":");
    std::printf("[eden-ring-light] walked  light %s\n[eden-ring-light] walked  mesh  %s\n", lw.c_str(), cw.c_str());
    char d[256];
    std::snprintf(d, sizeof(d), "placed %d, ring columns +%d, bulk reloads %d -> %d, rebuilds +%lld",
                  placed, g_ring_columns_landed - ring0, bulk0, g_bulk_reloads, reb1 - reb0);
    check(placed > 0 && g_bulk_reloads == bulk0 && g_ring_columns_landed > ring0 && reb1 > reb0,
          "the walk streams and re-lights per landed column", d);

    // The reference: what a load does to this window -- clear and sweep the light, re-mesh it all.
    eden_debug_light_edit(3, 0, 0, 0, 0);
    {
        memset(chunksToUpdate, true, sizeof(bool) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN);
        memset(columnsToUpdate, true, sizeof(bool) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
    }
    settle();
    const std::string ls = eden_debug_light_state(), cs = eden_debug_mesh_checksum();
    std::printf("[eden-ring-light] swept   light %s\n[eden-ring-light] swept   mesh  %s\n", ls.c_str(), cs.c_str());
    std::snprintf(d, sizeof(d), "walked %s lit %lld, swept %s lit %lld", str(lw.c_str(), "\"hash\":\"").c_str(),
                  field(lw.c_str(), "\"lit\":"), str(ls.c_str(), "\"hash\":\"").c_str(), field(ls.c_str(), "\"lit\":"));
    check(field(lw.c_str(), "\"lit\":") > 0 && str(lw.c_str(), "\"hash\":\"") == str(ls.c_str(), "\"hash\":\""),
          "the walked light equals a full sweep of the same window", d);
    std::snprintf(d, sizeof(d), "walked full %s, swept full %s", str(cw.c_str(), "\"full\":\"").c_str(),
                  str(cs.c_str(), "\"full\":\"").c_str());
    check(!str(cw.c_str(), "\"full\":\"").empty() && str(cw.c_str(), "\"geom\":\"") == str(cs.c_str(), "\"geom\":\"") &&
          str(cw.c_str(), "\"full\":\"") == str(cs.c_str(), "\"full\":\""),
          "the walked meshes equal a re-mesh of the same window (night: light is baked in)", d);

    std::printf("[eden-ring-light] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --shadow-selftest  (H.2: baked top-face shadows)
// ---------------------------------------------------------------------------------------------
// shadows-plan.md §8.2, in one process. The shadow-top table must match a fresh scan after every
// landing and every edit (`stale` 0), a canopy in the NEXT chunk up must re-shade the floor chunk
// (only the edit hook's dirtying can do that: updateChunks alone would not re-mesh it), mining it
// must give back the floor's exact pre-canopy mesh, a lightbox still lights a shaded floor at night,
// a walk's meshes must equal a re-scan + re-mesh of the same window, and Off must mesh exactly as
// before. Classic unless --shadows= says otherwise. --shadow-mutate=nohook|nodirty|nolanding|nodiag
// put the bug back; each must FAIL (shadows-plan.md §8.2 names the step).
int run_shadow_selftest() {
    g_selftestTag = "eden-shadow";
    const char* name = g_opt.world.empty() ? "shadow-selftest" : g_opt.world.c_str();
    if (g_shadow_mode == SHADOW_OFF) shadow_setOverride(SHADOW_CLASSIC);
    // X.15: the default scratch world is deleted first. A leftover one (an earlier run at another
    // height, with its lightbox and edits) was reopened as-is: --height was ignored and step 4 failed.
    if (g_opt.world.empty()) {
        for (int i = 0, n = eden_menu_world_count(); i < n; ++i)
            if (std::strcmp(eden_menu_world_name(i), name) == 0) { eden_menu_delete_at(i); break; }
    }
    if (!open_world(name, g_opt.height)) return 1;
    if (!g_opt.haveAt) { g_opt.at[0] = 65536.5f; g_opt.at[1] = 60.0f; g_opt.at[2] = 65536.5f; }
    const float X0 = g_opt.at[0], Y0 = g_opt.at[1], Z0 = g_opt.at[2];
    eden_console_teleport(X0, Y0, Z0);
    tick(g_opt.frames);

    auto field = [](const char* js, const char* key) -> long long {
        const char* k = std::strstr(js, key);
        return k ? atoll(k + std::strlen(key)) : -1;
    };
    auto str = [](const char* js, const char* key) {
        const char* k = std::strstr(js, key);
        if (!k) return std::string();
        k += std::strlen(key);
        const char* e = std::strchr(k, '"');
        return e ? std::string(k, e - k) : std::string();
    };
    auto settle = [&] {
        for (int i = 0; i < 1200 && field(eden_debug_shadow_state(), "\"pending\":") != 0; i++) tick(1);
        tick(120);
    };
    struct Shade { int classic = -1, corner[4] = {-1, -1, -1, -1}; float light[3] = {0}, lt[3] = {0}; };
    auto shade = [&](int x, int z, int y) {
        Shade s;
        const char* js = eden_debug_top_shade(x, z, y);
        s.classic = (int)field(js, "\"classic\":");
        std::sscanf(std::strstr(js, "\"corners\":[") ? std::strstr(js, "\"corners\":[") + 11 : "", "%d,%d,%d,%d",
                    &s.corner[0], &s.corner[1], &s.corner[2], &s.corner[3]);
        const char* l = std::strstr(js, "\"light\":[");
        if (l) std::sscanf(l + 9, "%f,%f,%f", &s.light[0], &s.light[1], &s.light[2]);
        const char* t = std::strstr(js, "\"light_top\":[");
        if (t) std::sscanf(t + 13, "%f,%f,%f", &s.lt[0], &s.lt[1], &s.lt[2]);
        return s;
    };
    int step = 0;
    auto state = [&](const char* what) {
        const std::string js = eden_debug_shadow_state();
        std::printf("[eden-shadow] step %d %-14s %s\n", step, what, js.c_str());
        std::fflush(stdout);
        return js;
    };
    char d[320];
    auto checkStale = [&](const std::string& js, const char* what) {
        std::snprintf(d, sizeof(d), "stale %lld, pending %lld", field(js.c_str(), "\"stale\":"),
                      field(js.c_str(), "\"pending\":"));
        check(field(js.c_str(), "\"stale\":") == 0 && field(js.c_str(), "\"pending\":") == 0, what, d);
    };

    // 0 baseline: the landing scan covers the whole window.
    settle();
    checkStale(state("baseline"), "step 0: every landed column's shadow top matches a fresh scan");

    // 1 site: a 5x5 floor on the top layer of its chunk, the 7x7 round it cleared to the sky, so the
    // only casters any floor face sees are the ones this test builds. Off the player's column.
    ++step;
    const PlayerState ps = player_state();
    const int H = g_world_height;
    const int x0 = (int)ps.x + 8, z0 = (int)ps.z + 8;
    int gy = ((H - 10) / CHUNK_SIZE) * CHUNK_SIZE - 1;   // gy%16==15, gy+8 < H
    for (int x = x0 - 3; x <= x0 + 3; ++x)
        for (int z = z0 - 3; z <= z0 + 3; ++z) {
            for (int y = H - 1; y > gy; --y)
                if (getLandc(x, z, y) != TYPE_NONE) eden_console_setblock(x, z, y, TYPE_NONE);
            if (std::abs(x - x0) <= 2 && std::abs(z - z0) <= 2) eden_console_setblock(x, z, gy, TYPE_STONE);
        }
    tick(10);
    const std::string H0 = str(eden_debug_chunk_mesh_hash(x0, z0, gy), "\"full\":\"");
    {
        const Shade c = shade(x0, z0, gy);
        std::snprintf(d, sizeof(d), "site (%d,%d,%d), centre shade %d, floor chunk full %s", x0, z0, gy, c.classic, H0.c_str());
        check(c.classic == 255 && !H0.empty(), "step 1: the cleared floor is unshaded", d);
        checkStale(state("site"), "step 1: stale == 0 after the clearing edits");
    }

    // 2 canopy: a 3x3 slab 4 above the floor -- in the next chunk up, and not its bottom layer.
    ++step;
    const int cy = gy + 4;
    for (int x = x0 - 1; x <= x0 + 1; ++x)
        for (int z = z0 - 1; z <= z0 + 1; ++z) eden_console_setblock(x, z, cy, TYPE_STONE);
    tick(10);
    {
        // §3.2's value, from the geometry rather than from Shadow.h: a 3x3 kernel column is covered
        // (gap 4: full weight) iff it is under the slab.
        auto want = [&](int x, int z) {
            int covered = 0;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                    if (std::abs(x + dx - x0) <= 1 && std::abs(z + dz - z0) <= 1) covered++;
            return 255 - (100 * covered * 16) / 144;
        };
        const int pts[4][2] = {{x0, z0}, {x0 + 2, z0}, {x0 + 2, z0 + 2}, {x0 + 3, z0}};
        bool ok = true;
        int n = 0;
        n += std::snprintf(d + n, sizeof(d) - n, "shade got/want:");
        for (auto& p : pts) {
            const int got = shade(p[0], p[1], gy).classic, w = want(p[0], p[1]);
            ok = ok && got == w;
            n += std::snprintf(d + n, sizeof(d) - n, " %d/%d", got, w);
        }
        check(ok, "step 2: centre, edge, corner and outside shades are the plan's values", d);
        const std::string h2 = str(eden_debug_chunk_mesh_hash(x0, z0, gy), "\"full\":\"");
        std::snprintf(d, sizeof(d), "floor chunk full %s -> %s", H0.c_str(), h2.c_str());
        check(!h2.empty() && h2 != H0, "step 2: the canopy re-meshed the floor chunk (the edit hook's dirtying)", d);
        checkStale(state("canopy"), "step 2: stale == 0");
    }

    // 2b nearest, not topmost: a roof of the same footprint 10 above the floor (6 over the canopy)
    // must change nothing -- the canopy is the nearest caster. A topmost-only scan saw the roof at
    // gap 10 (weight 14, not 16) and shaded every covered column lighter.
    {
        const int ry = gy + 10;
        for (int x = x0 - 1; x <= x0 + 1; ++x)
            for (int z = z0 - 1; z <= z0 + 1; ++z) eden_console_setblock(x, z, ry, TYPE_STONE);
        tick(10);
        const int want = 255 - (100 * 9 * 16) / 144;
        const int got = shade(x0, z0, gy).classic;
        std::snprintf(d, sizeof(d), "centre shade %d, want %d (canopy at gap 4, roof at gap 10)", got, want);
        check(got == want, "step 2: a roof above the canopy does not weaken its shade (nearest caster)", d);
        for (int x = x0 - 1; x <= x0 + 1; ++x)
            for (int z = z0 - 1; z <= z0 + 1; ++z) eden_console_setblock(x, z, ry, TYPE_NONE);
        tick(10);
        // And the other direction: the floor under only the roof (canopy gone) is shaded by it.
        for (int x = x0 - 1; x <= x0 + 1; ++x)
            for (int z = z0 - 1; z <= z0 + 1; ++z) eden_console_setblock(x, z, cy, TYPE_NONE);
        for (int x = x0 - 1; x <= x0 + 1; ++x)
            for (int z = z0 - 1; z <= z0 + 1; ++z) eden_console_setblock(x, z, ry, TYPE_STONE);
        tick(10);
        const int under = shade(x0, z0, gy).classic, wantRoof = 255 - (100 * 9 * (SHADOW_REACH_END - 10)) / 144;
        std::snprintf(d, sizeof(d), "centre shade %d, want %d (roof alone at gap 10)", under, wantRoof);
        check(under == wantRoof, "step 2: a lone roof at gap 10 shades with the faded weight", d);
        for (int x = x0 - 1; x <= x0 + 1; ++x)
            for (int z = z0 - 1; z <= z0 + 1; ++z) {
                eden_console_setblock(x, z, ry, TYPE_NONE);
                eden_console_setblock(x, z, cy, TYPE_STONE);
            }
        tick(10);
    }

    // 3 mine it out through the destroy path: the floor's mesh must be exactly what it was.
    ++step;
    for (int x = x0 - 1; x <= x0 + 1; ++x)
        for (int z = z0 - 1; z <= z0 + 1; ++z) eden_debug_light_edit(1, x, z, cy, 0);
    tick(10);
    {
        const std::string h3 = str(eden_debug_chunk_mesh_hash(x0, z0, gy), "\"full\":\"");
        std::snprintf(d, sizeof(d), "floor chunk full %s, before the canopy %s", h3.c_str(), H0.c_str());
        check(h3 == H0, "step 3: mining the canopy gives back the floor's exact mesh", d);
        checkStale(state("mine"), "step 3: stale == 0");
    }

    // 4 light: the canopy back, a lightbox 2 west of its edge. Day: never above 1.0. Night: the
    // box still lights the shaded floor beside it, and a shade never brightens a face.
    ++step;
    for (int x = x0 - 1; x <= x0 + 1; ++x)
        for (int z = z0 - 1; z <= z0 + 1; ++z) eden_console_setblock(x, z, cy, TYPE_STONE);
    eden_debug_light_edit(0, x0 - 3, z0, gy + 1, 0);
    for (int i = 0; i < 600 && field(eden_debug_light_state(), "\"pending\":") != 0; i++) tick(1);
    tick(10);
    {
        const Shade nd = shade(x0 - 1, z0, gy);
        std::snprintf(d, sizeof(d), "day light_top %.4f,%.4f,%.4f (shade %d)", nd.lt[0], nd.lt[1], nd.lt[2], nd.classic);
        check(nd.classic < 255 && nd.lt[0] <= 1.0f && nd.lt[1] <= 1.0f && nd.lt[2] <= 1.0f,
              "step 4: by day a shaded face beside a lightbox stays <= 1.0", d);
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) regionSkyColors[i][j] = 54;
    paintSky(54);
    memset(chunksToUpdate, true, sizeof(bool) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN);
    memset(columnsToUpdate, true, sizeof(bool) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
    tick(30);
    {
        const Shade near = shade(x0 - 1, z0, gy), far = shade(x0 + 1, z0, gy);
        std::snprintf(d, sizeof(d), "near shade %d lt %.4f light %.4f; far shade %d lt %.4f light %.4f",
                      near.classic, near.lt[0], near.light[0], far.classic, far.lt[0], far.light[0]);
        bool le = true;
        for (int c = 0; c < 3; ++c) le = le && near.lt[c] <= near.light[c] && far.lt[c] <= far.light[c];
        check(near.light[0] < 1.0f && near.classic < 255 && far.classic < 255 && near.lt[0] > far.lt[0] && le,
              "step 4: at night a lightbox lights a shaded face, and shading never brightens one", d);
        const std::string lOn = str(eden_debug_light_state(), "\"hash\":\"");
        const int mode = g_shadow_mode;
        shadow_setOverride(SHADOW_OFF);
        tick(10);
        const std::string lOff = str(eden_debug_light_state(), "\"hash\":\"");
        shadow_setOverride(mode);
        tick(10);
        std::snprintf(d, sizeof(d), "light hash on %s, off %s", lOn.c_str(), lOff.c_str());
        check(!lOn.empty() && lOn == lOff, "step 4: shadows do not touch the light store", d);
        checkStale(state("light"), "step 4: stale == 0");
    }

    // 5 walk: +x 160 then +z 160 at the F3 driver's 0.25 a frame (ring landings), against the
    // reference: every column re-scanned and every chunk re-meshed, the same window.
    ++step;
    const int bulk0 = g_bulk_reloads, ring0 = g_ring_columns_landed;
    float wx = X0, wz = Z0;
    auto walk = [&](float dx, float dz) {
        const int steps = (int)std::ceil(std::sqrt(dx * dx + dz * dz) / 0.25f);
        for (int k = 1; k <= steps; ++k) {
            const float f = (float)k / (float)steps;
            eden_console_teleport(wx + dx * f, Y0, wz + dz * f);
            tick(1);
        }
        wx += dx; wz += dz;
    };
    walk(160, 0);
    walk(0, 160);
    settle();
    const std::string sw = state("walked"), cw = eden_debug_mesh_checksum();
    std::printf("[eden-shadow] walked  mesh  %s\n", cw.c_str());
    std::snprintf(d, sizeof(d), "ring columns +%d, bulk reloads %d -> %d", g_ring_columns_landed - ring0, bulk0, g_bulk_reloads);
    check(g_ring_columns_landed > ring0 && g_bulk_reloads == bulk0, "step 5: the walk streams by the ring", d);
    checkStale(sw, "step 5: stale == 0 after the walk");
    shadow_markAllStale();
    memset(chunksToUpdate, true, sizeof(bool) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN);
    memset(columnsToUpdate, true, sizeof(bool) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
    settle();
    const std::string sr = state("reference"), cr = eden_debug_mesh_checksum();
    std::printf("[eden-shadow] reference mesh %s\n", cr.c_str());
    std::snprintf(d, sizeof(d), "shadow hash walked %s, reference %s", str(sw.c_str(), "\"hash\":\"").c_str(),
                  str(sr.c_str(), "\"hash\":\"").c_str());
    check(str(sw.c_str(), "\"hash\":\"") == str(sr.c_str(), "\"hash\":\""), "step 5: the walked tops equal a fresh scan", d);
    std::snprintf(d, sizeof(d), "walked geom %s full %s, reference geom %s full %s",
                  str(cw.c_str(), "\"geom\":\"").c_str(), str(cw.c_str(), "\"full\":\"").c_str(),
                  str(cr.c_str(), "\"geom\":\"").c_str(), str(cr.c_str(), "\"full\":\"").c_str());
    check(!str(cw.c_str(), "\"full\":\"").empty() && str(cw.c_str(), "\"geom\":\"") == str(cr.c_str(), "\"geom\":\"") &&
          str(cw.c_str(), "\"full\":\"") == str(cr.c_str(), "\"full\":\""),
          "step 5: the walked meshes equal a re-scan + re-mesh of the same window", d);

    // 6 toggle: Off -> Classic -> Smooth -> Off. Shadows never move geometry; Off is Off.
    ++step;
    {
        const int mode = g_shadow_mode;
        std::string geom[4], full[4];
        const int seq[4] = {SHADOW_OFF, SHADOW_CLASSIC, SHADOW_SMOOTH, SHADOW_OFF};
        for (int i = 0; i < 4; ++i) {
            shadow_setOverride(seq[i]);
            tick(10);
            const std::string c = eden_debug_mesh_checksum();
            geom[i] = str(c.c_str(), "\"geom\":\"");
            full[i] = str(c.c_str(), "\"full\":\"");
            std::printf("[eden-shadow] step %d mode %d mesh %s\n", step, seq[i], c.c_str());
        }
        shadow_setOverride(mode);
        std::snprintf(d, sizeof(d), "geom %s %s %s %s; full off %s classic %s smooth %s off %s",
                      geom[0].c_str(), geom[1].c_str(), geom[2].c_str(), geom[3].c_str(),
                      full[0].c_str(), full[1].c_str(), full[2].c_str(), full[3].c_str());
        check(!geom[0].empty() && geom[0] == geom[1] && geom[1] == geom[2] && geom[2] == geom[3],
              "step 6: geometry is the same in every mode", d);
        check(full[0] == full[3] && full[1] != full[0] && full[2] != full[1],
              "step 6: Off after == Off before, Classic and Smooth each differ", d);
    }

    std::printf("[eden-shadow] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --evict-selftest  (B.5b-1b: evict-save, node dropping, autosave)
// ---------------------------------------------------------------------------------------------
// Plan §3.4 gates 2, 3 and 6 (WORKING/render-distance-runtime-plan-2026-10-11.md). The ring stream
// overwrites a departing column's slot one column a frame; before it does, the occupant's edits
// must reach disk (evict-save, which no longer stops fire and water) and its burn/liquid nodes must
// go, or they simulate against the column that lands in the slot. Autosave is OFF for gates 2/3,
// so the evict-save is the only thing that can save the edits mid-walk.
int run_evict_selftest() {
    g_selftestTag = "eden-evict";
    Terrain::testNoEvictSave = g_opt.evictMutate == "nosave";
    Terrain::testNoDropDynamics = g_opt.evictMutate == "nodrop";
    const char* name = g_opt.world.empty() ? "evict-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    g_autosave_seconds = 0;
    // Engine time per tick fixed at 1/60 s, so frame counts below are engine seconds: a burn node
    // lives 6 s, spreads after 1 s; the autosave period is in engine seconds too.
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    // The bundled map's centre unless --at says otherwise (the default --at is the world origin,
    // outside the map, where a -x walk would reach negative chunk coordinates).
    const float X0 = g_opt.haveAt ? g_opt.at[0] : 65536.5f, Y0 = g_opt.haveAt ? g_opt.at[1] : 60.0f,
                Z0 = g_opt.haveAt ? g_opt.at[2] : 65536.5f;
    eden_console_teleport(X0, Y0, Z0);
    tick(g_opt.frames);
    Terrain* t = World::getWorld->terrain;
    const int H = g_world_height;
    char d[256];

    // FNV-1a over a resident column's blocks + colours; 0 if the column is not resident.
    auto colHash = [&](int cx, int cz) -> unsigned long long {
        if (!isColumnResident(cx * CHUNK_SIZE, cz * CHUNK_SIZE)) return 0;
        unsigned long long h = 1469598103934665603ULL;
        for (int cy = 0; cy < CHUNKS_PER_COLUMN; ++cy) {
            const TerrainChunk* c = t->chunkTable[threeToOne(cx, cy, cz)];
            const unsigned char* b[2] = {(const unsigned char*)c->pblocks, (const unsigned char*)c->pcolors};
            for (int k = 0; k < 2; ++k)
                for (int i = 0; i < CHUNK_SIZE3; ++i) { h ^= b[k][i]; h *= 1099511628211ULL; }
        }
        return h;
    };
    float wx = X0, wz = Z0;
    auto walk = [&](float dx, float dz) {   // 0.5 block a frame: a chunk crossing every 32 frames >= 18 columns
        const int steps = (int)std::ceil(std::sqrt(dx * dx + dz * dz) / 0.5f);
        for (int k = 1; k <= steps; ++k) {
            const float f = (float)k / (float)steps;
            eden_console_teleport(wx + dx * f, Y0, wz + dz * f);
            tick(1);
        }
        wx += dx; wz += dz;
    };
    auto storeKeys = [&]() {
        std::set<std::pair<int, int> > keys;
        emod::EdenWorldStore st;
        if (st.open(eden_debug_world_file(), false))
            for (const auto& k : st.columnKeys()) keys.insert(std::make_pair((int)k.first, (int)k.second));
        return keys;
    };
    const int pcx = (int)X0 / CHUNK_SIZE, pcz = (int)Z0 / CHUNK_SIZE;

    // ---- gate 2: edits survive eviction -------------------------------------------------------
    struct Col { int cx, cz; };
    const Col E[3] = {{pcx, pcz}, {pcx - 6, pcz}, {pcx + 4, pcz + 5}};
    unsigned long long eh[3];
    for (int i = 0; i < 3; ++i) {
        const int bx = E[i].cx * CHUNK_SIZE, bz = E[i].cz * CHUNK_SIZE;
        for (int k = 0; k < 6; ++k) t->updateChunks(bx + 2 + k, bz + 3 + 2 * k, H - 4 - 3 * k, TYPE_STONE);
        t->updateChunks(bx + 9, bz + 9, 2, TYPE_NONE);
        t->setColor(bx + 2, bz + 3, H - 4, 12);
        eh[i] = colHash(E[i].cx, E[i].cz);
    }
    tick(10);
    const int saves0 = g_ring_evict_saves, bulk0 = g_bulk_reloads, ring0 = g_ring_columns_landed;
    // B.5b-2: far enough that the window's trailing edge passes the +4 column at any radius (320 at 9..14).
    const float W = (float)std::max(320, CHUNK_SIZE * (T_RADIUS + 6));
    walk(W, 0);
    tick(200);
    bool gone = true;
    for (const Col& c : E) gone = gone && !isColumnResident(c.cx * CHUNK_SIZE, c.cz * CHUNK_SIZE);
    std::snprintf(d, sizeof(d), "evict-saves %d, ring columns +%d, bulk reloads %d->%d", g_ring_evict_saves - saves0,
                  g_ring_columns_landed - ring0, bulk0, g_bulk_reloads);
    check(gone && g_bulk_reloads == bulk0 && g_ring_columns_landed > ring0, "a long walk streams the edited columns out", d);
    walk(-W, 0);
    tick(g_opt.frames);
    int same = 0;
    for (int i = 0; i < 3; ++i) same += colHash(E[i].cx, E[i].cz) == eh[i];
    std::snprintf(d, sizeof(d), "%d/3 columns equal their pre-walk bytes; evict-saves %d", same, g_ring_evict_saves - saves0);
    check(same == 3, "gate 2: edits survive eviction and stream back", d);

    // ---- gate 3: dynamics never cross into a neighbour column ---------------------------------
    // D is the window's -x outer-ring column on the player's z chunk: one chunk of +x walking
    // evicts it, and its slot's new occupant (D + 18 chunks, the player's z) is the nearest stale
    // column, so the ring lands it in the first frame after the crossing -- well inside the 1 s a
    // burn node takes to spread. A wood tower through the whole column height, lit at three
    // heights, and a water source on a stone slab, all at least 4 blocks inside D.
    const std::set<std::pair<int, int> > keys0 = storeKeys();
    const int dcx = windowOriginChunk(wx), dcz = (int)wz / CHUNK_SIZE;
    const int tx = dcx * CHUNK_SIZE + 8, tz = dcz * CHUNK_SIZE + 10;
    std::map<std::pair<int, int>, unsigned long long> w0;
    for (int cx = dcx; cx < dcx + CHUNKS_PER_SIDE; ++cx)
        for (int cz = windowOriginChunk(wz); cz < windowOriginChunk(wz) + CHUNKS_PER_SIDE; ++cz)
            if (cx != dcx || cz != dcz) w0[std::make_pair(cx, cz)] = colHash(cx, cz);
    for (int y = 1; y < H - 1; ++y) t->updateChunks(tx, tz, y, TYPE_WOOD);
    for (int x = tx - 3; x <= tx + 3; ++x)
        for (int z = tz - 9; z <= tz - 5; ++z) t->updateChunks(x, z, H - 10, TYPE_STONE);
    t->updateChunks(tx, tz - 7, H - 9, TYPE_WATER);
    t->liquids->addSource(tx, tz - 7, H - 9);
    for (int y : {1, H / 4, H / 2}) t->burnBlock(tx, tz, y, false);
    tick(2);
    const int burning = t->nburn, dropped0 = g_ring_dynamics_dropped;
    walk(16, 0);
    int outside = 0;
    for (int i = 0; i < 600; ++i) { tick(1); outside = std::max(outside, t->dynamicsOutsideWindow()); }
    const bool dGone = !isColumnResident(dcx * CHUNK_SIZE, dcz * CHUNK_SIZE);
    std::snprintf(d, sizeof(d), "burning %d before, column evicted %d, nodes dropped %d, max outside the window %d",
                  burning, dGone, g_ring_dynamics_dropped - dropped0, outside);
    check(burning >= 3 && dGone && outside == 0, "gate 3: no burn/liquid node outlives its column", d);
    walk(-16, 0);
    tick(g_opt.frames);
    World::getWorld->fm->saveWorld();
    const std::set<std::pair<int, int> > keys1 = storeKeys();
    std::string extra;
    bool dSaved = false;
    for (const auto& k : keys1) {
        if (keys0.count(k)) continue;
        if (k.first == dcx && k.second == dcz) { dSaved = true; continue; }
        extra += " (" + std::to_string(k.first) + "," + std::to_string(k.second) + ")";
    }
    int changed = 0;
    for (const auto& e : w0) changed += colHash(e.first.first, e.first.second) != e.second;
    std::snprintf(d, sizeof(d), "own column committed %d; other new columns:%s; window columns changed %d of %d",
                  dSaved, extra.empty() ? " none" : extra.c_str(), changed, (int)w0.size());
    check(dSaved && extra.empty() && changed == 0, "gate 3: nothing outside the dynamics' own column changed", d);

    // ---- the autosave timer: saves pending edits and leaves the fire burning ------------------
    {
        // The edit in E[0]; the fire in a column of its own, so E[0]'s bytes stay put afterwards.
        const int ax = pcx * CHUNK_SIZE + 5, az = pcz * CHUNK_SIZE + 5;
        const int fx = (pcx + 2) * CHUNK_SIZE + 8, fz = (pcz - 3) * CHUNK_SIZE + 8;
        for (int y = 1; y < H - 1; ++y) t->updateChunks(fx, fz, y, TYPE_WOOD);
        t->burnBlock(fx, fz, H / 2, false);
        t->updateChunks(ax, az, H - 3, TYPE_STONE);
        const int a0 = g_autosaves;
        auto fileBytes = [] { SDL_PathInfo pi; return SDL_GetPathInfo(eden_debug_world_file(), &pi) ? (long long)pi.size : -1LL; };
        const long long f0 = fileBytes();
        g_autosave_seconds = 2;
        tick(150);
        // What one autosave writes: an append of the dirty columns (here 2: the edit and the
        // fire's), the creature record and the header -- never the whole file.
        std::printf("[eden-evict] autosave appended %lld bytes to a %lld-byte world\n", fileBytes() - f0, f0);
        bool clean = true;
        for (int cy = 0; cy < CHUNKS_PER_COLUMN; ++cy) clean = clean && !t->chunkTable[threeToOne(pcx, cy, pcz)]->modified;
        std::snprintf(d, sizeof(d), "autosaves %d, column clean %d, still burning %d", g_autosaves - a0, clean, t->nburn);
        check(g_autosaves - a0 == 1 && clean && t->nburn > 0, "autosave saves the edit, fire keeps burning", d);
        g_autosave_seconds = 0;
        for (int i = 0; i < 3; ++i) eh[i] = colHash(E[i].cx, E[i].cz);   // the edit above is E[0]'s now
    }

    // ---- gate 2, the round trip: quit, reload, the same bytes ---------------------------------
    if (!quit_to_menu() || !open_world(name, g_opt.height)) return 1;
    g_autosave_seconds = 0;
    eden_console_teleport(X0, Y0, Z0);
    tick(g_opt.frames);
    same = 0;
    for (int i = 0; i < 3; ++i) same += colHash(E[i].cx, E[i].cz) == eh[i];
    std::snprintf(d, sizeof(d), "%d/3 columns equal after quit + reload", same);
    check(same == 3, "gate 2: edits survive a save and reload", d);
    g_autosave_seconds = 60;
    g_app->viewController.setFixedEtime(0.0f);

    std::printf("[eden-evict] %s (%d failure(s))%s\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures,
                g_opt.evictMutate.empty() ? "" : (" [mutation " + g_opt.evictMutate + "]").c_str());
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_light
}  // namespace harness
