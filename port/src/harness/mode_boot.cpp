// G.4: split out of the old eden_main_native.cpp (see harness.h). Mode group "boot".
#include "harness.h"

using namespace harness;

namespace harness {
namespace g_boot {

int run_p1_gate() {
    // Exactly what web-port-final:web/tools/headless-p1-gate.js asserts: the headless-no-context line, then three
    // "[eden-p1] tick N: World::update returned" lines. Both come from code shared with, or
    // line-for-line mirrored from, the web build — see EdenViewController_native.cpp.
    tick(3);
    return 0;
}

// --light-lattice=STEP (B.5b-1c): the ring-light gate's world. The bundled map has no lightboxes,
// so --stage1's `.light` hash is all zeros whatever the ring does; this puts lightboxes everywhere a
// 160-block walk's window can reach (--at +-320), at every in-chunk offset (STEP coprime to 16, so
// some sit within LIGHT_RADIUS of every chunk border and so of every window edge the walks cross),
// half high in the air and half on the ground, in varied paint. Placed through buildBlock (the
// player's own path) from 9 teleport stops 200 blocks apart, then saved, so a walk run and a fresh
// run each load the same file. The whole map's sky is made night (palette 54) and saved that way:
// by day the mesher never reads the light at all (TerrainChunk's skylight==1.0 path), so only at
// night does a mesh baked from stale light show up in the geometry checksum.
int place_light_lattice(int S, const char* tag) {
    const int R = 320, H = g_world_height;
    const int x0 = (int)g_opt.at[0] - R, z0 = (int)g_opt.at[2] - R, n = 2 * R / S + 1;
    std::set<std::pair<int, int> > done;
    int placed = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) regionSkyColors[i][j] = 54;
    paintSky(54);
    for (int sx = -200; sx <= 200; sx += 200)
        for (int sz = -200; sz <= 200; sz += 200) {
            eden_console_teleport(g_opt.at[0] + sx, g_opt.at[1], g_opt.at[2] + sz);
            tick(g_opt.frames / 4);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const int x = x0 + i * S, z = z0 + j * S;
                    if (done.count(std::make_pair(i, j)) || !isColumnResident(x, z)) continue;
                    int y = H - 6 + (i * 3 + j) % 4;
                    if ((i + j) & 1) {   // on the ground: the first air above the topmost block
                        int top = H - 1;
                        while (top >= 0 && getLandc(x, z, top) == TYPE_NONE) --top;
                        if (top + 1 >= H) continue;
                        y = top + 1;
                    }
                    eden_debug_light_edit(0, x, z, y, (i * 5 + j * 11) % 54);
                    done.insert(std::make_pair(i, j));
                    if (getLandc(x, z, y) == TYPE_LIGHTBOX) ++placed;
                }
            tick(10);
        }
    std::printf("[%s] light-lattice placed %d of %d lightboxes (step %d)\n", tag, placed, n * n, S);
    std::fflush(stdout);
    return placed;
}
int light_lattice_prep() {
    const int placed = place_light_lattice(g_opt.lightLattice, "eden-stage1");
    save_world();
    if (!quit_to_menu()) return 1;
    return placed > 0 ? 0 : 1;
}

// --mesh-bench=PASSES (B.5b-2): the hot-path cost of the window's toroidal index, the 2026-08-06
// stride method moved from a whole load to the two loops that are nothing BUT that index. Pass A
// re-meshes every chunk that holds geometry, synchronously on this thread (rebuild2 + prepareVBO;
// the GL calls no-op headless); pass B reads every voxel of the window through GBLOCK by WORLD
// coordinate, which is the raw `%T_SIZE` cost and nothing else. Median of PASSES of each, so a
// runtime-radius build can be compared with the literal-288 one on the same machine.

void mesh_bench(const char* phase) {
    if (g_meshBenchPasses <= 0) return;
    Terrain* t = World::getWorld->terrain;
    const int n = CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN;
    std::vector<TerrainChunk*> meshed;
    for (int i = 0; i < n; ++i) {
        TerrainChunk* c = t->chunkTable[i];
        if (c && (c->rtn_vertices > 0 || c->rtn_vertices2 > 0)) meshed.push_back(c);
    }
    const int x0 = World::getWorld->fm->chunkOffsetX * CHUNK_SIZE, z0 = World::getWorld->fm->chunkOffsetZ * CHUNK_SIZE;
    // Pass C first, on the settled window: whole frames (World::update + renderFrame; the GL calls
    // no-op headless, the CPU loops over the window -- renderTree x2, the dirty-list scan, the
    // upload scan, the outer-ring sweep -- all run). Plan §2 group G: the cost a bigger radius adds
    // to EVERY frame, which a re-mesh pass does not see.
    std::vector<double> frameMs;
    for (int k = 0; k < 300; ++k) {
        Uint64 a = SDL_GetPerformanceCounter();
        g_app->viewController.drawFrame(true);
        frameMs.push_back((double)(SDL_GetPerformanceCounter() - a) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    }
    std::sort(frameMs.begin(), frameMs.end());
    std::vector<double> meshMs, readMs;
    unsigned long long sink = 0;
    for (int p = 0; p < g_meshBenchPasses; ++p) {
        Uint64 a = SDL_GetPerformanceCounter();
        for (TerrainChunk* c : meshed) { c->rebuild2(); c->prepareVBO(); }
        Uint64 b = SDL_GetPerformanceCounter();
        for (int x = x0; x < x0 + T_SIZE; ++x)
            for (int z = z0; z < z0 + T_SIZE; ++z)
                for (int y = 0; y < T_HEIGHT; ++y) sink += (unsigned char)GBLOCK(x, z, y);
        Uint64 c = SDL_GetPerformanceCounter();
        const double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
        meshMs.push_back((double)(b - a) * f);
        readMs.push_back((double)(c - b) * f);
    }
    std::sort(meshMs.begin(), meshMs.end());
    std::sort(readMs.begin(), readMs.end());
    std::printf("[eden-stage1] %s.bench {\"t_size\":%d,\"passes\":%d,\"chunks\":%d,\"mesh_ms\":%.2f,\"mesh_min_ms\":%.2f,"
                "\"read_ms\":%.2f,\"read_min_ms\":%.2f,\"frame_p50_ms\":%.3f,\"frame_p90_ms\":%.3f,"
                "\"frame_max_ms\":%.3f,\"sink\":%llu}\n",
                phase, T_SIZE, g_meshBenchPasses, (int)meshed.size(), meshMs[meshMs.size() / 2], meshMs[0],
                readMs[readMs.size() / 2], readMs[0], frameMs[150], frameMs[270], frameMs[299], sink);
    std::fflush(stdout);
}

// P.1b: per-pass samples over a run (Classes/PassTimers.h). add() after every tick(1) whose frame
// should count; print() emits one `[<tag>] <label>.passes {...}` JSON line: the whole-tick frame
// p50/p90, and per pass p50/p90/max/mean ms plus the median draws / vertices / issued setup calls the
// GL shim counted inside it. Parents include their children (docs/rendering.md, "Per-pass instruments").

// P.1a (= X.10): frame-time summary of a scripted windowed run. Each sample is one whole tick() --
// event pump, World::update + render, present (vsync blocks in it), audio -- minus nothing, so it is
// what a player's frame costs on this machine. Headless it measures the CPU side only. Printed as a
// `[eden-stage1] <phase>.frames {...}` JSON line; `thermal` is NSProcessInfo.thermalState at the
// start (0 nominal..3 critical, -1 unknown) and `battery_pct`/`on_battery` come from SDL_GetPowerInfo.
static void frame_summary(const char* phase, std::vector<double>& ms, int thermal0, int batt0, int onBatt0) {
    if (ms.empty()) return;
    std::sort(ms.begin(), ms.end());
    const size_t n = ms.size();
    auto pct = [&](double q) { return ms[std::min(n - 1, (size_t)(q * (double)n))]; };
    size_t over16 = 0, over33 = 0;
    for (double v : ms) { if (v > 16.7) ++over16; if (v > 33.3) ++over33; }
    std::printf("[eden-stage1] %s.frames {\"frames\":%d,\"p50_ms\":%.2f,\"p90_ms\":%.2f,\"p99_ms\":%.2f,"
                "\"worst_ms\":%.2f,\"pct_over_16_7\":%.1f,\"pct_over_33_3\":%.1f,\"thermal_start\":%d,"
                "\"thermal_end\":%d,\"battery_pct_start\":%d,\"on_battery\":%d}\n",
                phase, (int)n, pct(0.50), pct(0.90), pct(0.99), ms[n - 1],
                100.0 * (double)over16 / (double)n, 100.0 * (double)over33 / (double)n,
                thermal0, eden_native_thermal_state(), batt0, onBatt0);
    std::fflush(stdout);
}

int run_stage1() {
    const char* name = g_opt.world.empty() ? "stage1" : g_opt.world.c_str();

    // Phase 1 — menu. Settle first: Resources::loadResources runs during World construction and
    // the menu's own art loads over the first frames, so a sample taken immediately reports a
    // process that has not finished starting.
    tick(g_opt.frames / 4);
    std::printf("[eden-stage1] menu.mem %s\n", eden_debug_heap());
    eden_mem_trace("menu");
    std::fflush(stdout);

    // Phase 2 — a world.
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.lightLattice > 0) return light_lattice_prep();
    // Teleport to the FIXED origin before settling (see --at in the header comment for why a
    // fixed one is required for the checksum to mean anything).
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    report(g_opt.height == 256 ? "world256" : "world64");
    mesh_bench(g_opt.height == 256 ? "world256" : "world64");

    // B.5b-1a: a straight walk, the way the F3 driver walks (position set each frame, y pinned, no
    // velocity), so the window streams column by column instead of reloading. The checksums after
    // it must equal a fresh --stage1 at the printed end position.
    if (g_opt.walkLegs > 0) {
        float x = g_opt.at[0], z = g_opt.at[2];
        std::vector<double> walkMs;
        int pctBatt = -1;
        const bool onBatt = SDL_GetPowerInfo(nullptr, &pctBatt) == SDL_POWERSTATE_ON_BATTERY;
        const int thermal0 = eden_native_thermal_state();
        PassAgg walkPasses;
        g_pass.on = g_opt.passTimers; g_pass.finish = g_opt.passTimers && g_opt.passFinish;
        for (int leg = 0; leg < g_opt.walkLegs; ++leg) {
            const float dx = g_opt.walk[leg * 2], dz = g_opt.walk[leg * 2 + 1];
            const int steps = (int)std::ceil(std::sqrt(dx * dx + dz * dz) / 0.25f);
            for (int k = 1; k <= steps; ++k) {
                const float f = (float)k / (float)steps;
                eden_console_teleport(x + dx * f, g_opt.at[1], z + dz * f);
                pass_frame_reset();
                const Uint64 t0 = SDL_GetPerformanceCounter();
                tick(1);
                walkMs.push_back((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
                if (g_pass.on) walkPasses.add(walkMs.back());
            }
            x += dx; z += dz;
        }
        frame_summary("walk", walkMs, thermal0, pctBatt, onBatt ? 1 : 0);
        if (g_pass.on) walkPasses.print("eden-stage1", "walk", g_opt.headless ? 0 : 1, thermal0, pctBatt);
        g_pass.on = g_pass.finish = false;
        tick(g_opt.frames);
        const Vector p = World::getWorld->player->pos;
        std::printf("[eden-stage1] walked.end %.2f,%.2f,%.2f\n", p.x, p.y, p.z);
        report("walked");
    }

    // Phase 3 — back to the menu, so the "peak" figures cover a full load/unload cycle the way
    // headless-heap-ceiling-probe.js's do.
    if (!quit_to_menu()) return 1;
    tick(g_opt.frames / 4);
    std::printf("[eden-stage1] menu2.mem %s\n", eden_debug_heap());
    eden_mem_trace("menu2");
    std::fflush(stdout);
    return 0;
}

// The window pass. Everything Stage 1 measures is headless, so this exists purely to answer
// "does the desktop-GL backend actually draw?" — which the checksums cannot, because they are
// computed CPU-side before the vertices ever reach a buffer.
//
// The signal is eden_gl_stat(): 0 = draw calls in the last completed frame, 1 = setup calls
// issued, 2 = calls elided by the shim's dirty caches. A frame that drew nothing reports 0 draws
// while everything else looks healthy, which is exactly what a missing VAO or a shader that failed
// to compile produces on macOS core profile — no GL error, no exception, a black window.
int run_smoke() {
    const char* name = g_opt.world.empty() ? "stage1-smoke" : g_opt.world.c_str();
    std::printf("[eden-smoke] GL_VERSION  %s\n", R::driverString(R::DS_VERSION));
    std::printf("[eden-smoke] GL_RENDERER %s\n", R::driverString(R::DS_RENDERER));
    std::fflush(stdout);

    tick(120);
    std::printf("[eden-smoke] menu frame: draws=%d issued=%d elided=%d\n",
                eden_gl_stat(0), eden_gl_stat(1), eden_gl_stat(2));
    std::fflush(stdout);

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);

    const int draws = eden_gl_stat(0);
    std::printf("[eden-smoke] world frame: draws=%d issued=%d elided=%d\n",
                draws, eden_gl_stat(1), eden_gl_stat(2));

    // Render one more frame WITHOUT presenting, then read the back buffer. This is the difference
    // between "the shim issued 132 draw calls" and "132 draw calls put colour on the screen" —
    // and a shader-dialect mistake lands squarely between the two.
    {
        int w = 0, h = 0;
        eden_gl_context_get_drawable_size(&w, &h);
        const int side = 64;
        if (w >= side && h >= side) {
            eden_native_gl_set_present(0);
            tick(1);
            std::vector<unsigned char> px((size_t)side * side * 4);
            R::readPixels((w - side) / 2, (h - side) / 2, side, side, px.data());
            eden_native_gl_set_present(1);
            long long sum = 0; int nonBlack = 0;
            for (size_t i = 0; i < px.size(); i += 4) {
                const int lum = px[i] + px[i + 1] + px[i + 2];
                sum += lum;
                if (lum > 12) nonBlack++;   // >4/255 per channel on average: not a clear-colour pixel
            }
            const int total = side * side;
            std::printf("[eden-smoke] centre %dx%d readback: %d/%d non-black, mean luma %.1f\n",
                        side, side, nonBlack, total, (double)sum / (total * 3.0));
            if (nonBlack * 4 < total) {
                std::fprintf(stderr, "[eden-smoke] FAIL: the frame drew, but the pixels are (nearly) "
                                     "all background — suspect the shaders or the texture path.\n");
                return 1;
            }
        }
    }
    std::printf("[eden-smoke] %s\n", eden_debug_mesh_checksum());
    std::fflush(stdout);
    // A world frame that issues no draw calls is the failure this mode exists to catch, so say so
    // in the exit code rather than leaving it to be read out of the log.
    if (draws <= 0) {
        std::fprintf(stderr, "[eden-smoke] FAIL: the world frame issued no draw calls.\n");
        return 1;
    }
    // The readback above cannot see a frame that was drawn but never PRESENTED (N.4.12: iOS at a
    // non-100% render scale), so the present guard's count is the check for that.
    if (eden_native_gl_present_guard_fires() > 0) {
        std::fprintf(stderr, "[eden-smoke] FAIL: %d present(s) found the wrong renderbuffer bound.\n",
                     eden_native_gl_present_guard_fires());
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// --shot: frame captures of the real window
// ---------------------------------------------------------------------------------------------
// The GL UI is native's ONLY UI until Stage 5, so "does the interface look right" is now a
// first-class question — and the first play pass answered it with "broken looking and messy",
// which is not something a checksum or a draw-call count can localise. --smoke already proves the
// backend draws; this proves WHAT it draws, by writing the back buffer to a file a human (or the
// next session) can look at without sitting at the keyboard.
//
// BMP rather than PNG because it needs no encoder: this tree vendors stb_image (decode) but not
// stb_image_write, and 24-bit BMP is a 54-byte header plus bottom-up BGR rows — which is also
// exactly the order glReadPixels hands them back, so there is no flip. macOS Preview and
// `sips -s format png` both read it.

}  // namespace g_boot
}  // namespace harness
