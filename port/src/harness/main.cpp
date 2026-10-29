// main.cpp — the native `int main()` (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md). Twin of web-port-final:web/port/src/harness/eden_main.cpp; that file
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
//   --look-trace         Stage L (WORKING/touch-camera-plan-2026-10-17.md): one `look.trace` line per
//                        touch at its end -- events/frame, the motion the touch look consumed vs
//                        the finger's travel (lost), per-frame error, zero-step frames, frame dt.
//                        Not a mode: add it to a normal run (eden-args.txt on a device). Not
//                        diagnostics-gated.
//   --look-selftest      Stage L: the plan's steps 0-9 -- the trace, no loss, no staircase, per-
//                        window-point speed, smoothing at 30/60/120 Hz, tap vs drag slop, glide,
//                        arbitration, the latched slop. --look-mutate=<name> puts one bug back
//                        (lastonly int enginepts noflush noslop noclear nodecay unlatched multi);
//                        each must FAIL it.
//   --fly-selftest       FM.2: noclip -- Fly's measured cruise, noclip cruise and crisp stop, pitch
//                        steers, passes a wall Fly stops at, lava-proof, y floor, unstick, and a
//                        20 s run streaming keeps up with (run at --radius=9 and --radius=16).
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
//   --scene-shot[=PREFIX] G.5b-0, a REAL WINDOW: a scratch world (--world, default "scene-shot",
//                        deleted first) holding every object kind, fixed etime 1/60, the RNG pinned
//                        (eden_rng_seed(1)), three camera poses x day/night, the full frame plus each
//                        pass alone (chunks objects sky models signs transparent flowers effects hud
//                        other). PREFIX-<day|night>-p<N>-<pass>.bmp; compare two builds with
//                        port/tools/shot-diff.py. Legs: --render-scale=75, --window=WxH.
//   --state-trace=FILE   with --scene-shot: one line per draw for the day / pose 0 frame (pass, call
//                        site, program, texture, colour, fog, blend, depth, cull, alpha, lights,
//                        sprite, texture matrix, stack depths, arrays).
//   --render-selftest    G.5b-0, headless (CI): the renderer core's invariants over the menu, a world,
//                        HUD states and the scene's objects (balanced stacks/brackets, no draw outside
//                        a frame, the signs pass contract) with the null backend. With a window it
//                        also compiles the five programs and checks each against CPU expectations.
//                        --gl-selftest (W.2) is an alias since G.5b-7.
//   --lit-oracle[=DIR]   G.5b-4, macOS window: the lit program vs real fixed-function GL (a CGL legacy
//                        context), seven scenes, max delta 2/255; DIR gets each scene's two BMPs.
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
//   --windowed           FT.11: undo a mode's implied --headless (--mp-replay: leg 7 then checks the tag pixels)
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
//   --base-selftest      G.2b-0: the de-ObjC replacement layer (strf, eden_cstr_copy, path helpers vs the Apple
//                        vectors; EdenFile, eden_fs_*, eden_bundle_path; the flat prefs store). Needs --docs=DIR.
//   --creature-selftest  LG.7: the seven creature models (.edm) load, pose and skin on the CPU for every
//                        quarter frame; hashes pinned on macOS arm64 to the PowerVR SDK's own output
//                        (Classes/CreatureModelSelftest.cpp). Headless, needs only the bundle.
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

//
// G.4: the old eden_main_native.cpp (12.4k lines) is now this directory: main.cpp (argv, main(), the mode
// dispatch), core.cpp (the shared tick/open_world/check helpers), mode_<group>.cpp (the run_* modes, each group in
// its own harness::g_<group> namespace) and harness.h (the includes, externs and the declarations they share).

#include "harness.h"

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


using namespace harness;


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
static bool g_windowed = false;   // --windowed: a mode that defaults to headless opens a window after all
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
        else if (starts_with(a, "--save-bench="))     { g_opt.mode = "save-bench"; g_opt.headless = true; g_saveBenchList = a + 13; }
        else if (!std::strcmp(a, "--input-selftest")) { g_opt.mode = "input-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--audio-selftest")) { g_opt.mode = "audio-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--gamepad-selftest")) { g_opt.mode = "gamepad-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--touch-selftest")) { g_opt.mode = "touch-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--look-selftest"))  { g_opt.mode = "look-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--fly-selftest"))   { g_opt.mode = "fly-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--look-trace"))     eden_look_trace_enable(1);
        else if (starts_with(a, "--fly-mutate="))     g_opt.flyMutate = a + 13;
        else if (starts_with(a, "--look-mutate="))    g_opt.lookMutate = a + 14;
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
        else if (!std::strcmp(a, "--scene-shot"))     g_opt.mode = "scene-shot";
        else if (starts_with(a, "--scene-shot="))     { g_opt.mode = "scene-shot"; g_opt.shot = a + 13; }
        else if (starts_with(a, "--state-trace="))    g_opt.stateTrace = a + 14;
        else if (!std::strcmp(a, "--render-selftest")) { g_opt.mode = "render-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--render-selftest-gl")) g_opt.mode = "render-selftest";
        else if (!std::strcmp(a, "--lit-oracle")) g_opt.mode = "lit-oracle";
        else if (starts_with(a, "--lit-oracle=")) { g_opt.mode = "lit-oracle"; g_opt.litOracleDir = a + 13; }
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
        else if (!std::strcmp(a, "--windowed"))       g_windowed = true;   // FT.11: --mp-replay leg 7's pixel check
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
        else if (!std::strcmp(a, "--base-selftest")) { g_opt.mode = "base-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--emod-selftest")) { g_opt.mode = "emod-selftest"; g_opt.headless = true; }
        else if (!std::strcmp(a, "--creature-selftest")) { g_opt.mode = "creature-selftest"; g_opt.headless = true; }
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
        // G.5b-7 folded the shim's no-context check into --render-selftest ("null backend: ids nonzero
        // and distinct"); the old flag runs it.
        else if (!std::strcmp(a, "--gl-selftest"))   { g_opt.mode = "render-selftest"; g_opt.headless = true; }
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
        else if (starts_with(a, "--power-log="))      power_log_set(atof(a + 12));
        else if (starts_with(a, "--power-exp="))      power_parse_exp(a + 12);
        else if (!std::strcmp(a, "--diag-clear"))     eden_native_gl_set_diag_clear(1);
        else if (!std::strcmp(a, "--scale-probe"))    g_opt.mode = "scale-probe";
        else if (!std::strcmp(a, "--preset-probe"))   g_opt.mode = "preset-probe";
        else if (!std::strcmp(a, "--mem-trace"))      g_eden_mem_trace = 1;
        else if (starts_with(a, "--mem-trace="))      g_eden_mem_trace = atoi(a + 12);
        else if (starts_with(a, "--tex-exp=")) {
            // N.4.9 texture experiments (gl_textures.cpp): comma list of nomips, storage, fp16uv,
            // noflush. Diagnostics for a device A/B. The per-upload flush has been the default
            // since N.4.9's fix, so `noflush` is the old behaviour; `flush` is accepted as a no-op.
            int bits = 0;
            for (const char* p = a + 10; *p; ) {
                const char* e = std::strchr(p, ',');
                const size_t n = e ? (size_t)(e - p) : std::strlen(p);
                if (n == 6 && !std::strncmp(p, "nomips", 6))       bits |= 1;
                else if (n == 7 && !std::strncmp(p, "storage", 7)) bits |= 2;
                else if (n == 6 && !std::strncmp(p, "fp16uv", 6))  bits |= 4;   // I.1: old iOS mediump UVs
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
            std::fprintf(stderr, "eden: unknown option '%s' (see port/src/harness/main.cpp's header)\n", a);
            return 2;
        }
    }
    if (!g_opt.mpJoin.empty() && g_opt.headless && !g_opt.mode) g_opt.mode = "mp-join";   // MP.3b's live gate
    return 0;
}

static int eden_main_after_args(int argc, char** argv) {
    if (g_windowed) g_opt.headless = false;
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
    // ...and the save DIRECTORY. Without this, Classes/FileManager.mm asked real Foundation and
    // got ~/Documents; the seam's value (and therefore --docs=) would be set and ignored, which
    // is the state Stage 1 shipped in (G.2b-4: FileManager.cpp's fallback is now the seam itself). eden_platform_documents_root() outlives the process, which
    // is what the hook's contract requires.
    eden_documents_root_hook = eden_platform_documents_root;
    // The seam value IS what the engine uses now — Classes/FileManager.cpp asks
    // eden_platform_documents_root() rather than NSSearchPathForDirectoriesInDomains (Phase N
    // Stage 2). Through Stage 1 those were different things on this target and printing the seam
    // value here would have been a lie with a plausible shape; that is what changed.
    std::fprintf(stderr, "[eden] documents=%s\n[eden] bundle=%s\n", docs.c_str(), eden_platform_bundle_root());

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

    // The GL context MUST be live before App::boot(), and that is measured rather than
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
    // (App.cpp -> DisplayMode_native.cpp), so IS_IPAD is set and the ipad~ assets load --
    // the only paint mask that ships is ipad~paint_mask.png. Headless has no window and applies it
    // only once the settings load, after the textures, so a headless boot holds the 1x set and no
    // paint mask. --recolor-selftest asserts the pipeline a device runs, so it boots in that order.
    // Every other mode keeps the headless order its pinned checksums were taken under.
    if (g_opt.mode && !std::strcmp(g_opt.mode, "recolor-selftest")) eden_display_refresh();
    eden_native::App::boot();
    g_app = eden_native::App::get();
    eden_mem_trace("world-constructed");
    eden_native_input_init(eden_native_gl_window());

    // Must be BEFORE any world is meshed: prepareVBO() fingerprints a chunk only if this is on at
    // publish time, and it is opt-in because it costs a pass over every published vertex. Scripted
    // modes only — an interactive session should not pay for it.
    if (g_opt.mode) eden_debug_set_mesh_checksum(1);

    // NOT ENABLED: `g_app->setFixedEtime(1.0f/60.0f)` for the scripted modes. It is
    // the obviously right idea — "N frames" should be N/60 s of engine time regardless of machine
    // load — and it was tried here. It does not work on its own, because this build has THREE time
    // bases and only one of them would move: the engine's etime (fixed), Movement.cpp's
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
        else if (!std::strcmp(g_opt.mode, "save-bench"))    rc = run_save_bench();
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
        else if (!std::strcmp(g_opt.mode, "look-selftest")) rc = run_look_selftest();
        else if (!std::strcmp(g_opt.mode, "fly-selftest")) rc = run_fly_selftest();
        else if (!std::strcmp(g_opt.mode, "empty-bit-selftest")) rc = run_empty_bit_selftest();
        else if (!std::strcmp(g_opt.mode, "newblocks-selftest")) rc = run_newblocks_selftest();
        else if (!std::strcmp(g_opt.mode, "base-selftest")) rc = eden_base_selftest_run(g_opt.docs.c_str());
        else if (!std::strcmp(g_opt.mode, "net-selftest"))  rc = eden_net_selftest_run(g_opt.netServer.c_str());
        else if (!std::strcmp(g_opt.mode, "mp-selftest"))   rc = eden_mp_selftest_run(g_opt.netServer.c_str());
        else if (!std::strcmp(g_opt.mode, "mp-replay"))     rc = run_mp_replay();
        else if (!std::strcmp(g_opt.mode, "mp-join"))       rc = run_mp_join();
        else if (!std::strcmp(g_opt.mode, "photo-selftest")) rc = run_photo_selftest();
        else if (!std::strcmp(g_opt.mode, "trailer-selftest")) rc = run_trailer_selftest();
        else if (!std::strcmp(g_opt.mode, "signs-selftest")) rc = run_signs_selftest();
        else if (!std::strcmp(g_opt.mode, "camera-selftest")) rc = run_camera_selftest();
        else if (!std::strcmp(g_opt.mode, "sign-shot"))     rc = run_sign_shot();
        else if (!std::strcmp(g_opt.mode, "scene-shot"))    rc = run_scene_shot();
        else if (!std::strcmp(g_opt.mode, "render-selftest")) rc = run_render_selftest();
        else if (!std::strcmp(g_opt.mode, "lit-oracle")) rc = run_lit_oracle();
        else if (!std::strcmp(g_opt.mode, "cmds-selftest")) rc = run_cmds_selftest();
        else if (!std::strcmp(g_opt.mode, "console-selftest")) rc = run_console_selftest();
        else if (!std::strcmp(g_opt.mode, "light-selftest")) rc = run_light_selftest();
        else if (!std::strcmp(g_opt.mode, "evict-selftest")) rc = run_evict_selftest();
        else if (!std::strcmp(g_opt.mode, "ring-light-selftest")) rc = run_ring_light_selftest();
        else if (!std::strcmp(g_opt.mode, "shadow-selftest")) rc = run_shadow_selftest();
        else if (!std::strcmp(g_opt.mode, "classic-selftest")) rc = run_classic_selftest();
        else if (!std::strcmp(g_opt.mode, "props-selftest")) rc = run_props_selftest();
        else if (!std::strcmp(g_opt.mode, "codec-bench"))   rc = eden_codec_bench(g_opt.codecFile.c_str(), g_opt.height, 500);
        else if (!std::strcmp(g_opt.mode, "creature-selftest")) rc = eden_creature_selftest_run(eden_platform_bundle_root());
        else if (!std::strcmp(g_opt.mode, "emod-selftest"))
            rc = eden_emod_selftest_run(g_opt.emodFixtures.c_str(), g_opt.emodWork.c_str(), g_opt.emodQuick ? 1 : 0,
                                        g_opt.emodFile.c_str(), g_opt.emodRef.c_str());
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
        // (Settings.cpp's eden_get_fps_cap), but it is applied as PACING — sleep until the next
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
        if (!g_opt.liveCmds.empty()) eden_live_commands_init(g_opt.liveCmds.c_str());
        if (!g_opt.mpJoin.empty()) mp_join_from_args();   // MP.3b: straight from the main menu to a server
        // FT.15a --power-exp lever (off by default): menu30.
        while (g_running) {
            power_frame();
            int capFps = eden_get_fps_cap();
            if (eden_power_exp(EDEN_PEXP_MENU30) && power_game_mode() != GAME_MODE_PLAY && (capFps == 0 || capFps > 30))
                capFps = 30;
            if (capFps > 0) {
                const double interval = 1000.0 / (double)capFps;
                const double now = eden_platform_now_ms();
                // First frame, a cap change, or a stall (a slow load, a backgrounded app): restart
                // the schedule rather than rushing frames out to catch up. The early side allows the
                // slack: a frame that wakes kPaceSlackMs early and finishes in under that (a cheap menu
                // frame) is on schedule, and restarting there skipped every other sleep (FT.15c:
                // menu30 ran ~58 fps on the Air 2).
                if (nextFrameMs == 0.0 || now - nextFrameMs > interval ||
                    nextFrameMs - now > interval + kPaceSlackMs)
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
            eden_live_commands_tick();
            eden_look_trace_frame();   // L.0: after the pump, before World::update reads the touches
            if (g_app->isAnimating()) g_app->drawFrame(true);
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
