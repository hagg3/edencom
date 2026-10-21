// eden_main_native.cpp — the native `int main()` (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md). Twin of web-port-final:web/src/entry/eden_main.cpp; that file
// registers an emscripten_set_main_loop callback and returns, this one owns a real while-loop.
//
// It also carries Stage 1's HARNESS, because Stage 1's deliverable is "two numbers and one diff,
// then stop" and there is no `node eden.js` here to script the module from the outside. The
// scripted modes below drive the same engine exports the web harnesses drive
// (eden_menu_create_world / eden_menu_play / eden_tap_hud_button_* / eden_debug_*), so the two
// sides are running the same session and their outputs are directly comparable — which is the
// whole point of the criteria.
//
//   --headless           no window; every GL call no-ops behind the shim's context guard, exactly
//                        as `node eden.js` does. Geometry, memory and save round-trips are all
//                        taken this way, so Stage 1 needs no display.
//   --p1-gate            criterion 1: boot, three ticks, print the same three `[eden-p1]` lines
//                        web-port-final:web/tools/headless-p1-gate.js asserts on, exit.
//   --stage1             criteria 2+3: menu phase, then create-or-load a world, teleport to a
//                        fixed origin, settle, and print geometry + mesh checksum + memory per
//                        phase as `[eden-stage1] <phase> {json}` lines.
//   --smoke              opens a REAL WINDOW, loads a world, runs --frames frames and reports the
//                        GL version, the shim's per-frame draw accounting and the geometry
//                        checksum, then exits. This is the plan's highest-risk item made
//                        checkable: macOS core profile refuses to draw without a bound VAO (the
//                        shim deliberately has no VAO concept), and the four ES-dialect shaders
//                        have to compile as desktop GLSL. Both fail as a black window and no
//                        error, so "did it draw" has to be a number.
//   --input-selftest     Stage 2: pushes synthetic SDL key/mouse events through the real
//                        translator (src/seam/Input_native.cpp) and asserts the ENGINE moved —
//                        the player's position, yaw and HUD mode, read back through the same
//                        eden_debug_* probes the web harnesses use. It runs headless, because the
//                        input path needs no GL; what it cannot cover is feel, which is a human's
//                        job. Exists because "input is wired up" is otherwise only checkable by
//                        someone sitting at the keyboard, and that is not a regression test.
//   --shot[=PREFIX]      opens a REAL WINDOW and writes BMP captures of three screens — the main
//                        menu, the in-world HUD and the in-game (ESC) panel — as
//                        PREFIX-menu.bmp / -kit.bmp (U.1: every GL kit control, every state) /
//                        -settings.bmp (+ -settings-pN.bmp per page) /
//                        -keybinds.bmp / -hud.bmp / -pausemenu.bmp / -dialog.bmp /
//                        -menu-world.bmp / -rename.bmp (default prefix eden-shot), -hud-sign.bmp / -sign-prompt.bmp
//                        (H.15: -props-world-nophoto.bmp / -props-world.bmp / -props-terrain.bmp, World
//                        Properties after an H.16 photo; --classic=SEED makes the Terrain tab the Classic rows)
//                        (D.3c, where GL text entry exists), and since 5.9
//                        -browser.bmp / -browser-current.bmp (Get Worlds; live network unless
//                        --net-fixtures is given).
//                        The GL UI is native's only UI until Stage 5, so "what does it look like"
//                        needs an artefact; --smoke only answers "did it draw at all".
//   --keybind-selftest   Stage 5.2/5.3: the keybind MODEL and the rebind path. Asserts the
//                        defaults resolve, that rebinding a key really moves the behaviour from
//                        the old physical key to the new one (both directions checked, which is
//                        the half a "does the setter store it" test would miss), that a default
//                        double-binding still fires both of its actions, and that reset restores.
//                        Headless.
//   --ui-selftest        Stage 5.4 + N.4.5: the GL settings widgets (toggle tap, slider drag,
//                        enum stepper) through real pointer events at the rects the screen
//                        reports; GLW::TextField through pushed SDL text/key events (UTF-8
//                        backspace, the byte cap, Return, Escape); and the world rename, checked
//                        on disk — only WorldFileHeader::name may change; the N.5.11 toast's
//                        lifetime and statusbar routing. Headless.
//   --browser-selftest   ROADMAP 5.9: Get Worlds (Classes/WorldBrowser.mm) end to end, OFFLINE —
//                        it writes fixtures under <docs>/.net-fixtures and points the net seam at
//                        them, then drives the menu button, the source tabs, the archive filter,
//                        the servers' Featured/Recent/More/Search, five downloads (deflate zip,
//                        zip-in-zip, no-overwrite, two-member gzip, an HTML page that must be
//                        refused), the unreachable-server state, and plays a downloaded world.
//                        Headless; cleans up after itself.
//   --net-live-selftest  the same screen against the REAL archive and edengame.net servers: every
//                        source's lists and a preview, and the smallest archive world downloaded
//                        and loaded. Not a gate (it depends on three hosts being up).
//   --net-fixtures=DIR   answer every eden_net_fetch from DIR/<host>/<path>@<query> instead of the
//                        network (also the EDEN_NET_FIXTURES environment variable).
//   --objc-selftest      Stage 3: this port's own ObjC runtime (Linux/Windows only; macOS uses
//                        Apple's). Dispatch, ivar layout, super, categories, the empty-base ivar
//                        bias and the @"literal" layout.
//   --audio-selftest     Stage 2: decodes one real file of each shipped format and asserts a
//                        played effect reaches the mixer, drains, and that a music channel
//                        streams. Headless (no window needed) but it DOES open the audio device
//                        and make noise — that is the point.
//   --touch-selftest     Stage 4.3: pushes SDL finger events at the engine's OWN reported HUD rect
//                        and asserts the tap reaches Hud::inmenu. The only test of the touch path
//                        outside a browser.
//   --gamepad-selftest   Stage 2: attaches an SDL VIRTUAL gamepad, drives its sticks/buttons, and
//                        asserts the engine moved — so it needs no physical controller and runs
//                        headless. Native counterpart of web-port-final:web/tools/headless-gamepad-test.js.
//   --background-selftest Stage 4.2: pushes SDL_EVENT_WILL_ENTER_BACKGROUND and asserts the event
//                        WATCH (not the poll loop -- see eden_lifecycle_event_watch) reaches
//                        eden_app_will_background() and a real world file lands on disk.
//   --save-roundtrip     criterion 4: create a world, edit a block, save, quit, reload, and print
//                        the geometry checksum on both sides plus the saved file's path so the
//                        bytes can be diffed against a web-written save.
//   --leak-probe[=N]     Stage 3 verification: N (default 8) load->quit cycles of one world in one
//                        process, asserting the allocator's live-bytes figure is FLAT. The native
//                        counterpart of web-port-final:web/tools/headless-alloc-leak-probe.js and of
//                        headless-heap-ceiling-probe.js --torture=same64, which cannot run here
//                        because they drive `eden.js` through node. It matters on the non-Apple
//                        legs specifically: the shim Foundation runs outside Emscripten there for
//                        the first time, and this runtime frees objects without .cxx_destruct.
//   --height=64|256      world height for a newly created world (default 64).
//   --convert-prompt-selftest  Stage S / S.5e: Play a 64z `.eden` (--world=NAME, in --docs, with
//                        --world-format=emod) and answer the "Convert world" prompt with
//                        --convert-answer=upgrade|keep|cancel through its real buttons.
//   --upgrade-256z=0|1   Stage S / S.5e: whether a conversion promotes a 64z source to 256z
//                        (EDEN_UPGRADE_256Z). Scripted modes and --live-cmds default to 0 -- the
//                        source's height -- whatever the Settings toggle says; a player run follows it.
//   --radius=N           B.5b-2: the resident window's radius in chunks, 4..16 (EDEN_WINDOW_RADIUS). Scripted
//                        modes and --live-cmds default to 9 -- stock -- whatever the Render distance setting
//                        says, so every checksum gate keeps meaning the same window; a player run follows it.
//   --mesh-bench=PASSES  B.5b-2: --stage1 also times PASSES synchronous re-meshes of every meshed chunk and
//                        PASSES reads of every window voxel through GBLOCK (`.bench` line, medians): the
//                        hot-path cost of the runtime `%T_SIZE`, A/B'd against a literal build.
//   --world-format=...    Stage S / S.8: accepted and ignored (old scripts pass it). Every world is an
//                        `.emod`; a `.eden`/archive converts when played (S.5d).
//   --empty-shortcut=0   Stage R / R.2's A/B lever: 0 restores the pre-R.2 mesh scheduling (all-air
//                        chunks scanned and counted against the bulk-reload budget). Default 1.
//   --read-budget-bands=0  Stage R / R.2b's A/B lever: 0 restores the stock column-read budget
//                        (BULK_RELOAD_CHUNK_BUDGET/CHUNKS_PER_COLUMN columns a frame) instead of
//                        counting a landed column by its occupied bands. Default 1.
//   --empty-bit-selftest R.2: places a block in an all-air chunk and removes it again, asserting the
//                        empty bit follows both transitions and the block really meshes.
//   --newblocks-selftest D.2a: builds each of ids 112-127 (the 2026 blocks) through buildBlock,
//                        asserts each meshes, then saves, reloads and reads every id back.
//   --photo-selftest     H.16: the in-game photo's PNG encoder (Util.mm photoEncodePNG): signature, IHDR
//                        size/type, IDAT inflates to the exact filtered rows, chunk CRCs. Headless.
//   --trailer-selftest   D.3a: the sign/command-block trailer model (Classes/WorldTrailer.h). Always
//                        the synthetic legs (ordering, opaque, the 1 MiB refusal, sidecars, the index);
//                        each --trailer-file=W.eden|W.emod must parse and re-serialise byte-identical;
//                        with --world=NAME (a world with signs, already in --docs) it edits one sign's
//                        text, saves with zero dirty columns, reloads and checks the bytes on disk, then
//                        removes every record (no trailer left; --trailer-keep stops before that, leaving
//                        the edited world for `emod.py trailer --diff`). --trailer-no-dirty-mark is the
//                        mutation (edits stop marking the trailer dirty: the save gate must fail).
//   --signs-selftest     D.3b + D.3c: with --world=NAME (a world with signs, in --docs) and --at near
//                        them: the sign pass's probe (in range / drawn / hidden), then the SIGN tool
//                        on a floating anchor — four sides and the top placed through the real prompt
//                        (typed text, Return), the bottom refused with its toast, an edit, an empty
//                        edit deleting, the paint colour, a block in front hiding (not deleting) a
//                        sign; the picker's SIGN cell arming the tool and a real screen tap placing
//                        one; mining and TNT taking the anchor's signs; a command block's steel mined
//                        taking only its CMB1 record (+ its signs); save + reload. Either container
//                        (--world-format=emod). --signs-mutate=bottom|nohook are the mutation legs.
//   --camera-selftest    H.12: the third-person camera. Picking at screen centre hits the same block
//                        in all three views (13 ahead: inside reach only if the third-person ray starts
//                        at the player), a block behind the player neither picks nor blocks, pull-in in
//                        front of a wall and back out, the frustum is the drawn view's, F5 cycles the
//                        camera_view setting, the body slot is posed but never picked or counted.
//   --cmds-selftest      D.4c: with --world=NAME (the 3-bit adder, in --docs) and --at near its keypad:
//                        the parser on literal scripts, every CMB1 script parses, the adder shows
//                        3 + 4 = 7 on its own display, then every command, the caps (8 deep, 32 at
//                        once, the frame budget), the safety rules, @step/@near/@timer, a real screen
//                        tap, and nothing of a run surviving a reload. --cmds-mutate=nodepth must fail;
//                        --cmds-trace prints every statement.
//   --console-selftest   C.1: the command-block language as a console line (dev-console-plan.md §8).
//                        World-free: the shared parser table, gate 1 (port/tools/fixtures/cmds/corpus.txt,
//                        --console-corpus=PATH, parsed with block and console options), kVerbs vs the
//                        parser's errors; then a fresh world in --docs: gate 2 (a typed line == the
//                        script pressed on a block the player stands on), jobs/stop/`=`/sink/OP_EXT/
//                        timeHook. --console-mutate=anchor must FAIL it.
//   --sign-shot[=PREFIX] a REAL WINDOW: --world, --at, --look=YAW,PITCH, then PREFIX-signs.bmp and
//                        the sign pass's probe line.
//   --light-selftest     R.3: lightbox place/repaint/break (incl. saturation, the y clip and the
//                        toroidal seam) through the engine's own edit paths, light hash per step.
//   --evict-selftest     B.5b-1b: ring streaming's evict-save, node dropping and autosave. Edits in three
//                        columns survive a 320-block walk (longer past radius 14) out and back and a quit/reload (gate 2);
//                        a fire tower + water in the -x outer-ring column, evicted by one chunk of
//                        walking, leave no node outside the window and no committed column but their
//                        own (gate 3); the autosave timer saves an edit and the fire keeps burning.
//                        --evict-mutate=nosave|nodrop puts the bug back (gate 6: each must FAIL).
//   --ring-light-selftest B.5b-1c: per-column light under ring streaming, in one process (the CI form
//                        of WORKING/b5b1/ring-light-gate.sh). Lightbox lattice + night sky (as
//                        --light-lattice), then a +x and a +z 160-block walk; the walked window's light
//                        hash and mesh checksum must equal what a full re-sweep + re-mesh of the same
//                        window gives, the walk must stream (no bulk reload) and rebuild per column.
//                        Then (P.10) a lightbox built in a column remembered as lightbox-free, and
//                        one more chunk of walking that rebuilds that column's light, vs a re-sweep.
//                        --light-mutate=nosync|nomark|nolbmark must FAIL it.
//   --shadow-selftest    H.2: baked top-face shadows (Classes/Shadow.h; shadows-plan.md §8.2). Tops vs a
//                        fresh scan after every landing and edit, a canopy one chunk up re-shading the
//                        floor below (the edit hook's dirtying), mining it back to the floor's exact
//                        mesh, a lightbox at night, a walk vs a re-scan + re-mesh, Off -> Classic ->
//                        Smooth -> Off. Classic unless --shadows= says otherwise.
//                        --shadow-mutate=nohook|nodirty|nolanding|nodiag must each FAIL it.
//   --classic-selftest   H.4: the seeded "Classic" generator (generator-plan.md §7 gates 1-3, 6). 1.x
//                        columns regenerate byte for byte (--classic-fixture=PATH, default the checked-in
//                        port/tools/fixtures/classic/ cut), pinned per-profile hashes, a 64x64 window vs
//                        its 16 columns, the GENERATOR record + `.eden` mirror, then worlds in --docs:
//                        legacy load/detect/record-once, export mirror, mirror import, unknown => void.
//                        --classic-mutate=tables|trunc|noborder must each FAIL it.
//   --props-selftest     H.15: World Properties (world-properties-plan.md §6 gates 1-5) on worlds in --docs,
//                        through the real screen: Caves Off -> Deep (Cancel first) regenerates untouched
//                        land and keeps the edited column, setting it back restores it; a 1.x world's
//                        detected caves and its one record; home lifted to the new surface; the Terrain
//                        tab's shapes (Scale read-only on 1.x, Flat's type row). --props-world=FILE times
//                        one more read (gate 4). --props-mutate=nogen must FAIL it.
//   --classic=SEED[,SCALE4[,CAVES[,HILLS[,TREES]]]]  H.4: a world --stage1 & co. CREATE is a Classic one
//                        (new profile, the New World tab's defaults; SCALE4 4/6/8/12/16 at --height=256;
//                        X.13: CAVES 0 Off/1 2011/2 Deep, HILLS 0-4, TREES 0-3), e.g. for the ring walk.
//   --shadows=off|classic|smooth  H.2: override the Shadows setting for this run, NOT stored (native
//                        prefs are global, not per --docs). CI's geometry pair passes --shadows=off.
//   --light-selfcheck    R.3: also keep the stock dense light array, written by the stock code,
//                        and report voxels where it and the brick store disagree (`.light`'s
//                        `mismatch`, must be 0). --light-selftest asserts it when present.
//   --empty-selfcheck    R.2's stale-bit check: rebuild2() scans the chunks the empty bit lets it
//                        skip and counts any that were not empty. Reported on `.chunkstate`.
//   --codec-bench        Stage S / S.2: zstd-1/-3/-3+checksum vs zlib-6 encode/decode µs per column over
//                        ~500 columns of --codec-file=PATH (a RAW .eden; --height=64|256). No world load.
//   --bundled-map-selftest  Stage S / S.6: every column of the bundled map through the engine's own
//                        read + decode, once from the RLE `Eden.eden` and once from its bake `Eden.emod`
//                        (--bundled-eden=PATH --bundled-emod=PATH; default: beside the executable). 0 differ.
//   --net-selftest       MP.1: the multiplayer TCP seam (port/src/net): the `\n` framer (8 KB cap, 64 MB for
//                        `SNAPZ:`), base64, the SNAPZ codec and a loopback socket round trip against an in-process
//                        mock. --server=host:port adds a live leg on a local `edenserver`.
//   --mp-selftest        MP.2: the protocol session (join sequence, PING/PONG, chat, early-close strings -> errors)
//                        against the same mock; --server=host:port adds a live leg (port/tools/mp-live-gate.sh).
//   --mp-replay=FILE     MP.3a: a recorded server line stream (port/tools/fixtures/mp/*.lines) played into a session
//                        world with no socket; legs 1 (join burst hash vs FILE's .expect, from mp-replay.py), 2 (walk
//                        out and back, same hash), 5 (teleport = fresh join at the destination) + the E11 gates.
//                        --mp-mutate=noevictcommit|nolandhook must fail it; --mp-overlay-mb=N sets the overlay cap.
//                        MP.3b adds legs 3 (step 5c applies to the resident window; a refused local edit is back
//                        to flat after a re-REGION, reconcile; the pacer's lattice switch, unanswered re-asks
//                        and re-ask on return) and 4 (a 600k-record join through the full flow and a 2,000-cell
//                        relay storm: `mp.join` / `mp.storm` lines, the 24-chunk budget). noreconcile fails 3.
//                        MP.4 adds leg 6: the intent sites send each changed cell (build + paint, door halves,
//                        E10: no burn for a non-flammable, TNT's mine when its fuse runs out, none after a restore),
//                        a sign as SIGNP, a peer's edits apply through the engine (door pairs, fire) and send
//                        nothing back; `mp.edits` line. --mp-mutate=echo fails it.
//   --mp-join=HOST[:PORT] MP.3b: join a server (the dev path until MP.6's server list): interactive, or with
//                        --headless a live gate (join -> playable, the `mp.join` line, window state, exit).
//                        --mp-name=NAME (default Emod), --mp-password=P, --mp-type=N; --mp-reconcile=0 turns
//                        reconcile off (diagnosis). MP.4: --mp-edits (builder) / --mp-hold=FILE (watcher) run
//                        the two-client leg of port/tools/mp-replay.py edits; --mp-mutate=echo must fail it.
//   --emod-selftest      Stage S / S.3: EdenWorldStore + the streaming converter's gates (Classes/
//                        EdenWorldStoreSelftest.cpp). --emod-fixtures=DIR (port/tools/fixtures/emod: the
//                        emod.py byte comparison + the cross-target gate), --emod-work=DIR (scratch,
//                        default ./emod-work), --emod-quick. With --emod-file=W.eden[.gz] [--emod-ref=R.emod]
//                        it instead stream-converts one real world and compares it with emod.py's output.
//   --export=SRC         Stage S / S.5(b): export a world (`.emod` or `.eden`) with --export-out=PATH,
//                        --export-target=own|Legacy64z|NewDawn256z|NewFormat256z, --export-signs=keep|prune,
//                        --export-gz, --export-dry (pre-flight only). Prints `export-report {json}`.
//   --import=SRC         Stage S / S.5: convert a `.eden`/`.gz`/`.zip` to --import-out=W.emod through
//                        the production ImportJob (pre-flight, free-space floor, 500 MB cap; the
//                        EDEN_TEST_FREE_BYTES / EDEN_TEST_TEMP_CAP hooks). --import-kill-after=BYTES
//                        _exit()s mid-run (the kill gate). See src/seam/WorldTools_native.cpp.
//   --upload-*           Stage S / S.5c: build (and, with --upload-url, POST) an upload body; see
//                        WorldTools_native.cpp.
//   --world=NAME         world display name to create or reuse (default depends on the mode).
//   --at=X,Y,Z           teleport target. FIXED BY DEFAULT AND THAT MATTERS: a new world spawns
//                        the player tens of columns away each run, and chunk contents differ by
//                        position, so a floating origin would make every checksum incomparable.
//   --frames=N           settle frames per phase (default 600 — ~10 s of engine time).
//   --walk=DX,DZ[,DX,DZ...]  B.5b-1a: --stage1 also walks the player from --at after the first
//                        report, leg by leg (up to 4; 0.25 block/frame, y pinned — the F3 driver's
//                        walk), settles and reports a `walked` phase plus the end position. Ring
//                        streaming's gate: its geometry/chunkstate must equal a plain --stage1 at
//                        that end position, and `walked.geometry`'s bulk_reloads must not grow.
//   --light-lattice=STEP B.5b-1c: --stage1 instead PREPARES a lightbox world for the ring-light gate
//                        (WORKING/b5b1/ring-light-gate.sh): lightboxes every STEP blocks over --at
//                        +-320 (alternately high in the air and on the surface, varied paint),
//                        placed from 9 teleport stops through buildBlock, then saves and quits.
//   --light-mutate=nosync|nomark|wrap|nolbmark  B.5b-1c's bug-put-back legs (Lighting.h
//                        g_light_test_mutation): never rebuild a landed column's light / rebuild it
//                        but re-mesh nothing / let light wrap past the window edge / (P.10) never
//                        forget a "no lightbox" column on a landing or a lightbox edit. Each must FAIL
//                        the ring-light gate (wrap: --light-selftest's window-edge step).
//   --docs=DIR           the save directory. WORKS AS OF STAGE 2 — through Stage 1 it was inert,
//                        because Classes/FileManager.mm asked real Foundation for
//                        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory) and the
//                        eden_platform_documents_root() seam this flag drives was only read by the
//                        Foundation SHIM, which this target does not compile. That call site now
//                        asks the seam on both targets (byte-identical on web, where the shim's
//                        answer was already exactly this value). Default:
//                        ~/Library/Application Support/Emod — Stage 2's deliberate choice (and
//                        Emod, not Eden: see src/eden_app_identity.h), with
//                        a one-time migration of anything an earlier build left in ~/Documents.
//   --bundle=DIR         likewise ignored: real NSBundle answers with the executable's own
//                        directory, which is why CMake symlinks the flat asset set there.
//
// With no scripted mode it just plays: SDL event pump plus the frame loop.

#include "../seam/main_native.h"
#include "../seam/EdenAppDelegate_native.h"
#include "../seam/Input_native.h"
#include "../seam/SimpleAudioEngine_native.h"
#include "save_backup.h"                 // port/src/shim/foundation — the shared backup rules
#include "../../../Classes/FileManager.h"  // eden_save_backup_hook
#include "../../../Classes/EdenWorldStore.h" // --background-selftest's `.emod` leg
#include "../../../Classes/SimpleAudioEngine.h"
#include "../../../Classes/Resources.h"
#include "../../../Classes/Constants.h"    // CHUNK_SIZE / T_SIZE, for the map-border arithmetic below
#include "../../../Classes/TerrainGen2.h"  // GSIZE — same, and it is macros only
#include "../../../Classes/Menu.h"         // Menu::settings, for --shot's keybinds capture
#include "../../../Classes/WorldPropertiesMenu.h"   // H.15: --props-selftest, --ui-selftest, --shot
#include "../../../Classes/ConsoleMenu.h"           // C.3: --ui-selftest, --shot
#include "../../../Classes/SettingsMenu.h" // SettingsMenu::showKeybinds()
#include "../../../Classes/GLDialog.h"    // --ui-selftest drives GLDialog::prompt
#include "../../../Classes/WorldShare.h"  // --browser-selftest's Share legs (S.5 / S.5c)
#include "../../../Classes/EdenWorldExport.h"
#include "../../../Classes/WorldTrailer.h"   // --trailer-selftest (D.3a)
#include "../../../Classes/SignRenderer.h"   // --signs-selftest / --sign-shot (D.3b)
#include "../../../Classes/SignTool.h"       // --signs-selftest (D.3c)
#include "../../../Classes/CmdScript.h"      // --cmds-selftest (D.4c)
#include "../../../Classes/Console.h"        // --console-selftest (C.2)
#include "../seam/PlayerReadme_native.h"      // C.9: README.txt
#include "../../../Classes/Model.h"          // --cmds-selftest: CountActiveCreatures
#include "../../../Classes/PassTimers.h"     // --pass-probe / --pass-timers (P.1b)
#include "../../../Classes/Shadow.h"         // --shadows= / --shadow-selftest (H.2)
#include "../../../Classes/TerrainGenerator.h"   // --classic-selftest (H.4)
#include "../../../Classes/MpWorld.h"            // --mp-replay (MP.3a)
#include "../../../Classes/ChatFeed.h"           // MP.7: --mp-replay leg 8
#include "../../../Classes/ServerListMenu.h"     // MP.6: --ui-selftest, --shot, --mp-mock
#include "../../../Classes/ServerFormMenu.h"
#include "../../../port/src/net/MpSession.h"     // --mp-replay: the session's own line parser, fed from a file
extern int g_classic_cave_votes_cells, g_classic_cave_votes_air;   // FileManager.mm (H.4), --classic-selftest
extern bool g_wp_test_skip_generator;   // FileManager.mm (H.15), --props-mutate=nogen
extern Vector colorTable[256];               // --cmds-selftest: the night sky (palette 54)
extern int g_ring_columns_landed, g_bulk_reloads, g_ring_evict_saves, g_ring_dynamics_dropped, g_autosaves; // Terrain.mm, --evict-selftest
extern int g_light_test_mutation;            // Lighting.mm (B.5b-1c), --light-mutate
BOOL light_columnKnownFree(int cx, int cz);  // Lighting.mm (P.10), --ring-light-selftest's last leg
extern int g_offcx, g_offcz;                 // Terrain.mm: the toroidal offset (--light-selftest's edge step)
Vector8 lightAtToroidal(int tx, int tz, int y);   // Terrain.mm
extern int regionSkyColors[4][4];            // FileManager.mm: the saved per-region sky (--light-lattice)
void paintSky(int color);                    // TerrainGen2.mm
extern BOOL* chunksToUpdate;                 // Terrain.mm: the mesher's dirty flags (--ring-light-selftest)
extern BOOL* columnsToUpdate;
extern block8* blockarray;                   // Terrain.mm: --mesh-bench's raw window read (B.5b-2)
#include "../../../Classes/Terrain.h"
#include "../../../Classes/Player.h"
#include "../../../Classes/Camera.h"
#include "../../../Classes/Frustum.h"   // --camera-selftest (H.12): ViewTestAABB
#include "gl_es1_shim.h"   // also gives glGetString (renamed to the shim's guarded form)
#include "platform_shims.h"
#include "../eden_app_identity.h"   // Emod is not Eden — see that file

#import <Foundation/Foundation.h>

#include <SDL3/SDL.h>

// PHASE N STAGE 4.1 — iOS HAS NO `int main()`, AND THIS ONE LINE IS THE WHOLE ENTRY POINT.
// <SDL3/SDL_main.h> defines `main` to `SDL_main` and supplies a real main() that calls
// SDL_RunApp -> UIApplicationMain, so the process gets a UIApplication, an app delegate and a
// run loop before a single line below executes — and SDL's delegate then calls this file's
// main() from -postFinishLaunch and exit()s with what it returns. That is why the harness's
// argv parsing and exit codes still work under `simctl launch`.
//
// INCLUDED ONLY ON iOS, deliberately. On macOS, Linux and Windows SDL_main.h is a no-op-ish
// convenience (SDL_MAIN_AVAILABLE, not NEEDED) — but it still renames main, and the three
// desktop legs have working entry points that this stage has no business perturbing. One
// platform's requirement should not become four platforms' indirection.
#if defined(EDEN_PLATFORM_IOS)
#include <SDL3/SDL_main.h>
#endif

#include <cstddef>
#include <vector>
#include <zlib.h>
bool photoEncodePNG(const unsigned char* rgb, int w, int h, std::vector<unsigned char>& out);   // Classes/Util.mm (H.16)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <cmath>
#include <functional>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <cctype>
#include <unistd.h>
#include <zlib.h>                  // --browser-selftest builds its gzip/zip fixtures

// Shared seam exports (port/src/seam/*, compiled into this target too).
extern "C" void eden_click_begin(int);               // Input_web.mm
extern "C" void eden_click_end(int);
extern "C" {
int   eden_get_fps_cap(void);
void  eden_settings_init(void);
void  eden_set_detected_touch(int isTouch);
int   eden_settings_loaded(void);
void  eden_heap_pressure_tick(void);
int   eden_native_thermal_state(void);   // SDLView_native.mm (P.1a)
void  eden_native_gl_set_headless(int on);
void  eden_native_gl_set_present(int on);
int   eden_native_gl_present_guard_fires(void);
void  eden_gl_glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void* px);
void  eden_gl_context_get_drawable_size(int* w, int* h);
SDL_Window* eden_native_gl_window(void);

void  eden_ui_force_legacy(int on);
int   eden_get_fly_mode(void);

int   eden_menu_active(void);
int   eden_menu_world_count(void);
const char* eden_menu_world_name(int index);
const char* eden_menu_world_file(int index);
void  eden_menu_select(int index);
int   eden_menu_selected_index(void);
int   eden_menu_play(void);
char* eden_menu_name_buffer(void);
int   eden_menu_create_world(void);
void  eden_menu_set_pending_world_type(int flat);
void  eden_menu_set_pending_world_height(int height);
int   eden_menu_delete_at(int index);   // X.15: --shadow-selftest clears its scratch world
void  eden_menu_set_pending_generator(const unsigned char* rec28);   // H.4: NULL = none

// Phase N Stage 4.2 (port/src/seam/AppLifecycle_web.mm, cmake-shared into this target).
int   eden_app_will_background(void);
int   eden_app_background_save_count(void);
int   eden_app_background_last_status(void);

void  eden_tap_hud_button_begin(int which);
void  eden_tap_hud_button_end(int which);
int   eden_hud_in_menu(void);
void  eden_console_menu_open(void);      // C.3 (Menu_web.mm)
int   eden_chat_open(int slash);         // MP.7 (Menu_web.mm)
int   eden_console_menu_toggle(void);
int   eden_console_menu_back(void);
int   eden_ui_captures_keys(void);
int   eden_ui_wants_cursor(void);

void  eden_settings_menu_open_now(void);
void  eden_settings_menu_close(void);

int   eden_gl_stat(int which);
int   eden_console_teleport(float x, float y, float z);
int   eden_console_setblock(int x, int z, int y, int type);

const char* eden_debug_menu_state(void);
const char* eden_debug_player_state(void);
const char* eden_debug_terrain_geometry(void);
const char* eden_debug_display_state(void);   // --touch-selftest reads `rmenu` out of this
void        eden_native_gl_get_letterbox(int* x, int* y, int* w, int* h);
struct SDL_Window;
SDL_Window* eden_native_gl_window(void);
const char* eden_debug_mesh_checksum(void);
const char* eden_debug_chunk_state(void);
const char* eden_debug_light_state(void);
const char* eden_debug_shadow_state(void);                    // H.2
const char* eden_debug_top_shade(int x, int z, int y);        // H.2
const char* eden_debug_chunk_mesh_hash(int x, int z, int y);  // H.2
int   eden_debug_light_edit(int op, int x, int z, int y, int color);
void  eden_set_save_inplace_threshold(unsigned long long);   // N.8
void  eden_debug_set_light_selfcheck(int on);
void  eden_debug_set_empty_selfcheck(int on);
long long eden_debug_chunk_empty(int x, int z, int y);
int   eden_debug_build_block(int x, int z, int y, int type);
void* eden_debug_world_trailer(void);                    // WorldTrailer* (D.3a)
const char* eden_debug_world_file(void);
int   eden_debug_modified_chunks(void);
const char* eden_debug_trailer_refusal_toast(int result);
int   eden_console_getblock(int x, int z, int y);
void  eden_debug_set_mesh_checksum(int on);
const char* eden_debug_world_format(void);
const char* eden_debug_heap(void);
void  eden_debug_heap_reset_peak(void);
void  eden_native_apply_display_mode(void);   // src/seam/DisplayMode_native.cpp
void  eden_native_set_safe_area_override(float l, float t, float r, float b);   // I.2, same file
void  eden_safe_rect(float* x, float* y, float* w, float* h);   // I.2, DisplayProfile_web.mm
void  eden_display_debug_ignore_safe_area(int on);
int   eden_debug_safe_area_violations(float sx, float sy, float sw, float sh, char* buf, int cap);       // I.2, DebugState_web.mm
void  eden_live_commands_init(const char* path);   // src/seam/LiveCommands_native.mm (diagnostics)
void  eden_live_commands_tick(void);
}

extern float SCREEN_WIDTH;
extern float SCREEN_HEIGHT;

// Settings_web.mm's mirrored gamepad rows; --gamepad-selftest reports the live deadzone.
extern float eden_gamepad_deadzone;

// Classes/Alert.h — the native seam (seam_link_stubs_native.mm) implements this as a GLDialog.
void showAlertWarpHome();

#if defined(EDEN_DIAGNOSTICS)
extern "C" const char* eden_bench_save(void);   // DevConsole_web.mm (X.1: --save-bench)
#endif

extern "C++" int eden_codec_bench(const char* path, int height, int want);   // CodecBench_native.cpp
extern "C++" int eden_world_tool_main(int argc, char** argv);                 // WorldTools_native.cpp
int fmh_selftestCompare(const char* edenPath, const char* emodPath, int* compared);   // Classes/FileManagerHelper.mm (S.6)
extern "C" int eden_net_selftest_run(const char* server);   // port/src/net/NetSelftest.cpp (MP.1)
extern "C" int eden_mp_selftest_run(const char* server);    // same file (MP.2)
#if defined(EDEN_DIAGNOSTICS)
extern "C" int eden_emod_selftest_run(const char* fixtureDir, const char* workDir, int flags,
                                      const char* file, const char* ref);       // Classes/EdenWorldStoreSelftest.cpp
#endif

namespace {

void eden_setenv(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

struct Options {
    bool headless = false;
    const char* mode = nullptr;          // "p1-gate" | "stage1" | "save-roundtrip" | nullptr
    int  height = 64;
    std::string world;
    float at[3] = {0.0f, 40.0f, 0.0f};
    bool haveAt = false;
    int  frames = 600;
    int  cycles = 8;                     // --leak-probe: load/quit cycles in one process
    int  winW = 0, winH = 0;             // --window=WxH: 0 means the built-in default (16:9)
    bool touchProfile = false;           // --touch-profile: pretend to be a touchscreen
    float safeL = 0, safeT = 0, safeR = 0, safeB = 0;   // --safe-area=L,T,R,B (window points); I.0 overlay
    std::string docs;
    std::string bundle;
    std::string shot;                    // --shot=PREFIX: file prefix for --shot's captures
    std::string netServer;               // --server=host:port: --net-selftest / --mp-selftest live leg
    std::string mpReplay, mpMutate;      // --mp-replay=FILE, --mp-mutate=noevictcommit|nolandhook (MP.3a)
    int mpOverlayMb = 0;                 // --mp-overlay-mb=N (0 = MpWorld's default, 48)
    std::string mpJoin, mpName, mpPassword;   // --mp-join=HOST[:PORT], --mp-name=, --mp-password= (MP.3b)
    int mpType = 0;                      // --mp-type=N (characterType)
    bool mpEdits = false;                // --mp-edits: MP.4's two-client leg, the builder
    std::string mpHold;                  // --mp-hold=FILE: ... the watcher, until FILE exists
    std::string codecFile;               // --codec-file=PATH: the raw .eden --codec-bench samples
    std::string emodFixtures, emodWork, emodFile, emodRef;   // --emod-selftest's inputs
    std::string bundledEden, bundledEmod;                    // --bundled-map-selftest's inputs
    bool emodQuick = false;
    std::string liveCmds;                // --live-cmds=FILE: see src/seam/LiveCommands_native.mm
    bool trailerNoDirty = false;         // --trailer-no-dirty-mark: the mutation (live leg only)
    bool trailerKeep = false;            // --trailer-keep: stop after the reload (leave the edited world for emod.py --diff)
    std::vector<std::string> trailerFiles;   // --trailer-file=W.eden|W.emod (repeatable): --trailer-selftest's byte gate
    std::string evictMutate;             // --evict-mutate=nosave|nodrop: --evict-selftest's mutation legs
    std::string signsMutate;             // --signs-mutate=bottom|nohook: --signs-selftest's mutation legs
    std::string cmdsMutate;              // --cmds-mutate=nodepth: --cmds-selftest's mutation leg
    std::string consoleMutate;           // --console-mutate=anchor: --console-selftest's mutation leg (C.1)
    std::string consoleCorpus = "../port/tools/fixtures/cmds/corpus.txt";   // --console-corpus=
    std::string consoleRef = "../docs/console-reference.md";                // --console-reference=
    bool cmdsTrace = false;              // --cmds-trace: print every command-block statement
    float look[2] = {0.0f, 0.0f};        // --look=YAW,PITCH (degrees): --sign-shot's view
    bool haveLook = false;
    float walk[8] = {0};                 // --walk=DX,DZ[,...]: --stage1's scripted walk legs (B.5b-1a)
    int  walkLegs = 0;
    int  lightLattice = 0;               // --light-lattice=STEP: --stage1 prepares the ring-light gate's world (B.5b-1c)
    int  renderScalePct = 0;             // --render-scale=50|75|100|125: 0 leaves the setting alone
    std::vector<std::pair<std::string,float>> sets;   // --set=KEY=VALUE (repeatable): a settings write once loaded (P.2a)
    int  passFrames = 600;               // --pass-probe=N: timed frames per pose (P.1b)
    bool passTimers = false;             // --pass-timers: --stage1 --walk also prints `walk.passes`
    bool passFinish = false;             // --pass-finish: glFinish at every pass boundary (upper bound per pass)
    bool passVsync = false;              // --pass-vsync: keep vsync on (the player's frame; present then holds the vsync wait)
    std::string convertAnswer;           // --convert-answer=upgrade|keep|cancel: --convert-prompt-selftest
    std::string classicFixture = "../port/tools/fixtures/classic/legacy-1315365413.bin.gz";   // --classic-fixture=
    std::string classicMutate;           // --classic-mutate=tables|trunc|noborder: --classic-selftest's mutation legs
    std::string propsMutate;             // --props-mutate=nogen: --props-selftest's mutation leg (H.15)
    std::string propsWorld;              // --props-world=FILE: --props-selftest times reading this world (gate 4)
    std::string classicNew;              // --classic=SEED[,SCALE4]: a world open_world() CREATES is Classic (ring-walk gate)
};

Options g_opt;
eden_native::EdenAppDelegate* g_app = nullptr;

// Phase N Stage 4.2 — see the SDL_AddEventWatch() call in main() for why this is a watch and not a
// case in the poll loop. A named SDLCALL function rather than a lambda because SDLCALL is a real
// calling convention on the Windows toolchains Stage 3 ships, and a converted lambda's is whatever
// the compiler's default happens to be.
static bool SDLCALL eden_lifecycle_event_watch(void*, SDL_Event* ev) {
    if (!g_app) return true;
    switch (ev->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_TERMINATING:
            g_app->onPageHide();          // save the open world, then stop the loop
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            g_app->onVisibilityVisible(); // ...and start it again on the way back
            break;
        default:
            break;
    }
    return true;                          // never filter: the poll loop still sees everything
}

// ---------------------------------------------------------------------------------------------
// Frame driving
// ---------------------------------------------------------------------------------------------

// One frame, plus the SDL event pump when there is a window. The 2 ms sleep in scripted mode is
// not throttling for its own sake: EdenViewController::drawFrame derives etime from a real clock,
// and a loop that spins as fast as it can feeds the engine a ~0 s delta, which stalls everything
// time-driven (streaming, physics, the lighting slices) while burning CPU. 2 ms gives an etime in
// the same ballpark a real frame has.
void pump_events();

// Set by --input-selftest only. The other scripted modes must NOT run the input tick: it calls
// eden_ui_tick(), which drives the block-preview ghost — and a preview block in the window would
// change the very geometry checksum --stage1 exists to compare.
bool g_tickInput = false;

// The settings model, pumped once a frame until it takes. `eden_settings_init()` needs
// World::getWorld->menu->settings to exist and early-returns until it does, so the contract is
// "call it every frame and stop when eden_settings_loaded() answers yes" — exactly what
// public/eden-st.html's rAF loop does on web. It is NOT a one-shot call after eden_seam_main():
// the World is built by the app delegate but the menu's SettingsMenu is not reachable on the
// first frame, and a single early call is a silent no-op that leaves every port-owned setting at
// its file-scope initialiser instead of its declared default.
int  set_setting_by_key(const char* key, float v);   // --set, defined beside run_preset_probe()
bool set_render_scale_pct(int pct);   // --render-scale, defined beside run_scale_probe()
extern "C" int g_eden_mem_trace;           // HeapProbe_native.cpp, --mem-trace (N.4.9)
extern "C" void eden_mem_trace_install(void);   // HeapProbe_native.cpp, --mem-trace=2
extern "C" void eden_gl_set_tex_experiments(int bits);   // gl_fixed_function.cpp, --tex-exp=
extern "C" void eden_gl_set_tex_flush(int on);           // gl_fixed_function.cpp, N.4.9's fix
extern "C" void eden_mem_trace(const char* where);
extern "C" void eden_test_force_touch_input(void);   // Settings_web.mm (X.19)
void settings_pump() {
    if (!eden_settings_loaded()) eden_settings_init();
    // --render-scale=N is applied the first frame the model has loaded (before then a write would
    // be overwritten by the stored value). It is an ordinary settings write, so it persists the
    // way a player's choice in the Settings screen would.
    static bool scaleApplied = false;
    if (!scaleApplied && g_opt.renderScalePct > 0 && eden_settings_loaded()) {
        scaleApplied = true;
        set_render_scale_pct(g_opt.renderScalePct);
    }
    // I.0: the stored prefs load after startup and would put a saved Input mode back over --touch-profile.
    static bool touchForced = false;
    if (!touchForced && g_opt.touchProfile && eden_settings_loaded()) {
        touchForced = true;
        eden_test_force_touch_input();
    }
    static bool setsApplied = false;
    if (!setsApplied && !g_opt.sets.empty() && eden_settings_loaded()) {
        setsApplied = true;
        for (auto& kv : g_opt.sets)
            if (!set_setting_by_key(kv.first.c_str(), kv.second))
                std::fprintf(stderr, "[eden] --set: no setting %s\n", kv.first.c_str());
    }
}

void tick(int n) {
    if (!g_app) return;
    for (int i = 0; i < n; ++i) {
        pump_events();
        settings_pump();
        if (g_tickInput) eden_native_input_tick();
        g_app->viewController.drawFrame(true);
        eden_native_audio_tick();
        eden_heap_pressure_tick();
        // The 1 ms yield is not frame pacing any more (setFixedEtime does that) — it is there so
        // the wall-clock-driven hold-to-act repeat in the input translator still gets a realistic
        // number of pulses per scripted second, and so a runaway loop is interruptible.
        if (g_opt.mode) SDL_Delay(1);
    }
}

// Ticks until `pred` is true or `timeoutFrames` frames have passed. Returns whether it happened.
template <typename Pred>
bool tick_until(Pred pred, int timeoutFrames, const char* what) {
    // A frame budget alone is racy against worker threads (the net seam, the manifest parse): a
    // fast machine burns 1500 frames in a few ms. So a timeout needs the frames AND 10 s of wall
    // clock (a real failure costs at most 10 s more) — Windows CI lost "selecting a world shows
    // its preview" to exactly this.
    const Uint64 t0 = SDL_GetTicks();
    for (int i = 0; i < timeoutFrames || SDL_GetTicks() - t0 < 10000; ++i) {
        if (pred()) return true;
        tick(1);
    }
    std::fprintf(stderr, "[eden-stage1] TIMEOUT waiting for %s (%d frames)\n", what, timeoutFrames);
    return false;
}

int game_mode() {
    const char* s = eden_debug_menu_state();
    const char* k = std::strstr(s, "\"game_mode\":");
    return k ? atoi(k + 12) : -1;
}

// ---------------------------------------------------------------------------------------------
// Scripted phases
// ---------------------------------------------------------------------------------------------

void report(const char* phase) {
    std::printf("[eden-stage1] %s.mem %s\n", phase, eden_debug_heap());
    eden_mem_trace(phase);
    std::printf("[eden-stage1] %s.geometry %s\n", phase, eden_debug_terrain_geometry());
    std::printf("[eden-stage1] %s.checksum %s\n", phase, eden_debug_mesh_checksum());
    std::printf("[eden-stage1] %s.chunkstate %s\n", phase, eden_debug_chunk_state());
    std::printf("[eden-stage1] %s.light %s\n", phase, eden_debug_light_state());
    std::printf("[eden-stage1] %s.shadow %s\n", phase, eden_debug_shadow_state());
    std::printf("[eden-stage1] %s.format %s\n", phase, eden_debug_world_format());
    std::fflush(stdout);
}

// Create the named world if the menu does not already list it, then play it. Mirrors what
// public/eden-menu.js does and what the headless probes do: set the pending world TYPE and HEIGHT
// first (the engine asks for both at Play time, not at create time — see Menu_web.mm), then
// create, select and play.
bool open_world(const char* displayName, int height) {
    int idx = -1;
    const int count = eden_menu_world_count();
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(eden_menu_world_name(i), displayName) == 0) { idx = i; break; }
    }
    // X.16: `_` may stand for a space (args are whitespace-split), tried only when the literal name
    // is not a world: --world=Quarry_Town_V24I opens "Quarry Town V24I".
    std::string spaced;
    if (idx < 0 && std::strchr(displayName, '_')) {
        spaced = displayName;
        for (size_t k = 0; k < spaced.size(); ++k) if (spaced[k] == '_') spaced[k] = ' ';
        for (int i = 0; i < count; ++i)
            if (std::strcmp(eden_menu_world_name(i), spaced.c_str()) == 0) { idx = i; displayName = spaced.c_str(); break; }
    }
    const Uint64 tOpen0 = SDL_GetTicks();
    // "Normal" (not flat) so the world streams from the bundled Eden.eden — which is the whole
    // point for criterion 2: the default world is a fixed, shipped 2880x2880 map, so the same
    // origin yields the same blocks on every platform and every run. A flat world would compare
    // equal for the trivial reason that it is empty.
    eden_menu_set_pending_world_type(0);
    eden_menu_set_pending_world_height(height);
    // H.4: --classic=SEED[,SCALE4] makes a world this call creates a Classic one (new profile,
    // the New World tab's defaults), as the tab's Play would; an existing world keeps its own.
    unsigned char rec[CLASSIC_RECORD_BYTES];
    const bool classic = idx < 0 && !g_opt.classicNew.empty();
    if (classic) {
        ClassicGen g;
        int seed = 0, scale4 = 4, caves = -1, hills = -1, trees = -1;
        std::sscanf(g_opt.classicNew.c_str(), "%d,%d,%d,%d,%d", &seed, &scale4, &caves, &hills, &trees);
        classic_default_params(&g, seed);
        if (height == 256) g.scale4 = (unsigned char)scale4;
        if (caves >= 0 && caves <= 2) g.caves = (unsigned char)caves;     // X.13: 0 Off, 1 2011, 2 Deep
        if (hills >= 0 && hills <= 4) g.hilliness = (unsigned char)hills;
        if (trees >= 0 && trees <= 3) g.trees = (unsigned char)trees;
        classic_encode_record(g, rec);
    }
    eden_menu_set_pending_generator(classic ? rec : nullptr);
    if (idx < 0) {
        char* buf = eden_menu_name_buffer();
        std::snprintf(buf, 128, "%s", displayName);
        idx = eden_menu_create_world();
        if (idx < 0) { std::fprintf(stderr, "[eden-stage1] create_world failed\n"); return false; }
    }
    eden_menu_select(idx);
    if (eden_menu_play() != 1) { std::fprintf(stderr, "[eden-stage1] menu_play refused\n"); return false; }
    const bool ok = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
    std::printf("[eden-stage1] open_world \"%s\" %s first-load %llu ms\n", displayName, classic ? "(created Classic)" : "", (unsigned long long)(SDL_GetTicks() - tOpen0));
    std::fflush(stdout);
    return ok;
}

// The HUD taps the web harnesses use, with the same button indices (0 = open the in-game menu,
// 3 = save, 6 = exit to the main menu). Driven as real synthetic touches rather than by poking
// engine flags, because every `hud->` input flag is re-derived from the live touch set each frame
// — writing the flag directly is silently overwritten (web/CLAUDE.md's fast facts; it has bitten
// this port three times).
void tap_hud(int which) {
    eden_tap_hud_button_begin(which);
    tick(6);
    eden_tap_hud_button_end(which);
    tick(6);
}

void ensure_hud_menu_open() {
    if (eden_hud_in_menu() == 0) tap_hud(0);
}

void save_world() {
    ensure_hud_menu_open();
    tap_hud(3);
    tick(120);
}

bool quit_to_menu() {
    ensure_hud_menu_open();
    tap_hud(6);
    return tick_until([] { return game_mode() == 0; }, 20000, "game_mode back to MENU");
}

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
static int g_meshBenchPasses = 0;
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
struct PassAgg {
    std::vector<double> frame, ms[P_COUNT];
    std::vector<long> draws[P_COUNT], verts[P_COUNT], setup[P_COUNT];
    std::vector<long> fDraws, fVerts, fSetup, fElided;
    void add(double frameMs) {
        frame.push_back(frameMs);
        for (int i = 0; i < P_COUNT; ++i) {
            ms[i].push_back(g_pass.ms[i]);
            draws[i].push_back(g_pass.draws[i]); verts[i].push_back(g_pass.verts[i]); setup[i].push_back(g_pass.setup[i]);
        }
        // eden_gl_stat() is the frame before this one's (it rotates at drawFrame's boundary); over
        // hundreds of frames at a still pose that is the same distribution.
        fDraws.push_back(eden_gl_stat(0)); fVerts.push_back(eden_gl_stat(4));
        fSetup.push_back(eden_gl_stat(1)); fElided.push_back(eden_gl_stat(2));
    }
    template <typename T> static T q(std::vector<T> v, double f) {
        if (v.empty()) return T();
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))];
    }
    static double mean(const std::vector<double>& v) {
        double t = 0; for (double x : v) t += x; return v.empty() ? 0 : t / (double)v.size();
    }
    void print(const char* tag, const char* label, int vsync, int thermal0, int batt0) const {
        std::string out;
        char b[256];
        std::snprintf(b, sizeof b, "{\"frames\":%d,\"vsync\":%d,\"finish\":%d,\"frame_p50_ms\":%.3f,\"frame_p90_ms\":%.3f,"
                      "\"frame_max_ms\":%.3f,\"thermal_start\":%d,\"thermal_end\":%d,\"battery_pct_start\":%d,"
                      "\"draws\":%ld,\"verts\":%ld,\"setup\":%ld,\"elided\":%ld,\"p\":{",
                      (int)frame.size(), vsync, g_pass.finish ? 1 : 0, q(frame, 0.5), q(frame, 0.9),
                      q(frame, 1.0), thermal0, eden_native_thermal_state(), batt0,
                      q(fDraws, 0.5), q(fVerts, 0.5), q(fSetup, 0.5), q(fElided, 0.5));
        out += b;
        for (int i = 0; i < P_COUNT; ++i) {
            std::snprintf(b, sizeof b, "%s\"%s\":{\"p50\":%.3f,\"p90\":%.3f,\"max\":%.3f,\"mean\":%.3f,\"draws\":%ld,\"verts\":%ld,\"setup\":%ld}",
                          i ? "," : "", kEdenPassNames[i], q(ms[i], 0.5), q(ms[i], 0.9), q(ms[i], 1.0), mean(ms[i]),
                          q(draws[i], 0.5), q(verts[i], 0.5), q(setup[i], 0.5));
            out += b;
        }
        std::printf("[%s] %s.passes %s}}\n", tag, label, out.c_str());
        std::fflush(stdout);
    }
};

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
    std::printf("[eden-smoke] GL_VERSION  %s\n", (const char*)glGetString(GL_VERSION));
    std::printf("[eden-smoke] GL_RENDERER %s\n", (const char*)glGetString(GL_RENDERER));
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
            // 0x1908 = GL_RGBA, 0x1401 = GL_UNSIGNED_BYTE. Named numerically because this file
            // sees the shim's guarded GL surface, not the raw enums.
            eden_gl_glReadPixels((w - side) / 2, (h - side) / 2, side, side, 0x1908, 0x1401, px.data());
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
bool write_bmp(const char* path, int w, int h, const std::vector<unsigned char>& rgba) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "[eden-shot] cannot open %s\n", path); return false; }
    const int rowBytes = ((w * 3) + 3) & ~3;          // BMP rows are 4-byte aligned
    const int imageBytes = rowBytes * h;
    unsigned char hdr[54] = {0};
    auto put32 = [&](int off, unsigned v) {
        hdr[off] = v & 0xff; hdr[off+1] = (v >> 8) & 0xff;
        hdr[off+2] = (v >> 16) & 0xff; hdr[off+3] = (v >> 24) & 0xff;
    };
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(2, 54 + imageBytes); put32(10, 54);
    put32(14, 40); put32(18, (unsigned)w); put32(22, (unsigned)h);
    hdr[26] = 1; hdr[28] = 24;                         // 1 plane, 24 bpp, BI_RGB
    put32(34, (unsigned)imageBytes);
    std::fwrite(hdr, 1, sizeof(hdr), f);
    std::vector<unsigned char> row((size_t)rowBytes, 0);
    for (int y = 0; y < h; ++y) {                      // glReadPixels is already bottom-up
        const unsigned char* src = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 2];   // B
            row[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];   // G
            row[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 0];   // R
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// Draws one more frame WITHOUT presenting it and reads the whole back buffer, the same trick
// run_smoke()'s centre readback uses — a presented frame's back buffer is undefined after
// SDL_GL_SwapWindow, so reading it after the fact is a race with the driver.
// `overlay`, when given, draws over that frame before the readback (the `kit` page's way in).
bool g_fitGate = false;   // I.3: --fit-selftest runs run_shot()'s tour and checks instead of shooting
void fit_check(const char* label);   // defined after check()

bool capture(const char* label, void (*overlay)() = nullptr) {
    if (g_fitGate) {
        GLW::debugRecordControls(true);
        eden_native_gl_set_present(0);
        tick(1);
        eden_native_gl_set_present(1);
        if (!overlay) fit_check(label);   // the kit page is a demo sheet, not a screen
        GLW::debugRecordControls(false);
        return true;
    }
    int w = 0, h = 0;
    eden_gl_context_get_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) { std::fprintf(stderr, "[eden-shot] no drawable\n"); return false; }
    eden_native_gl_set_present(0);
    tick(1);
    if (overlay) overlay();
    std::vector<unsigned char> px((size_t)w * h * 4);
    eden_gl_glReadPixels(0, 0, w, h, 0x1908, 0x1401, px.data());   // GL_RGBA, GL_UNSIGNED_BYTE
    eden_native_gl_set_present(1);
    if (g_opt.safeL > 0 || g_opt.safeT > 0 || g_opt.safeR > 0 || g_opt.safeB > 0) {
        // I.0: tint the unsafe bands (island / home bar) red so a shot shows what the hand covers.
        // Rows are bottom-up, so the "top" inset is the last rows. Window points -> drawable pixels.
        int sw = 0, sh = 0;
        if (eden_native_gl_window()) SDL_GetWindowSize(eden_native_gl_window(), &sw, &sh);
        if (sw > 0 && sh > 0) {
            const float kx = (float)w / sw, ky = (float)h / sh;
            const int L = (int)(g_opt.safeL * kx), R = (int)(g_opt.safeR * kx);
            const int T = (int)(g_opt.safeT * ky), B = (int)(g_opt.safeB * ky);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                if (x < L || x >= w - R || y < B || y >= h - T) {
                    unsigned char* q = px.data() + ((size_t)y * w + x) * 4;
                    q[0] = (unsigned char)((q[0] + 255 * 2) / 3); q[1] = (unsigned char)(q[1] * 2 / 3); q[2] = (unsigned char)(q[2] * 2 / 3);
                }
            }
        }
    }
    char path[1024];
    std::snprintf(path, sizeof(path), "%s-%s.bmp",
                  g_opt.shot.empty() ? "eden-shot" : g_opt.shot.c_str(), label);
    if (!write_bmp(path, w, h, px)) return false;
    int ww = 0, wh = 0, wp = 0, hp = 0;
    if (eden_native_gl_window()) {
        SDL_GetWindowSize(eden_native_gl_window(), &ww, &wh);
        SDL_GetWindowSizeInPixels(eden_native_gl_window(), &wp, &hp);
    }
    std::printf("[eden-shot] %s -> %s (drawable %dx%d, window %dx%d pt / %dx%d px, "
                "point space %.0fx%.0f)\n",
                label, path, w, h, ww, wh, wp, hp, SCREEN_WIDTH, SCREEN_HEIGHT);
    std::printf("[eden-shot]   player %s\n", eden_debug_player_state());
    std::printf("[eden-shot]   geom   %s\n", eden_debug_terrain_geometry());
    std::fflush(stdout);
    return true;
}

void ui_tap(float x, float yUp);   // --ui-selftest's pointer tap, defined below

// --shot's `kit` page (U.1): every GL kit control in every state, laid out in the Stage U panel
// (WORKING/ui-rework-plan-2026-10-11.md §3.1) so the geometry can be judged against the reference
// screenshots. It is drawn by the harness over the main-menu frame, not by a screen: the controls
// are the subject, and no screen uses all of them yet. Built once (labels are texture uploads).
void render_kit_page() {
    using namespace GLW;
    struct Kit {
        IconRail rail;
        GLW::Button back, play, defaults, share, tile;
        Label title;
        TwoLineRow rows[9];
        Toggle toggles[9];
        Slider slider;
        Stepper stepper;
        ScrollView scroll;
        CGRect panel, content;
    };
    static Kit* k = nullptr;
    const float pad = du(9), gap = du(6);
    if (!k) {
        k = new Kit;
        const int icons[] = { UIICON_WRENCH, UIICON_VOLUME_2, UIICON_GAMEPAD_2, UIICON_KEYBOARD,
                              UIICON_MONITOR, UIICON_FLASK_CONICAL, UIICON_HARD_DRIVE, UIICON_SETTINGS,
                              UIICON_SQUARE, UIICON_MOUNTAIN, UIICON_PLAY, UIICON_INFO };
        k->rail.setTiles(icons, 12);              // 12 > what fits: the rail scrolls
        k->rail.setEnabled(5, false);
        k->rail.setSelected(0);
        k->rail.setHeld(2);
        k->back.setLabel("Back", du(20));     k->back.sizeToFit();
        k->play.setLabel("Play", du(20));     k->play.setIcon(UIICON_PLAY);
        k->play.setTone(GLW::Button::TONE_POSITIVE); k->play.sizeToFit();
        k->defaults.setLabel("Defaults", du(20)); k->defaults.sizeToFit(); k->defaults.setPressed(true);
        k->share.setLabel("Share", du(20));   k->share.sizeToFit(); k->share.setEnabled(false);
        k->tile.setIcon(UIICON_INFO);         k->tile.sizeToFit();
        k->title.set("Kit Settings", du(32), UITextAlignmentCenter);
    }
    const float W = std::min(SCREEN_WIDTH - 2 * du(12), du(783));
    const float H = std::min(SCREEN_HEIGHT - 2 * du(12), du(587));
    k->panel = CGRectMake((SCREEN_WIDTH - W) * 0.5f, (SCREEN_HEIGHT - H) * 0.5f, W, H);
    const CGRect& P = k->panel;
    const float barH = du(29), barTop = P.origin.y + P.size.height - du(7);
    const float left = P.origin.x + pad, right = P.origin.x + P.size.width - pad;
    k->back.setRect(CGRectMake(left, barTop - barH, 0, barH));
    float x = right;
    GLW::Button* acts[] = { &k->play, &k->defaults, &k->share, &k->tile };
    for (GLW::Button* b : acts) { x -= b->fitWidth(); b->setRect(CGRectMake(x, barTop - barH, 0, barH)); x -= du(5); }
    const float bodyTop = barTop - barH - pad, bodyBot = P.origin.y + pad;
    k->rail.setRect(CGRectMake(left, bodyBot, IconRail::designW(), bodyTop - bodyBot));
    const float barW = du(18);
    const CGRect bar = CGRectMake(right - barW, bodyBot, barW, bodyTop - bodyBot);
    k->scroll.setBarRect(bar);
    const float cx0 = left + IconRail::designW() + gap;
    k->content = CGRectMake(cx0, bodyBot, bar.origin.x - gap - cx0, bodyTop - bodyBot);
    const CGRect& C = k->content;

    // Rows: every toggle state, a dependent pair, a wrapping hint, a slider, a stepper, no hint.
    struct R { const char* t; const char* h; int kind; bool on, muted, indent; };
    static const R spec[9] = {
        { "Health", "Take damage from creatures, explosions and lava.", 0, true, false, false },
        { "Creatures", "Spawn and render creatures.", 0, false, false, false },
        { "Hostile creatures", "Spawn and render Chargers and Stalkers (muted, parent off).", 0, true, true, true },
        { "Night creatures", "Muted and off.", 0, false, true, true },
        { "Render distance", "How far the world draws. Longer is slower; on an old iPad the Low "
          "preset keeps it at four chunks and this hint wraps to a second line to prove it can.", 1, false, false, false },
        { "Graphics preset", "Low, Default or High.", 2, false, false, false },
        { "Auto-jump", nullptr, 0, true, false, false },
        { "Mouse sensitivity", "A slider row.", 1, false, false, false },
        { "Experiments", "Off.", 0, false, false, false },
    };
    float heights[9];
    const float sliderW = du(200), stepW = du(29) * 2 + du(110);
    for (int i = 0; i < 9; ++i) {
        const float cw = spec[i].kind == 0 ? Toggle::designW() : spec[i].kind == 1 ? sliderW : stepW;
        const float ch = spec[i].kind == 0 ? Toggle::designH() : du(29);
        k->rows[i].build(spec[i].t, spec[i].h, C.size.width, cw, ch, spec[i].indent ? du(14) : 0.0f);
        k->rows[i].setMuted(spec[i].muted);
        heights[i] = k->rows[i].height();
    }
    k->scroll.setRowHeights(heights, 9, C.size.height);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    const float sc = IS_IPAD ? SCALE_WIDTH : 1.0f;
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCREEN_WIDTH * sc, 0, SCREEN_HEIGHT * sc, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    fill(CGRectMake(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT), kScrim);
    bevel(P, BEVEL_WINDOW);
    k->back.render();
    for (GLW::Button* b : acts) b->render();
    k->title.drawCentered(P.origin.x + P.size.width * 0.5f, barTop - (barH - k->title.height()) * 0.5f, kText);
    k->rail.render();
    bevel(C, BEVEL_CONTENT);
    const int first = k->scroll.first(), last = first + k->scroll.visible();
    for (int i = first; i < last && i < 9; ++i) {
        TwoLineRow& row = k->rows[i];
        row.setOrigin(C.origin.x, C.origin.y + C.size.height - k->scroll.rowOffset(i));
        row.setLast(i == 8);
        row.render();
        if (spec[i].kind == 0) {
            Toggle& t = k->toggles[i];
            t.setRect(row.controlRect(Toggle::designW(), Toggle::designH()));
            t.setOn(spec[i].on);
            t.setMuted(spec[i].muted);
            t.render();
        } else if (spec[i].kind == 1) {
            k->slider.setRange(0, 1, 0.05f);
            k->slider.setValue(i == 4 ? 0.6f : 0.25f);
            k->slider.setRect(row.controlRect(sliderW, du(29)));
            k->slider.render();
        } else {
            k->stepper.setGlyphs("<", ">");
            k->stepper.setValueText("Default", du(20));
            k->stepper.setRect(row.controlRect(stepW, du(29)));
            k->stepper.render();
        }
    }
    k->scroll.render();

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

void ui_tap_rect(CGRect r);   // --ui-selftest's, defined below (the Classic -64 shot taps the height pair)

int run_shot() {
    const char* name = g_opt.world.empty() ? "shot" : g_opt.world.c_str();
    // No vsync: a capture needs drawn frames, not paced ones, and a vsync-blocked swap never
    // returns while the display is asleep or locked (2026-10-06: --shot sat at tick 0 for minutes
    // on an unattended Mac).
    SDL_GL_SetSwapInterval(0);
    tick(180);
    // I.7: -home is the first screen; -menu stays the world list ("My Worlds").
    if (World::getWorld && World::getWorld->menu) World::getWorld->menu->showHome(true);
    tick(4);
    capture("home");
    if (World::getWorld && World::getWorld->menu) World::getWorld->menu->showHome(false);
    tick(4);
    capture("menu");
    capture("kit", render_kit_page);

    // Stage 5.9: Get Worlds, on the archive tab and on the current server's, each with its first
    // world selected and (when the host has one) its preview. Live network unless --net-fixtures
    // was given; a capture of the "couldn't reach" state is still the screen working.
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->browserOffered()) {
        Menu* m = World::getWorld->menu;
        WorldBrowser* b = m->browser;
        m->openBrowser();
        for (int src = 0; src < 2; ++src) {
            CGRect r = b->tabRect(src);
            ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
            tick_until([b] { return !b->listLoading(); }, 6000, "the browser's list");
            if (b->rowRect(0, &r)) ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
            tick_until([b] { return b->previewShown(); }, 1500, "a preview");
            tick(4);
            capture(src == 0 ? "browser" : "browser-current");
        }
        b->close();
        m->showbrowser = FALSE;
        tick(4);
    }

    // MP.6: the Multiplayer screens over the --mp-mock fixture (UI plan §9): the list, its empty
    // state, the forms (one with a faked keyboard), the password prompt, Connecting, two errors.
    if (World::getWorld && World::getWorld->menu) {
        Menu* m = World::getWorld->menu;
        ServerListMenu* sl = m->servers;
        ServerFormMenu* fm = sl->form();
        char keptName[32];   // the developer's own profile survives a --shot on real prefs
        eden_mp_profile_name(keptName, sizeof keptName);
        const int keptChar = eden_mp_profile_character();
        eden_mp_servers_mock(1);
        eden_mp_profile_set("Sam", 1);
        m->openServers();
        tick_until([] { return eden_mp_servers_poll() == 0; }, 300, "the mock probes");
        tick(4);
        capture("mp-list");
        sl->showTab(1);   // MP.8: the fixture's two matchmakers and four listings
        tick(4);
        capture("mp-list-matchmakers");
        sl->showTab(0);
        tick(2);
        fm->openAdd();
        fm->blurFields();
        tick(4);
        capture("mp-form-add");
        fm->close(ServerFormMenu::OUT_BACK);
        fm->openEdit(0);
        fm->blurFields();
        tick(4);
        capture("mp-form-edit");
        eden_native_debug_set_keyboard_top(SCREEN_HEIGHT * 0.55f);
        fm->field("password")->focus();
        tick(4);
        capture("mp-form-keyboard");
        eden_native_debug_set_keyboard_top(-2.0f);
        fm->close(ServerFormMenu::OUT_BACK);
        fm->openProfile(false);
        fm->blurFields();
        tick(4);
        capture("mp-profile");
        fm->close(ServerFormMenu::OUT_BACK);
        sl->select(0);
        sl->debugPassword(false);
        tick(4);
        capture("mp-password");
        GLDialog::dismiss();
        static const char* const kLines[] = {"[Server] Welcome, Sam! (Character Type: 1)",
                                             "Welcome! No griefing. /help for commands"};
        sl->debugConnecting(EDEN_MP_JOIN_RECEIVING, 0.62f, kLines, 2);
        tick(4);
        capture("mp-connecting");
        sl->back();
        sl->debugError(EDEN_MP_ERR_REFUSED, "Nothing is running at castle.example:27015.");
        tick(4);
        capture("mp-error-refused");
        GLDialog::dismiss();
        sl->debugError(EDEN_MP_ERR_INVALID_NAME, "The server refused the name Sam: name taken.");
        tick(4);
        capture("mp-error-name");
        GLDialog::dismiss();
        sl->close();
        m->showservers = FALSE;
        eden_mp_servers_mock(2);   // the empty fixture (never the real servers.txt: --shot may run on real docs)
        m->openServers();
        tick(4);
        capture("mp-list-empty");
        sl->close();
        m->showservers = FALSE;
        eden_mp_servers_mock(0);
        eden_mp_profile_set(keptName, keptChar);
        tick(4);
    }

    // U.5: the New World screen, one shot per world type (-newworld is the first visit's type,
    // Normal; -newworld-<type> for the others). The field is blurred so the shot shows the
    // placeholder rather than a caret.
    if (World::getWorld && World::getWorld->menu) {
        Menu* m = World::getWorld->menu;
        NewWorldMenu* nw = m->newworld;
        m->openNewWorld();
        nw->blurFields();
        tick(4);
        const int first = nw->type();
        capture("newworld");
        for (int t = 0; t < nw->typeCount(); ++t) {
            if (t == first) continue;
            nw->showType(t);
            tick(4);
            char label[48];
            std::snprintf(label, sizeof(label), "newworld-%s", nw->typeShot(t));
            capture(label);
            // H.4: the Classic tab at Original 64 (Scale hidden), then back to the default height.
            CGRect hb;
            if (!std::strcmp(nw->typeShot(t), "classic") && nw->choice("height") == 256 && nw->choiceRect("height", 1, &hb)) {
                ui_tap_rect(hb);
                tick(4);
                capture("newworld-classic-64");
                if (nw->choiceRect("height", 0, &hb)) ui_tap_rect(hb);
                tick(4);
            }
        }
        nw->showType(first);
        nw->back();
        m->shownewworld = FALSE;
        tick(4);
    }

    // The rewritten generic GL settings screen (Phase N Stage 2.5). Meaningful only from the main
    // menu (in game the panel is a host overlay); shot here before any world loads.
    eden_settings_menu_open_now();
    tick(30);
    capture("settings");
    // U.3: every tab, so a row whose control does not fit is visible without a human clicking
    // through the rail. -settings.bmp stays the first tab (Gameplay; its name predates tabs), then
    // -settings-<tab> for the rest, Keys included (since U.4 that is the keybinds list).
    if (World::getWorld && World::getWorld->menu && World::getWorld->menu->settings) {
        SettingsMenu* sm = World::getWorld->menu->settings;
        for (int t = 1; t < sm->tabCount(); ++t) {
            sm->showTab(t);
            tick(4);
            char label[48];
            std::snprintf(label, sizeof(label), "settings-%s", sm->tabName(t));
            capture(label);
        }
        sm->resetView();
    }

    // Stage 5.3's -keybinds shot is gone with U.4: the list is the Keys tab, -settings-keys above.

    eden_settings_menu_close();
    tick(15);

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    capture("hud");

    // The HUD's status line (statusbar.mm, the full-width box). It printed two and a bit copies
    // side by side until Stage 5.6 (Texture2D_web.mm's text-texture cap); this is the artefact.
    // N.5.11: it is the GL kit's toast now, which fades in over 0.2 s of etime — and etime is
    // wall clock here, so a fixed tick count catches it part-way. Wait out the fade in real time.
    World::getWorld->hud->sb->setStatus(@"World Saved", 30);
    {
        const Uint64 t0 = SDL_GetTicks();
        do tick(1); while (SDL_GetTicks() - t0 < 400);
    }
    capture("hud-status");
    World::getWorld->hud->sb->clear();

    // Stage 5.7: the block and colour pickers, opened the way E and C open them (a tap on the
    // HUD's rbuild / rpaint), and closed by the same tap.
    tap_hud(1);
    tick(20);
    capture("picker-blocks");
    // D.2p: page 2, turned the way [ ] and the wheel turn it.
    World::getWorld->hud->pickerPageStep(1);
    tick(10);
    capture("picker-blocks-p2");
    // ...and a page-2 block picked (117, cheese): the HUD's current-block icon and the held block.
    {
        const CGRect c = World::getWorld->hud->blockBounds[5];
        ui_tap(c.origin.x + c.size.width * 0.5f, c.origin.y + c.size.height * 0.5f);
    }
    tick(20);
    capture("hud-newblock");
    // D.3c: SIGN armed (the HUD's current-block button shows its caption), then its prompt.
    if (Hud::pickerToolEnabled(HUD_TOOL_SIGN)) {
        tap_hud(1);
        tick(10);
        const CGRect c = World::getWorld->hud->blockBounds[17];   // H.14: 16 is the flower block
        ui_tap(c.origin.x + c.size.width * 0.5f, c.origin.y + c.size.height * 0.5f);
        tick(20);
        capture("hud-sign");
        tap_hud(1);                    // D.4b: the picker again, SIGN now the selected cell
        tick(20);
        capture("picker-blocks-p2-sign");
        static const char* const kBtn[] = {"OK", "Cancel"};
        GLDialog::prompt("New sign", NULL, "Welcome to my world, traveller. Mind the lava pit past the second door!",
                         95, kBtn, 2, nullptr, 3);
        tick(20);
        capture("sign-prompt");
        GLDialog::dismiss();
        tick(2);
        // D.4b: CMD armed, then the command-block script prompt (multi-line, scrolled to the caret).
        {
            const CGRect c = World::getWorld->hud->blockBounds[18];
            ui_tap(c.origin.x + c.size.width * 0.5f, c.origin.y + c.size.height * 0.5f);
        }
        tick(20);
        capture("hud-cmd");
        GLDialog::prompt("Command block", "Statements separated by ;  Empty clears the script.",
                         "@touch; say Adding...; set ~0 ~2 ~0 gold; wait 1; fill ~-2 ~0 ~-2 ~2 ~3 ~2 glass; "
                         "wait 2; fill ~-2 ~0 ~-2 ~2 ~3 ~2 air; sound door; flash; say Done!",
                         511, kBtn, 2, nullptr, 6);
        tick(20);
        capture("cmd-prompt");
        GLDialog::dismiss();
        tick(2);
        World::getWorld->hud->blocktype = TYPE_STONE;
    }
    tap_hud(2);
    tick(20);
    capture("picker-colors");
    tap_hud(2);
    tick(10);

    // The in-game (ESC) panel, through the same synthetic HUD tap Escape uses. This is the shot
    // that would have caught the suppressed-panel bug (eden_ui_force_legacy) at Stage 2's start
    // instead of at the first play pass.
    eden_tap_hud_button_begin(0);
    tick(6);
    eden_tap_hud_button_end(0);
    tick(30);
    std::printf("[eden-shot] in_menu=%d\n", eden_hud_in_menu());
    capture("pausemenu");

    // Stage 5.5: Settings opened from the pause menu — the GL SettingsMenu as an in-game modal.
    // Set directly, as the pause menu's Settings button does; Back is the screen's own path.
    World::getWorld->menu->settings->resetView();
    World::getWorld->menu->showsettings = TRUE;
    tick(10);
    capture("pausemenu-settings");
    World::getWorld->menu->showsettings = FALSE;
    tick(2);

    // C.3: the Console over the running world -- full (a few lines through the real path), Variables,
    // Help, then the collapsed strip. Opened from play (the pause menu is toggled off and back on).
    eden_tap_hud_button_begin(0); tick(6); eden_tap_hud_button_end(0); tick(10);
    {
        ConsoleMenu* cm = World::getWorld->menu->console;
        eden_console_menu_open();
        tick(5);
        static const char* const kLines[] = { "help set", "where", "set 0 3 0 lamp", "video.fov", "fill 0 0 0 9 2 9 glas", "echo done" };
        for (const char* l : kLines) { cm->setInput(l); cm->submit(); tick(2); }
        tick(10);
        capture("console");
        cm->setInput("fi");
        cm->complete();
        tick(5);
        capture("console-complete");
        cm->showTab(ConsoleMenu::TAB_VARIABLES);
        tick(10);
        capture("console-variables");
        cm->showTab(ConsoleMenu::TAB_HELP);
        tick(10);
        capture("console-help");
        cm->showTab(ConsoleMenu::TAB_CONSOLE);
        cm->setCollapsed(true);
        tick(10);
        capture("console-strip");
        cm->setCollapsed(false);
        // C.4: a pretend on-screen keyboard over the bottom 30% (iPad-like), then 62% (iPhone
        // landscape: the auto strip). The keyboard itself is not drawn; the console's bottom is its top.
        eden_native_debug_set_keyboard_top(SCREEN_HEIGHT * 0.3f);
        tick(10);
        capture("console-keyboard");
        eden_native_debug_set_keyboard_top(SCREEN_HEIGHT * 0.62f);
        tick(10);
        capture("console-keyboard-tall");
        eden_native_debug_set_keyboard_top(-2.0f);
        cm->close();
        tick(5);
    }
    eden_tap_hud_button_begin(0); tick(6); eden_tap_hud_button_end(0); tick(10);

    // The GL modal (Phase N Stage 2.5) — the ESC menu's Home button raises this. Shown directly
    // here rather than hunting the icon's rect; the point is the artefact.
    ::showAlertWarpHome();
    tick(20);
    capture("dialog");

    // N.4.5: the main menu with a world selected (its Properties button, Rename's slot since H.15)
    // and the rename prompt (no longer reached from the list, kept as an artefact of the dialog).
    // H.15: World Properties -- the World tab without a photo, then with camera mode's photo
    // (taken here, in game, by the H.16 path), and the Terrain tab.
    GLDialog::dismiss();
    tick(2);
    {
        const std::string png = std::string(eden_platform_documents_root()) + "/" +
                                cpstring(World::getWorld->terrain->world_name) + ".png";
        std::remove(png.c_str());
    }
    if (quit_to_menu()) {
        tick(60);
        capture("menu-world");
        Menu* m = World::getWorld ? World::getWorld->menu : NULL;
        if (m && m->renameOffered()) {
            m->beginRename();
            tick(10);
            capture("rename");
            GLDialog::dismiss();
            tick(2);
        }
        if (m && m->propertiesOffered()) {
            m->openProperties();
            tick(10);
            capture("props-world-nophoto");
            m->props->back();
            m->showprops = FALSE;
            tick(2);
        }
        // Back in for the photo: H.16's takeScreenshot() writes <world file>.png from the frame.
        const int sel = m ? eden_menu_selected_index() : -1;
        if (sel >= 0 && eden_menu_play() == 1 && tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY")) {
            tick(60);
            // H.16b: the real path: camera mode + take_screenshot reads inside Hud::render, over the
            // frame just drawn (a bare call here would read a presented, undefined back buffer).
            // Through the mouse-click path (eden_click_begin/end), not the F2 flag: a click in camera mode
            // once left camera mode before its release and took no photo.
            World::getWorld->hud->mode = MODE_CAMERA;
            eden_click_begin(1);
            tick(2);
            eden_click_end(1);
            tick(3);
            if (quit_to_menu()) {
                tick(30);
                if (m->propertiesOffered()) {
                    m->openProperties();
                    tick(10);
                    capture("props-world");
                    m->props->showTab(WorldPropertiesMenu::TAB_TERRAIN);
                    tick(5);
                    capture("props-terrain");
                    m->props->back();
                    m->showprops = FALSE;
                    tick(2);
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// --render-scale / --scale-probe  (ROADMAP N.4.11, the native render-resolution setting)
// ---------------------------------------------------------------------------------------------
// The setting is an ENUM row in Settings_web.mm (index into 50/75/100/125%), so a percentage is
// mapped to its option index here rather than written raw. Returns false for a percentage the row
// does not offer, which the caller reports instead of silently rendering at 100%.
extern "C" int eden_settings_count(void);
extern "C" const char* eden_settings_key(int i);
extern "C" void eden_settings_set(int i, float v);
extern "C" int eden_get_render_scale_pct(void);
extern "C" int eden_settings_native_hidden(int i);
bool set_render_scale_pct(int pct) {
    static const int kPct[] = {50, 75, 100, 125};
    int opt = -1;
    for (int j = 0; j < 4; ++j) if (kPct[j] == pct) opt = j;
    if (opt < 0) { std::fprintf(stderr, "[eden-scale] %d%% is not an option (50/75/100/125)\n", pct); return false; }
    for (int i = 0; i < eden_settings_count(); ++i) {
        if (std::strcmp(eden_settings_key(i), "render_scale") == 0) {
            eden_settings_set(i, (float)opt);
            return eden_get_render_scale_pct() == pct;
        }
    }
    return false;
}

// Opens a world, then for each scale: settles, takes a --shot-style capture, and times frames.
// THE TIMING IS CPU+GPU PER FRAME, NOT PRESENT-PACED: swap interval is set to 0 for the probe and
// every frame ends in a 1-pixel glReadPixels, which cannot return until the GPU has finished the
// frame. That is what makes the scales comparable on a device whose present would otherwise
// quantise everything to 16.7 ms steps. The number to read is the median ("p50"); "fps" is
// 1000/p50, i.e. what the frame would allow, not what vsync would show. The capture is the
// on-screen check that the HUD stays sharp while the world softens.
int run_scale_probe() {
    const char* name = g_opt.world.empty() ? "scale-probe" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);                       // let the window fill before timing anything
    SDL_GL_SetSwapInterval(0);
#if defined(EDEN_PLATFORM_IOS)
    const int kScales[] = {100, 75, 50, 100};   // no 125% on iOS (Settings_web.mm, N.4.11b)
#else
    const int kScales[] = {100, 75, 50, 125, 100};
#endif
    const int kTimed = 240;
    for (int pct : kScales) {
        if (!set_render_scale_pct(pct)) return 1;
        tick(30);
        char label[32];
        std::snprintf(label, sizeof(label), "scale%d", pct);
        capture(label);
        std::vector<double> ms;
        ms.reserve(kTimed);
        unsigned char px[4];
        for (int i = 0; i < kTimed; ++i) {
            const Uint64 t0 = SDL_GetTicksNS();
            tick(1);
            eden_gl_glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, px);   // GL_RGBA, GL_UNSIGNED_BYTE
            ms.push_back((double)(SDL_GetTicksNS() - t0) / 1e6);
        }
        std::sort(ms.begin(), ms.end());
        const double p50 = ms[ms.size() / 2], p90 = ms[ms.size() * 9 / 10];
        int dw = 0, dh = 0;
        eden_gl_context_get_drawable_size(&dw, &dh);
        std::printf("[eden-scale] %3d%%  scene %4dx%-4d  p50 %6.2f ms  p90 %6.2f ms  (%5.1f fps)  mem %s\n",
                    pct, dw * pct / 100, dh * pct / 100, p50, p90, 1000.0 / p50, eden_debug_heap());
    }
    SDL_GL_SetSwapInterval(1);
    return 0;
}

// --preset-probe (B.8-measure): the same timing as --scale-probe, but over the graphics presets and
// the view-distance steps B.5a offers. Each step is p50/p90 of CPU+GPU frame time at the spot you
// give with --at; the last step restores Default. Put --shot=PREFIX before it for the captures.
int set_setting_by_key(const char* key, float v) {
    for (int i = 0; i < eden_settings_count(); ++i)
        if (std::strcmp(eden_settings_key(i), key) == 0) { eden_settings_set(i, v); return 1; }
    return 0;
}
int run_preset_probe() {
    const char* name = g_opt.world.empty() ? "preset-probe" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    SDL_GL_SetSwapInterval(0);
    struct Step { const char* label; const char* key; float v; };
    const Step steps[] = {
        {"preset Default", "graphics_preset", 2}, {"preset Low", "graphics_preset", 1},
        {"preset High", "graphics_preset", 3},    {"view Auto", "view_distance", 0},
        {"view 4", "view_distance", 1}, {"view 5", "view_distance", 2}, {"view 6", "view_distance", 3},
        {"view 7", "view_distance", 4}, {"preset Default", "graphics_preset", 2},
    };
    const int kTimed = 240;
    for (const Step& st : steps) {
        if (!set_setting_by_key(st.key, st.v)) { std::fprintf(stderr, "[eden-preset] no setting %s\n", st.key); return 1; }
        tick(30);
        std::vector<double> ms;
        ms.reserve(kTimed);
        unsigned char px[4];
        for (int i = 0; i < kTimed; ++i) {
            const Uint64 t0 = SDL_GetTicksNS();
            tick(1);
            eden_gl_glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
            ms.push_back((double)(SDL_GetTicksNS() - t0) / 1e6);
        }
        std::sort(ms.begin(), ms.end());
        char label[48];
        std::snprintf(label, sizeof(label), "preset-%s-%s", st.key, st.label);
        for (char* c = label; *c; ++c) if (*c == ' ') *c = '-';
        capture(label);
        std::printf("[eden-preset] %-16s p50 %6.2f ms  p90 %6.2f ms  (%5.1f fps)  mem %s\n",
                    st.label, ms[ms.size() / 2], ms[ms.size() * 9 / 10], 1000.0 / ms[ms.size() / 2], eden_debug_heap());
    }
    SDL_GL_SetSwapInterval(1);
    return 0;
}

// --pass-probe[=N] (P.1b): where a frame goes, at the Stage P benchmark poses. Opens a world, and
// per pose teleports, settles (--frames, default 600), then times N frames (default 600) with the
// per-pass timers on (Classes/PassTimers.h) and prints one `[eden-pass] <pose>.passes {...}` line
// (PassAgg::print). Vsync is OFF unless --pass-vsync, so the `present` pass is time blocked in the
// swap -- the GPU proxy: near zero means the GPU kept up with the CPU. --pass-finish adds a
// glFinish at every pass boundary (each pass then includes its own GPU work; an upper bound, and
// the frame total is no longer a real frame). --at/--look replace the pose list with one pose.
int run_pass_probe() {
    const char* name = g_opt.world.empty() ? "pass-probe" : g_opt.world.c_str();
    struct Pose { const char* label; float x, y, z, yaw, pitch; };
    // The bundled map. `home` is the stock harness origin (--stage1's, 43 blocks above a lake), kept
    // so these numbers sit beside every other run's; `forest` and `water` are, of the 16x16 tiles in
    // the window loaded there, the one with the most leaf/trunk blocks (390) and a tile of the
    // 256-column lake, each ~8 blocks above its surface (picked once by a scan, 2026-10-12). All
    // three are inside that first window, so moving between them streams little.
    std::vector<Pose> poses = {
        {"home",   65536.5f, 60.0f, 65536.5f,  45.0f, -10.0f},
        {"forest", 65656.5f, 30.0f, 65448.5f, 135.0f, -15.0f},
        {"water",  65528.5f, 25.0f, 65592.5f,  45.0f, -10.0f},
    };
    if (g_opt.haveAt)
        poses = {{"at", g_opt.at[0], g_opt.at[1], g_opt.at[2], g_opt.haveLook ? g_opt.look[0] : 45.0f,
                  g_opt.haveLook ? g_opt.look[1] : -10.0f}};
    if (!open_world(name, g_opt.height)) return 1;
    {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);   // ES / compatibility only; core says NULL
        std::printf("[eden-pass] gl {\"renderer\":\"%s\",\"version\":\"%s\",\"ext_disjoint_timer_query\":%d}\n",
                    (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION),
                    ext ? (std::strstr(ext, "GL_EXT_disjoint_timer_query") != nullptr) : -1);
        while (glGetError() != GL_NO_ERROR) {}   // a core context answers GL_EXTENSIONS with INVALID_ENUM
    }
    if (!g_opt.passVsync) SDL_GL_SetSwapInterval(0);
    Player* pl = World::getWorld->player;
    int rc = 0;
    for (const Pose& ps : poses) {
        for (int k = 0; k < g_opt.frames; ++k) {
            eden_console_teleport(ps.x, ps.y, ps.z);
            pl->yaw = ps.yaw; pl->pitch = ps.pitch;
            tick(1);
        }
        int batt = -1;
        SDL_GetPowerInfo(nullptr, &batt);
        const int thermal0 = eden_native_thermal_state();
        PassAgg agg;
        g_pass.on = true; g_pass.finish = g_opt.passFinish;
        for (int k = 0; k < g_opt.passFrames; ++k) {
            eden_console_teleport(ps.x, ps.y, ps.z);
            pl->yaw = ps.yaw; pl->pitch = ps.pitch;
            pass_frame_reset();
            const Uint64 t0 = SDL_GetPerformanceCounter();
            tick(1);
            agg.add((double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
        }
        agg.print("eden-pass", ps.label, g_opt.passVsync ? 1 : 0, thermal0, batt);
        g_pass.on = g_pass.finish = false;
        if (!g_opt.headless && PassAgg::q(agg.draws[P_R_CHUNKS], 0.5) == 0) rc = 1;   // a window that drew no terrain
    }
    SDL_GL_SetSwapInterval(1);
    return rc;
}

// Push a real SDL_Event through SDL_PushEvent so it comes back out of SDL_PollEvent and travels
// the SAME path a physical key does — rather than calling eden_native_input_handle_event()
// directly, which would test the translator while skipping the pump that feeds it.
void push_key(SDL_Scancode sc, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.repeat = false;
    SDL_PushEvent(&e);
}
void push_mouse_motion(float xrel, float yrel) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.xrel = xrel;
    e.motion.yrel = yrel;
    SDL_PushEvent(&e);
}
void push_mouse_button(Uint8 button, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = button;
    SDL_PushEvent(&e);
}

struct PlayerState { float x, y, z, yaw, pitch; int hudMode, blocktype; };
PlayerState player_state() {
    PlayerState p{};
    const char* s = eden_debug_player_state();
    const char* k = std::strstr(s, "\"pos\":[");
    if (k) sscanf(k + 7, "%f,%f,%f", &p.x, &p.y, &p.z);
    if ((k = std::strstr(s, "\"yaw\":")))       p.yaw = (float)atof(k + 6);
    if ((k = std::strstr(s, "\"pitch\":")))     p.pitch = (float)atof(k + 8);
    if ((k = std::strstr(s, "\"hud_mode\":")))  p.hudMode = atoi(k + 11);
    if ((k = std::strstr(s, "\"blocktype\":"))) p.blocktype = atoi(k + 12);
    return p;
}

int g_selftestFailures = 0;
const char* g_selftestTag = "eden-input";   // set by whichever selftest is running
void check(bool ok, const char* what, const char* detail) {
    std::printf("[%s] %-46s %s%s%s\n", g_selftestTag, what, ok ? "PASS" : "FAIL",
                detail && *detail ? "  " : "", detail ? detail : "");
    if (!ok) g_selftestFailures++;
}

// I.3: every kit control drawn on a screen of --shot's tour lies inside the safe rect (the screen,
// where there are no insets). Run at a device preset (`--fit-selftest --device=iphone16`). The pause
// menu fits a phone since I.4 (two columns when one does not fit), so it is a plain pass too; the
// overflow bookkeeping below stays as the gate's teeth should the column ever outgrow the screen.
void fit_check(const char* label) {
    float sx = 0, sy = 0, sw = 0, sh = 0;
    eden_safe_rect(&sx, &sy, &sw, &sh);
    char who[400], detail[600], what[96];
    int total = 0;
    // The pause column is drawn under the Settings/dialog screens opened from it, so wherever it
    // overflows, its buttons are excused on every screen but its own.
    std::vector<CGRect> pause;
    bool overflow = false;
    if (World::getWorld && World::getWorld->hud && eden_hud_in_menu() == 1) {
        const Hud* h = World::getWorld->hud;
        const Button* pm[] = { &h->rresume, &h->rsave, &h->rhome, &h->rcam, &h->rsettings, &h->rconsole, &h->rexit };
        for (const Button* b : pm) {
            if (b->size.width <= 0) continue;
            pause.push_back(CGRectMake(b->origin.x, b->origin.y, b->size.width, b->size.height));
            if (b->origin.y < sy || b->origin.y + b->size.height > sy + sh) overflow = true;
        }
    }
    const bool own = !std::strcmp(label, "pausemenu");
    const std::vector<CGRect> none;
    const int bad = GLW::debugControlsOutside(CGRectMake(sx, sy, sw, sh), who, sizeof(who), &total,
                                              overflow && !own ? pause : none);
    std::snprintf(detail, sizeof(detail), "%d of %d outside %.0f,%.0f %.0fx%.0f%s%s", bad, total,
                  sx, sy, sw, sh, bad ? ": " : "", who);
    if (own) {
        std::snprintf(what, sizeof(what), "pausemenu: %s", overflow ? "OVERFLOWS" : "every control inside");
        check(total > 0 && !overflow && bad == 0, what, detail);
        return;
    }
    std::snprintf(what, sizeof(what), "%s: every control inside", label);
    check(total > 0 ? bad == 0 : true, what, detail);
}

int run_fit_selftest() {
    g_selftestTag = "eden-fit";
    g_fitGate = true;
    const int rc = run_shot();
    std::printf("[eden-fit] %s (%d failure(s)) at %.0fx%.0f points, touch floor %.0f\n",
                (rc || g_selftestFailures) ? "FAILURES" : "ALL PASS", g_selftestFailures,
                SCREEN_WIDTH, SCREEN_HEIGHT, GLW::touchFloor());
    return (rc || g_selftestFailures) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --touch-selftest  (Phase N Stage 4.3, the SDL touch translator)
// ---------------------------------------------------------------------------------------------
// Touch is the one input path this port has always had and has never been able to TEST outside a
// browser: the engine half is the original 2010 code, the translator half was `public/eden-input.js`
// only, and a desktop leg has no fingers. SDL_PushEvent does, so this drives the same four events
// UIKit delivers on a device and asserts the effect at the far end.
//
// **IT USES THE ENGINE'S OWN HUD RECT, not a guessed coordinate.** eden_debug_display_state()
// reports `rmenu` (the in-game menu corner icon) in the same point space the touch core consumes,
// so this cannot pass by hitting the wrong thing or fail because a layout constant moved — which
// is exactly the failure mode a hard-coded "tap at 0.9, 0.1" would have.
//
// TWO COORDINATE FACTS, both of which are one-line bugs if they are wrong:
//   * SDL reports finger positions NORMALISED to the window (0..1); the mouse path reports window
//     points. The translator multiplies by the point space directly for that reason.
//   * HUD rects are BOTTOM-LEFT origin and the pointer API is TOP-LEFT (see
//     eden_tap_hud_button_begin, which flips with SCREEN_HEIGHT - cy). The flip belongs to the
//     caller, so this test does it too — and if the translator ever grows its own flip, this test
//     is what catches the double one.
// Defined further down, next to --input-selftest/--gamepad-selftest, which is where the rest of
// the movement harness lives. Declared here so the joystick checks below can reuse it rather than
// grow a second copy — the map-border clamp lesson (see teleport_for_movement) applies to any test
// that asks whether the player moved.
void teleport_for_movement();
void rise_until_clear(int maxFrames);
void check_clear_of_map_border(const char* what);

extern "C" void eden_set_fly_mode(int);
extern "C" void eden_test_force_touch_input(void);   // Settings_web.mm (X.19)
extern "C" void eden_prefs_set_ephemeral(int on);    // NSUserDefaults_native.mm (X.21)
// ---------------------------------------------------------------------------------------------
// --empty-bit-selftest  (Stage R / R.2, the per-chunk empty bit)
// ---------------------------------------------------------------------------------------------
// --stage1 proves the shortcut changes nothing on a world nobody edits. This covers the two
// transitions an edit makes, through the same Terrain::updateChunks entry point a build/mine
// reaches: a block placed in an all-air chunk must clear the bit BEFORE the next mesh decision
// (else rebuild2() skips the scan and the block is invisible), and the chunk must rejoin the
// shortcut once it is air again (rebuild2()'s scan refreshes the bit; setLand never sets it).
int run_empty_bit_selftest() {
    g_selftestTag = "eden-empty";
    // NOT --empty-selfcheck's mode: that runs the full scan on a chunk the bit calls empty, which
    // would mesh the block below even with a stale bit and make the test pass vacuously.
    const char* name = g_opt.world.empty() ? "empty-bit-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    // Top band of the window, a few blocks off the player: air on the bundled map at any height.
    // Off the player's REAL position -- the default --at is clamped to the map corner (see the
    // default-origin comment in eden_main_after_args), so --at itself is not where it lands.
    const PlayerState ps = player_state();
    const int x = (int)ps.x + 5, z = (int)ps.z + 5, y = g_opt.height - 3;
    char d[256];
    auto st = [&] { return eden_debug_chunk_empty(x, z, y); };
    long long s0 = st();
    std::snprintf(d, sizeof(d), "chunk at (%d,%d,%d): probe %lld", x, z, y, s0);
    check(s0 >= 0 && (s0 & 3) == 3, "target chunk is air and marked empty", d);
    if (s0 < 0 || (s0 & 3) != 3) {
        std::printf("[eden-empty] FAILURES (%d failure(s))\n", g_selftestFailures);
        return 1;
    }

    eden_console_setblock(x, z, y, 2 /* TYPE_STONE */);
    long long s1 = st();
    std::snprintf(d, sizeof(d), "probe %lld", s1);
    check((s1 & 3) == 0, "placing a block clears the bit at once", d);
    tick(30);
    long long s2 = st();
    std::snprintf(d, sizeof(d), "getblock %d, vertices %lld -> %lld",
                  eden_console_getblock(x, z, y), s0 >> 2, s2 >> 2);
    check(eden_console_getblock(x, z, y) == 2 && (s2 >> 2) > (s0 >> 2),
          "the placed block meshes (vertices grew)", d);

    eden_console_setblock(x, z, y, 0);
    tick(30);
    long long s3 = st();
    std::snprintf(d, sizeof(d), "probe %lld, vertices %lld", s3, s3 >> 2);
    check((s3 & 3) == 3 && (s3 >> 2) == (s0 >> 2), "removing it re-marks the chunk empty", d);

    const char* cs = eden_debug_chunk_state();
    const char* k1 = std::strstr(cs, "\"stale\":");
    const char* k2 = std::strstr(cs, "\"selfcheck_stale\":");
    check(k1 && atoi(k1 + 8) == 0 && k2 && atoi(k2 + 18) == 0,
          "window audit: no stale bits, self-check clean", cs);

    std::printf("[eden-empty] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --newblocks-selftest  (Stage D / D.2a, ids 112-127 in the engine's tables)
// ---------------------------------------------------------------------------------------------
// Until D.2a, NUM_BLOCKS was 111 and rebuild2()'s corrupt-byte repair turned every 2026 block it
// meshed into stone, in the world, kept by the next save (D.2c). This builds each new id through
// the real build path in air near the player, asserts it stays itself and meshes, saves, reloads
// and asserts every id comes back. Runs on either container (add --world-format=emod).
int run_newblocks_selftest() {
    g_selftestTag = "eden-newblocks";
    const char* name = g_opt.world.empty() ? "newblocks-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);

    // Top band, off the player's REAL position (see --empty-bit-selftest), two apart so every
    // block has all six faces exposed.
    const PlayerState ps = player_state();
    const int x0 = (int)ps.x + 5, z = (int)ps.z + 5, y = g_opt.height - 3;
    char d[256];
    for (int id = 112; id <= 127; id++) {
        const int x = x0 + (id - 112) * 2;
        const long long v0 = eden_debug_chunk_empty(x, z, y) >> 2;
        eden_debug_build_block(x, z, y, id);
        tick(30);
        const long long v1 = eden_debug_chunk_empty(x, z, y) >> 2;
        std::snprintf(d, sizeof(d), "id %d at (%d,%d,%d): getblock %d, vertices %lld -> %lld",
                      id, x, z, y, eden_console_getblock(x, z, y), v0, v1);
        check(eden_console_getblock(x, z, y) == id && v1 > v0, "a new id builds, stays itself and meshes", d);
    }
    save_world();
    if (!quit_to_menu()) return 1;
    tick(120);
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    int back = 0;
    for (int id = 112; id <= 127; id++) {
        const int x = x0 + (id - 112) * 2;
        const int got = eden_console_getblock(x, z, y);
        const long long v = eden_debug_chunk_empty(x, z, y) >> 2;
        std::snprintf(d, sizeof(d), "id %d at (%d,%d,%d): getblock %d, chunk vertices %lld", id, x, z, y, got, v);
        check(got == id && v > 0, "after save + reload the id is still there and meshes", d);
        back += (got == id);
    }
    std::printf("[eden-newblocks] %d/16 ids back after reload\n", back);
    std::printf("[eden-newblocks] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --trailer-selftest  (Stage D / D.3a, the trailer model on both containers)
// ---------------------------------------------------------------------------------------------
namespace trailer_test {

bool is_emod_file(const std::string& path) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
    if (!f) return false;
    char m[8] = {0};
    const bool ok = std::fread(m, 1, 8, f) == 8 && std::memcmp(m, "EMODWLD", 8) == 0;
    std::fclose(f);
    return ok;
}

// The trailer exactly as the file holds it: a `.eden`'s gate-failing directory tail (the engine's own
// layout reader) or an `.emod`'s live SIGN_TRAILER record. `cols` gets one digest over every column.
bool read_world(const std::string& path, std::vector<uint8_t>& trailer, std::string* colDigest, std::string& err) {
    trailer.clear();
    emod::Sha256 h;
    if (is_emod_file(path)) {
        emod::EdenWorldStore st;
        if (!st.open(path.c_str(), false)) { err = st.error(); return false; }
        if (st.hasRecord(emod::T_SIGN_TRAILER) && st.readRecord(emod::T_SIGN_TRAILER, trailer) != emod::EdenWorldStore::READ_OK) {
            err = "SIGN_TRAILER does not decode"; return false;
        }
        if (colDigest) {
            std::vector<uint8_t> col((size_t)st.bands() * emod::BAND);
            for (const auto& k : st.columnKeys()) {
                st.readColumn(k.first, k.second, &col[0]);
                h.update(&k, sizeof(k));
                h.update(&col[0], col.size());
            }
        }
    } else {
        FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
        if (!f) { err = "cannot open"; return false; }
        const uint64_t size = emod::io::file_size(f);
        uint8_t hdr[emod::EDEN_HEADER];
        emod::io::seek_to(f, 0);
        if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fclose(f); err = "short header"; return false; }
        uint64_t dirOff; std::memcpy(&dirOff, hdr + 32, 8);
        if (dirOff < sizeof(hdr) || dirOff > size) { std::fclose(f); err = "bad directory offset"; return false; }
        std::vector<uint8_t> dir((size_t)(size - dirOff));
        emod::io::seek_to(f, dirOff);
        if (!dir.empty() && std::fread(&dir[0], 1, dir.size(), f) != dir.size()) { std::fclose(f); err = "short directory"; return false; }
        emod::EdenLayout L;
        if (!emod::eden_read_layout(hdr, size, dir, L)) { std::fclose(f); err = "layout"; return false; }
        trailer = L.trailer;
        if (colDigest) {
            std::vector<emod::EdenLayoutCol> cols = L.cols;
            std::sort(cols.begin(), cols.end(), [](const emod::EdenLayoutCol& a, const emod::EdenLayoutCol& b) {
                return a.x != b.x ? a.x < b.x : a.z < b.z; });
            std::vector<uint8_t> col((size_t)L.bands * emod::BAND);
            for (const auto& c : cols) {
                std::fill(col.begin(), col.end(), 0);
                emod::io::seek_to(f, c.off);
                std::fread(&col[0], 1, (size_t)std::min<uint64_t>(c.span, col.size()), f);
                int32_t k[2] = {c.x, c.z};
                h.update(k, sizeof(k));
                h.update(&col[0], col.size());
            }
        }
        std::fclose(f);
    }
    if (colDigest) {
        uint8_t d[32]; h.final(d);
        char hex[65];
        for (int i = 0; i < 32; i++) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
        *colDigest = hex;
    }
    return true;
}

std::vector<uint8_t> rows_of(const std::vector<uint8_t>& payload) {   // tag a payload into rows
    std::vector<uint8_t> p = payload, out;
    p.resize((p.size() + 11) / 12 * 12, 0);
    for (size_t i = 0; i < p.size(); i += 12) {
        static const uint8_t tag[4] = {0xff, 0xff, 0xff, 0xff};
        out.insert(out.end(), tag, tag + 4);
        out.insert(out.end(), p.begin() + i, p.begin() + i + 12);
    }
    return out;
}

void synthetic() {
    char d[256];
    // 1. a new world: CMB1 goes in front of SGN1 whatever order the edits come in; empty = no trailer
    WorldTrailer t;
    check(t.parsed() && !t.dirty() && t.encode().empty(), "a cleared trailer is empty, editable and clean", "");
    const TrailerPos p0 = TrailerPos::fromTerrainArgs(65600, 65610, 40);
    check(t.addSign(p0, 3, 45, 2, "hello") == WorldTrailer::TR_OK, "addSign on a new world", "");
    check(t.addCmd(p0, "@touch;say hi") == WorldTrailer::TR_OK, "addCmd on a new world", "");
    std::vector<uint8_t> e = t.encode();
    check(t.dirty() && e.size() == (size_t)(24 + 528 + 24 + 120) / 12 * 16 && e.size() % 16 == 0,
          "a new world's trailer is CMB1 + SGN1 rows", "");
    check(e.size() > 8 && std::memcmp(&e[4], "CMB1", 4) == 0, "CMB1 is written first (the 2026 game's order)", "");
    WorldTrailer r;
    check(r.load(e.data(), e.size()) && r.signCount() == 1 && r.cmdCount() == 1 && r.rebuild() == e,
          "it reads back, and re-serialises byte-identical", r.describe().c_str());
    // 2. the coordinate helper: file (x,y,z) = Terrain (x,z,y) = engine (x, y_up=z, z=y)
    int ex, ey, ez; p0.toEngine(&ex, &ey, &ez);
    TrailerPos back = TrailerPos::fromEngine(ex, ey, ez);
    std::snprintf(d, sizeof(d), "engine (%d,%d,%d)", ex, ey, ez);
    check(ex == 65600 && ey == 40 && ez == 65610 && back == p0, "file <-> engine coordinates", d);
    // 3. the index, and the anchor-removal hook
    check(t.hasAnchored(p0) && !t.hasAnchored(TrailerPos::fromTerrainArgs(65600, 65610, 41)), "hasAnchored is exact", "");
    check(t.addSign(p0, 3, 45, 2, "again") == WorldTrailer::TR_EXISTS && t.addSign(p0, 1, 45, 2, "side") == WorldTrailer::TR_OK,
          "one sign per (anchor, face)", "");
    check(t.addSign(p0, 6, 45, 2, "x") == WorldTrailer::TR_BAD && t.addSign(p0, 0, 0, 2, "x") == WorldTrailer::TR_BAD &&
          t.addSign(p0, 0, 45, 2, "tab\there") == WorldTrailer::TR_BAD &&
          t.addSign(p0, 0, 45, 2, std::string(96, 'x').c_str()) == WorldTrailer::TR_BAD &&
          t.addSign(p0, 0, 45, 2, std::string(95, 'x').c_str()) == WorldTrailer::TR_OK,
          "face/colour/text validated (95 printable ASCII fits, 96 does not)", "");
    std::snprintf(d, sizeof(d), "removed %d", t.removeAnchored(p0));
    check(t.signCount() == 0 && t.cmdCount() == 0 && t.encode().empty(), "removeAnchored takes every sign and the command block", d);
    // 4. an edit that empties a section drops it; the untouched section stays as read; markSaved holds
    WorldTrailer m;
    m.load(e.data(), e.size());
    m.removeCmd(0);
    std::vector<uint8_t> me = m.encode();
    check(me.size() == (size_t)(24 + 120) / 12 * 16 && std::memcmp(&me[4], "SGN1", 4) == 0, "an emptied CMB1 is left out", "");
    m.markSaved();
    check(!m.dirty() && m.rebuild() == me && m.encode() == me, "after markSaved the model rebuilds to what was written", "");
    // 5. opaque: kept verbatim, every edit refused
    std::vector<uint8_t> bad = e;
    bad[16] = 0;                                    // the second row's tag
    WorldTrailer o;
    check(!o.load(bad.data(), bad.size()) && o.rebuild() == bad && o.encode() == bad && !o.dirty(),
          "an unreadable trailer is opaque and kept byte for byte", o.why().c_str());
    check(o.addSign(p0, 3, 45, 0, "x") == WorldTrailer::TR_OPAQUE && o.removeAnchored(p0) == 0 && o.encode() == bad,
          "an opaque trailer refuses every edit", "");
    // an unknown section is carried verbatim in its slot through an edit
    std::vector<uint8_t> pay;
    const char unk[] = "XYZ1";
    pay.insert(pay.end(), unk, unk + 4);
    uint32_t w[2] = {12, 0}; pay.insert(pay.end(), (uint8_t*)w, (uint8_t*)w + 8);
    for (int i = 0; i < 12; i++) pay.push_back((uint8_t)(0xa0 + i));
    std::vector<uint8_t> ep;
    for (size_t i = 0; i < e.size(); i += 16) ep.insert(ep.end(), e.begin() + i + 4, e.begin() + i + 16);
    pay.insert(pay.end(), ep.begin(), ep.end());
    pay.insert(pay.end(), 24, 0);                   // a zero tail
    std::vector<uint8_t> mixed = rows_of(pay);
    WorldTrailer u;
    check(u.load(mixed.data(), mixed.size()) && u.rebuild() == mixed, "unknown section + zero tail: parse -> serialise identical",
          u.describe().c_str());
    u.setSignText(0, "edited");
    std::vector<uint8_t> ue = u.rebuild();
    size_t diff = 0;
    for (size_t i = 0; i < std::min(ue.size(), mixed.size()); i++) diff += ue[i] != mixed[i];
    std::snprintf(d, sizeof(d), "%zu B differ, sizes %zu/%zu", diff, ue.size(), mixed.size());
    check(ue.size() == mixed.size() && diff > 0 && diff <= 6, "an edit changes only that record's text bytes", d);
    // 6. sidecars: a bare section, adopted only when the trailer has none of that magic
    std::vector<uint8_t> side;
    const char sg[] = "SGN1"; side.insert(side.end(), sg, sg + 4);
    uint32_t vc[2] = {1, 2}; side.insert(side.end(), (uint8_t*)vc, (uint8_t*)vc + 8);
    TrailerSign s2[2]; std::memset(s2, 0, sizeof(s2));
    s2[0].x = 1; s2[0].a = 3; s2[0].b = 2; std::strcpy(s2[0].text, "one");
    s2[1].x = 2; s2[1].a = 5; s2[1].b = 2; std::strcpy(s2[1].text, "two");
    side.insert(side.end(), (uint8_t*)s2, (uint8_t*)s2 + sizeof(s2));
    WorldTrailer sc;
    check(sc.adoptSidecar(side.data(), side.size()) == WorldTrailer::TR_OK && sc.signCount() == 2 && sc.dirty(),
          "a sidecar is adopted into an empty trailer (dirty)", sc.describe().c_str());
    WorldTrailer sc2; sc2.load(e.data(), e.size());
    check(sc2.adoptSidecar(side.data(), side.size()) == WorldTrailer::TR_EXISTS && sc2.signCount() == 1 && !sc2.dirty(),
          "the world's own SGN1 wins over a sidecar", "");
    // 7. the 1 MiB refusal: (786,432 payload bytes - 24) / 120 = 6,553 signs fit, the next is refused
    WorldTrailer full;
    int n = 0;
    WorldTrailer::Result rr = WorldTrailer::TR_OK;
    for (; n < 10000; n++) {
        rr = full.addSign(TrailerPos::fromTerrainArgs(60000 + n % 100, 60000 + n / 100, 10), 3, 45, 0, "x");
        if (rr != WorldTrailer::TR_OK) break;
    }
    const std::vector<uint8_t> before = full.encode();
    const WorldTrailer::Result again = full.addSign(TrailerPos::fromTerrainArgs(1, 1, 1), 3, 45, 0, "x");
    const WorldTrailer::Result cmdFull = full.addCmd(TrailerPos::fromTerrainArgs(1, 1, 1), "@touch");
    std::snprintf(d, sizeof(d), "%d signs, %zu B, refused with %d/%d/%d", n, before.size(), (int)rr, (int)again, (int)cmdFull);
    check(n == 6553 && rr == WorldTrailer::TR_FULL && again == WorldTrailer::TR_FULL && cmdFull == WorldTrailer::TR_FULL &&
          before.size() <= WorldTrailer::MAX_BYTES && full.encode() == before && full.signCount() == 6553,
          "past 1 MiB a new sign / command block is refused, nothing truncated", d);
}

}  // namespace trailer_test

// --photo-selftest (H.16): round-trips a gradient through Util.mm's PNG encoder and a zlib decode.
int run_photo_selftest() {
    const int w = 301, h = 200;
    std::vector<unsigned char> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned char* p = &rgb[((size_t)y * w + x) * 3];
        p[0] = (unsigned char)x; p[1] = (unsigned char)y; p[2] = (unsigned char)(x ^ y);
    }
    std::vector<unsigned char> png;
    int bad = 0;
    auto chk = [&](bool ok, const char* what) { std::printf("[eden-photo] %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) bad++; };
    chk(photoEncodePNG(rgb.data(), w, h, png), "encode");
    auto be32 = [&](size_t o) { return ((unsigned)png[o] << 24) | (png[o+1] << 16) | (png[o+2] << 8) | png[o+3]; };
    static const unsigned char sig[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    chk(png.size() > 33 && !std::memcmp(png.data(), sig, 8), "signature");
    chk(png.size() > 33 && be32(16) == (unsigned)w && be32(20) == (unsigned)h && png[24] == 8 && png[25] == 2, "IHDR 301x200 8-bit RGB");
    // Walk the chunks: check each CRC, gather IDAT.
    std::vector<unsigned char> idat; bool crcOk = true, iend = false;
    for (size_t o = 8; o + 12 <= png.size();) {
        unsigned n = be32(o);
        if (o + 12 + n > png.size()) { crcOk = false; break; }
        uLong c = crc32(crc32(0L, Z_NULL, 0), &png[o + 4], 4 + n);
        if ((unsigned)c != be32(o + 8 + n)) crcOk = false;
        if (!std::memcmp(&png[o + 4], "IDAT", 4)) idat.insert(idat.end(), &png[o + 8], &png[o + 8] + n);
        if (!std::memcmp(&png[o + 4], "IEND", 4)) iend = true;
        o += 12 + n;
    }
    chk(crcOk && iend, "chunk CRCs and IEND");
    std::vector<unsigned char> raw((size_t)(w * 3 + 1) * h);
    uLongf rn = (uLongf)raw.size();
    bool inf = uncompress(raw.data(), &rn, idat.data(), (uLong)idat.size()) == Z_OK && rn == raw.size();
    chk(inf, "IDAT inflates to h*(1+3w) bytes");
    bool same = inf;
    for (int y = 0; same && y < h; y++)
        same = raw[(size_t)y * (w * 3 + 1)] == 0 && !std::memcmp(&raw[(size_t)y * (w * 3 + 1) + 1], &rgb[(size_t)y * w * 3], (size_t)w * 3);
    chk(same, "pixels round-trip, not one flat colour");
    std::printf("[eden-photo] %s\n", bad ? "FAILED" : "ALL PASS");
    return bad ? 1 : 0;
}

int run_trailer_selftest() {
    using namespace trailer_test;
    g_selftestTag = "eden-trailer";
    char d[512];
    synthetic();

    // Gate 1: every real specimen parses and re-serialises byte-identical.
    for (const std::string& f : g_opt.trailerFiles) {
        std::vector<uint8_t> t;
        std::string err;
        const bool ok = read_world(f, t, nullptr, err);
        WorldTrailer w;
        const bool parsed = ok && w.load(t.data(), t.size());
        std::snprintf(d, sizeof(d), "%s: %zu B, %s", f.c_str(), t.size(), ok ? w.describe().c_str() : err.c_str());
        check(parsed && !t.empty() && w.rebuild() == t, "a real trailer: parse -> serialise byte-identical", d);
        std::printf("[eden-trailer] %s\n", d);
    }

    if (!g_opt.world.empty()) {
        // Gates 2 + 3: one sign's text edited in play, saved with zero dirty columns, reloaded.
        WorldTrailer::testNoDirtyMark = g_opt.trailerNoDirty;
        if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
        tick(60);
        WorldTrailer* t = (WorldTrailer*)eden_debug_world_trailer();
        const std::string path = eden_debug_world_file();
        std::printf("[eden-trailer] %s: %s (%s)\n", path.c_str(), t ? t->describe().c_str() : "no trailer", eden_debug_world_format());
        if (!t || !t->parsed() || t->signCount() == 0) {
            check(false, "the world has a readable trailer with signs", path.c_str());
            std::printf("[eden-trailer] FAILURES (%d failure(s))\n", g_selftestFailures);
            return 1;
        }
        std::vector<uint8_t> disk0;
        std::string cols0, err;
        check(read_world(path, disk0, &cols0, err), "read the world back from disk", err.c_str());
        if (t->dirty()) {
            // Adopted sidecars: the first save must write them in.
            std::printf("[eden-trailer] sidecars adopted at load: %s\n", t->describe().c_str());
            const std::vector<uint8_t> want = t->encode();
            save_world();
            std::vector<uint8_t> disk;
            read_world(path, disk, nullptr, err);
            check(!t->dirty() && disk == want, "adopted sidecars are written into the world by the first save", "");
            disk0 = disk;
        }
        check(t->encode() == disk0, "the model holds what the file holds", "");
        const std::string old0 = t->sign(0).text;
        WorldTrailer scratch;
        scratch.load(disk0.data(), disk0.size());
        scratch.setSignText(0, "D3A edited sign");
        const std::vector<uint8_t> want = scratch.rebuild();
        size_t diff = 0;
        for (size_t i = 0; i < std::min(want.size(), disk0.size()); i++) diff += want[i] != disk0[i];

        const int dirtyChunks = eden_debug_modified_chunks();
        std::snprintf(d, sizeof(d), "%d modified chunks", dirtyChunks);
        check(dirtyChunks == 0, "no column is dirty before the save (the trailer alone must force it)", d);
        check(t->setSignText(0, "D3A edited sign") == WorldTrailer::TR_OK, "edit sign 0's text in play", old0.c_str());
        const std::string toast = eden_debug_trailer_refusal_toast(WorldTrailer::TR_FULL);
        check(toast == WorldTrailer::resultMessage(WorldTrailer::TR_FULL), "a refusal shows its toast", toast.c_str());
        save_world();
        std::vector<uint8_t> disk1;
        std::string cols1;
        read_world(path, disk1, &cols1, err);
        std::snprintf(d, sizeof(d), "disk %zu B (was %zu), %zu B of the text differ, dirty %d", disk1.size(), disk0.size(), diff,
                      t->dirty() ? 1 : 0);
        check(disk1 == want && want.size() == disk0.size() && diff > 0 && diff <= 96,
              "a zero-dirty-column save writes the edited trailer, only that sign's text changed", d);
        check(cols1 == cols0, "the save left every column byte-identical", cols1.c_str());

        if (!quit_to_menu()) return 1;
        tick(60);
        if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
        tick(60);
        t = (WorldTrailer*)eden_debug_world_trailer();
        std::vector<uint8_t> disk2;
        std::string cols2;
        read_world(path, disk2, &cols2, err);
        check(t && t->encode() == want && !t->dirty() && std::strcmp(t->sign(0).text, "D3A edited sign") == 0 &&
              disk2 == want && cols2 == cols0, "after reload: the edit is there, nothing else changed", t ? t->describe().c_str() : "");

        // Remove every record: no trailer at all, on both containers.
        if (g_opt.trailerKeep) t = nullptr;
        while (t && t->signCount()) t->removeSign(t->signCount() - 1);
        while (t && t->cmdCount()) t->removeCmd(t->cmdCount() - 1);
        save_world();
        std::vector<uint8_t> disk3;
        read_world(path, disk3, nullptr, err);
        std::snprintf(d, sizeof(d), "%zu B on disk", disk3.size());
        if (!g_opt.trailerKeep) {
        check(t && t->encode().empty() && disk3.empty(), "removing every record leaves no trailer", d);
        if (!quit_to_menu()) return 1;
        tick(60);
        if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
        tick(30);
        t = (WorldTrailer*)eden_debug_world_trailer();
        check(t && t->signCount() == 0 && t->cmdCount() == 0 && t->encode().empty(), "...and it reloads empty",
              t ? t->describe().c_str() : "");
        }
    }
    std::printf("[eden-trailer] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --classic-selftest  (Stage H / H.4, the seeded "Classic" generator)
// ---------------------------------------------------------------------------------------------
// generator-plan.md §7, gates 1, 2, 3 and 6. Pure-function legs first, then the engine's load
// and save path in --docs:
//   1. 1.x exactness: every column of --classic-fixture (the 159 unmodified columns of a Sept 2011
//      world, port/tools/fixtures/classic/) regenerates byte for byte. Trees, leaves, flowers and
//      clouds over predicted air, and the grass kinds, are wildcards (h3-tools/cmp.py's rule).
//      --classic-mutate=tables|trunc must FAIL it (an earlier random() caller; truncation).
//   2. Cross-target hashes: SHA-256 (first 16 hex) over 256 decorated columns per profile, pinned below and
//      identical on every target. A mismatch is a stop; fp contraction is the first suspect.
//   3. Seams: one 64x64 window equals the 16 columns it covers generated one at a time (the new
//      profile's cross-border trees; the 2x/4x lattice). --classic-mutate=noborder must FAIL it.
//   4. The GENERATOR record and its `.eden` mirror: round trip; unknown revisions refused.
//   5. Engine: a 1.x world loads legacy (caves detected off), its unsaved columns are the pure
//      function's, and it writes GENERATOR once; an exported `.eden` carries the mirror; a mirror
//      in the header is honoured; an unknown record loads void and saves nothing.
namespace classic_test {

// Recorded on macOS arm64 (2026-10-15). Every target must print the same: these are the bits of
// the baked tables, the 1.x float order and -ffp-contract=off. Re-pin only for a deliberate
// generator change (a new revision), in the same commit.
const char* const kPins[][2] = {
    {"legacy",       "b822c5c1cece9f0b"},
    {"new1x",        "63ba029b591b0787"},
    {"4x-2011",      "b189472315ffee9d"},
    {"2x-deep-wild", "b43e84546435f737"},
};

struct Fixture { int seed = 0, height = 0; std::vector<std::pair<int, int> > keys; std::vector<uint8_t> types; };

bool load_fixture(const std::string& path, Fixture& fx, std::string& err) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    int n;
    while ((n = gzread(f, buf, sizeof buf)) > 0) b.insert(b.end(), buf, buf + n);
    gzclose(f);
    if (b.size() < 20 || std::memcmp(&b[0], "EMCLFX1", 8) != 0) { err = "not a classic fixture"; return false; }
    int32_t h[3];
    std::memcpy(h, &b[8], 12);
    fx.seed = h[0]; fx.height = h[1];
    const size_t col = 256u * (size_t)fx.height;
    if (fx.height != 64 || b.size() != 20 + (size_t)h[2] * (8 + col)) { err = "fixture size mismatch"; return false; }
    for (int i = 0; i < h[2]; ++i) {
        const uint8_t* p = &b[20 + (size_t)i * (8 + col)];
        int32_t k[2];
        std::memcpy(k, p, 8);
        fx.keys.push_back(std::make_pair(k[0], k[1]));
        fx.types.insert(fx.types.end(), p + 8, p + 8 + col);
    }
    return true;
}

std::string hex16(emod::Sha256& h) {
    uint8_t d[32];
    h.final(d);
    char s[17];
    for (int i = 0; i < 8; ++i) std::snprintf(s + 2 * i, 3, "%02x", d[i]);
    return s;
}

struct Profile { const char* name; ClassicGen g; int height; };
std::vector<Profile> profiles() {
    std::vector<Profile> v;
    Profile p;
    p.name = "legacy"; classic_legacy_params(&p.g, 80866); p.height = 64; v.push_back(p);
    p.name = "new1x"; classic_default_params(&p.g, 1234567); p.g.caves = CLASSIC_CAVES_2011; p.g.trees = 3;
    p.height = 64; v.push_back(p);
    p.name = "4x-2011"; classic_default_params(&p.g, 777); p.g.scale4 = 16; p.g.caves = CLASSIC_CAVES_2011;
    p.height = 256; v.push_back(p);
    p.name = "2x-deep-wild"; classic_default_params(&p.g, 4242); p.g.scale4 = 8; p.g.hilliness = 4;
    p.g.caves = CLASSIC_CAVES_DEEP; p.g.trees = 1; p.height = 256; v.push_back(p);
    return v;
}

// Walks an .emod's framed records (docs/emod-file-format.md: 64-byte file header, then 32-byte
// record headers "EmR1", type @4, payload length @20, payloads padded to 8) and counts one type.
int count_records(const std::string& path, int type) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
    if (!f) return -1;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    int count = 0;
    for (size_t off = emod::FILE_HEADER; off + emod::REC_HEADER <= b.size();) {
        if (std::memcmp(&b[off], "EmR1", 4) != 0) return -2;
        uint32_t plen;
        std::memcpy(&plen, &b[off + 20], 4);
        count += b[off + 4] == type;
        off += emod::REC_HEADER + ((plen + 7) & ~7u);
    }
    return count;
}

// A world file in --docs: header (seed, home/pos at block x,z), 200 empty creature slots, the given
// columns (64z, types (x*16+z)*64+y, paint 0) and optionally a GENERATOR record.
bool write_world(const std::string& path, const char* name, int seed, int hx, int hz,
                 const Fixture* fx, const uint8_t* genRecord, const uint8_t* mirror, std::string& err) {
    emod::io::remove_file(path);
    emod::EdenWorldStore st;
    if (!st.create(path.c_str(), 4, 0)) { err = st.error(); return false; }
    st.beginBatch();
    WorldFileHeader h;
    std::memset(&h, 0, sizeof h);
    h.level_seed = seed;
    h.pos.x = h.home.x = (float)hx; h.pos.z = h.home.z = (float)hz; h.pos.y = h.home.y = 60;
    std::snprintf(h.name, sizeof h.name, "%s", name);
    h.version = 4;
    h.goldencubes = 10;
    for (int i = 0; i < 16; ++i) h.skycolors[i] = COLOR_NORMAL_BLUE;
    if (mirror) std::memcpy(h.reserved, mirror, 36);
    std::vector<uint8_t> cr(200 * sizeof(EntityData), 0);
    for (size_t o = 0; o < cr.size(); o += sizeof(EntityData)) { int32_t t = -1; std::memcpy(&cr[o + 28], &t, 4); }
    st.putRecord(emod::T_WORLD_HEADER, &h, sizeof h);
    st.putRecord(emod::T_CREATURES, &cr[0], cr.size());
    if (genRecord && !st.putRecord(emod::T_GENERATOR, genRecord, CLASSIC_RECORD_BYTES)) { err = st.error(); return false; }
    if (fx) {
        std::vector<uint8_t> col(4 * emod::BAND, 0);
        for (size_t i = 0; i < fx->keys.size(); ++i) {
            std::fill(col.begin(), col.end(), 0);
            const uint8_t* t = &fx->types[i * 256 * 64];
            for (int xz = 0; xz < 256; ++xz)
                for (int y = 0; y < 64; ++y) col[(size_t)(y / 16) * emod::BAND + xz * 16 + y % 16] = t[xz * 64 + y];
            if (!st.putColumn(fx->keys[i].first, fx->keys[i].second, &col[0])) { err = st.error(); return false; }
        }
    }
    if (!st.commitBatch()) { err = st.error(); return false; }
    st.close();
    return true;
}

bool play_file(const char* file) {
    World::getWorld->menu->loadWorlds();
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_file(i), file) == 0) idx = i;
    if (idx < 0) { std::fprintf(stderr, "[eden-classic] %s is not listed\n", file); return false; }
    eden_menu_select(idx);
    if (eden_menu_play() != 1) { std::fprintf(stderr, "[eden-classic] menu_play refused %s\n", file); return false; }
    return tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
}

// Every cell of resident column (cx,cz) against the pure function; returns mismatches (-1: not resident).
int column_vs_pure(const ClassicGen& g, int cx, int cz, int height) {
    if (!isColumnResident(cx * 16, cz * 16)) return -1;
    std::vector<block8> t((size_t)256 * height);
    std::vector<color8> c((size_t)256 * height);
    classic_generate_window(g, cx * 16, cz * 16, 16, 16, height, &t[0], &c[0], true);
    int bad = 0;
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
            for (int y = 0; y < height; ++y)
                bad += getLandc(cx * 16 + x, cz * 16 + z, y) != t[(size_t)(x * 16 + z) * height + y];
    return bad;
}

}  // namespace classic_test

int run_classic_selftest() {
    using namespace classic_test;
    g_selftestTag = "eden-classic";
    char d[512];
    const std::string& mut = g_opt.classicMutate;
    classic_set_mutation(mut == "tables" ? 1 : mut == "trunc" ? 2 : mut == "noborder" ? 3 : 0);

    // ---- 1. 1.x exactness
    Fixture fx;
    std::string err;
    const bool haveFx = load_fixture(g_opt.classicFixture, fx, err);
    check(haveFx, "the 1.x fixture loads", haveFx ? g_opt.classicFixture.c_str() : err.c_str());
    if (haveFx) {
        std::vector<block8> t(256 * 64);
        std::vector<color8> c(256 * 64);
        ClassicGen g;
        classic_legacy_params(&g, fx.seed);
        int exact = 0;
        long cells = 0, wrong = 0;
        for (size_t i = 0; i < fx.keys.size(); ++i) {
            classic_generate_window(g, fx.keys[i].first * 16, fx.keys[i].second * 16, 16, 16, 64, &t[0], &c[0], false);
            const uint8_t* a = &fx.types[i * 256 * 64];
            long w = 0;
            for (int k = 0; k < 256 * 64; ++k) {
                int av = (int8_t)a[k], ev = t[k];
                if (av == 8 || av == 11 || av == 12) av = 3;
                if (ev == 0 && (av == 0 || av == 5 || av == 6 || av == 19 || av == 73)) continue;
                ++cells;
                w += av != ev;
            }
            wrong += w;
            exact += w == 0;
        }
        std::snprintf(d, sizeof d, "seed %d: %d of %zu columns exact, %ld of %ld cells differ",
                      fx.seed, exact, fx.keys.size(), wrong, cells);
        check(exact == (int)fx.keys.size() && cells > 0, "1.x columns regenerate byte for byte", d);
    }

    // ---- 2. hashes
    std::vector<Profile> ps = profiles();
    for (size_t pi = 0; pi < ps.size(); ++pi) {
        const Profile& p = ps[pi];
        std::vector<block8> t((size_t)256 * p.height);
        std::vector<color8> c((size_t)256 * p.height);
        emod::Sha256 h;
        Uint64 ns = 0;
        for (int cx = 4088; cx < 4104; ++cx)
            for (int cz = 4088; cz < 4104; ++cz) {
                const Uint64 t0 = SDL_GetTicksNS();
                classic_generate_window(p.g, cx * 16, cz * 16, 16, 16, p.height, &t[0], &c[0], true);
                ns += SDL_GetTicksNS() - t0;
                h.update(&t[0], t.size());
                h.update(&c[0], c.size());
            }
        const double ms = ns / 1e6 / 256;
        const std::string got = hex16(h);
        std::printf("[eden-classic] hash %s %s (%.3f ms/column)\n", p.name, got.c_str(), ms);
        std::snprintf(d, sizeof d, "%s: got %s, pinned %s", p.name, got.c_str(), kPins[pi][1]);
        check(got == kPins[pi][1], "profile hash matches its pin", d);
    }

    // ---- 3. seams
    for (size_t pi = 1; pi < ps.size(); ++pi) {
        const Profile& p = ps[pi];
        const int H = p.height, X0 = 4096 * 16, Z0 = 4097 * 16;
        std::vector<block8> wt((size_t)64 * 64 * H), t((size_t)256 * H);
        std::vector<color8> wc((size_t)64 * 64 * H), c((size_t)256 * H);
        classic_generate_window(p.g, X0, Z0, 64, 64, H, &wt[0], &wc[0], true);
        long bad = 0, trees = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                classic_generate_window(p.g, X0 + i * 16, Z0 + j * 16, 16, 16, H, &t[0], &c[0], true);
                for (int x = 0; x < 16; ++x)
                    for (int z = 0; z < 16; ++z)
                        for (int y = 0; y < H; ++y) {
                            const size_t a = ((size_t)(i * 16 + x) * 64 + (j * 16 + z)) * H + y, b = ((size_t)x * 16 + z) * H + y;
                            bad += wt[a] != t[b] || wc[a] != c[b];
                            trees += t[b] == TYPE_TREE;
                        }
            }
        std::snprintf(d, sizeof d, "%s: %ld cells differ (%ld trunk cells)", p.name, bad, trees);
        check(bad == 0 && trees > 0, "a 64x64 window equals its 16 columns", d);
    }
    if (!mut.empty()) {
        std::printf("[eden-classic] %s (%d failure(s)) [mutation %s]\n", g_selftestFailures ? "FAILURES" : "ALL PASS",
                    g_selftestFailures, mut.c_str());
        return g_selftestFailures ? 1 : 0;
    }

    // ---- 4. the record
    {
        ClassicGen a = ps[2].g, b;
        a.flags = 1;
        uint8_t r[CLASSIC_RECORD_BYTES], m[36];
        classic_encode_record(a, r);
        bool ok = classic_decode_record(r, sizeof r, &b) && std::memcmp(&a, &b, sizeof a) == 0;
        classic_encode_mirror(a, m);
        ClassicGen mb;
        ok = ok && classic_decode_mirror(m, &mb) && std::memcmp(&a, &mb, sizeof a) == 0;
        check(ok, "GENERATOR record and mirror round-trip", "");
        uint8_t r2[CLASSIC_RECORD_BYTES];
        std::memcpy(r2, r, sizeof r); r2[2] = 2;
        bool refused = !classic_decode_record(r2, sizeof r2, &b);
        std::memcpy(r2, r, sizeof r); r2[27] = 1;
        refused = refused && !classic_decode_record(r2, sizeof r2, &b);
        std::memcpy(r2, r, sizeof r); r2[8] = 5;
        refused = refused && !classic_decode_record(r2, sizeof r2, &b) && !classic_decode_record(r, 27, &b);
        m[20] ^= 1;
        refused = refused && !classic_decode_mirror(m, &b);
        check(refused, "unknown revisions, bad fields and a bad mirror CRC are refused", "");
    }

    // ---- 5. engine: load, generate, save
    if (haveFx) {
        const std::string docs = eden_platform_documents_root();
        TerrainGenerator* tg = World::getWorld->terrain->tgen;
        const int hx = fx.keys[0].first * 16 + 8, hz = fx.keys[0].second * 16 + 8;
        // (a) a 1.x world
        const std::string lp = docs + "/classic-legacy.emod";
        bool ok = write_world(lp, "classic legacy", fx.seed, hx, hz, &fx, NULL, NULL, err) && play_file("classic-legacy.emod");
        check(ok, "a 1.x world (fixture columns, no record) plays", err.c_str());
        if (ok) {
            tick(30);
            const ClassicGen& g = tg->classic;
            std::snprintf(d, sizeof d, "state %d profile %d caves %d flags %d, votes %d/%d", tg->classicState, g.profile,
                          g.caves, g.flags, g_classic_cave_votes_air, g_classic_cave_votes_cells);
            check(tg->classicState == TerrainGenerator::CLASSIC_STATE_SET && g.profile == CLASSIC_PROFILE_LEGACY &&
                  g.caves == CLASSIC_CAVES_OFF && g.flags == 1 && g_classic_cave_votes_cells >= 2000,
                  "it loads the legacy profile, caves detected off", d);
            std::set<std::pair<int, int> > saved(fx.keys.begin(), fx.keys.end());
            int tested = 0, bad = 0;
            for (int dx = -4; dx <= 4 && tested < 6; ++dx)
                for (int dz = -4; dz <= 4 && tested < 6; ++dz) {
                    const int cx = fx.keys[0].first + dx, cz = fx.keys[0].second + dz;
                    if (saved.count(std::make_pair(cx, cz))) continue;
                    const int b = column_vs_pure(g, cx, cz, g_world_height);
                    if (b < 0) continue;
                    ++tested; bad += b;
                }
            std::snprintf(d, sizeof d, "%d unsaved columns, %d cells differ", tested, bad);
            check(tested == 6 && bad == 0, "its unsaved columns are the pure function's", d);
            save_world();
            save_world();
            const int n9 = count_records(lp, emod::T_GENERATOR);
            std::snprintf(d, sizeof d, "%d GENERATOR record(s) after two saves", n9);
            check(n9 == 1, "a legacy world writes GENERATOR exactly once", d);
            quit_to_menu();
            // the exported `.eden` carries the mirror
            const std::string ep = docs + "/classic-legacy-export.eden";
            emod::ExportOptions eo;
            std::string xerr;
            bool exported = emod::export_world_file(lp, ep, eo, false, NULL, &xerr);
            ClassicGen mg;
            bool mirrored = false;
            if (exported) {
                FILE* f = emod::io::fopen_utf8(ep.c_str(), "rb");
                WorldFileHeader eh;
                mirrored = f && std::fread(&eh, 1, sizeof eh, f) == sizeof eh &&
                           classic_decode_mirror((const unsigned char*)eh.reserved, &mg) &&
                           mg.seed == fx.seed && mg.profile == CLASSIC_PROFILE_LEGACY && mg.flags == 1;
                if (f) std::fclose(f);
                emod::io::remove_file(ep);
            }
            check(exported && mirrored, "the exported .eden carries the GENERATOR mirror", xerr.c_str());
        }
        // (b) a mirror in the header (an imported Classic `.eden`) is honoured and recorded
        ClassicGen mg;
        classic_default_params(&mg, 31337);
        mg.trees = 3; mg.hilliness = 3; mg.caves = CLASSIC_CAVES_DEEP;
        uint8_t mirror[36];
        classic_encode_mirror(mg, mirror);
        const std::string mp = docs + "/classic-mirror.emod";
        ok = write_world(mp, "classic mirror", 31337, 4096 * 16 + 8, 4096 * 16 + 8, NULL, NULL, mirror, err) &&
             play_file("classic-mirror.emod");
        check(ok, "a world whose header holds a mirror plays", err.c_str());
        if (ok) {
            tick(30);
            const bool same = tg->classicState == TerrainGenerator::CLASSIC_STATE_SET &&
                              std::memcmp(&tg->classic, &mg, sizeof mg) == 0;
            const int b = column_vs_pure(mg, 4096, 4096, g_world_height);
            std::snprintf(d, sizeof d, "state %d, trees %d caves %d, spawn column %d cells differ", tg->classicState,
                          tg->classic.trees, tg->classic.caves, b);
            check(same && b == 0, "the mirror's settings generate the world", d);
            save_world();
            const int n9 = count_records(mp, emod::T_GENERATOR);
            emod::EdenWorldStore st;
            std::vector<uint8_t> rec;
            ClassicGen back;
            const bool rd = st.open(mp.c_str(), false) && st.readRecord(emod::T_GENERATOR, rec) == emod::EdenWorldStore::READ_OK &&
                            classic_decode_record(&rec[0], rec.size(), &back) && std::memcmp(&back, &mg, sizeof mg) == 0;
            st.close();
            std::snprintf(d, sizeof d, "%d record(s), decodes to the mirror: %d", n9, rd ? 1 : 0);
            check(n9 == 1 && rd, "the next save records it", d);
            quit_to_menu();
        }
        // (c) a GENERATOR from a newer Emod: void, nothing saved
        uint8_t future[CLASSIC_RECORD_BYTES];
        classic_encode_record(mg, future);
        future[2] = 2;   // revision 2
        const std::string vp = docs + "/classic-void.emod";
        ok = write_world(vp, "classic void", 31337, 4096 * 16 + 8, 4096 * 16 + 8, NULL, future, NULL, err) &&
             play_file("classic-void.emod");
        check(ok, "a world with an unknown GENERATOR plays", err.c_str());
        if (ok) {
            tick(10);
            int solid = 0;
            for (int x = 0; x < 16; ++x)
                for (int z = 0; z < 16; ++z)
                    for (int y = 0; y < g_world_height; ++y) solid += getLandc(4096 * 16 + x, 4096 * 16 + z, y) != 0;
            std::snprintf(d, sizeof d, "state %d, %d solid cells at spawn", tg->classicState, solid);
            check(tg->classicState == TerrainGenerator::CLASSIC_STATE_VOID && solid == 0, "it loads void", d);
            save_world();
            emod::EdenWorldStore st;
            const bool opened = st.open(vp.c_str(), false);
            std::snprintf(d, sizeof d, "%zu columns, %d GENERATOR record(s)", opened ? st.columnCount() : (size_t)0,
                          count_records(vp, emod::T_GENERATOR));
            check(opened && st.columnCount() == 0 && count_records(vp, emod::T_GENERATOR) == 1,
                  "saving it stores no columns and keeps the record", d);
            st.close();
            quit_to_menu();
        }
    }

    std::printf("[eden-classic] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

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
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    aim(1, 4.0f);
    cam->render();
    const bool behindIn = inside(px - 1.5f), aheadIn = inside(px + 3.0f), pastEyeIn = inside(px - 6.0f);
    glLoadIdentity();
    aim(0, 4.0f);
    cam->render();
    const bool behindIn0 = inside(px - 1.5f);
    glPopMatrix();
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
void ui_tap(float x, float yUp);
void ui_tap_rect(CGRect r);
void push_text(const char* utf8);
void push_tap_key(SDL_Scancode sc);
void ui_menu_to(bool home);   // I.7, defined after ui_tap_rect

namespace props_test {

// classic_test::write_world at either height: header, empty creature slots, an optional record.
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
    if (menu()->showprops) { menu()->props->back(); menu()->showprops = FALSE; }
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
    using classic_test::column_vs_pure;
    using classic_test::count_records;
    using classic_test::play_file;
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
        ok = fm->readWorldProperties(@"props-round.emod", &p);
        p.gen.caves = CLASSIC_CAVES_OFF;
        const int r = ok ? fm->applyWorldProperties(@"props-round.emod", p) : -1;
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
        ok = ok && fm->readWorldProperties(@"props-round.emod", &p);
        p.gen.hilliness = 4;   // Wild: a different surface
        const int r = ok ? fm->applyWorldProperties(@"props-round.emod", p) : -1;
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
    classic_test::Fixture fx;
    const bool haveFx = classic_test::load_fixture(g_opt.classicFixture, fx, err);
    check(haveFx, "the 1.x fixture loads", haveFx ? g_opt.classicFixture.c_str() : err.c_str());
    if (haveFx) {
        const std::string lp = docs + "/props-legacy.emod";
        const int hx = fx.keys[0].first * 16 + 8, hz = fx.keys[0].second * 16 + 8;
        ok = classic_test::write_world(lp, "props legacy", fx.seed, hx, hz, &fx, NULL, NULL, err) && open_props("props-legacy.emod");
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
        const bool rd = fm->readWorldProperties([NSString stringWithUTF8String:g_opt.propsWorld.c_str()], &p);
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
void push_text(const char* utf8);            // --ui-selftest's helpers, defined below
void push_tap_key(SDL_Scancode sc);
void ui_pointer(int phase, float x, float yUp);
void ui_tap_rect(CGRect r);

namespace signs_test {

std::string toast() {
    NSString* t = World::getWorld->hud->sb->current();
    return t ? std::string([t UTF8String]) : std::string();
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
        ter->burnBlock(cx - 1, cz, cy, TRUE);   // lit as a blast lights it: a 0.5-0.8 s fuse,
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

// ---------------------------------------------------------------------------------------------
// --cmds-selftest  (Stage D / D.4c, the command-block interpreter)
// ---------------------------------------------------------------------------------------------
// With --world=NAME (the 3-bit adder, in --docs) and --at near its keypad: a parser leg on literal
// scripts; every CMB1 script in the world parses with no bad statement; the adder computes 3 + 4 = 7
// on its own seven-segment display when its buttons are pressed; then every command, the caps and
// the triggers on command blocks this test places in open air; a real screen tap on a command block
// with a block armed pushes it and builds nothing; save + reload keeps no runtime state. Fixed
// 1/60 s etime: waits, timers and fuses are engine time. --cmds-mutate=nodepth (run ignores the
// 8-deep cap) must fail the depth leg; --cmds-trace prints every statement.
namespace cmds_test {

Terrain* ter() { return World::getWorld->terrain; }
WorldTrailer* tr() { return (WorldTrailer*)eden_debug_world_trailer(); }
// Engine order (x, y_up, z) throughout this test.
int land(int x, int y, int z) { return ter()->getLand(x, z, y); }
int paint(int x, int y, int z) { return ter()->getColor(x, z, y); }
void put(int x, int y, int z, int type) { ter()->updateChunks(x, z, y, type); }
TrailerPos P(int x, int y, int z) { return TrailerPos::fromEngine(x, y, z); }
// A command block at (x, y, z) with this script (steel + record, as the CMD tool makes one).
TrailerPos make(int x, int y, int z, const char* script) {
    put(x, y, z, TYPE_STEEL);
    ter()->setColor(x, z, y, 0);
    const TrailerPos p = P(x, y, z);
    const int i = tr()->findCmd(p);
    if (i >= 0) tr()->setCmdScript((size_t)i, script);
    else tr()->addCmd(p, script);
    return p;
}
void secs(float s) { tick((int)(s * 60.0f + 0.5f)); }
std::string toast() {
    NSString* t = World::getWorld->hud->sb->current();
    return t ? std::string([t UTF8String]) : std::string();
}
// An all-air box (w x h x d) starting 3 above the highest ground under it, near (px, pz). Engine order.
bool open_air(int px, int pz, int w, int h, int d, int* ox, int* oy, int* oz) {
    // East of the adder (it spans x 65512..65581), inside the resident window.
    for (int k = 0; k < 16; ++k) {
        const int x = px + 50 + (k % 4) * (w + 2), z = pz - 20 + (k / 4) * (d + 2);
        int top = -1;
        for (int a = 0; a < w; ++a)
            for (int c = 0; c < d; ++c)
                for (int y = T_HEIGHT - 1; y > top; --y)
                    if (land(x + a, y, z + c) > 0) { top = y; break; }
        const int y0 = top + 3;
        if (top < 0 || y0 + h + 1 >= T_HEIGHT) continue;
        *ox = x; *oy = y0; *oz = z;
        return true;
    }
    return false;
}

// The adder's units display: a seven-segment digit on the plane x = 65512 (engine order), each
// segment's cells read off the scripts at x = 65521 that light it ("fill -9 9 -1 -9 9 1 lamp 9"...).
struct Seg { const char* name; int z0, z1, y0, y1; };
const Seg kSegs[7] = {
    {"top", 65532, 65534, 42, 42}, {"top-left", 65535, 65535, 39, 41}, {"top-right", 65531, 65531, 39, 41},
    {"center", 65532, 65534, 38, 38}, {"bottom-left", 65535, 65535, 35, 37},
    {"bottom-right", 65531, 65531, 35, 37}, {"bottom", 65532, 65534, 34, 34}};
// Bit i = segment i lit (every cell lamp 9, the off colour is 54); -1 if a segment is mixed or not lamps.
int segments(std::string* why) {
    int bits = 0;
    bool bad = false;
    for (int i = 0; i < 7; ++i) {
        int lit = 0, off = 0, other = 0;
        for (int z = kSegs[i].z0; z <= kSegs[i].z1; ++z)
            for (int y = kSegs[i].y0; y <= kSegs[i].y1; ++y) {
                const int t = land(65512, y, z), c = paint(65512, y, z);
                if (t == TYPE_LIGHTBOX && c == 9) ++lit;
                else if (t == TYPE_LIGHTBOX && c == 54) ++off;
                else ++other;
            }
        if (why) *why += std::string(kSegs[i].name) + "=" + std::to_string(lit) + "/" + std::to_string(off) +
                         (other ? "/?" + std::to_string(other) : "") + " ";
        if (other || (lit && off)) bad = true;
        if (lit) bits |= 1 << i;
    }
    return bad ? -1 : bits;
}
int digitOf(int bits) {
    // bit order: top, top-left, top-right, center, bottom-left, bottom-right, bottom
    static const int kDigits[10] = {0x77, 0x24, 0x5D, 0x6D, 0x2E, 0x6B, 0x7B, 0x25, 0x7F, 0x6F};
    for (int d = 0; d < 10; ++d) if (kDigits[d] == bits) return d;
    return -1;
}

}  // namespace cmds_test

// The parser on literal scripts: one case table, run by --cmds-selftest and --console-selftest (C.1
// moved it out of the adder test so CI, which has no adder, checks the parser on every target).
void cmds_parser_cases() {
    char d[1024];
    {
        CmdScript::Program p;
        CmdScript::parse("say WELCOME TRAVELER;wait 1;set 0 3 0 lamp;gold 1", &p);
        check(p.errors == 0 && p.trigger == CmdScript::TRIG_TOUCH && p.stmts.size() == 4 &&
              p.stmts[0].op == CmdScript::OP_SAY && p.stmts[0].text == "WELCOME TRAVELER" &&
              p.stmts[1].op == CmdScript::OP_WAIT && p.stmts[1].f == 1.0f && p.stmts[2].op == CmdScript::OP_SET &&
              p.stmts[2].v[1] == 3 && p.stmts[2].type == TYPE_LIGHTBOX && p.stmts[3].op == CmdScript::OP_GOLD,
              "parse: cmd.html's first example", p.firstError.c_str());
        CmdScript::parse("@timer .1; if 0 -1 0 2; run 0 -2 0; set 0 -1 0 0", &p);
        check(p.errors == 0 && p.trigger == CmdScript::TRIG_TIMER && std::fabs(p.triggerArg - 0.1f) < 1e-6f &&
              p.stmts.size() == 3 && p.stmts[0].op == CmdScript::OP_IF && p.stmts[0].type == TYPE_STONE &&
              p.stmts[1].op == CmdScript::OP_RUN && p.stmts[1].v[1] == -2,
              "parse: a header, block numbers, negative heights", p.firstError.c_str());
        CmdScript::parse("@near 6;set ~ ~3 ~ glass;fill -1 0 -1 1 2 1 brick 19", &p);
        check(p.errors == 0 && p.trigger == CmdScript::TRIG_NEAR && p.triggerArg == 6 && p.stmts[0].v[0] == 0 &&
              p.stmts[0].v[1] == 3 && p.stmts[0].type == TYPE_GLASS && p.stmts[1].op == CmdScript::OP_FILL &&
              p.stmts[1].color == 19, "parse: tildes, fill with a colour", p.firstError.c_str());
        CmdScript::parse("frob 1 2; fill 0 0 0 16 16 16 stone; say ok; @step; set 0 0 0 voidstone; wait 9999; colour", &p);
        std::snprintf(d, sizeof(d), "errors %d, first '%s', wait %.0f", p.errors, p.firstError.c_str(),
                      p.stmts.size() > 5 ? p.stmts[5].f : -1.0f);
        check(p.errors == 5 && p.firstError == "Unknown command: frob" && p.stmts.size() == 7 &&
              p.stmts[2].op == CmdScript::OP_SAY && p.stmts[5].op == CmdScript::OP_WAIT && p.stmts[5].f == 600.0f,
              "parse: unknown command, fill > 4096, late trigger, unknown block; wait capped at 600", d);
        // The last sound and one past it (NUM_SOUNDS grew from 73 to 79 in 6ef81de; this leg pinned 72/73).
        std::snprintf(d, sizeof(d), "fill 0 0 0 15 15 15 stone; spawn moof; spawn charger 1 2 3; spawn dragon; sound %d; "
                      "sound %d; time 0.5; time 2; paint 0 0 0 54; paint 0 0 0 55", NUM_SOUNDS - 1, NUM_SOUNDS);
        CmdScript::parse(d, &p);
        std::snprintf(d, sizeof(d), "errors %d, first '%s'", p.errors, p.firstError.c_str());
        check(p.errors == 4 && p.stmts.size() == 10 && p.stmts[0].op == CmdScript::OP_FILL && p.stmts[2].hasPos &&
              p.stmts[2].type == M_CHARGER && p.stmts[3].op == CmdScript::OP_BAD && p.stmts[4].op == CmdScript::OP_SOUND &&
              p.stmts[5].op == CmdScript::OP_BAD && p.stmts[7].op == CmdScript::OP_BAD &&
              p.stmts[8].op == CmdScript::OP_PAINT && p.stmts[9].op == CmdScript::OP_BAD,
              "parse: fill at exactly 4096, creatures, sound/time/colour ranges", d);
    }
}

int run_cmds_selftest() {
    using namespace cmds_test;
    g_selftestTag = "eden-cmds";
    CmdScript::testNoDepthCap = g_opt.cmdsMutate == "nodepth";
    CmdScript::trace = g_opt.cmdsTrace;
    char d[1024];

    // ---- the parser, on literal scripts (shared with --console-selftest)
    cmds_parser_cases();

    if (g_opt.world.empty()) { check(false, "--world=NAME (the 3-bit adder) is given", ""); return 1; }
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 4);
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }   // --world-format=emod's "World converted"
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    WorldTrailer* t = tr();
    Hud* hud = World::getWorld->hud;
    Player* pl = World::getWorld->player;
    std::printf("[eden-cmds] world %s: %s\n", eden_debug_world_file(), t ? t->describe().c_str() : "no trailer");

    // ---- every script in the world parses
    {
        int bad = 0;
        std::string first;
        for (size_t i = 0; t && i < t->cmdCount(); ++i) {
            CmdScript::Program p;
            CmdScript::parse(t->cmd(i).script, &p);
            bad += p.errors;
            if (p.errors && first.empty()) first = std::string(t->cmd(i).script) + " -> " + p.firstError;
        }
        std::snprintf(d, sizeof(d), "%zu scripts, %d bad statements%s%s", t ? t->cmdCount() : 0, bad,
                      first.empty() ? "" : ": ", first.c_str());
        check(t && t->cmdCount() > 0 && bad == 0, "every CMB1 script parses (0 bad statements)", d);
    }

    // ---- the adder: Reset, 3, +, 4, =. The keypad, file order (x, y, z) -> engine (x, z, y).
    // The answer is the digit on the units display.
    {
        struct Key { const char* name; int x, y, z; };
        const Key keys[] = {{"Reset", 65531, 33, 65524}, {"3", 65531, 34, 65523}, {"+", 65531, 33, 65522},
                            {"4", 65531, 35, 65525}, {"=", 65531, 33, 65523}};
        std::string log;
        bool allPressed = true;
        for (const Key& k : keys) {
            const int r = CmdScript::press(P(k.x, k.y, k.z));
            allPressed = allPressed && r == CmdScript::PRESS_STARTED;
            secs(3.0f);
            std::string why;
            const int bits = segments(&why);
            log += std::string(k.name) + ":" + std::to_string(r) + "->" + std::to_string(digitOf(bits)) + " ";
            std::printf("[eden-cmds] adder after %-5s %s| %s\n", k.name, why.c_str(), CmdScript::describe());
        }
        secs(4.0f);
        std::string why;
        const int digit = digitOf(segments(&why));
        const CmdScript::Stats s = CmdScript::stats();
        std::snprintf(d, sizeof(d), "%s| final %d (%s) depth-refused %d unknown %d", log.c_str(), digit, why.c_str(),
                      s.refusedDepth, s.unknown);
        check(allPressed && digit == 7 && s.refusedDepth == 0 && s.unknown == 0 && s.refusedFull == 0,
              "the 3-bit adder: Reset, 3, +, 4, = shows 7", d);
        // ... and the keys light their own digit on the way (3 after "3", 4 after "4").
        check(log.find("3:1->3") != std::string::npos && log.find("4:1->4") != std::string::npos,
              "the adder's keys show their digit as they are pressed", log.c_str());
        std::printf("[eden-cmds] adder %s\n", CmdScript::describe());

        // More sums, so "7" is the adder adding and not a coincidence: the tens display is a "1" at
        // z = 65538 (the TEN script), lit or not.
        const Key digitKeys[8] = {{"0", 65531, 33, 65525}, {"1", 65531, 34, 65525}, {"2", 65531, 34, 65524},
                                  {"3", 65531, 34, 65523}, {"4", 65531, 35, 65525}, {"5", 65531, 35, 65524},
                                  {"6", 65531, 35, 65523}, {"7", 65531, 36, 65525}};
        auto tens = [&]() {
            int lit = 0, off = 0;
            for (int y = 35; y <= 41; ++y) {
                if (y == 38) continue;
                const int ty = land(65512, y, 65538), c = paint(65512, y, 65538);
                if (ty == TYPE_LIGHTBOX && c == 9) ++lit;
                else if (ty == TYPE_LIGHTBOX && c == 54) ++off;
            }
            return lit == 6 ? 1 : off == 6 ? 0 : -1;
        };
        const int sums[][2] = {{0, 0}, {1, 6}, {2, 3}, {5, 2}, {5, 5}, {7, 7}, {6, 7}, {7, 1}};
        std::string all;
        int wrong = 0;
        for (const auto& ab : sums) {
            const Key seq[] = {keys[0], digitKeys[ab[0]], keys[2], digitKeys[ab[1]], keys[4]};
            for (const Key& k : seq) { CmdScript::press(P(k.x, k.y, k.z)); secs(3.0f); }
            secs(3.0f);
            const int u = digitOf(segments(NULL)), tn = tens();
            const int got = (u < 0 || tn < 0) ? -1 : tn * 10 + u;
            all += std::to_string(ab[0]) + "+" + std::to_string(ab[1]) + "=" + std::to_string(got) + " ";
            if (got != ab[0] + ab[1]) ++wrong;
        }
        std::snprintf(d, sizeof(d), "%s| maxDepth %d, depth-refused %d", all.c_str(), CmdScript::stats().maxDepth,
                      CmdScript::stats().refusedDepth);
        check(wrong == 0 && CmdScript::stats().refusedDepth == 0, "the adder adds: eight more sums, 0..14", d);
    }

    // ---- the commands, on blocks placed in open air. The player is parked out of every written cell.
    const PlayerState ps = player_state();
    int ox, oy, oz;
    if (!open_air((int)ps.x, (int)ps.z, 16, 12, 16, &ox, &oy, &oz)) { check(false, "an open-air test site", ""); return 1; }
    auto park = [&]() {
        pl->pos = MakeVector(ox - 30.5f, (float)oy + 6.0f, oz - 30.5f);
        pl->vel = MakeVector(0, 0, 0);
    };
    park();
    tick(10);
    std::printf("[eden-cmds] site %d,%d,%d (engine order)\n", ox, oy, oz);
    const int X = ox + 2, Y = oy + 2, Z = oz + 2;   // the first block; legs use offsets from it

    // set / fill / paint / a lamp, and the skin's green button while it runs (min 0.25 s)
    {
        const TrailerPos A = make(X, Y, Z, "set 0 2 0 glass; fill 1 0 0 2 1 1 brick 19; paint 1 0 0 30; set 0 1 0 lamp 9");
        tick(2);
        const int r = CmdScript::press(A);
        tick(1);
        const bool green = CmdTool::running(A);
        std::snprintf(d, sizeof(d), "press %d; glass %d; brick %d/%d %d/%d; lamp %d/%d; green %d",
                      r, land(X, Y + 2, Z), land(X + 1, Y, Z), paint(X + 1, Y, Z), land(X + 2, Y + 1, Z + 1),
                      paint(X + 2, Y + 1, Z + 1), land(X, Y + 1, Z), paint(X, Y + 1, Z), green);
        check(r == CmdScript::PRESS_STARTED && land(X, Y + 2, Z) == TYPE_GLASS && land(X + 1, Y, Z) == TYPE_BRICK &&
              paint(X + 1, Y, Z) == 30 && land(X + 2, Y + 1, Z + 1) == TYPE_BRICK && paint(X + 2, Y + 1, Z + 1) == 19 &&
              land(X, Y + 1, Z) == TYPE_LIGHTBOX && paint(X, Y + 1, Z) == 9 && green,
              "set, fill (+colour), paint, a lamp; the button goes green", d);
        secs(0.5f);
        std::snprintf(d, sizeof(d), "green %d, busy %d", CmdTool::running(A), CmdScript::busy(A));
        check(!CmdTool::running(A) && !CmdScript::busy(A), "a script with no wait is done; green fades after 0.25 s", d);
        // clean up for the next legs: air through set (the record on A goes too, like mining)
        const TrailerPos C = make(X + 8, Y, Z, "fill -8 0 0 -6 2 1 air");
        CmdScript::press(C);
        tick(2);
        std::snprintf(d, sizeof(d), "A voxel %d record %d; brick %d", land(X, Y, Z), tr()->findCmd(A), land(X + 1, Y, Z));
        check(land(X, Y, Z) == 0 && tr()->findCmd(A) < 0 && land(X + 1, Y, Z) == 0 && land(X, Y + 1, Z) == 0,
              "fill air clears blocks and takes a command block's record with its steel", d);
    }

    // wait: non-blocking, the button stays green while it waits, other blocks keep running
    {
        const TrailerPos A = make(X, Y, Z, "set 0 2 0 stone; wait 1; set 0 2 0 air");
        const TrailerPos B = make(X + 2, Y, Z, "set 0 2 0 dirt");
        CmdScript::press(A);
        tick(3);
        const bool mid = land(X, Y + 2, Z) == TYPE_STONE && CmdScript::busy(A) && CmdTool::running(A);
        const int rb = CmdScript::press(A);   // busy: no re-entry
        CmdScript::press(B);
        tick(2);
        const bool other = land(X + 2, Y + 2, Z) == TYPE_DIRT && land(X, Y + 2, Z) == TYPE_STONE;
        secs(1.2f);
        std::snprintf(d, sizeof(d), "mid %d, re-press %d, other %d, after: %d busy %d green %d", mid, rb, other,
                      land(X, Y + 2, Z), CmdScript::busy(A), CmdTool::running(A));
        check(mid && rb == CmdScript::PRESS_BUSY && other && land(X, Y + 2, Z) == 0 && !CmdScript::busy(A) &&
              !CmdTool::running(A), "wait holds only its own script; a busy block ignores a press", d);
    }

    // if / unless gate the next statement; a skipped if takes its gated statement along
    {
        put(X, Y + 2, Z, 0);
        const TrailerPos A = make(X, Y, Z, "if 0 2 0 air; set 0 3 0 dirt; unless 0 2 0 air; set 0 4 0 dirt; "
                                           "if 0 2 0 stone; if 0 2 0 air; set 0 5 0 dirt; set 0 6 0 dirt; "
                                           "unless 0 2 0 stone; unless 0 3 0 stone; set 0 7 0 dirt");
        CmdScript::press(A);
        tick(2);
        std::snprintf(d, sizeof(d), "y+3 %d y+4 %d y+5 %d y+6 %d y+7 %d", land(X, Y + 3, Z), land(X, Y + 4, Z),
                      land(X, Y + 5, Z), land(X, Y + 6, Z), land(X, Y + 7, Z));
        check(land(X, Y + 3, Z) == TYPE_DIRT && land(X, Y + 4, Z) == 0 && land(X, Y + 5, Z) == 0 &&
              land(X, Y + 6, Z) == TYPE_DIRT && land(X, Y + 7, Z) == TYPE_DIRT, "if / unless, and chained (AND)", d);
        for (int y = 3; y <= 7; ++y) put(X, Y + y, Z, 0);
    }

    // run chains 8 deep: ten blocks in a row, each "run 1 0 0; set 0 1 0 stone" -- the first nine set
    // their stone (depth 0..8), the tenth is refused. The mutation runs all ten.
    {
        const CmdScript::Stats s0 = CmdScript::stats();
        for (int k = 0; k < 10; ++k) make(X + k, Y + 4, Z + 4, "run 1 0 0; set 0 1 0 stone");
        tick(2);
        CmdScript::press(P(X, Y + 4, Z + 4));
        tick(2);
        int n = 0;
        for (int k = 0; k < 10; ++k) n += land(X + k, Y + 5, Z + 4) == TYPE_STONE;
        const CmdScript::Stats s1 = CmdScript::stats();
        std::snprintf(d, sizeof(d), "%d of 10 ran, depth refusals %d, toast '%s'", n, s1.refusedDepth - s0.refusedDepth,
                      toast().c_str());
        check(n == 9 && land(X + 9, Y + 5, Z + 4) == 0 && s1.refusedDepth - s0.refusedDepth == 1,
              "run chains 8 deep, the 9th run is refused", d);
        CmdScript::press(make(X + 12, Y + 4, Z + 4, "fill -12 0 0 -2 1 0 air"));
        tick(2);
    }

    // 32 at once: 40 blocks with "wait 2", pressed in one frame
    {
        const CmdScript::Stats s0 = CmdScript::stats();
        std::vector<TrailerPos> v;
        for (int k = 0; k < 40; ++k) v.push_back(make(X + (k % 10), Y + 7, Z + 6 + k / 10, "wait 2"));
        tick(1);
        for (const TrailerPos& p : v) CmdScript::press(p);
        const int live = CmdScript::liveCount();
        const CmdScript::Stats s1 = CmdScript::stats();
        secs(2.3f);
        std::snprintf(d, sizeof(d), "live %d, refused %d, after %d", live, s1.refusedFull - s0.refusedFull, CmdScript::liveCount());
        check(live == 32 && s1.refusedFull - s0.refusedFull == 8 && CmdScript::liveCount() == 0,
              "at most 32 scripts run at once", d);
        CmdScript::press(make(X + 12, Y + 7, Z + 6, "fill -12 0 0 -3 0 3 air"));
        tick(2);
    }

    // the safety rules: bedrock, the player's cell, another command block, unresident cells
    {
        int by = -1;
        for (int y = 0; y < 4; ++y) if (land(X, y, Z) == TYPE_BEDROCK) { by = y; break; }
        const TrailerPos B = make(X + 3, Y, Z, "");
        char sc[200];
        std::snprintf(sc, sizeof(sc), "set 0 %d 0 air; set 3 0 0 stone; set 0 0 400 stone; fill 0 -1 0 0 -1 0 stone", by - Y);
        const TrailerPos A = make(X, Y, Z, sc);
        // the player stands in the cell below A (where the fill writes)
        pl->pos = MakeVector(X + 0.5f, (float)(Y - 1) + pl->boxheight / 2 + 0.001f, Z + 0.5f);
        pl->vel = MakeVector(0, 0, 0);
        const int below = land(X, Y - 1, Z);
        CmdScript::press(A);
        park();
        tick(2);
        std::snprintf(d, sizeof(d), "bedrock y=%d -> %d; cmd B %d/%d; far %d; player cell %d (was %d)", by,
                      by >= 0 ? land(X, by, Z) : -9, land(X + 3, Y, Z), tr()->findCmd(B), land(X, Y, Z + 400),
                      land(X, Y - 1, Z), below);
        check(by >= 0 && land(X, by, Z) == TYPE_BEDROCK && land(X + 3, Y, Z) == TYPE_STEEL && tr()->findCmd(B) >= 0 &&
              land(X, Y - 1, Z) == below, "set/fill never touch bedrock, a command block or the player's cell", d);
        CmdScript::press(make(X + 1, Y, Z, "set -1 0 0 air; set 2 0 0 air"));
        tick(2);
    }

    // an unknown command shows its message; the rest still runs
    {
        const TrailerPos A = make(X, Y, Z, "frob 1; set 0 2 0 dirt");
        const CmdScript::Stats s0 = CmdScript::stats();
        CmdScript::press(A);
        tick(1);
        const std::string msg = toast();
        std::snprintf(d, sizeof(d), "toast '%s', dirt %d", msg.c_str(), land(X, Y + 2, Z));
        check(msg == "Unknown command: frob" && land(X, Y + 2, Z) == TYPE_DIRT && CmdScript::stats().unknown == s0.unknown + 1,
              "an unknown command shows a message and the rest still runs", d);
        put(X, Y + 2, Z, 0);
    }

    // say, tp, gold, heal, hurt (only with Health on), flash, night / day, sound, spawn
    {
        const TrailerPos A = make(X, Y, Z, "say HELLO there; tp 0 3 0; gold 5; heal; flash; sound 8");
        const int g0 = hud->goldencubes;
        pl->life = 0.5f;
        CmdScript::press(A);
        tick(1);
        const PlayerState p1 = player_state();
        const float feet = pl->pos.y - pl->boxheight / 2;
        std::snprintf(d, sizeof(d), "toast '%s', player %.2f,%.2f,%.2f feet %.3f, gold %d->%d, life %.2f, flash %.2f",
                      toast().c_str(), p1.x, p1.y, p1.z, feet, g0, hud->goldencubes, pl->life, hud->flash);
        check(toast() == "HELLO there" && std::fabs(pl->pos.x - (X + 0.5f)) < 0.01f && std::fabs(pl->pos.z - (Z + 0.5f)) < 0.01f &&
              std::fabs(feet - (Y + 3)) < 0.05f && hud->goldencubes == std::min(999, g0 + 5) && pl->life == 1.0f && hud->flash > 0,
              "say, tp (feet at the cell), gold, heal, flash", d);
        park();
        const BOOL h0 = pl->health_option;
        pl->health_option = FALSE;
        tr()->setCmdScript((size_t)tr()->findCmd(A), "hurt 25");
        CmdScript::press(A);
        tick(1);
        const float offLife = pl->life;
        pl->health_option = TRUE;
        CmdScript::press(A);
        tick(1);
        const float onLife = pl->life;
        pl->health_option = h0;
        pl->life = 1;
        std::snprintf(d, sizeof(d), "health off %.2f, on %.2f", offLife, onLife);
        check(offLife == 1.0f && std::fabs(onLife - 0.75f) < 0.01f, "hurt bites only with Health on", d);
        tr()->setCmdScript((size_t)tr()->findCmd(A), "night");
        CmdScript::press(A);
        tick(2);
        const bool night = v_equals(ter()->final_skycolor, colorTable[54]);
        tr()->setCmdScript((size_t)tr()->findCmd(A), "time 0.5");
        CmdScript::press(A);
        tick(2);
        const bool day = !v_equals(ter()->final_skycolor, colorTable[54]);
        check(night && day, "night paints the sky night, time 0.5 gives the day back",
              night && day ? "" : night ? "day failed" : "night failed");
        tr()->setCmdScript((size_t)tr()->findCmd(A), "spawn moof; spawn stumpy 0 1 3");
        const int c0 = CountActiveCreatures();
        CmdScript::press(A);
        tick(2);
        std::snprintf(d, sizeof(d), "creatures %d -> %d", c0, CountActiveCreatures());
        check(!CREATURES_ON || CountActiveCreatures() >= c0 + 1, "spawn puts creatures in the world", d);
    }

    // fire and explode: a wood block burns; a lit TNT drops and blows (taking the command block)
    {
        put(X + 4, Y, Z, TYPE_WOOD);
        const TrailerPos A = make(X, Y, Z, "fire 4 0 0; explode 0 0 0");
        tick(1);
        CmdScript::press(A);
        tick(2);
        const bool burning = isOnFire(X + 4, Z, Y);
        const bool tnt = land(X, Y, Z) == TYPE_TNT && isOnFire(X, Z, Y);
        bool gone = false;
        for (int k = 0; k < 2400 && !gone; ++k) { tick(1); gone = land(X, Y, Z) == 0; }
        std::snprintf(d, sizeof(d), "wood burning %d, tnt lit %d, blown %d, record %d", burning, tnt, gone, tr()->findCmd(A));
        check(burning && tnt && gone && tr()->findCmd(A) < 0, "fire lights a burnable block; explode drops lit TNT", d);
        tick(240);
    }

    // triggers: @step, @near, @timer (only within 48 blocks)
    {
        const int sx = X + 6, sy = Y, sz = Z + 10;
        const TrailerPos S = make(sx, sy, sz, "@step; set 2 0 0 dirt");
        const TrailerPos N = make(sx + 8, sy, sz, "@near 3; set 0 2 0 dirt");
        tick(2);
        auto standOn = [&](int x, int y, int z) {
            pl->pos = MakeVector(x + 0.5f, (float)(y + 1) + pl->boxheight / 2 + 0.001f, z + 0.5f);
            pl->vel = MakeVector(0, 0, 0);
        };
        standOn(sx, sy, sz);
        tick(2);
        const bool s1 = land(sx + 2, sy, sz) == TYPE_DIRT;
        put(sx + 2, sy, sz, 0);
        standOn(sx, sy, sz);
        tick(5);
        const bool sStay = land(sx + 2, sy, sz) == 0;   // standing still does not re-fire
        park();
        tick(2);
        standOn(sx, sy, sz);
        tick(2);
        const bool s2 = land(sx + 2, sy, sz) == TYPE_DIRT;   // re-armed by stepping off
        park();
        std::snprintf(d, sizeof(d), "first %d, still %d, again %d", s1, sStay, s2);
        check(s1 && sStay && s2, "@step fires on stepping on, once, and re-arms off the block", d);

        pl->pos = MakeVector(sx + 8 + 0.5f + 2.0f, sy + 0.5f, sz + 0.5f);   // 2 blocks away
        pl->vel = MakeVector(0, 0, 0);
        tick(2);
        const bool n1 = land(sx + 8, sy + 2, sz) == TYPE_DIRT;
        put(sx + 8, sy + 2, sz, 0);
        tick(5);
        const bool nStay = land(sx + 8, sy + 2, sz) == 0;
        park();
        tick(2);
        pl->pos = MakeVector(sx + 8 + 0.5f, sy + 0.5f, sz + 0.5f - 2.5f);
        pl->vel = MakeVector(0, 0, 0);
        tick(2);
        const bool n2 = land(sx + 8, sy + 2, sz) == TYPE_DIRT;
        park();
        tick(2);
        std::snprintf(d, sizeof(d), "first %d, still %d, again %d", n1, nStay, n2);
        check(n1 && nStay && n2, "@near fires on approach, once, and re-arms when the player leaves", d);

        // Counted in golden cubes: the adder's own @timer .1 relays fire too while it is within 48.
        const TrailerPos T = make(sx + 4, sy, sz + 3, "@timer 0.5; gold 1");
        hud->goldencubes = 0;
        pl->pos = MakeVector(sx + 4.5f, sy + 0.5f, sz + 3.5f + 20.0f);   // 20 blocks: in range
        pl->vel = MakeVector(0, 0, 0);
        secs(3.0f);
        const int near = hud->goldencubes;
        pl->pos = MakeVector(sx + 4.5f, sy + 0.5f, sz + 3.5f + 60.0f);   // 60 blocks: asleep
        pl->vel = MakeVector(0, 0, 0);
        tick(1);
        const int g1 = hud->goldencubes;
        secs(3.0f);
        const int far = hud->goldencubes - g1;
        hud->goldencubes = 10;
        std::snprintf(d, sizeof(d), "in range %d fires in 3 s, at 60 blocks %d", near, far);
        check(near >= 5 && near <= 7 && far == 0, "@timer 0.5 fires every 0.5 s, only within 48 blocks", d);
        park();
        for (const TrailerPos& p : {S, N, T}) put(p.x, p.z, p.y, 0);
        put(sx + 2, sy, sz, 0);
        put(sx + 8, sy + 2, sz, 0);
        tick(2);
    }

    // a real screen tap with a block armed pushes a command block and builds nothing
    {
        const TrailerPos A = make(X, Y, Z, "set 0 2 0 dirt");
        tick(4);
        hud->mode = MODE_BUILD;
        hud->blocktype = TYPE_STONE;
        Camera* cam = World::getWorld->cam;
        const float fx = X + 0.5f, fy = Y + 0.5f, fz = (float)Z + 1.0f;   // its +z face
        for (int k = 0; k < 4; ++k) {
            pl->pos = MakeVector(fx, fy, fz + 3.0f);
            pl->vel = MakeVector(0, 0, 0);
            tick(1);
            const float dx = fx - cam->px, dy = fy - cam->py, dz = fz - cam->pz;
            pl->yaw = std::atan2(dz, dx) * 180.0f / (float)M_PI;
            pl->pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0f / (float)M_PI;
        }
        tick(1);
        const int presses0 = CmdScript::stats().presses;
        ui_pointer(0, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(1);
        ui_pointer(2, SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f); tick(2);
        std::snprintf(d, sizeof(d), "presses +%d, dirt %d, built in front %d", CmdScript::stats().presses - presses0,
                      land(X, Y + 2, Z), land(X, Y, Z + 1));
        check(CmdScript::stats().presses == presses0 + 1 && land(X, Y + 2, Z) == TYPE_DIRT && land(X, Y, Z + 1) == 0,
              "a build tap on a command block pushes it (and builds nothing)", d);
        park();
        put(X, Y + 2, Z, 0);
        tick(2);
    }

    // the frame budget: five 4096-cell fills in one script pause and finish on the next frames
    {
        const CmdScript::Stats s0 = CmdScript::stats();
        const TrailerPos A = make(X - 1, Y - 1, Z - 1, "fill 0 1 0 15 16 15 air; fill 0 1 0 15 16 15 air; "
                                                       "fill 0 1 0 15 16 15 air; fill 0 1 0 15 16 15 air; "
                                                       "fill 0 1 0 15 16 15 air; set 0 1 0 dirt");
        CmdScript::press(A);
        const bool paused = CmdScript::busy(A) && CmdScript::stats().suspended > s0.suspended;
        tick(4);
        std::snprintf(d, sizeof(d), "paused %d, done %d, dirt %d", paused, !CmdScript::busy(A), land(X - 1, Y, Z - 1));
        check(paused && !CmdScript::busy(A) && land(X - 1, Y, Z - 1) == TYPE_DIRT,
              "past 20,000 statements + cells a frame scripts pause and resume", d);
        put(X - 1, Y, Z - 1, 0);
        put(X - 1, Y - 1, Z - 1, 0);
    }

    // a script still waiting when the world closes: nothing of it survives the reload
    const TrailerPos W = make(X, Y, Z, "wait 30; set 0 2 0 dirt");
    CmdScript::press(W);
    tick(2);
    const bool wasGreen = CmdTool::running(W);
    g_app->viewController.setFixedEtime(0.0f);
    save_world();
    if (!quit_to_menu()) return 1;
    std::snprintf(d, sizeof(d), "green before %d; after quit: live %d, green %d", wasGreen, CmdScript::liveCount(), CmdTool::running(W));
    check(wasGreen && CmdScript::liveCount() == 0 && !CmdTool::running(W), "closing the world stops every script", d);
    tick(30);
    if (!open_world(g_opt.world.c_str(), g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(30);
    t = tr();
    std::snprintf(d, sizeof(d), "%s; W record %d, live %d", t ? t->describe().c_str() : "no trailer",
                  t ? t->findCmd(W) : -1, CmdScript::liveCount());
    check(t && t->findCmd(W) >= 0 && CmdScript::liveCount() == 0 && !CmdTool::running(W),
          "after reload: the command block is kept, its run is not", d);
    std::printf("[eden-cmds] %s\n", CmdScript::describe());
    std::printf("[eden-cmds] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --console-selftest  (Stage C / C.1, the command-block language as a console line)
// ---------------------------------------------------------------------------------------------
// dev-console-plan.md §8. World-free first: the shared parser case table; gate 1, the corpus
// (--console-corpus=PATH, default ../port/tools/fixtures/cmds/corpus.txt) parsed with block and
// console options, classified and field-compared; the kVerbs table against the parser's own errors;
// runLine's refusals before a world. Then in a fresh world in --docs: gate 2 (execution equivalence:
// each script pressed on a command block P, then typed with the player standing on P -- the cells,
// player, purse, health, creatures and sky must come out identical), and runLine's jobs, `=`, sink,
// stop, the stand-in registry and the time hook. --console-mutate=anchor (the anchor is the feet cell,
// not the one under them) must FAIL it.
extern "C" {   // port/src/seam/Settings_web.mm -- C.2's gates read the tables the cvars were built from
int         eden_settings_count(void);
const char* eden_settings_key(int i);
const char* eden_settings_label(int i);
const char* eden_settings_group(int i);
int         eden_settings_kind(int i);
float       eden_settings_min(int i);
float       eden_settings_max(int i);
float       eden_settings_def(int i);
float       eden_settings_get(int i);
void        eden_settings_set(int i, float v);
int         eden_settings_enum_count(int i);
const char* eden_settings_enum_label(int i, int j);
int         eden_settings_native_hidden(int i);
int         eden_settings_apply(int i);
int         eden_keybind_count(void);
int         eden_keybind_index(const char* action);
int         eden_keybind_get(int i);
int         eden_keybind_default(int i);
int         eden_keybind_native_hidden(int i);
const char* eden_keybind_action(int i);
int         eden_keybind_action_after(int prev, int code);
void        eden_keybind_set(int i, int code);
}

namespace console_test {

struct TestSink : CmdScript::Sink {
    std::vector<std::pair<int, std::string>> lines;
    void line(int kind, const std::string& text) override { lines.push_back(std::make_pair(kind, text)); }
    bool has(const std::string& t) const {
        for (const auto& l : lines) if (l.second == t) return true;
        return false;
    }
    std::string all() const {
        std::string r;
        for (const auto& l : lines) r += (r.empty() ? "" : " | ") + l.second;
        return r;
    }
};

// The stand-in for the C.2 registry: three verbs, one dotted like a cvar.
const char* const kTestVerbs[] = {"echo", "help", "video.fov"};
int g_extCalls = 0, g_extLast = -1, g_extAnchor[3] = {0, 0, 0};
std::vector<std::string> g_extArgs;
int test_ext_find(const char* v) {
    for (int i = 0; i < 3; ++i) if (!std::strcmp(v, kTestVerbs[i])) return i;
    return -1;
}
void test_ext_run(int i, const CmdScript::ExtCall& c) {
    g_extCalls++;
    g_extLast = i;
    g_extArgs = c.args;
    g_extAnchor[0] = c.ax; g_extAnchor[1] = c.ay; g_extAnchor[2] = c.az;
    if (c.sink) c.sink->line(CmdScript::Sink::OUT, std::string("ext ") + kTestVerbs[i]);
}
const CmdScript::Ext kTestExt = {test_ext_find, test_ext_run};

bool stmt_eq(const CmdScript::Stmt& a, const CmdScript::Stmt& b) {
    if (a.op != b.op || a.type != b.type || a.color != b.color || a.hasPos != b.hasPos || a.f != b.f ||
        a.text != b.text || a.ext != b.ext || a.args != b.args) return false;
    for (int i = 0; i < 6; ++i) if (a.v[i] != b.v[i] || a.abs[i] != b.abs[i]) return false;
    return true;
}
bool any_abs(const CmdScript::Stmt& s) {
    for (int i = 0; i < 6; ++i) if (s.abs[i]) return true;
    return false;
}

// Gate 1 on one corpus line; "" if it holds, else why not.
std::string corpus_line(char cls, const std::string& src) {
    CmdScript::Program blk, con;
    CmdScript::parse(src.c_str(), &blk);
    CmdScript::parseLine(src.c_str(), &con, CmdScript::consoleOptions(&kTestExt));
    const bool bok = blk.errors == 0, cok = con.errors == 0;
    const char got = bok && cok ? 'B' : bok ? 'b' : cok ? 'c' : 'x';
    if (got != cls)
        return std::string("is '") + got + "' (block: " + (bok ? "ok" : blk.firstError) + "; console: " +
               (cok ? "ok" : con.firstError) + ")";
    const auto& b = blk.stmts;
    const auto& c = con.stmts;
    if (cls == 'B') {
        if (b.size() != c.size()) return "statement counts differ";
        for (size_t i = 0; i < b.size(); ++i)
            if (!stmt_eq(b[i], c[i])) return "statement " + std::to_string(i + 1) + " differs";
    } else if (cls == 'b') {
        size_t k = src.find_first_not_of(" \t;");
        if (k == std::string::npos || src[k] != '@') return "a blocks-only line without a trigger header";
        if (con.errors != 1 || con.firstError != "Triggers only work in a command block")
            return "console error is '" + con.firstError + "'";
        if (c.size() != b.size() + 1) return "statement counts differ";
        for (size_t i = 0; i < b.size(); ++i)
            if (!stmt_eq(b[i], c[i + 1])) return "statement " + std::to_string(i + 2) + " differs";
    } else if (cls == 'c') {
        if (b.size() != c.size()) return "statement counts differ";
        for (size_t i = 0; i < b.size(); ++i) {
            if (b[i].op == CmdScript::OP_BAD) {
                if (c[i].op != CmdScript::OP_EXT && !any_abs(c[i]))
                    return "statement " + std::to_string(i + 1) + " is console-only without `=` or a registry verb";
            } else if (!stmt_eq(b[i], c[i])) {
                return "statement " + std::to_string(i + 1) + " differs";
            }
        }
    } else if (b.size() == c.size()) {
        for (size_t i = 0; i < b.size(); ++i)
            if (b[i].op != CmdScript::OP_BAD && c[i].op != CmdScript::OP_BAD && !stmt_eq(b[i], c[i]))
                return "statement " + std::to_string(i + 1) + " differs";
    }
    return "";
}

// One argument a verb accepts, by kind.
const char* sample(int kind) {
    switch (kind) {
    case CmdScript::ARG_COORD: return "1";
    case CmdScript::ARG_BLOCK: return "stone";
    case CmdScript::ARG_COLOR: return "7";
    case CmdScript::ARG_CREATURE: return "moof";
    case CmdScript::ARG_NUMBER: return "1";   // sound takes whole numbers only
    default: return "hi";
    }
}

// Gate 2's snapshot: the site's cells, the player, the purse, health, creatures, the sky, the toast.
struct Snap {
    std::vector<int> cells;
    float px, py, pz;
    int gold, creatures;
    float life;
    bool night;
    std::string toast;
};

}  // namespace console_test
void mkdirs(const std::string& path);   // defined with main()

int run_console_selftest() {
    using namespace cmds_test;
    using namespace console_test;
    using CS = CmdScript;
    g_selftestTag = "eden-console";
    CS::testAnchorFeet = g_opt.consoleMutate == "anchor";
    Console::testNoBridge = g_opt.consoleMutate == "noreg";
    CS::trace = g_opt.cmdsTrace;
    char d[1024];

    // ---- the shared literal-script table (block options)
    cmds_parser_cases();

    // ---- gate 1: the corpus
    {
        std::FILE* f = std::fopen(g_opt.consoleCorpus.c_str(), "rb");
        check(f != NULL, "gate 1: the corpus opens", g_opt.consoleCorpus.c_str());
        int lines = 0, bad = 0, counts[4] = {0, 0, 0, 0};
        std::string why;
        char buf[2048];
        while (f && std::fgets(buf, sizeof(buf), f)) {
            std::string l(buf);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();   // CRLF checkouts
            if (l.empty() || l[0] == '#') continue;
            const char cls = l[0];
            const char* kinds = "Bbcx";
            const char* at = std::strchr(kinds, cls);
            if (!at || (l.size() > 1 && l[1] != ' ')) { ++bad; why += "unreadable: " + l + "; "; continue; }
            const std::string src = l.size() > 2 ? l.substr(2) : std::string();
            ++lines;
            counts[at - kinds]++;
            const std::string r = corpus_line(cls, src);
            if (!r.empty()) {
                ++bad;
                if (bad <= 6) why += "'" + src + "' " + r + "; ";
            }
        }
        if (f) std::fclose(f);
        std::snprintf(d, sizeof(d), "%d lines (B %d, b %d, c %d, x %d), %d wrong %s", lines, counts[0], counts[1],
                      counts[2], counts[3], bad, why.c_str());
        check(f && lines >= 100 && bad == 0 && counts[0] && counts[1] && counts[2] && counts[3],
              "gate 1: block and console parse the corpus alike", d);
    }

    // ---- the verb table is the parser's: every row parses, every "needs" error is the row's
    {
        size_t n = 0;
        const CS::Verb* vs = CS::verbs(&n);
        std::string why;
        for (size_t i = 0; i < n; ++i) {
            const CS::Verb& v = vs[i];
            std::string line = v.name;
            for (int k = 0; k < 9 && v.kinds[k]; ++k) line += std::string(" ") + sample(v.kinds[k]);
            CS::Program p;
            CS::parse(line.c_str(), &p);
            if (p.errors || p.stmts.size() != 1) why += line + " -> " + p.firstError + "; ";
            if (*v.usage && std::strcmp(v.name, "say")) {
                CS::parse(v.name, &p);
                const std::string want = std::string(v.name) + " needs " + (v.needs ? v.needs : v.usage);
                if (p.errors != 1 || p.firstError != want) why += std::string(v.name) + " -> '" + p.firstError + "' not '" + want + "'; ";
            }
            if (v.kinds[0] == CS::ARG_COORD || v.kinds[0] == CS::ARG_CREATURE) {
                line = std::string(v.name) + (v.kinds[0] == CS::ARG_CREATURE ? " moof 1" : " 1");
                CS::parse(line.c_str(), &p);
                const std::string want = std::string(v.name) + " needs " + v.usage;
                if (p.firstError != want) why += line + " -> '" + p.firstError + "' not '" + want + "'; ";
            }
        }
        std::snprintf(d, sizeof(d), "%zu verbs %s", n, why.c_str());
        check(n == 22 && why.empty(), "kVerbs: every verb parses; its errors are its usage", d);
        check(CS::findVerb("FILL") && CS::findVerb("Unless") && !CS::findVerb("video.fov") && !CS::findVerb("echo"),
              "findVerb: case-insensitive, built-ins only", "");
    }

    // ---- C.9 / C.10: the player's README and the commands reference name nothing that is gone
    {
        Console::init();
        // every `backticked` token whose first word starts with a letter must be a verb or a variable
        // (a ';' separates statements); the verbs the roadmap promises must be named
        const std::string text = eden_player_readme_text();
        std::string why;
        std::set<std::string> named;
        for (size_t at = text.find('`'); at != std::string::npos;) {
            const size_t end = text.find('`', at + 1);
            if (end == std::string::npos) { why += "unbalanced backtick; "; break; }
            std::string tok = text.substr(at + 1, end - at - 1);
            at = text.find('`', end + 1);
            size_t p = 0;
            while (p <= tok.size()) {
                size_t q = tok.find(';', p);
                if (q == std::string::npos) q = tok.size();
                std::string st = tok.substr(p, q - p);
                p = q + 1;
                size_t b = st.find_first_not_of(' ');
                if (b == std::string::npos || !std::isalpha((unsigned char)st[b])) continue;
                const std::string w = st.substr(b, st.find(' ', b) == std::string::npos ? std::string::npos : st.find(' ', b) - b);
                named.insert(w);
                if (!CS::findVerb(w.c_str()) && Console::cvarIndex(w.c_str()) < 0) {
                    bool reg = false;
                    for (size_t i = 0; i < Console::commandCount(); ++i)
                        if (w == Console::commandAt(i).name) reg = true;
                    if (!reg) why += "`" + w + "` ; ";
                }
            }
        }
        for (const char* must : {"help", "set", "fill", "tp", "day", "night", "spawn", "video.fov", "toggle", "reset", "where", "anchor"})
            if (!named.count(must)) why += std::string("missing ") + must + "; ";
        check(why.empty(), "README.txt: every command and variable it names exists, the promised ones are named", why.c_str());
        const std::string rd = std::string(eden_platform_documents_root()) + "/readme-test";
        mkdirs(rd);
        std::remove((rd + "/README.txt").c_str());
        const int w1 = eden_player_readme_write(rd.c_str()), w2 = eden_player_readme_write(rd.c_str());
        std::string back;
        if (std::FILE* f = std::fopen((rd + "/README.txt").c_str(), "rb")) {
            char b[512]; size_t n;
            while ((n = std::fread(b, 1, sizeof(b), f)) > 0) back.append(b, n);
            std::fclose(f);
        }
        {   // a stale version line is rewritten; a current file with edits is left alone
            if (std::FILE* f = std::fopen((rd + "/README.txt").c_str(), "wb")) { std::fputs("Emod README v0\nold\n", f); std::fclose(f); }
        }
        const int w3 = eden_player_readme_write(rd.c_str());
        std::snprintf(d, sizeof(d), "first %d, again %d, after a stale one %d, bytes %zu", w1, w2, w3, back.size());
        check(w1 == 1 && w2 == 0 && w3 == 1 && back == text, "README.txt: written once, idempotent, rewritten only when its version changes", d);

        // docs/console-reference.md: every verb, console verb and variable group appears in it
        std::string doc;
        if (std::FILE* f = std::fopen(g_opt.consoleRef.c_str(), "rb")) {
            char b[4096]; size_t n;
            while ((n = std::fread(b, 1, sizeof(b), f)) > 0) doc.append(b, n);
            std::fclose(f);
        }
        std::string miss;
        size_t nv = 0;
        const CS::Verb* vs = CS::verbs(&nv);
        for (size_t i = 0; i < nv; ++i)
            if (doc.find(std::string("`") + vs[i].name + "`") == std::string::npos) miss += std::string(vs[i].name) + " ";
        for (size_t i = 0; i < Console::commandCount(); ++i)
            if (doc.find(std::string("`") + Console::commandAt(i).name + "`") == std::string::npos) miss += std::string(Console::commandAt(i).name) + " ";
        std::set<std::string> groups;
        for (size_t i = 0; i < Console::cvarCount(); ++i) {
            const std::string n = Console::cvarAt(i)->name;
            groups.insert(n.substr(0, n.find('.') + 1));
            if (doc.find("`" + n + "`") == std::string::npos) miss += n + " ";
        }
        for (const std::string& g : groups)
            if (doc.find("`" + g + "`") == std::string::npos) miss += g + " ";
        std::snprintf(d, sizeof(d), "%s: %zu bytes; missing: %s (run port/tools/console-reference.py)", g_opt.consoleRef.c_str(), doc.size(), miss.c_str());
        check(!doc.empty() && miss.empty(), "console-reference.md names every verb, variable and group", d);
    }

    // ---- what only the options change
    {
        CS::Program b, c;
        CS::parse("tp =1 0 0", &b);
        CS::parseLine("tp =1 0 0", &c, CS::consoleOptions(&kTestExt));
        std::snprintf(d, sizeof(d), "block '%s', console errors %d", b.firstError.c_str(), c.errors);
        check(b.firstError == "Not a coordinate: =1" && c.errors == 0 && c.stmts[0].abs[0] && !c.stmts[0].abs[1],
              "`=N` is a console coordinate only", d);
        CS::parseLine("fill =1 2 =3 4 =5 6 stone", &c, CS::consoleOptions(&kTestExt));
        const CS::Stmt& s = c.stmts[0];
        check(c.errors == 0 && s.abs[0] && !s.abs[1] && s.abs[2] && !s.abs[3] && s.abs[4] && !s.abs[5] && s.v[0] == 1 &&
              s.v[5] == 6, "abs[] marks exactly the `=` coordinates", c.firstError.c_str());
        CS::parse("echo hi", &b);
        CS::parseLine("video.fov 95", &c, CS::consoleOptions(&kTestExt));
        std::snprintf(d, sizeof(d), "block '%s'; console op %d ext %d args %zu", b.firstError.c_str(), c.stmts[0].op,
                      c.stmts[0].ext, c.stmts[0].args.size());
        check(b.firstError == "Unknown command: echo" && c.stmts[0].op == CS::OP_EXT && c.stmts[0].ext == 2 &&
              c.stmts[0].args == std::vector<std::string>{"95"} && c.ext == &kTestExt,
              "registry verbs are OP_EXT in the console, unknown in a block", d);
        CS::parseLine("@step;say hi", &c, CS::consoleOptions(&kTestExt));
        check(c.errors == 1 && c.firstError == "Triggers only work in a command block", "the console refuses triggers",
              c.firstError.c_str());
    }

    // ---- runLine before a world: the parse refusals still come first, then "No world is open"
    {
        TestSink sk;
        const int rBad = CS::runLine("set 1 1 1 glass; fill 0 0 0 9 2 9 glas; frob", 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        const bool msgs = sk.has("2: Unknown block: glas") && sk.has("3: Unknown command: frob") && sk.has("line not run");
        const std::string longLine(512, 'x');
        const int rLong = CS::runLine(longLine.c_str(), 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        const int rEmpty = CS::runLine(" ;; ", 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        const int rNone = CS::runLine("say hi", 0, 0, 0, CS::SRC_CONSOLE, &sk, &kTestExt);
        std::snprintf(d, sizeof(d), "bad %d long %d empty %d noworld %d | %s", rBad, rLong, rEmpty, rNone, sk.all().c_str());
        check(rBad == CS::RUN_BAD && msgs && rLong == CS::RUN_LONG && rEmpty == CS::RUN_EMPTY &&
              rNone == CS::RUN_NO_WORLD && sk.has("No world is open"),
              "runLine: a bad line lists every error and runs nothing", d);
    }

    // ---- a world
    const char* name = g_opt.world.empty() ? "console-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    if (g_opt.haveAt) eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 4);
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    Hud* hud = World::getWorld->hud;
    Player* pl = World::getWorld->player;
    const PlayerState ps = player_state();
    int ox, oy, oz;
    if (!open_air((int)ps.x - 50, (int)ps.z, 16, 12, 16, &ox, &oy, &oz)) { check(false, "an open-air test site", ""); return 1; }
    const int PX = ox + 8, PY = oy + 3, PZ = oz + 8;   // the command block P, engine order
    std::printf("[eden-console] site %d,%d,%d, P %d,%d,%d (engine order)\n", ox, oy, oz, PX, PY, PZ);
    void (*const defaultTime)(float) = CS::timeHook;

    // Every gate-2 run starts from the same site: the box air, P holding `script`, stone at +2, grass at
    // +3, a command block at +5 (run's target), the player standing on P, purse 10, life 0.5, day sky.
    auto setup = [&](const char* script) {
        CS::stop(CS::STOP_ALL);
        tick(1);
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 12; ++b)
                for (int c = 0; c < 16; ++c)
                    if (land(ox + a, oy + b, oz + c) != 0) put(ox + a, oy + b, oz + c, 0);
        put(PX + 2, PY, PZ, TYPE_STONE);
        ter()->setColor(PX + 2, PZ, PY, 0);
        put(PX + 3, PY, PZ, TYPE_GRASS);
        ter()->setColor(PX + 3, PZ, PY, 0);
        make(PX + 5, PY, PZ, "set 0 2 0 brick 7");
        const TrailerPos p = make(PX, PY, PZ, script);
        pl->pos = MakeVector(PX + 0.5f, PY + 1 + pl->boxheight / 2 + 0.001f, PZ + 0.5f);
        pl->vel = MakeVector(0, 0, 0);
        pl->life = 0.5f;
        hud->goldencubes = 10;
        defaultTime(0.5f);
        return p;
    };
    auto snap = [&](Snap* s, int creatures0) {
        s->cells.clear();
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 12; ++b)
                for (int c = 0; c < 16; ++c)
                    s->cells.push_back(land(ox + a, oy + b, oz + c) * 256 + paint(ox + a, oy + b, oz + c));
        s->gold = hud->goldencubes;
        s->life = pl->life;
        (void)creatures0;
        s->night = v_equals(ter()->final_skycolor, colorTable[54]);
    };
    auto diff = [&](const Snap& a, const Snap& b) {
        std::string r;
        for (size_t i = 0; i < a.cells.size() && r.empty(); ++i)
            if (a.cells[i] != b.cells[i]) {
                const int a0 = (int)i / (12 * 16), b0 = ((int)i / 16) % 12, c0 = (int)i % 16;
                r = "cell P+" + std::to_string(ox + a0 - PX) + "," + std::to_string(oy + b0 - PY) + "," +
                    std::to_string(oz + c0 - PZ) + " " + std::to_string(a.cells[i] >> 8) + ":" + std::to_string(a.cells[i] & 255) +
                    " vs " + std::to_string(b.cells[i] >> 8) + ":" + std::to_string(b.cells[i] & 255);
            }
        if (std::fabs(a.px - b.px) > 0.001f || std::fabs(a.py - b.py) > 0.001f || std::fabs(a.pz - b.pz) > 0.001f) r += " player";
        if (a.gold != b.gold) r += " gold " + std::to_string(a.gold) + " vs " + std::to_string(b.gold);
        if (std::fabs(a.life - b.life) > 1e-4f) r += " life";
        if (a.creatures != b.creatures) r += " creatures " + std::to_string(a.creatures) + " vs " + std::to_string(b.creatures);
        if (a.night != b.night) r += " sky";
        if (a.toast != b.toast) r += " toast '" + a.toast + "' vs '" + b.toast + "'";
        return r;
    };

    // ---- gate 2: execution equivalence
    {
        const char* scripts[] = {
            "set 0 2 0 glass; fill 1 0 1 3 2 2 brick 19; paint 2 0 0 30",
            "set 0 0 0 glass; set 0 1 0 dirt; set 1 1 0 dirt; set 2 0 0 air",
            "if 2 0 0 stone; set -1 1 0 lamp 9; unless 3 0 0 grass; set -2 1 0 lamp 9",
            "if 2 0 0 grass; if 3 0 0 grass; set 0 3 0 brick; set 0 4 0 brick",
            "run 5 0 0",
            "gold 1; run 0 0 0",
            "tp 0 6 3",
            "gold 7; heal",
            "say HELLO there",
            "fill -3 0 -3 -1 2 -1 cobble 12",
            "set 1 0 -1 glass; wait 0.5; set 1 1 -1 glass; wait 0.2; fill -1 3 -1 1 3 1 ice",
            "spawn moof 4 1 4",
            "night",
            "set 4 0 4 lamp 12; set 4 1 4 lamp 3; set 4 0 4 air",
            "hurt 30; sound 8; flash",
            "fill -2 -1 -2 2 -1 2 stone; fill -1 -1 -1 1 -1 1 air",
        };
        int same = 0, total = 0, changed = 0;
        std::string why;
        bool anchorOk = true;
        for (const char* sc : scripts) {
            ++total;
            Snap A, B, base;
            const bool sayIt = std::strncmp(sc, "say", 3) == 0;
            // As a command block: press P.
            const TrailerPos P0 = setup(sc);
            int c0 = CountActiveCreatures();
            base.px = pl->pos.x; base.py = pl->pos.y; base.pz = pl->pos.z;
            snap(&base, c0);
            base.creatures = 0;
            CS::press(P0);
            // Creatures: right after the run, before a frame lets ambient spawning move the count.
            const int cA = CountActiveCreatures() - c0;
            A.px = pl->pos.x; A.py = pl->pos.y; A.pz = pl->pos.z;
            A.toast = sayIt ? toast() : "";
            secs(1.5f);
            snap(&A, c0);
            A.creatures = cA;
            if (!diff(base, A).empty()) ++changed;   // (a toast counts: base's is "")
            // Typed, standing on P.
            setup(sc);
            c0 = CountActiveCreatures();
            int ax, ay, az;
            CS::playerAnchor(&ax, &ay, &az);
            anchorOk = anchorOk && ax == PX && ay == PY && az == PZ;
            TestSink sk;
            const int r = CS::runLine(sc, ax, ay, az, CS::SRC_CONSOLE, &sk, NULL);
            const int cB = CountActiveCreatures() - c0;
            B.px = pl->pos.x; B.py = pl->pos.y; B.pz = pl->pos.z;
            B.toast = sayIt ? toast() : "";
            secs(1.5f);
            snap(&B, c0);
            B.creatures = cB;
            const std::string dd = r < CS::RUN_DONE ? "runLine " + std::to_string(r) + " " + sk.all() : diff(A, B);
            if (dd.empty()) ++same;
            else why += std::string("'") + sc + "': " + dd + "; ";
        }
        std::snprintf(d, sizeof(d), "%d/%d identical, %d changed the world %s", same, total, changed, why.c_str());
        // Only "hurt 30; sound 8; flash" leaves nothing to compare (Health is off in a new world).
        check(same == total && changed >= total - 1, "gate 2: a typed line = the same script pressed on a block", d);
        check(anchorOk, "the anchor is the cell under the player's feet", "");
    }

    // ---- runLine in play
    {
        // a line with a bad statement runs nothing, even its good ones
        setup("");
        TestSink sk;
        int r = CS::runLine("set 1 1 1 glass; fill 0 0 0 9 2 9 glas", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        std::snprintf(d, sizeof(d), "r %d cell %d | %s", r, land(PX + 1, PY + 1, PZ + 1), sk.all().c_str());
        check(r == CS::RUN_BAD && land(PX + 1, PY + 1, PZ + 1) == 0 && sk.has("2: Unknown block: glas") && sk.has("line not run"),
              "in play: a bad line changes nothing", d);

        // `=` absolute, mixed with relative, from an anchor far away
        sk.lines.clear();
        char line[160];
        std::snprintf(line, sizeof(line), "set =%d =%d =%d glass; set =%d 5 =%d brick", PX + 1, PY + 4, PZ, PX, PZ);
        r = CS::runLine(line, PX - 40, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        std::snprintf(d, sizeof(d), "r %d glass %d brick %d | %s", r, land(PX + 1, PY + 4, PZ), land(PX, PY + 5, PZ), sk.all().c_str());
        check(r == CS::RUN_DONE && land(PX + 1, PY + 4, PZ) == TYPE_GLASS && land(PX, PY + 5, PZ) == TYPE_BRICK &&
              sk.has("2 cells set, 0 refused"), "`=` coordinates are absolute; the rest count from the anchor", d);

        // a job: prints its cells, waits, finishes, says so
        sk.lines.clear();
        r = CS::runLine("set 1 0 1 glass; wait 0.3; set 1 1 1 glass", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const bool waiting = r > 0 && land(PX + 1, PY + 1, PZ + 1) == 0 && CS::jobs().size() == 1 && CS::liveCount() == 0;
        secs(0.6f);
        const std::string jn = "[job " + std::to_string(r) + "]";
        std::snprintf(d, sizeof(d), "r %d waiting %d cell %d jobs %zu | %s", r, waiting, land(PX + 1, PY + 1, PZ + 1),
                      CS::jobs().size(), sk.all().c_str());
        check(waiting && land(PX + 1, PY + 1, PZ + 1) == TYPE_GLASS && CS::jobs().empty() && sk.has("1 cell set, 0 refused") &&
              sk.has(jn + " waiting 0.3 s") && sk.has(jn + " done"),
              "a waiting line is a job outside the 32; it finishes and says so", d);

        // four jobs at most; a line with no wait still runs; stop N, stop jobs, stop all
        sk.lines.clear();
        int ids[4];
        for (int i = 0; i < 4; ++i) ids[i] = CS::runLine("wait 5; set 0 6 0 brick", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const int r5 = CS::runLine("wait 1", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const int rNo = CS::runLine("set 1 6 0 brick", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const size_t j4 = CS::jobs().size();
        const int s1 = CS::stop(ids[0]);
        const size_t j3 = CS::jobs().size();
        const TrailerPos Q = make(PX - 3, PY, PZ, "wait 5; set 0 1 0 brick");
        CS::press(Q);
        const int sJobs = CS::stop(CS::STOP_JOBS);
        const bool qBusy = CS::busy(Q);
        CS::runLine("wait 5", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        const int sAll = CS::stop(CS::STOP_ALL);
        secs(6.0f);
        std::snprintf(d, sizeof(d), "ids %d %d %d %d, 5th %d, nowait %d, jobs %zu -> stop1 %d -> %zu, stopjobs %d (block busy %d), "
                      "stopall %d; cells %d %d %d", ids[0], ids[1], ids[2], ids[3], r5, rNo, j4, s1, j3, sJobs, qBusy, sAll,
                      land(PX, PY + 6, PZ), land(PX + 1, PY + 6, PZ), land(PX - 3, PY + 1, PZ));
        check(ids[0] > 0 && ids[3] == ids[0] + 3 && r5 == CS::RUN_FULL && rNo == CS::RUN_DONE && j4 == 4 && s1 == 1 &&
              j3 == 3 && sJobs == 3 && qBusy && sAll == 2 && !CS::busy(Q) && CS::jobs().empty() &&
              land(PX, PY + 6, PZ) == 0 && land(PX + 1, PY + 6, PZ) == TYPE_BRICK && land(PX - 3, PY + 1, PZ) == 0 &&
              sk.has("[job " + std::to_string(ids[0]) + "] stopped"),
              "jobs: 4 at most, stop N / jobs / all; blocks only stop with all", d);

        // say: a toast and a line; a `run` with no block there says so
        sk.lines.clear();
        CS::runLine("say ping; run 9 9 9", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        std::snprintf(d, sizeof(d), "toast '%s' | %s", toast().c_str(), sk.all().c_str());
        char none[64];
        std::snprintf(none, sizeof(none), "No command block at %d %d %d", PX + 9, PY + 9, PZ + 9);
        check(toast() == "ping" && sk.has("ping") && sk.has(none), "say toasts and prints; a missing run target is reported", d);

        // the registry hook: OP_EXT runs with the anchor and the words, and `if` gates it
        sk.lines.clear();
        const int calls0 = g_extCalls;
        r = CS::runLine("echo hi there; if 2 0 0 grass; echo no", PX, PY, PZ, CS::SRC_CONSOLE, &sk, &kTestExt);
        std::snprintf(d, sizeof(d), "r %d calls %d last %d args %zu anchor %d,%d,%d | %s", r, g_extCalls - calls0, g_extLast,
                      g_extArgs.size(), g_extAnchor[0], g_extAnchor[1], g_extAnchor[2], sk.all().c_str());
        check(r == CS::RUN_DONE && g_extCalls == calls0 + 1 && g_extLast == 0 &&
              g_extArgs == std::vector<std::string>{"hi", "there"} && g_extAnchor[0] == PX && g_extAnchor[1] == PY &&
              g_extAnchor[2] == PZ && sk.has("ext echo"), "OP_EXT calls the registry with the anchor and its words", d);

        // the time hook: blocks and the console go through it together
        static std::vector<float> times;
        times.clear();
        CS::timeHook = [](float t) { times.push_back(t); };
        CS::runLine("night; time 0.3", PX, PY, PZ, CS::SRC_CONSOLE, &sk, NULL);
        CS::press(make(PX - 3, PY, PZ + 3, "midnight"));
        tick(1);
        CS::timeHook = defaultTime;
        std::snprintf(d, sizeof(d), "%zu calls: %.2f %.2f %.2f", times.size(), times.size() > 0 ? times[0] : -1.0f,
                      times.size() > 1 ? times[1] : -1.0f, times.size() > 2 ? times[2] : -1.0f);
        check(times.size() == 3 && times[0] == 0.75f && std::fabs(times[1] - 0.3f) < 1e-6f && times[2] == 0.0f,
              "day/night/time call CmdScript::timeHook from a line and a block", d);
        setup("");
    }

    // ---- gates 3-5 (C.2): cvars, completion / help / history, gating
    {
        using namespace Console;
        init();
        auto lowerS = [](std::string t) { for (char& ch : t) ch = (char)std::tolower((unsigned char)ch); return t; };
        auto numS = [](double v) { char b[48]; std::snprintf(b, sizeof(b), "%g", v); return std::string(b); };
        // Run one console line; the output is the scrollback it left.
        auto con = [&](const char* line, std::string* out = NULL) {
            Console::clear();
            const int r = Console::run(line);
            if (out) {
                out->clear();
                for (const Line& l : scrollback()) *out += (out->empty() ? "" : " | ") + l.text;
            }
            return r;
        };
        auto has = [](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
#ifdef EDEN_DIAGNOSTICS
        const bool diag = true;
#else
        const bool diag = false;
#endif
        std::string out;
        // The player stands on P (setup), stone at +2 and grass at +3, so the anchor is P.
        setup("");

        // gate 3: every kSettings row is a cvar named group.key with the row's metadata
        {
            std::string why;
            int rows = 0, present = 0;
            for (int i = 0; i < eden_settings_count(); ++i) {
                if (eden_settings_kind(i) == 4) continue;   // MP.8: KIND_TEXT rows are not cvars
                const std::string name = lowerS(eden_settings_group(i)) + "." + eden_settings_key(i);
                const int ci = cvarIndex(name.c_str());
                const bool expect = diag || !eden_settings_native_hidden(i);
                ++rows;
                if (ci < 0) { if (expect) why += name + " missing; "; continue; }
                if (!expect) { why += name + " should be absent; "; continue; }
                ++present;
                const Cvar* c = cvarAt(ci);
                if (c->kind != eden_settings_kind(i) || c->min != eden_settings_min(i) || c->max != eden_settings_max(i) ||
                    c->def != eden_settings_def(i) || c->apply != eden_settings_apply(i)) why += name + " metadata; ";
                const float v = eden_settings_get(i);
                const std::string want = c->kind == KIND_TOGGLE ? (v != 0 ? "on" : "off")
                                       : c->kind == KIND_ENUM ? std::string(eden_settings_enum_label(i, (int)std::lround(v)))
                                       : numS(v);
                if (cvarValue(ci) != want) why += name + " value '" + cvarValue(ci) + "' not '" + want + "'; ";
            }
            std::snprintf(d, sizeof(d), "%d rows, %d cvars %s", rows, present, why.substr(0, 300).c_str());
            check(why.empty() && rows >= 40, "gate 3: every Settings row is a cvar group.key with its metadata", d);
        }

        // set / clamp / persist (through eden_settings_set), print, enum by label and by index
        {
            const int fov = cvarIndex("video.fov"), sh = cvarIndex("video.shadows");
            const int fovRow = [&] { for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), "fov")) return i; return -1; }();
            const int shRow = [&] { for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), "shadows")) return i; return -1; }();
            check(fov >= 0 && sh >= 0 && fovRow >= 0 && shRow >= 0, "video.fov and video.shadows exist", "");
            if (fov >= 0 && sh >= 0 && fovRow >= 0 && shRow >= 0) {
                std::string o1, o2, o3, o4, o5, o6;
                con("video.fov", &o1);
                con("video.fov 95", &o2);
                const float after95 = eden_settings_get(fovRow);
                con("video.fov 500", &o3);
                const float after500 = eden_settings_get(fovRow);
                con("video.fov abc", &o4);
                std::snprintf(d, sizeof(d), "'%s' | '%s' (%g) | '%s' (%g) | '%s'", o1.c_str(), o2.c_str(), after95, o3.c_str(), after500, o4.c_str());
                check(has(o1, "video.fov = 80") && has(o1, "60..110") && has(o1, "default 80") && after95 == 95 &&
                      has(o2, "video.fov = 95") && after500 == 110 && has(o3, "clamped to 110") && has(o4, "takes a number"),
                      "cvars: read, set, clamp like the slider, refuse text", d);
                con("reset video.fov", &o1);
                const bool fovBack = eden_settings_get(fovRow) == 80;

                con("video.shadows smooth", &o1);
                const float byLabel = eden_settings_get(shRow);
                con("video.shadows 1", &o2);
                const float byIndex = eden_settings_get(shRow);
                con("video.shadows CLASSIC", &o3);
                const float byCase = eden_settings_get(shRow);
                con("video.shadows smooth", &o4);
                con("toggle video.shadows", &o5);
                const float wrapped = eden_settings_get(shRow);
                con("video.shadows bogus", &o6);
                std::snprintf(d, sizeof(d), "label %g index %g case %g wrap %g | '%s' | '%s'", byLabel, byIndex, byCase, wrapped, o5.c_str(), o6.c_str());
                check(fovBack && byLabel == 2 && byIndex == 1 && byCase == 1 && wrapped == 0 && has(o6, "takes one of"),
                      "cvars: an enum by label (any case) or index; toggle steps and wraps; reset restores", d);
                con("reset video.shadows", &o1);
            }
            const int ap = cvarIndex("saves.autosave"), rd = cvarIndex("video.render_radius");
            if (ap >= 0 && rd >= 0) {
                std::string o1, o2;
                con("saves.autosave 2min", &o1);
                const bool two = cvarValue(ap) == "2 min";
                con("video.render_radius 12", &o2);
                const bool next = has(o2, "applies when a world opens") && cvarValue(rd) == "12";
                std::snprintf(d, sizeof(d), "autosave '%s' | '%s'", cvarValue(ap).c_str(), o2.c_str());
                check(two && next, "cvars: `2min` finds `2 min`; render_distance reports 'applies when a world opens'", d);
                con("reset saves.autosave", &o1);
                con("reset video.render_radius", &o1);
            } else check(false, "saves.autosave and video.render_radius exist", "");
            std::string o1, o2, o3;
            con("toggle gameplay.health", &o1);
            const bool flipped = cvarValue(cvarIndex("gameplay.health")) == "off";
            con("toggle gameplay.health", &o2);
            con("toggle video.fov", &o3);
            check(flipped && cvarValue(cvarIndex("gameplay.health")) == "on" && has(o3, "not an on/off"),
                  "toggle flips an on/off variable and refuses a range", o3.c_str());
        }

        // keybinds are cvars: rebind, and the old key stops (both directions)
        {
            const int jump = eden_keybind_index("jump");
            const int def = jump >= 0 ? eden_keybind_default(jump) : 0;
            std::string o1, o2, o3;
            con("keys.jump", &o1);
            con("keys.jump F9", &o2);
            const int nowF9 = jump >= 0 ? eden_keybind_get(jump) : -1;
            const bool oldStops = jump >= 0 && eden_keybind_action_after(-1, def) != jump;
            const bool newFires = jump >= 0 && eden_keybind_action_after(-1, nowF9) == jump;
            con("keys.jump nonsense", &o3);
            con("reset keys.jump", &o1);
            const bool back = jump >= 0 && eden_keybind_get(jump) == def && eden_keybind_action_after(-1, def) == jump;
            std::snprintf(d, sizeof(d), "now %d old stops %d new fires %d back %d | '%s' | '%s'", nowF9, oldStops, newFires, back, o2.c_str(), o3.c_str());
            check(jump >= 0 && nowF9 != def && oldStops && newFires && back && has(o2, "keys.jump = F9") && has(o3, "Not a key name"),
                  "keys.jump rebinds; the old key stops; reset restores", d);
        }

        // world.* / gen.*: read-only ones refuse; world.gold is live; the name is live
        {
            std::string o1, o2, o3;
            const int seed0 = ter()->level_seed;
            con("world.seed 5", &o1);
            con("gen.caves deep", &o2);
            std::snprintf(d, sizeof(d), "'%s' | '%s' | seed %d", o1.c_str(), o2.c_str(), ter()->level_seed);
            check(has(o1, "read-only") && has(o2, "World Properties") && ter()->level_seed == seed0,
                  "world.seed and gen.* are read-only in game and point at World Properties", d);
            con("world.gold 50", &o1);
            const int g50 = hud->goldencubes;
            con("world.gold 5000", &o2);
            const int g999 = hud->goldencubes;
            hud->goldencubes = 10;
            check(g50 == 50 && g999 == 999 && has(o2, "clamped to 999"), "world.gold is the purse, clamped to 0..999", o2.c_str());
            con("world.height", &o1);
            con("world.type", &o2);
            con("world.cmdblocks", &o3);
            std::snprintf(d, sizeof(d), "'%s' | '%s' | '%s'", o1.c_str(), o2.c_str(), o3.c_str());
            check(has(o1, std::to_string(g_world_height).c_str()) && has(o2, "world.type = ") && has(o3, "world.cmdblocks = 2"),
                  "world.height / type / cmdblocks read the open world", d);
        }

        // gate 4: completion, help, history
        {
            Completion f = complete("fi");
            std::string got;
            for (const std::string& m : f.matches) got += (got.empty() ? "" : " ") + m;
            check(got == "fill find fire", "completion: `fi` lists fill find fire", got.c_str());
            Completion v = complete("video.sha");
            check(v.line == "video.shadows ", "completion: `video.sha` -> video.shadows", v.line.c_str());
            Completion b = complete("set 0 0 0 gla");
            check(b.line == "set 0 0 0 glass ", "completion: block names by argument kind", b.line.c_str());
            Completion e = complete("gold 1; video.shadows sm");
            check(e.line == "gold 1; video.shadows Smooth ", "completion: after `;`, a cvar's enum labels", e.line.c_str());
            Completion k = complete("keys.jump F1");
            std::string km;
            for (const std::string& m : k.matches) km += m + " ";
            check(k.matches.size() >= 2 && k.line == "keys.jump F1", "completion: key labels for a keys.* variable (F1, F10, F11, F12)", km.c_str());

            std::string o;
            con("help set", &o);
            const CS::Verb* sv = CS::findVerb("set");
            check(sv && has(o, (std::string(sv->name) + " " + sv->usage).c_str()), "help set = the parser's usage string", o.c_str());
            con("help video.fov", &o);
            check(has(o, "video.fov = ") && has(o, "60..110"), "help on a cvar describes it", o.c_str());
            con("help nonsense", &o);
            check(has(o, "No verb or variable named nonsense"), "help on nothing says so", o.c_str());
            con("find shad", &o);
            check(has(o, "video.shadows") && has(o, "shadow"), "find searches names and descriptions", o.c_str());
            con("cvars video", &o);
            check(has(o, "video.fov = 80") && has(o, "video.shadows = "), "cvars GROUP lists the group", o.c_str());

            historyClear();
            std::string last;
            for (int i = 0; i < 60; ++i) { last = "line " + std::to_string(i); historyAdd(last); }
            historyAdd(last);   // a repeat of the last is not stored
            const bool capped = historyCount() == (size_t)MAX_HISTORY && historyGet(0) == "line 10" && historyGet(MAX_HISTORY - 1) == last;
            historySave();
            historyClear();
            historyLoad();
            check(capped && historyCount() == (size_t)MAX_HISTORY && historyGet(0) == "line 10" && historyGet(MAX_HISTORY - 1) == last,
                  "history keeps 50, skips repeats, survives a restart", numS((double)historyCount()).c_str());
            con("echo hello  world", &o);
            check(historyGet(historyCount() - 1) == "echo hello  world" && o == "hello world", "run() records history; echo prints only here", o.c_str());
        }

        // the verbs on the anchor, chaining with block verbs, jobs
        {
            std::string o;
            con("if 2 0 0 stone; echo yes; if 2 0 0 grass; echo no", &o);
            check(o == "yes", "registry verbs chain and gate like block verbs", o.c_str());
            con("anchor 1 0 0", &o);
            int ax, ay, az;
            currentAnchor(&ax, &ay, &az);
            const bool moved = anchorIsFixed() && ax == PX + 1 && ay == PY && az == PZ;
            con("if 1 0 0 stone; echo anchored", &o);
            const bool rel = o == "anchored";
            con("anchor me", &o);
            currentAnchor(&ax, &ay, &az);
            const bool me = !anchorIsFixed() && ax == PX && ay == PY && az == PZ;
            char ln[96];
            std::snprintf(ln, sizeof(ln), "anchor =%d =%d =%d", PX + 3, PY, PZ);
            con(ln, &o);
            currentAnchor(&ax, &ay, &az);
            const bool absolute = anchorIsFixed() && ax == PX + 3;
            con("where", &o);
            const bool where = has(o, "you stand on") && has(o, "anchor ");
            con("anchor me", &o);
            con("anchor look", &o);
            const bool lookRefusedOrSet = has(o, "Nothing in front") || has(o, "anchor ");
            con("anchor me", &o);
            std::snprintf(d, sizeof(d), "moved %d rel %d me %d abs %d where %d look %d", moved, rel, me, absolute, where, lookRefusedOrSet);
            check(moved && rel && me && absolute && where && lookRefusedOrSet, "anchor X Y Z / =N / me / look; where prints cells", d);

            con("wait 5; set 0 6 0 brick", &o);
            std::string jo, so;
            con("jobs", &jo);
            const size_t waiting = CS::jobs().size();
            con("stop all", &so);
            const bool stopped = CS::jobs().empty();
            con("jobs", &o);
            check(waiting == 1 && has(jo, "[job ") && stopped && has(o, "No jobs waiting"), "jobs lists and stop all stops console jobs", jo.c_str());
            con("set 0 3 0 glas", &o);
            check(has(o, "Unknown block: glas") && has(o, "line not run") && land(PX, PY + 3, PZ) == 0,
                  "a bad line through run() prints its errors and changes nothing", o.c_str());
            con("reset all", &o);
            con("video.fov 95", &o);
            con("reset all", &o);
            const int fovRow = cvarIndex("video.fov");
            const bool kept = fovRow >= 0 && cvarValue(fovRow) == "95";
            con("reset video", &o);
            check(kept && cvarValue(fovRow) == "80" && has(o, "reset"), "reset all asks first; reset GROUP resets the group", o.c_str());
        }

        // gate 5: gating and registration rules
        {
            static bool ran;
            ran = false;
            addCommand("cheattest", "", "test", CF_CHEAT, {}, [](Ctx&) { ran = true; });
            std::string o;
            con("cheattest", &o);
            check(!ran && has(o, "Allow cheats"), "a CF_CHEAT verb is refused without Allow cheats", o.c_str());
            bool gb = false;
            for (size_t i = 0; i < commandCount(); ++i) if (!std::strcmp(commandAt(i).name, "getblock")) gb = true;
            check(gb == diag, "a CF_DIAG verb exists only in a diagnostics build", diag ? "diagnostics on" : "diagnostics off");
            const size_t n0 = commandCount(), v0 = cvarCount();
            addCommand("fill", "", "dup of a block verb", 0, {}, [](Ctx&) {});
            addCommand("help", "", "dup", 0, {}, [](Ctx&) {});
            addCommand("dotted.verb", "", "dotted", 0, {}, [](Ctx&) {});
            Cvar nodot = {}; nodot.name = "nodot"; nodot.kind = KIND_TOGGLE;
            addCvar(nodot);
            Cvar dup = {}; dup.name = "world.gold"; dup.kind = KIND_RANGE;
            addCvar(dup);
            check(commandCount() == n0 && cvarCount() == v0, "duplicate, dotted and block-verb names are dropped", "");
            CS::Program p;
            CS::parse("video.fov 95", &p);
            CS::parse("help", &p);
            check(p.firstError == "Unknown command: help", "a command block cannot see the registry", p.firstError.c_str());
            con("video.fov 70; tp 0 1 0; video.fov", &o);
            check(has(o, "video.fov = 70"), "console verbs and block verbs share a line", o.c_str());
            con("reset video.fov", &o);
        }
        setup("");
    }

    // ---- world.name is live: it round-trips through a save and a reload (last: it closes the world)
    {
        Console::init();
        Console::clear();
        Console::run("world.name Console Renamed");
        const std::string nowName = Console::cvarValue(Console::cvarIndex("world.name"));
        const std::string file = Console::cvarValue(Console::cvarIndex("world.file"));
        Console::run("save");
        const bool reopened = quit_to_menu() && open_world(name, g_opt.height);
        const std::string onDisk = Console::storedWorldName(file);
        std::snprintf(d, sizeof(d), "now '%s', header of %s after save + reload '%s'", nowName.c_str(), file.c_str(), onDisk.c_str());
        check(reopened && nowName == "Console Renamed" && onDisk == "Console Renamed", "world.name round-trips through a save and a reload", d);
        Console::run("world.name Console selftest");   // leave the saved name as the file was made
        Console::run("save");
    }

    std::printf("[eden-console] %s\n", CS::describe());
    std::printf("[eden-console] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}


#if defined(EDEN_DIAGNOSTICS)
// ---- S.0(a) --save-bench=1,10,50,144 (dirty-column save cost; X.1: diagnostics-only) --------------
static std::string g_saveBenchList = "1,10,50,144";
int run_save_bench() {
    g_selftestTag = "eden-savebench";
    const char* name = g_opt.world.empty() ? "S0 W64" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames / 2);
    if (const char* th = std::getenv("SAVEBENCH_THRESHOLD")) { eden_set_save_inplace_threshold(strtoull(th, 0, 10)); }
    const PlayerState ps = player_state();
    const int px = ((int)ps.x / 16) * 16 + 8, pz = ((int)ps.z / 16) * 16 + 8;
    std::printf("[eden-savebench] world=%s player=(%d,%d)\n", name, (int)ps.x, (int)ps.z);
    { const char* r = eden_bench_save(); std::printf("[eden-savebench] warmup(no dirt) %s\n", r); }
    int flip = 0;
    std::string list = g_saveBenchList;
    for (size_t pos = 0; pos < list.size();) {
        size_t c = list.find(',', pos); if (c == std::string::npos) c = list.size();
        int n = atoi(list.substr(pos, c - pos).c_str()); pos = c + 1;
        for (int rep = 0; rep < 3; ++rep) {
            ++flip;
            int side = 1; while (side * side < n) ++side;
            int placed = 0;
            for (int i = 0; i < side && placed < n; ++i)
                for (int j = 0; j < side && placed < n; ++j, ++placed)
                    eden_console_setblock(px + (i - side / 2) * 16, pz + (j - side / 2) * 16, 30, 10 + (flip % 20));
            tick(3);
            const char* r = eden_bench_save();
            std::printf("[eden-savebench] cols=%d rep=%d %s\n", n, rep, r);
        }
    }
    return 0;
}
#endif

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
        memset(chunksToUpdate, TRUE, sizeof(BOOL) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN);
        memset(columnsToUpdate, TRUE, sizeof(BOOL) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
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
    memset(chunksToUpdate, TRUE, sizeof(BOOL) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN);
    memset(columnsToUpdate, TRUE, sizeof(BOOL) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
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
    memset(chunksToUpdate, TRUE, sizeof(BOOL) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN);
    memset(columnsToUpdate, TRUE, sizeof(BOOL) * CHUNKS_PER_SIDE * CHUNKS_PER_SIDE);
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
    for (int y : {1, H / 4, H / 2}) t->burnBlock(tx, tz, y, FALSE);
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
        t->burnBlock(fx, fz, H / 2, FALSE);
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

// ---------------------------------------------------------------------------------------------
// --mp-replay=FILE  (MP.3a: the session world offline; engine plan §15 legs 1, 2, 5)
// ---------------------------------------------------------------------------------------------
// FILE is a server's line stream as one client received it (port/tools/mp-replay.py record, against a
// local edenserver); FILE's `.expect` holds what mp-replay.py computes from the same lines with its own
// decoder (base64 + raw inflate + the burst merge) over flat. The lines go through MpSession's own
// parser (feedLine, no socket), the world events into the overlay before the window loads (§7 step 2),
// then the session world is opened through the menu as a join would.
namespace {
struct MpReplayData {
    bool haveSpawn = false;
    float spawn[3] = {65536.5f, 34.0f, 65536.5f};
    std::vector<eden_net::MpWorldEvent> world;
    int lines = 0;
};
static_assert(sizeof(MpRec) == sizeof(eden_net::SnapzRec), "MpRec is SnapzRec's layout");
extern "C" void eden_set_fire_melts_ice(int on);   // Classes/Terrain.mm (H.8)

bool mp_replay_load(const std::string& path, MpReplayData& r) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "[eden-mp-replay] cannot open %s\n", path.c_str()); return false; }
    eden_net::MpSession s;
    std::string line;
    for (int ch; (ch = std::fgetc(f)) != EOF;) {
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();   // a CRLF checkout (Windows)
            s.feedLine(line);
            r.lines++;
            line.clear();
        } else {
            line += (char)ch;
        }
    }
    std::fclose(f);
    std::vector<eden_net::MpSideEvent> side;
    s.drainSide(side);
    for (const auto& e : side) {
        if (e.kind == eden_net::MpSideEvent::Error) {
            std::fprintf(stderr, "[eden-mp-replay] the replay does not parse: %s\n", e.text.c_str());
            return false;
        }
        if (e.kind == eden_net::MpSideEvent::Spawn) {
            r.haveSpawn = true;
            for (int i = 0; i < 3; ++i) r.spawn[i] = (float)e.v[i];
        }
    }
    s.drainWorld(r.world);
    return true;
}

// beginSession, the replay's world events into the overlay, then the menu's load, as a join's
// Syncing -> Loading steps (§7) would do it.
bool mp_replay_open(const MpReplayData& r, const float at[3]) {
    MpWorld::beginSession("replay", 0, "Replay");
    for (const auto& e : r.world) {
        if (e.kind == eden_net::MpWorldEvent::Snapz)
            MpWorld::mergeSnapz(reinterpret_cast<const MpRec*>(e.recs.data()), e.recs.size());
        else if (e.kind == eden_net::MpWorldEvent::SignP)
            MpWorld::applySign(e.x, e.y, e.z, e.sa, e.sb, e.sc, e.text.c_str());
        else
            MpWorld::applyAction(e.who.c_str(), e.x, e.y, e.z, e.mode, e.extra);
    }
    MpWorld::setSpawn(at[0], at[1], at[2]);
    if (!MpWorld::openSession()) { std::fprintf(stderr, "[eden-mp-replay] openSession refused\n"); return false; }
    return tick_until([] { return game_mode() == 1; }, 60000, "the session world to load");
}

// MP.3b: a socket-free MpWorld::Link. The pacer's clock is simulated (17 ms a frame, advanced in pump),
// so every wait is a frame count and the run is deterministic. A REGION it was told the answer to is
// answered on the next frame (one frame of "latency"), anything else is never answered.
int64_t g_mpFakeMs = 0;
int64_t mp_fake_clock() { return g_mpFakeMs; }
long long mp_chat_clock() { return (long long)g_mpFakeMs; }
inline int mp_lattice(float p) { return 128 * (int)std::floor(p / 128.0f + 0.5f); }

struct ReplayLink : MpWorld::Link {
    std::map<std::pair<int, int>, std::vector<std::vector<eden_net::SnapzRec> > > answers;
    std::vector<std::pair<int, int> > pending;
    std::vector<std::string> sent;
    std::function<void(int, int)> onAsk;   // the leg's hook, right after a REGION line goes out
    bool joinFlow = false;                 // open the session once joinReady(), as MpLink_native's does
    bool released = false;
    void pump() override {
        g_mpFakeMs += 17;
        for (const auto& p : pending) {
            auto it = answers.find(p);
            if (it == answers.end()) continue;
            for (const auto& f : it->second) MpWorld::enqueueSnapz(reinterpret_cast<const MpRec*>(f.data()), f.size());
        }
        pending.clear();
        if (joinFlow && MpWorld::joinReady() && MpWorld::openSession()) joinFlow = false;
    }
    void sendLine(const char* l) override {
        sent.push_back(l);
        int x = 0, z = 0;
        if (std::sscanf(l, "REGION:%d:%d", &x, &z) == 2) {
            pending.push_back(std::make_pair(x, z));
            if (onAsk) onAsk(x, z);
        }
    }
    void release() override { released = true; }   // the leg owns it
    int count(const std::string& l) const { return (int)std::count(sent.begin(), sent.end(), l); }
};

// The replay's SNAPZ frames split into REGION replies (a reply ends at a frame of < 3000 records).
std::vector<std::vector<std::vector<eden_net::SnapzRec> > > mp_bursts(const MpReplayData& r) {
    std::vector<std::vector<std::vector<eden_net::SnapzRec> > > out(1);
    for (const auto& e : r.world) {
        if (e.kind != eden_net::MpWorldEvent::Snapz) continue;
        out.back().push_back(e.recs);
        if (e.recs.size() < 3000) out.emplace_back();
    }
    if (out.back().empty()) out.pop_back();
    return out;
}

std::set<std::string> mp_docs_listing() {
    std::set<std::string> names;
    if (DIR* d = opendir(eden_platform_documents_root())) {
        while (dirent* e = readdir(d)) names.insert(e->d_name);
        closedir(d);
    }
    return names;
}
}  // namespace

int run_mp_replay() {
    g_selftestTag = "eden-mp-replay";
    MpWorld::testNoEvictCommit = g_opt.mpMutate == "noevictcommit";
    MpWorld::testNoLandHook = g_opt.mpMutate == "nolandhook";
    MpWorld::testNoReconcile = g_opt.mpMutate == "noreconcile";
    MpWorld::testEcho = g_opt.mpMutate == "echo";   // MP.4: fails leg 6
    if (g_opt.mpOverlayMb > 0) MpWorld::overlayCapBytes = (size_t)g_opt.mpOverlayMb << 20;
    MpReplayData rep;
    if (!mp_replay_load(g_opt.mpReplay, rep)) return 1;
    std::string ex = g_opt.mpReplay;
    if (ex.size() > 6 && ex.compare(ex.size() - 6, 6, ".lines") == 0) ex.resize(ex.size() - 6);
    ex += ".expect";
    std::map<std::string, std::string> want;
    if (FILE* f = std::fopen(ex.c_str(), "rb")) {
        char b[512];
        while (std::fgets(b, sizeof b, f)) {
            std::string l(b);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            const size_t eq = l.find('=');
            if (!l.empty() && l[0] != '#' && eq != std::string::npos) want[l.substr(0, eq)] = l.substr(eq + 1);
        }
        std::fclose(f);
    } else {
        std::fprintf(stderr, "[eden-mp-replay] no %s (port/tools/mp-replay.py expect --write)\n", ex.c_str());
        return 1;
    }
    size_t snapz = 0;
    for (const auto& e : rep.world) snapz += e.kind == eden_net::MpWorldEvent::Snapz ? e.recs.size() : 0;
    std::printf("[eden-mp-replay] %s: %d lines, %zu world events, %zu SNAPZ records, spawn %.2f %.2f %.2f\n",
                g_opt.mpReplay.c_str(), rep.lines, rep.world.size(), snapz, rep.spawn[0], rep.spawn[1], rep.spawn[2]);
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    g_autosave_seconds = 0;
    char d[320];
    auto hex = [](uint64_t h) { char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return std::string(b); };
    auto report_ws = [&](const char* leg, const MpWorld::WindowState& ws) {
        std::printf("[eden-mp-replay] mp.replay leg=%s hash=%s cells=%d ox=%d oz=%d side=%d missing=%d gblock=%d "
                    "overlay=%zu cells/%zu cols/%zu B dropped=%d\n", leg, hex(ws.hash).c_str(), ws.cells, ws.ox, ws.oz,
                    ws.side, ws.missing, ws.gblockMismatch, MpWorld::overlayCells(), MpWorld::overlayColumns(),
                    MpWorld::overlayBytes(), MpWorld::droppedRecords());
        std::fflush(stdout);
    };
    auto settle = [&]() {   // until the window around the player is resident (a teleport's bulk reload)
        for (int i = 0; i < 1200 && MpWorld::windowState().missing; ++i) tick(1);
        tick(g_opt.frames);
    };
    const std::set<std::string> docs0 = mp_docs_listing();

    // ---- leg 1: the join burst lands as flat + overlay -----------------------------------------
    if (!mp_replay_open(rep, rep.spawn)) return 1;
    settle();
    const MpWorld::WindowState w1 = MpWorld::windowState();
    report_ws("1", w1);
    std::snprintf(d, sizeof d, "hash %s cells %d window (%d,%d) side %d; expect %s cells %s window (%s,%s)",
                  hex(w1.hash).c_str(), w1.cells, w1.ox, w1.oz, w1.side, want["spawn.hash"].c_str(),
                  want["spawn.cells"].c_str(), want["spawn.ox"].c_str(), want["spawn.oz"].c_str());
    check(w1.missing == 0 && w1.gblockMismatch == 0, "leg 1: the whole window is resident and blockarray agrees", d);
    check(hex(w1.hash) == want["spawn.hash"] && std::to_string(w1.cells) == want["spawn.cells"] &&
          std::to_string(w1.ox) == want["spawn.ox"] && std::to_string(w1.oz) == want["spawn.oz"] && w1.side == 18,
          "leg 1: the window equals mp-replay.py's flat + replay", d);
    check(w1.cells > 0, "leg 1: the replay has edits in the window", d);

    // ---- the E11 gates, while in the session ---------------------------------------------------
    {
        Terrain* t = World::getWorld->terrain;
        std::snprintf(d, sizeof d, "height %d radius %d seed %d creatures %d gold %d world_name %s", T_HEIGHT, T_RADIUS,
                      t->level_seed, (int)CREATURES_ON, World::getWorld->hud->goldencubes, [t->world_name UTF8String]);
        check(MpWorld::active() && T_HEIGHT == 256 && T_RADIUS <= 9 && t->level_seed == 0 && !CREATURES_ON &&
              World::getWorld->hud->goldencubes == 0, "gates: 256z, radius <= 9, seed 0, no creatures, no gold", d);
        const int rc = CmdScript::runLine("set 0 0 0 stone", 0, 40, 0, CmdScript::SRC_HARNESS, nullptr, nullptr);
        const int rc2 = CmdScript::runLine("tp 0 5 0", 0, 40, 0, CmdScript::SRC_HARNESS, nullptr, nullptr);
        std::snprintf(d, sizeof d, "set -> %d, tp -> %d", rc, rc2);
        check(rc == CmdScript::RUN_BAD && rc2 == CmdScript::RUN_BAD, "gates: console world commands are refused", d);
        eden_set_fire_melts_ice(1);
        const bool iceBurns = (blockinfo[TYPE_ICE] & IS_FLAMMABLE) != 0;
        eden_set_fire_melts_ice(0);
        check(!iceBurns, "gates: H.8 fire-melts-ice stays off in a session", "");
        World::getWorld->fm->saveWorld();   // the pause menu's Save, the bulk reload's, a warp's: to RAM only
    }

    // ---- leg 2: walk out of the window and back -------------------------------------------------
    {
        Terrain* t = World::getWorld->terrain;
        const float X0 = rep.spawn[0], Z0 = rep.spawn[2], Y0 = 60.0f;
        eden_console_teleport(X0, Y0, Z0);
        tick(4);
        const int pcx = (int)X0 / CHUNK_SIZE, pcz = (int)Z0 / CHUNK_SIZE;
        // Local results nobody sent (what fire, liquids and blasts leave): only the evict-commit keeps them.
        struct Col { int cx, cz; };
        const Col E[3] = {{pcx, pcz}, {pcx - 6, pcz + 2}, {pcx + 4, pcz - 5}};
        for (const Col& c : E) {
            const int bx = c.cx * CHUNK_SIZE, bz = c.cz * CHUNK_SIZE;
            for (int k = 0; k < 5; ++k) t->updateChunks(bx + 2 + k, bz + 3 + 2 * k, 200 - 7 * k, TYPE_BRICK);
            t->updateChunks(bx + 9, bz + 9, 32, TYPE_NONE);
            t->setColor(bx + 2, bz + 3, 200, 17);
        }
        tick(4);
        const MpWorld::WindowState w2a = MpWorld::windowState();
        report_ws("2-before", w2a);
        const int bulk0 = g_bulk_reloads, ring0 = g_ring_columns_landed;
        float wx = X0;
        auto walk = [&](float dx) {
            const int steps = (int)std::ceil(std::fabs(dx) / 0.5f);
            for (int k = 1; k <= steps; ++k) { eden_console_teleport(wx + dx * (float)k / (float)steps, Y0, Z0); tick(1); }
            wx += dx;
        };
        const float W = (float)std::max(320, CHUNK_SIZE * (T_RADIUS + 6));
        walk(W);
        tick(120);
        bool gone = true;
        for (const Col& c : E) gone = gone && !isColumnResident(c.cx * CHUNK_SIZE, c.cz * CHUNK_SIZE);
        int keptType = 0, keptColor = 0;
        MpWorld::overlayCell(E[0].cx * CHUNK_SIZE + 2, 200, E[0].cz * CHUNK_SIZE + 3, &keptType, &keptColor);
        walk(-W);
        settle();
        const MpWorld::WindowState w2b = MpWorld::windowState();
        report_ws("2-after", w2b);
        std::snprintf(d, sizeof d, "edited columns streamed out %d; the overlay holds an edit as %d:%d; ring +%d, bulk %d->%d; "
                      "hash %s -> %s", gone, keptType, keptColor, g_ring_columns_landed - ring0, bulk0, g_bulk_reloads,
                      hex(w2a.hash).c_str(), hex(w2b.hash).c_str());
        check(gone && g_ring_columns_landed > ring0 && g_bulk_reloads == bulk0, "leg 2: the walk streams the edited columns out", d);
        check(w2b.hash == w2a.hash && w2b.cells == w2a.cells && w2b.missing == 0 && w2b.gblockMismatch == 0,
              "leg 2: walking out and back keeps every cell (evict-commit to RAM)", d);
    }

    // ---- leg 3 (MP.3b): the live world -- step 5c, reconcile, the pacer ---------------------------
    {
        Terrain* t = World::getWorld->terrain;
        const float X0 = rep.spawn[0], Z0 = rep.spawn[2], Y0 = 60.0f;
        const auto bursts = mp_bursts(rep);
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        g_mpFakeMs = 1000000;
        MpWorld::resetStats();
        ReplayLink* L = new ReplayLink;
        const int px = mp_lattice(X0), pz = mp_lattice(Z0);
        if (!bursts.empty()) L->answers[std::make_pair(px, pz)] = bursts[0];
        char want0[48];
        std::snprintf(want0, sizeof want0, "REGION:%d:%d", px, pz);

        // 3a: step 5c applies to the resident window, in the frame it drains.
        const int ax = (int)X0 + 6, az = (int)Z0 - 4;
        MpWorld::enqueueAction("server", ax, 45, az, 0, TYPE_BRICK);   // a restore: raw
        MpWorld::enqueueAction("peer", ax, 32, az, 0, TYPE_STONE);     // a peer's build onto grass: no overwrite
        MpWorld::enqueueAction("peer", ax, 46, az, 0, TYPE_COBBLESTONE);
        MpWorld::enqueueAction("peer", ax, 46, az, 3, 21);             // ... and its paint
        tick(1);
        std::snprintf(d, sizeof d, "queued %zu; (%d,45,%d) %d, (%d,32,%d) %d, (%d,46,%d) %d:%d", MpWorld::queuedEvents(), ax, az,
                      t->getLand(ax, az, 45), ax, az, t->getLand(ax, az, 32), ax, az, t->getLand(ax, az, 46), t->getColor(ax, az, 46));
        check(MpWorld::queuedEvents() == 0 && t->getLand(ax, az, 45) == TYPE_BRICK && t->getLand(ax, az, 32) == TYPE_GRASS &&
              t->getLand(ax, az, 46) == TYPE_COBBLESTONE && t->getColor(ax, az, 46) == 21,
              "leg 3: step 5c applies ACTIONs to the resident window the frame they drain (8.2's rules)", d);

        // 3b: a refused edit (a local mine nobody's server accepted) and a re-REGION -> back to flat.
        const int rx = (int)X0 + 12, rz = (int)Z0 - 9;
        t->updateChunks(rx, rz, 32, TYPE_NONE);
        int kx = 0, ky = 0, kz = 0, kt = 0;
        std::sscanf(want["reconcile.keep"].c_str(), "%d:%d:%d:%d", &kx, &ky, &kz, &kt);
        bool kept = false;
        L->onAsk = [&](int, int) {   // changed after the REGION went out: its reply may not know it
            if (kept) return;
            kept = true;
            t->updateChunks(kx, kz, ky, kt);
            MpWorld::noteLocalEdit(kx, ky, kz);
        };
        MpWorld::setLink(L);
        MpWorld::startRegions(true);
        tick_until([] { return MpWorld::stats().reconciles >= 1 && MpWorld::queuedEvents() == 0; }, 600, "the reconcile");
        const MpWorld::Stats s1 = MpWorld::stats();
        const MpWorld::WindowState w3 = MpWorld::windowState();
        report_ws("3-reconcile", w3);
        std::printf("[eden-mp-replay] mp.reconcile sent=%d replies=%d reconciles=%d cells=%d writes=%d frames=%d worst_chunks=%d "
                    "worst_ms=%.2f\n", s1.regionsSent, s1.repliesComplete, s1.reconciles, s1.reconciledCells, s1.residentWrites,
                    s1.applyFrames, s1.worstFrameChunks, s1.worstFrameMs);
        std::snprintf(d, sizeof d, "first line %s (want %s); refused (%d,32,%d) = %d; kept (%d,%d,%d) = %d; restore = %d",
                      L->sent.empty() ? "-" : L->sent[0].c_str(), want0, rx, rz, t->getLand(rx, rz, 32), kx, ky, kz,
                      t->getLand(kx, kz, ky), t->getLand(ax, az, 45));
        check(!L->sent.empty() && L->sent[0] == want0, "leg 3: the first REGION asks the spawn's 128-block lattice point", d);
        check(t->getLand(rx, rz, 32) == TYPE_GRASS && t->getLand(ax, az, 45) == TYPE_NONE,
              "leg 3: a refused edit is back to flat after a re-REGION (reconcile)", d);
        check(t->getLand(kx, kz, ky) == kt, "leg 3: a cell changed after the REGION went out is spared", d);
        std::snprintf(d, sizeof d, "hash %s cells %d; expect %s cells %s; missing %d gblock %d", hex(w3.hash).c_str(), w3.cells,
                      want["reconcile.hash"].c_str(), want["reconcile.cells"].c_str(), w3.missing, w3.gblockMismatch);
        check(hex(w3.hash) == want["reconcile.hash"] && std::to_string(w3.cells) == want["reconcile.cells"] && w3.missing == 0 &&
              w3.gblockMismatch == 0, "leg 3: the window equals mp-replay.py's reconcile model", d);
        std::snprintf(d, sizeof d, "worst frame dirtied %d chunks over %d frames", s1.worstFrameChunks, s1.applyFrames);
        check(s1.worstFrameChunks <= 24 + 6, "leg 3: the play budget caps a frame at 24 chunks (+1 cell)", d);

        // 3c: the lattice switch (> 72 blocks from the point), unanswered asks, the re-ask on return.
        float wx = X0;
        auto walk = [&](float dx) {
            const int steps = (int)std::ceil(std::fabs(dx) / 0.5f);
            for (int k = 1; k <= steps; ++k) { eden_console_teleport(wx + dx * (float)k / (float)steps, Y0, Z0); tick(1); }
            wx += dx;
        };
        char want1[48];
        std::snprintf(want1, sizeof want1, "REGION:%d:%d", mp_lattice(X0 + 80.0f), pz);
        walk(80.0f);
        tick_until([] { return MpWorld::stats().unanswered >= 3; }, 2000, "three unanswered REGIONs");
        const int asked = L->count(want1);
        tick(300);   // 5 s more: the point is given up until it changes
        std::snprintf(d, sizeof d, "%s sent %d then %d times; unanswered %d", want1, asked, L->count(want1), MpWorld::stats().unanswered);
        check(asked == 3 && L->count(want1) == 3 && MpWorld::stats().unanswered == 3,
              "leg 3: walking 80 blocks asks the next lattice point; unanswered 3 times, then it waits", d);
        walk(-80.0f);
        tick_until([] { return MpWorld::stats().reconciles >= 2 && MpWorld::queuedEvents() == 0; }, 600, "the second reconcile");
        settle();
        const MpWorld::WindowState w3b = MpWorld::windowState();
        report_ws("3-return", w3b);
        std::snprintf(d, sizeof d, "%s sent %d times; reconciles %d; kept cell %d; hash %s cells %d, expect %s cells %s", want0,
                      L->count(want0), MpWorld::stats().reconciles, t->getLand(kx, kz, ky), hex(w3b.hash).c_str(), w3b.cells,
                      want["reconcile2.hash"].c_str(), want["reconcile2.cells"].c_str());
        check(L->count(want0) == 2 && MpWorld::stats().reconciles == 2, "leg 3: returning re-asks the spawn's point (never deduplicated)", d);
        // The kept cell was spared only by the REGION it postdated; this reply (which in life would name it,
        // had the server accepted it) doesn't, so it goes.
        check(hex(w3b.hash) == want["reconcile2.hash"] && std::to_string(w3b.cells) == want["reconcile2.cells"] &&
              t->getLand(kx, kz, ky) == TYPE_NONE, "leg 3: a later reply reconciles the spared cell too", d);
        MpWorld::dropLink();
        check(L->released, "leg 3: dropLink releases the link", "");
        delete L;
        MpWorld::clockMs = clock0;
    }

    // ---- leg 6 (MP.4): edits out at the intent sites, a peer's edits in, no echo -----------------
    {
        Terrain* t = World::getWorld->terrain;
        Hud* hud = World::getWorld->hud;
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        ReplayLink* L = new ReplayLink;   // answers nothing: what it was sent is the evidence
        MpWorld::setLink(L);
        MpWorld::resetStats();
        auto edits = [&](size_t from) {   // the ACTION / SIGNP lines sent since `from` (the pacer's REGIONs aside)
            std::vector<std::string> v;
            for (size_t i = from; i < L->sent.size(); ++i)
                if (L->sent[i].rfind("ACTION:", 0) == 0 || L->sent[i].rfind("SIGNP:", 0) == 0) v.push_back(L->sent[i]);
            return v;
        };
        auto joined = [](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& l : v) s += (s.empty() ? "" : " ") + l;
            return s.empty() ? std::string("(none)") : s;
        };
        auto line = [](const char* fmt, int x, int y, int z, int v) {
            char b[64];
            std::snprintf(b, sizeof b, fmt, x, y, z, v);
            return std::string(b);
        };
        const int bx = (int)rep.spawn[0] + 20, bz = (int)rep.spawn[2] - 30, by = 50;
        bool clear = true;
        for (int dx = -6; dx <= 40; ++dx)
            for (int dz = -6; dz <= 16; ++dz)
                for (int y = by - 2; y <= by + 2; ++y) clear = clear && t->getLand(bx + dx, bz + dz, y) == TYPE_NONE;
        check(clear, "leg 6: the test area is air", "");
        const int savedType = hud->blocktype, savedPaint = hud->block_paintcolor;
        size_t n0 = L->sent.size();

        // 6a: a painted build, then a repaint: the cell as built, one line each.
        hud->blocktype = TYPE_BRICK;
        hud->block_paintcolor = 12;
        MpWorld::intentBuild(bx, bz, by);
        MpWorld::intentPaint(bx, bz, by, 30);
        std::vector<std::string> got = edits(n0);
        std::vector<std::string> want6 = {line("ACTION:%d:%d:%d:0:%d", bx, by, bz, TYPE_BRICK),
                                          line("ACTION:%d:%d:%d:3:%d", bx, by, bz, 12), line("ACTION:%d:%d:%d:3:%d", bx, by, bz, 30)};
        check(got == want6, "leg 6: a painted build and a repaint go out as build + paint, paint", joined(got).c_str());

        // 6b: a door: both halves out on the build and on the mine (U1: retail unknown).
        n0 = L->sent.size();
        hud->blocktype = TYPE_DOOR_TOP;
        hud->block_paintcolor = 0;
        MpWorld::intentBuild(bx + 2, bz, by);
        const int bottom = t->getLand(bx + 2, bz, by - 1);
        MpWorld::intentMine(bx + 2, bz, by);
        got = edits(n0);
        want6 = {line("ACTION:%d:%d:%d:0:%d", bx + 2, by - 1, bz, bottom), line("ACTION:%d:%d:%d:0:%d", bx + 2, by, bz, TYPE_DOOR_TOP),
                 line("ACTION:%d:%d:%d:1%.0d", bx + 2, by - 1, bz, 0), line("ACTION:%d:%d:%d:1%.0d", bx + 2, by, bz, 0)};
        check(bottom >= TYPE_DOOR1 && bottom < TYPE_DOOR_TOP && got == want6, "leg 6: a door goes out as both halves, built and mined",
              joined(got).c_str());

        // 6c: E10 -- a burn on a non-flammable block sends nothing (the server would delete it).
        n0 = L->sent.size();
        MpWorld::intentBurn(bx, bz, by);
        got = edits(n0);
        check(got.empty() && !isOnFire(bx, bz, by), "leg 6: burning brick sends nothing", joined(got).c_str());

        // 6d: TNT: the burn out, then the follow-up mine once its fuse runs out on time.
        const int tx = bx + 30;
        n0 = L->sent.size();
        hud->blocktype = TYPE_TNT;
        MpWorld::intentBuild(tx, bz, by);
        MpWorld::intentBurn(tx, bz, by);
        const bool litTnt = isOnFire(tx, bz, by);
        for (int i = 0; i < 600 && isOnFire(tx, bz, by); ++i) tick(1);
        tick(2);
        got = edits(n0);
        want6 = {line("ACTION:%d:%d:%d:0:%d", tx, by, bz, TYPE_TNT), line("ACTION:%d:%d:%d:2%.0d", tx, by, bz, 0),
                 line("ACTION:%d:%d:%d:1%.0d", tx, by, bz, 0)};
        check(litTnt && got == want6 && t->getLand(tx, bz, by) == TYPE_NONE,
              "leg 6: TNT goes out as build, burn, then one mine when the fuse runs out (the blast is not sent)", joined(got).c_str());

        // 6e: a burn whose cell a server restore mines first: no follow-up mine.
        const int wx = bx + 12;
        n0 = L->sent.size();
        hud->blocktype = TYPE_WOOD;
        MpWorld::intentBuild(wx, bz, by);
        MpWorld::intentBurn(wx, bz, by);
        const bool litWood = isOnFire(wx, bz, by);
        MpWorld::enqueueAction("server", wx, by, bz, 1, -1);
        for (int i = 0; i < 600 && isOnFire(wx, bz, by); ++i) tick(1);
        tick(2);
        got = edits(n0);
        want6 = {line("ACTION:%d:%d:%d:0:%d", wx, by, bz, TYPE_WOOD), line("ACTION:%d:%d:%d:2%.0d", wx, by, bz, 0)};
        check(litWood && got == want6 && t->getLand(wx, bz, by) == TYPE_NONE,
              "leg 6: a burn ended by a restore's mine sends no mine of its own", joined(got).c_str());

        // 6f: a sign on the brick: SIGNP with the wire's y = height.
        n0 = L->sent.size();
        const int su = SignTool::use(bx, bz, by, 0, 1, 0, 0.0f);
        SignTool::answer(0, "Hello: MP.4");
        if (GLDialog::active()) GLDialog::dismiss();
        tick(2);
        got = edits(n0);
        char sw[96];
        std::snprintf(sw, sizeof sw, "SIGNP:%d:%d:%d:3:45:%d:Hello: MP.4", bx, by, bz, SignTool::facingForYaw(0.0f));
        check(su == SignTool::USE_PROMPT_NEW && got == std::vector<std::string>{sw}, "leg 6: a placed sign goes out as SIGNP",
              joined(got).c_str());
        hud->blocktype = savedType;
        hud->block_paintcolor = savedPaint;

        // 6f2: a server's signs land as sent: b = 0 (old sidecars carry it; the editor's range check refused
        // them, 2026-10-17), UTF-8 text as '?', text past SGN1's 95 cut, not dropped.
        {
            WorldTrailer* tr = World::getWorld->fm->trailer();
            const int d0 = MpWorld::droppedRecords();
            MpWorld::enqueueSign(bx, by, bz, 0, 0, 0, "Caf\xc3\xa9 \xe2\x86\x92 north");
            MpWorld::enqueueSign(bx, by, bz, 1, 2, 7, std::string(256, 'y').c_str());
            tick(1);
            const int i0 = tr->findSign(TrailerPos::fromEngine(bx, by, bz), 0);
            const int i1 = tr->findSign(TrailerPos::fromEngine(bx, by, bz), 1);
            const bool ok = i0 >= 0 && i1 >= 0 && !std::strcmp(tr->sign((size_t)i0).text, "Caf? ? north") &&
                            tr->sign((size_t)i0).b == 0 && tr->sign((size_t)i1).c == 7 &&
                            std::strlen(tr->sign((size_t)i1).text) == 95 && MpWorld::droppedRecords() == d0;
            check(ok, "leg 6: a server's b = 0 / UTF-8 / 256-byte signs land, cleaned, none dropped",
                  i0 >= 0 ? tr->sign((size_t)i0).text : "(no sign)");
            if (i1 >= 0) tr->removeSign((size_t)i1);
            const int j0 = tr->findSign(TrailerPos::fromEngine(bx, by, bz), 0);
            if (j0 >= 0) tr->removeSign((size_t)j0);
        }

        // 6g: a peer's edits in, through the engine's own primitives (doors in pairs, fire runs here),
        // and nothing goes back out (--mp-mutate=echo fails this).
        const int px = bx, pz = bz + 10;
        n0 = L->sent.size();
        const MpWorld::Stats s0 = MpWorld::stats();
        MpWorld::enqueueAction("peer", px, by - 1, pz, 0, TYPE_DOOR1);
        MpWorld::enqueueAction("peer", px, by, pz, 0, TYPE_DOOR_TOP);
        MpWorld::enqueueAction("peer", px, by - 1, pz, 3, 21);   // paintBlock paints the top half too
        tick(1);
        const int topPaint = t->getColor(px, pz, by);
        MpWorld::enqueueAction("peer", px, by, pz, 1, -1);        // destroyBlock takes both halves
        tick(1);
        const int doorAfter = t->getLand(px, pz, by - 1);
        MpWorld::enqueueAction("peer", px + 3, by, pz, 0, TYPE_WOOD);
        MpWorld::enqueueAction("peer", px + 3, by, pz, 2, -1);    // burnBlock(FALSE): the fire runs here
        tick(1);
        const bool peerFire = isOnFire(px + 3, pz, by);
        for (int i = 0; i < 600 && isOnFire(px + 3, pz, by); ++i) tick(1);
        tick(2);
        got = edits(n0);
        std::snprintf(d, sizeof d, "top paint %d, bottom after mine %d, peer fire %d, wood after %d, peer-applied %d; sent %s", topPaint,
                      doorAfter, (int)peerFire, t->getLand(px + 3, pz, by), MpWorld::stats().peerApplied - s0.peerApplied,
                      joined(got).c_str());
        check(topPaint == 21 && doorAfter == TYPE_NONE && peerFire && t->getLand(px + 3, pz, by) == TYPE_NONE,
              "leg 6: a peer's paint, mine and burn apply as the game's (door pairs, the fire runs locally)", d);
        check(got.empty(), "leg 6: applying a peer's edits sends nothing (no echo)", d);
        const MpWorld::Stats s6 = MpWorld::stats();
        std::printf("[eden-mp-replay] mp.edits actions=%d signs=%d peer_applied=%d\n", s6.actionsSent, s6.signsSent, s6.peerApplied);
        MpWorld::dropLink();
        delete L;
        MpWorld::clockMs = clock0;
    }

    // ---- leg 7 (MP.5): remote players: avatars, dead reckoning, POSVEL out, leaving ----------------
    {
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        ReplayLink* L = new ReplayLink;
        MpWorld::setLink(L);
        Player* pl = World::getWorld->player;
        const int sent0 = MpWorld::posVelSent();
        const double a[6] = {rep.spawn[0] + 10.0, rep.spawn[1] + 2.0, rep.spawn[2] + 10.0, 0, 0, 0};
        MpWorld::peerSeen("Zed", 3, 2, a);
        const double b[6] = {a[0] + 4.0, a[1], a[2], 4.0, 0, 0};   // 4 blocks east, walking east at 4/s
        MpWorld::peerSeen("Zed", 3, 2, b);
        MpWorld::peerSeen("Amy", 9, 0, a);
        tick(1);
        MpWorld::PeerView v;
        bool have = MpWorld::peerCount() == 2 && MpWorld::peerView(0, &v);
        std::snprintf(d, sizeof d, "peers %d, bodies shown %d, available %d", MpWorld::peerCount(), RemoteBodiesShown(),
                      (int)PlayerBodyAvailable());
        check(have && RemoteBodiesShown() == 2 && PlayerBodyAvailable(), "leg 7: two peers are drawn as bodies in a session", d);
        // Glide: monotone toward (and never past) the extrapolated goal, within the cap.
        float prev = -1e9f;
        bool mono = true, within = true;
        for (int i = 0; i < 40; ++i) {
            tick(1);
            for (int k = 0; k < MpWorld::peerCount(); ++k) {
                if (!MpWorld::peerView(k, &v) || std::strcmp(v.name, "Zed") != 0) continue;
                mono = mono && v.x >= prev - 1e-3f;
                prev = v.x;
                within = within && v.x <= v.tx + 0.6f * 4.0f + 1e-2f;
            }
        }
        MpWorld::peerView(0, &v);
        std::snprintf(d, sizeof d, "Zed x %.2f, last report %.2f, yaw %.1f, motion %d, avatar model %d", v.x, v.tx, v.yaw, v.motion,
                      MpWorld::avatarModel(v.type));
        check(mono && within && v.x > a[0] + 3.0f && v.motion == PB_WALK && std::fabs(v.yaw) < 5.0f,
              "leg 7: a walking peer glides to its report and faces its heading", d);
        check(MpWorld::avatarModel(0) == M_GREEN && MpWorld::avatarModel(10) >= 0 && MpWorld::avatarModel(99) == MpWorld::avatarModel(10) &&
                  MpWorld::avatarModel(-1) == M_GREEN,
              "leg 7: characterType 0..10 maps to a shipped model, out-of-range clamps", "");
        // A far jump snaps.
        const double far_[6] = {a[0] + 200.0, a[1], a[2], 0, 0, 0};
        MpWorld::peerSeen("Amy", 9, 0, far_);
        tick(1);
        for (int k = 0; k < MpWorld::peerCount(); ++k)
            if (MpWorld::peerView(k, &v) && !std::strcmp(v.name, "Amy")) break;
        check(std::fabs(v.x - far_[0]) < 0.5f, "leg 7: a peer reported far away snaps instead of gliding", "");
        // POSVEL out: ~4 Hz while the player moves, then at most 1 Hz still.
        const int64_t t0 = mp_fake_clock();
        const int n0 = MpWorld::posVelSent();
        size_t l0 = L->sent.size();
        for (int i = 0; i < 120; ++i) {   // ~2 s of fake time
            pl->pos.x += 0.2f;
            pl->vel.x = 6.0f;
            tick(1);
        }
        const int moving = MpWorld::posVelSent() - n0;
        const int64_t dt = mp_fake_clock() - t0;
        pl->vel.x = 0;
        const int n1 = MpWorld::posVelSent();
        for (int i = 0; i < 120; ++i) tick(1);
        const int idle = MpWorld::posVelSent() - n1;
        int posvels = 0;
        bool fmt = true;
        for (size_t i = l0; i < L->sent.size(); ++i)
            if (L->sent[i].rfind("POSVEL:", 0) == 0) {
                ++posvels;
                int c = 0;
                for (char ch : L->sent[i]) c += ch == ':';
                fmt = fmt && c == 6;
            }
        std::snprintf(d, sizeof d, "moving: %d POSVEL in %lld ms, idle: %d in the next 120 frames; lines %d, 7-field %d", moving,
                      (long long)dt, idle, posvels, (int)fmt);
        check(moving >= 1 && moving <= (int)(dt / 250) + 1 && idle >= 1 && idle <= (int)(dt / 1000) + 2 && fmt && posvels == moving + idle,
              "leg 7: POSVEL leaves at <= 4 Hz moving and <= 1 Hz still, in the wire format", d);
        (void)sent0;
        // ---- leg 8 (MP.7): chat in, chat out, the rate guard, /login, the panel, the HUD feed ---------
        {
            ChatFeed::reset();
            ChatFeed::clockMs = mp_chat_clock;
            ChatFeed::addWire(false, "Alex", "anyone near spawn?");
            ChatFeed::addWire(true, "", "TNT is disabled on this server.");
            const std::vector<Console::Line>& cl = ChatFeed::lines();
            check(cl.size() == 2 && cl[0].text == "Alex: anyone near spawn?" && cl[0].kind == ChatFeed::K_CHAT &&
                      cl[1].text == "[Server] TNT is disabled on this server." && cl[1].kind == ChatFeed::K_SERVER,
                  "leg 8: relayed chat and [Server] lines land in the scrollback, styled by kind", "");
            const size_t s0 = L->sent.size();
            check(ChatFeed::send("hello") == ChatFeed::SENT && L->sent.size() == s0 + 1 && L->sent.back() == "MSG:hello" &&
                      cl.back().text == "You: hello" && cl.back().kind == ChatFeed::K_SELF,
                  "leg 8: a line goes out as MSG:text with a local echo", L->sent.back().c_str());
            check(ChatFeed::send("a:b:c") == ChatFeed::SENT && L->sent.back() == "MSG:a:b:c", "leg 8: a ':' in the text is safe", L->sent.back().c_str());
            int ok = 2;
            while (ChatFeed::send("burst") == ChatFeed::SENT && ok < 20) ++ok;
            std::snprintf(d, sizeof d, "%d lines passed the 0.5/s burst-5 guard", ok);
            check(ok == 5, "leg 8: the client guard stops the sixth line in a burst (burst 5)", d);
            const size_t sBefore = L->sent.size();
            check(ChatFeed::send("held") == ChatFeed::TOO_FAST && L->sent.size() == sBefore, "leg 8: a refused line is not sent", "");
            check(ChatFeed::send("/msg Alex psst") == ChatFeed::SENT && L->sent.back() == "MSG:/msg Alex psst",
                  "leg 8: a / command is exempt from the guard and goes out verbatim", L->sent.back().c_str());
            g_mpFakeMs += 2500;   // 1.25 tokens
            check(ChatFeed::send("later") == ChatFeed::SENT, "leg 8: the guard refills over time", "");
            const size_t hist0 = ChatFeed::historyCount();
            ChatFeed::send("/login 1234");
            check(L->sent.back() == "MSG:/login 1234" && cl.back().text == "> /login ****" && ChatFeed::historyCount() == hist0,
                  "leg 8: /login is masked in the scrollback and never kept in history", cl.back().text.c_str());
            check(ChatFeed::send("   ") == ChatFeed::EMPTY, "leg 8: an empty line is not sent", "");
            // The panel: the Console screen on its Chat tile, with the session tiles in front.
            ConsoleMenu* con = World::getWorld->menu->console;
            check(eden_chat_open(0) == 1 && con->isOpen() && con->chatTab() && con->railTabs() == 5,
                  "leg 8: T opens the Console screen on Chat with five tiles in a session", "");
            const double pa[6] = {rep.spawn[0] + 6.0, rep.spawn[1] + 2.0, rep.spawn[2], 0, 0, 0};
            MpWorld::peerSeen("Kim", 2, 0, pa);
            con->showTab(ConsoleMenu::TAB_PLAYERS);
            tick(1);
            std::snprintf(d, sizeof d, "rows %d peers %d", con->playerRows(), MpWorld::peerCount());
            check(con->playerRows() == MpWorld::peerCount() + 1, "leg 8: Players lists you and every player seen", d);
            con->showTab(ConsoleMenu::TAB_CHAT);
            con->setInput("sent from the panel");
            const size_t s1 = L->sent.size();
            g_mpFakeMs += 3000;
            con->submit();
            check(L->sent.size() == s1 + 1 && L->sent.back() == "MSG:sent from the panel" && con->input().empty(),
                  "leg 8: Send from the panel sends and clears the field", L->sent.back().c_str());
            con->close();
            check(eden_chat_open(1) == 1 && con->input() == "/", "leg 8: / opens Chat with the slash typed", con->input().c_str());
            con->close();
            // The HUD feed: the newest lines (cap 5), gone after 10 s.
            ChatFeed::update(0.0f);
            ChatFeed::renderHud(false);
            const int shown = ChatFeed::feedLines();
            ChatFeed::update(11.0f);
            ChatFeed::renderHud(false);
            std::snprintf(d, sizeof d, "feed %d lines, then %d after 11 s", shown, ChatFeed::feedLines());
            check(shown >= 1 && shown <= 5 && ChatFeed::feedLines() == 0, "leg 8: the HUD feed shows at most 5 lines and they fade out after 10 s", d);
            ChatFeed::clockMs = nullptr;
            ChatFeed::reset();
            MpWorld::peerLeft("Kim");
        }
        // Leaving.
        MpWorld::peerLeft("Zed");
        tick(1);
        std::snprintf(d, sizeof d, "peers %d, bodies shown %d", MpWorld::peerCount(), RemoteBodiesShown());
        check(MpWorld::peerCount() == 1 && RemoteBodiesShown() == 1, "leg 7: a peer that left is gone with its body", d);
        MpWorld::dropLink();
        delete L;
        MpWorld::clockMs = clock0;
    }

    // ---- leg 5: a teleport's bulk reload equals a fresh join there ------------------------------
    const float dest[3] = {(float)std::atof(want["dest.x"].c_str()), (float)std::atof(want["dest.y"].c_str()),
                           (float)std::atof(want["dest.z"].c_str())};
    {
        const int bulk0 = g_bulk_reloads;
        eden_console_teleport(dest[0], dest[1], dest[2]);
        tick(2);
        settle();
        const MpWorld::WindowState w5 = MpWorld::windowState();
        report_ws("5-teleport", w5);
        std::snprintf(d, sizeof d, "bulk reloads %d->%d; hash %s cells %d window (%d,%d); expect %s cells %s window (%s,%s)",
                      bulk0, g_bulk_reloads, hex(w5.hash).c_str(), w5.cells, w5.ox, w5.oz, want["dest.hash"].c_str(),
                      want["dest.cells"].c_str(), want["dest.ox"].c_str(), want["dest.oz"].c_str());
        check(g_bulk_reloads > bulk0 && w5.missing == 0 && w5.gblockMismatch == 0, "leg 5: the teleport bulk-reloads the window", d);
        check(hex(w5.hash) == want["dest.hash"] && std::to_string(w5.cells) == want["dest.cells"] && w5.cells > 0,
              "leg 5: the reloaded window equals mp-replay.py's at the destination", d);
        if (!quit_to_menu()) return 1;
        std::snprintf(d, sizeof d, "active %d, overlay %zu cells, creatures %d", (int)MpWorld::active(), MpWorld::overlayCells(),
                      (int)CREATURES_ON);
        check(!MpWorld::active() && MpWorld::overlayCells() == 0, "exit: leaving the session drops the overlay", d);
        if (!mp_replay_open(rep, dest)) return 1;
        settle();
        const MpWorld::WindowState wf = MpWorld::windowState();
        report_ws("5-fresh", wf);
        std::snprintf(d, sizeof d, "teleport %s/%d, fresh join %s/%d", hex(w5.hash).c_str(), w5.cells, hex(wf.hash).c_str(), wf.cells);
        check(wf.hash == w5.hash && wf.cells == w5.cells && wf.missing == 0, "leg 5: a fresh join at the destination matches", d);
        if (!quit_to_menu()) return 1;
    }

    // ---- leg 4 (MP.3b): a 600k-record join through the whole flow, then a relay storm ----------
    {
        const int P = 65536, H = 80;   // a 160x160 area, y 33-44, every cell a painted block: 614,400 records
        std::vector<eden_net::SnapzRec> all;
        for (int cx = (P - H) >> 4; cx < (P + H) >> 4; ++cx)          // the server's order: chunk by chunk,
            for (int cz = (P - H) >> 4; cz < (P + H) >> 4; ++cz)      // then x, z, y, flag inside one
                for (int cy = 2; cy <= 2; ++cy)
                    for (int x = cx * 16; x < cx * 16 + 16; ++x)
                        for (int z = cz * 16; z < cz * 16 + 16; ++z)
                            for (int y = 33; y < 45; ++y) {
                                const int ty = (x + z + y) % 3 == 0 ? TYPE_BRICK : (x + z + y) % 3 == 1 ? TYPE_COBBLESTONE : TYPE_STONE;
                                all.push_back({x, y, z, 0, ty});
                                all.push_back({x, y, z, 3, (x * 7 + z * 3 + y) % 54 + 1});
                            }
        std::vector<std::string> lines;
        for (size_t i = 0; i < all.size(); i += 3000)
            lines.push_back(eden_net::encodeSnapzLine(all.data() + i, std::min<size_t>(3000, all.size() - i), 1));
        if (all.size() % 3000 == 0) lines.push_back(eden_net::encodeSnapzLine(nullptr, 0, 1));
        size_t wire = 0;
        for (const auto& l : lines) wire += l.size() + 1;
        // The net thread's work (base64 + inflate + parse), timed here on the main thread.
        const Uint64 td0 = SDL_GetPerformanceCounter();
        eden_net::MpSession dec;
        for (const auto& l : lines) dec.feedLine(l);
        std::vector<eden_net::MpWorldEvent> frames;
        dec.drainWorld(frames);
        const double decodeMs = (double)(SDL_GetPerformanceCounter() - td0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        int64_t (*clock0)() = MpWorld::clockMs;
        MpWorld::clockMs = mp_fake_clock;
        g_mpFakeMs = 2000000;
        ReplayLink* L = new ReplayLink;
        for (const auto& f : frames) L->answers[std::make_pair(P, P)].push_back(f.recs);
        L->joinFlow = true;
        MpWorld::resetStats();
        MpWorld::beginSession("replay", 0, "Leg 4");
        MpWorld::setSpawn((float)P + 0.5f, 60.0f, (float)P + 0.5f);
        MpWorld::setLink(L);
        auto frameMs = [] {
            const Uint64 a = SDL_GetPerformanceCounter();
            tick(1);
            return (double)(SDL_GetPerformanceCounter() - a) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        };
        const Uint64 tj0 = SDL_GetPerformanceCounter();
        auto since = [tj0] { return (double)(SDL_GetPerformanceCounter() - tj0) * 1000.0 / (double)SDL_GetPerformanceFrequency(); };
        MpWorld::startRegions(true);
        double syncWorst = 0, loadWorst = 0, syncMs = 0, loadMs = 0;
        int syncFrames = 0, loadFrames = 0;
        for (int i = 0; i < 20000 && L->joinFlow; ++i) { syncWorst = std::max(syncWorst, frameMs()); syncFrames++; }
        syncMs = since();
        for (int i = 0; i < 20000 && game_mode() != 1; ++i) {   // the load lands the whole window before PLAY
            loadWorst = std::max(loadWorst, frameMs());
            loadFrames++;
        }
        loadMs = since() - syncMs;
        const MpWorld::WindowState w4 = MpWorld::windowState();
        report_ws("4-join", w4);
        std::printf("[eden-mp-replay] mp.join records=%zu frames=%zu wire=%zu B decode=%.1f ms sync=%.1f ms/%d frames worst=%.2f ms "
                    "load=%.1f ms/%d frames worst=%.2f ms playable=%.1f ms overlay=%zu B\n", all.size(), frames.size(), wire,
                    decodeMs, syncMs, syncFrames, syncWorst, loadMs, loadFrames, loadWorst, since(), MpWorld::overlayBytes());
        std::snprintf(d, sizeof d, "game_mode %d, window cells %d (want %d), missing %d, gblock %d, sent %zu", game_mode(), w4.cells,
                      2 * H * 2 * H * 12, w4.missing, w4.gblockMismatch, L->sent.size());
        check(game_mode() == 1 && w4.cells == 2 * H * 2 * H * 12 && w4.missing == 0 && w4.gblockMismatch == 0,
              "leg 4: a 600k-record join lands whole through Syncing -> Loading -> Play", d);
        std::snprintf(d, sizeof d, "%d sync frames for %zu SNAPZ frames, worst %.2f ms", syncFrames, frames.size(), syncWorst);
        check(syncFrames >= 2, "leg 4: the join drain is spread over frames (8 ms budget)", d);

        // A 2,000-cell relay storm in play: peers building into air, all over the window.
        Terrain* t = World::getWorld->terrain;
        tick(30);
        MpWorld::resetStats();
        for (int i = 0; i < 2000; ++i)
            MpWorld::enqueueAction("peer", P - 100 + (i % 50) * 4, 50 + (i / 500) * 20, P - 100 + ((i / 50) % 10) * 20, 0, TYPE_BRICK);
        double stormWorst = 0;
        int stormFrames = 0;
        for (int i = 0; i < 2000 && MpWorld::queuedEvents(); ++i) { stormWorst = std::max(stormWorst, frameMs()); stormFrames++; }
        int landed = 0;
        for (int i = 0; i < 2000; ++i)
            landed += t->getLand(P - 100 + (i % 50) * 4, P - 100 + ((i / 50) % 10) * 20, 50 + (i / 500) * 20) == TYPE_BRICK;
        const MpWorld::Stats s4 = MpWorld::stats();
        std::printf("[eden-mp-replay] mp.storm cells=2000 landed=%d frames=%d worst_frame=%.2f ms worst_apply=%.2f ms worst_chunks=%d\n",
                    landed, stormFrames, stormWorst, s4.worstFrameMs, s4.worstFrameChunks);
        std::snprintf(d, sizeof d, "landed %d/2000 over %d frames, worst %d chunks", landed, stormFrames, s4.worstFrameChunks);
        check(landed == 2000 && stormFrames >= 2 && s4.worstFrameChunks <= 24 + 6,
              "leg 4: a 2,000-cell relay storm lands whole, at most 24 chunks (+1 cell) a frame", d);
        if (!quit_to_menu()) return 1;
        check(L->released && !MpWorld::active(), "leg 4: leaving the session releases the link", "");
        delete L;
        MpWorld::clockMs = clock0;
    }

    // Nothing a session did reached the save directory (no world file, no thumbnail, no backup slot).
    {
        const std::set<std::string> docs1 = mp_docs_listing();
        std::string extra;
        for (const auto& n : docs1) if (!docs0.count(n)) extra += " " + n;
        check(extra.empty(), "a session writes nothing to the save directory", extra.empty() ? "none" : extra.c_str());
    }
    g_app->viewController.setFixedEtime(0.0f);
    std::printf("[eden-mp-replay] %s (%d failure(s))%s\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures,
                g_opt.mpMutate.empty() ? "" : (" [mutation " + g_opt.mpMutate + "]").c_str());
    return g_selftestFailures ? 1 : 0;
}

// --mp-join=HOST[:PORT]: the MP.3b join flow (native/src/seam/MpLink_native.mm). Interactive: straight from
// the main menu. Headless (run_mp_join): a live gate -- join, wait for play, print the window state, run
// --frames, leave; exit 0 only if the session became playable and left cleanly.
extern "C" int eden_mp_join(const char* host, int port, const char* name, int characterType, const char* password);
int mp_join_from_args() {
    std::string host = g_opt.mpJoin;
    int port = 27015;
    const size_t c = host.rfind(':');
    if (c != std::string::npos) { port = std::atoi(host.c_str() + c + 1); host.resize(c); }
    return eden_mp_join(host.c_str(), port, g_opt.mpName.c_str(), g_opt.mpType, g_opt.mpPassword.c_str());
}

// MP.4's two-client leg (port/tools/mp-replay.py edits). --mp-edits (the builder): edits near the spawn
// through the intent sites -- a painted build and repaint, a door, a burnt wood block, a TNT blast, a
// mined grass cell, a sign -- then waits for its fires and sends. --mp-hold=FILE (the watcher): plays
// until FILE exists. Both then re-ask their REGION, wait for its reconcile, and print the window hash,
// the session's signs and what they sent; the script compares them with each other and the server.
void mp_wait_ms(Uint32 ms) {
    const Uint64 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < ms) tick(1);
}
void mp_live_edits(const MpWorld::WindowState& w0) {
    Terrain* t = World::getWorld->terrain;
    Hud* hud = World::getWorld->hud;
    if (g_opt.mpEdits) {
        const Vector p = World::getWorld->player->pos;
        const int bx = (int)p.x + 20, bz = (int)p.z - 30, by = 50, tx = bx + 30, wx = bx + 12;
        hud->blocktype = TYPE_BRICK;
        hud->block_paintcolor = 12;
        MpWorld::intentBuild(bx, bz, by);
        MpWorld::intentPaint(bx, bz, by, 30);
        hud->block_paintcolor = 0;
        hud->blocktype = TYPE_DOOR_TOP;
        MpWorld::intentBuild(bx + 4, bz, by);
        hud->blocktype = TYPE_WOOD;
        MpWorld::intentBuild(wx, bz, by);
        MpWorld::intentBurn(wx, bz, by);
        hud->blocktype = TYPE_BRICK;
        for (int k = -2; k <= 2; k += 4) MpWorld::intentBuild(tx + k, bz, by);   // in the blast
        hud->blocktype = TYPE_TNT;
        MpWorld::intentBuild(tx, bz, by);
        MpWorld::intentBurn(tx, bz, by);
        MpWorld::intentMine(bx, bz + 5, 32);   // natural grass
        SignTool::use(bx, bz, by, 0, 1, 0, 0.0f);
        SignTool::answer(0, "Hello: MP.4");
        if (GLDialog::active()) GLDialog::dismiss();
        tick_until([&] { return !isOnFire(tx, bz, by) && !isOnFire(wx, bz, by); }, 3000, "the builder's fires to burn out");
        check(t->getLand(tx, bz, by) == TYPE_NONE && t->getLand(wx, bz, by) == TYPE_NONE && t->getLand(bx, bz, by) == TYPE_BRICK,
              "edits: the builder's edits landed locally", "");
        mp_wait_ms(1500);   // the net thread writes what is queued
    } else {
        const Uint64 t0 = SDL_GetTicks();
        while (SDL_GetTicks() - t0 < 180000) {
            if (FILE* f = std::fopen(g_opt.mpHold.c_str(), "rb")) { std::fclose(f); break; }
            tick(1);
        }
        mp_wait_ms(1000);
    }
    const MpWorld::WindowState wp = MpWorld::windowState();   // what the relays alone made
    const int r0 = MpWorld::stats().reconciles;
    MpWorld::reaskRegion();
    const bool rec = tick_until([r0] { return MpWorld::stats().reconciles > r0 && MpWorld::queuedEvents() == 0; }, 3000,
                                "the closing reconcile");
    check(rec, "edits: the closing REGION is answered and reconciled", "");
    const MpWorld::WindowState w = MpWorld::windowState();
    const MpWorld::Stats s = MpWorld::stats();
    std::printf("[eden-mp-join] edits role=%s final hash=%016llx cells=%d pre=%016llx/%d (joined %016llx/%d) sent actions=%d "
                "signs=%d peer_applied=%d\n", g_opt.mpEdits ? "builder" : "watcher", (unsigned long long)w.hash, w.cells,
                (unsigned long long)wp.hash, wp.cells, (unsigned long long)w0.hash, w0.cells, s.actionsSent, s.signsSent, s.peerApplied);
    WorldTrailer* tr = World::getWorld->fm->trailer();
    for (size_t i = 0; i < tr->signCount(); ++i) {
        const TrailerSign& sg = tr->sign(i);
        char text[96];
        std::memcpy(text, sg.text, 95);
        text[95] = 0;
        std::printf("[eden-mp-join] sign %d:%d:%d:%d:%d:%d:%s\n", sg.x, sg.z, sg.y, sg.a, sg.b, sg.c, text);   // wire order
    }
    std::fflush(stdout);
}

int run_mp_join() {
    g_selftestTag = "eden-mp-join";
    g_autosave_seconds = 0;
    MpWorld::testEcho = g_opt.mpMutate == "echo";   // MP.4: mp-replay.py edits --mutate echo must fail
    tick(10);
    check(mp_join_from_args() == 1, "the join starts", g_opt.mpJoin.c_str());
    const bool ok = tick_until([] { return game_mode() == 1 && MpWorld::active(); }, 3000,
                               "the session world to be playable");
    check(ok, "the session becomes playable", g_opt.mpJoin.c_str());
    if (ok) {
        const MpWorld::WindowState w = MpWorld::windowState();
        const MpWorld::Stats s = MpWorld::stats();
        std::printf("[eden-mp-join] window hash=%016llx cells=%d ox=%d oz=%d overlay=%zu cells regions=%d replies=%d reconciles=%d\n",
                    (unsigned long long)w.hash, w.cells, w.ox, w.oz, MpWorld::overlayCells(), s.regionsSent, s.repliesComplete,
                    s.reconciles);
        if (g_opt.mpEdits || !g_opt.mpHold.empty()) mp_live_edits(w);
        tick(g_opt.frames);
        check(quit_to_menu() && !MpWorld::active() && !MpWorld::link(), "leaving the session closes it", "");
    }
    std::printf("[eden-mp-join] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_touch_selftest() {
    g_selftestTag = "eden-touch";
    g_tickInput = true;

    // A touch test runs in the TOUCH profile, on every platform. Without this the desktop legs
    // test the touch translator against a layout that has no on-screen joystick at all
    // (`use_joystick` is a profile field — Settings_web.mm's eden_apply_input_profile), so the
    // stick checks below would be asserting against chrome that is not drawn. iOS already sets
    // this in main(); saying it again here is what makes the four platforms run the same test.
    eden_set_detected_touch(1);
    eden_test_force_touch_input();   // X.19: a stored Input mode of Keyboard+Mouse would beat the line above

    const char* name = g_opt.world.empty() ? "touch-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(120);

    // Ask the engine where its menu button is. `rmenu` is [x, y, w, h] in point space.
    float rx = 0, ry = 0, rw = 0, rh = 0;
    {
        const char* st = eden_debug_display_state();
        const char* p = st ? std::strstr(st, "\"rmenu\":[") : nullptr;
        if (p) std::sscanf(p + 9, "%f,%f,%f,%f", &rx, &ry, &rw, &rh);
    }
    char detail[220];
    std::snprintf(detail, sizeof(detail), "rmenu [%.1f,%.1f,%.1f,%.1f] in a %.0fx%.0f point space",
                  rx, ry, rw, rh, SCREEN_WIDTH, SCREEN_HEIGHT);
    check(rw > 0.0f && rh > 0.0f, "the engine reports a menu-button rect", detail);
    if (rw <= 0.0f || rh <= 0.0f) {
        std::printf("[eden-touch] FAILURES (%d failure(s))\n", ++g_selftestFailures);
        return 1;
    }

    // Centre of the rect, flipped into the top-left space the pointer API uses...
    const float cx = rx + rw * 0.5f;
    const float cy = SCREEN_HEIGHT - (ry + rh * 0.5f);

    // ...and then normalised the way SDL reports a finger — THROUGH THE LETTERBOX, which is the
    // half of this test that matters. A finger is a fraction of the WHOLE WINDOW, and the HUD is
    // drawn inside a box that may be smaller than the window (bars, when the layout's aspect and
    // the screen's differ — the touch profile pinned 16:9 until 2026-09-07, and the clamps in
    // DisplayProfile_web.mm can still make the two differ on an extreme aspect). Assuming the
    // fraction
    // maps straight onto the point space is exactly the assumption the translator used to make,
    // and it is what an iPad Air 2 reported as "switches don't respond to touch at all". So this
    // computes the inverse of what the translator does; if the two ever disagree, the tap misses
    // and this test says so.
    // Point space (top-left origin) -> the 0..1 window fraction SDL reports a finger as. Now a
    // lambda because the joystick checks at the bottom need it too: any test that pushes a finger
    // has to invert exactly what the translator does, and having two copies of that inverse is how
    // the two silently stop agreeing.
    auto to_norm = [&](float px, float pyTopLeft, float* outNx, float* outNy) {
        float nx = px / SCREEN_WIDTH;
        float ny = pyTopLeft / SCREEN_HEIGHT;
        // Read the box LIVE rather than reusing the capture above: opening a world re-derives the
        // display metrics (the profile seeder runs on the first settings pump), so a value read
        // before that is a value from a different layout.
        int lx = 0, ly = 0, lw = 0, lh = 0;
        eden_native_gl_get_letterbox(&lx, &ly, &lw, &lh);
        const float bx = (float)lx, by = (float)ly, bw = (float)lw, bh = (float)lh;
        if (bw > 0.0f && bh > 0.0f && eden_native_gl_window()) {
            // Pixels -> window points, the same conversion window_to_point() makes.
            int ww = 0, wh = 0, pw = 0, ph = 0;
            SDL_GetWindowSize(eden_native_gl_window(), &ww, &wh);
            SDL_GetWindowSizeInPixels(eden_native_gl_window(), &pw, &ph);
            const float sx = (pw > 0 && ww > 0) ? (float)pw / (float)ww : 1.0f;
            const float sy = (ph > 0 && wh > 0) ? (float)ph / (float)wh : 1.0f;
            const float boxXpt = bx / sx, boxYpt = by / sy;
            const float boxWpt = bw / sx, boxHpt = bh / sy;
            if (ww > 0 && wh > 0 && boxWpt > 0.0f && boxHpt > 0.0f) {
                nx = (boxXpt + px        / SCREEN_WIDTH  * boxWpt) / (float)ww;
                ny = (boxYpt + pyTopLeft / SCREEN_HEIGHT * boxHpt) / (float)wh;
            }
        }
        *outNx = nx;
        *outNy = ny;
    };
    float nx = 0.0f, ny = 0.0f;
    to_norm(cx, cy, &nx, &ny);
    if (eden_native_gl_window()) {
        int lx = 0, ly = 0, lw = 0, lh = 0, pw = 0, ph = 0;
        eden_native_gl_get_letterbox(&lx, &ly, &lw, &lh);
        SDL_GetWindowSizeInPixels(eden_native_gl_window(), &pw, &ph);
        if (lw > 0 && lh > 0) {
            std::snprintf(detail, sizeof(detail), "box %dx%d px at (%d,%d) in a %dx%d px surface",
                          lw, lh, lx, ly, pw, ph);
            check(lw <= pw && lh <= ph, "the letterbox fits inside the surface", detail);
        }
    }

    auto push_finger_id = [&](SDL_FingerID id, Uint32 type, float x, float y) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = type;
        ev.tfinger.timestamp = SDL_GetTicksNS();
        ev.tfinger.touchID = 1;      // any non-zero device; the translator keys on fingerID
        ev.tfinger.fingerID = id;    // the slot table is what maps it onto a pointer identity
        ev.tfinger.x = x;
        ev.tfinger.y = y;
        ev.tfinger.dx = 0.0f;
        ev.tfinger.dy = 0.0f;
        ev.tfinger.pressure = 1.0f;
        SDL_PushEvent(&ev);
    };
    auto push_finger = [&](Uint32 type, float x, float y) { push_finger_id(7, type, x, y); };

    // --- a tap on the menu icon opens the in-game menu ---
    {
        const int before = eden_hud_in_menu();
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "tap at (%.3f,%.3f) normalised: in_menu %d -> %d",
                      nx, ny, before, after);
        check(after != before, "a finger tap reaches the HUD", detail);
    }

    // --- and a second tap closes it, which is the part a stuck touch would break ---
    {
        const int before = eden_hud_in_menu();
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "in_menu %d -> %d", before, after);
        check(after != before, "a second tap closes it (no stuck touch)", detail);
    }

    // --- a CANCELLED finger must not leave its slot occupied ---
    // Nine fingers is a dropped ninth, by design; a leaked slot turns into one after eight taps.
    // This is the cheap proof that the release path runs for cancel as well as for up.
    {
        for (int i = 0; i < 10; ++i) {
            push_finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.5f);
            tick(2);
            push_finger(SDL_EVENT_FINGER_CANCELED, 0.5f, 0.5f);
            tick(2);
        }
        const int before = eden_hud_in_menu();
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "after 10 cancelled fingers: in_menu %d -> %d",
                      before, after);
        check(after != before, "cancelled fingers free their slots", detail);
    }

    // Back to PLAYING before anything below runs. The three checks above leave the in-game menu
    // open, and `g_relativeMouse` — the flag that decides whether a mouse event looks/mines at all
    // — is derived from `eden_hud_in_menu()`. The first version of the synthetic-mouse checks
    // below sat here with the menu open, so they passed with the fix REMOVED: they were asserting
    // against a mouse path that was switched off, not against the guard. A gate has to be run once
    // with the bug put back.
    if (eden_hud_in_menu()) {
        push_finger(SDL_EVENT_FINGER_DOWN, nx, ny);
        tick(8);
        push_finger(SDL_EVENT_FINGER_UP, nx, ny);
        tick(24);
    }
    std::snprintf(detail, sizeof(detail), "in_menu %d", eden_hud_in_menu());
    check(eden_hud_in_menu() == 0, "the checks below run while PLAYING, not in the menu", detail);

    // ---------------------------------------------------------------------------------------
    // WHAT THIS GATE DID NOT ASSERT UNTIL NOW, and what the second device pass cost because of it
    // (STATUS §2.0.12). Everything above taps: down, a few ticks, up. That proves a finger reaches
    // the HUD and that a slot comes back. It says nothing about (a) a finger that is HELD — which
    // is the entire on-screen joystick — and nothing about (b) the events SDL generates BESIDES
    // the finger, which on a real touchscreen is a whole second mouse.
    // ---------------------------------------------------------------------------------------------

    // --- SDL's touch->mouse emulation must not reach the game twice ---
    // SDL_HINT_TOUCH_MOUSE_EVENTS defaults to "1", so on a device every finger also arrives as a
    // mouse event with `which == SDL_TOUCH_MOUSEID`. While playing, `g_relativeMouse` is true, so
    // that second copy went to mouse-look and to the hold-to-mine slot: one tap edited a block
    // where the finger landed AND at the crosshair ("breaking a block breaks it in TWO PLACES AT
    // ONCE"), and dragging the stick swung the camera. SDL_PushEvent does not synthesise these, so
    // this pushes them by hand — which is the only way a desk can see an iOS-only default.
    {
        auto push_mouse = [&](Uint32 type, SDL_MouseID which, float xrel, float yrel) {
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                ev.motion.timestamp = SDL_GetTicksNS();
                ev.motion.which = which;
                ev.motion.x = SCREEN_WIDTH * 0.5f;
                ev.motion.y = SCREEN_HEIGHT * 0.5f;
                ev.motion.xrel = xrel;
                ev.motion.yrel = yrel;
            } else {
                ev.button.timestamp = SDL_GetTicksNS();
                ev.button.which = which;
                ev.button.button = SDL_BUTTON_LEFT;
                ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                ev.button.clicks = 1;
                ev.button.x = SCREEN_WIDTH * 0.5f;
                ev.button.y = SCREEN_HEIGHT * 0.5f;
            }
            SDL_PushEvent(&ev);
        };

        const PlayerState before = player_state();
        push_mouse(SDL_EVENT_MOUSE_MOTION, SDL_TOUCH_MOUSEID, 200.0f, 120.0f);
        tick(4);
        const PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "yaw %.2f -> %.2f, pitch %.2f -> %.2f",
                      before.yaw, after.yaw, before.pitch, after.pitch);
        check(after.yaw == before.yaw && after.pitch == before.pitch,
              "a touch-synthesised mouse motion does not look", detail);

        push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_TOUCH_MOUSEID, 0.0f, 0.0f);
        tick(4);
        const int claimed = eden_native_input_debug_hold_active();
        push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_TOUCH_MOUSEID, 0.0f, 0.0f);
        tick(4);
        std::snprintf(detail, sizeof(detail), "hold-to-act slot claimed: %d (expected 0)", claimed);
        check(claimed == 0, "a touch-synthesised mouse button does not mine", detail);
    }

    // --- a HELD finger on the joystick walks, and KEEPS walking ---
    // The stick is the one control that is a sustained state rather than an event, and this gate
    // had never held anything. "Pushing the joystick forward nudges the player a tiny bit at a
    // time" is precisely what a held finger looks like when only its first frame counts, so the
    // assertion is not just "did the player move" — it is "did the player move as far in the
    // SECOND half of the hold as in the first", with no further SDL events in between.
    {
        float jx = 0, jy = 0, jw = 0, jh = 0;
        {
            const char* st = eden_debug_display_state();
            const char* p2 = st ? std::strstr(st, "\"rjoystick\":[") : nullptr;
            if (p2) std::sscanf(p2 + 13, "%f,%f,%f,%f", &jx, &jy, &jw, &jh);
        }
        std::snprintf(detail, sizeof(detail), "rjoystick [%.1f,%.1f,%.1f,%.1f], use_joystick %s",
                      jx, jy, jw, jh, std::strstr(eden_debug_display_state(), "\"use_joystick\":1")
                                          ? "on" : "OFF");
        check(jw > 0.0f && jh > 0.0f, "the engine reports an on-screen joystick pad", detail);

        if (jw > 0.0f && jh > 0.0f) {
            // Fixed 1/60 s etime for EVERYTHING from here to the walk checks (X.11). The coast
            // check below flaked on loaded machines (Linux CI, 4/4 locally under load) because it
            // ran on wall-clock etime: Player::update integrates pos += vel*etime AND substeps
            // (re-applying its per-call damping) when a frame's displacement exceeds 0.1, so one
            // 20-tick window that caught a slow frame covered more ground than the first. The
            // touch joystick does not go through Movement_web.mm's wall-clock EMA on native (only
            // the keyboard path does), so engine etime is the only clock this block reads.
            g_app->viewController.setFixedEtime(1.0f / 60.0f);
            teleport_for_movement();   // the map CENTRE — see its own comment for why --at's is not
            tick(120);
            rise_until_clear(1800);    // and clear of terrain, so "did not move" means the input
            check_clear_of_map_border("the player is clear of the map border");

            // Top of the pad, horizontally centred: full forward tilt. Point space is bottom-left
            // here (the rects are), and the pointer API is top-left — the same flip the rmenu tap
            // above makes, and the reason it is written out rather than folded into to_norm().
            const float px = jx + jw * 0.5f;
            const float py = SCREEN_HEIGHT - (jy + jh - 4.0f);
            float snx = 0.0f, sny = 0.0f;
            to_norm(px, py, &snx, &sny);

            const PlayerState p0 = player_state();
            push_finger_id(11, SDL_EVENT_FINGER_DOWN, snx, sny);
            tick(150);
            const PlayerState p1 = player_state();
            tick(150);                          // held, with NO further SDL events at all
            const PlayerState p2 = player_state();
            push_finger_id(11, SDL_EVENT_FINGER_UP, snx, sny);
            tick(20);

            const float d1 = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
            const float d2 = std::sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.z - p1.z) * (p2.z - p1.z));
            std::snprintf(detail, sizeof(detail),
                          "finger held at (%.3f,%.3f): %.2f blocks in the first 150 ticks",
                          snx, sny, d1);
            check(d1 > 0.5f, "a held finger on the joystick walks", detail);
            std::snprintf(detail, sizeof(detail),
                          "%.2f blocks then %.2f blocks, no events in between", d1, d2);
            check(d2 > d1 * 0.5f, "and it keeps walking while it is held", detail);

            // Releasing must stop DRIVING the player — which is not the same as stopping the
            // player, and the difference is worth stating because the first version of this check
            // asserted the wrong one and failed. These checks run in fly mode (rise_until_clear,
            // so terrain cannot be mistaken for dead input), and fly mode damps lateral velocity
            // by 0.995 per frame on purpose (Classes/Player.mm:1036 — it is a glide). A released
            // stick therefore coasts for a long way and that is correct engine behaviour. What a
            // stuck touch would look like instead is a rate that does NOT decay, so decay is what
            // this asserts: six windows, each one slower than the last.
            // "Never faster than the first window, and clearly slower by the last" rather than a
            // strictly monotonic series: the engine integrates on its own substep, so one window
            // of six can come out a hair above its predecessor without anything being wrong. What
            // cannot happen while coasting is a window that beats the FIRST one.
            float first = 0.0f, last = 0.0f, peak = 0.0f;
            for (int w = 0; w < 6; ++w) {
                const PlayerState a = player_state();
                tick(20);
                const PlayerState b = player_state();
                const float d = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
                if (w == 0) first = d;
                if (d > peak) peak = d;
                last = d;
            }
            std::snprintf(detail, sizeof(detail),
                          "coasting down: %.3f -> %.3f blocks per 20 ticks, peak %.3f "
                          "(fly-mode 0.995 damping)", first, last, peak);
            // (1.02 lost to 1.175 vs 1.151 on Windows CI; that "substep jitter" was wall-clock etime,
            // fixed at the top of this block in X.11 — the 1.05 margin is now headroom, not noise.)
            check(peak <= first * 1.05f && last < first * 0.9f,
                  "lifting the finger stops driving the player", detail);


            // --- and WITHOUT fly mode: a held stick walks ---
            // Everything above flies (rise_until_clear), so until N.4.7 this gate never walked, and
            // "fly is fine, walking barely moves" reached a device. On a stone runway built in open
            // air, because the map centre is ponds and hills and a walk into water or a hillside
            // is not a question about input. Fixed 1/60 s etime: the engine's walk speed depends
            // on its tick rate (per-tick ground friction), so a wall-clock run would assert on
            // whatever rate this machine's vsync happens to give the harness. The tick rate the
            // REAL loop feeds the engine is the interactive loop's pacing, not this gate's.
            eden_set_fly_mode(0);   // (still on the fixed step set at the top of this block)
            const int ry = g_opt.height - 14;
            const float wx = (float)(4096 * CHUNK_SIZE) + 0.5f, wz = (float)(4096 * CHUNK_SIZE) + 0.5f;
            eden_console_teleport(wx, (float)ry + 3.0f, wz);
            tick(60);
            for (int x = -20; x <= 20; ++x)      // square, so a yaw nudged by a stray host mouse
                for (int z = -20; z <= 20; ++z) {    // still walks on stone
                    eden_console_setblock((int)wx + x, (int)wz + z, ry, 2 /*TYPE_STONE*/);
                    for (int y = 1; y <= 4; ++y) eden_console_setblock((int)wx + x, (int)wz + z, ry + y, 0);
                }
            eden_console_teleport(wx, (float)ry + 2.5f, wz);
            tick(120);
            const PlayerState w0 = player_state();
            push_finger_id(12, SDL_EVENT_FINGER_DOWN, snx, sny);
            tick(120);
            const PlayerState w1 = player_state();
            push_finger_id(12, SDL_EVENT_FINGER_UP, snx, sny);
            tick(30);
            g_app->viewController.setFixedEtime(0.0f);
            eden_set_fly_mode(1);
            const float dw = std::sqrt((w1.x - w0.x) * (w1.x - w0.x) + (w1.z - w0.z) * (w1.z - w0.z));
            std::snprintf(detail, sizeof(detail),
                          "%.2f blocks in 2 s of engine time on flat stone (~7.3 expected), dy %.2f",
                          dw, w1.y - w0.y);
            check(dw > 4.0f && std::fabs(w1.y - w0.y) < 0.5f, "a held stick walks with fly OFF", detail);
        }
    }

    // I.2: every edge-anchored HUD rect (and the rail-screen panel) inside the safe rect. Only a
    // real check where there are insets (`--device=iphone16`, `--safe-area=`, a notched device);
    // there it also proves it has teeth: with the insets ignored the same layout must FAIL.
    {
        float sx = 0, sy = 0, sw = 0, sh = 0;
        eden_safe_rect(&sx, &sy, &sw, &sh);
        char who[512];
        const int bad = eden_debug_safe_area_violations(sx, sy, sw, sh, who, sizeof(who));
        std::snprintf(detail, sizeof(detail), "safe rect %.0f,%.0f %.0fx%.0f in %.0fx%.0f; %d outside%s%s",
                      sx, sy, sw, sh, SCREEN_WIDTH, SCREEN_HEIGHT, bad, bad ? ": " : "", who);
        check(bad == 0, "every HUD hit box lies inside the safe rect", detail);
        if (sw < SCREEN_WIDTH || sh < SCREEN_HEIGHT) {
            eden_display_debug_ignore_safe_area(1);
            const int bad0 = eden_debug_safe_area_violations(sx, sy, sw, sh, who, sizeof(who));
            eden_display_debug_ignore_safe_area(0);
            std::snprintf(detail, sizeof(detail), "%d rect(s) outside with the insets ignored: %s", bad0, who);
            check(bad0 > 0, "the safe-area gate fails with the inset ignored", detail);
        }
    }

    std::printf("[eden-touch] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// W.2: native gates for the retired web checks that had no replacement (docs/web-port-archive.md)
// ---------------------------------------------------------------------------------------------
// Number readers for the eden_debug_* JSON probes. Each probe writes a fixed key set, so a strstr
// on `"key":` is exact; a missing key reads as -9999 (no real field has that value).
int probe_int(const char* json, const char* key) {
    char k[64];
    std::snprintf(k, sizeof(k), "\"%s\":", key);
    const char* p = json ? std::strstr(json, k) : nullptr;
    if (!p) return -9999;
    p += std::strlen(k);
    if (!std::strncmp(p, "true", 4)) return 1;
    if (!std::strncmp(p, "false", 5)) return 0;
    return atoi(p);
}
double probe_num(const char* json, const char* key) {
    char k[64];
    std::snprintf(k, sizeof(k), "\"%s\":", key);
    const char* p = json ? std::strstr(json, k) : nullptr;
    return p ? atof(p + std::strlen(k)) : -9999.0;
}
struct ProbeRect { float v[4]; };
ProbeRect probe_rect(const char* json, const char* key) {
    ProbeRect r = {{-1, -1, -1, -1}};
    char k[64];
    std::snprintf(k, sizeof(k), "\"%s\":[", key);
    const char* p = json ? std::strstr(json, k) : nullptr;
    if (p) std::sscanf(p + std::strlen(k), "%f,%f,%f,%f", &r.v[0], &r.v[1], &r.v[2], &r.v[3]);
    return r;
}
bool same_rect(const ProbeRect& a, const ProbeRect& b) {
    for (int i = 0; i < 4; ++i) if (std::fabs(a.v[i] - b.v[i]) >= 0.01f) return false;
    return true;
}
std::string rect_str(const ProbeRect& r) {
    char b[96];
    std::snprintf(b, sizeof(b), "[%g,%g,%g,%g]", r.v[0], r.v[1], r.v[2], r.v[3]);
    return b;
}

// --gl-selftest  (W.2 (a); was the GL half of web-port-final:web/tools/headless-shim-selftest.js)
// The shim's no-context bookkeeping: every headless gate boots through it (World::World() ->
// Graphics::initGraphics() issues glGenBuffers with no context), and the engine trusts the fake names
// to be nonzero and distinct (TerrainChunk.mm treats 0 as "no buffer yet"). The proof is
// eden_gl_selftest_run() in port/src/shim/gl/gl_fixed_function.cpp; this mode is headless so the
// guarded path is the one engaged. Diagnostics builds only, like the function.
#if defined(EDEN_DIAGNOSTICS)
extern "C" int eden_gl_selftest_run(void);
#endif
int run_gl_selftest() {
    g_selftestTag = "eden-gl";
#if defined(EDEN_DIAGNOSTICS)
    tick(3);
    check(eden_gl_have_context() == 0, "headless: no GL context is current", "");
    const int ok = eden_gl_selftest_run();
    check(ok == 1, "guarded glGen* names are nonzero and distinct", "see [eden-gl-selftest] above");
#else
    check(false, "this build has EDEN_DIAGNOSTICS", "eden_gl_selftest_run() is compiled out");
#endif
    std::printf("[eden-gl] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// --recolor-selftest  (W.2 (b); was web-port-final:web/tools/headless-recolor-test.js, audit row 11 / A5)
// The paint / door / creature-skin recolour pipeline. Its failure is invisible: a Texture2D whose
// GL name stayed 0 draws as nothing and reports no error. eden_debug_recolor_state(color)
// (port/src/seam/DebugState_web.mm) reports the stored source images, the recoloured textures'
// names and the positional skin/mask tables Resources fills while loading. Headless, the names are
// the shim's fake ones -- nonzero is still exactly the "something was uploaded" signal.
extern "C" const char* eden_debug_recolor_state(int color);
int run_recolor_selftest() {
    g_selftestTag = "eden-recolor";
    tick(3);
    std::printf("[eden-recolor] metrics IS_IPAD=%d SUPPORTS_RETINA=%d SCALE=%gx%g SCREEN=%gx%g\n", (int)IS_IPAD,
                (int)SUPPORTS_RETINA, SCALE_WIDTH, SCALE_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
    char d[600];
    std::string s1 = eden_debug_recolor_state(1);
    std::snprintf(d, sizeof(d), "%s", s1.c_str());
    check(probe_int(s1.c_str(), "stored_paint") != -9999, "Resources exists and the probe answers", d);
    std::printf("[eden-recolor] colour 1: %s\n", s1.c_str());
    const char* s = s1.c_str();
    check(probe_int(s, "stored_paint") == 1, "storedPaint was captured during texture load", "");
    check(probe_int(s, "stored_paint_mask") == 1, "storedPaintMask was captured during texture load", "");
    check(probe_int(s, "stored_door") == 1, "storedDoor was captured during texture load", "");
    check(probe_int(s, "stored_door_mask") == 1, "storedDoorMask was captured during texture load", "");
    const int pw = probe_int(s, "paint_w"), ph = probe_int(s, "paint_h");
    const int mw = probe_int(s, "mask_w"), mh = probe_int(s, "mask_h");
    std::snprintf(d, sizeof(d), "paint %dx%d, mask %dx%d", pw, ph, mw, mh);
    check(pw > 0 && ph > 0, "paint image decoded to real pixels", d);
    check(mw > 0 && mh > 0, "paint mask decoded to real pixels", d);
    check(pw == mw && ph == mh, "paint image and mask agree on size", d);
    check(pw == 90 && mw == 90, "the ipad~ (2x) paint art loaded, as on a device", d);
    check(probe_int(s, "paint_tex") > 0, "getPaintTex(1) produced a texture (name != 0)", "");
    check(probe_int(s, "door_tex") > 0, "getDoorTex(1) produced a texture (name != 0)", "");

    // paint_cache is ONE slot keyed on colour (a second colour must rebuild it); door_cache is
    // per colour, so two colours are two live names at once.
    const std::string a = eden_debug_recolor_state(2);
    const std::string b = eden_debug_recolor_state(7);
    const int da = probe_int(a.c_str(), "door_tex"), db = probe_int(b.c_str(), "door_tex");
    std::snprintf(d, sizeof(d), "door 2 -> %d, door 7 -> %d", da, db);
    check(probe_int(b.c_str(), "paint_tex") > 0, "a second paint colour also produces a texture", "");
    check(da > 0 && db > 0 && da != db, "door textures for two colours are distinct names", d);

    // The creature half: Resources fills storedSkins/storedMasks POSITIONALLY (the Nth texture load
    // after loadResources zeroes the counters), so a reordered load silently swaps creatures' skins.
    // 5 models x 2 states; 15 skin PNGs load and every third (the Rage variant) is not stored.
    std::snprintf(d, sizeof(d), "skins %d, masks %d, counters %d/%d", probe_int(s, "skins_filled"),
                  probe_int(s, "masks_filled"), probe_int(s, "skin_counter"), probe_int(s, "mask_counter"));
    check(probe_int(s, "skins_filled") == 10, "all 10 creature skins captured", d);
    check(probe_int(s, "masks_filled") == 10, "all 10 creature masks captured", d);
    check(probe_int(s, "skin_counter") == 10, "skin counter stopped at 10 (every 3rd skipped)", d);
    check(probe_int(s, "mask_counter") == 10, "mask counter stopped at 10", d);

    const std::string a2 = eden_debug_recolor_state(2);
    std::snprintf(d, sizeof(d), "%d then %d", da, probe_int(a2.c_str(), "door_tex"));
    check(probe_int(a2.c_str(), "door_tex") == da, "the door cache returns a stable name per colour", d);

    std::printf("[eden-recolor] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// --display-selftest  (W.2 (c); was web-port-final:web/tools/headless-display-profile-test.js, audit D1 + D4)
// The derived point space (port/src/seam/DisplayProfile_web.mm) and the re-layout it drives
// (Hud::layoutForScreen, Menu::layoutForScreen, Input::screenMetricsChanged). A rect that does not
// follow a metrics change is drawn in one place and hit-tested in another, silently. Pins: (1) the
// stock 568x320 HUD rects, byte-exact; (2) the derivation; (3) idempotent, reversible re-layout;
// (4) the profiles' chrome and the classic-width floor; (5) the aspect clamps; (6) X.20's whole-
// pixels-per-point snap on the desktop profile. --touch-selftest
// covers the touch half at a real window; this one is headless and drives the viewport itself
// (eden_display_set_viewport is exactly what DisplayMode_native.cpp calls on a window resize).
extern "C" void eden_display_set_viewport(int css_w, int css_h);
extern "C" void eden_display_set_viewport_px(int css_w, int css_h, int px_w, int px_h);   // X.20
extern "C" int  eden_display_px_per_point(void);
extern "C" void eden_display_refresh(void);
int run_display_selftest() {
    g_selftestTag = "eden-display";
    if (!tick_until([] { return probe_int(eden_debug_display_state(), "hud") == 1; }, 600, "the Hud")) {
        check(false, "World + Hud came up", eden_debug_display_state());
        std::printf("[eden-display] FAILURES (%d failure(s))\n", g_selftestFailures);
        return 1;
    }
    char d[300];
    std::string st;
    auto state = [&]() -> const char* { st = eden_debug_display_state(); return st.c_str(); };
    // Option indexes: ui_scale 0 Auto, 1 100%, 2 125%, 3 150%, 4 200%; display_layout 0 Auto,
    // 1 Classic, 2 Adaptive; input_mode 0 Auto, 1 Touch, 2 Keyboard+Mouse.
    auto set = [](const char* key, float v) {
        if (!set_setting_by_key(key, v)) std::fprintf(stderr, "[eden-display] no setting %s\n", key);
        tick(2);
    };
    auto pickViewport = [&](const char* label, const char* s) {
        // Util.mm's findWorldCoords unprojects a point-space tap scaled by SCALE_* (2) against
        // GL_VIEWPORT, so GL_VIEWPORT must answer the point space x2, never the real drawable.
        ProbeRect pv = probe_rect(s, "pick_viewport");   // two numbers; the other two stay -1
        const double w = probe_num(s, "SCREEN_WIDTH"), h = probe_num(s, "SCREEN_HEIGHT");
        std::snprintf(d, sizeof(d), "%s: [%g,%g] vs %gx%g x2", label, pv.v[0], pv.v[1], w, h);
        check(pv.v[0] == (float)(w * 2) && pv.v[1] == (float)(h * 2), "GL_VIEWPORT is the point space x2", d);
    };
    auto gridInCard = [&](const char* label, const char* s) {
        const ProbeRect c = probe_rect(s, "rpaintframe");
        bool ok = true;
        for (const char* k : {"block0", "color0"}) {
            const ProbeRect r = probe_rect(s, k);
            ok = ok && r.v[0] >= c.v[0] && r.v[1] >= c.v[1] && r.v[0] + r.v[2] <= c.v[0] + c.v[2] &&
                 r.v[1] + r.v[3] <= c.v[1] + c.v[3];
        }
        std::snprintf(d, sizeof(d), "%s: card %s block0 %s color0 %s", label, rect_str(c).c_str(),
                      rect_str(probe_rect(s, "block0")).c_str(), rect_str(probe_rect(s, "color0")).c_str());
        check(ok, "the picker grid sits inside the picker card", d);
    };
    auto dims = [&](const char* s) {
        std::snprintf(d, sizeof(d), "%gx%g (aspect %.4f)", probe_num(s, "SCREEN_WIDTH"), probe_num(s, "SCREEN_HEIGHT"),
                      probe_num(s, "P_ASPECT_RATIO"));
        return (const char*)d;
    };

    // ---- 1. The classic profile is byte-exact ----------------------------------------------------
    set("input_mode", 2);
    set("display_layout", 1);
    set("ui_scale", 4);
    const std::string classic = state();
    const char* c = classic.c_str();
    check(probe_num(c, "SCREEN_WIDTH") == 568 && probe_num(c, "SCREEN_HEIGHT") == 320, "classic: 568x320 points", dims(c));
    check(std::fabs(probe_num(c, "P_ASPECT_RATIO") - 1.775) < 0.001, "classic: P_ASPECT_RATIO is 1.775", dims(c));
    check(probe_int(c, "IS_WIDESCREEN") == 1, "classic: IS_WIDESCREEN", "");
    const ProbeRect scr = probe_rect(c, "scr");
    std::snprintf(d, sizeof(d), "Input [%g,%g]", scr.v[0], scr.v[1]);
    check(scr.v[0] == 568 && scr.v[1] == 320, "classic: Input's point space matches the engine", d);
    // The stock rects, by hand from Classes/Hud.mm's constants at 568x320:
    //   HUDR_X = 568-45-13 = 510; rmine.y = 4*320/5-45-3 = 208; rjumphit.x = 510-17-1 = 492, w = 76;
    //   rmenu.y = 320-45 = 275; blockBounds[0] = (7+15+4+57, -3+320-10-15-38) = (83, 254);
    //   rburn.y = 320-45-3 = 272; rpaint.y = 2*320/5-45-3+5 = 85; rpaintframe = (4+57, 10+10) 402x282.
    const struct { const char* key; ProbeRect want; } kStock[] = {
        {"rmine",       {{510, 208, 45, 45}}},
        {"rjumphit",    {{492, 0, 76, 68}}},
        {"rmenu",       {{0, 275, 45, 45}}},
        {"block0",      {{83, 254, 38, 38}}},
        {"rburn",       {{510, 272, 45, 45}}},
        {"rpaint",      {{510, 85, 45, 45}}},
        {"rpaintframe", {{61, 20, 402, 282}}},
    };
    for (const auto& k : kStock) {
        const ProbeRect got = probe_rect(c, k.key);
        char what[64];
        std::snprintf(what, sizeof(what), "classic: %s is the stock rect", k.key);
        std::snprintf(d, sizeof(d), "%s, want %s", rect_str(got).c_str(), rect_str(k.want).c_str());
        check(same_rect(got, k.want), what, d);
    }
    pickViewport("classic", c);

    // ---- 2. The derivation ----------------------------------------------------------------------
    set("display_layout", 2);
    set("ui_scale", 1);
    eden_display_set_viewport(1600, 1000);
    tick(2);
    const std::string wide = state();
    const char* w = wide.c_str();
    check(probe_num(w, "SCREEN_HEIGHT") == 640, "adaptive/100%: point height is 640", dims(w));
    check(probe_num(w, "SCREEN_WIDTH") == 1024, "adaptive/100%: point width follows the 1.6 aspect", dims(w));
    check(std::fabs(probe_num(w, "P_ASPECT_RATIO") - probe_num(w, "SCREEN_WIDTH") / probe_num(w, "SCREEN_HEIGHT")) < 0.0005,
          "adaptive: P_ASPECT_RATIO is always width/height", dims(w));
    check(std::abs(probe_int(w, "aspect_x1000") - 1600) <= 2, "adaptive: the aspect getter agrees", dims(w));
    const ProbeRect wscr = probe_rect(w, "scr");
    std::snprintf(d, sizeof(d), "Input [%g,%g]", wscr.v[0], wscr.v[1]);
    check(wscr.v[0] == 1024 && wscr.v[1] == 640, "adaptive: Input's point space followed", d);
    check(std::fabs(probe_rect(w, "rmine").v[0] - (1024 - 45 - 13)) < 0.01, "adaptive: the right-anchored column followed",
          rect_str(probe_rect(w, "rmine")).c_str());
    check(std::fabs(probe_rect(w, "rmenu").v[1] - (640 - 45)) < 0.01, "adaptive: the top-anchored icon followed",
          rect_str(probe_rect(w, "rmenu")).c_str());
    pickViewport("adaptive/100%", w);
    gridInCard("adaptive/100%", w);
    {
        const float burnY = probe_rect(w, "rburn").v[1], paintY = probe_rect(w, "rpaint").v[1];
        std::snprintf(d, sizeof(d), "rburn.y %g, rpaint.y %g", burnY, paintY);
        // rpaint carries a stock +5 nudge on top of its three 64-point steps.
        check(std::fabs((burnY - paintY + 5) - 64 * 3) < 0.01, "adaptive: the mode column keeps its 64-point pitch", d);
        check(std::fabs(burnY - (640 - 45 - 3)) < 0.01, "adaptive: the mode column stays top-anchored", d);
    }
    const ProbeRect mf = probe_rect(w, "rmenuframe");
    check(std::fabs(mf.v[2] - 268) < 0.01 && std::fabs(mf.v[3] - 240) < 0.01, "adaptive: the menu card keeps 268x240",
          rect_str(mf).c_str());
    set("ui_scale", 2);
    const std::string dense = state();
    check(probe_num(dense.c_str(), "SCREEN_HEIGHT") == 512, "ui_scale 125%: point height is 512", dims(dense.c_str()));
    check(std::fabs(probe_num(dense.c_str(), "P_ASPECT_RATIO") - probe_num(w, "P_ASPECT_RATIO")) < 0.005,
          "ui_scale 125%: the aspect is unchanged", dims(dense.c_str()));
    pickViewport("ui_scale 125%", dense.c_str());
    gridInCard("ui_scale 125%", dense.c_str());

    // ---- 3. Re-layout is idempotent and reversible -----------------------------------------------
    set("ui_scale", 1);
    const std::string again = state();
    std::snprintf(d, sizeof(d), "block0 %s vs %s", rect_str(probe_rect(again.c_str(), "block0")).c_str(),
                  rect_str(probe_rect(w, "block0")).c_str());
    check(same_rect(probe_rect(again.c_str(), "rmine"), probe_rect(w, "rmine")) &&
          same_rect(probe_rect(again.c_str(), "block0"), probe_rect(w, "block0")) &&
          same_rect(probe_rect(again.c_str(), "color0"), probe_rect(w, "color0")),
          "idempotent: returning to a size reproduces its rects", d);
    // block0.x carries marginLeft2, which the layout body MUTATES: a pass that did not reset it
    // would drift right on every refresh.
    eden_display_refresh();
    eden_display_refresh();
    tick(2);
    std::snprintf(d, sizeof(d), "block0 %s", rect_str(probe_rect(state(), "block0")).c_str());
    check(same_rect(probe_rect(st.c_str(), "block0"), probe_rect(w, "block0")), "idempotent: extra refreshes do not drift", d);

    // ---- 4. Back to classic: the stock numbers again ---------------------------------------------
    set("display_layout", 1);
    set("ui_scale", 4);
    const std::string back = state();
    bool same = true;
    for (const char* k : {"rmine", "rjumphit", "rmenu", "block0", "color0", "rpaintframe"})
        same = same && same_rect(probe_rect(back.c_str(), k), probe_rect(c, k));
    std::snprintf(d, sizeof(d), "block0 %s", rect_str(probe_rect(back.c_str(), "block0")).c_str());
    check(same, "round trip: classic rects identical after adaptive", d);

    // ---- 5. The profiles ---------------------------------------------------------------------------
    set("input_mode", 1);
    const std::string touch = state();
    check(std::strstr(touch.c_str(), "\"profile\":\"touch\"") != nullptr, "forcing Touch selects the touch profile", "");
    check(probe_int(touch.c_str(), "touch_chrome") == 1 && probe_int(touch.c_str(), "use_joystick") == 1,
          "the touch profile turns the on-screen chrome on", "");
    set("input_mode", 2);
    const std::string desk = state();
    check(std::strstr(desk.c_str(), "\"profile\":\"desktop\"") != nullptr, "forcing Keyboard+Mouse selects desktop", "");
    check(probe_int(desk.c_str(), "touch_chrome") == 0 && probe_int(desk.c_str(), "use_joystick") == 0,
          "the desktop profile turns the on-screen chrome off", "");
    // Auto + the touch profile on a 16:9 screen is the shipped layout, rect for rect.
    set("ui_scale", 0);
    set("display_layout", 0);
    set("input_mode", 1);
    eden_display_set_viewport(1136, 640);
    tick(2);
    const std::string at = state();
    check(probe_num(at.c_str(), "SCREEN_WIDTH") == 568 && probe_num(at.c_str(), "SCREEN_HEIGHT") == 320,
          "Auto + touch on 16:9 is still 568x320", dims(at.c_str()));
    check(same_rect(probe_rect(at.c_str(), "rmine"), probe_rect(c, "rmine")) &&
          same_rect(probe_rect(at.c_str(), "block0"), probe_rect(c, "block0")),
          "Auto + touch reproduces the stock HUD rects", rect_str(probe_rect(at.c_str(), "block0")).c_str());
    // On 4:3 it fills the screen, and the classic-width floor keeps the space >= 568 wide.
    eden_display_set_viewport(1024, 768);
    tick(2);
    const std::string ipad = state();
    check(std::abs(probe_int(ipad.c_str(), "aspect_x1000") - 1333) <= 5, "Auto + touch on 4:3 fills it", dims(ipad.c_str()));
    check(probe_num(ipad.c_str(), "SCREEN_WIDTH") >= 568, "the classic-width floor keeps >= 568 points", dims(ipad.c_str()));
    eden_display_set_viewport(800, 1200);
    tick(2);
    check(probe_num(state(), "SCREEN_WIDTH") >= 568, "the floor holds at the aspect clamp", dims(st.c_str()));
    eden_display_set_viewport(568, 320);
    tick(2);
    check(probe_num(state(), "SCREEN_WIDTH") == 568 && probe_num(st.c_str(), "SCREEN_HEIGHT") == 320,
          "an iPhone-5-sized viewport resolves to 568x320", dims(st.c_str()));

    // ---- 6. The clamps ----------------------------------------------------------------------------
    set("input_mode", 2);
    eden_display_set_viewport(400, 900);
    tick(2);
    check(std::abs(probe_int(state(), "aspect_x1000") - 1200) <= 10, "a portrait viewport clamps to aspect 1.20", dims(st.c_str()));
    eden_display_set_viewport(3840, 1080);
    tick(2);
    check(std::abs(probe_int(state(), "aspect_x1000") - 2400) <= 10, "a 32:9 viewport clamps to aspect 2.40", dims(st.c_str()));

    // ---- 7. X.20: the pixel snap (desktop, adaptive) ----------------------------------------------
    // Given the box in device pixels, the desktop point space is a whole number of pixels per point
    // (1 or even preferred; odd only when 25% closer). Expected values are the measured windows:
    // the default 1136x640 on the 1x and the 2x monitor, a maximised 1080p window, a maximised
    // window on the "looks like 2304x1296" 2x screen.
    set("display_layout", 2);
    set("ui_scale", 2);
    const struct { int cw, ch, pw, ph, wantW, wantH, wantP; const char* what; } kSnap[] = {
        {1136, 640, 1136,  640, 1136, 640, 1, "125%, default window, 1x: 1 px/pt"},
        {1136, 640, 2272, 1280, 1136, 640, 2, "125%, default window, 2x: 2 px/pt, same points"},
        {1920, 1050, 1920, 1050, 960, 525, 2, "125%, maximised 1080p: 2 px/pt, art 1:1, odd height kept"},
        {2304, 1240, 4608, 2480, 1152, 620, 4, "125%, maximised 2x 2304-wide: 4 px/pt"},
    };
    for (const auto& k : kSnap) {
        eden_display_set_viewport_px(k.cw, k.ch, k.pw, k.ph);
        tick(2);
        std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
        check(probe_num(st.c_str(), "SCREEN_WIDTH") == k.wantW && probe_num(st.c_str(), "SCREEN_HEIGHT") == k.wantH &&
              eden_display_px_per_point() == k.wantP, k.what, d);
    }
    pickViewport("snapped 4 px/pt", st.c_str());
    set("ui_scale", 4);
    eden_display_set_viewport_px(1920, 1050, 1920, 1050);
    tick(2);
    std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
    check(probe_num(st.c_str(), "SCREEN_HEIGHT") == 350 && eden_display_px_per_point() == 3,
          "200%, maximised 1080p: an odd 3 px/pt when it is much closer", d);
    set("ui_scale", 2);
    eden_display_set_viewport(1136, 640);
    tick(2);
    std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
    check(probe_num(st.c_str(), "SCREEN_HEIGHT") == 512 && eden_display_px_per_point() == 0,
          "no pixel size: unsnapped, the old 512-point space", d);
    set("input_mode", 1);
    set("ui_scale", 0);
    eden_display_set_viewport_px(1136, 640, 2272, 1280);
    tick(2);
    std::snprintf(d, sizeof(d), "%s, snap %d", dims(state()), eden_display_px_per_point());
    check(probe_num(st.c_str(), "SCREEN_WIDTH") == 568 && probe_num(st.c_str(), "SCREEN_HEIGHT") == 320 &&
          eden_display_px_per_point() == 0, "the touch profile is never snapped", d);
    set("input_mode", 2);

    std::printf("[eden-display] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// --convert64-selftest  (W.2 (e); was web-port-final:web/tools/headless-256z-convert-crosscheck.js)
// FileManager::convertWorldTo64 is a from-scratch C++ restatement of port/tools/eden-convert.js's
// `--to-64`; the two are kept in step by hand. This authors a 256z world with known content above
// y=63 (a 6x6 lattice of brick pillars at four heights, 20 apart so each lands in its own column,
// and three door pairs straddling the cut: DOOR1 at 63 kept, DOOR_TOP at 64 cut = one orphan each),
// saves it, exports the `.emod` to a NewDawn256z `.eden`, then converts with the engine twice --
// the `.emod` (what the Storage tab runs on a live world) and a copy of the `.eden` (an import
// source) -- and prints each report. port/tools/convert64-crosscheck.js runs this mode, converts the
// kept reference copy (<docs>/convert64-ref/src256.eden) with eden-convert.js and asserts all
// three agree. The in-process checks below are only the floor: the content really landed above 63.
void mkdirs(const std::string& path);   // defined with main()
int run_convert64_selftest() {
    g_selftestTag = "eden-convert64";
    const char* name = g_opt.world.empty() ? "convert64-xcheck" : g_opt.world.c_str();
    if (!open_world(name, 256)) return 1;
    tick(g_opt.frames / 2);
    char d[600];
    check(g_world_height == 256, "the world opened at height 256", eden_debug_world_format());

    const PlayerState ps = player_state();
    const int bx = (int)std::lround(ps.x), bz = (int)std::lround(ps.z);
    const int kHighY[] = {64, 90, 150, 220};
    int placed = 0;
    for (int gx = 0; gx < 6; ++gx)
        for (int gz = 0; gz < 6; ++gz)
            for (int y : kHighY) { eden_console_setblock(bx + (gx - 3) * 20, bz + (gz - 3) * 20, y, 13 /* TYPE_BRICK */); placed++; }
    // Off the lattice (its offsets are multiples of 20 in -60..40), so no door top overwrites a brick.
    const int kDoors[3][2] = {{bx + 60, bz}, {bx, bz + 60}, {bx - 50, bz - 50}};
    for (const auto& c : kDoors) {
        eden_console_setblock(c[0], c[1], 63, 66 /* TYPE_DOOR1 */);
        eden_console_setblock(c[0], c[1], 64, 70 /* TYPE_DOOR_TOP */);
    }
    // Creatures above the cut, for the dropped/relocated counters: a 5x5 brick deck at y=150, off the
    // lattice (x and z offsets 8..12), and three creatures stood on it just before the save.
    for (int x = bx + 8; x <= bx + 12; ++x)
        for (int z = bz + 8; z <= bz + 12; ++z) { eden_console_setblock(x, z, 150, 13 /* TYPE_BRICK */); placed++; }
    std::snprintf(d, sizeof(d), "getblock(%d,%d,220) = %d", bx, bz, eden_console_getblock(bx, bz, 220));
    check(eden_console_getblock(bx, bz, 220) == 13, "a high-band block reads back before saving", d);
    std::snprintf(d, sizeof(d), "door at 63/64: %d/%d", eden_console_getblock(kDoors[0][0], kDoors[0][1], 63),
                  eden_console_getblock(kDoors[0][0], kDoors[0][1], 64));
    check(eden_console_getblock(kDoors[0][0], kDoors[0][1], 63) == 66 &&
          eden_console_getblock(kDoors[0][0], kDoors[0][1], 64) == 70, "a door pair straddles the cut", d);
    tick(30);
    int spawned = 0;
    for (int i = 0; i < 3; ++i) spawned += SpawnCreatureAt(i, Vector{bx + 9.5f + i, 151.5f, bz + 10.5f}) ? 1 : 0;
    std::snprintf(d, sizeof(d), "%d of 3", spawned);
    check(spawned == 3, "three creatures spawn on the deck above the cut", d);
    save_world();
    if (!quit_to_menu()) return 1;
    tick(10);

    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_name(i), name)) idx = i;
    check(idx >= 0, "the world is listed after the save", name);
    if (idx < 0) return 1;
    const std::string docs = eden_platform_documents_root();
    const std::string file = eden_menu_world_file(idx);
    const std::string refDir = docs + "/convert64-ref";
    mkdirs(refDir);
    const std::string ref = refDir + "/src256.eden", engEden = "convert64-xcheck-src.eden";
    emod::ExportOptions xo;
    xo.target = emod::X_NEWDAWN256Z;
    std::string err;
    const bool exA = emod::export_world_file(docs + "/" + file, ref, xo, false, nullptr, &err);
    const bool exB = exA && emod::export_world_file(docs + "/" + file, docs + "/" + engEden, xo, false, nullptr, &err);
    std::snprintf(d, sizeof(d), "%s -> %s: %s", file.c_str(), ref.c_str(), err.empty() ? "ok" : err.c_str());
    check(exA && exB, "the 256z world exports to a NewDawn256z .eden (twice)", d);
    if (!exA || !exB) return 1;
    std::printf("[eden-convert64] ref %s\n", ref.c_str());
    std::printf("[eden-convert64] placed %d\n", placed + 3);

    auto report = [&](const char* leg, NSString* target) {
        const ConvertTo64Report r = World::getWorld->fm->convertWorldTo64(target);
        std::printf("[eden-convert64] engine-%s {\"ok\":%s,\"columns\":%d,\"blocksDiscarded\":%d,\"columnsAffected\":%d,"
                    "\"doorsOrphaned\":%d,\"creaturesDropped\":%d,\"creaturesRelocated\":%d,\"creaturesOverflow\":%d}\n",
                    leg, r.ok ? "true" : "false", r.columns, r.blocksDiscarded, r.columnsAffected, r.doorsOrphaned,
                    r.creaturesDropped, r.creaturesRelocated, r.creaturesOverflow);
        std::snprintf(d, sizeof(d), "%s: ok %d \"%s\", discarded %d, orphaned doors %d", leg, r.ok ? 1 : 0, r.error,
                      r.blocksDiscarded, r.doorsOrphaned);
        check(r.ok, "convertWorldTo64 succeeds", d);
        check(r.blocksDiscarded >= placed + 3, "it discards at least the bricks and door tops placed", d);
        check(r.doorsOrphaned == 3, "it clears exactly the three orphaned doors", d);
        std::snprintf(d, sizeof(d), "%s: dropped %d, relocated %d, overflow %d", leg, r.creaturesDropped,
                      r.creaturesRelocated, r.creaturesOverflow);
        check(r.creaturesDropped >= 1, "it drops the creatures above the cut", d);
    };
    @autoreleasepool {
        report("emod", [NSString stringWithUTF8String:file.c_str()]);
        report("eden", [NSString stringWithUTF8String:engEden.c_str()]);
    }

    std::printf("[eden-convert64] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --objc-selftest  (Phase N Stage 3.3)
// ---------------------------------------------------------------------------------------------
// A mode that exists only on the targets that USE this port's own Objective-C runtime — i.e. not
// macOS, which uses Apple's libobjc and needs no proof from us. On Linux and Windows it is the
// single most load-bearing check in the stage: port/src/shim/objc/objc_runtime.cpp is 800 lines of
// hand-written ABI, it had only ever run on wasm32, and every failure mode it has is silent
// (a wrong ivar offset reads the wrong memory; a wrong dispatch reaches the wrong method).
//
// The proof itself is port/src/shim/objc/objc_selftest.mm, shared with the web build's
// headless-shim-selftest suite, so a regression shows up on both targets from one source.
//
// It runs BEFORE any world is touched — a broken runtime would otherwise present as a corrupt
// save or a wrong checksum several thousand message sends downstream, which is exactly the kind of
// diagnosis this project has paid for before.
#if !defined(__APPLE__) && defined(EDEN_DIAGNOSTICS)
extern "C" int eden_objc_selftest_run(void);
#endif

int run_objc_selftest() {
#if defined(__APPLE__)
    std::fprintf(stderr, "[eden-objc] this target uses Apple's libobjc, not "
                         "port/src/shim/objc/objc_runtime.cpp — nothing to self-test.\n");
    return 0;
#elif !defined(EDEN_DIAGNOSTICS)
    std::fprintf(stderr, "[eden-objc] built without -DEDEN_DIAGNOSTICS=ON; the self-test is not "
                         "compiled in.\n");
    return 1;
#else
    return eden_objc_selftest_run() == 1 ? 0 : 1;
#endif
}

// ---------------------------------------------------------------------------------------------
// --audio-selftest
// ---------------------------------------------------------------------------------------------
// The audio twin of --input-selftest, and it exists for the same reason: "sound works" is
// otherwise only checkable by someone with speakers on, and that is not a regression test. It
// asserts the two things that can silently break — that the DECODER handled each of the five
// formats the sound library actually ships, and that a played effect reached the MIXER — and
// leaves the one thing it cannot judge (does it sound right) to a human.
//
// The five probes are one real file per format, chosen from what Classes/Resources.mm names:
// IMA4 .caf is 428 of the 436 effects, the others are a handful each.
int run_audio_selftest() {
    g_selftestTag = "eden-audio";
    struct Probe { const char* file; const char* what; };
    const Probe probes[] = {
        {"block_break_stone_1.caf",              ".caf (Apple IMA4 — 428 of 436 effects)"},
        {"explosion.caf",                        ".caf (the explosion set)"},
        {"ambience_open.wav",                    ".wav (ambience bed)"},
        {"trampoline_block_bounce_sound.mp3",    ".mp3 (effect)"},
        {"Eden_title.mp3",                       ".mp3 (music track)"},
        // Phase N Stage 3.5. One file in the whole library is AIFF, and on the non-Apple targets
        // it is the only user of a hand-written decoder that nothing else exercises — so without
        // this probe the AIFF path has no coverage at all on the platforms that actually need it.
        {"Grab.aif",                             ".aif (the one AIFF in the library)"},
    };
    char detail[200];
    for (const Probe& p : probes) {
        int frames = 0, ch = 0, rate = 0;
        const bool ok = eden_native_audio_probe(p.file, &frames, &ch, &rate) != 0;
        if (ok) std::snprintf(detail, sizeof(detail), "%s: %d frames, %d ch, %d Hz",
                              p.file, frames, ch, rate);
        else    std::snprintf(detail, sizeof(detail), "%s: DECODE FAILED", p.file);
        check(ok && frames > 0, p.what, detail);
        // Drop it again: Eden_title.mp3 decodes to ~8 M frames, i.e. ~32 MB of PCM in the effect
        // cache, and the cache is meant for short sounds. Nothing in the engine plays a music
        // track as an effect; only this probe does, and only to prove the decoder reads mp3.
        CocosDenshion::SimpleAudioEngine::sharedEngine()->unloadEffect(p.file);
    }

    // Reaching the mixer. playSound() goes through Resources, i.e. the path the engine itself
    // uses, rather than calling the backend directly — the point is that the ENGINE's sound comes
    // out, not that the backend can be driven.
    {
        CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
        const unsigned int id = a->playEffect("explosion.caf", false);
        const int voices = eden_native_audio_voice_count();
        std::snprintf(detail, sizeof(detail), "voice id %u, %d voice(s) bound", id, voices);
        check(id != 0 && voices > 0, "an effect reaches the mixer", detail);

        // ...and drains. A voice that never reports empty is the leak that fills the 48-voice
        // pool a few minutes into a session, so the reap path is asserted, not assumed.
        for (int i = 0; i < 600 && eden_native_audio_voice_count() > 0; ++i) {
            eden_native_audio_tick();
            SDL_Delay(10);
        }
        std::snprintf(detail, sizeof(detail), "%d voice(s) left", eden_native_audio_voice_count());
        check(eden_native_audio_voice_count() == 0, "the voice drains and is reaped", detail);
    }

    // A streaming channel. isBackgroundMusicPlaying() is AVAudioPlayer's own view of itself, so
    // this is "the file opened, decoded and is running", not "we called play".
    {
        CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
        a->setBackgroundMusicVolume(0.6f);
        a->playBackgroundMusic("Eden_title.mp3", false);
        SDL_Delay(300);
        const bool playing = a->isBackgroundMusicPlaying();
        std::snprintf(detail, sizeof(detail), "isBackgroundMusicPlaying=%d", playing ? 1 : 0);
        check(playing, "the music channel streams", detail);
        a->stopBackgroundMusic(false);
    }

    // An ambience channel, driven the way Resources::update drives them: play, then push a fade
    // value every frame. The fade is the half that is easy to get wrong — a channel that plays at
    // volume 0 forever is indistinguishable from one that never started.
    {
        CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
        a->setAmbienceVolume(1.0f);
        a->playAmbience(0, "ambience_open.wav", true);
        a->setAmbienceFade(0, 1.0f);
        SDL_Delay(400);
        std::snprintf(detail, sizeof(detail), "isAmbiencePlaying(0)=%d", a->isAmbiencePlaying(0) ? 1 : 0);
        check(a->isAmbiencePlaying(0), "an ambience layer streams", detail);
        a->stopAmbience(0);
    }

    // ...and the same thing IN A WORLD, driven by the engine rather than by this harness. This is
    // the leg that matters: the direct call above proves the backend works, and only a real
    // session proves Resources::soundEventBed ever reaches it (it early-returns unless `playmusic`
    // is on and the game mode is PLAY, and the bed layer is chosen from the biome under the
    // player).
    {
        g_tickInput = false;
        const char* w = g_opt.world.empty() ? "audio-selftest" : g_opt.world.c_str();
        if (open_world(w, g_opt.height)) {
            tick(600);
            CocosDenshion::SimpleAudioEngine* a = CocosDenshion::SimpleAudioEngine::sharedEngine();
            const int bed = a->isAmbiencePlaying(0) ? 1 : 0;
            const int prox = a->isAmbiencePlaying(1) ? 1 : 0;
            std::snprintf(detail, sizeof(detail),
                          "bed=%d(%s vol %.2f) prox=%d playmusic=%d music=%d",
                          bed, eden_native_audio_channel_name(1),
                          eden_native_audio_channel_volume(1), prox,
                          Resources::getResources ? Resources::getResources->playmusic : -1,
                          a->isBackgroundMusicPlaying() ? 1 : 0);
            check(bed != 0 && eden_native_audio_channel_volume(1) > 0.0f,
                  "the engine starts an AUDIBLE ambience bed in-world", detail);

            // 2026-10-05, two music bugs the user heard. (1) Resources::update wrote its song
            // crossfade into the SLIDER's knob every frame, so a Music volume change lasted one
            // frame. The slider value must survive the engine running.
            auto idx = [](const char* key) {
                for (int i = 0; i < eden_settings_count(); ++i)
                    if (std::strcmp(eden_settings_key(i), key) == 0) return i;
                return -1;
            };
            const int mv = idx("music_volume"), mu = idx("music");
            if (mv >= 0) {
                eden_settings_set(mv, 0.30f);
                tick(30);
                const float got = a->getBackgroundMusicVolume();
                std::snprintf(detail, sizeof(detail), "set 0.30, after 30 frames %.2f", got);
                check(std::fabs(got - 0.30f) < 0.001f, "the Music volume setting survives Resources::update", detail);
                eden_settings_set(mv, 1.0f);
            }
            // (2) Switching Music on from the in-game settings called playMenuTune(): a TITLE
            // track over gameplay. In a world it must cue an in-game song instead.
            if (mu >= 0) {
                eden_settings_set(mu, 0.0f);
                tick(5);
                eden_settings_set(mu, 1.0f);
                tick(30);
                const char* ch0 = eden_native_audio_channel_name(0);
                std::snprintf(detail, sizeof(detail), "channel 0 = %s, playing=%d",
                              ch0 ? ch0 : "(none)", a->isBackgroundMusicPlaying() ? 1 : 0);
                check(ch0 && !std::strstr(ch0, "title") && a->isBackgroundMusicPlaying(),
                      "Music switched on in a world plays an in-game song, not a title track", detail);
            }
        }
    }

    std::printf("[eden-audio] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --leak-probe — the native counterpart of web-port-final:web/tools/headless-alloc-leak-probe.js and of
// headless-heap-ceiling-probe.js --torture=same64 (Phase N Stage 3 verification).
// ---------------------------------------------------------------------------------------------
// WHY THIS IS A NATIVE MODE AND NOT A REUSE OF THOSE TOOLS: both of them are node scripts that
// instantiate `eden.js` and call exports through `Module._`. There is no Module here. The measured
// quantity, though, is exactly theirs — load a world, quit to the menu, repeat in ONE process, and
// require the allocator's live-bytes figure to be FLAT — so this drives the same engine exports in
// the same order and prints the same statistic under the same key.
//
// It is Stage 3's obligation rather than a nice-to-have because the shim Foundation is running
// outside Emscripten for the first time, and that is precisely M6's leak class: this runtime's
// object_dispose() is a bare free() with no .cxx_destruct, so every shim class holding a C++ ivar
// owes a hand-written -dealloc. A missing one leaks silently and only shows up as a slope.
//
// The signal is `heapSize` (HeapProbe_native.cpp's allocInUse: malloc_zone size_in_use on Apple,
// mallinfo2 uordblks on glibc, PrivateUsage commit charge on Windows — read the Windows one as a
// trend, not a byte count). RSS is printed beside it but NOT asserted on: the OS is free to keep
// freed pages mapped, so RSS can plateau above the live figure without anything leaking.
//
// CYCLE 0 IS A WARM-UP AND IS NOT IN THE SLOPE. The first load populates caches that are supposed
// to persist for the life of the process — the texture set, the atlas, the resident block window —
// and counting that as leakage would make any threshold meaningless.
double heap_field(const char* json, const char* key) {
    char pat[64];
    std::snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* k = std::strstr(json, pat);
    return k ? atof(k + std::strlen(pat)) : -1.0;
}

int run_leak_probe() {
    g_selftestTag = "eden-leak";
    const char* name = g_opt.world.empty() ? "leak-probe" : g_opt.world.c_str();
    const int cycles = g_opt.cycles < 3 ? 3 : g_opt.cycles;
    const double MB = 1024.0 * 1024.0;

    std::vector<double> live(cycles, 0.0), rss(cycles, 0.0);

    tick(g_opt.frames / 4);
    std::printf("[eden-leak] %d cycles of load->quit on a %dz world, one process\n",
                cycles, g_opt.height);
    std::fflush(stdout);

    for (int c = 0; c < cycles; ++c) {
        if (!open_world(name, g_opt.height)) return 1;
        // The same fixed origin the checksum modes use, so the load does real streaming work
        // rather than sitting on whatever column the spawn picked.
        eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
        tick(g_opt.frames / 2);
        if (!quit_to_menu()) return 1;
        tick(g_opt.frames / 4);

        const char* h = eden_debug_heap();
        live[c] = heap_field(h, "heapSize");
        rss[c]  = heap_field(h, "rss");
        std::printf("[eden-leak] cycle %d/%d at the menu: live %7.2f MB   rss %7.2f MB%s\n",
                    c + 1, cycles, live[c] / MB, rss[c] / MB,
                    c == 0 ? "   (warm-up, not in the slope)" : "");
        std::fflush(stdout);
    }

    // A LEAST-SQUARES SLOPE, not first-vs-last, and the difference is not pedantry: the live figure
    // jitters by several MB between samples (18.5 / 23.6 / 23.7 / 19.3 / 19.0 in the run this was
    // sized against), because how much of the block window and how many pooled meshes are alive at
    // the instant of the sample is not identical cycle to cycle. Two endpoints can land on opposite
    // ends of that jitter and manufacture a slope out of nothing; a fit over every post-warm-up
    // point cannot.
    const int n = cycles - 1;                       // cycles 1..cycles-1, i.e. warm-up excluded
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int c = 1; c < cycles; ++c) {
        const double x = (double)c, y = live[c];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double denom = (double)n * sxx - sx * sx;
    const double perCycle = denom != 0.0 ? ((double)n * sxy - sx * sy) / denom : 0.0;
    char detail[220];
    std::snprintf(detail, sizeof(detail),
                  "%+.2f MB/cycle fitted over cycles 2..%d (live %.2f -> %.2f MB)",
                  perCycle / MB, cycles, live[1] / MB, live[cycles - 1] / MB);
    // 4 MB/cycle, and the number is sized from the jitter rather than from the leak. Three flat
    // 8-cycle runs on the development Mac fitted -0.71, +0.94 and +0.45 MB/cycle; the samples they
    // were fitted through oscillate in roughly 4.6 MB steps (18.4 / 23.0 / 27.9 recur exactly),
    // which is the resolution this statistic actually has. M6's leak was ~22 MB PER LOAD, so 4 MB
    // still catches it five times over. Raise this only with a measurement that says why.
    check(perCycle < 4.0 * MB, "the live heap is flat across world loads", detail);

    std::printf("[eden-leak] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// The bundled map has a HARD BORDER, and this harness's default origin sits exactly on it.
// ---------------------------------------------------------------------------------------------
// Classes/Player.mm's preupdate clamps pos.x and pos.z into
//     [4096*CHUNK_SIZE - GSIZE/2, 4096*CHUNK_SIZE + GSIZE/2]
// on every world carrying DEFAULT_LEVEL_SEED — which is every world these selftests create. GSIZE
// is T_SIZE*10 = 2880 and 4096*CHUNK_SIZE is 65536, so the playable square is [64096, 66976] on
// both axes, and this file's default `--at=1440,40,1440` (shared, verbatim, with
// web-port-final:web/tools/headless-mesh-checksum.js) is OUTSIDE it on both axes. The engine clamps it to exactly
// the south-west corner, 64096,64096 — which is where every scripted run has been standing.
//
// THAT CLAMP, NOT THE INPUT PATH AND NOT TERRAIN, IS THE "moved 0.00 blocks" FLAKE. A player on
// the corner cannot move in -X or -Z at all: `pos.x += vel.x*etime` runs, the clamp puts pos.x
// straight back, and the reported displacement is EXACTLY zero — the tell that separates this from
// a blocked player, who always drifts a fraction. Measured with a per-frame dump: velocity built
// normally to -112 blocks/s while pos.x and pos.z never left 64096.0000, with the whole 9x9
// neighbourhood reading air. Whether a run passed was decided by the spawn's random yaw: the ~half
// of the circle pointing into the map moved 40-70 blocks, the other half moved 0.00 or a fraction.
// That is the "1 in 6" both movement selftests have been flaking at, and the previous sessions'
// hill-and-retry story was fitting a curve to it.
//
// So the movement selftests start at the map's CENTRE instead — 1440 blocks of clearance in every
// direction, which no 300-frame push can reach. An explicit --at still wins: a human debugging a
// specific spot is entitled to stand on the border.
//
// The CHECKSUM modes deliberately keep the corner origin. It is a fixed, in-bounds-after-clamping
// position that every platform lands on identically, which is all criterion 2 asks of it, and
// re-recording c26fa4fd2a5b2660 / 49b71a4e16afb125 / b19d3e33a8717c60 across four platforms buys
// nothing. What was wrong was the belief about where it was, not the number. (B.5b-1a did
// re-record them, for a different reason: it changed which columns are meshed. The current values
// live in .github/scripts/check-stage1.py.)
// X.6: the clamp is skipped on "unbounded" worlds (FileManager::worldUnbounded); fresh worlds still clamp.
constexpr float kMapCentre = (float)(4096 * CHUNK_SIZE);
constexpr float kMapHalf   = (float)(GSIZE / 2);

// Teleport for a movement check: the map centre unless the caller named a spot.
void teleport_for_movement() {
    if (g_opt.haveAt) {
        eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
        return;
    }
    eden_console_teleport(kMapCentre, g_opt.at[1], kMapCentre);
}

// ...and then SAY SO if we are on the border anyway. Without this the failure mode above is
// indistinguishable from a dead translator, which is what cost two sessions; with it, a run that
// somehow ends up clamped names itself. 64 blocks is comfortably more than the ~70 a 300-frame
// full-speed fly push covers.
void check_clear_of_map_border(const char* what) {
    const PlayerState p = player_state();
    const float marginX = std::fmin(p.x - (kMapCentre - kMapHalf), (kMapCentre + kMapHalf) - p.x);
    const float marginZ = std::fmin(p.z - (kMapCentre - kMapHalf), (kMapCentre + kMapHalf) - p.z);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "at %.1f,%.1f — %.0f/%.0f blocks from the X/Z border",
                  p.x, p.z, marginX, marginZ);
    check(marginX > 64.0f && marginZ > 64.0f, what, detail);
}

// Rise until nothing can be in the way. Shared by --input-selftest and --gamepad-selftest.
// A player standing INSIDE a hill cannot walk, which is a correct engine behaviour that reads as
// "input is broken" from here — so ascend until y stops rising (the world ceiling, or a slab
// overhead) rather than for a fixed count. Uses the real key path so it still exercises the
// translator.
//
// THIS IS NOT THE FIX FOR THE "moved 0.00 blocks" FLAKE, though two sessions believed it was and
// this comment used to say so. That was the map-border clamp; see teleport_for_movement() above.
// The tell those sessions had and did not read: a player pressed against rock still drifts a
// fraction, and the flake reported EXACTLY zero. Clearing the terrain is still worth doing — it is
// why the check can use a 0.5-block threshold instead of arguing about scenery — but it is
// housekeeping, not the cure.
void rise_until_clear(int maxFrames) {
    if (!eden_get_fly_mode()) {
        push_key(SDL_SCANCODE_V, true);
        push_key(SDL_SCANCODE_V, false);
        tick(10);
    }
    // ...but NOT past the top of the world. Fly mode has no ceiling of its own, so "rise until it
    // stops" can park the player above the block array entirely (y 93 in a 64-tall world was
    // reached here), and horizontal travel is much slower up there — MEASURED, not explained:
    // 0.8 blocks in the same 300 frames that cover 44 lower down, with the same axes reaching
    // eden_set_move_input either way. Worth knowing if a future session sees "the player barely
    // moves" while flying high; it is not a state normal play reaches, so the harness just stays
    // out of it rather than this being chased here.
    const float ceiling = (float)g_opt.height - 8.0f;
    push_key(SDL_SCANCODE_SPACE, true);
    float lastY = player_state().y;
    int spent = 0;
    while (spent < maxFrames) {
        tick(60);
        spent += 60;
        const float y = player_state().y;
        if (y >= ceiling) break;           // high enough; do not leave the world
        if (y - lastY < 0.05f) break;      // stopped climbing: something solid overhead
        lastY = y;
    }
    push_key(SDL_SCANCODE_SPACE, false);
    tick(60);
}

// ---------------------------------------------------------------------------------------------
// --gamepad-selftest
// ---------------------------------------------------------------------------------------------
// Drives a REAL SDL virtual gamepad rather than calling the translator's internals, so the state
// travels the same path a physical controller's does — SDL's own joystick layer, the gamepad
// mapping database, SDL_GetGamepadAxis/Button — and then asserts the ENGINE moved. Same contract
// as --input-selftest, and the native counterpart of web-port-final:web/tools/headless-gamepad-test.js.
//
// SDL_AttachVirtualJoystick + the SDL_GAMEPAD_* masks is what makes SDL treat it as a mapped
// gamepad; without the masks it arrives as a bare joystick and SDL_IsGamepad() is false, which is
// exactly the "unknown layout" case the translator (and eden-gamepad.js before it) refuses to
// guess at.
int run_gamepad_selftest() {
    g_selftestTag = "eden-gamepad";
    g_tickInput = true;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    // THE MASKS AND THE COUNTS MUST AGREE, AND CONTIGUOUSLY. SDL builds the virtual pad's mapping
    // by walking the mask and assigning joystick indices IN ORDER, so a mask with a hole in it
    // (e.g. omitting GUIDE) shifts every later button: SDL_SetJoystickVirtualButton(…,
    // RIGHT_SHOULDER) would then set the joystick button that the mapping calls DPAD_UP, and the
    // test would report a dead shoulder button with nothing else wrong. Bits 0..DPAD_RIGHT is the
    // whole d-pad-and-face block with no gaps, so index == SDL_GamepadButton value throughout.
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.button_mask = (Uint32)((1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u);
    desc.axis_mask = (Uint32)((1u << SDL_GAMEPAD_AXIS_COUNT) - 1u);
    desc.name = "Eden virtual pad";

    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::fprintf(stderr, "[eden-gamepad] SDL_AttachVirtualJoystick failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Joystick* vj = SDL_OpenJoystick(vid);
    if (!vj) {
        std::fprintf(stderr, "[eden-gamepad] SDL_OpenJoystick failed: %s\n", SDL_GetError());
        return 1;
    }
    check(SDL_IsGamepad(vid), "SDL maps the virtual pad as a gamepad", SDL_GetGamepadNameForID(vid));

    const char* name = g_opt.world.empty() ? "gamepad-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();   // the map CENTRE, not --at's corner-clamped default
    tick(240);

    char detail[200];

    // Clear the terrain first — see rise_until_clear(). A buried or hill-blocked player who
    // cannot walk reads as "the stick is not wired", and that is what this used to report.
    rise_until_clear(1800);
    check(eden_get_fly_mode() != 0, "fly mode is on for the movement checks", "");
    check_clear_of_map_border("the player is clear of the map border");

    // --- left stick moves ---
    // Retried once, for the one reason that survives teleport_for_movement(): the direction the
    // spawn happens to face may be into a hill. (The retry was ALSO credited with covering "the
    // virtual axis reads 0 on the first batch of ticks". It never did anything of the sort — the
    // axis was always arriving, and per-frame dumps show g_padForward at a clean 1.000 through
    // every one of the 0.00-block runs. Kept for the hill; the assertion and threshold are
    // unchanged.)
    {
        auto push_stick = [&](float* outDist) {
            PlayerState before = player_state();
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -32767); // full forward (Y is +down)
            tick(300);
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
            tick(10);
            PlayerState after = player_state();
            const float dx = after.x - before.x, dz = after.z - before.z;
            *outDist = std::sqrt(dx * dx + dz * dz);
            return after;
        };

        float dist = 0.0f;
        PlayerState after = push_stick(&dist);
        if (dist <= 0.5f) {
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
            tick(120);                                  // roughly half a turn
            SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
            rise_until_clear(600);
            after = push_stick(&dist);
        }
        std::snprintf(detail, sizeof(detail), "moved %.2f blocks (-> %.1f,%.1f,%.1f yaw %.0f)",
                      dist, after.x, after.y, after.z, after.yaw);
        check(dist > 0.5f, "left stick moves the player", detail);
    }

    // --- right stick looks ---
    {
        PlayerState before = player_state();
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
        tick(60);
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        tick(10);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "yaw %.2f -> %.2f", before.yaw, after.yaw);
        check(after.yaw != before.yaw, "right stick turns the camera", detail);
    }

    // --- deadzone: a stick inside it must produce NOTHING ---
    // The one assertion that would fail silently in the other direction: a drifting stick that
    // never rests at exactly zero turns the camera on its own forever, which is the single most
    // common controller complaint there is.
    {
        PlayerState before = player_state();
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)(32767 * 0.10f));
        tick(120);
        SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHTX, 0);
        tick(10);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "0.10 tilt vs %.2f deadzone: yaw %.2f -> %.2f",
                      eden_gamepad_deadzone, before.yaw, after.yaw);
        check(after.yaw == before.yaw, "inside the deadzone nothing moves", detail);
    }

    // --- shoulder scrolls the hotbar ---
    {
        PlayerState before = player_state();
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
        tick(20);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
        tick(20);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", before.blocktype, after.blocktype);
        check(after.blocktype != before.blocktype, "shoulder scrolls the hotbar", detail);
    }

    // --- Start opens the in-game menu (the HUD-tap path, same rects the keyboard uses) ---
    {
        const int before = eden_hud_in_menu();
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
        tick(10);
        SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
        tick(30);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "in_menu %d -> %d", before, after);
        check(after != before, "Start opens the in-game menu", detail);
    }

    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    std::printf("[eden-gamepad] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --keybind-selftest  (Phase N Stage 5.2/5.3)
// ---------------------------------------------------------------------------------------------
// Stage 5.2 moved the keybind map out of `public/eden-keybinds.js` and into the shared C model
// (port/src/seam/Settings_web.mm). The thing that has to be true afterwards is not "the setter
// stored a number" — it is that the INPUT PATH reads the model, i.e. that rebinding moves the
// behaviour off the old key and onto the new one. So this asserts BOTH directions: the new key
// works AND the old one has stopped. A test that only checked the new key would pass against an
// Input_native.cpp that still had its hard-coded `switch (sc)` and merely also consulted the
// model, which is exactly the half-ported state this stage could have landed in.
//
// It also asserts the double-binding case, because "one key, two actions" is shipped
// configuration (Ctrl is flyDown and crouch) and the iterator that honours it
// (eden_keybind_action_after) is new code with exactly one caller.
extern "C" {
int  eden_keybind_count(void);
int  eden_keybind_index(const char* action);
int  eden_keybind_get(int i);
int  eden_keybind_default(int i);
void eden_keybind_set(int i, int code);
void eden_keybind_reset_all(void);
int  eden_keybind_action_after(int prev, int code);
int  eden_keybind_capture_active(void);
void eden_keybind_capture_begin(int i);
int  eden_keybind_capture_feed(int code);
const char* eden_keybind_code_name(int code);
}

// ---------------------------------------------------------------------------------------------
// --ui-selftest  (Stage 5.4 widgets + N.4.5 text entry and world rename)
// ---------------------------------------------------------------------------------------------
// Drives the REAL screens with the inputs a player would produce — pointer events at the rects
// the screen itself reports (SettingsMenu::showRow), SDL text/key events pushed through the pump
// — and asserts on the model and the disk, never on the widgets' own state. Headless: none of
// this needs a pixel, and --shot is the artefact for what it looks like.
extern "C" {
float eden_settings_get(int i);
int   eden_settings_kind(int i);
int   eden_settings_enum_count(int i);
void  eden_input_pointer_event(int phase, int identity, float x, float y);
}
int settings_index(const char* key) {
    for (int i = 0; i < eden_settings_count(); ++i)
        if (std::strcmp(eden_settings_key(i), key) == 0) return i;
    return -1;
}

// Engine touches are y-UP point space; eden_input_pointer_event takes y-DOWN (window order).
const int kUiPointer = 4242;
void ui_pointer(int phase, float x, float yUp) {
    eden_input_pointer_event(phase, kUiPointer, x, SCREEN_HEIGHT - yUp);
}
void ui_tap(float x, float yUp) {
    ui_pointer(0, x, yUp); tick(2);
    ui_pointer(2, x, yUp); tick(2);
}

void push_text(const char* utf8) {          // must be a literal: SDL keeps the pointer
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = utf8;
    SDL_PushEvent(&e);
}
void push_tap_key(SDL_Scancode sc) { push_key(sc, true); push_key(sc, false); }

int  g_promptChosen = -2;
std::string g_promptText;
void ui_prompt_cb(int chosen, const char* text) { g_promptChosen = chosen; g_promptText = text ? text : ""; }

bool read_file(const std::string& path, std::vector<unsigned char>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

void ui_tap_rect(CGRect r) { ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f); }

// I.7: put the main menu on the home screen or on the world list ("My Worlds") with the tap a
// player would make (the home's My Worlds tile, the list's Back). Boot lands on home; Quit to Menu
// and a Get Worlds download land on the list.
void ui_menu_to(bool home) {
    Menu* m = World::getWorld ? World::getWorld->menu : NULL;
    if (!m || (m->showhome != FALSE) == home) return;
    ui_tap_rect(m->controlRect(home ? "back" : "home.myworlds"));
    tick(3);
}

void push_wheel(float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = y;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&e);
}

// MP.6: the Multiplayer screens (UI plan §9's tap paths), from the home screen and back to it.
// A real servers.txt in this run's --docs first (add, save, probe, join -> profile -> error, delete),
// then the --mp-mock fixture (one row per status, the secure prompt, the keyboard rule).
std::string mp_read_servers_txt() {
    const std::string p = std::string(eden_platform_documents_root()) + "/servers.txt";
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    char b[1024];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

int mp_ui_checks(Menu* m) {
    char detail[300];
    ServerListMenu* sl = m->servers;
    ServerFormMenu* fm = sl->form();
    std::remove((std::string(eden_platform_documents_root()) + "/servers.txt").c_str());
    eden_mp_profile_set("", 0);
    ui_tap_rect(m->controlRect("home.multiplayer"));
    tick(3);
    check(m->showservers && eden_mp_servers_count() == 0, "a fresh list is empty", "");
    check(sl->controlRect("emptyadd").size.width > 0, "the empty list offers + Add server", "");

    // Add: typed, validated live, saved.
    ui_tap_rect(sl->controlRect("add"));
    tick(3);
    check(fm->isOpen() && fm->mode() == ServerFormMenu::MODE_ADD, "Add opens the server form", "");
    check(fm->field("address")->focused(), "Add focuses the Address field under a pointer", "");
    push_text("bad host");
    tick(3);
    std::snprintf(detail, sizeof detail, "hint '%s' save %d", fm->hint("address"), fm->saveEnabled());
    check(!fm->saveEnabled() && !std::strcmp(fm->hint("address"), "No spaces in an address"),
          "an address with a space disables Save and says why", detail);
    fm->field("address")->setText("");
    push_text("127.0.0.1:1");
    tick(3);
    check(fm->saveEnabled(), "a valid host:port enables Save", fm->hint("address"));
    ui_tap_rect(fm->controlRect("save"));
    tick(3);
    const std::string txt = mp_read_servers_txt();
    check(!fm->isOpen() && eden_mp_servers_count() == 1 && txt.find("\t127.0.0.1\t1\t") != std::string::npos &&
          txt.compare(0, 15, "emod-servers 1\n") == 0, "Save writes servers.txt and lists the row", txt.c_str());
    tick_until([sl] { EdenMpServerRow r; return eden_mp_servers_get(0, &r) && r.status == EDEN_MP_ST_NOT_RUNNING; }, 400,
               "the probe of a closed port");
    EdenMpServerRow r0;
    eden_mp_servers_get(0, &r0);
    std::snprintf(detail, sizeof detail, "status %d", r0.status);
    check(r0.status == EDEN_MP_ST_NOT_RUNNING, "the new row is probed (a closed port reads Not running)", detail);

    // Join with no name: the Profile form first; Save continues the join, which fails into the dialog.
    CGRect rr;
    sl->rowRect(0, &rr);
    ui_tap_rect(rr);   // the selected row: a tap joins
    tick(3);
    check(fm->isOpen() && fm->mode() == ServerFormMenu::MODE_PROFILE, "the first Join with no name opens Profile", "");
    fm->field("name")->setText("");
    push_text("a:b");
    tick(3);
    check(!fm->saveEnabled() && !std::strcmp(fm->hint("name"), "No : [ or ]"), "Profile refuses a name with ':'", fm->hint("name"));
    fm->field("name")->setText("");
    push_text("Sam");
    tick(3);
    ui_tap_rect(fm->controlRect("character.inc"));
    tick(2);
    ui_tap_rect(fm->controlRect("save"));
    tick(3);
    char nm[32];
    eden_mp_profile_name(nm, sizeof nm);
    check(!std::strcmp(nm, "Sam") && eden_mp_profile_character() == 1, "Profile saves the name and character", nm);
    check(sl->connecting() || GLDialog::active(), "Profile's Save continues the join", "");
    tick_until([] { return GLDialog::active(); }, 600, "the join's error dialog");
    CGRect b2;
    check(GLDialog::active() && GLDialog::buttonRect(2, &b2), "a refused join shows Retry / Edit server / Back", "");
    if (GLDialog::active()) { GLDialog::buttonRect(2, &b2); ui_tap_rect(b2); tick(3); }
    check(!GLDialog::active() && !sl->connecting() && m->showservers, "Back leaves the dialog on the list", "");

    // Connecting -> Cancel (a server that accepts and never answers).
    {
        eden_net::Sock ls;
        int port = 0;
        eden_net::tcpListen(0, ls, &port);
        char host[64];
        std::snprintf(host, sizeof host, "127.0.0.1");
        const int i = eden_mp_servers_add("Silent", host, port, "");
        eden_mp_servers_save();
        sl->select(i);
        tick(2);
        ui_tap_rect(sl->controlRect("join"));
        tick(5);
        std::snprintf(detail, sizeof detail, "sel %d sel-count %d dialog %d form %d tab %d", sl->selected(), eden_mp_servers_count(), GLDialog::active(), fm->isOpen(), sl->tab());
        check(sl->connecting(), "Join opens the Connecting window", detail);
        ui_tap_rect(sl->controlRect("cancel"));
        tick(3);
        EdenMpJoinState js;
        eden_mp_join_state(&js);
        check(!sl->connecting() && js.stage == EDEN_MP_JOIN_IDLE && !MpWorld::link(), "Cancel closes the join", "");
        eden_net::closeSock(ls);
        eden_mp_servers_remove(i);
        eden_mp_servers_save();
    }

    // Edit -> Delete -> Remove.
    sl->select(0);
    ui_tap_rect(sl->controlRect("edit"));
    tick(3);
    check(fm->isOpen() && fm->mode() == ServerFormMenu::MODE_EDIT, "Edit opens the form on the row", "");
    ui_tap_rect(fm->controlRect("delete"));
    tick(3);
    CGRect b0;
    if (GLDialog::buttonRect(0, &b0)) { ui_tap_rect(b0); tick(3); }
    check(!fm->isOpen() && eden_mp_servers_count() == 0 && mp_read_servers_txt() == "emod-servers 1\n",
          "Delete -> Remove empties the list and the file", "");
    push_tap_key(SDL_SCANCODE_ESCAPE);
    tick(3);
    check(!m->showservers && m->showhome, "Escape on the server list returns home", "");

    // The fixture: one row per status (list plan §4-§5), the secure prompt, the keyboard rule.
    eden_mp_servers_mock(1);
    ui_tap_rect(m->controlRect("home.multiplayer"));
    tick(3);
    tick_until([] { return eden_mp_servers_poll() == 0; }, 300, "the mock probes");
    tick(2);
    static const int kWant[7] = {EDEN_MP_ST_ONLINE, EDEN_MP_ST_ONLINE, EDEN_MP_ST_BASIC, EDEN_MP_ST_FULL,
                                 EDEN_MP_ST_NOT_RUNNING, EDEN_MP_ST_NO_ANSWER, EDEN_MP_ST_UNKNOWN_HOST};
    int okRows = 0;
    for (int i = 0; i < 7; ++i) { EdenMpServerRow r; okRows += eden_mp_servers_get(i, &r) && r.status == kWant[i]; }
    check(eden_mp_servers_count() == 7 && okRows == 7, "the mock list shows every status", "");
    EdenMpServerRow c0;
    eden_mp_servers_get(0, &c0);
    check(!std::strcmp(c0.motd, "Welcome! No griefing. /help for commands") && c0.players == 12 && c0.locked == 1,
          "an INFO reply fills the row", c0.motd);
    // MP.8: the Matchmakers tab (the fixture has two addresses and four canned listings).
    {
        char clean[200], err[200];
        check(eden_mp_matchmakers_validate("a.example, b.example:99 ,", clean, sizeof clean, err, sizeof err) &&
              !std::strcmp(clean, "a.example:27020, b.example:99"), "the Matchmakers setting is cleaned (default port 27020)", clean);
        check(!eden_mp_matchmakers_validate("good.example, bad host", clean, sizeof clean, err, sizeof err) &&
              std::strstr(err, "bad host"), "an invalid Matchmakers entry rejects the save and names it", err);
        check(!eden_mp_matchmakers_validate("1,2,3,4,5,6,7,8,9", clean, sizeof clean, err, sizeof err), "more than 8 matchmakers is refused", err);
        check(sl->tabsShown() && sl->tab() == 0, "matchmakers set: the list shows the tabs, on My servers", "");
        ui_tap_rect(sl->controlRect("tab1"));
        tick(3);
        std::snprintf(detail, sizeof detail, "tab %d rows %d", sl->tab(), eden_mp_matchmaker_rows());
        check(sl->tab() == 1 && eden_mp_matchmaker_rows() == 4, "the Matchmakers tab lists the canned LIST rows", detail);
        EdenMpServerRow mr;
        eden_mp_matchmaker_get(0, &mr);
        check(!std::strcmp(mr.label, "Sky Kingdom") && mr.players == 9, "listings sort by players, then name", mr.label);
        CGRect sv = sl->controlRect("save"), ed = sl->controlRect("edit");
        check(sv.size.width > 0 && sv.origin.x == ed.origin.x, "Save takes Edit's place on the Matchmakers tab", "");
        sl->select(1);
        tick(2);
        ui_tap_rect(sl->controlRect("save"));
        tick(3);
        EdenMpServerRow last;
        const int n = eden_mp_servers_count();
        eden_mp_servers_get(n - 1, &last);
        std::snprintf(detail, sizeof detail, "tab %d count %d last '%s'", sl->tab(), n, last.label);
        check(sl->tab() == 0 && n == 8 && !std::strcmp(last.label, "Moonlit Builders") && sl->selected() == n - 1,
              "Save copies a listing into My servers and selects it", detail);
        eden_mp_servers_remove(n - 1);   // back to the fixture's seven for the checks below
        sl->select(0);
        tick(2);
    }
    sl->select(6);
    tick(2);
    ui_tap_rect(sl->controlRect("join"));
    tick(3);
    check(!sl->connecting() && !GLDialog::active(), "Join is off for an Unknown host row", "");
    sl->select(1);
    sl->debugPassword(false);
    tick(2);
    GLDialog::getDialog();
    push_text("secret");
    tick(3);
    const GLW::TextField* pf = GLDialog::promptField();
    std::snprintf(detail, sizeof detail, "text '%s' shown '%s'", pf ? pf->text().c_str() : "-", pf ? pf->shownText().c_str() : "-");
    check(pf && pf->secure() && pf->text() == "secret" && pf->shownText().find("secret") == std::string::npos,
          "the password prompt's raster never holds the password", detail);
    GLDialog::dismiss();
    tick(2);
    // The keyboard never covers the focused field (the standing rule): the form with a faked keyboard.
    ui_tap_rect(sl->controlRect("add"));
    tick(3);
    const float kb = SCREEN_HEIGHT * 0.55f;
    eden_native_debug_set_keyboard_top(kb);
    fm->field("password")->focus();
    tick(3);
    const CGRect pr = fm->controlRect("password");
    std::snprintf(detail, sizeof detail, "field y %.0f..%.0f keyboard top %.0f foot %d", pr.origin.y, pr.origin.y + pr.size.height, kb,
                  fm->footShown());
    check(pr.origin.y >= kb && !fm->footShown() && fm->controlRect("save").origin.y > kb,
          "a keyboard over the form: the focused field and Save stay above it", detail);
    eden_native_debug_set_keyboard_top(-2.0f);
    fm->blurFields();
    ui_tap_rect(fm->controlRect("back"));
    tick(3);
    push_tap_key(SDL_SCANCODE_ESCAPE);
    tick(3);
    eden_mp_servers_mock(0);
    eden_mp_profile_set("", 0);
    check(!m->showservers && m->showhome, "back home after the multiplayer checks", "");
    return 0;
}

// Stage 5.6: the kit main menu, driven only through taps at the rects the screen reports (and one
// wheel event), asserting on the menu's model — the list, the selection, `loading`, the disk.
// Works on whatever list this machine already has: it adds its own worlds and only deletes those.
int run_menu_selftest() {
    char detail[240];
    Menu* m = World::getWorld->menu;
    tick(5);
    const int n0 = eden_menu_world_count();

    // I.7: the home screen. Boot lands on it; Multiplayer is drawn but disabled until MP.6; My Worlds
    // opens the list, its Back and Escape both return home; Escape on home stays there.
    {
        const bool booted = m->showhome != FALSE;
        if (!booted) { m->showHome(true); tick(3); }
        check(booted, "boot lands on the home screen", "");
        // MP.6: the tile opens the server list now (its own block below); Back returns home.
        ui_tap_rect(m->controlRect("home.multiplayer"));
        tick(3);
        check(m->showservers && !m->showbrowser && !m->showsettings, "the Multiplayer tile opens the server list", "");
        ui_tap_rect(m->servers->controlRect("back"));
        tick(3);
        check(!m->showservers && m->showhome, "the server list's Back returns home", "");
        if (mp_ui_checks(m) != 0) return 1;
        const CGRect gw = m->controlRect("getworlds"), hg = m->controlRect("home.getworlds");
        auto same = [](CGRect a, CGRect b) {
            return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width &&
                   a.size.height == b.size.height && a.size.width > 0;
        };
        check(same(gw, hg) && same(m->controlRect("settings"), m->controlRect("home.settings")),
              "the old \"getworlds\"/\"settings\" keys resolve to the home's controls", "");
        ui_tap_rect(m->controlRect("home.settings"));
        tick(3);
        check(m->showsettings && m->showhome, "home -> Settings opens settings", "");
        eden_settings_menu_close();
        tick(3);
        check(!m->showsettings && m->showhome, "Settings closes back to the home screen", "");
        ui_tap_rect(m->controlRect("home.myworlds"));
        tick(3);
        check(!m->showhome, "the My Worlds tile opens the world list", "");
        ui_tap_rect(m->controlRect("back"));
        tick(3);
        check(m->showhome, "the list's Back returns home", "");
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        check(m->showhome && !m->showsettings, "Escape on the home screen does nothing", "");
        ui_menu_to(false);
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        check(m->showhome, "Escape on the list returns home", "");
        ui_menu_to(false);
        check(!m->showhome, "back on the list for the checks below", "");
    }

    // U.5: New opens the New World screen and adds nothing; Back closes it, still nothing.
    ui_tap_rect(m->controlRect("new"));
    tick(3);
    NewWorldMenu* nw = m->newworld;
    std::snprintf(detail, sizeof(detail), "shown %d open %d count %d -> %d", m->shownewworld ? 1 : 0,
                  nw->isOpen() ? 1 : 0, n0, eden_menu_world_count());
    check(m->shownewworld && nw->isOpen() && eden_menu_world_count() == n0, "New opens the New World screen", detail);
    check(nw->focused("name") && eden_text_input_active() == 1, "the name field has focus on a desktop", "");
    ui_tap_rect(nw->controlRect("back"));
    tick(3);
    std::snprintf(detail, sizeof(detail), "shown %d count %d (was %d) text input %d", m->shownewworld ? 1 : 0,
                  eden_menu_world_count(), n0, eden_text_input_active());
    check(!m->shownewworld && eden_menu_world_count() == n0 && eden_text_input_active() == 0,
          "Back closes it and creates nothing", detail);

    // A world created through the seam: one more, selected, scrolled into view.
    eden_menu_create_world();
    tick(3);
    int sel = eden_menu_selected_index();
    CGRect rr;
    std::snprintf(detail, sizeof(detail), "count %d -> %d, selected %d, first %d of %d visible",
                  n0, eden_menu_world_count(), sel, m->firstVisibleRow(), m->visibleRows());
    check(eden_menu_world_count() == n0 + 1 && sel == n0 && m->rowRect(sel, &rr),
          "a new world is selected and scrolled into view", detail);
    // Enough worlds that the list must scroll.
    for (int guard = 0; eden_menu_world_count() < m->visibleRows() + 3 && guard < 40; ++guard) {
        eden_menu_create_world();
        tick(3);
    }
    const int total = eden_menu_world_count();
    const int vis = m->visibleRows();
    check(total > vis, "the list is longer than the window (it has to scroll)", "");

    // A tap on a row that is not selected selects it — and does not play it.
    sel = eden_menu_selected_index();
    int pick = m->firstVisibleRow();
    if (pick == sel) pick++;
    if (m->rowRect(pick, &rr)) ui_tap_rect(rr);
    tick(3);
    std::snprintf(detail, sizeof(detail), "tapped %d: selected %d loading %d", pick, eden_menu_selected_index(), m->loading);
    check(eden_menu_selected_index() == pick && m->loading == 0, "tapping a row selects it without playing", detail);

    // Drag the list down two rows: earlier rows come into view, and the drag selects nothing.
    {
        sel = eden_menu_selected_index();
        m->rowRect(m->firstVisibleRow(), &rr);
        const float pitch = rr.size.height;
        eden_menu_select(total - 1);                    // scrolls to the end, so there is room above
        tick(2);
        sel = total - 1;
        const int f0 = m->firstVisibleRow();
        const CGRect L = m->controlRect("list");
        const float x = L.origin.x + L.size.width * 0.5f, y0 = L.origin.y + L.size.height * 0.5f;
        ui_pointer(0, x, y0); tick(2);
        ui_pointer(1, x, y0 - pitch); tick(2);
        ui_pointer(1, x, y0 - 2.0f * pitch); tick(2);
        ui_pointer(2, x, y0 - 2.0f * pitch); tick(2);
        const int f1 = m->firstVisibleRow();
        std::snprintf(detail, sizeof(detail), "first %d -> %d, selected %d (was %d), loading %d",
                      f0, f1, eden_menu_selected_index(), sel, m->loading);
        check(f1 == f0 - 2 && eden_menu_selected_index() == sel && m->loading == 0,
              "dragging the list scrolls by whole rows and selects nothing", detail);

        // The wheel: one notch toward the user is one row later.
        push_wheel(-1.0f);
        tick(3);
        std::snprintf(detail, sizeof(detail), "first %d -> %d", f1, m->firstVisibleRow());
        check(m->firstVisibleRow() == f1 + 1, "a wheel notch scrolls one row", detail);

        // The scrollbar: a tap on the track above the thumb pages up.
        const int f2 = m->firstVisibleRow();
        const CGRect B = m->controlRect("scrollbar");
        ui_tap(B.origin.x + B.size.width * 0.5f, B.origin.y + B.size.height - 2.0f);
        const int want = std::max(0, f2 - vis);
        std::snprintf(detail, sizeof(detail), "first %d -> %d (want %d)", f2, m->firstVisibleRow(), want);
        check(m->firstVisibleRow() == want, "a tap on the scrollbar track pages", detail);
    }

    // Delete: Cancel keeps the world, Delete removes it (the dialog was a no-op before 5.6).
    {
        eden_menu_select(total - 1);
        tick(2);
        const std::string victim = eden_menu_world_name(total - 1);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        CGRect b;
        const bool up = GLDialog::active() && GLDialog::buttonRect(1, &b);
        check(up, "Delete asks first", "");
        if (up) ui_tap_rect(b);
        tick(3);
        std::snprintf(detail, sizeof(detail), "count %d (was %d), dialog %d", eden_menu_world_count(), total,
                      GLDialog::active() ? 1 : 0);
        check(eden_menu_world_count() == total && !GLDialog::active(), "Cancel keeps the world", detail);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick(3);
        bool listed = false;
        for (int i = 0; i < eden_menu_world_count(); ++i) listed |= (victim == eden_menu_world_name(i));
        std::snprintf(detail, sizeof(detail), "count %d (was %d), \"%s\" listed %d", eden_menu_world_count(), total,
                      victim.c_str(), listed ? 1 : 0);
        check(eden_menu_world_count() == total - 1 && !listed, "Delete -> Delete removes the world", detail);
    }

    // Play: the button, then the stock world-type + height dialogs (Flat, then the height's first
    // button -- New Dawn 256, the default since S.5e), to a world.
    {
        const int idx = eden_menu_world_count() - 1;
        eden_menu_select(idx);
        tick(2);
        const std::string name = eden_menu_world_name(idx);
        ui_tap_rect(m->controlRect("play"));
        CGRect b;
        const bool asked = tick_until([] { return GLDialog::active(); }, 600, "world-type dialog");
        if (asked && GLDialog::buttonRect(0, &b)) ui_tap_rect(b);            // Flat
        tick(3);
        if (GLDialog::active() && GLDialog::buttonRect(0, &b)) ui_tap_rect(b); // New Dawn 256 (S.5e)
        const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
        check(asked && played, "Play -> Flat -> the default height loads the selected world", name.c_str());
        if (!played) return 1;
        std::snprintf(detail, sizeof(detail), "g_world_height %d", g_world_height);
        check(g_world_height == 256, "the height dialog's default (button 0) is New Dawn 256 (S.5e)", detail);
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);

        // Now it has a file: Delete must remove that too.
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (name == eden_menu_world_name(i)) at = i;
        check(at >= 0, "the played world is listed after quitting", name.c_str());
        if (at < 0) return 1;
        const std::string file = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(at);
        std::vector<unsigned char> bytes;
        const bool had = read_file(file, &bytes);
        eden_menu_select(at);
        tick(2);
        ui_tap_rect(m->controlRect("delete"));
        tick(3);
        if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
        tick(3);
        const bool gone = !read_file(file, &bytes);
        std::snprintf(detail, sizeof(detail), "%s: before %d after %d", file.c_str(), had ? 1 : 0, gone ? 0 : 1);
        check(had && gone, "deleting a played world removes its file", detail);
    }

    // U.5 (and W.2 (d)): the New World screen end to end, through its real controls — a typed name
    // (a two-byte character included), the Flat tile, Classic 64, Play -> a flat 64z world, no
    // dialog, whose file header carries the typed name. Then Escape: it closes the screen.
    {
        NewWorldMenu* nw = m->newworld;
        const int before = eden_menu_world_count();
        ui_tap_rect(m->controlRect("new"));
        tick(3);
        const char* typed = "Selftest V\xC3\xADlag";   // "Selftest Vílag"
        push_text(typed);
        tick(2);
        int flatTile = -1;
        for (int t = 0; t < nw->typeCount(); ++t) if (nw->typeFlat(t)) flatTile = t;
        ui_tap_rect(nw->typeRect(flatTile));
        tick(2);
        CGRect b;
        if (nw->choiceRect("height", 1, &b)) ui_tap_rect(b);     // Original 64
        tick(2);
        std::snprintf(detail, sizeof(detail), "type %d (flat tile %d) height %d name \"%s\" focused %d",
                      nw->type(), flatTile, nw->choice("height"), nw->text("name"), nw->focused("name") ? 1 : 0);
        check(nw->type() == flatTile && nw->choice("height") == 64 && std::strcmp(nw->text("name"), typed) == 0 &&
              !nw->focused("name"), "the rail picks Flat, the pair picks Original 64, the name is typed (and a tap elsewhere blurs it)", detail);
        ui_tap_rect(nw->controlRect("play"));
        tick(2);
        bool dialog = false;
        const bool played = tick_until([&dialog] { dialog |= GLDialog::active(); return game_mode() == 1; }, 60000,
                                       "game_mode == PLAY");
        std::snprintf(detail, sizeof(detail), "played %d dialog %d count %d (was %d) height %d genflat %d",
                      played ? 1 : 0, dialog ? 1 : 0, eden_menu_world_count(), before, g_world_height,
                      World::getWorld->fm->genflat ? 1 : 0);
        check(played && !dialog && eden_menu_world_count() == before + 1 && g_world_height == 64 &&
              World::getWorld->fm->genflat, "Play makes and loads a flat Original 64 world without asking again", detail);
        if (!played) return 1;
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (std::strcmp(eden_menu_world_name(i), typed) == 0) at = i;
        std::string onDisk;
        if (at >= 0) {
            NSString* fn = [NSString stringWithUTF8String:eden_menu_world_file(at)];
            onDisk = cpstring(World::getWorld->fm->getName(fn));
        }
        std::snprintf(detail, sizeof(detail), "listed at %d, header name \"%s\"", at, onDisk.c_str());
        check(at >= 0 && onDisk == typed, "the world's file header carries the typed name", detail);

        // Escape over the screen is its Back. The field has focus (desktop), so the first Escape
        // only leaves the field; the second closes the screen.
        const int count = eden_menu_world_count();
        ui_tap_rect(m->controlRect("new"));
        tick(3);
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        const bool fieldLeft = !nw->focused("name") && m->shownewworld;
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::snprintf(detail, sizeof(detail), "after one: field left %d; after two: shown %d count %d (was %d)",
                      fieldLeft ? 1 : 0, m->shownewworld ? 1 : 0, eden_menu_world_count(), count);
        check(fieldLeft && !m->shownewworld && eden_menu_world_count() == count,
              "Escape leaves the name field, then closes the screen", detail);

        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("delete"));
            tick(3);
            if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
            tick(3);
        }
    }

    // H.4: the Classic tab through its real controls. Scale is hidden at Original 64 and back at
    // New Dawn 256; a worded seed becomes seedFromText's number; the slider, a choice and a toggle
    // set their fields; Play loads a Classic world whose generator is exactly those settings, and
    // its first save writes the GENERATOR record.
    {
        NewWorldMenu* nw = m->newworld;
        ui_tap_rect(m->controlRect("new"));
        tick(3);
        push_text("Selftest Classic");
        tick(2);
        int classicTile = -1;
        for (int t = 0; t < nw->typeCount(); ++t) if (!std::strcmp(nw->typeShot(t), "classic")) classicTile = t;
        ui_tap_rect(nw->typeRect(classicTile));
        tick(2);
        CGRect b;
        if (nw->choiceRect("height", 1, &b)) ui_tap_rect(b);     // Original 64
        tick(2);
        const bool hidden64 = nw->choice("height") == 64 && !nw->rowVisible("scale") && nw->rowVisible("seed");
        if (nw->choiceRect("height", 0, &b)) ui_tap_rect(b);     // New Dawn 256
        tick(2);
        const bool shown256 = nw->choice("height") == 256 && nw->rowVisible("scale");
        std::snprintf(detail, sizeof(detail), "type %d (classic tile %d) scale hidden at 64 %d, shown at 256 %d",
                      nw->type(), classicTile, hidden64 ? 1 : 0, shown256 ? 1 : 0);
        check(nw->type() == classicTile && hidden64 && shown256, "the Classic tab hides Scale at 64 and shows it at 256", detail);

        const int rolled = nw->seed();
        ui_tap_rect(nw->controlRect("seed"));
        tick(2);
        push_text("hello");
        tick(2);
        const int want = NewWorldMenu::seedFromText("hello");
        std::snprintf(detail, sizeof(detail), "rolled %d, typed \"%s\" -> %d (want %d); \"12345\" -> %d, \"333333\" -> %d",
                      rolled, nw->text("seed"), nw->seed(), want, NewWorldMenu::seedFromText("12345"),
                      NewWorldMenu::seedFromText("333333"));
        check(rolled >= 1 && rolled < (1 << 24) && rolled != 333333 && want >= 1 && want < (1 << 24) &&
              nw->seed() == want && NewWorldMenu::seedFromText("12345") == 12345 &&
              NewWorldMenu::seedFromText("333333") != 333333,
              "a blank seed is the rolled one; digits are the number, words are hashed into [1, 2^24)", detail);

        CGRect sr;
        if (nw->sliderRect("scale", &sr)) ui_tap_rect(CGRectMake(sr.origin.x + sr.size.width - 1, sr.origin.y, 1, sr.size.height));
        tick(2);
        if (nw->choiceRect("trees", 3, &b)) ui_tap_rect(b);      // Dense
        tick(2);
        if (nw->choiceRect("caves", 1, &b)) ui_tap_rect(b);      // 2011
        tick(2);
        if (nw->toggleRect("flowers", &b)) ui_tap_rect(b);       // off
        tick(2);
        ClassicGen want_g;
        nw->classicSettings(&want_g);
        std::snprintf(detail, sizeof(detail), "scale4 %d trees %d caves %d flowers %d clouds %d",
                      want_g.scale4, want_g.trees, want_g.caves, want_g.flowers, want_g.clouds);
        check(want_g.scale4 == 16 && want_g.trees == 3 && want_g.caves == CLASSIC_CAVES_2011 && !want_g.flowers &&
              want_g.clouds && !nw->toggle("flowers"), "the slider, a choice and a toggle set the Classic settings", detail);

        ui_tap_rect(nw->controlRect("play"));
        tick(2);
        bool dialog = false;
        const bool played = tick_until([&dialog] { dialog |= GLDialog::active(); return game_mode() == 1; }, 60000,
                                       "game_mode == PLAY");
        TerrainGenerator* tg = World::getWorld->terrain->tgen;
        const ClassicGen& g = tg->classic;
        std::snprintf(detail, sizeof(detail), "played %d dialog %d height %d genflat %d state %d seed %d/%d scale4 %d trees %d caves %d flowers %d",
                      played ? 1 : 0, dialog ? 1 : 0, g_world_height, World::getWorld->fm->genflat ? 1 : 0,
                      tg->classicState, tg->LEVEL_SEED, g.seed, g.scale4, g.trees, g.caves, g.flowers);
        check(played && !dialog && g_world_height == 256 && !World::getWorld->fm->genflat &&
              tg->classicState == TerrainGenerator::CLASSIC_STATE_SET && tg->LEVEL_SEED == want && g.seed == want &&
              g.profile == CLASSIC_PROFILE_NEW && g.scale4 == 16 && g.trees == 3 && g.caves == CLASSIC_CAVES_2011 &&
              !g.flowers && g.clouds, "Play loads a Classic world with exactly the tab's settings", detail);
        if (!played) return 1;
        tick(60);
        if (!quit_to_menu()) return 1;
        tick(30);
        int at = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (std::strcmp(eden_menu_world_name(i), "Selftest Classic") == 0) at = i;
        const std::string file = at >= 0 ? std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(at) : std::string();
        const int n9 = at >= 0 ? classic_test::count_records(file, emod::T_GENERATOR) : -1;
        std::snprintf(detail, sizeof(detail), "listed at %d, %d GENERATOR record(s)", at, n9);
        check(at >= 0 && n9 == 1, "the Classic world's save carries its GENERATOR record", detail);
        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("delete"));
            tick(3);
            if (GLDialog::buttonRect(0, &b)) ui_tap_rect(b);
            tick(3);
        }
    }
    return 0;
}

// --convert-prompt-selftest (Stage S / S.5e): the "Convert world" prompt, through its real buttons.
// --docs holds a 64z `.eden` whose display name is --world=NAME; --world-format=emod;
// --convert-answer=upgrade|keep|cancel picks the button. The toggle and the prompt are forced on
// (EDEN_UPGRADE_256Z=1, EDEN_CONVERT_PROMPT unset), so this is the player's path, not a harness's.
// One answer per process: a converted source is paired and never asks again.
int run_convert_prompt_selftest() {
    g_selftestTag = "eden-convert";
    char d[320];
    const std::string ans = g_opt.convertAnswer;
    const int btn = ans == "upgrade" ? 0 : ans == "keep" ? 1 : ans == "cancel" ? 2 : -1;
    if (btn < 0) { check(false, "--convert-answer=upgrade|keep|cancel is given", ans.c_str()); return 1; }
    tick(5);
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (g_opt.world == eden_menu_world_name(i)) idx = i;
    std::snprintf(d, sizeof(d), "\"%s\"", g_opt.world.c_str());
    check(idx >= 0, "the source world is listed", d);
    if (idx < 0) return 1;
    const std::string docs = eden_platform_documents_root();
    const std::string src = eden_menu_world_file(idx);
    auto emodCount = [&docs]() {
        int n = 0;
        for (int i = 0; i < eden_menu_world_count(); ++i) {
            const std::string f = eden_menu_world_file(i);
            n += f.size() > 5 && f.compare(f.size() - 5, 5, ".emod") == 0;
        }
        (void)docs;
        return n;
    };
    const int emod0 = emodCount();
    auto play = [&]() {
        eden_menu_select(idx);
        tick(2);
        return eden_menu_play() == 1 && tick_until([] { return GLDialog::active(); }, 600, "the convert prompt");
    };
    // The prompt, not "World converted" (which a conversion that never asked would show next): no
    // `.emod` beside the source yet, and no original waiting on that question.
    auto converted = [&]() {
        const std::string stem = src.substr(0, src.find_last_of('.'));
        FILE* f = std::fopen((docs + "/" + stem + ".emod").c_str(), "rb");
        if (f) std::fclose(f);
        return f != nullptr || World::getWorld->fm->pendingOriginal() != nil;
    };
    const bool asked = play() && !converted();
    std::snprintf(d, sizeof(d), "%s, game_mode %d, converted %d", src.c_str(), game_mode(), converted() ? 1 : 0);
    check(asked && game_mode() != 1, "Play on a 64z source asks before converting", d);
    if (!asked) return 1;
    CGRect b;
    if (GLDialog::buttonRect(btn, &b)) ui_tap_rect(b);
    if (btn == 2) {
        const bool back = tick_until([] { return World::getWorld->menu->loading == 0; }, 600, "loading back to 0");
        tick(30);
        World::getWorld->menu->loadWorlds();
        tick(2);
        std::snprintf(d, sizeof(d), "loading %d, game_mode %d, .emod %d -> %d", World::getWorld->menu->loading, game_mode(),
                      emod0, emodCount());
        check(back && game_mode() != 1 && !GLDialog::active() && emodCount() == emod0, "Cancel backs out to the menu, nothing converted", d);
        idx = -1;
        for (int i = 0; i < eden_menu_world_count(); ++i) if (src == eden_menu_world_file(i)) idx = i;
        const bool again = idx >= 0 && play() && !converted();
        check(again, "the next Play asks again", "");
        if (again && GLDialog::buttonRect(2, &b)) ui_tap_rect(b);
        tick_until([] { return World::getWorld->menu->loading == 0; }, 600, "loading back to 0");
        return 0;
    }
    const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
    const std::string file = eden_debug_world_file();
    emod::EdenWorldStore st;
    const bool opened = st.open(file.c_str(), false);
    const int want = btn == 0 ? 256 : 64;
    std::snprintf(d, sizeof(d), "%s: g_world_height %d, bands %d", file.c_str(), g_world_height, opened ? st.bands() : -1);
    check(played && opened && g_world_height == want && st.bands() == want / 16,
          btn == 0 ? "Upgrade to 256 converts to a 16-band .emod and plays 256z" : "Keep 64 converts to a 4-band .emod and plays 64z", d);
    st.close();
    if (GLDialog::active()) { GLDialog::dismiss(); tick(2); }   // "World converted": keep the original
    return 0;
}

// Stage 5.7: the pickers kept stock's hit path; prove a tap on a cell still picks, through the
// HUD's own rects, and that the current choice is what a build would use.
extern "C" const int hudBlocksMap[NUM_BLOCKS+1];   // Classes/Hud.mm
int run_picker_selftest() {
    char detail[200];
    Hud* hud = World::getWorld->hud;
    tap_hud(1);
    tick(5);
    check(hud->mode == MODE_PICK_BLOCK, "E's HUD button opens the block picker", "");
    // Cells 2 and 3 are dark stone and stone in the stock table: plain blocks, no special pick.
    const CGRect c0 = hud->blockBounds[2];
    ui_tap_rect(c0);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d blocktype %d", hud->mode, hud->blocktype);
    check(hud->mode == MODE_BUILD, "tapping a block cell picks it and closes the picker", detail);
    const int picked = hud->blocktype;
    tap_hud(1);
    tick(5);
    ui_tap_rect(hud->blockBounds[3]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", picked, hud->blocktype);
    check(hud->mode == MODE_BUILD && hud->blocktype != picked, "a different cell picks a different block", detail);
    const int page1Pick = hud->blocktype;

    // --- D.2p: page 2 ------------------------------------------------------------------------
    tap_hud(1);
    tick(5);
    std::snprintf(detail, sizeof(detail), "page %d", hud->pickerPage);
    check(hud->pickerPage == 0, "the picker opens on page 1 when the current block is a page-1 one", detail);
    ui_tap_rect(hud->pickerNext);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d page %d", hud->mode, hud->pickerPage);
    check(hud->mode == MODE_PICK_BLOCK && hud->pickerPage == 1, "> turns to page 2 and keeps the picker open", detail);
    {
        int got[16], reopenedOn2 = 0;
        bool inOrder = true;
        for (int i = 0; i < 16; ++i) {
            ui_tap_rect(hud->blockBounds[i]);
            tick(5);
            got[i] = hud->mode == MODE_BUILD ? hud->blocktype : -1;
            inOrder &= got[i] == TYPE_ORE_SAND + i;
            tap_hud(1);                                    // reopen: lands on the current block's page
            tick(5);
            reopenedOn2 += hud->pickerPage == 1;
        }
        std::snprintf(detail, sizeof(detail), "first %d last %d, reopened on page 2: %d/16", got[0], got[15], reopenedOn2);
        check(inOrder, "page 2's cells pick 112..127 in id order", detail);
        check(reopenedOn2 == 16, "the picker reopens on page 2 while a page-2 block is current", detail);
    }
    {
        // D.3c / D.4b: SIGN and CMD are live where GL text entry exists (here).
        ui_tap_rect(hud->blockBounds[18]);                 // CMD
        tick(5);
        const bool cmdArmed = hud->mode == MODE_BUILD && hud->blocktype == HUD_TOOL_CMD;
        tap_hud(1);                                        // reopen: the tool's page
        tick(5);
        ui_tap_rect(hud->blockBounds[17]);                 // SIGN
        tick(5);
        const bool signArmed = hud->mode == MODE_BUILD && hud->blocktype == HUD_TOOL_SIGN;
        tap_hud(1);
        tick(5);
        std::snprintf(detail, sizeof(detail), "cells %d %d, CMD armed %d, SIGN armed %d, reopened on page %d",
                      Hud::pickerCell(1, 17), Hud::pickerCell(1, 18), cmdArmed, signArmed, hud->pickerPage);
        check(Hud::pickerCell(1, 16) == TYPE_GRASS3 && Hud::pickerCell(1, 17) == HUD_TOOL_SIGN && Hud::pickerCell(1, 18) == HUD_TOOL_CMD && cmdArmed &&
              signArmed && hud->mode == MODE_PICK_BLOCK && hud->pickerPage == 1,
              "the flower block, SIGN and CMD sit after 127 and each tool arms", detail);
    }
    ui_tap_rect(hud->pickerPrev);
    tick(5);
    const int afterPrev = hud->pickerPage;
    ui_tap_rect(hud->blockBounds[3]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "page after < %d, blocktype %d (stock pick %d)", afterPrev, hud->blocktype, page1Pick);
    check(afterPrev == 0 && hud->mode == MODE_BUILD && hud->blocktype == page1Pick,
          "< returns to page 1 and its picks are unchanged", detail);

    // [ ] and the wheel, as real SDL events through the translator; < on page 1 wraps.
    tap_hud(1);
    tick(5);
    push_key(SDL_SCANCODE_RIGHTBRACKET, true); push_key(SDL_SCANCODE_RIGHTBRACKET, false);
    tick(3);
    const int afterKey = hud->pickerPage;
    push_key(SDL_SCANCODE_LEFTBRACKET, true); push_key(SDL_SCANCODE_LEFTBRACKET, false);
    tick(3);
    const int afterKey2 = hud->pickerPage;
    push_wheel(-1.0f);                                     // toward the user = next page
    tick(3);
    const int afterWheel = hud->pickerPage;
    push_wheel(1.0f);
    tick(3);
    const int afterWheel2 = hud->pickerPage;
    ui_tap_rect(hud->pickerPrev);
    tick(5);
    std::snprintf(detail, sizeof(detail), "] %d [ %d wheel %d/%d, < from page 1 %d",
                  afterKey, afterKey2, afterWheel, afterWheel2, hud->pickerPage);
    check(afterKey == 1 && afterKey2 == 0 && afterWheel == 1 && afterWheel2 == 0 && hud->pickerPage == 1,
          "] / [ and the wheel page the picker; < wraps", detail);
    tap_hud(1);                                            // close
    tick(5);

    // Block TNT's second pick: no page-2 cell has a BT variant, so all are veiled; SIGN/CMD too.
    {
        tap_hud(1);
        tick(5);
        if (hud->pickerPage != 0) { ui_tap_rect(hud->pickerNext); tick(5); }
        int tnt = -1;
        for (int i = 0; i < Hud::pickerCellCount(0); ++i) if (Hud::pickerCell(0, i) == TYPE_BLOCK_TNT) tnt = i;
        if (tnt >= 0) ui_tap_rect(hud->blockBounds[tnt]);
        tick(5);
        ui_tap_rect(hud->pickerNext);
        tick(5);
        int bt = 0;
        for (int i = 0; i < 16; ++i) bt += hudBlocksMap[Hud::pickerCell(1, i)] != -1;
        ui_tap_rect(hud->blockBounds[17]);                 // SIGN mid-pick: nothing
        tick(5);
        const int modeAfterSign = hud->mode;
        ui_tap_rect(hud->blockBounds[0]);                  // 112: the stock fallback, a plain block
        tick(5);
        std::snprintf(detail, sizeof(detail), "tnt cell %d, page-2 BT variants %d, mode after SIGN %d, then %d blocktype %d",
                      tnt, bt, modeAfterSign, hud->mode, hud->blocktype);
        check(tnt >= 0 && bt == 0 && modeAfterSign == MODE_PICK_BLOCK && hud->mode == MODE_BUILD &&
              hud->blocktype == TYPE_ORE_SAND,
              "Block TNT's second pick survives a page turn and veils every page-2 cell", detail);
    }

    tap_hud(2);
    tick(5);
    check(hud->mode == MODE_PICK_COLOR, "C's HUD button opens the colour picker", "");
    ui_tap_rect(hud->colorBounds[10]);
    tick(5);
    std::snprintf(detail, sizeof(detail), "mode %d paintColor %d", hud->mode, (int)hud->paintColor);
    check(hud->mode == MODE_PAINT && (int)hud->paintColor == 11, "tapping swatch 10 paints colour 11", detail);
    return 0;
}

int run_ui_selftest() {
    g_selftestTag = "eden-ui";
    g_tickInput = true;            // headless pump_events() only drains SDL's queue with this set
    char detail[240];
    tick(120);
    check(eden_settings_loaded() != 0, "the settings model loaded", "");

    // --- the slider's arithmetic, on its own --------------------------------------------------
    {
        GLW::Slider sl;
        sl.setRect(CGRectMake(100, 100, 200, 40));
        sl.setRange(0.25f, 3.0f, 0.05f);
        sl.setValue(1.0f);
        const bool moved = sl.beginDrag(100.0f);
        const float lo = sl.value();
        const bool again = sl.dragTo(90.0f);           // already pinned at min: no change
        sl.dragTo(300.0f);
        const float hi = sl.value();
        sl.dragTo(173.0f);
        const float mid = sl.value();
        sl.endDrag();
        const float steps = (mid - 0.25f) / 0.05f;
        std::snprintf(detail, sizeof(detail), "lo %.3f hi %.3f mid %.4f (%.3f steps) moved=%d again=%d",
                      lo, hi, mid, steps, moved, again);
        check(moved && !again && std::fabs(lo - 0.25f) < 1e-4f && std::fabs(hi - 3.0f) < 1e-4f &&
              std::fabs(steps - std::floor(steps + 0.5f)) < 1e-3f,
              "Slider pins to its ends, snaps to min+k*step, reports only real changes", detail);
    }

    // --- U.1: the kit's new arithmetic (variable-height ScrollView, IconRail, sizeToFit) -------
    {
        GLW::ScrollView sv;
        const float hs[] = { 50, 50, 120, 30, 30, 30 };   // 310 tall in a 100 view
        sv.setRowHeights(hs, 6, 100.0f);
        const int v0 = sv.visible();                       // 50+50 fit, the 120 does not
        sv.ensureVisible(2);                               // the tall row: alone, overflowing
        const int f2 = sv.first(), v2 = sv.visible();
        sv.setFirst(99);                                   // clamps to the last row that fills
        const int fmax = sv.first(), vmax = sv.visible();
        sv.setFirst(0);
        sv.beginContentDrag(0.0f);
        sv.contentDragTo(76.0f);                           // > 50 + 25: two rows on
        const int fd = sv.first();
        sv.endDrag();
        const float off = (sv.setFirst(1), sv.rowOffset(3));   // 50 + 120
        sv.setRows(10, 4);                                 // back to uniform
        const bool uni = sv.visible() == 4 && sv.total() == 10;
        std::snprintf(detail, sizeof(detail), "v0 %d first/visible at 2: %d/%d max %d/%d drag %d off %.0f uniform %d",
                      v0, f2, v2, fmax, vmax, fd, off, uni);
        check(v0 == 2 && f2 == 2 && v2 == 1 && fmax == 3 && vmax == 3 && fd == 2 && off == 170.0f && uni,
              "ScrollView variable row heights: visible/first/clamp/drag/rowOffset", detail);

        GLW::IconRail rail;
        const int icons[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        rail.setTiles(icons, 10);
        const float tw = GLW::IconRail::designW();
        rail.setRect(CGRectMake(0, 0, tw, GLW::du(4 * 2 + 64 * 3 + 6 * 2)));   // exactly 3 tiles
        rail.setEnabled(1, false);
        const CGRect t0 = rail.tileRect(0), t3 = rail.tileRect(3);
        const int h0 = rail.hit(t0.origin.x + 2, t0.origin.y + 2);
        const CGRect t1 = rail.tileRect(1);
        const int h1 = rail.hit(t1.origin.x + 2, t1.origin.y + 2);   // disabled: inert
        const bool moved = rail.scrollBy(100);
        const int rf = rail.first();
        std::snprintf(detail, sizeof(detail), "visible %d tile0 %.0fx%.0f hidden3 %.0f hit0 %d hit1 %d scrolled %d first %d",
                      rail.visibleTiles(), t0.size.width, t0.size.height, t3.size.width, h0, h1, moved, rf);
        check(rail.visibleTiles() == 3 && t0.size.width > 0 && t3.size.width == 0 && h0 == 0 && h1 == -1 &&
              moved && rf == 7, "IconRail: whole tiles, disabled tiles inert, scrolls to the last page", detail);

        GLW::Button a, b;
        a.setLabel("Ok", GLW::du(20));   a.sizeToFit();
        b.setLabel("A much longer action label", GLW::du(20)); b.sizeToFit();
        a.setRect(CGRectMake(10, 10, 500, 29));          // the width is ignored once fitted
        std::snprintf(detail, sizeof(detail), "short %.1f (min %.1f) long %.1f rect %.1f",
                      a.fitWidth(), GLW::du(64), b.fitWidth(), a.rect().size.width);
        check(std::fabs(a.fitWidth() - GLW::du(64)) < 0.01f && b.fitWidth() > GLW::du(64) &&
              a.rect().size.width == a.fitWidth(), "Button::sizeToFit shrink-wraps with a 64u floor", detail);
    }

    // --- N.5.11: the toast's lifetime, and the HUD-style statusbar routed through it ----------
    {
        GLW::Toast t;
        t.show("Can't place a sign under the block", 2.0f);
        const bool shown = t.visible() && t.rect().size.width > 0.0f;
        const float w1 = t.rect().size.width;
        t.update(1.5f);
        t.show("Can't place a sign under the block", 2.0f);   // same text: lifetime reset only
        t.update(1.5f);
        const bool refreshed = t.visible();
        t.show("World Saved", 2.0f);
        const float w2 = t.rect().size.width;                 // shrink-wrapped: shorter text, narrower
        t.update(2.5f);
        const bool expired = !t.visible() && t.text().empty();
        t.show("Loading", 1000.0f);
        t.update(5000.0f);
        const bool sticky = t.visible();
        t.clear();
        statusbar sb(CGRectMake(0, 0, 0, 0));
        sb.useToast();
        sb.setStatus(@"World Saved", 1.0f);
        const bool sbShown = sb.current() != nil;
        sb.update(2.0f);
        const bool sbGone = sb.current() == nil;
        sb.setStatus(@"World Saved", 1.0f);                  // same text after expiry re-shows
        const bool sbAgain = sb.current() != nil;
        std::snprintf(detail, sizeof(detail), "shown=%d w %.1f>%.1f refreshed=%d expired=%d sticky=%d "
                      "sb %d/%d/%d", shown, w1, w2, refreshed, expired, sticky, sbShown, sbGone, sbAgain);
        check(shown && w1 > w2 && w2 > 0.0f && refreshed && expired && sticky && sbShown && sbGone &&
              sbAgain, "Toast shows, refreshes on the same text, expires, honours >=1000 s, "
              "shrink-wraps; statusbar::useToast() routes through it", detail);
    }

    // --- the settings screen, through real pointer events -------------------------------------
    eden_settings_menu_open_now();
    tick(10);
    SettingsMenu* sm = (World::getWorld && World::getWorld->menu) ? World::getWorld->menu->settings : nullptr;
    if (!sm) { check(false, "the settings screen exists", ""); return 1; }
    {   // U.3: seven tabs, every settings row on exactly one of them
        int rows = 0, shown = 0;
        for (int t = 0; t < sm->tabCount(); ++t) rows += sm->tabRowCount(t);
        for (int i = 0; i < eden_settings_count(); ++i) if (!eden_settings_native_hidden(i)) shown++;
        std::snprintf(detail, sizeof(detail), "%d tabs, %d rows on them, %d rows shown", sm->tabCount(), rows, shown);
        check(sm->tabCount() == 7 && rows == shown && sm->tabRowCount(3) == 0,
              "settings has 7 tabs and every native row is on one", detail);
    }
    {   // U.4: the Keys tab. A key button arms a capture, any tap cancels it, Defaults resets.
        // Every binding is saved first and put back after: Defaults resets them ALL.
        std::vector<int> saved;
        for (int i = 0; i < eden_keybind_count(); ++i) saved.push_back(eden_keybind_get(i));
        const int mi = eden_keybind_index("blockPicker");
        CGRect kr;
        int armed = -1, cancelled = -1, rebound = 0, restored = 0;
        const bool found = mi >= 0 && sm->keyRect(mi, &kr);
        if (found) {
            tick(2);
            ui_tap_rect(kr);
            armed = eden_keybind_capture_active();
            ui_tap_rect(kr);
            cancelled = eden_keybind_capture_active();
            eden_keybind_set(mi, (int)SDL_SCANCODE_T);
            rebound = eden_keybind_get(mi);
            ui_tap_rect(sm->defaultsRect());
            restored = eden_keybind_get(mi);
        }
        std::snprintf(detail, sizeof(detail), "found=%d armed=%d cancelled=%d rebound=%d restored=%d (default %d)",
                      (int)found, armed, cancelled, rebound, restored, found ? eden_keybind_default(mi) : -1);
        check(found && armed == mi && cancelled < 0 && rebound == (int)SDL_SCANCODE_T &&
              restored == eden_keybind_default(mi),
              "Keys tab: a key button arms a capture, a tap cancels, Defaults resets", detail);
        for (int i = 0; i < (int)saved.size(); ++i) eden_keybind_set(i, saved[i]);
        sm->resetView();
        tick(2);
    }

    const int iHealth = settings_index("health");
    const int iVol    = settings_index("music_volume");
    // The first ENUM row this build shows (input_mode, the obvious one, is web-only).
    const char* modeKey = nullptr;
    for (int i = 0; i < eden_settings_count() && !modeKey; ++i)
        if (eden_settings_kind(i) == 2 && sm->showRow(eden_settings_key(i), nullptr)) modeKey = eden_settings_key(i);
    const int iMode   = modeKey ? settings_index(modeKey) : -1;
    const float health0 = eden_settings_get(iHealth), vol0 = eden_settings_get(iVol);
    const float mode0 = iMode >= 0 ? eden_settings_get(iMode) : 0.0f;
    CGRect r;

    {   // B.8: presets drive view_distance (+ render_scale off iOS); a hand edit drops to Custom
        const int iP = settings_index("graphics_preset"), iV = settings_index("view_distance"),
                  iD = settings_index("render_radius");     // B.5b-2, X.17
        const float p0 = iP >= 0 ? eden_settings_get(iP) : 0.0f, v0 = iV >= 0 ? eden_settings_get(iV) : 0.0f,
                    d0 = iD >= 0 ? eden_settings_get(iD) : 0.0f;
#if defined(EDEN_PLATFORM_IOS)
        const float highD = 6.0f;                              // radius 10 (P.4 decision 6)
#else
        const float highD = 8.0f;                              // radius 16 (kRenderRadius[8])
#endif
        bool ok = iP >= 0 && iV >= 0 && iD >= 0;
        if (ok) {
            eden_settings_set(iP, 1.0f);                       // Low
            ok = eden_settings_get(iV) == 1.0f && eden_settings_get(iD) == 5.0f && eden_settings_get(iP) == 1.0f;
            eden_settings_set(iP, 3.0f);                       // High: Auto view, the larger window
            ok = ok && eden_settings_get(iV) == 0.0f && eden_settings_get(iD) == highD && eden_settings_get(iP) == 3.0f;
            eden_settings_set(iV, 3.0f);                       // by hand
            ok = ok && eden_settings_get(iP) == 0.0f;          // -> Custom
            eden_settings_set(iP, 2.0f);                       // Default restores Auto and radius 9
            ok = ok && eden_settings_get(iV) == 0.0f && eden_settings_get(iD) == 5.0f;
            eden_settings_set(iD, 2.0f);                       // the radius by hand -> Custom too
            ok = ok && eden_settings_get(iP) == 0.0f;
            eden_settings_set(iV, v0); eden_settings_set(iD, d0); eden_settings_set(iP, p0);
        }
        check(ok, "graphics presets set their knobs; a hand edit becomes Custom", "");
    }

    if (sm->showRow("health", &r)) {
        tick(2);
        ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
        const float after = eden_settings_get(iHealth);
        ui_tap(r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f);
        std::snprintf(detail, sizeof(detail), "%.0f -> %.0f -> %.0f", health0, after, eden_settings_get(iHealth));
        check(after != health0 && eden_settings_get(iHealth) == health0, "tapping a Toggle flips its setting, twice restores", detail);
    } else check(false, "the health row is shown", "");

    if (sm->showRow("music_volume", &r)) {
        tick(2);
        const float cy = r.origin.y + r.size.height * 0.5f;
        ui_pointer(0, r.origin.x + 1.0f, cy); tick(3);
        const float atLeft = eden_settings_get(iVol);
        ui_pointer(1, r.origin.x + r.size.width - 1.0f, cy + 60.0f); tick(3);   // off the track, vertically
        const float atRight = eden_settings_get(iVol);
        ui_pointer(1, r.origin.x + r.size.width * 0.5f, cy); tick(3);
        const float atMid = eden_settings_get(iVol);
        ui_pointer(2, r.origin.x + r.size.width * 0.5f, cy); tick(3);
        std::snprintf(detail, sizeof(detail), "left %.2f right %.2f mid %.2f (was %.2f)", atLeft, atRight, atMid, vol0);
        check(atLeft == 0.0f && atRight == 1.0f && std::fabs(atMid - 0.5f) <= 0.051f,
              "dragging a Slider writes the setting live, and keeps tracking off the track", detail);
    } else check(false, "the music volume row is shown", "");

    if (modeKey && sm->showRow(modeKey, &r)) {
        tick(2);
        const float h = r.size.height;    // the stepper's buttons are square at its height
        ui_tap(r.origin.x + r.size.width - h * 0.5f, r.origin.y + h * 0.5f);
        const int n = eden_settings_enum_count(iMode);
        const int want = ((int)mode0 + 1) % (n > 0 ? n : 1);
        std::snprintf(detail, sizeof(detail), "%s: %.0f -> %.0f of %d", modeKey, mode0, eden_settings_get(iMode), n);
        check((int)eden_settings_get(iMode) == want, "a Stepper's > steps an enum row", detail);
    } else check(false, "an enum row is shown", "");

    eden_settings_set(iHealth, health0);
    eden_settings_set(iVol, vol0);
    if (iMode >= 0) eden_settings_set(iMode, mode0);
    eden_settings_menu_close();
    tick(10);

    if (run_menu_selftest() != 0) return 1;

    // --- the text field: typing, UTF-8 backspace, the byte cap, Return, Escape -----------------
    {
        static const char* const kBtn[] = { "OK", "Cancel" };
        check(GLDialog::textEntryAvailable(), "native reports GL text entry", "");
        g_promptChosen = -2;
        GLDialog::prompt("Selftest", NULL, "abc", 8, kBtn, 2, ui_prompt_cb);
        tick(2);
        check(eden_text_input_active() == 1, "focusing the field starts platform text input", "");
        push_text("d\xC3\xA9");             // "dé": é is two bytes
        tick(2);
        push_tap_key(SDL_SCANCODE_BACKSPACE);
        tick(2);
        push_text("xyz12345");
        push_tap_key(SDL_SCANCODE_W);       // must reach the field's queue (nothing), not the game
        tick(2);
        push_tap_key(SDL_SCANCODE_RETURN);
        tick(3);
        std::snprintf(detail, sizeof(detail), "chosen %d text \"%s\" active %d dialog %d",
                      g_promptChosen, g_promptText.c_str(), eden_text_input_active(), GLDialog::active() ? 1 : 0);
        check(g_promptChosen == 0 && g_promptText == "abcdxyz1" && eden_text_input_active() == 0 && !GLDialog::active(),
              "type + backspace (a whole UTF-8 char) + 8-byte cap + Return commits", detail);

        push_text("late");
        tick(2);
        char sink[16];
        check(eden_text_input_take(sink, sizeof(sink)) == 0, "text typed with no field focused is dropped", "");

        g_promptChosen = -2;
        GLDialog::prompt("Selftest", NULL, "keep", 8, kBtn, 2, ui_prompt_cb);
        tick(2);
        push_text("zz");
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::snprintf(detail, sizeof(detail), "chosen %d text \"%s\" in_menu %d", g_promptChosen,
                      g_promptText.c_str(), eden_hud_in_menu());
        check(g_promptChosen == 1 && !GLDialog::active(), "Escape picks the LAST button (Cancel) and closes", detail);
    }

    // --- rename a world that exists on disk -------------------------------------------------
    // Play it once so there is a file (a created-but-never-played world has none), go back to the
    // menu, rename through the real button -> screen -> save path, then check the file: ONLY
    // WorldFileHeader::name may differ, and the world must still load under its new name.
    // H.15: the button is Properties (World Properties, Classes/WorldPropertiesMenu.h), whose World
    // tab holds the name; a name-only change saves with no dialog. (N.4.5 opened a prompt here.)
    const char* name = g_opt.world.empty() ? "ui-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(60);
    if (!quit_to_menu()) return 1;
    tick(30);
    check(!World::getWorld->menu->showhome, "Quit to Menu lands on My Worlds, not the home screen", "");   // I.7
    int idx = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (std::strcmp(eden_menu_world_name(i), name) == 0) idx = i;
    check(idx >= 0, "the played world is listed", name);
    if (idx < 0) return 1;
    eden_menu_select(idx);
    tick(5);
    const std::string file = std::string(eden_platform_documents_root()) + "/" + eden_menu_world_file(idx);
    std::vector<unsigned char> before, after;
    check(read_file(file, &before) && before.size() > sizeof(WorldFileHeader), "the world has a file", file.c_str());

    Menu* menu = World::getWorld->menu;
    const bool offered = menu->propertiesOffered();
    check(offered, "the menu offers Properties for the selected world (Rename's slot)", "");
    ui_tap_rect(menu->controlRect("properties"));
    tick(3);
    WorldPropertiesMenu* wp = menu->props;
    std::snprintf(detail, sizeof(detail), "showprops %d open %d dialog %d save %d", menu->showprops ? 1 : 0,
                  wp->isOpen() ? 1 : 0, GLDialog::active() ? 1 : 0, wp->saveEnabled() ? 1 : 0);
    check(menu->showprops && wp->isOpen() && !GLDialog::active() && !wp->saveEnabled(),
          "Properties opens World Properties (no prompt), Save disabled until something changes", detail);
    std::snprintf(detail, sizeof(detail), "name \"%s\" height \"%s\"", wp->text("name"), wp->value("height"));
    check(std::strcmp(wp->text("name"), name) == 0 && wp->rowVisible("photo") && !wp->rowVisible("seed"),
          "the World tab shows the photo row and the name, and no seed for a Normal world", detail);
    ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_TERRAIN));
    tick(2);
    std::snprintf(detail, sizeof(detail), "tab %d type \"%s\" hills %d", wp->tab(), wp->value("type"),
                  wp->rowVisible("hills") ? 1 : 0);
    check(wp->tab() == WorldPropertiesMenu::TAB_TERRAIN && wp->rowVisible("type") && !wp->rowEditable("type") &&
          !wp->rowVisible("hills"), "a Normal world's Terrain tab is its type only", detail);
    ui_tap_rect(wp->tabRect(WorldPropertiesMenu::TAB_WORLD));
    tick(2);
    ui_tap_rect(wp->controlRect("name"));
    tick(2);
    check(wp->focused("name") && eden_text_input_active() == 1, "tapping the name field focuses it", "");
    for (int i = 0; i < 64; ++i) push_tap_key(SDL_SCANCODE_BACKSPACE);
    tick(2);
    push_text("Renamed \xC3\x9C");          // "Renamed Ü"
    tick(2);
    check(wp->saveEnabled() && !wp->terrainChanged(), "a new name enables Save (a name-only change)", "");
    push_tap_key(SDL_SCANCODE_RETURN);       // Return in the field is Save
    tick(5);
    std::snprintf(detail, sizeof(detail), "open %d dialog %d result %d", wp->isOpen() ? 1 : 0,
                  GLDialog::active() ? 1 : 0, wp->lastResult());
    check(!wp->isOpen() && !menu->showprops && !GLDialog::active() && wp->lastResult() == WP_WROTE_NAME,
          "a name-only save raises no dialog and closes the screen", detail);
    const char* want = "Renamed \xC3\x9C";
    const std::string listed = eden_menu_world_name(idx);   // copy: one static buffer per accessor family
    std::string onDisk = cpstring(World::getWorld->fm->getName([NSString stringWithUTF8String:eden_menu_world_file(idx)]));
    std::snprintf(detail, sizeof(detail), "listed \"%s\" on disk \"%s\"", listed.c_str(), onDisk.c_str());
    check(listed == want && onDisk == want, "the rename reaches the list and the file header", detail);

    read_file(file, &after);
    // S.8: the world is an `.emod` -- append-only, so a rename appends a new WORLD_HEADER record and no
    // byte already written moves (the `.eden` form rewrote the 50 name bytes in place).
    size_t diffPrefix = 0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i]) diffPrefix++;
    std::snprintf(detail, sizeof(detail), "size %zu -> %zu, %zu earlier bytes changed",
                  before.size(), after.size(), diffPrefix);
    check(after.size() > before.size() && diffPrefix == 0,
          "the rename only appended to the file", detail);
    {
        // H.15: Escape over the screen is Back (discards).
        ui_tap_rect(menu->controlRect("properties"));
        tick(3);
        const bool opened = menu->showprops;
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(3);
        std::vector<unsigned char> again;
        read_file(file, &again);
        std::snprintf(detail, sizeof(detail), "opened %d showprops %d bytes %zu -> %zu", opened ? 1 : 0,
                      menu->showprops ? 1 : 0, after.size(), again.size());
        check(opened && !menu->showprops && again.size() == after.size(), "Escape closes Properties and writes nothing", detail);
    }

    const bool reopened = open_world(want, g_opt.height);
    check(reopened, "the renamed world loads under its new name", "");

    // --- Stage 5.5: the kit pause menu --------------------------------------------------------
    // Real taps at the rects the Hud laid out: Settings opens the GL SettingsMenu as an in-game
    // modal, its own Back returns to the pause menu (not to gameplay, not to the title screen),
    // and Resume closes the menu.
    if (reopened) {
        Hud* hud = World::getWorld->hud;
        tick(30);
        if (!eden_hud_in_menu()) tap_hud(0);
        tick(10);
        check(eden_hud_in_menu() == 1, "Escape's HUD button opens the pause menu", "");
        const Button rs = hud->rsettings;
        ui_tap(rs.origin.x + rs.size.width * 0.5f, rs.origin.y + rs.size.height * 0.5f);
        tick(5);
        std::snprintf(detail, sizeof(detail), "showsettings=%d game_mode=%d", (int)menu->showsettings,
                      World::getWorld->game_mode);
        check(menu->showsettings && World::getWorld->game_mode == GAME_MODE_PLAY,
              "pause menu -> Settings opens the settings screen over the world", detail);
        const CGRect bk = menu->settings->backRect();
        ui_tap(bk.origin.x + bk.size.width * 0.5f, bk.origin.y + bk.size.height * 0.5f);
        tick(5);
        std::snprintf(detail, sizeof(detail), "showsettings=%d in_menu=%d", (int)menu->showsettings,
                      eden_hud_in_menu());
        check(!menu->showsettings && eden_hud_in_menu() == 1, "Settings' Back returns to the pause menu", detail);
        // U.3: Escape over in-game Settings is its Back (it used to be swallowed). U.4: on the Keys
        // tab with a capture armed, the first Escape only cancels the capture.
        ui_tap(rs.origin.x + rs.size.width * 0.5f, rs.origin.y + rs.size.height * 0.5f);
        tick(5);
        const int miPick = eden_keybind_index("blockPicker");
        const int pick0 = eden_keybind_get(miPick);
        CGRect kr;
        if (menu->settings->keyRect(miPick, &kr)) ui_tap_rect(kr);
        tick(2);
        const int armed = eden_keybind_capture_active();
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(5);
        const bool stillSettings = menu->showsettings && eden_keybind_capture_active() < 0 &&
                                   eden_keybind_get(miPick) == pick0;
        push_tap_key(SDL_SCANCODE_ESCAPE);
        tick(5);
        std::snprintf(detail, sizeof(detail), "armed=%d; after 1st Esc settings+uncaptured=%d; after 2nd "
                      "showsettings=%d in_menu=%d", armed, (int)stillSettings, (int)menu->showsettings,
                      eden_hud_in_menu());
        check(armed == miPick && stillSettings && !menu->showsettings && eden_hud_in_menu() == 1,
              "Escape steps back capture -> Settings -> the pause menu", detail);
        // --- C.3: the Console over the running world --------------------------------------------
        {
            ConsoleMenu* cm = menu->console;
            const Button* pm[7] = { &hud->rresume, &hud->rsave, &hud->rhome, &hud->rcam, &hud->rsettings,
                                    &hud->rconsole, &hud->rexit };
            bool allIn = true;
            for (const Button* b : pm)
                if (b->size.width <= 0 || b->origin.x < 0 || b->origin.y < 0 ||
                    b->origin.x + b->size.width > SCREEN_WIDTH || b->origin.y + b->size.height > SCREEN_HEIGHT) allIn = false;
            std::snprintf(detail, sizeof(detail), "screen %.0fx%.0f console button y=%.0f h=%.0f", SCREEN_WIDTH, SCREEN_HEIGHT,
                          hud->rconsole.origin.y, hud->rconsole.size.height);
            check(allIn, "the pause menu's seven buttons all fit on the screen", detail);
            const Button rc = hud->rconsole;
            ui_tap(rc.origin.x + rc.size.width * 0.5f, rc.origin.y + rc.size.height * 0.5f);
            tick(5);
            std::snprintf(detail, sizeof(detail), "showconsole=%d open=%d in_menu=%d game_mode=%d", (int)menu->showconsole,
                          cm->isOpen() ? 1 : 0, eden_hud_in_menu(), World::getWorld->game_mode);
            check(menu->showconsole && cm->isOpen() && eden_hud_in_menu() == 0 && World::getWorld->game_mode == GAME_MODE_PLAY,
                  "pause menu -> Console opens it over the world and closes the pause menu", detail);
            check(cm->inputFocused() && eden_text_input_active() == 1, "the Console's input is focused on open", "");
            check(eden_ui_wants_cursor() == 1, "the Console releases the mouse", "");

            push_text("echo hello world");
            tick(2);
            push_tap_key(SDL_SCANCODE_RETURN);
            tick(3);
            bool echoed = false, printed = false;
            for (const Console::Line& l : Console::scrollback()) {
                if (l.kind == Console::ECHO && l.text == "> echo hello world") echoed = true;
                if (l.kind != Console::ECHO && l.text == "hello world") printed = true;
            }
            check(echoed && printed && cm->input().empty() && cm->inputFocused(),
                  "Return runs the line: echoed, answered, the field cleared and still focused", "");
            check(cm->visibleLines() >= 2 && cm->following(), "the scrollback shows both lines and follows the bottom", "");

            ui_tap_rect(cm->controlRect("up"));
            tick(3);
            check(cm->input() == "echo hello world", "Up recalls the last line", cm->input().c_str());
            ui_tap_rect(cm->controlRect("down"));
            tick(3);
            check(cm->input().empty(), "Down returns to the (empty) draft", cm->input().c_str());

            cm->setInput("vid");
            ui_tap_rect(cm->controlRect("tab"));
            tick(3);
            check(cm->input().compare(0, 6, "video.") == 0, "Tab completes a variable to its group", cm->input().c_str());
            cm->setInput("");

            ui_tap_rect(cm->tabRect(ConsoleMenu::TAB_VARIABLES));
            tick(5);
            std::snprintf(detail, sizeof(detail), "tab %d rows %d (cvars %zu)", cm->tab(), cm->varRows(), Console::cvarCount());
            check(cm->tab() == ConsoleMenu::TAB_VARIABLES && cm->varRows() == (int)Console::cvarCount() && cm->varRows() > 40,
                  "the Variables tab lists every variable", detail);
            // Every cvar's name pointer still names it (C.3 found them dangling once: a vector of records
            // with std::string members moved them, and the short names live inside the string object).
            int badName = 0;
            for (size_t i = 0; i < Console::cvarCount(); ++i)
                if (Console::cvarIndex(Console::cvarAt(i)->name) != (int)i) badName++;
            check(badName == 0, "every variable's name pointer is stable", "");
            ui_tap_rect(cm->tabRect(ConsoleMenu::TAB_HELP));
            tick(5);
            std::snprintf(detail, sizeof(detail), "tab %d rows %d", cm->tab(), cm->helpRows());
            check(cm->tab() == ConsoleMenu::TAB_HELP && cm->helpRows() >= 30, "the Help tab lists the verbs", detail);
            ui_tap_rect(cm->tabRect(ConsoleMenu::TAB_CONSOLE));
            tick(5);
            check(cm->tab() == ConsoleMenu::TAB_CONSOLE && cm->inputFocused(), "back on the Console tab the input is focused again", "");

            // The strip: collapse, run a line from it, expand.
            ui_tap_rect(cm->controlRect("collapse"));
            tick(5);
            check(cm->collapsed(), "Collapse docks the strip", "");
            const CGRect sp = cm->controlRect("panel");
            check(sp.origin.y + sp.size.height > SCREEN_HEIGHT * 0.6f, "the strip sits at the top so the world shows below", "");
            cm->setInput("echo from the strip");
            ui_tap_rect(cm->controlRect("run"));
            tick(3);
            bool strip = false;
            for (const Console::Line& l : Console::scrollback()) if (l.text == "from the strip") strip = true;
            check(strip && cm->visibleLines() >= 1, "Run works from the strip", "");
            ui_tap_rect(cm->controlRect("collapse"));
            tick(3);
            check(!cm->collapsed(), "Expand restores the full panel", "");

            // C.4: a hardware keyboard's Up / Down / Tab do what the buttons do.
            push_tap_key(SDL_SCANCODE_UP);
            tick(3);
            check(cm->input() == "echo from the strip", "the Up key recalls the last line", cm->input().c_str());
            push_tap_key(SDL_SCANCODE_DOWN);
            tick(3);
            check(cm->input().empty(), "the Down key returns to the draft", cm->input().c_str());
            cm->setInput("vid");
            push_tap_key(SDL_SCANCODE_TAB);
            tick(3);
            check(cm->input().compare(0, 6, "video.") == 0, "the Tab key completes", cm->input().c_str());
            cm->setInput("");

            // C.4: the on-screen keyboard (faked through the seam). A low keyboard: the full layout moves
            // above it without the rail; a tall one (iPhone landscape) or an unknown one: the auto strip;
            // gone: the full layout with the rail. The input is never below the keyboard's top.
            const float kbLow = SCREEN_HEIGHT * 0.3f, kbTall = SCREEN_HEIGHT * 0.62f;
            eden_native_debug_set_keyboard_top(kbLow);
            tick(3);
            CGRect in = cm->controlRect("input"), pn = cm->controlRect("panel");
            std::snprintf(detail, sizeof(detail), "kb %.0f input y %.0f panel y %.0f auto %d rail %d lines %d", kbLow,
                          in.origin.y, pn.origin.y, cm->autoStrip(), cm->railShown(), cm->visibleLines());
            check(!cm->autoStrip() && !cm->railShown() && in.origin.y >= kbLow && pn.origin.y >= kbLow && cm->visibleLines() >= 2,
                  "a keyboard: the full layout sits above it, without the rail", detail);
            eden_native_debug_set_keyboard_top(kbTall);
            tick(3);
            in = cm->controlRect("input"); pn = cm->controlRect("panel");
            std::snprintf(detail, sizeof(detail), "kb %.0f input y %.0f panel y %.0f auto %d lines %d", kbTall,
                          in.origin.y, pn.origin.y, cm->autoStrip(), cm->visibleLines());
            check(cm->autoStrip() && !cm->collapsed() && in.origin.y >= kbTall && pn.origin.y >= kbTall && cm->visibleLines() >= 1,
                  "a tall keyboard: the auto strip, above it", detail);
            push_text("echo over the keyboard");
            tick(2);
            push_tap_key(SDL_SCANCODE_RETURN);
            tick(3);
            bool overKb = false;
            for (const Console::Line& l : Console::scrollback()) if (l.text == "over the keyboard") overKb = true;
            check(overKb, "typing and Return work in the auto strip", "");
            eden_native_debug_set_keyboard_top(-1.0f);
            tick(3);
            check(cm->autoStrip(), "a keyboard of unknown height: the auto strip (top-docked)", "");
            eden_native_debug_set_keyboard_top(0.0f);
            tick(3);
            check(!cm->autoStrip() && cm->railShown() && cm->controlRect("panel").origin.y < kbLow,
                  "the keyboard gone: the full layout and the rail are back", "");
            eden_native_debug_set_keyboard_top(-2.0f);

            // Escape: the focused field takes the first (blur), the second closes. Blurred, the keys are
            // still the Console's: typing must never walk.
            push_tap_key(SDL_SCANCODE_ESCAPE);
            tick(3);
            check(cm->isOpen() && !cm->inputFocused() && eden_ui_captures_keys() == 1, "Escape once blurs the field and keeps the Console open", "");
            push_tap_key(SDL_SCANCODE_ESCAPE);
            tick(3);
            check(!menu->showconsole && !cm->isOpen() && eden_hud_in_menu() == 0, "Escape twice closes it, back to play", "");

            // The `console` key (Backquote): opens from play; typed into a focused field; closes once the
            // field is blurred.
            push_tap_key(SDL_SCANCODE_GRAVE);
            tick(3);
            check(menu->showconsole && cm->isOpen() && cm->inputFocused() && cm->input().empty(), "the console key opens it, input focused and empty", "");
            push_tap_key(SDL_SCANCODE_GRAVE);
            tick(3);
            check(cm->isOpen(), "a focused field owns the console key", "");
            push_tap_key(SDL_SCANCODE_ESCAPE);
            tick(3);
            push_tap_key(SDL_SCANCODE_GRAVE);
            tick(3);
            check(!cm->isOpen(), "the console key closes it while the field is blurred", "");
            ui_tap_rect(cm->controlRect("back"));   // closed: a no-op, must not crash
            tick(2);

            // Back closes it too.
            eden_console_menu_open();
            tick(3);
            ui_tap_rect(cm->controlRect("back"));
            tick(3);
            check(!cm->isOpen() && !menu->showconsole, "Back closes the Console", "");
            tap_hud(0);                      // the pause menu again, for Resume below
            tick(10);
        }
        const Button rr = hud->rresume;
        ui_tap(rr.origin.x + rr.size.width * 0.5f, rr.origin.y + rr.size.height * 0.5f);
        tick(5);
        check(eden_hud_in_menu() == 0, "Resume closes the pause menu", "");
        run_picker_selftest();
        quit_to_menu();
    }

    std::printf("[eden-ui] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --browser-selftest  (ROADMAP 5.9: Get Worlds — the archive, the current and the legacy server)
// ---------------------------------------------------------------------------------------------
// OFFLINE, and that is the point: the CI runners must not depend on edengame.net being up, so
// Net_native.cpp's fixture mode answers every URL from files this test writes first —
// `<docs>/.net-fixtures/<host>/<path>@<query>`. Everything above the HTTP stack is the real thing:
// the menu's button, the browser's tabs/rows/field/buttons through pointer and SDL text events,
// the list parsers, the per-frame unpacker (a gzip, a deflate zip, a stored zip around a deflate
// zip), the import, and finally loading a downloaded world. The payload is a world this test plays
// and saves first, so "the download is a world" is checked byte-for-byte and by loading it.
//
// The legacy server gets NO fixtures on purpose: that is the "server unreachable" path.
// --net-live-selftest below runs the same screen against the real servers (not a CI gate).
extern "C" void eden_net_set_fixture_root(const char* dir);
void mkdirs(const std::string& path);

namespace {
std::vector<std::string> g_fixtureFiles;

bool write_bytes(const std::string& path, const std::vector<unsigned char>& b) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) mkdirs(path.substr(0, slash));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    g_fixtureFiles.push_back(path);
    return ok;
}
std::vector<unsigned char> bytes_of(const std::string& s) { return std::vector<unsigned char>(s.begin(), s.end()); }

std::vector<unsigned char> deflate_bytes(const std::vector<unsigned char>& in, int windowBits) {
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    deflateInit2(&z, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    std::vector<unsigned char> out(deflateBound(&z, (uLong)in.size()) + 64);
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    z.next_out = out.data();
    z.avail_out = (uInt)out.size();
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}
std::vector<unsigned char> gzip_bytes(const std::vector<unsigned char>& in) { return deflate_bytes(in, 15 + 16); }

// A single-entry PK zip, the shape the archive serves: local header, data, central directory, EOCD.
// `streamed` sets general-purpose bit 3 and ZEROES the local header's sizes, as a streaming
// archiver does — the case that makes a reader trust the central directory.
std::vector<unsigned char> zip_bytes(const char* name, const std::vector<unsigned char>& data, bool deflate,
                                     bool streamed) {
    const std::vector<unsigned char> body = deflate ? deflate_bytes(data, -15) : data;
    const unsigned crc = (unsigned)crc32(0, data.data(), (uInt)data.size());
    const unsigned nlen = (unsigned)std::strlen(name);
    std::vector<unsigned char> z;
    auto u16 = [&](unsigned v) { z.push_back(v & 0xff); z.push_back((v >> 8) & 0xff); };
    auto u32 = [&](unsigned v) { u16(v & 0xffff); u16(v >> 16); };
    const unsigned method = deflate ? 8 : 0, flags = streamed ? 8 : 0;
    u32(0x04034b50); u16(20); u16(flags); u16(method); u16(0); u16(0);
    u32(streamed ? 0 : crc); u32(streamed ? 0 : (unsigned)body.size()); u32(streamed ? 0 : (unsigned)data.size());
    u16(nlen); u16(0);
    z.insert(z.end(), name, name + nlen);
    z.insert(z.end(), body.begin(), body.end());
    if (streamed) { u32(0x08074b50); u32(crc); u32((unsigned)body.size()); u32((unsigned)data.size()); }
    const unsigned cdOff = (unsigned)z.size();
    u32(0x02014b50); u16(20); u16(20); u16(flags); u16(method); u16(0); u16(0);
    u32(crc); u32((unsigned)body.size()); u32((unsigned)data.size());
    u16(nlen); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
    z.insert(z.end(), name, name + nlen);
    const unsigned cdSize = (unsigned)z.size() - cdOff;
    u32(0x06054b50); u16(0); u16(0); u16(1); u16(1); u32(cdSize); u32(cdOff); u16(0);
    return z;
}

int menu_index_of_file(const char* file) {
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_file(i), file)) return i;
    return -1;
}

// Stage S / S.5: with --world-format=emod a download lands as `<id>.emod` (converted while it
// inflated) — so "the same world" means its `.eden` export equals what was served.
const char* world_ext() { return ".emod"; }
bool world_bytes(const std::string& path, std::vector<unsigned char>* out) {
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".emod") != 0) return read_file(path, out);
    emod::EdenExporter ex;
    if (!ex.begin(path, emod::ExportOptions())) return false;
    out->clear();
    unsigned char buf[1 << 16];
    for (size_t n; (n = ex.read(buf, sizeof buf)) > 0;) out->insert(out->end(), buf, buf + n);
    return !ex.failed();
}
std::vector<unsigned char> gunzip_bytes(const std::vector<unsigned char>& in) {
    z_stream z;
    std::memset(&z, 0, sizeof(z));
    inflateInit2(&z, 15 + 32);
    std::vector<unsigned char> out;
    unsigned char buf[1 << 16];
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    int rc;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (rc == Z_OK);
    inflateEnd(&z);
    return rc == Z_STREAM_END ? out : std::vector<unsigned char>();
}

bool browser_wait_list(WorldBrowser* b) {
    return tick_until([b] { return !b->listLoading(); }, 3000, "the browser's list");
}

// Selects the row whose id is `id` (scrolled into view first) and taps
// Download, then waits for the browser to hand back to the menu or to report a failure.
bool browser_download(WorldBrowser* b, Menu* m, const char* id) {
    int at = -1;
    for (int i = 0; i < b->entryCount(); ++i) if (!std::strcmp(b->entryId(i), id)) at = i;
    CGRect r;
    if (at >= 0) b->showRow(at);
    if (at < 0 || !b->rowRect(at, &r)) return false;
    ui_tap_rect(r);
    tick(2);
    if (b->selectedIndex() != at) return false;
    ui_tap_rect(b->controlRect("download"));
    tick(2);
    return tick_until([b, m] { return !m->showbrowser || !b->busy(); }, 6000, "the download");
}
}  // namespace

int run_browser_selftest_body(bool live) {
    char detail[300];
    Menu* m = World::getWorld->menu;
    WorldBrowser* b = m->browser;
    const std::string docs = eden_platform_documents_root();

    if (live) {
        check(WorldBrowser::available(), "this build has a network backend", "");
        if (!WorldBrowser::available()) return 1;
        ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
        tick(3);
        check(m->showbrowser && b->isOpen(), "Get Worlds opens the browser", "");
        const char* names[3] = { "archive", "current server", "legacy server" };
        for (int src = 0; src < 3; ++src) {
            ui_tap_rect(b->tabRect(src));
            tick(3);
            browser_wait_list(b);
            std::snprintf(detail, sizeof(detail), "%d worlds; status \"%s\"; first \"%s\"", b->entryCount(),
                          b->statusText(), b->entryName(0));
            char what[80];
            std::snprintf(what, sizeof(what), "the %s lists worlds", names[src]);
            check(b->entryCount() > 5, what, detail);
            if (src >= 1) {                   // Recent, the paged list, on the servers
                ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
                tick(3);
                browser_wait_list(b);
                std::snprintf(detail, sizeof(detail), "%d worlds; first \"%s\" (%s)", b->entryCount(), b->entryName(0), b->entryId(0));
                std::snprintf(what, sizeof(what), "the %s's Recent list", names[src]);
                check(b->entryCount() > 5, what, detail);
            }
            if (b->entryCount() > 0) {
                CGRect r;
                if (b->rowRect(0, &r)) ui_tap_rect(r);
                tick_until([b] { return b->previewShown(); }, 4000, "a preview");
                std::printf("[eden-browser] %s preview for \"%s\": %s\n", names[src], b->entryName(0),
                            b->previewShown() ? "shown" : "none (many worlds have none)");
            }
        }
        // One real download, chosen to be SMALL: the archive is the only source that publishes
        // sizes, and the first entry of a server list can be gigabytes (on 2026-10-07 the current
        // server's first Featured world inflated to 1.88 GB). HTTPS + a zip, end to end.
        ui_tap_rect(b->tabRect(WorldBrowser::SRC_ARCHIVE));
        tick(3);
        browser_wait_list(b);
        int pick = -1;
        double best = 1e18;
        for (int i = 0; i < b->entryCount(); ++i) {
            double v = 0;
            char unit[8] = {0};
            if (std::sscanf(b->entrySize(i), "%lf %7s", &v, unit) != 2 || v <= 0) continue;
            const double bytes = v * (!std::strcmp(unit, "GB") ? 1e9 : !std::strcmp(unit, "MB") ? 1e6 : !std::strcmp(unit, "KB") ? 1e3 : 1.0);
            if (bytes < best) { best = bytes; pick = i; }
        }
        const std::string id = pick >= 0 ? b->entryId(pick) : "";
        const std::string size = pick >= 0 ? b->entrySize(pick) : "";
        const int before = eden_menu_world_count();
        const bool ok = !id.empty() && browser_download(b, m, id.c_str());
        const int at = menu_index_of_file((id + ".eden").c_str());
        std::snprintf(detail, sizeof(detail), "id %s (%s), worlds %d -> %d, status \"%s\", listed as \"%s\"", id.c_str(),
                      size.c_str(), before, eden_menu_world_count(), b->statusText(), at >= 0 ? eden_menu_world_name(at) : "-");
        check(ok && at >= 0 && !m->showbrowser, "the smallest archive world downloads, unpacks and is listed", detail);
        check(!m->showhome, "a download opened from the home screen lands on My Worlds", "");   // I.7
        if (at >= 0) {
            eden_menu_select(at);
            tick(2);
            ui_tap_rect(m->controlRect("play"));
            const bool played = tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
            check(played, "the downloaded world loads", "");
            if (played) { tick(60); quit_to_menu(); tick(10); }
            const int again = menu_index_of_file((id + ".eden").c_str());
            if (again >= 0) { eden_menu_select(again); m->a_deleteConfirm(); }
        }
        if (m->showbrowser) { ui_tap_rect(b->controlRect("back")); tick(3); }
        return 0;
    }

    // --- a world to serve: play one, save it, read its bytes --------------------------------
    const char* name = g_opt.world.empty() ? "browser-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    tick(60);
    if (!quit_to_menu()) return 1;
    tick(20);
    int src = -1;
    for (int i = 0; i < eden_menu_world_count(); ++i) if (!std::strcmp(eden_menu_world_name(i), name)) src = i;
    std::vector<unsigned char> world;
    const bool haveWorld = src >= 0 && world_bytes(docs + "/" + eden_menu_world_file(src), &world);
    check(haveWorld && world.size() > 192, "a saved world to serve", "");
    if (!haveWorld) return 1;

    // --- fixtures -----------------------------------------------------------------------------
    const std::string fx = docs + "/.net-fixtures";
    const std::string arch = fx + "/hagg3.github.io/edenarchive/assets";
    write_bytes(arch + "/data/worlds.json", bytes_of(
        "[\n"
        " {\"filename\": \"1600000001.eden\", \"worldname\": \"Selftest Alpha\", \"publishdate\": \"2020-09-13\",\n"
        "  \"archivedate\": null, \"filesize\": \"1.0 MB\", \"author\": \"Tester\", \"tags\": [\"castle\", true, 7],\n"
        "  \"url\": \"/worlds/alpha/\"},\n"
        " {\"filename\": \"1600000002.eden\", \"worldname\": \"Selftest Beta \\u00dc\", \"publishdate\": null,\n"
        "  \"archivedate\": null, \"filesize\": null, \"author\": null, \"tags\": [], \"url\": \"/worlds/beta/\"},\n"
        " {\"filename\": \"../evil.eden\", \"worldname\": \"Path traversal\", \"tags\": []}\n"
        "]\n"));
    // Alpha: a deflate zip written the streaming way (bit 3, zero sizes in the local header).
    write_bytes(arch + "/worldfiles/1600000001/1600000001.eden.zip", zip_bytes("1600000001.eden", world, true, true));
    // Beta: the "double-compressed" case the archive's own site warns about — a stored zip
    // around a deflate zip around the world.
    write_bytes(arch + "/worldfiles/1600000002/1600000002.eden.zip",
                zip_bytes("1600000002.eden.zip", zip_bytes("1600000002.eden", world, true, false), false, false));
    std::vector<unsigned char> png;
    char exe[1024] = {0};
    const char* base = SDL_GetBasePath();
    std::snprintf(exe, sizeof(exe), "%sreport_flag.png", base ? base : "");
    const bool havePng = read_file(exe, &png);
    if (havePng) write_bytes(arch + "/worldfiles/1600000001/1600000001.eden.png", png);

    const std::string app2 = fx + "/app2.edengame.net", files2 = fx + "/files2.edengame.net";
    write_bytes(files2 + "/popularlist.txt", bytes_of("1600000101.eden\nServer Alpha.name\n"));
    write_bytes(app2 + "/list2.php@start=0&sort=2", bytes_of(
        "1600000101.eden\nServer Alpha.name\n\n1600000102.eden\nBroken World.name\n"));   // a stray blank line
    write_bytes(app2 + "/list2.php@start=2&sort=2", bytes_of(
        "1600000102.eden\nBroken World.name\n1600000103.eden\nServer Gamma.name\n"));    // one repeat, one new
    write_bytes(app2 + "/list2.php@start=3&sort=2", bytes_of(""));
    write_bytes(app2 + "/list2.php@search=gamma", bytes_of("1600000103.eden\nServer Gamma.name\n"));
    write_bytes(files2 + "/1600000101.eden", gzip_bytes(world));
    write_bytes(files2 + "/1600000102.eden", bytes_of("<html><body>Not found</body></html>\n"));
    // A gzip file of two members (what `cat a.gz b.gz` makes) — still one world once inflated.
    {
        const size_t half = world.size() / 2;
        std::vector<unsigned char> a(world.begin(), world.begin() + (long)half), c(world.begin() + (long)half, world.end());
        std::vector<unsigned char> two = gzip_bytes(a), tail = gzip_bytes(c);
        two.insert(two.end(), tail.begin(), tail.end());
        write_bytes(files2 + "/1600000103.eden", two);
    }
    eden_net_set_fixture_root(fx.c_str());

    // --- the menu offers it, and it opens on the archive --------------------------------------
    tick(3);
    check(m->browserOffered(), "the menu offers Get Worlds", "");
    ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    check(m->showbrowser && b->isOpen() && b->source() == WorldBrowser::SRC_ARCHIVE,
          "Get Worlds opens the browser on the archive tab", "");
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries: \"%s\", \"%s\"", b->entryCount(), b->entryName(0), b->entryName(1));
    check(b->entryCount() == 2 && !std::strcmp(b->entryName(0), "Selftest Alpha") &&
          !std::strcmp(b->entryName(1), "Selftest Beta \xC3\x9C"),
          "the manifest parses (\\u escape, odd tags) and drops a bad id", detail);

    // The archive filters as you type: by name, then by a tag.
    ui_tap_rect(b->controlRect("search"));
    tick(2);
    push_text("beta");
    tick(3);
    std::snprintf(detail, sizeof(detail), "%d: \"%s\"", b->entryCount(), b->entryName(0));
    check(b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000002"), "typing filters the archive by name", detail);
    for (int k = 0; k < 4; ++k) push_tap_key(SDL_SCANCODE_BACKSPACE);
    tick(2);
    push_text("castle");
    tick(3);
    std::snprintf(detail, sizeof(detail), "%d: \"%s\"", b->entryCount(), b->entryName(0));
    check(b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000001"), "...and by tag", detail);
    for (int k = 0; k < 6; ++k) push_tap_key(SDL_SCANCODE_BACKSPACE);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(3);
    check(b->entryCount() == 2, "clearing the field shows the whole list again", "");

    // A selection fetches its preview.
    CGRect r;
    if (b->rowRect(0, &r)) ui_tap_rect(r);
    tick_until([b] { return b->previewShown(); }, 1500, "the preview");
    check(!havePng || b->previewShown(), "selecting a world shows its preview", havePng ? "" : "(no PNG to serve)");

    // --- downloads: deflate zip, nested zip, no overwrite --------------------------------------
    struct Want { const char* id; const char* file; const char* what; int source; };
    const std::string X = world_ext();
    const std::string f1 = "1600000001" + X, f2 = "1600000002" + X, f1b = "1600000001-2" + X, f3 = "1600000103" + X;
    const Want wants[] = {
        { "1600000001", f1.c_str(),  "a streamed deflate zip imports as a world", WorldBrowser::SRC_ARCHIVE },
        { "1600000002", f2.c_str(),  "a zip inside a zip imports as a world", WorldBrowser::SRC_ARCHIVE },
        { "1600000001", f1b.c_str(), "a second download never replaces the first", WorldBrowser::SRC_ARCHIVE },
    };
    std::vector<std::string> made;
    for (const Want& w : wants) {
        if (!m->showbrowser) { ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds")); tick(3); browser_wait_list(b); }
        const bool done = browser_download(b, m, w.id);
        // The converted case (S.5) runs a time slice per frame; wait for it to hand back.
        tick_until([m, b] { return !m->showbrowser || !b->busy(); }, 6000, "the conversion");
        const int at = menu_index_of_file(w.file);
        std::vector<unsigned char> got;
        const bool same = world_bytes(docs + "/" + w.file, &got) && got == world;
        std::snprintf(detail, sizeof(detail), "%s: listed %d selected %d same-bytes %d browser %d status \"%s\"", w.file,
                      at, eden_menu_selected_index(), same ? 1 : 0, m->showbrowser ? 1 : 0, b->statusText());
        check(done && at >= 0 && eden_menu_selected_index() == at && same && !m->showbrowser, w.what, detail);
        if (at >= 0) made.push_back(w.file);
    }
    {
        // S.5 plan §4: the download is converted while it inflates and is gone after the commit.
        bool stray = false;
        for (const char* f : { "1600000001.eden", "1600000002.eden", "1600000001-2.eden", "1600000001.eden.zip", "1600000002.eden.zip" })
            if (FILE* t = std::fopen((docs + "/" + f).c_str(), "rb")) { std::fclose(t); stray = true; }
        check(!stray, "converted downloads leave no .eden and no download behind", "");
    }
    {
        const int at = menu_index_of_file(f1.c_str());
        std::snprintf(detail, sizeof(detail), "\"%s\"", at >= 0 ? eden_menu_world_name(at) : "-");
        check(at >= 0 && !std::strcmp(eden_menu_world_name(at), name), "a downloaded world is named from its own header", detail);
    }

    // --- the current server: Featured, Recent + More, Search, gzip, a bad payload -------------
    ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    ui_tap_rect(b->tabRect(WorldBrowser::SRC_CURRENT));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "source %d mode %d, %d entries", b->source(), b->mode(), b->entryCount());
    check(b->source() == WorldBrowser::SRC_CURRENT && b->mode() == WorldBrowser::MODE_FEATURED && b->entryCount() == 1,
          "the current server's tab opens on Featured", detail);
    ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries", b->entryCount());
    check(b->mode() == WorldBrowser::MODE_RECENT && b->entryCount() == 2, "Recent lists pairs past a stray blank line", detail);
    ui_tap_rect(b->controlRect("more"));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries", b->entryCount());
    check(b->entryCount() == 3, "More appends the next page and skips the repeat", detail);
    ui_tap_rect(b->controlRect("search"));
    tick(2);
    push_text("gamma");
    tick(2);
    push_tap_key(SDL_SCANCODE_RETURN);
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "mode %d, %d: \"%s\"", b->mode(), b->entryCount(), b->entryName(0));
    check(b->mode() == WorldBrowser::MODE_SEARCH && b->entryCount() == 1 && !std::strcmp(b->entryId(0), "1600000103"),
          "Return runs a server search", detail);
    {
        const bool done = browser_download(b, m, "1600000103");
        tick_until([m, b] { return !m->showbrowser || !b->busy(); }, 6000, "the conversion");
        std::vector<unsigned char> got;
        const bool same = world_bytes(docs + "/" + f3, &got) && got == world;
        std::snprintf(detail, sizeof(detail), "listed %d same-bytes %d status \"%s\"", menu_index_of_file(f3.c_str()),
                      same ? 1 : 0, b->statusText());
        check(done && same && !m->showbrowser, "a two-member gzip from the server imports as a world", detail);
        if (same) made.push_back(f3);
    }
    ui_menu_to(true); ui_tap_rect(m->controlRect("getworlds"));
    tick(3);
    check(b->source() == WorldBrowser::SRC_CURRENT && b->mode() == WorldBrowser::MODE_SEARCH,
          "the browser reopens where it was", "");
    ui_tap_rect(b->modeRect(WorldBrowser::MODE_RECENT));
    tick(3);
    browser_wait_list(b);
    {
        const int before = eden_menu_world_count();
        browser_download(b, m, "1600000102");
        std::vector<unsigned char> got;
        const bool none = !read_file(docs + "/1600000102.eden", &got);
        std::snprintf(detail, sizeof(detail), "status \"%s\", worlds %d -> %d, browser %d", b->statusText(), before,
                      eden_menu_world_count(), m->showbrowser ? 1 : 0);
        check(std::strstr(b->statusText(), "not an Eden world") && none && eden_menu_world_count() == before && m->showbrowser,
              "an HTML page served as a world is refused", detail);
    }

    // --- the legacy server: nothing answers -----------------------------------------------------
    ui_tap_rect(b->tabRect(WorldBrowser::SRC_LEGACY));
    tick(3);
    browser_wait_list(b);
    std::snprintf(detail, sizeof(detail), "%d entries, status \"%s\"", b->entryCount(), b->statusText());
    check(b->source() == WorldBrowser::SRC_LEGACY && b->entryCount() == 0 && std::strstr(b->statusText(), "Couldn't reach"),
          "an unreachable server says so", detail);
    ui_tap_rect(b->controlRect("back"));
    tick(3);
    check(!m->showbrowser, "Back returns to the menu", "");

    // --- a downloaded world plays -------------------------------------------------------------
    {
        const int at = menu_index_of_file(f2.c_str());
        if (at >= 0) eden_menu_select(at);
        check(m->showhome, "Get Worlds' Back returns to the home screen it was opened from", "");   // I.7
        ui_menu_to(false);
        tick(2);
        ui_tap_rect(m->controlRect("play"));
        const bool played = at >= 0 && tick_until([] { return game_mode() == 1; }, 60000, "game_mode == PLAY");
        check(played, "a downloaded world loads and plays", "");
        if (played) { tick(60); quit_to_menu(); tick(20); }
    }

    // --- Share (S.5 / S.5c): export to Exports/ as .eden.gz, and the upload, offline -----------
    {
        const std::string srcFile = docs + "/" + eden_menu_world_file(src);
        eden_menu_select(src);
        tick(2);
        WorldNode* node = m->selected_world;
        check(node && m->shareShown(), "the menu offers Share on a selected world", "");
        const bool started = node && WorldShare::startExport(node, emod::X_OWN, emod::SIGNS_KEEP);
        tick_until([] { return !WorldShare::busy(); }, 6000, "the export");
        std::string out = WorldShare::lastResult();
        const size_t at = out.find("Exports/");
        std::string path = at == std::string::npos ? std::string() : docs + "/" + out.substr(at, out.find(" (", at) - at);
        std::vector<unsigned char> gz;
        const bool same = !path.empty() && read_file(path, &gz) && gunzip_bytes(gz) == world;
        std::snprintf(detail, sizeof(detail), "\"%s\"", out.c_str());
        check(started && out.compare(0, 4, "ok: ") == 0 && same, "Share > Export writes the world as Exports/<name>.eden.gz", detail);
        if (!path.empty()) { std::remove(path.c_str()); SDL_RemovePath((docs + "/Exports").c_str()); }

        // Upload: refused with no preview, then the multipart POST to the fixture upload2.php.
        write_bytes(app2 + "/upload2.php", bytes_of("YES"));
        const bool refused = node && !WorldShare::startUpload(node, emod::UPLOAD_CURRENT, emod::X_OWN, emod::SIGNS_KEEP) &&
                             std::strstr(WorldShare::lastResult(), "failed");
        check(refused, "an upload without a preview PNG is refused", WorldShare::lastResult());
        if (havePng) write_bytes(srcFile + ".png", png);
        const bool up = node && WorldShare::startUpload(node, emod::UPLOAD_CURRENT, emod::X_OWN, emod::SIGNS_KEEP);
        tick_until([] { return !WorldShare::busy(); }, 6000, "the upload");
        std::vector<unsigned char> posted, ignored;
        const bool gotPost = read_file(app2 + "/upload2.php.posted", &posted);
        g_fixtureFiles.push_back(app2 + "/upload2.php.posted");
        const std::string head = std::string("--") + emod::UPLOAD_BOUNDARY + "\r\nContent-Disposition: form-data; name=\"uploaded\"; filename=\"file.bin\"\r\n\r\n";
        const bool shape = gotPost && posted.size() > head.size() && std::memcmp(posted.data(), head.data(), head.size()) == 0 &&
                           posted[head.size()] == 0x1f && posted[head.size() + 1] == 0x8b;
        const bool bodyGone = !read_file(srcFile + ".upload-body", &ignored);
        std::snprintf(detail, sizeof(detail), "\"%s\", posted %zu B, multipart+gzip %d, temp body gone %d", WorldShare::lastResult(),
                      posted.size(), shape ? 1 : 0, bodyGone ? 1 : 0);
        check(!havePng || (up && !std::strcmp(WorldShare::lastResult(), "ok: Uploaded") && shape && bodyGone),
              "Share > Upload POSTs the pinned upload2.php multipart and reads YES", detail);
        std::remove((srcFile + ".png").c_str());
    }

    // --- clean up: the downloads (through the menu's own delete), the fixtures, the world -------
    eden_net_set_fixture_root("");
    made.push_back(eden_menu_world_file(src));
    for (const std::string& f : made) {
        const int at = menu_index_of_file(f.c_str());
        if (at < 0) continue;
        eden_menu_select(at);
        m->a_deleteConfirm();
    }
    // Files, then their now-empty directories, deepest first (SDL_RemovePath only takes an empty one).
    std::vector<std::string> dirs;
    for (const std::string& f : g_fixtureFiles) {
        std::remove(f.c_str());
        for (std::string d = f.substr(0, f.find_last_of('/')); d.size() > docs.size(); d = d.substr(0, d.find_last_of('/')))
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& d : dirs) SDL_RemovePath(d.c_str());
    return 0;
}

int run_browser_selftest(bool live) {
    g_selftestTag = "eden-browser";
    g_tickInput = true;
    tick(120);
    const int rc = run_browser_selftest_body(live);
    std::printf("[eden-browser] %s (%d failure(s))\n", (rc || g_selftestFailures) ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return (rc || g_selftestFailures) ? 1 : 0;
}

int run_keybind_selftest() {
    g_selftestTag = "eden-keybind";
    g_tickInput = true;
    const char* name = g_opt.world.empty() ? "keybind-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();
    tick(240);

    char detail[220];

    // --- the model resolves, and its codes are the scancodes this platform reports -------------
    const int miForward = eden_keybind_index("moveForward");
    const int miFlyDown = eden_keybind_index("flyDown");
    const int miCrouch  = eden_keybind_index("crouch");
    std::snprintf(detail, sizeof(detail), "%d rows; moveForward=%d flyDown=%d crouch=%d",
                  eden_keybind_count(), miForward, miFlyDown, miCrouch);
    check(miForward >= 0 && miFlyDown >= 0 && miCrouch >= 0,
          "the model resolves the actions by name", detail);
    if (miForward < 0) { std::printf("[eden-keybind] FAILURES (%d)\n", ++g_selftestFailures); return 1; }

    eden_keybind_reset_all();
    std::snprintf(detail, sizeof(detail), "moveForward default %d (%s), SDL_SCANCODE_W is %d",
                  eden_keybind_default(miForward), eden_keybind_code_name(eden_keybind_default(miForward)),
                  (int)SDL_SCANCODE_W);
    check(eden_keybind_default(miForward) == (int)SDL_SCANCODE_W,
          "a binding IS the SDL scancode, not a parallel numbering", detail);

    // --- a default double-binding fires both of its actions ------------------------------------
    {
        int hits = 0, seen[4] = {-1, -1, -1, -1};
        for (int mi = eden_keybind_action_after(-1, (int)SDL_SCANCODE_LCTRL);
             mi >= 0 && hits < 4;
             mi = eden_keybind_action_after(mi, (int)SDL_SCANCODE_LCTRL))
            seen[hits++] = mi;
        std::snprintf(detail, sizeof(detail), "Ctrl -> %d action(s): %d,%d", hits, seen[0], seen[1]);
        check(hits == 2 && ((seen[0] == miFlyDown && seen[1] == miCrouch) ||
                            (seen[0] == miCrouch  && seen[1] == miFlyDown)),
              "one key still drives both flyDown and crouch", detail);
    }

    // --- the rebind actually moves the behaviour ----------------------------------------------
    // Same measurement --input-selftest uses (rise clear, walk, measure XZ), reused verbatim so
    // "did the player move" means the same thing in both — including the retry, since the flake
    // it retires (spawning face-first into a hill) is a property of the world, not of the test.
    auto walk_with = [&](SDL_Scancode sc, float* outDist) {
        rise_until_clear(1800);
        // SETTLE FIRST. The player is flying by this point (rise_until_clear turns fly on), and
        // flying has almost no drag — so the velocity left over from the PREVIOUS measurement
        // carries into the next one. The first version of this test measured 17 blocks of pure
        // coast and read it as "W still works", which is the wrong conclusion drawn from a real
        // number: the key was already unbound. 120 idle ticks is where the drift stops changing.
        tick(120);
        PlayerState before = player_state();
        push_key(sc, true);
        tick(300);
        push_key(sc, false);
        tick(10);
        PlayerState after = player_state();
        const float dx = after.x - before.x, dz = after.z - before.z;
        *outDist = std::sqrt(dx * dx + dz * dz);
    };

    // Establish that the DEFAULT key moves the player at all before rebinding — otherwise a
    // "T moves, W does not" result below could just as well mean the world is in the way.
    float distW = 0.0f;
    walk_with(SDL_SCANCODE_W, &distW);
    if (distW <= 0.5f) { push_mouse_motion(1800.0f, 0.0f); tick(20); walk_with(SDL_SCANCODE_W, &distW); }
    std::snprintf(detail, sizeof(detail), "moved %.2f blocks on the default binding", distW);
    check(distW > 0.5f, "W moves the player before any rebind", detail);

    eden_keybind_set(miForward, (int)SDL_SCANCODE_T);
    std::snprintf(detail, sizeof(detail), "moveForward -> %d (%s)",
                  eden_keybind_get(miForward), eden_keybind_code_name(eden_keybind_get(miForward)));
    check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_T, "the rebind is stored", detail);

    float distT = 0.0f;
    walk_with(SDL_SCANCODE_T, &distT);
    std::snprintf(detail, sizeof(detail), "moved %.2f blocks on the NEW binding", distT);
    check(distT > 0.5f, "T moves the player after the rebind", detail);

    // A CONTROL, not an absolute threshold. "Did not move" is not a number this harness can
    // assert directly — even settled, a flying player drifts a little, and how much depends on the
    // world. So measure a key that is bound to NOTHING (P, in no row of the table) and require
    // the now-unbound W to be no better than it. That is the claim the test actually wants, and
    // unlike a fixed epsilon it cannot be tuned into passing.
    float distControl = 0.0f, distWAfter = 0.0f;
    walk_with(SDL_SCANCODE_P, &distControl);
    walk_with(SDL_SCANCODE_W, &distWAfter);
    std::snprintf(detail, sizeof(detail), "W %.2f blocks vs %.2f for an unbound key",
                  distWAfter, distControl);
    check(distWAfter <= distControl + 1.0f && distWAfter < distT * 0.25f,
          "W is no more effective than an unbound key after the rebind", detail);

    // --- capture swallows the key instead of playing it ---------------------------------------
    // The keybinds screen arms a capture; the very next key must rebind and must NOT also be
    // dispatched as gameplay. Driven through the model rather than the screen because the screen
    // needs a window and this mode is headless — the ordering it protects (capture ahead of every
    // dispatch, Input_native.cpp's KEY_DOWN) is the part that can silently regress.
    {
        eden_keybind_capture_begin(miForward);
        check(eden_keybind_capture_active() == miForward, "capture arms", "");
        const int consumed = eden_keybind_capture_feed((int)SDL_SCANCODE_Y);
        std::snprintf(detail, sizeof(detail), "consumed=%d, moveForward now %d (%s)",
                      consumed, eden_keybind_get(miForward),
                      eden_keybind_code_name(eden_keybind_get(miForward)));
        check(consumed != 0 && eden_keybind_get(miForward) == (int)SDL_SCANCODE_Y &&
              eden_keybind_capture_active() < 0,
              "a captured key rebinds and is consumed", detail);

        // Escape CANCELS — and must leave the binding alone. This is the one key the screen
        // deliberately cannot bind to, and getting it wrong strands a player in a modal.
        eden_keybind_capture_begin(miForward);
        eden_keybind_capture_feed((int)SDL_SCANCODE_ESCAPE);
        std::snprintf(detail, sizeof(detail), "moveForward still %d", eden_keybind_get(miForward));
        check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_Y && eden_keybind_capture_active() < 0,
              "Escape cancels a capture without rebinding", detail);
    }

    // --- reset puts it back -------------------------------------------------------------------
    eden_keybind_reset_all();
    std::snprintf(detail, sizeof(detail), "moveForward back to %d (%s)",
                  eden_keybind_get(miForward), eden_keybind_code_name(eden_keybind_get(miForward)));
    check(eden_keybind_get(miForward) == (int)SDL_SCANCODE_W, "reset restores the defaults", detail);

    std::printf("[eden-keybind] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_input_selftest() {
    g_tickInput = true;
    const char* name = g_opt.world.empty() ? "input-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    teleport_for_movement();   // the map CENTRE, not --at's corner-clamped default
    tick(240);   // let the window fill and the player settle on the ground

    char detail[160];

    // --- movement -----------------------------------------------------------------------------
    // The assertion is on the XZ plane, so it holds whether the player is flying or walking.
    {
        // Rise clear of the terrain first — a player standing INSIDE a hill cannot walk, which is
        // a correct engine behaviour that reads as "input is broken" from here. rise_until_clear()
        // explains why a fixed frame count was not enough and what it cost.
        rise_until_clear(1800);
        check(eden_get_fly_mode() != 0, "V toggles fly mode", "");
        check_clear_of_map_border("the player is clear of the map border");

        // ...AND THEN, IF SOMETHING IS STILL IN THE WAY, TURN AROUND AND TRY AGAIN.
        // rise_until_clear() gets the player out of the ground; it cannot get a hill, a tree or a
        // cliff out of the direction the spawn happens to face, and there is no reason it should —
        // "W is wired to the movement axes" is what this asserts, and which way the world's
        // scenery lies is not part of that. Without the retry this is a ~1-in-6 flake (STATUS
        // §2.0 recorded it as "W reports 0.00 on ~1 run in 6" and credited rise_until_clear with
        // retiring it; a CI run measured 0.38 blocks against a 0.5 threshold, so it had not).
        // A 180° turn is the smallest change that keeps the assertion exactly what it was.
        auto walk_forward = [&](float* outDx, float* outDz) {
            PlayerState before = player_state();
            push_key(SDL_SCANCODE_W, true);
            tick(300);
            push_key(SDL_SCANCODE_W, false);
            tick(10);
            PlayerState after = player_state();
            *outDx = after.x - before.x;
            *outDz = after.z - before.z;
            return std::sqrt(*outDx * *outDx + *outDz * *outDz);
        };

        float dx = 0.0f, dz = 0.0f;
        float dist = walk_forward(&dx, &dz);
        if (dist <= 0.5f) {
            // Half a turn. push_mouse_motion's units are the same ones the look check uses; the
            // engine's yaw rate makes 1800 px a bit over half a revolution, and "roughly opposite"
            // is all this needs.
            push_mouse_motion(1800.0f, 0.0f);
            tick(20);
            rise_until_clear(600);
            dist = walk_forward(&dx, &dz);
        }
        // 300 scripted frames is ~0.75 s of engine time (tick() sleeps 2 ms), and steady-state
        // walk is ~5.25 blocks/s — so anything above half a block means the axes reached
        // Player::walk_force and the acceleration ramp did its job. The threshold is deliberately
        // loose: this asserts the WIRING, not the feel, which is a human's job.
        std::snprintf(detail, sizeof(detail), "moved %.2f blocks (%.2f,%.2f)", dist, dx, dz);
        check(dist > 0.5f, "W moves the player horizontally", detail);
    }

    // --- look ---------------------------------------------------------------------------------
    // Relative-mouse motion only reaches eden_apply_look_delta when the translator is in its
    // logical relative mode, which follows the engine's own eden_ui_wants_cursor() — so this also
    // asserts that the capture predicate resolved correctly for a player in the world.
    {
        PlayerState before = player_state();
        push_mouse_motion(120.0f, 0.0f);
        tick(20);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "yaw %.3f -> %.3f", before.yaw, after.yaw);
        check(after.yaw != before.yaw, "mouse motion turns the camera", detail);
    }

    // --- hotbar -------------------------------------------------------------------------------
    {
        PlayerState before = player_state();
        push_key(SDL_SCANCODE_3, true);
        push_key(SDL_SCANCODE_3, false);
        tick(20);
        PlayerState after = player_state();
        std::snprintf(detail, sizeof(detail), "blocktype %d -> %d", before.blocktype, after.blocktype);
        check(after.blocktype != before.blocktype, "digit key selects a hotbar slot", detail);
    }

    // --- the HUD tap path (Escape -> the in-game menu) ------------------------------------------
    // This is the one that proves the straddle-a-tick rule is honoured: a begin and end delivered
    // in the same frame produce no state change at all, silently. Last, because it leaves the HUD
    // in a different mode than everything above ran in.
    {
        const int before = eden_hud_in_menu();
        push_key(SDL_SCANCODE_ESCAPE, true);
        push_key(SDL_SCANCODE_ESCAPE, false);
        tick(30);
        const int after = eden_hud_in_menu();
        std::snprintf(detail, sizeof(detail), "in_menu %d -> %d", before, after);
        check(after != before, "Escape opens the in-game menu", detail);
    }

    // --- mine ---------------------------------------------------------------------------------
    // Put a block where the crosshair can definitely reach it, rather than trying to find one.
    //
    // The obvious version of this check — descend to the ground, look down, click — was written
    // first and was FLAKY, and the reason is worth recording because it is a real property of the
    // input path rather than test scaffolding. This fork ships FLY_MODE=true
    // (Classes/Player.mm:32), so a teleported player HOVERS instead of falling; whether a
    // fly-down descent actually reached the ground varied run to run, and when it did not, the
    // raycast missed. **A miss is sticky**: eden_click_begin() responds to a miss by setting
    // hud->mode = MODE_NONE, and every subsequent click then returns early on that same test. So
    // one unlucky frame turned into a permanently dead mouse button for the rest of the session.
    //
    // Placing a block two below the player's own feet (Terrain's argument order is (x, z, y) with
    // y vertical — CLAUDE.md convention #1) and pitching straight down removes the terrain from
    // the equation entirely, and leaves the check testing what it is for: does a held left button
    // reach the engine as a mine.
    {
        // WAIT FOR THE PLAYER TO COME TO REST FIRST. The teleport puts them at y=40 over unknown
        // terrain, and whether they hover there or fall depends on FLY_MODE — which
        // eden_settings_init() can turn off, because it is an engine-backed setting seeded from
        // SettingsMenu::properties. Placing the target slab relative to a player who is still
        // falling leaves it 30 blocks above them, which is exactly how this check failed on the
        // run right after settings loading was added. Gravity is deterministic; a fixed frame
        // count is not.
        float lastY = player_state().y;
        for (int i = 0; i < 40; ++i) {
            tick(60);
            const float y = player_state().y;
            if (std::fabs(y - lastY) < 0.05f) break;
            lastY = y;
        }
        tick(60);

        PlayerState p = player_state();
        // A 3x3 slab, not a single block. `eden_apply_look_delta` clamps pitch at -80 degrees
        // rather than -90, so a "straight down" crosshair is really 10 degrees off vertical and
        // lands a third of a block sideways — and which way depends on the player's yaw, which is
        // NOT deterministic across runs (it comes from where the world spawned them). A single
        // block under the feet was therefore hit about half the time. Nine covers the cone.
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
                eden_console_setblock((int)p.x + dx, (int)p.z + dz, (int)p.y - 2, 1);
        tick(90);                                  // let the chunk re-mesh with the new blocks
        push_mouse_motion(0.0f, 1200.0f);          // pitch fully down; eden_apply_look_delta clamps
        tick(30);
        char before[256];
        std::snprintf(before, sizeof(before), "%s", eden_debug_terrain_geometry());
        const int modeBefore = player_state().hudMode;
        // POLL, don't guess a duration. Mining is a timed action whose length depends on the
        // block's hardness, and a fixed hold is wrong in both directions: 180 frames was enough
        // for grass and not for stone, and 700 was enough two runs out of three. Waiting for the
        // observable outcome is both faster on average and not a coin flip.
        push_mouse_button(SDL_BUTTON_LEFT, true);
        bool mined = false;
        for (int i = 0; i < 30 && !mined; ++i) {
            tick(100);
            mined = std::strcmp(before, eden_debug_terrain_geometry()) != 0;
        }
        push_mouse_button(SDL_BUTTON_LEFT, false);
        tick(60);
        const char* after = eden_debug_terrain_geometry();
        PlayerState q = player_state();
        const bool broke = std::strcmp(before, after) != 0;

        // WHAT IS ASSERTED vs WHAT IS REPORTED, and why the line is drawn here. The gate is "the
        // click produced an observable change in the engine" — either hud->mode moved (only
        // eden_click_begin() writes MODE_MINE, and only it writes MODE_NONE on a raycast miss) or
        // terrain geometry changed. That is deterministic and it exercises the whole mouse path.
        //
        // What is NOT gated is which of those outcomes happened, because neither is under a
        // headless script's control: whether the block finishes BREAKING depends on its hardness,
        // on where the player came to rest, and on how much engine time the scripted pacing
        // accumulates; and the final mode depends on eden_ui_tick()'s tool-mode restore. Earlier
        // drafts gated on each in turn and each passed about half the time — which is worse than
        // no check at all, because a suite people learn to re-run until it goes green has stopped
        // being evidence.
        // REPORTED, NOT GATED — and that is a deliberate design decision, not a shrug.
        //
        // Every version of a pass/fail rule for this check turned out to be a coin flip: on the
        // block breaking (depends on hardness, on where the player came to rest, and on how much
        // engine time the scripted pacing accumulates), on hud->mode being MODE_MINE (eden_ui_tick's
        // tool-mode restore puts it back before this can read it), and on mode having changed at
        // all (same reason). A check that passes about half the time is worse than no check,
        // because a suite people learn to re-run until it goes green has stopped being evidence.
        //
        // The four checks above ARE gated, and between them they prove the whole translator:
        // continuous keys reach the player, relative mouse motion reaches the camera, a one-shot
        // key reaches the hotbar, and — the one that proves the straddle-a-tick rule — a synthetic
        // HUD tap reaches Hud::update. Whether a click *finishes* a mine is what a human playing
        // for thirty seconds settles, and Stage 2 owes that pass.
        std::printf("[eden-input] %-46s %s  hud_mode %d -> %d, y %.1f\n",
                    "left mouse click (reported, not gated)",
                    broke ? "block broke" : "block did not break this run",
                    modeBefore, q.hudMode, q.y);
    }

    std::printf("[eden-input] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// --background-selftest  (Phase N Stage 4.2)
// ---------------------------------------------------------------------------------------------
// The native half of save-on-background. The POLICY (when is a save safe?) is shared code and is
// gated on web by tools/headless-save-on-background-test.js -- there is no value in asserting it
// twice. What is native-only, and what this mode exists for, is the WIRING: that
// SDL_EVENT_WILL_ENTER_BACKGROUND actually reaches eden_app_will_background() through the event
// WATCH rather than the poll loop. That distinction is the whole point on iOS (SDL_events.h: these
// events "must be handled in a callback set with SDL_AddEventWatch()"), it is invisible in a code
// review, and pushing the event is the only way to find out.
//
// SDL_PushEvent invokes event watches synchronously as it enqueues, so this exercises the real
// path with no window and no OS involvement.
int run_background_selftest() {
    g_selftestTag = "eden-background";
    const char* name = g_opt.world.empty() ? "background-selftest" : g_opt.world.c_str();

    check(eden_app_will_background() == 1, "on the menu: refuses (NO_WORLD)", "");
    check(eden_app_background_save_count() == 0, "on the menu: nothing saved", "");

    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);

    // Without a real edit there is nothing for saveWorld() to write (saveColumn skips any column
    // whose `modified` flag is FALSE), so the size assertion below would pass on an empty file.
    // (x, z, y) with y vertical -- CLAUDE.md convention #1, same as --save-roundtrip.
    eden_console_setblock((int)g_opt.at[0] + 2, (int)g_opt.at[2] + 2, (int)g_opt.at[1], 1);
    tick(60);

    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
    ev.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);

    const int saves = eden_app_background_save_count();
    check(saves == 1, "SDL_EVENT_WILL_ENTER_BACKGROUND saved the open world",
          saves == 1 ? "" : "(the event watch did not reach eden_app_will_background)");
    check(eden_app_background_last_status() == 0, "...and it answered SAVED", "");

    // And the bytes are real. A save count is a claim about a call; a file on disk with a column
    // in it is the thing the row is actually about.
    char path[1024] = {0};
    for (int i = 0; i < eden_menu_world_count(); ++i) {
        if (std::strcmp(eden_menu_world_name(i), name) == 0) {
            std::snprintf(path, sizeof(path), "%s/%s",
                          eden_platform_documents_root(), eden_menu_world_file(i));
            break;
        }
    }
    long long bytes = -1;
    if (path[0]) {
        if (FILE* f = std::fopen(path, "rb")) {
            std::fseek(f, 0, SEEK_END);
            bytes = std::ftell(f);
            std::fclose(f);
        }
    }
    char detail[1200];
    std::snprintf(detail, sizeof(detail), "%s = %lld bytes", path[0] ? path : "(not found)", bytes);
    const size_t plen = std::strlen(path);
    if (plen > 5 && !std::strcmp(path + plen - 5, ".emod")) {
        // Stage S / S.4 (--world-format=emod): a compressed append log has no size floor worth
        // asserting, so ask the store itself -- a committed batch holding at least one column.
        emod::EdenWorldStore st;
        const bool ok = st.open(path, false) && st.columnCount() >= 1;
        std::snprintf(detail + std::strlen(detail), sizeof(detail) - std::strlen(detail),
                      ", %u column(s), %u commit(s)", (unsigned)st.columnCount(), (unsigned)st.lastSeq());
        check(ok, "the world file on disk holds at least one column", detail);
    } else {
        // 192-byte header + 200*60 creature block + at least one 32,768-byte column + a directory.
        check(bytes >= 192 + 200 * 60 + 32768, "the world file on disk holds at least one column", detail);
    }

    // Foreground and background again: the round trip must not wedge, and the second departure
    // must still save. On a phone this is the common case, not an edge case.
    SDL_zero(ev);
    ev.type = SDL_EVENT_DID_ENTER_FOREGROUND;
    ev.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);
    tick(30);
    SDL_zero(ev);
    ev.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
    ev.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&ev);
    check(eden_app_background_save_count() == 2, "foreground then background saves again", "");

    if (!quit_to_menu()) return 1;
    check(eden_app_will_background() == 1, "back on the menu: refuses again (NO_WORLD)", "");
    check(eden_app_background_save_count() == 2, "quitting to the menu counted no extra save", "");

    std::printf("[eden-background] %s (%d failure(s))\n",
                g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

int run_save_roundtrip() {
    const char* name = g_opt.world.empty() ? "stage1-roundtrip" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);

    // One deliberate edit, at a coordinate derived from the fixed origin, so the round-trip is
    // proving that WRITTEN data comes back rather than that an untouched file is unchanged.
    // Terrain's own argument order is (x, z, y) with y vertical — CLAUDE.md convention #1.
    const int bx = (int)g_opt.at[0] + 2, bz = (int)g_opt.at[2] + 2, by = (int)g_opt.at[1];
    eden_console_setblock(bx, bz, by, 1);
    tick(60);

    std::printf("[eden-stage1] presave.checksum %s\n", eden_debug_mesh_checksum());
    save_world();

    // Name the file so an outside script can diff the bytes against a web-written save. The
    // directory is the SEAM's now — Classes/FileManager.mm asks eden_platform_documents_root()
    // as of Stage 2, so this and the engine can no longer disagree. Through Stage 1 they could,
    // and this line printed Foundation's answer for exactly that reason.
    for (int i = 0; i < eden_menu_world_count(); ++i) {
        if (std::strcmp(eden_menu_world_name(i), name) == 0) {
            std::printf("[eden-stage1] savefile %s/%s\n",
                        eden_platform_documents_root(), eden_menu_world_file(i));
        }
    }

    if (!quit_to_menu()) return 1;
    tick(120);
    if (!open_world(name, g_opt.height)) return 1;
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    std::printf("[eden-stage1] postload.checksum %s\n", eden_debug_mesh_checksum());
    std::printf("[eden-stage1] postload.geometry %s\n", eden_debug_terrain_geometry());
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Interactive loop
// ---------------------------------------------------------------------------------------------

bool g_running = true;

void pump_events() {
    // NOT `if (headless) return;` — --input-selftest is headless and drives the translator through
    // SDL_PushEvent, so the queue has to be pumped even with no window. SDL_PollEvent is harmless
    // with the video subsystem uninitialised (it just never has anything to report), and the
    // window-event cases below are all null-guarded.
    // Headless still needs a live pump for the selftests: --input-selftest's synthetic keystrokes
    // and --gamepad-selftest's virtual pad both come back OUT of SDL_PollEvent, and the latter's
    // SDL_EVENT_GAMEPAD_ADDED is the only thing that hands the pad to the translator. `g_tickInput`
    // is exactly "a selftest is driving input", which is the condition that matters — keying off
    // the mode name meant adding a second selftest silently got no events.
    if (g_opt.headless && !g_tickInput) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                g_running = false;
                if (g_app) g_app->onPageHide();
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (g_app) g_app->onVisibilityHidden();
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (g_app) g_app->onVisibilityVisible();
                break;
            case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:   // I.2: rotation, island side, home bar
                eden_native_apply_display_mode();
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                // The new surface first, then the FIT. eden_set_drawable_size() clears the
                // letterbox origin (the box is the whole surface until something says otherwise)
                // and eden_native_apply_display_mode() re-derives the point space from the new
                // window shape and re-centres the box — the same two steps, in the same order,
                // that public/eden-viewport.js runs on a browser resize.
                eden_set_drawable_size(e.window.data1, e.window.data2);
                eden_native_apply_display_mode();
                break;
            default:
                break;
        }
        // Stage 2: keyboard/mouse -> the eden_* input exports (src/seam/Input_native.cpp). Given
        // every event, including the window ones above, because it is the translator's business
        // to decide what it cares about — and a `default:` that swallowed input was exactly the
        // shape of this file before Stage 2.
        eden_native_input_handle_event(e);
    }
}

// Default asset root: the repo this binary was built from. argv[0] is <repo>/native/build/eden,
// so two levels up is the tree that holds Eden.eden and media/ — the same layout web's
// --preload-file rules package into /bundle. An explicit --bundle= always wins.
std::string default_bundle(const char* argv0) {
    if (const char* env = getenv("EDEN_BUNDLE")) return env;
#if defined(EDEN_PLATFORM_IOS)
    // iOS: ASK NSBUNDLE, do not walk. There is no repo beside the executable on a device — the
    // assets are inside the .app, which for an iOS bundle IS `resourcePath` (unlike macOS, where
    // it is Contents/Resources). The walk below would in fact find them anyway, because an iOS
    // .app is flat and the executable sits in the same directory as Eden.eden — but "anyway" is
    // not a contract, and this is the call the engine itself makes for every model and texture
    // (Classes/World.mm LoadModels()). Ask the same source it asks.
    (void)argv0;
    @autoreleasepool {
        NSString* rp = [[NSBundle mainBundle] resourcePath];
        if (rp) return std::string([rp UTF8String]);
    }
    return ".";
#else
    std::string p = argv0 ? argv0 : "";
    // BOTH separators on Windows. The CRT hands back argv[0] with backslashes when the process is
    // launched by path (D:\...\eden.exe), so splitting on '/' alone found nothing, fell back to
    // ".", and then walked up from the CURRENT DIRECTORY instead of from the executable. In CI
    // that happened to land on the repo root and worked by luck; from a packaged zip launched
    // from anywhere else it would look for the assets beside the shell's cwd.
#if defined(_WIN32)
    size_t slash = p.find_last_of("/\\");
#else
    size_t slash = p.find_last_of('/');
#endif
    std::string dir = (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
    // Walk up looking for the marker file rather than counting directories, so the binary still
    // finds its assets when it is run from a different build tree or a symlink.
    for (int up = 0; up < 6; ++up) {
        struct stat st;
        if (stat((dir + "/Eden.emod").c_str(), &st) == 0) return dir;
        dir += "/..";
    }
    return ".";
#endif
}

std::string default_docs() {
    if (const char* env = getenv("EDEN_DOCUMENTS")) return env;
#if defined(_WIN32)
    // Phase N Stage 3.6. %APPDATA% is the roaming per-user application-data root — the Windows
    // equivalent of the reasoning behind the macOS choice below, and for the same reason: these
    // are the app's own store, not documents the player hands around.
    //
    // NOT SDL_GetPrefPath("Emod","Emod"), although the plan named that shape. It would give
    // %APPDATA%\Emod\Emod — an org/app pair this project does not have, since the organisation
    // and the application are the same thing — and it would disagree with where
    // src/shim/foundation/NSUserDefaults_native.mm already puts the prefs file. One "Emod"
    // directory holding `prefs` and `Documents\` is the layout, on all three platforms.
    if (const char* appdata = getenv("APPDATA")) {
        if (*appdata) return std::string(appdata) + "\\" EDEN_APP_DIR_NAME "\\Documents";
    }
    return "./emod-documents";
#else
    const char* home = getenv("HOME");
    if (!home || !*home) return "./eden-documents";
#if defined(EDEN_PLATFORM_IOS)
    // iOS: `<container>/Documents`, and NOT the Application Support choice the macOS branch
    // below argues for — because the argument that decided that one inverts here. On macOS
    // ~/Documents is the user's own folder and these files are the app's private store; inside
    // an iOS sandbox `Documents` IS the app's private store, and it is additionally the ONLY
    // directory the Files app and iTunes/Finder file sharing can expose (Info.plist's
    // UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace, both set — see native/ios/
    // Info.plist.in). A player who wants a world off the device has no Cmd-Shift-G here.
    //
    // It is also the directory iOS BACKS UP and does not purge, which Library/Caches is not.
    return std::string(home) + "/Documents";
#elif defined(__APPLE__)
    // Phase N Stage 2 decided this deliberately, which Stage 1 explicitly left open. It is where
    // macOS puts application data, and these files are the app's own store rather than documents
    // the player hands around: sharing a world goes through the engine's own export, not through
    // Finder. The cost, stated because it is real: ~/Library is hidden in Finder by default, so
    // "where is my world file" is Cmd-Shift-G rather than a double-click.
    // migrate_legacy_saves() below moves anything an earlier build wrote to ~/Documents.
    return std::string(home) + "/Library/Application Support/" EDEN_APP_DIR_NAME;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    return (xdg && *xdg) ? std::string(xdg) + "/" EDEN_APP_DIR_LOWER "/Documents"
                         : std::string(home) + "/.local/share/" EDEN_APP_DIR_LOWER "/Documents";
#endif
#endif
}

// One-time move of saves an earlier native build wrote before Stage 2 chose a save directory.
//
// TWO SOURCES, and the second one is the reason this is not a two-line function. Until this
// session Classes/FileManager.mm asked real Foundation for NSDocumentDirectory, which for this
// binary is ~/Documents — except macOS does not actually let an unsigned command-line tool write
// there. It REDIRECTS the writes into a per-binary sandbox container,
// ~/Library/Containers/<UUID>/Data/Documents, silently and with no error at any layer. So the
// worlds from every pre-Stage-2 session are sitting in a directory named after an opaque UUID
// that the OS is free to discard, which is a far better argument for Application Support than any
// of the tidiness ones: it is the difference between saves that exist and saves that are on
// borrowed time. (Found by looking for the files, not by reasoning: ~/Documents was empty after a
// save that had just printed ~/Documents as its destination.)
//
// Rules, deliberately conservative:
//   * gated on a MARKER FILE, not on "does the new directory already have worlds" — the scripted
//     modes each create a world, so a single harness run would otherwise satisfy that test and
//     the player's real saves would stay lost with nothing reporting it;
//   * only UUID-shaped container names are swept. A real sandboxed app's container is named after
//     its bundle id (it has dots); a bare UUID is the unsigned-binary redirect. `.eden` is
//     unambiguous enough on its own, but this keeps the scan out of other apps' data entirely;
//   * the name filter is EXACT, not a substring test. "anything containing .eden" was tried and
//     it is wrong: another of this machine's sandboxed tools keeps `MP_Flatland.eden.lz4`,
//     `signs_<n>.eden.dat` and `spacemap3_<n>.eden.raw` in ITS container, and the loose rule
//     swept eight of them out of it. A name must either end in ".eden" or be ".eden" plus one
//     suffix this engine actually produces — nothing else is ours to touch. Restored by hand;
//     the lesson is that a migration's match rule is a safety boundary, not a convenience;
//   * rename(), never copy: the specimen worlds here run to gigabytes, so a copy would need the
//     space twice and take minutes. A rename that fails is reported and skipped, never forced,
//     and nothing is ever deleted.
// Exactly the names this engine writes: "<world>.eden", and that same name plus one of the
// siblings FileManager/Util produce. Anything else is somebody else's file. See the note above.
bool is_eden_save_name(const std::string& n) {
    auto ends = [&](const char* suf) {
        const size_t k = std::strlen(suf);
        return n.size() > k && n.compare(n.size() - k, k, suf) == 0;
    };
    static const char* kSuffixes[] = {
        ".eden", ".eden.bak", ".eden.png", ".eden.zip", ".eden.bak.zip",
        ".eden.savetmp", ".eden.journal",
    };
    for (const char* suf : kSuffixes) if (ends(suf)) return true;
    return false;
}

void migrate_saves_from(const std::string& from, const std::string& to, int* moved) {
    DIR* d = opendir(from.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (is_eden_save_name(n)) names.push_back(n);
    }
    closedir(d);
    if (names.empty()) return;
    std::fprintf(stderr, "[eden] migrating %zu file(s) from %s\n", names.size(), from.c_str());
    for (const std::string& n : names) {
        const std::string src = from + "/" + n;
        const std::string dst = to + "/" + n;
        struct stat st;
        if (stat(dst.c_str(), &st) == 0) {
            std::fprintf(stderr, "[eden]   skipped %s (already present)\n", n.c_str());
            continue;
        }
        if (rename(src.c_str(), dst.c_str()) == 0) {
            std::fprintf(stderr, "[eden]   moved %s\n", n.c_str());
            (*moved)++;
        } else {
            std::fprintf(stderr, "[eden]   COULD NOT move %s (%s) — left where it is\n",
                         n.c_str(), std::strerror(errno));
        }
    }
}

// "Looks like a bare UUID" — 36 chars, hex and dashes only. Excludes every bundle-id container.
bool is_uuid_name(const char* n) {
    if (!n || std::strlen(n) != 36) return false;
    for (const char* p = n; *p; ++p) {
        if (!(std::isxdigit((unsigned char)*p) || *p == '-')) return false;
    }
    return true;
}

// THE "Eden" -> "Emod" MOVE (2026-09-06), and it runs on all three desktop platforms because all
// three had the bug. Until this commit every desktop leg wrote its worlds into an application-data
// directory named after the SHIPPED GAME — `~/Library/Application Support/Eden`,
// `%APPDATA%\Eden`, `~/.local/share/eden` — so a community fork's saves landed in the same
// directory a user's official Eden files would use. See native/src/eden_app_identity.h for the
// rule; this is the one-time move of anything already written there.
//
// Same machinery as the Apple container rescue below (exact-suffix match, rename never copy,
// never overwrite, never delete), for the same reasons — the specimen worlds here run to
// gigabytes and a match rule is a safety boundary.
void migrate_from_legacy_app_dir(const std::string& newDocs, int* moved) {
#if defined(_WIN32)
    const char* appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return;
    const std::string oldDir = std::string(appdata) + "\\" EDEN_APP_DIR_NAME_LEGACY "\\Documents";
#else
    const char* home = getenv("HOME");
    if (!home || !*home) return;
#if defined(__APPLE__)
    const std::string oldDir = std::string(home) + "/Library/Application Support/"
                               EDEN_APP_DIR_NAME_LEGACY;
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    const std::string oldDir = (xdg && *xdg)
        ? std::string(xdg) + "/" EDEN_APP_DIR_LOWER_LEGACY "/Documents"
        : std::string(home) + "/.local/share/" EDEN_APP_DIR_LOWER_LEGACY "/Documents";
#endif
#endif
    if (oldDir == newDocs) return;
    migrate_saves_from(oldDir, newDocs, moved);
}

// APPLE ONLY, and that is the whole point of it: it exists to rescue saves out of the sandbox
// container macOS silently redirected an unsigned CLI tool's ~/Documents writes into. Linux and
// Windows have no such redirect and no pre-Stage-2 saves to rescue — this port has never run on
// either — so there is nothing for it to find and a container scan there would be superstition.
// (The "Eden" -> "Emod" move above is separate and DOES run on all three.)
#if defined(__APPLE__) && !defined(EDEN_PLATFORM_IOS)
void migrate_legacy_saves(const std::string& newDocs) {
    const char* home = getenv("HOME");
    if (!home || !*home) return;

    const std::string marker = newDocs + "/.migrated-from-documents";
    {
        struct stat st;
        if (stat(marker.c_str(), &st) == 0) return;
    }
    // Written even when nothing moves: a fresh install has no legacy saves and should not re-scan
    // every container on every launch forever.
    if (FILE* m = std::fopen(marker.c_str(), "w")) { std::fputs("1\n", m); std::fclose(m); }

    int moved = 0;
    migrate_from_legacy_app_dir(newDocs, &moved);
    const std::string oldDocs = std::string(home) + "/Documents";
    if (oldDocs != newDocs) migrate_saves_from(oldDocs, newDocs, &moved);

    const std::string containers = std::string(home) + "/Library/Containers";
    if (DIR* cd = opendir(containers.c_str())) {
        while (struct dirent* e = readdir(cd)) {
            if (!is_uuid_name(e->d_name)) continue;
            migrate_saves_from(containers + "/" + e->d_name + "/Data/Documents", newDocs, &moved);
        }
        closedir(cd);
    }
    if (moved) std::fprintf(stderr, "[eden] migration complete: %d file(s) now in %s\n",
                            moved, newDocs.c_str());
}
#else
// Off Apple there is no container to rescue from, but the "Eden" -> "Emod" move still applies —
// Windows and Linux both wrote into an Eden-named directory until 2026-09-06. Same marker file,
// so a fresh install does not re-scan forever.
void migrate_legacy_saves(const std::string& newDocs) {
    const std::string marker = newDocs + "/.migrated-from-documents";
    {
        struct stat st;
        if (stat(marker.c_str(), &st) == 0) return;
    }
    if (FILE* m = std::fopen(marker.c_str(), "w")) { std::fputs("1\n", m); std::fclose(m); }
    int moved = 0;
    migrate_from_legacy_app_dir(newDocs, &moved);
    if (moved) std::fprintf(stderr, "[eden] migration complete: %d file(s) now in %s\n",
                            moved, newDocs.c_str());
}
#endif

// Phase N Stage 3.6: SDL's, not a hand-rolled mkdir(2) walk. SDL_CreateDirectory already creates
// missing parents, and — the reason this changed — mingw's mkdir() takes ONE argument where
// POSIX's takes two, so the walk below was a compile error on Windows rather than a portability
// footnote. Same substitution as src/shim/foundation/NSUserDefaults_native.mm.
void mkdirs(const std::string& path) {
    SDL_CreateDirectory(path.c_str());
}

bool starts_with(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

}  // namespace

// ONE OPTION, PARSED FROM ONE STRING — because on a phone there is no argv (Phase N Stage 4.1).
// Every mode this harness has is selected by a command-line flag, and a home-screen tap supplies
// none; `xcrun simctl launch` does forward trailing arguments, but that is a detail of one tool
// on one host and the gate that depends on it should not be the only way to drive an iOS build.
// EDEN_ARGS is the seam: whatever it holds is parsed exactly as argv would be, AFTER argv, so a
// real command line still wins on the desktop. In CI it arrives as SIMCTL_CHILD_EDEN_ARGS.
static int parse_one_arg(const char* a);
static int eden_main_after_args(int argc, char** argv);
static int parse_arg_string(const char* text);

// X.2: a bad option must END the process. On iOS, returning from main() does not necessarily
// terminate the app (UIKit's run loop can outlive SDL_main), so a typo in eden-args.txt used to
// leave the harness sitting at the menu until its timeout instead of failing fast with rc 2.
static int arg_error_exit(int rc) {
    std::fflush(stdout); std::fflush(stderr);
    std::exit(rc);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (int rc = parse_one_arg(argv[i])) return arg_error_exit(rc);
    }
    // ...and EDEN_LOG, for the same reason one step further on: a phone has no terminal either.
    // An iOS app's stdout goes to a pty only if something is holding one open on the other end
    // (`simctl launch --console-pty`), and if that side ever fails to deliver — it did, on the
    // first CI run of this job — there is no output at all and no way to tell a hang from a crash.
    // A file in the app's own container is retrievable afterwards, from a simulator with
    // `simctl get_app_container` and from a device with the Files app, because the save directory
    // is user-visible (see default_docs()'s iOS branch).
    //
    // stderr is dup2'd onto the SAME descriptor rather than opened separately, so the interleaving
    // is preserved: nearly every diagnostic in this port is fprintf(stderr) and the checksums are
    // printf(stdout), and reading them out of order would make a log worse than no log.
    // ON iOS THE LOG IS ALWAYS WRITTEN, and to a place a player can reach. A tapped app has no
    // terminal, no environment and no way to be attached to — the first device pass came back as
    // four sentences of symptoms with no numbers behind them, and the numbers (which display
    // profile, what point space, what letterbox, what GL) are exactly what decides the diagnosis.
    // `Documents` is the user-visible container directory (UIFileSharingEnabled, see
    // native/ios/Info.plist.in), so the file can be handed straight to the Files app and mailed.
    // Overwritten each launch: it is the LAST run, not a history, and a growing log on a phone is
    // its own bug.
    std::string iosLog;
#if defined(EDEN_PLATFORM_IOS)
    if (!getenv("EDEN_LOG")) {
        const std::string d = g_opt.docs.empty() ? default_docs() : g_opt.docs;
        mkdirs(d);
        iosLog = d + "/emod-last-run.log";
        setenv("EDEN_LOG", iosLog.c_str(), 1);
    }
#endif
    if (const char* logPath = getenv("EDEN_LOG")) {
        if (freopen(logPath, "w", stdout)) {
            dup2(fileno(stdout), fileno(stderr));
            setvbuf(stdout, nullptr, _IOLBF, 0);
            setvbuf(stderr, nullptr, _IOLBF, 0);
            std::fprintf(stderr, "[eden] logging to %s\n", logPath);
        }
    }
    if (const char* extra = getenv("EDEN_ARGS")) {
        std::fprintf(stderr, "[eden] EDEN_ARGS=%s\n", extra);
        if (int rc = parse_arg_string(extra)) return arg_error_exit(rc);
    }
#if defined(EDEN_PLATFORM_IOS)
    // THE ONE-SHOT ARGS FILE (2026-10-04) — EDEN_ARGS for a device driven over SSH. A SpringBoard
    // launch (`uiopen --bundleid`) carries no environment, so `Documents/eden-args.txt` is the only
    // way to put a jailbroken iPad into `--shot` or a selftest without Xcode. It is RENAMED to
    // eden-args.last.txt before it is parsed, so it applies to exactly one launch: a leftover file
    // must never trap the next home-screen tap in a harness mode. Parsed after EDEN_ARGS, same parser.
    {
        const std::string d = g_opt.docs.empty() ? default_docs() : g_opt.docs;
        const std::string f = d + "/eden-args.txt", used = d + "/eden-args.last.txt";
        if (std::rename(f.c_str(), used.c_str()) == 0) {
            std::string text;
            if (FILE* fp = std::fopen(used.c_str(), "r")) {
                char buf[1024];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
                std::fclose(fp);
            }
            std::fprintf(stderr, "[eden] eden-args.txt=%s\n", text.c_str());
            if (int rc = parse_arg_string(text.c_str())) return arg_error_exit(rc);
            // --shot's captures land beside the log, where scp can reach them; the app's cwd
            // is not writable.
            if (g_opt.mode && !std::strcmp(g_opt.mode, "shot") && g_opt.shot.empty())
                g_opt.shot = d + "/eden-shot";
        }
    }
#endif
    return eden_main_after_args(argc, argv);
}

// Whitespace-separated options, each parsed exactly as one argv entry. X.2: quote-aware like a
// shell word split -- `--world="3 bit adder"` or `--world='3 bit adder'` is ONE option whose value
// is `3 bit adder` (the quotes themselves are dropped; no escapes). An unterminated quote is an
// error (rc 2), not a silently swallowed rest-of-file.
// The pieces are remembered (g_string_args) because the world-file tools re-read their own options
// from an argv: without them an EDEN_ARGS / eden-args.txt launch (the only way to drive the iPad)
// reached `eden_world_tool_main` with no arguments and "nothing to do".
static std::vector<std::string> g_string_args;
static int parse_arg_string(const char* text) {
    std::string acc;
    bool have = false;    // an empty quoted value ("") is still an argument
    char quote = 0;
    for (const char* p = text; ; ++p) {
        if (quote) {
            if (!*p) { std::fprintf(stderr, "eden: unterminated %c quote in arguments: %s\n", quote, text); return 2; }
            if (*p == quote) quote = 0; else acc += *p;
            continue;
        }
        if (*p == '"' || *p == '\'') { quote = *p; have = true; continue; }
        if (*p && !std::isspace((unsigned char)*p)) { acc += *p; have = true; continue; }
        if (have) { g_string_args.push_back(acc); if (int rc = parse_one_arg(acc.c_str())) return rc; acc.clear(); have = false; }
        if (!*p) break;
    }
    return 0;
}

// The body of the loop above, verbatim from when it was one. Split out only so argv and EDEN_ARGS
// cannot drift into two parsers.
static int parse_one_arg(const char* a) {
    {
        if (!std::strcmp(a, "--headless"))            g_opt.headless = true;
        else if (!std::strcmp(a, "--p1-gate"))        { g_opt.mode = "p1-gate";  g_opt.headless = true; }
        else if (!std::strcmp(a, "--stage1"))         g_opt.mode = "stage1";
        else if (starts_with(a, "--mesh-bench="))     g_meshBenchPasses = atoi(a + 13);
        else if (!std::strcmp(a, "--smoke"))          g_opt.mode = "smoke";
#if defined(EDEN_DIAGNOSTICS)
        else if (starts_with(a, "--save-bench="))     { g_opt.mode = "save-bench"; g_opt.headless = true; g_saveBenchList = a + 13; }
#endif
        else if (!std::strcmp(a, "--input-selftest")) { g_opt.mode = "input-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--audio-selftest")) { g_opt.mode = "audio-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--gamepad-selftest")) { g_opt.mode = "gamepad-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--touch-selftest")) { g_opt.mode = "touch-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--empty-bit-selftest")) { g_opt.mode = "empty-bit-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--newblocks-selftest")) { g_opt.mode = "newblocks-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--photo-selftest")) { g_opt.mode = "photo-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--trailer-selftest")) { g_opt.mode = "trailer-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--trailer-file="))  g_opt.trailerFiles.push_back(a + 15);
        else if (!std::strcmp(a, "--trailer-no-dirty-mark")) g_opt.trailerNoDirty = true;
        else if (!std::strcmp(a, "--trailer-keep"))  g_opt.trailerKeep = true;
        else if (!std::strcmp(a, "--signs-selftest")) { g_opt.mode = "signs-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--camera-selftest")) { g_opt.mode = "camera-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--signs-mutate="))  g_opt.signsMutate = a + 15;
        else if (!std::strcmp(a, "--cmds-selftest"))  { g_opt.mode = "cmds-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--cmds-mutate="))   g_opt.cmdsMutate = a + 14;
        else if (!std::strcmp(a, "--cmds-trace"))     g_opt.cmdsTrace = true;
        else if (!std::strcmp(a, "--console-selftest")) { g_opt.mode = "console-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--console-mutate=")) g_opt.consoleMutate = a + 17;
        else if (starts_with(a, "--console-corpus=")) g_opt.consoleCorpus = a + 17;
        else if (starts_with(a, "--console-reference=")) g_opt.consoleRef = a + 20;
        else if (!std::strcmp(a, "--console-dump")) { g_opt.mode = "console-dump"; g_opt.headless = true; }
        else if (starts_with(a, "--signs="))         SignRenderer::enabled = atoi(a + 8) != 0;
        else if (!std::strcmp(a, "--sign-shot"))      g_opt.mode = "sign-shot";
        else if (starts_with(a, "--sign-shot="))      { g_opt.mode = "sign-shot"; g_opt.shot = a + 12; }
        else if (starts_with(a, "--look=")) {
            g_opt.haveLook = std::sscanf(a + 7, "%f,%f", &g_opt.look[0], &g_opt.look[1]) == 2;
        }
        else if (starts_with(a, "--walk=")) {
            float* w = g_opt.walk;
            const int n = std::sscanf(a + 7, "%f,%f,%f,%f,%f,%f,%f,%f", w, w + 1, w + 2, w + 3, w + 4, w + 5, w + 6, w + 7);
            g_opt.walkLegs = n / 2;
        }
        else if (!std::strcmp(a, "--codec-bench"))   { g_opt.mode = "codec-bench"; g_opt.headless = true; }
        else if (starts_with(a, "--codec-file="))    g_opt.codecFile = a + 13;
        else if (!std::strcmp(a, "--bundled-map-selftest")) { g_opt.mode = "bundled-map-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--bundled-eden="))  g_opt.bundledEden = a + 15;
        else if (starts_with(a, "--bundled-emod="))  g_opt.bundledEmod = a + 15;
        else if (!std::strcmp(a, "--net-selftest"))  { g_opt.mode = "net-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--mp-selftest"))   { g_opt.mode = "mp-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--server="))        g_opt.netServer = a + 9;
        else if (starts_with(a, "--mp-replay="))     { g_opt.mode = "mp-replay"; g_opt.headless = true; g_opt.mpReplay = a + 12; }
        else if (starts_with(a, "--mp-mutate="))     g_opt.mpMutate = a + 12;
        else if (starts_with(a, "--mp-overlay-mb=")) g_opt.mpOverlayMb = atoi(a + 16);
        else if (starts_with(a, "--mp-join="))       g_opt.mpJoin = a + 10;
        else if (starts_with(a, "--mp-name="))       g_opt.mpName = a + 10;
        else if (starts_with(a, "--mp-password="))   g_opt.mpPassword = a + 14;
        else if (starts_with(a, "--mp-type="))       g_opt.mpType = atoi(a + 10);
        else if (!std::strcmp(a, "--mp-edits"))      g_opt.mpEdits = true;
        else if (starts_with(a, "--mp-hold="))       g_opt.mpHold = a + 10;
        else if (!std::strcmp(a, "--mp-mock"))       eden_mp_servers_mock(1);   // MP.6: the canned server list
        else if (starts_with(a, "--mp-reconcile="))  MpWorld::reconcileOn = atoi(a + 15) != 0;
        else if (!std::strcmp(a, "--emod-selftest")) { g_opt.mode = "emod-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--emod-fixtures=")) g_opt.emodFixtures = a + 16;
        else if (starts_with(a, "--emod-work="))     g_opt.emodWork = a + 12;
        else if (starts_with(a, "--emod-file="))     g_opt.emodFile = a + 12;
        else if (starts_with(a, "--emod-ref="))      g_opt.emodRef = a + 11;
        else if (!std::strcmp(a, "--emod-quick"))    g_opt.emodQuick = true;
        else if (!std::strcmp(a, "--light-selftest")) { g_opt.mode = "light-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--evict-selftest")) { g_opt.mode = "evict-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--ring-light-selftest")) { g_opt.mode = "ring-light-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--evict-mutate="))  g_opt.evictMutate = a + 15;
        else if (starts_with(a, "--light-lattice=")) g_opt.lightLattice = atoi(a + 16);
        else if (!std::strcmp(a, "--shadow-selftest")) { g_opt.mode = "shadow-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--classic-selftest")) { g_opt.mode = "classic-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--classic-fixture=")) g_opt.classicFixture = a + 18;
        else if (starts_with(a, "--classic-mutate=")) g_opt.classicMutate = a + 17;
        else if (!std::strcmp(a, "--props-selftest")) { g_opt.mode = "props-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--props-mutate=")) g_opt.propsMutate = a + 15;
        else if (starts_with(a, "--props-world=")) g_opt.propsWorld = a + 14;
        else if (starts_with(a, "--classic="))        g_opt.classicNew = a + 10;
        else if (starts_with(a, "--shadows=")) {
            const char* m = a + 10;
            const int v = !std::strcmp(m, "off") ? 0 : !std::strcmp(m, "classic") ? 1 : !std::strcmp(m, "smooth") ? 2 : -2;
            if (v < 0) { std::fprintf(stderr, "[eden] --shadows= takes off|classic|smooth\n"); return 2; }
            shadow_setOverride(v);
        }
        else if (starts_with(a, "--shadow-mutate=")) {
            const char* m = a + 16;
            g_shadow_test_mutation = !std::strcmp(m, "nohook") ? 1 : !std::strcmp(m, "nodirty") ? 2
                                   : !std::strcmp(m, "nolanding") ? 3 : !std::strcmp(m, "nodiag") ? 4 : 0;
        }
        else if (starts_with(a, "--light-mutate=")) {
            const char* m = a + 15;
            g_light_test_mutation = !std::strcmp(m, "nosync") ? 1 : !std::strcmp(m, "nomark") ? 2
                                  : !std::strcmp(m, "wrap") ? 3 : !std::strcmp(m, "nolbmark") ? 4 : 0;
        }
        else if (!std::strcmp(a, "--keybind-selftest")) { g_opt.mode = "keybind-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--ui-selftest"))   { g_opt.mode = "ui-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--convert-prompt-selftest")) { g_opt.mode = "convert-prompt-selftest"; g_opt.headless = true;
                                                        eden_setenv("EDEN_UPGRADE_256Z", "1"); }
        else if (starts_with(a, "--convert-answer=")) g_opt.convertAnswer = a + 17;
        else if (!std::strcmp(a, "--browser-selftest")) { g_opt.mode = "browser-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--net-live-selftest")) { g_opt.mode = "net-live-selftest"; g_opt.headless = true; }
        else if (starts_with(a, "--net-fixtures="))  eden_net_set_fixture_root(a + 15);
        else if (!std::strcmp(a, "--objc-selftest")) { g_opt.mode = "objc-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--gl-selftest"))   { g_opt.mode = "gl-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--recolor-selftest")) { g_opt.mode = "recolor-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--display-selftest")) { g_opt.mode = "display-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--convert64-selftest")) { g_opt.mode = "convert64-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--save-roundtrip")) g_opt.mode = "save-roundtrip";
        else if (!std::strcmp(a, "--save-inplace")) { /* S.8: ignored -- there is no in-place .eden save any more */ }
        else if (!std::strcmp(a, "--background-selftest")) { g_opt.mode = "background-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--pass-probe"))     g_opt.mode = "pass-probe";
        else if (starts_with(a, "--pass-probe="))     { g_opt.mode = "pass-probe"; g_opt.passFrames = std::max(10, atoi(a + 13)); }
        else if (!std::strcmp(a, "--pass-timers"))    g_opt.passTimers = true;
        else if (starts_with(a, "--set=") && std::strchr(a + 6, '='))
            { std::string kv = a + 6; size_t e = kv.find('='); g_opt.sets.push_back({kv.substr(0, e), (float)atof(kv.c_str() + e + 1)}); }
        else if (!std::strcmp(a, "--pass-finish"))    g_opt.passFinish = true;
        else if (!std::strcmp(a, "--pass-vsync"))     g_opt.passVsync = true;
        else if (!std::strcmp(a, "--shot"))           g_opt.mode = "shot";
        else if (!std::strcmp(a, "--fit-selftest"))   g_opt.mode = "fit-selftest";   // I.3, windowed like --shot
        else if (starts_with(a, "--shot="))           { g_opt.mode = "shot"; g_opt.shot = a + 7; }
        else if (starts_with(a, "--height="))         g_opt.height = atoi(a + 9);
        else if (starts_with(a, "--world-format="))   { /* S.8: ignored -- every world is an .emod */ }
        else if (starts_with(a, "--radius="))         eden_setenv("EDEN_WINDOW_RADIUS", a + 9);
        else if (starts_with(a, "--upgrade-256z="))   eden_setenv("EDEN_UPGRADE_256Z", atoi(a + 15) ? "1" : "0");
        else if (starts_with(a, "--empty-shortcut=")) { extern bool g_empty_shortcut; g_empty_shortcut = atoi(a + 17) != 0; }
        else if (std::strcmp(a, "--empty-selfcheck") == 0) eden_debug_set_empty_selfcheck(1);
        else if (std::strcmp(a, "--light-selfcheck") == 0) eden_debug_set_light_selfcheck(1);
        else if (starts_with(a, "--read-budget-bands=")) { extern bool g_read_budget_bands; g_read_budget_bands = atoi(a + 20) != 0; }
        else if (starts_with(a, "--export") || starts_with(a, "--import") || starts_with(a, "--upload-"))
                                                      { g_opt.mode = "world-tool"; g_opt.headless = true; }   // WorldTools_native.cpp
        else if (starts_with(a, "--world="))          g_opt.world = a + 8;
        else if (starts_with(a, "--frames="))         g_opt.frames = atoi(a + 9);
        else if (!std::strcmp(a, "--touch-profile")) g_opt.touchProfile = true;
        else if (starts_with(a, "--safe-area=")) {
            float l = 0, t = 0, r = 0, b = 0;
            if (sscanf(a + 12, "%f,%f,%f,%f", &l, &t, &r, &b) == 4) {
                g_opt.safeL = l; g_opt.safeT = t; g_opt.safeR = r; g_opt.safeB = b;
                eden_native_set_safe_area_override(l, t, r, b);   // I.2: the same seam call SDL's area feeds
            }
        }
        else if (starts_with(a, "--device=")) {
            // I.0: phone/tablet preview on a desktop = window size in points + touch profile + safe insets.
            struct Dev { const char* n; int w, h; float l, t, r, b; };
            static const Dev kDev[] = {
                {"iphone16", 852, 393, 59, 0, 59, 21}, {"iphone16promax", 932, 430, 59, 0, 59, 21},
                {"iphonese", 667, 375, 0, 0, 0, 0},    {"ipadair2", 1024, 768, 0, 0, 0, 0},
                {"ipadpro11", 1194, 834, 0, 0, 0, 20},
            };
            bool found = false;
            for (const Dev& d : kDev) if (!std::strcmp(a + 9, d.n)) {
                g_opt.winW = d.w; g_opt.winH = d.h; g_opt.touchProfile = true;
                g_opt.safeL = d.l; g_opt.safeT = d.t; g_opt.safeR = d.r; g_opt.safeB = d.b; found = true;
                eden_native_set_safe_area_override(d.l, d.t, d.r, d.b);
            }
            if (!found) std::fprintf(stderr, "[eden] unknown --device=%s (iphone16 iphone16promax iphonese ipadair2 ipadpro11)\n", a + 9);
        }
        else if (starts_with(a, "--window=")) {
            // Phase N Stage 4.3. The letterbox fit can only be exercised where the window's aspect
            // differs from the engine's, and every default this harness has is 16:9 — which is the
            // reason the missing fit survived three stages. `--window=1024x768` reproduces an
            // iPad's 4:3 on a desktop, which is the only place it can be tested without hardware.
            int w = 0, h = 0;
            if (sscanf(a + 9, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                g_opt.winW = w; g_opt.winH = h;
            }
        }
        else if (!std::strcmp(a, "--leak-probe"))     { g_opt.mode = "leak-probe"; g_opt.headless = true; }
        else if (starts_with(a, "--leak-probe="))     { g_opt.mode = "leak-probe"; g_opt.headless = true;
                                                        g_opt.cycles = atoi(a + 13); }
        else if (starts_with(a, "--cycles="))         g_opt.cycles = atoi(a + 9);
        else if (starts_with(a, "--docs="))           g_opt.docs = a + 7;
        else if (starts_with(a, "--bundle="))         g_opt.bundle = a + 9;
        else if (starts_with(a, "--live-cmds="))      g_opt.liveCmds = a + 12;
        else if (starts_with(a, "--render-scale="))   g_opt.renderScalePct = atoi(a + 15);
        else if (!std::strcmp(a, "--scale-probe"))    g_opt.mode = "scale-probe";
        else if (!std::strcmp(a, "--preset-probe"))   g_opt.mode = "preset-probe";
        else if (!std::strcmp(a, "--mem-trace"))      g_eden_mem_trace = 1;
        else if (starts_with(a, "--mem-trace="))      g_eden_mem_trace = atoi(a + 12);
        else if (starts_with(a, "--tex-exp=")) {
            // N.4.9 texture experiments (gl_fixed_function.cpp): comma list of nomips, storage,
            // noflush. Diagnostics for a device A/B. The per-upload flush has been the default
            // since N.4.9's fix, so `noflush` is the old behaviour; `flush` is accepted as a no-op.
            int bits = 0;
            for (const char* p = a + 10; *p; ) {
                const char* e = std::strchr(p, ',');
                const size_t n = e ? (size_t)(e - p) : std::strlen(p);
                if (n == 6 && !std::strncmp(p, "nomips", 6))       bits |= 1;
                else if (n == 7 && !std::strncmp(p, "storage", 7)) bits |= 2;
                else if (n == 5 && !std::strncmp(p, "flush", 5))   eden_gl_set_tex_flush(1);
                else if (n == 7 && !std::strncmp(p, "noflush", 7)) eden_gl_set_tex_flush(0);
                else { std::fprintf(stderr, "eden: unknown --tex-exp entry in '%s'\n", a); return 2; }
                p += n; if (*p == ',') ++p;
            }
#if !defined(EDEN_PLATFORM_IOS)
            if (bits & 2) std::fprintf(stderr, "eden: --tex-exp=storage is ES 3.0 (iOS) only; ignored here\n");
#endif
            eden_gl_set_tex_experiments(bits);
        }
        else if (starts_with(a, "--at=")) {
            if (sscanf(a + 5, "%f,%f,%f", &g_opt.at[0], &g_opt.at[1], &g_opt.at[2]) == 3)
                g_opt.haveAt = true;
        } else {
            std::fprintf(stderr, "eden: unknown option '%s' (see eden_main_native.cpp's header)\n", a);
            return 2;
        }
    }
    if (!g_opt.mpJoin.empty() && g_opt.headless && !g_opt.mode) g_opt.mode = "mp-join";   // MP.3b's live gate
    return 0;
}

static int eden_main_after_args(int argc, char** argv) {
    // S.5e: harness modes pin the height a conversion writes: the source's, unless --upgrade-256z=1
    // (or EDEN_UPGRADE_256Z) says otherwise -- never the Settings toggle, which a --docs directory
    // starts at its default (on) and which would silently change what every convert gate measures.
    if ((g_opt.mode || !g_opt.liveCmds.empty()) && !getenv("EDEN_UPGRADE_256Z")) eden_setenv("EDEN_UPGRADE_256Z", "0");
    // B.5b-2: likewise the window radius -- stock 9 unless --radius= (or EDEN_WINDOW_RADIUS) says otherwise.
    if ((g_opt.mode || !g_opt.liveCmds.empty()) && !getenv("EDEN_WINDOW_RADIUS")) eden_setenv("EDEN_WINDOW_RADIUS", "9");
    // Stage S / S.5: the world-file tools need no engine, no window and no save directory.
    if (g_opt.mode && !std::strcmp(g_opt.mode, "world-tool")) {
        std::vector<char*> av(argv, argv + argc);
        for (std::string& a : g_string_args) av.push_back(&a[0]);
        const int rc = eden_world_tool_main((int)av.size(), av.data());
#if defined(EDEN_PLATFORM_IOS)
        std::fflush(stdout); std::fflush(stderr);
        std::_Exit(rc);   // an iOS app's main returning does not end the process; the harness waits on it
#endif
        return rc;
    }
    (void)argc;
    // X.21: every `--*-selftest` keeps its prefs in memory (no read of the user's real prefs file, no
    // write), so a stored setting cannot change a result (X.19's cause). Nothing has touched prefs yet.
    // Q.1 F13: every scripted mode (not just `-selftest`) and `--live-cmds` runs ephemeral, so `--set=` /
    // `--render-scale=` / console history from a script never reach the profile.
    if (g_opt.mode || !g_opt.liveCmds.empty()) {
        const size_t ml = g_opt.mode ? std::strlen(g_opt.mode) : 0;
        const bool selftest = ml > 9 && !std::strcmp(g_opt.mode + ml - 9, "-selftest");
        const bool plays = g_opt.mode && !std::strcmp(g_opt.mode, "mp-join");   // a real join keeps the profile
        if (selftest || !plays) eden_prefs_set_ephemeral(1);
    }
    // `--touch-selftest --window=WxH` runs WINDOWED, and that is the whole point of the
    // combination: headless has no window, so the letterbox is degenerate and the mapping under
    // test is the fallback rather than the real one. With a window — and `--touch-profile` for the
    // touch layout, its UI scale and its on-screen joystick — this is an iPad's exact
    // configuration on a developer's desk.
    if (g_opt.mode && !std::strcmp(g_opt.mode, "touch-selftest")) {
#if defined(EDEN_PLATFORM_IOS)
        // ALWAYS windowed on iOS. There the window is the device's screen and the point space is
        // derived from it, so the whole window->point mapping is live — which is exactly what
        // broke, and testing the degenerate headless mapping there would prove nothing.
        g_opt.headless = false;
#else
        if (g_opt.winW > 0) g_opt.headless = false;
#endif
    }

    if (g_opt.mode && !g_opt.haveAt) {
        // A FIXED default origin: the engine spawns a new world at a hashed position, and chunk
        // contents are a function of position, so a run that did not teleport would checksum
        // something different every time.
        //
        // It is NOT, as this comment claimed until Stage 3, "inside the bundled map". 1440 is
        // outside the playable square on both axes and Player::preupdate clamps it to the map's
        // south-west CORNER — see teleport_for_movement() for the arithmetic and for what that
        // cost. Fixed and reproducible is all the checksum modes need, so the value stays and the
        // recorded fingerprints stay with it; the movement selftests go to the centre instead.
        g_opt.at[0] = 1440.0f; g_opt.at[1] = 40.0f; g_opt.at[2] = 1440.0f;
    }

    const std::string docs   = g_opt.docs.empty()   ? default_docs()            : g_opt.docs;
    const std::string bundle = g_opt.bundle.empty() ? default_bundle(argv[0])   : g_opt.bundle;
    mkdirs(docs);
    eden_platform_set_roots(docs.c_str(), bundle.c_str());
    // C.10: `--console-dump` prints the generated tables of docs/console-reference.md (port/tools/
    // console-reference.py splices them in); the registry needs no world and no window.
    if (g_opt.mode && !std::strcmp(g_opt.mode, "console-dump")) {
        Console::init();
        const char* const secs[] = {"block-verbs", "console-verbs", "variables"};
        for (const char* sec : secs)
            std::printf("<!-- BEGIN generated:%s -->\n%s<!-- END generated:%s -->\n", sec, Console::referenceMarkdown(sec).c_str(), sec);
        return 0;
    }
    // Only the platform default save directory migrates. An explicit --docs= / EDEN_DOCUMENTS is a
    // harness or a deliberate override: migrating into it MOVED a real save out of
    // ~/Library/Application Support/Eden (2026-10-09) and, on macOS, hung unattended runs on the
    // ~/Documents privacy prompt (2026-10-07) unless a `.migrated-from-documents` marker was made first.
    if (g_opt.docs.empty() && !getenv("EDEN_DOCUMENTS")) migrate_legacy_saves(docs);
    // C.9: the player's README.txt. Harness runs leave their --docs directories as they found them.
    if (!g_opt.mode && g_opt.liveCmds.empty()) {
        const int r = eden_player_readme_write(docs.c_str());
        if (r < 0) std::fprintf(stderr, "[eden] README.txt not written in %s\n", docs.c_str());
    }

    // The previous-save slot. Stage 1 left native WITHOUT one, and that was a real regression
    // against web rather than a missing nicety: web gets it from the Foundation shim's
    // NSFileHandle, and this target links Apple's real Foundation, whose NSFileHandle is a class
    // cluster with `-writeData:` on a private subclass — so there is no honest place to swizzle
    // it. The engine grew a hook at the top of FileManager::saveWorld instead, and both targets
    // run the identical rules (port/src/shim/foundation/save_backup.cpp). Installed here rather
    // than inside the seam file so the file that OWNS the decision is the one that shows it.
    //
    // Narrower than web's, and knowingly so: the shim guards every file open, including the C3
    // rollback journal's, while this covers the world file at save time. That is the case the
    // slot exists for; the journal has its own recovery.
    eden_save_backup_hook = eden_save_backup_before_overwrite;
    // ...and the save DIRECTORY. Without this, Classes/FileManager.mm asks real Foundation and
    // gets ~/Documents; the seam's value (and therefore --docs=) would be set and ignored, which
    // is the state Stage 1 shipped in. eden_platform_documents_root() outlives the process, which
    // is what the hook's contract requires.
    eden_documents_root_hook = eden_platform_documents_root;
    // The seam value IS what the engine uses now — Classes/FileManager.mm asks
    // eden_platform_documents_root() rather than NSSearchPathForDirectoriesInDomains (Phase N
    // Stage 2). Through Stage 1 those were different things on this target and printing the seam
    // value here would have been a lie with a plausible shape; that is what changed.
    @autoreleasepool {
        std::fprintf(stderr, "[eden] documents=%s\n[eden] bundle=%s\n",
                     docs.c_str(), [[[NSBundle mainBundle] resourcePath] UTF8String]);
    }

    if (g_opt.headless) eden_native_gl_set_headless(1);
    // Audio off for every scripted mode, not just the headless ones. --stage1 and
    // --input-selftest run thousands of frames in a few seconds, so real playback would be every
    // sound the engine fires, decoded and queued at ~50x real time — and --smoke/--shot open a
    // window but are still measurements, not a session.
    if (g_opt.mode && std::strcmp(g_opt.mode, "audio-selftest") != 0)
        eden_native_audio_set_enabled(0);

    // The EVENTS subsystem, always. Video is initialised later by eden_gl_context_create() and only
    // when there is a window — but --input-selftest is headless and still needs a live event queue
    // for SDL_PushEvent/SDL_PollEvent to carry its synthetic keystrokes through the real pump.
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "[eden] SDL_InitSubSystem(EVENTS) failed: %s\n", SDL_GetError());
    }

    // Phase N Stage 4.2 — the mobile half of save-on-background, and it CANNOT go in the poll loop
    // below. SDL_events.h says so of these four events in as many words ("This event must be
    // handled in a callback set with SDL_AddEventWatch()"): on iOS they are delivered synchronously
    // from inside UIKit's applicationWillResignActive:/DidEnterBackground:, and by the time the
    // main loop next reaches SDL_PollEvent the OS may already have suspended — or killed — the
    // process. A watch runs on the spot, which is the only place a save is still possible. Desktop
    // never fires these, so this costs one function pointer there. (SDL_EVENT_QUIT stays in the
    // poll loop: it is a normal event and the loop must also stop running.)
    SDL_AddEventWatch(eden_lifecycle_event_watch, nullptr);

    // The GL context MUST be live before eden_seam_main(), and that is measured rather than
    // preferred: World::World() -> Graphics::initGraphics() issues real glGenBuffers/glBufferData
    // during construction. A failure here is NOT fatal — the shim's context guard keeps the whole
    // engine running headless, which is exactly what the scripted modes want.
    eden_mem_trace_install();
    eden_mem_trace("boot");
    eden_gl_context_create(g_opt.winW, g_opt.winH);

    // N.4.10 (2026-10-04): native never calls eden_set_low_memory(), on purpose — the decision,
    // not an omission. On web it refuses 256z worlds and seeds 1x DPR / 75% render scale / 45 fps.
    // On the 2 GB iPad Air 2, once N.4.9's texture flush landed, the F.3 walk peaks at 206 MB at
    // 64z and 252 MB at 256z, so the refusal would protect nothing. And 75% render scale on native
    // ADDS an offscreen framebuffer (N.4.11) rather than shrinking the drawable. Revisit only on a
    // device that measures over criterion 3 (300 MB) with the fix in; the predicate would then be
    // SDL_GetSystemRAM(). Never LOW_MEM_DEVICE: that turns coloured lighting off.
    // W.2 (b): a window's boot applies the display profile BEFORE World::World() loads textures
    // (SDLView_native.mm -> DisplayMode_native.cpp), so IS_IPAD is set and the ipad~ assets load --
    // the only paint mask that ships is ipad~paint_mask.png. Headless has no window and applies it
    // only once the settings load, after the textures, so a headless boot holds the 1x set and no
    // paint mask. --recolor-selftest asserts the pipeline a device runs, so it boots in that order.
    // Every other mode keeps the headless order its pinned checksums were taken under.
    if (g_opt.mode && !std::strcmp(g_opt.mode, "recolor-selftest")) eden_display_refresh();
    eden_native::eden_seam_main();
    g_app = eden_native::eden_seam_get_app_delegate();
    eden_mem_trace("world-constructed");
    eden_native_input_init(eden_native_gl_window());

    // Must be BEFORE any world is meshed: prepareVBO() fingerprints a chunk only if this is on at
    // publish time, and it is opt-in because it costs a pass over every published vertex. Scripted
    // modes only — an interactive session should not pay for it.
    if (g_opt.mode) eden_debug_set_mesh_checksum(1);

    // NOT ENABLED: `g_app->viewController.setFixedEtime(1.0f/60.0f)` for the scripted modes. It is
    // the obviously right idea — "N frames" should be N/60 s of engine time regardless of machine
    // load — and it was tried here. It does not work on its own, because this build has THREE time
    // bases and only one of them would move: the engine's etime (fixed), Movement_web.mm's
    // frame-rate-normalisation EMA (wall clock, so with frames flying past it pins at its 4x clamp
    // and the player walks 500 blocks in 300 frames), and the input translator's hold-to-act repeat
    // (wall clock, so the number of mine pulses per second of engine time collapses). Making the
    // harness deterministic means putting all three on the same clock, which is a real piece of
    // design and not a line of setup. The lever is left in place, documented, and off — see
    // EdenViewController_native::setFixedEtime().

    // The settings model. This was the "NOT CALLED YET, DELIBERATELY" block through commit
    // d8e815f, because adding eden_settings_init() broke --input-selftest's "W moves the player"
    // check on 4 runs of 4. THE CAUSE WAS THE SELF-TEST, NOT THE SETTINGS.
    //
    //   * The check teleports to a fixed origin that lands the player INSIDE terrain, and got out
    //     of it by holding Space — which lifts you only because this fork ships FLY_MODE=true
    //     (Classes/Player.mm:32). The `fly` row's schema default is 0, so loading the settings
    //     correctly turned fly OFF, Space became an ordinary jump, the player stayed embedded in
    //     the hill and walked exactly 0.00 blocks. A buried player who cannot walk is the engine
    //     behaving; the harness was asserting on a default it never declared.
    //   * That is also why eden_set_detected_touch(0) "helped" (1 of 3 rather than 0 of 4): it
    //     changes the active display profile, hence the point space, hence nothing about movement
    //     — it just perturbed where the settle ended up. There was never a race.
    //
    // run_input_selftest() now forces fly on through the real V binding before it climbs, so it
    // asserts on the wiring rather than on a default. GENERALISES: when enabling a subsystem
    // "breaks" a test, check whether the test was depending on that subsystem being absent.
    //
    // Two host facts have to be stated before the model loads, because both are properties of
    // this build rather than preferences:
    eden_ui_force_legacy(1);        // there is no DOM UI here; the GL menus are the only menus
    // ...and iOS is the one target where the second fact inverts. Phase N Stage 4.1: this read
    // `eden_set_detected_touch(0)` unconditionally, which was true of the three desktop legs and
    // is a lie on a phone — the profile it selects lays the HUD out for a pointer on a device that
    // has none. Stage 2 paid for this class of mistake once already (the ESC menu drew nothing
    // because `legacy_menu` defaulted to a value chosen for a DOM host): EVERY port-owned default
    // was chosen against some other host, so audit them rather than inherit them.
    //
    // `--touch-profile` forces the same choice on a desktop run, and it is a testing tool with a
    // real job: the profile decides the LAYOUT — its UI scale, its on-screen joystick, and (until
    // 2026-09-07, when the device reported black bars) whether the aspect was pinned to 16:9. A
    // desktop window is not a phone, so without this flag an iPad's configuration cannot be
    // reproduced anywhere a developer can see it — which is most of why the missing viewport call
    // survived three stages.
#if defined(EDEN_PLATFORM_IOS)
    eden_set_detected_touch(1);
#else
    eden_set_detected_touch(g_opt.touchProfile ? 1 : 0);
    if (g_opt.touchProfile) eden_test_force_touch_input();  // I.0: a saved input_mode must not undo the preview (in-process only)
#endif
    settings_pump();                // may be too early (needs menu->settings); tick() retries

    int rc = 0;
    if (g_opt.mode) {
        if (!std::strcmp(g_opt.mode, "p1-gate"))            rc = run_p1_gate();
        else if (!std::strcmp(g_opt.mode, "stage1"))        rc = run_stage1();
        else if (!std::strcmp(g_opt.mode, "smoke"))         rc = run_smoke();
#if defined(EDEN_DIAGNOSTICS)
        else if (!std::strcmp(g_opt.mode, "save-bench"))    rc = run_save_bench();
#endif
        else if (!std::strcmp(g_opt.mode, "input-selftest")) rc = run_input_selftest();
        else if (!std::strcmp(g_opt.mode, "keybind-selftest")) rc = run_keybind_selftest();
        else if (!std::strcmp(g_opt.mode, "ui-selftest")) rc = run_ui_selftest();
        else if (!std::strcmp(g_opt.mode, "convert-prompt-selftest")) {
#if !defined(_WIN32)
            unsetenv("EDEN_CONVERT_PROMPT");
#else
            _putenv_s("EDEN_CONVERT_PROMPT", "");
#endif
            rc = run_convert_prompt_selftest();
            if (g_selftestFailures) rc = 1;
            std::printf("[eden-convert] %s (%d failure(s))\n", rc ? "FAILURES" : "ALL PASS", g_selftestFailures);
        }
        else if (!std::strcmp(g_opt.mode, "bundled-map-selftest")) {
            int n = 0;
            int bad = fmh_selftestCompare(g_opt.bundledEden.c_str(), g_opt.bundledEmod.c_str(), &n);
            std::printf("[bundled-map] %d columns compared, %d differ -> %s\n", n, bad, bad == 0 ? "ALL PASS" : "FAILURES");
            rc = bad == 0 ? 0 : 1;
        }
        else if (!std::strcmp(g_opt.mode, "browser-selftest")) rc = run_browser_selftest(false);
        else if (!std::strcmp(g_opt.mode, "net-live-selftest")) rc = run_browser_selftest(true);
        else if (!std::strcmp(g_opt.mode, "audio-selftest")) rc = run_audio_selftest();
        else if (!std::strcmp(g_opt.mode, "gamepad-selftest")) rc = run_gamepad_selftest();
        else if (!std::strcmp(g_opt.mode, "touch-selftest")) rc = run_touch_selftest();
        else if (!std::strcmp(g_opt.mode, "empty-bit-selftest")) rc = run_empty_bit_selftest();
        else if (!std::strcmp(g_opt.mode, "newblocks-selftest")) rc = run_newblocks_selftest();
        else if (!std::strcmp(g_opt.mode, "net-selftest"))  rc = eden_net_selftest_run(g_opt.netServer.c_str());
        else if (!std::strcmp(g_opt.mode, "mp-selftest"))   rc = eden_mp_selftest_run(g_opt.netServer.c_str());
        else if (!std::strcmp(g_opt.mode, "mp-replay"))     rc = run_mp_replay();
        else if (!std::strcmp(g_opt.mode, "mp-join"))       rc = run_mp_join();
        else if (!std::strcmp(g_opt.mode, "photo-selftest")) rc = run_photo_selftest();
        else if (!std::strcmp(g_opt.mode, "trailer-selftest")) rc = run_trailer_selftest();
        else if (!std::strcmp(g_opt.mode, "signs-selftest")) rc = run_signs_selftest();
        else if (!std::strcmp(g_opt.mode, "camera-selftest")) rc = run_camera_selftest();
        else if (!std::strcmp(g_opt.mode, "sign-shot"))     rc = run_sign_shot();
        else if (!std::strcmp(g_opt.mode, "cmds-selftest")) rc = run_cmds_selftest();
        else if (!std::strcmp(g_opt.mode, "console-selftest")) rc = run_console_selftest();
        else if (!std::strcmp(g_opt.mode, "light-selftest")) rc = run_light_selftest();
        else if (!std::strcmp(g_opt.mode, "evict-selftest")) rc = run_evict_selftest();
        else if (!std::strcmp(g_opt.mode, "ring-light-selftest")) rc = run_ring_light_selftest();
        else if (!std::strcmp(g_opt.mode, "shadow-selftest")) rc = run_shadow_selftest();
        else if (!std::strcmp(g_opt.mode, "classic-selftest")) rc = run_classic_selftest();
        else if (!std::strcmp(g_opt.mode, "props-selftest")) rc = run_props_selftest();
        else if (!std::strcmp(g_opt.mode, "codec-bench"))   rc = eden_codec_bench(g_opt.codecFile.c_str(), g_opt.height, 500);
#if defined(EDEN_DIAGNOSTICS)
        else if (!std::strcmp(g_opt.mode, "emod-selftest"))
            rc = eden_emod_selftest_run(g_opt.emodFixtures.c_str(), g_opt.emodWork.c_str(), g_opt.emodQuick ? 1 : 0,
                                        g_opt.emodFile.c_str(), g_opt.emodRef.c_str());
#endif
        else if (!std::strcmp(g_opt.mode, "objc-selftest")) rc = run_objc_selftest();
        else if (!std::strcmp(g_opt.mode, "gl-selftest"))   rc = run_gl_selftest();
        else if (!std::strcmp(g_opt.mode, "recolor-selftest")) rc = run_recolor_selftest();
        else if (!std::strcmp(g_opt.mode, "display-selftest")) rc = run_display_selftest();
        else if (!std::strcmp(g_opt.mode, "convert64-selftest")) rc = run_convert64_selftest();
        else if (!std::strcmp(g_opt.mode, "save-roundtrip")) rc = run_save_roundtrip();
        else if (!std::strcmp(g_opt.mode, "background-selftest")) rc = run_background_selftest();
        else if (!std::strcmp(g_opt.mode, "shot"))          rc = run_shot();
        else if (!std::strcmp(g_opt.mode, "fit-selftest"))  rc = run_fit_selftest();
        else if (!std::strcmp(g_opt.mode, "scale-probe"))   rc = run_scale_probe();
        else if (!std::strcmp(g_opt.mode, "preset-probe"))  rc = run_preset_probe();
        else if (!std::strcmp(g_opt.mode, "pass-probe"))    rc = run_pass_probe();
        else if (!std::strcmp(g_opt.mode, "leak-probe"))    rc = run_leak_probe();
    } else {
        // Interactive. The frame-rate cap is the same setting the web build's frame gate reads
        // (Settings_web.mm's eden_get_fps_cap), but it is applied as PACING — sleep until the next
        // frame is due, then update and draw together — and not as web's "skip the draw, still run
        // update" gate. Web can afford that gate because rAF only fires at the display rate, so a
        // skipped draw still leaves one engine tick per display frame. This loop has no rAF: until
        // 2026-09-25 it ran `drawFrame(false)` + SDL_Delay(1) on every capped iteration, i.e. ~540
        // engine ticks/s against 58 draws/s under the touch profile's default 60 fps cap. The engine
        // cannot tick that fast (N.4.7, "walking barely moves on iOS"): ground friction is
        // per-TICK (Player.mm, vel*=.90), so walk speed falls with the tick rate, and positions
        // near the map centre are float32 at ~65536 where one ULP is 1/128 block — a 1.8 ms tick's
        // walking step is below half of that and `pos += vel*etime` rounds to no movement at all.
        // Fly mode (0.995 damping, far higher speed) cleared the ULP, which is why only walking died.
        //
        // Audit row A4 (skipping the whole tick dropped input on capped frames) does not come back:
        // nothing is skipped. Events that arrive while we sleep stay in SDL's queue and are pumped
        // into the same tick they would have reached anyway. Paced to a fixed deadline schedule, not
        // "interval since the last frame", so the average rate is the cap exactly; the slack lets a
        // frame start a little early (the vsync-blocking swap absorbs it) so that a cap equal to the
        // display rate never sleeps past a vsync and halves the frame rate.
        constexpr double kPaceSlackMs = 2.0;
        double nextFrameMs = 0.0;
#if defined(EDEN_DIAGNOSTICS)
        if (!g_opt.liveCmds.empty()) eden_live_commands_init(g_opt.liveCmds.c_str());
#endif
        if (!g_opt.mpJoin.empty()) mp_join_from_args();   // MP.3b: straight from the main menu to a server
        while (g_running) {
            const int capFps = eden_get_fps_cap();
            if (capFps > 0) {
                const double interval = 1000.0 / (double)capFps;
                const double now = eden_platform_now_ms();
                // First frame, a cap change, or a stall (a slow load, a backgrounded app): restart
                // the schedule rather than rushing frames out to catch up.
                if (nextFrameMs == 0.0 || now - nextFrameMs > interval || nextFrameMs - now > interval)
                    nextFrameMs = now;
                const double wait = nextFrameMs - now - kPaceSlackMs;
                if (wait > 0.0) SDL_DelayPrecise((Uint64)(wait * 1e6));
                nextFrameMs += interval;
            } else {
                nextFrameMs = 0.0;
            }
            pump_events();
            if (!g_app) break;
            settings_pump();
            // BEFORE drawFrame: World::update() is what consumes the queued touches, so input
            // pushed after it would land a frame late — the same ordering public/eden-input.js
            // gets by running inside the rAF callback ahead of the engine tick.
            eden_native_input_tick();
#if defined(EDEN_DIAGNOSTICS)
            eden_live_commands_tick();
#endif
            if (g_app->viewController.isAnimating()) g_app->viewController.drawFrame(true);
            // AFTER the tick that queued this frame's sounds, so a sound fired now is bound to
            // the device in the same frame rather than the next one.
            eden_native_audio_tick();
            eden_heap_pressure_tick();
        }
    }

    eden_native_audio_shutdown();
    eden_gl_context_destroy();
    SDL_Quit();
#if defined(EDEN_PLATFORM_IOS)
    // ON iOS, RETURNING FROM main() DOES NOT END THE PROCESS — and the first CI run of the
    // simulator gates is what proved it. The app booted, decoded all 874 bundle assets, ran its
    // three [eden-p1] ticks with glErr=0 and then sat there: SDL's UIKit delegate calls SDL_main
    // from -postFinishLaunch and, when it returns, simply goes back to the UIKit run loop, which
    // is correct for an app someone tapped and wrong for a harness. `simctl launch --console-pty`
    // waits for the process, so a passing gate looked exactly like a hang for seven minutes.
    //
    // exit() rather than a request to UIKit to terminate: this build has no scene lifecycle to
    // unwind and everything above has already shut down. Unconditional, not `if (g_opt.mode)` —
    // an interactive session only reaches this line because g_running went false, i.e. the player
    // asked to quit, and an iOS app that will not close is worse than one that exits abruptly.
    std::fflush(nullptr);
    exit(rc);
#endif
    return rc;
}
