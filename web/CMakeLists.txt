# web/CMakeLists.txt — Eden: World Builder web port build.
#
# Reads tools/engine-sources.txt (the AUTHORITATIVE 81-file list the Eden.xcodeproj Sources
# phase compiles, per CLAUDE.md: "only files in Eden.xcodeproj are built") and compiles all of
# them EXCEPT the platform seam (excluded below, each with its rationale), plus this port's own
# seam replacements / Foundation+GL shims / entry point.
#
# STATUS (see docs/PORT-STATUS.md for the full log, and web/docs/RESUME-HERE.md for current pass):
# this is long since resolved and playable, not a status to re-derive. emsdk 3.1.74 is installed
# (pass 4, gitignored at web/emsdk/) via `emcmake cmake -B build-st && cmake --build build-st`
# (the actually-runnable single-thread target — see EDEN_THREADED below and RESUME-HERE.md's
# "Running it"). Emscripten ships no Objective-C runtime, so this port hand-writes one against
# the GNU gnustep-1.9 ABI (src/shim/objc/) — settled since pass ~6, see PORT-STATUS.md's
# "Distilled hard-won knowledge" for the ABI measurements, not the objc/runtime.h TODO this
# comment used to point at. See web/cmake/README.md for the exact toolchain invocation + hosting
# requirements.
#
# Per WORKING/web-port-plan.md Phase 0 "Toolchain baseline": CMake (not Xcode-project-file
# reuse), reading the exact Eden.xcodeproj file list, -O2/-O3 + -sALLOW_MEMORY_GROWTH +
# source maps for debug.

cmake_minimum_required(VERSION 3.20)
project(EdenWeb LANGUAGES C CXX)

set(EDEN_PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(EDEN_CLASSES_DIR "${EDEN_PARENT_DIR}/Classes")

# ---------------------------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------------------------

# D1 (web-port-plan.md): run the engine in a Web Worker with real pthreads + OffscreenCanvas +
# WASMFS/OPFS sync-access, gated behind COOP/COEP headers (see web/cmake/README.md). The
# single-threaded fallback (default) is the plan's documented escape hatch if OffscreenCanvas/
# COOP-COEP prove unworkable on a target browser: single-threaded build on the main thread, OPFS
# via WASMFS's async access, load runs synchronously with a progress yield. Kept as a build flag
# (not a source fork) specifically so this stays cheap to flip — per the plan: "Keep the code
# thread-agnostic so this stays a build flag, not a rewrite."
#
# Audit row A1 (project-audit-2026-07-30.md): this option defaulted ON despite there being no
# working host page for it — worker-bootstrap.js was a 38-line sketch and index.html was
# scaffolding, both deleted per audit row I5 (never ran against a real build; re-sketch when C1
# actually starts), and the OffscreenCanvas handshake in eden_main.cpp is still TODO D1. Every
# build anyone actually
# runs (build-st, build-rel) passes -DEDEN_THREADED=OFF explicitly; a fresh clone running the
# obvious `cmake ..`/`emcmake cmake -B build` got a target that links but cannot be played. The ON
# path is real, in-development work (audit row C1, Opus 5 xhigh, 3-6 weeks) — it is not what a
# first-time builder should get by omitting a flag they don't know exists yet. Default is now OFF;
# pass -DEDEN_THREADED=ON explicitly to build the in-development threaded target.
option(EDEN_THREADED "Build with pthreads + OffscreenCanvas + PROXY_TO_PTHREAD (D1, IN DEVELOPMENT — no working host page yet, see audit row C1). Default OFF = single-thread build, the only one with a runnable host page today." OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Perf audit item Q7/C8: the scripted-drive test harness (eden_debug_tick, DebugState_web.mm's
# eden_debug_menu_state()/eden_debug_player_state()/etc., the [eden-p1]/[eden-gl-dbg] fprintf
# probes, and the per-draw glGetError() call gl_es1_shim.cpp does for the first 5 frames) is
# load-bearing for headless `node eden.js` regression checks (see docs/RESUME-HERE.md "Running
# it") but has no business shipping in a build meant to be played — glGetError() is a pipeline
# stall and the prints are pure noise. Default it to whatever CMAKE_BUILD_TYPE was BEFORE this
# point (Debug for build-st, Release for build-rel unless overridden), so both existing build
# trees keep their current behavior with no cache change required; override explicitly with
# -DEDEN_DIAGNOSTICS=ON/OFF for anything else (e.g. a diagnostics-enabled Release build).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(EDEN_DIAGNOSTICS_DEFAULT ON)
else()
  set(EDEN_DIAGNOSTICS_DEFAULT OFF)
endif()
option(EDEN_DIAGNOSTICS "Compile the scripted-drive debug probes (eden_debug_tick, DebugState_web.mm, [eden-p1]/[eden-gl-dbg] prints, per-draw glGetError). Default ON for Debug, OFF otherwise." ${EDEN_DIAGNOSTICS_DEFAULT})

# ---------------------------------------------------------------------------------------------
# Authoritative engine source list
# ---------------------------------------------------------------------------------------------
# The read/filter/MeshPool.mm-append logic that used to live inline here now lives in
# cmake/EngineSources.cmake's eden_collect_engine_sources() (Phase N Stage 0.1,
# WORKING/native-migration-plan-2026-09-04.md), shared with the future native/CMakeLists.txt.
# This target's own EDEN_SEAM_EXCLUDE list (below, unchanged) is passed in rather than moved,
# since the exclusion rationale is per-target.
include("${EDEN_PARENT_DIR}/cmake/EngineSources.cmake")

# --- Platform seam: files to EXCLUDE from the 81 and replace with src/seam/ / src/entry/
# equivalents instead (per web-port-plan.md principle #2 and docs/PORT-STATUS.md's seam
# classification). Each exclusion is commented with WHY, not just what.
set(EDEN_SEAM_EXCLUDE
  # --- Surface + loop + touches: replaced by src/seam/EAGLView_web.mm,
  # EdenViewController_web.{h,cpp}, EdenAppDelegate_web.{h,cpp}, src/entry/eden_main.cpp.
  # No CAEAGLLayer/UIApplicationMain/CADisplayLink equivalent exists on web; Stage P2/D1 owns
  # the real canvas/worker loop instead.
  "Classes/EAGLView.mm"
  "Classes/EdenViewController.mm"
  "Classes/EdenAppDelegate.mm"
  "main.m"

  # --- Raster: Texture2D.mm decodes images via UIImage/CoreGraphics and rasterizes strings via
  # UIFont — no web equivalent exists. REPLACED (pass 11) by src/seam/Texture2D_web.mm, following
  # the SAME "reuse the original .h, replace only the .mm" pattern documented in
  # src/seam/EAGLView_web.mm's header comment (Texture2D.h is included by 8+ other engine headers
  # per a grep done in pass 2 — kept as-is, only Texture2D.mm is excluded here). The replacement
  # decodes PNGs via stb_image instead of UIImage/CGBitmapContext (see Texture2D_web.mm's header
  # comment for what was ported verbatim vs. deliberately deferred to P2b).
  "Classes/Texture2D.mm"

  # --- File I/O: NSFileHandle/NSFileManager-based append-only .eden format I/O (per
  # docs/eden-file-format.md, docs/save-load.md). STAGE P4 (pass 22): FileManager.mm and
  # FileManagerHelper.mm are NO LONGER EXCLUDED — the REAL engine files are compiled as-is,
  # because the Foundation file-I/O surface they use (NSFileHandle/NSFileManager/NSBundle/
  # NSData/NSSearchPathForDirectoriesInDomains) is already fully shimmed over stdio+OPFS
  # (src/shim/foundation/), so reusing the original tested RLE-column / directory / creature
  # logic is far lower-risk than reimplementing it. Their save path writes to /documents (WASMFS,
  # OPFS-backed in the threaded build); default-world terrain streams from the bundled Eden.eden
  # (preloaded to /bundle/Eden.eden below, resolved via NSBundle). The only link cost was a few
  # CoreGraphics symbols in the DEAD-in-this-build loadGenFromDisk() (JUST_TERRAIN_GEN only) —
  # stubbed in src/shim/foundation/uikit_stubs.{h,mm}. FileArchive.mm stays excluded: both files
  # reference it only in commented-out compress/decompress code (grep-verified), so no link dep.
  "Classes/FileArchive.mm"

  # --- Audio: CocosDenshion/OpenAL façade. TODO P5: SimpleAudioEngine_web.mm over Emscripten's
  # OpenAL-over-WebAudio (first attempt) or a small hand-written Web Audio shim if gaps appear.
  # NOT YET REPLACED this pass.
  "Classes/SimpleAudioEngine.mm"

  # --- Networking: NSURLConnection-based world sharing (docs/networking.md). TODO P6: fetch/
  # XMLHttpRequest-based replacements; audit finding H4 (shared-list parser bounds check) lands
  # here per the plan. NOT YET REPLACED this pass — feature-flagged off is an acceptable P6
  # outcome per the plan ("Feature-flag Shared Worlds off if the endpoint can't be reached").
  "Classes/FileDownload.mm"
  "Classes/FileUpload.mm"
  "Classes/SharedList.mm"
  "Classes/ShareUtil.mm"
  "Classes/ShareMenu.mm"
  "Classes/Alert.mm"

  # --- Strip entirely (CLAUDE.md L12 / plan Stage P7): rating-prompt nag + analytics remnants.
  # No web replacement — the one caller (Classes/EdenAppDelegate.mm) is itself excluded above,
  # and grep confirms no other file references Appirater (see foundation-usage.md).
  "Classes/Appirater.mm"

  # --- Crypto/CF file hashing: md5.c is 100 lines of Apple plumbing (CFReadStreamCreateWithFile,
  # CFURLCreateWithFileSystemPath, CFStringCreateWithCString) wrapped around CommonCrypto's
  # CC_MD5_*. Replaced by src/shim/foundation/md5_web.mm, which re-expresses the SAME function
  # (Classes/md5.h is untouched and still declares it) over stdio plus a real RFC-1321 MD5 —
  # shimming CFReadStream for one call site would have been the larger change. See that file's
  # header comment for why the digest has to be genuine rather than stubbed.
  "Classes/md5.c"

  # --- JUDGMENT CALL made THIS pass (see docs/PORT-STATUS.md "Decisions" for the rationale in
  # full): VKeyboard.mm was listed in the original seam classification as "verify during P1",
  # not excluded. A grep this pass found it calls a real UIView method
  # (`[G_EAGL_VIEW insertSubview:atIndex:]`, Classes/VKeyboard.mm:85) to overlay a native
  # UITextField for world-renaming text entry — there is no such native-overlay concept on web
  # without a real DOM, so it's reclassified here as excluded/seam-replaced. TODO P3: a real
  # HTML <input> positioned over the canvas is the natural web equivalent, matching the
  # ORIGINAL's own "real native text field over the GL view" approach in spirit (the D1 worker
  # sketch this comment used to point at was deleted per audit row I5 — it never ran and predates
  # the eventual D1 host page, which doesn't exist yet either). NOT YET REPLACED this
  # pass — no VKeyboard_web stub exists yet (out of this pass's scope, tracked as a P3 TODO).
  "Classes/VKeyboard.mm"
)
# statusbar.mm and Gamepad.mm were ALSO checked this pass (grepped for UIView/addSubview/
# insertSubview/UITextField coupling) and found clean — real GL-drawn UI + input polling only,
# no native-overlay dependency. They stay classified as ordinary ENGINE files (compiled as-is,
# not excluded) per the original "verify during P1" note; verification = done, result = keep.

eden_collect_engine_sources(
  OUT_VAR EDEN_ENGINE_SOURCES
  SOURCES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tools/engine-sources.txt"
  BASE_DIR "${EDEN_PARENT_DIR}"
  EXCLUDE_LIST ${EDEN_SEAM_EXCLUDE}
)

# ---------------------------------------------------------------------------------------------
# This port's own sources
# ---------------------------------------------------------------------------------------------
set(EDEN_SHIM_FOUNDATION_SOURCES
  src/shim/foundation/NSObject.mm
  src/shim/foundation/NSAutoreleasePool.mm
  src/shim/foundation/NSString.mm
  src/shim/foundation/NSData.mm
  src/shim/foundation/NSNumber.mm
  src/shim/foundation/NSArray.mm
  src/shim/foundation/NSDate.mm
  src/shim/foundation/NSFileHandle.mm
  src/shim/foundation/NSFileManager.mm
  src/shim/foundation/NSThread.mm
  src/shim/foundation/NSBundle.mm
  src/shim/foundation/NSUserDefaults.mm
  src/shim/foundation/NSErrorException.mm
  src/shim/foundation/NSTimer.mm
  src/shim/foundation/NSOperation.mm
  src/shim/foundation/NSLogSupport.mm
  src/shim/foundation/uikit_stubs.mm
  src/shim/foundation/CoreFoundation.mm
  src/shim/foundation/md5_web.mm
  # Phase N Stage 2: the previous-save ".bak" rules, lifted out of NSFileHandle.mm (which still
  # calls them, at the same two places) so the native target can run the identical implementation
  # from its own engine hook. Portable C++; see save_backup.h for why native needs a hook at all.
  src/shim/foundation/save_backup.cpp
  # NSURLConnection.h has no .mm — P6, header-only stub this pass (see foundation-usage.md).
)

# The Objective-C runtime itself (web-port-plan.md blocker #1, option (c)). Emscripten ships no
# libobjc, so without this NOTHING that clang emits for a .mm file links: class registration
# (__objc_exec_class) and message dispatch (objc_msg_lookup_sender/objc_slot_lookup_super) are
# both undefined otherwise. Plain C++ on purpose — it implements the ABI rather than using it, so
# it must NOT be added to EDEN_MM_SOURCES below. Read src/shim/objc/objc_abi.h before touching it:
# every struct layout there was measured off real emcc codegen, not recalled.
set(EDEN_SHIM_OBJC_SOURCES
  src/shim/objc/objc_runtime.cpp
)

# Stage P5 placeholder: failing AudioToolbox stubs so the P1 headless link can happen without a
# CAF/WAV decoder. See the file's header comment for why FAILING (not no-op-success) is the safe
# choice here.
set(EDEN_SHIM_AUDIO_SOURCES
  src/shim/audio/audiotoolbox_stub.cpp
)

# Plain-C++ platform primitives (arc4random) — not .mm, see src/shim/foundation/platform_shims.h.
set(EDEN_SHIM_PLATFORM_SOURCES
  src/shim/foundation/platform_shims.cpp
)

set(EDEN_SHIM_GL_SOURCES
  # Split in Phase N Stage 0.2 (WORKING/native-migration-plan-2026-09-04.md): gl_fixed_function.cpp
  # is the portable ES1->GL translator (cmake-shared with native); gl_context_web.cpp is the
  # ~40 lines of genuine Emscripten context-lifecycle coupling that native replaces with its own
  # gl_context_native.cpp against the same gl_es1_shim.h / gl_shim_internal.h interface.
  src/shim/gl/gl_fixed_function.cpp
  src/shim/gl/gl_context_web.cpp
)

set(EDEN_SEAM_SOURCES
  src/seam/EAGLView_web.mm
  src/seam/DisplayProfile_web.mm  # audit D1+D4: the device profile table + the derived (no longer pinned) point space. See file header.
  src/seam/EdenViewController_web.cpp
  src/seam/EdenAppDelegate_web.cpp
  src/seam/main_web.cpp
  src/seam/seam_link_stubs.mm
  src/seam/Texture2D_web.mm
  src/seam/Input_web.mm
  src/seam/pvrt_matrix_palette.cpp  # --wrap: installs the shim's GL_OES_matrix_palette entry points. See file header.
  src/seam/Movement_web.mm  # PC controls audit F1/F2 --wrap of Player::setSpeed (frame-rate-normalized walk speed). See file header.
  src/seam/Settings_web.mm  # settings model + --wrap of the old GL settings panel and gluPerspective. See file header.
  src/seam/Storage_web.mm  # local-storage panel data source (world list/size/mtime, delete). See file header.
  src/seam/Menu_web.mm  # C accessors the DOM main menu (public/eden-menu.js) drives Menu.mm through. NOT a replacement — see file header.
  src/seam/LoadFailure_web.mm  # C4: corrupt/truncated-save signal for FileManager.mm's loadWorld() + .bak restore. See file header.
  src/seam/AppLifecycle_web.mm  # Phase N Stage 4.2: eden_app_will_background() — save-on-background. Portable, cmake-shared with native. See file header.
  src/seam/MeshTiming_web.mm  # row 36/C1: measures mesh-CPU vs VBO-upload cost per chunk, --wrap of TerrainChunk::rebuild2/prepareVBO. See file header.
  src/seam/HeapProbe_web.mm  # ROADMAP Phase M / M0: eden_debug_heap() — heap high-water readable in Release + threaded. See file header.
  src/seam/SimpleAudioEngine_web.mm  # P5 audio (Web Audio + streaming music). See file header.
  # P4 landed by un-excluding the REAL FileManager.mm/FileManagerHelper.mm (see EDEN_SEAM_EXCLUDE
  # above) rather than writing FileManager_web.mm — no seam replacement needed here.
  # TODO P5/P6: SimpleAudioEngine_web.mm,
  # FileDownload_web.mm/FileUpload_web.mm/SharedList_web.mm/ShareUtil_web.mm/ShareMenu_web.mm/
  # Alert_web.mm, VKeyboard_web.mm — add each here as its stage lands (see EDEN_SEAM_EXCLUDE
  # comments above for what each replaces).
)

# Q7/C8: DebugState_web.mm (eden_debug_menu_state/player_state/hud_rects/alloc_state/…) is only
# ever called manually from a scripted `node`/console drive (grep-verified: nothing in public/*.js
# or eden-st.html references any eden_debug_* export), so it is safe to leave out of the binary
# entirely rather than just gating its prints.
if(EDEN_DIAGNOSTICS)
  list(APPEND EDEN_SEAM_SOURCES src/seam/DebugState_web.mm)
  # Audit row F5 — the dev console (teleport/spawn/world-stats) is deliberately gated the same
  # way: a shipped build should not let a player teleport or spawn creatures at will.
  list(APPEND EDEN_SEAM_SOURCES src/seam/DevConsole_web.mm)
  # Audit row 21/I6 — real @interface/@implementation test classes exercising objc_runtime.cpp's
  # dispatch (slot lookup, ivar layout, super, category merging); see the file's own header for
  # why this needs to be genuine ObjC compiled through the same frontend, not a mock. A shipped
  # build has no reason to carry it, same reasoning as the two sources above.
  list(APPEND EDEN_SEAM_SOURCES src/shim/objc/objc_selftest.mm)
endif()

# ROADMAP Phase M / M6 — the per-world-load leak bisect instrument. Interposes malloc/free on top
# of emmalloc's weak symbols and keeps a live-allocation histogram + a big-allocation table
# (src/seam/AllocTrace_web.c). Separate option from EDEN_DIAGNOSTICS on purpose: it replaces the
# allocator entry points, it is single-threaded-only, and it perturbs what it measures, so it must
# never be on by accident. Build a scratch tree with -DEDEN_ALLOC_TRACE=ON to use it.
option(EDEN_ALLOC_TRACE "Interpose malloc/free to bisect allocation leaks (Phase M / M6). Single-threaded scratch builds only — never ship." OFF)
if(EDEN_ALLOC_TRACE)
  if(EDEN_THREADED)
    message(FATAL_ERROR "EDEN_ALLOC_TRACE uses non-atomic counters; it is single-threaded-only (see src/seam/AllocTrace_web.c).")
  endif()
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    # It forwards to emmalloc_* by name, and -sMALLOC=emmalloc is a Release-only link flag below.
    message(FATAL_ERROR "EDEN_ALLOC_TRACE needs -DCMAKE_BUILD_TYPE=Release (emmalloc is Release-only here).")
  endif()
  list(APPEND EDEN_SEAM_SOURCES src/seam/AllocTrace_web.c)
endif()
# --profiling-funcs keeps the wasm name section, without which AllocTrace's captured callstacks are
# a list of `wasm-function[1234]` indices. Applied at the bottom of this file with the other link
# options (see the EDEN_ALLOC_TRACE entry there).

# ---- case-mismatched #import shims -----------------------------------------------------------
# macOS's default filesystem is case-INsensitive, so Classes/*.mm quoting the wrong case for a
# sibling header (e.g. Camera.h: `#import "Glu.h"` when the real file is `glu.h`) has always
# silently resolved on the Xcode/simulator build. Linux (every CI runner) is case-sensitive and
# fails with "file not found" — grep-verified this is every such mismatch in Classes/ today.
# Never edit Classes/*.h to fix the quoted name (see the no-Classes-edits rule above); instead
# create the missing case-variant as a relative symlink, same trick as the audio/Eden.eden
# symlinks below. Harmless no-op on macOS (the target already resolved); required on Linux.
foreach(pair IN ITEMS "Glu.h=glu.h" "input.h=Input.h" "terrain.h=Terrain.h" "Vkeyboard.h=VKeyboard.h" "GLobals.h=Globals.h")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 wrong_case)
  list(GET pair 1 real_file)
  if(NOT EXISTS "${EDEN_CLASSES_DIR}/${wrong_case}")
    file(CREATE_LINK "${real_file}" "${EDEN_CLASSES_DIR}/${wrong_case}" SYMBOLIC)
  endif()
endforeach()

# ---- P5 audio assets -------------------------------------------------------------------------
# ~45 MB of .caf/.mp3/.wav under ../media. Deliberately NOT --preload-file'd: that would bake all
# of it into eden.data, more than doubling the initial download, and none of it is needed before
# the first sound plays. It is served over HTTP and fetched on demand instead (see
# src/seam/SimpleAudioEngine_web.mm). Two pieces of glue are generated here:
#   * public/audio -> ../media symlink, so the dev server can reach the assets from the page's
#     own origin (python3 -m http.server follows symlinks).
#   * public/audio-manifest.json, mapping the BARE filenames the engine passes ("explosion.caf",
#     "Eden_title.mp3") to their media/ subdirectory. The iOS app bundle is flat, so the engine
#     never had to name a directory; on the web the media/ tree keeps its structure.
file(CREATE_LINK "${EDEN_PARENT_DIR}/media" "${CMAKE_CURRENT_SOURCE_DIR}/public/audio" SYMBOLIC)

# ---- lazy Eden.eden fetch (pass 30) ---------------------------------------------------------
# The pre-generated default world (~52 MB, repo-root Eden.eden) used to be --preload-file'd into
# eden.data, meaning first paint was gated on downloading it even though it is only ever read
# lazily by the engine itself (fmh_readColumnFromDefault streams individual 32 KB column records
# on demand as the player walks into ungenerated terrain — see docs/world-and-terrain.md). Instead
# symlink it into public/ (same trick as the audio symlink above) so it's reachable over HTTP from
# the page's own origin, and populate it into /bundle/Eden.eden via a --pre-js Module.preRun run
# dependency (src/seam/js/eden_default_world.pre.js) that does a fetch() in the browser or
# fs.readFileSync under `node eden.js`. NOT `FS.createLazyFile` — that throws outside a Web Worker
# on this emsdk version ("Cannot do synchronous binary XHRs outside webworkers in modern
# browsers"), so this is "fetched separately, concurrently with the small asset package," not a
# true byte-range-lazy read; see the .pre.js file's own header and perf-audit item §5b.1 for why
# a real range-fetch needs either the threaded build or a custom FS node. This paragraph itself
# used to describe the abandoned FS.createLazyFile approach — corrected per perf-audit Q9.
file(CREATE_LINK "${EDEN_PARENT_DIR}/Eden.eden" "${CMAKE_CURRENT_SOURCE_DIR}/public/Eden.eden" SYMBOLIC)
file(GLOB_RECURSE EDEN_AUDIO_FILES
     RELATIVE "${EDEN_PARENT_DIR}/media"
     "${EDEN_PARENT_DIR}/media/*.caf" "${EDEN_PARENT_DIR}/media/*.mp3"
     "${EDEN_PARENT_DIR}/media/*.wav")
list(SORT EDEN_AUDIO_FILES)  # deterministic winner when a basename appears in two directories
set(EDEN_AUDIO_MANIFEST "{")
set(EDEN_AUDIO_SEEN "")
foreach(rel IN LISTS EDEN_AUDIO_FILES)
  get_filename_component(base "${rel}" NAME)
  if(NOT base IN_LIST EDEN_AUDIO_SEEN)
    list(APPEND EDEN_AUDIO_SEEN "${base}")
    string(APPEND EDEN_AUDIO_MANIFEST "\"${base}\":\"${rel}\",")
  endif()
endforeach()
string(REGEX REPLACE ",$" "" EDEN_AUDIO_MANIFEST "${EDEN_AUDIO_MANIFEST}")
string(APPEND EDEN_AUDIO_MANIFEST "}")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/public/audio-manifest.json" "${EDEN_AUDIO_MANIFEST}")
list(LENGTH EDEN_AUDIO_SEEN EDEN_AUDIO_COUNT)
message(STATUS "eden: audio manifest lists ${EDEN_AUDIO_COUNT} sound files")

set(EDEN_ENTRY_SOURCES
  src/entry/eden_main.cpp
)

set(EDEN_ALL_SOURCES
  ${EDEN_ENGINE_SOURCES}
  ${EDEN_SHIM_FOUNDATION_SOURCES}
  ${EDEN_SHIM_OBJC_SOURCES}
  ${EDEN_SHIM_AUDIO_SOURCES}
  ${EDEN_SHIM_PLATFORM_SOURCES}
  ${EDEN_SHIM_GL_SOURCES}
  ${EDEN_SEAM_SOURCES}
  ${EDEN_ENTRY_SOURCES}
)

add_executable(eden ${EDEN_ALL_SOURCES})

# ST-build-only synchronous pthread_create shim (src/seam/pthread_sync_web.c). The engine's sole
# thread is the world-load thread (Classes/World.mm, pthread_create → loadWorldThread, which sets
# doneLoading=2). In the non-pthread build Emscripten's stub pthread_create never runs the routine,
# so loadWorld() hangs forever at doneLoading==1 — the "clicking to load a world freezes" bug. This
# override runs the routine synchronously. It MUST NOT link into the threaded build (it would
# collide with real pthreads) and MUST stay plain C — NOT in EDEN_MM_SOURCES, so it never gets the
# -x objective-c++ / prefix-header treatment applied to .mm sources below.
if(NOT EDEN_THREADED)
  target_sources(eden PRIVATE src/seam/pthread_sync_web.c)
endif()

# ---------------------------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------------------------
# ORDER MATTERS for the two framework/ dirs: they provide fake `<Foundation/Foundation.h>`,
# `<UIKit/UIKit.h>`, `<OpenGLES/ES1/gl.h>`, `<OpenGLES/ES1/glext.h>`, `<OpenGLES/EAGL.h>`, and
# `<Availability.h>` — angle-bracket includes only (see gl_es1_shim.h's framework/OpenGLES/
# ES1/gl.h for why quoted "X.h" includes can't be redirected this way; that's why the seam .mm
# files reach into ../../../Classes/*.h with explicit relative paths instead of relying on -I).
target_include_directories(eden PRIVATE
  src/shim/foundation/framework
  src/shim/gl/framework
  src/shim/foundation
  src/shim/gl
  src/shim/vendor            # stb_image.h/stb_truetype.h — P2 Texture2D_web.mm's PNG decoder.
  "${EDEN_CLASSES_DIR}"      # lets Classes/*.mm's quoted #import "X.h" resolve sibling headers
                              # (same-directory lookup already covers most of this; explicit
                              # for clarity and as a safety net)
  "${EDEN_PARENT_DIR}"        # a few Classes/*.mm files quote-include ROOT-level headers, e.g.
                              # Classes/Terrain.mm: `#import "Lighting.h"` where Lighting.h
                              # lives at the repo root, not in Classes/ — grep-verified this pass.
)

# ---------------------------------------------------------------------------------------------
# Objective-C++ compilation (D3a) — .mm files only
# ---------------------------------------------------------------------------------------------
# CMake does not auto-detect .mm as a language without enabling OBJCXX, which has spotty
# support in custom (non-Apple-Clang) toolchain files like Emscripten's — so every .mm source is
# forced to compile as CXX and the ObjC-specific flags are layered on as COMPILE_OPTIONS.
#
# *** BUT `LANGUAGE CXX` ALONE IS NOT ENOUGH — it does NOT enable the Objective-C frontend. ***
# (An earlier revision of this comment claimed "clang dispatches on content, not the CMake
# LANGUAGE property". That is wrong, and it cost ~900 of the 992 errors in the fourth real
# build.) Driven as `em++ file.mm`, clang compiles the file as plain C++: `__OBJC__` is never
# defined and every `@interface` in the Foundation shim fails to parse with "expected external
# declaration". It also silently breaks the ENGINE, because the unmodified ../Eden_Prefix.pch
# wraps its Foundation/UIKit imports in `#ifdef __OBJC__` — so .mm files were getting no
# Foundation at all (that's why Classes/hashmap.mm reported malloc/calloc/NULL undeclared).
# `-x objective-c++` is what actually selects the frontend; it must come BEFORE the source file,
# which COMPILE_OPTIONS guarantees.
set(EDEN_MM_SOURCES ${EDEN_SHIM_FOUNDATION_SOURCES} ${EDEN_SEAM_SOURCES})
foreach(src IN LISTS EDEN_ENGINE_SOURCES)
  if(src MATCHES "\\.mm$")
    list(APPEND EDEN_MM_SOURCES ${src})
  endif()
endforeach()
set_source_files_properties(${EDEN_MM_SOURCES} PROPERTIES LANGUAGE CXX)

# D3(a): "target clang's GNU objc runtime (-fobjc-runtime=gnustep-2.0 ...) and provide a small
# hand-written Foundation shim". `-fno-objc-arc`: CLAUDE.md convention #6 — this engine is
# manual retain/release throughout, predates ARC; do not enable it.
#
# `-fconstant-string-class=NSConstantString`: the engine uses `@"literal"` syntax 743 times
# across non-seam Classes/*.mm, and clang lowers each to a STATICALLY EMITTED instance of the
# class named here, with an ivar layout it hard-codes as `{Class isa; const char *cString;
# unsigned length;}`. src/shim/foundation/NSString.h implements exactly that class as part of a
# GNUstep-style class cluster (zero-ivar NSObject + zero-ivar abstract NSString + this class +
# a concrete std::string-backed EdenConcreteString), which is what makes the layout safe.
# Read NSString.h's header comment and web-port-plan.md's "D3 refinement" section before
# changing ANY of these three flags — they are a matched set, not independent knobs.
#
# STILL UNPROVEN until a real link runs (P0.1): (1) whether the gnustep-2.0 v2 ABI expects a
# wider/flagged layout than the classic three-word one assumed above, and (2) whether this
# target honors -fconstant-string-class at all or hard-requires the name `NXConstantString`
# (if the latter: rename the class in NSString.h/.mm and alias NSConstantString to it).
# *** P0.1 SPIKE RESULT (2026-07-19, first real emcc run) — gnustep-2.0 DOES NOT WORK. ***
# `-fobjc-runtime=gnustep-2.0` fails at compile time with "GNUstep Objective-C runtime version 2
# incompatible with target binary format" (46 occurrences, the single dominant error in the first
# build). The v2 ABI requires ELF/COFF section features wasm doesn't provide. Measured
# alternatives, all compiled against a one-class test .mm under emcc 3.1.74:
#     gnustep-2.0  FAILS (binary format)   gnustep-1.9  COMPILES  <-- chosen
#     gnustep-1.8  COMPILES                gcc          COMPILES
#     objfw        COMPILES
# gnustep-1.9 is the newest ABI that works, so it's the least-regression choice.
#
# ALSO CONFIRMED BY THAT SPIKE: `-fconstant-string-class=NSConstantString` IS honored under
# gnustep-1.9, and the three-word `{isa, const char *, unsigned}` layout NSString.h implements
# compiles clean against a real `@"..."` literal. That closes open question (2) from NSString.h's
# header comment — the cluster design is compile-validated, not just reasoned.
set(EDEN_OBJCXX_FLAGS
  -x objective-c++          # selects the ObjC frontend — see the LANGUAGE CXX note above
  -fobjc-runtime=gnustep-1.9
  -fno-objc-arc
  -fconstant-string-class=NSConstantString
)
set_source_files_properties(${EDEN_MM_SOURCES} PROPERTIES COMPILE_OPTIONS "${EDEN_OBJCXX_FLAGS}")

# ---------------------------------------------------------------------------------------------
# Preprocessor definitions carried over from the Xcode target
# ---------------------------------------------------------------------------------------------
# *** THESE ARE NOT THIS PORT'S INVENTION — they are settings the shipped build already had, and
# missing them silently changes which code the vendored PVRT SDK compiles. *** Found by grepping
# Eden.xcodeproj/project.pbxproj, which sets:
#     GCC_PREPROCESSOR_DEFINITIONS_NOT_USED_IN_PRECOMPS = ( TARGET_OS_IPHONE, BUILD_OGLES, );
#
#  * BUILD_OGLES gates `#include "PVRTFixedPointAPI.h"` in Classes/PVRTFixedPoint.h, which is the
#    ONLY path by which Classes/PVRTContext.h — and therefore PVRTglesExt.h, and therefore the
#    class CPVRTglesExt — reaches Classes/Model.mm. Without it, Model.mm fails with "unknown type
#    name 'CPVRTglesExt'" and the include chain gives no hint why (the chain is
#    Model.mm → PVRTModelPOD.h → PVRTVector.h → PVRTFixedPoint.h → [gated] → PVRTFixedPointAPI.h
#    → PVRTContext.h → PVRTglesExt.h).
#  * TARGET_OS_IPHONE selects the iOS branch in several PVRT headers and in Classes/Sound.m's
#    AudioFileOpenURL call. It is normally defined by Apple's <TargetConditionals.h>; on this
#    target nothing defines it, so the project's own explicit definition is what has to be
#    reproduced. Note this does NOT make __APPLE__ true — the PVRT headers test
#    `#if defined(__APPLE__)` FIRST and fall to their generic-GLES branch here, which is why
#    framework/GLES/glext.h and EGL_NOT_PRESENT below exist.
#
# EGL_NOT_PRESENT is this port's own addition rather than Xcode's, and it is the honest value:
# there is no EGL on the web. It makes PVRTContext.h skip `#include <GLES/egl.h>` and makes
# PVRTglesExt.h define `PVRGetProcAddress(x)` as NULL, so every GL-extension function pointer is
# null — correct, since WebGL has none of the OES extensions PVRT looks for. See
# src/shim/gl/framework/OpenGLES/ES1/glext.h for the one consequence worth carrying forward
# (GL_OES_matrix_palette is absent, so creature models do not load until skinning is ported).
target_compile_definitions(eden PRIVATE
  TARGET_OS_IPHONE
  BUILD_OGLES
  EGL_NOT_PRESENT
  # Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): compiles the `-Wl,--wrap=`
  # interposers at the tail of src/seam/{Settings,Movement,MeshTiming}_web.mm. They are guarded
  # because Apple's ld64 has no --wrap, so the native target compiles the portable half of each
  # of those files and nothing else. This definition and the -Wl,--wrap= link flags below are a
  # matched set: remove one without the other and the link fails loudly rather than silently
  # changing behaviour. See the guard comment in any of the three files.
  EDEN_LD_WRAP=1
  $<$<BOOL:${EDEN_DIAGNOSTICS}>:EDEN_DIAGNOSTICS=1>
  $<$<BOOL:${EDEN_ALLOC_TRACE}>:EDEN_ALLOC_TRACE=1>
)

# Pass 4 found ~90 -Wc++11-narrowing errors in unmodified engine constant tables (e.g. values
# like 4294967196 — small negatives stored as unsigned — narrowed into smaller integer types).
# Legal in the original C++98-era Xcode build; this port does not edit engine sources, so silence
# the warning-as-error instead. Applied target-wide rather than per-file; the project declares
# only C/CXX (line 19 above — .mm files are forced to LANGUAGE CXX at line ~225, there is no
# separate OBJCXX language here), and this port's own shim/seam/entry code shouldn't be writing
# narrowing initializers in the first place, so the broad C++ scope is harmless.
target_compile_options(eden PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>
  # Same class of problem, same resolution: Classes/TerrainGenerator.mm's Perlin noise code uses
  # `register int i, j;`, legal C++98 and REMOVED in C++17, which is the standard emcc defaults
  # to. Downgrading the whole build to -std=c++98 to accommodate one keyword would change
  # overload resolution and library behavior across 60+ engine files; suppressing the diagnostic
  # changes nothing but the diagnostic (`register` has been a no-op hint to compilers for
  # decades). Per CLAUDE.md this port does not edit engine sources to satisfy the toolchain.
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
)

# Force-include the UNMODIFIED parent-tree prefix header for ENGINE sources only (not this
# port's own shim/seam/entry files, which manage their own includes explicitly) — reproduces
# Xcode's GCC_PRECOMPILE_PREFIX_HEADER behavior exactly, including the NSLog/printg macros'
# __OPTIMIZE__-gated behavior (see src/shim/foundation/NSLogSupport.h) and the `#import
# <Availability.h>` this needs a trampoline for (src/shim/foundation/framework/Availability.h).
# Per CLAUDE.md, ../Eden_Prefix.pch is read here, never edited.
set(EDEN_ENGINE_MM_SOURCES "")
foreach(src IN LISTS EDEN_ENGINE_SOURCES)
  if(src MATCHES "\\.mm$" OR src MATCHES "\\.m$")
    list(APPEND EDEN_ENGINE_MM_SOURCES ${src})
  endif()
endforeach()
set_source_files_properties(${EDEN_ENGINE_MM_SOURCES} PROPERTIES
  COMPILE_OPTIONS "${EDEN_OBJCXX_FLAGS};-include;${EDEN_PARENT_DIR}/Eden_Prefix.pch"
)

# ---------------------------------------------------------------------------------------------
# Emscripten flags
# ---------------------------------------------------------------------------------------------
if(EMSCRIPTEN)
  target_compile_options(eden PRIVATE
    # Q1 (dazzling-munching-bengio.md perf audit, C1): the build actually served to players was
    # `build-st/`, configured Debug (`-O0 -g -gsource-map`) — every engine hot loop (mesher
    # rebuild2(), Terrain::update, the ObjC dispatch cache, the shim's per-draw mat4_multiply) ran
    # UNOPTIMISED, and no Release build had ever been configured, built, or measured. `-fno-strict-
    # aliasing` per the audit's own risk note: this is 2010-era C/C++ (type punning in the on-disk
    # struct paths, `-Wno-c++11-narrowing` tables, the mesher's counting/fill passes) that has been
    # getting away with UB since -O0 never exploited it. Landing `-O2` (not `-O3`) first is the
    # audit's explicitly recommended lower-risk step — bump to -O3/-Oz only after a real A/B
    # (`node eden.js` P1 tick + a live-Chrome playtest) confirms no regression.
    #
    # Project audit 2026-07-30, row B1 — -flto (both compile- and link-side; the pair must match,
    # see EDEN_LINK_FLAGS below). Pass 53 tried this, hit a hard `Aborted()` partway through menu
    # asset load under `node eden.js`, and reverted it with the root cause unchased. Pass 56 found
    # it, and it was NOT an LTO bug: `NSUserDefaults`' `std::unordered_map` ivar was never
    # constructed, because the hand-written ObjC runtime's class_createInstance() is a bare
    # `calloc` with no `.cxx_construct` support. A zeroed unordered_map has max_load_factor 0.0,
    # so the first insert asks for `ceil(1 / 0.0)` buckets; LTO's cross-TU inlining is simply what
    # let the compiler constant-fold that and commit to libc++'s throw path (= abort, exceptions
    # being off) where plain -O2 had been limping. Latent UB in every build, LTO or not — fixed at
    # source in src/shim/foundation/NSUserDefaults.mm (which carries the full write-up, plus the
    # same-shaped leak found in NSAutoreleasePool.mm). With that fixed, all four headless suites
    # pass against an LTO build. If a future flag ever "breaks" this port again, that file is the
    # precedent: suspect our own latent UB before suspecting the optimiser.
    $<$<CONFIG:Release>:-O2;-fno-strict-aliasing;-flto>
    $<$<CONFIG:Debug>:-O0;-g;-gsource-map>
    # zlib is a PORT, not a shim: Classes/FileArchive/the RLE Eden.eden path include <zlib.h>, and
    # emsdk ships zlib itself, so this is the one "missing header" in the pass-4 list that needed
    # a flag rather than a trampoline. It has to appear on the COMPILE side too (not just link)
    # for the header to be found at all.
    -sUSE_ZLIB=1
  )
  set(EDEN_LINK_FLAGS
    -Wl,--error-limit=0
    -sALLOW_MEMORY_GROWTH=1
    # The engine's STATIC data alone is ~62 MB, so the default 16 MB initial heap cannot even be
    # linked ("initial memory too small, 64928208 bytes needed"). ALLOW_MEMORY_GROWTH does not
    # help — that grows the heap at runtime, whereas this is the size the module starts at, which
    # must already cover static data + stack.
    #
    # WHY SO LARGE, and why this is expected rather than alarming: docs/world-and-terrain.md's
    # resident window is an 18x18-chunk column array, 288x288x64 blocks at 2 bytes each = ~10.6 MB
    # for `blockarray` alone, plus the parallel lighting/colour arrays and the mesher's
    # per-face-direction vertex buckets — all statically allocated C globals (CLAUDE.md: "hot data
    # are C globals"). This is a 2010 iPhone-era fixed budget, not a leak.
    #
    # MEASURED, project-audit row 14 / E1, 2026-07-31 — `node tools/headless-memory-probe.js`, which
    # intercepts the module's own emscripten_resize_heap requests rather than reading buffer sizes
    # back (those only show the size AFTER the growth policy's overshoot). Numbers, with the 2 MB
    # stack below:
    #
    #   linker floor (static data + stack; must fit before main runs) ......  64.28 MB
    #     = 62.30 MB static data + 2 MB stack. Below this the link fails outright with
    #       "initial memory too small, N bytes needed" — N is where 64.28 comes from.
    #   peak heap demand over a full session ......................  112 - 140 MB, run to run
    #     (create world from the bundled Eden.eden -> play -> quit -> reload from the save file)
    #
    # 96 MB IS KEPT, deliberately, after measuring the alternatives — it is not the unmeasured
    # number it used to be. Raising it does not help: the demand above is not a constant, so a
    # bigger initial heap does not buy "no growth", it just raises the floor everyone pays,
    # including the menu-only session and the phone that dies before it ever loads a world. The
    # sweep (peak resident over two runs each, worst case shown):
    #     80 MB -> 166 MB   96 MB -> 144 MB   112 MB -> 144 MB   128 MB -> 154 MB
    #
    # WHAT ACTUALLY MOVED THE PEAK is the growth policy, not the starting point. Emscripten's
    # default growMemory takes max(requested, current * 1.2), so the heap walks a geometric ladder
    # and lands well above the demand that triggered it (96 -> 115.25 -> 138.31 -> 166 MB, for a
    # demand of ~140). A 16 MB linear step tracks the demand instead, which cut worst-case peak
    # resident from 166 MB to 144 MB — ~22 MB of a mobile tab's budget, for one flag. The step is
    # a balance: smaller tracks tighter but each growth rebuilds every JS-side heap view.
    #
    # IF THE INITIAL SIZE IS EVER TOO SMALL, NOTHING BREAKS: growth is still on, so an
    # under-estimate costs a growth event, while an over-estimate is committed memory nobody uses.
    # When in doubt, err low.
    #
    # ROADMAP Phase M / M4, 2026-08-31 — RE-MEASURED and KEPT AT 96 MB. The row asked whether the
    # up-front commit a phone that dies at the menu pays could be trimmed. `headless-memory-probe.js`
    # against a scratch tree at 80 MB initial: growth profile is IDENTICAL to 96 MB (3 events,
    # requests 109/113/128 MB, same 144 MB peak) — the linear step already neutralised the old
    # geometric-overshoot penalty that made the pre-2026 sweep read "80 -> 166 MB". So 80 MB is
    # peak-neutral and would save ~16 MB up-front ONLY for a session whose demand never exceeds
    # 80 MB. Not taken, for two reasons: (1) real menu demand measured 93 MB by
    # `headless-heap-ceiling-probe.js` (the -O0 memory-probe menu phase understates it), so 80 MB
    # regrows to 96 MB immediately anyway; (2) this flag is shared with the THREADED build, where an
    # extra growth event is a realloc-and-copy of a SharedArrayBuffer that invalidates every worker
    # heap view (-Wpthreads-mem-growth). Net: the ~30 MB the row was theoretically worth is really
    # ~0. Closed no-change.
    -sINITIAL_MEMORY=100663296
    -sMEMORY_GROWTH_LINEAR_STEP=16777216
    # ROADMAP Phase M / M1, 2026-08-31. Emscripten's DEFAULT maximum is 2 GB, and leaving it unset
    # bakes `maximum:32768` into the module's WebAssembly.Memory (visible in build-relthr/eden.js).
    # For the THREADED build that is a shared memory, which cannot grow by realloc-and-copy, so the
    # engine reserves the whole 2 GB of ADDRESS SPACE up front. That reservation is the leading
    # suspect for the iPad Air 2 (2 GB RAM) failing to load `?build=thr` at all.
    #
    # WHAT M0 MEASURED, so this is not oversold (WORKING/web-port-memory-plan.md §"M0 results"):
    # on desktop WebKit the reservation is VIRTUAL, not resident — `vmmap` shows the same 4.3 G
    # virtual region whether the maximum is 2 GB or 512 MB, and capping moved the tab's physical
    # footprint by roughly nothing (311 -> 307 MB idle). So this flag is NOT a footprint win on a
    # desktop Mac and must not be claimed as one. It is here for the two cases desktop WebKit does
    # not model: Blink charges committed SharedArrayBuffer differently (the user's ~1.9 GB Chromium
    # reading is exactly the shape of a 2 GB maximum), and a 2 GB device can fail a 2 GB virtual
    # reservation that a 64 GB Mac shrugs off.
    #
    # SIZING — measured, `node tools/headless-heap-ceiling-probe.js` (written for this row; it is
    # the only probe here that reports the 256z ceiling rather than the 64z one):
    #
    #   menu, no world ...............................................  93 MB
    #   64z world loaded + saved .....................................  116 MB
    #   256z world loaded, 5 teleport bursts, save ...................  232 - 240 MB   <- the real
    #     ceiling for ONE world load; the 64z number every other probe reports is ~110 MB low
    #
    # M1 shipped this at 768 MB rather than the planned 512 MB, because the same probe's torture
    # run found peak linear memory was NOT bounded by the biggest world a session opens: every
    # world load leaked ~22 MB that was never reused, at both heights, with no plateau (reloading
    # the SAME 256z world 8 times walked sbrkTop 240 -> 426 MB in a straight line). Peak was a
    # function of how many worlds the player opened, and a cap sized on one load would have turned
    # a long session into a hard abort (-sMALLOC=emmalloc + the default ABORTING_MALLOC=true —
    # a clean abort with a message, but still a lost session).
    #
    # THAT LEAK IS FIXED (Phase M / M6, 2026-09-02) — four shim classes held C++ ivars that this
    # port's hand-written ObjC runtime never destroyed; see src/shim/foundation/NSData.mm's
    # -dealloc. The same torture run is now dead flat at 233.2 MB across 8 loads, so peak is
    # bounded by world size again and this comes down to 512 MB as M1 said it would: 2.2x the
    # measured 256z ceiling, i.e. the sizing method's 1.5x rule with room to spare.
    # `node tools/headless-heap-ceiling-probe.js --cycles=4 --torture=same256` is the regression
    # test; tools/headless-alloc-leak-probe.js is the finer-grained one.
    #
    # Failing safe: eden_heap_pressure_tick() (src/seam/HeapProbe_web.mm) warns once at 80% and
    # once at 90% of this number, so the first field report of a cap abort arrives as "Eden: wasm
    # heap at 82% of its 512 MB cap" and not as an unexplained crash. Rollback is deleting this
    # one line.
    -sMAXIMUM_MEMORY=536870912
    # P4: the wasm STACK is separate from INITIAL_MEMORY and defaults to only 64 KB. Terrain::render
    # declares `vertexObject objVertices[300*6*6]` (10800 * 64 B ≈ 690 KB) as a plain LOCAL, plus the
    # mesher's own large frames — the iOS build had a multi-MB thread stack, so these never mattered.
    # On the default 64 KB wasm stack the first in-world Terrain::render() frame overflows and traps
    # as "memory access out of bounds" (measured: OOB in Terrain::render right after game_mode->1).
    #
    # 2 MB. MEASURED, project-audit row 14 / E1, 2026-07-31 — `node tools/headless-memory-probe.js`
    # paints the stack region and scans the high-water mark per phase (this was 8 MB before, chosen
    # to match the iOS main-thread budget because nothing had measured it):
    #     menu idle 4 KB · world generate+load 67 KB · IN-WORLD FRAME LOOP 694 KB · reload 694 KB
    # 694 KB is essentially objVertices alone, and it is a FIXED-SIZE local in a non-recursive call
    # graph (max_render_objects is a compile-time 300, Terrain::render does not recurse), so the
    # peak is deterministic rather than a sample — the deepest frame this engine has is the one
    # measured. 2 MB is ~2.9x that, measured in the -O0 build whose frames are FATTER than
    # Release's. Do not trim it closer: an undersized wasm stack in a build with -sASSERTIONS=0
    # fails as silent memory corruption, not as an error.
    -sSTACK_SIZE=2097152
    # Creature skinning (pass 27). GL_OES_matrix_palette does not exist in WebGL, so the shim
    # emulates it (src/shim/gl/gl_es1_shim.cpp GROUP 8) and advertises it in glGetString — but the
    # four extension ENTRY POINTS still resolve to NULL inside Classes/PVRTglesExt.cpp, whose
    # PVRGetProcAddress macro is a literal NULL on any non-__APPLE__ target. This wrap is where
    # they get filled in; see src/seam/pvrt_matrix_palette.cpp for why nothing else can.
    # (It REPLACES the old `--wrap=_Z12RenderModelsv` no-op guard, which existed only because
    # RenderModels() null-dereferenced PODs that LoadModels() had refused to load. With the
    # extension emulated they load, so RenderModels() runs for real again.)
    "SHELL:-Wl,--wrap=_ZN12CPVRTglesExt14LoadExtensionsEv"
    # PC controls audit F1/F2 (see /Users/sam/.claude/plans/please-run-an-audit-lovely-dongarra.md
    # and src/seam/Movement_web.mm's header). Player::setSpeed(Vector,float) is called from two
    # separate TUs (Input_web.mm's keyboard path, Classes/Joystick.mm's real touch joystick), so
    # --wrap sees it from both. Mangled name confirmed via
    # `llvm-nm build-st/CMakeFiles/eden.dir/.../Classes/Player.mm.o | grep setSpeed`, not guessed.
    "SHELL:-Wl,--wrap=_ZN6Player8setSpeedE6Vectorf"
    # PC controls audit Phase 6 (opt-in "advanced movement" — zero-delay bhop). Player::preupdate
    # is called from Classes/World.mm:499 (cross-TU), so --wrap sees every engine tick exactly
    # once. See src/seam/Movement_web.mm's header. Mangled name confirmed via llvm-nm, not guessed.
    "SHELL:-Wl,--wrap=_ZN6Player9preupdateEf"
    # row 36/C1 — measure mesh-CPU vs VBO-upload cost before designing an off-thread mesher, per
    # c1-threaded-build-handoff.md §5 item 1. Both cross-TU (Classes/Terrain.mm calls them on a
    # TerrainChunk* defined in Classes/TerrainChunk.mm). See src/seam/MeshTiming_web.mm.
    "SHELL:-Wl,--wrap=_ZN12TerrainChunk8rebuild2Ev"
    "SHELL:-Wl,--wrap=_ZN12TerrainChunk10prepareVBOEv"
    "SHELL:-Wl,--wrap=_ZN11FileManager10readColumnEiiP12NSFileHandle"
    # Settings (pass 28). The old GL settings panel is neutralised rather than seam-replaced:
    # SettingsMenu::{load,save,getNewWorldName} and properties[] are kept and driven by the DOM
    # panel, while update()/render() (both called from Classes/Menu.mm, a different TU) become
    # no-ops. gluPerspective is wrapped to make the hard-coded 80-degree FOV in
    # Classes/Graphics.mm:370 a setting. See src/seam/Settings_web.mm.
    "SHELL:-Wl,--wrap=_ZN12SettingsMenu6updateEf"
    "SHELL:-Wl,--wrap=_ZN12SettingsMenu6renderEv"
    "SHELL:-Wl,--wrap=gluPerspective"
    # World-save persistence (pass 29). /documents is plain MEMFS otherwise — session-scoped, gone
    # on reload, the #1 gap per docs/PORT-STATUS.md. Links Emscripten's IDBFS backend; the actual
    # mount (with {autoPersist:true}, so any write/unlink/rename under /documents queues an async
    # IndexedDB sync with no engine-side hook needed — see library_idbfs.js) plus the initial
    # populate-before-main() happens JS-side in public/eden-storage.js via Module.preRun, because
    # that populate is inherently async and main() must not run (and Menu::loadWorlds must not list
    # Documents) until it has finished. Safe to link into both build flavours: node's headless
    # `eden.js` has no indexedDB, so eden-storage.js's mount is guarded and no-ops there (falls
    # back to the previous in-memory-only behaviour, same as NSUserDefaults' localStorage guard).
    -lidbfs.js
    # Lazy Eden.eden fetch (pass 30) — populates /bundle/Eden.eden via fetch()/node fs before
    # main() runs, instead of --preload-file'ing the ~52MB default world into eden.data. See the
    # file's own header for why this can't be a true deferred-until-first-use fetch. Compiled in
    # (not a public/*.js <script>) so it also runs under headless `node eden.js`.
    "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/src/seam/js/eden_default_world.pre.js"
    -sWASM=1
    -sUSE_ZLIB=1
    # P2: the shim creates a WebGL2 context (src/shim/gl/gl_es1_shim.cpp GROUP 2c). Without
    # this, emscripten_webgl_create_context caps at majorVersion 1 and the create() call fails
    # at run time rather than at link time — a browser-only failure the node build cannot catch.
    -sMAX_WEBGL_VERSION=2
    -sMIN_WEBGL_VERSION=2
    # P2 (Texture2D_web.mm): preload the UI/HUD/block-atlas art into the virtual FS at
    # /bundle/media, preserving subdirectories (--preload-file does NOT flatten) — NSBundle.mm
    # flattens on lookup instead by indexing every basename once (see its header comment).
    # Deliberately excludes media/{models,music,new_sound,sound,screens} (~47 MB): audio is P5's
    # (HTTP-fetched, see the audio-manifest step above), screens/ is unused marketing art, and
    # models/ is preloaded FLAT to /bundle further down (the bundle layout the engine expects).
    # Revisit the excluded set as stages land — this is asset-packaging config, not a
    # source-of-truth list; nothing else in the port depends on exactly these 6 subdirs.
    #
    # Each is wrapped in "SHELL:" — WITHOUT it, CMake's target_link_options deduplicates
    # byte-identical list ELEMENTS across the whole option list, and every one of these entries
    # starts with the same literal token "--preload-file". That collapsed all but the first
    # occurrence (discovered via a real build: emcc reported the 2nd+ src@dst path as a stray
    # "input file", because its own --preload-file flag had been silently removed). SHELL:
    # marks the flag+value pair as one atomic, non-splittable, non-dedupable unit.
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/textures@/bundle/media/textures"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/menu@/bundle/media/menu"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/menu_text@/bundle/media/menu_text"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/ui@/bundle/media/ui"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/ipad_menu@/bundle/media/ipad_menu"
    # media/icons is NOT here (audit row E4, pass 62 — it used to be, packaging 159 KB of iOS app
    # icons into eden.data). Icon_57/72/114/144.png ARE still shipped and still used — but over
    # HTTP, by public/manifest.webmanifest and service-worker.js via the public/audio -> ../media
    # symlink, never through the virtual FS. Nothing in Classes/ or src/ ever asks the bundle for
    # them, so their copy inside eden.data was download weight that could not be read.
    # A handful of used textures sit directly in media/ (not a subdir) — sky, particles, portal
    # swirl. Individually named rather than preloading all of media/ root (numbers_text.psd,
    # .DS_Store are not assets the engine reads).
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/sky_box.png@/bundle/media/sky_box.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/sky_box_bw.png@/bundle/media/sky_box_bw.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/smoke_tex.png@/bundle/media/smoke_tex.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/smoke.png@/bundle/media/smoke.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/Flame_256.png@/bundle/media/Flame_256.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/Flame_512.png@/bundle/media/Flame_512.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/portal_twirl.png@/bundle/media/portal_twirl.png"
    # report_flag.png lives at the REPO ROOT, not under media/ — so no directory rule above reaches
    # it, and until pass 62 it was the one texture the engine asks for that this build did not ship
    # (`Texture2D_web: no data at '/bundle/media/report_flag.png'`, twice per session, in every
    # headless run since the port began). Found while auditing the opposite question, row E4's
    # "is everything we ship reachable". Resources::loadMenuTextures pushes it as the last
    # menutextures entry, so it is reachable whenever the legacy GL menu is (Settings' `legacy_menu`
    # row keeps that a real, supported choice — see src/seam/Menu_web.mm's header). 21 KB.
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/report_flag.png@/bundle/media/report_flag.png"
    # Creature skins (pass 27): Moof_Default.png / *_Blink / *_Rage plus the MASKS/ variants that
    # Resources::getSkin recolours from. Same basename-index lookup as every other texture, so
    # this one goes under /bundle/media like the rest. WITHOUT it the models draw as pure black
    # silhouettes and nothing errors: Model.mm binds `getTex(SKIN_MOOF)->name`, which is 0 when
    # the PNG never loaded, and sampling an unbound texture unit yields (0,0,0,1). Measured, not
    # guessed — the shim's [eden-skin] probe reported boundTex=0 with every other uniform healthy.
    #
    # Audit row B2: this used to be one `--preload-file media/models@/bundle/media/models` rule,
    # which packaged the .pod files below a *second* time (file_packager has no per-rule exclude —
    # `--exclude-file` patterns are matched against the same srcpath used by the flat .pod rules
    # below, so a glob can't tell "this .pod via the directory walk" from "this .pod via its own
    # explicit rule" apart; the only fix is to not hand the .pod files to the directory rule at
    # all). Named individually here instead of via the directory so only the skin PNGs + MASKS/
    # get packaged under /bundle/media/models — the .pod files are packaged exactly once, by their
    # own flat rules just below.
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Moof_Default.png@/bundle/media/models/Moof_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Moof_Blink.png@/bundle/media/models/Moof_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Moof_Rage.png@/bundle/media/models/Moof_Rage.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Batty_Default.png@/bundle/media/models/Batty_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Batty_Blink.png@/bundle/media/models/Batty_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Batty_Rage.png@/bundle/media/models/Batty_Rage.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Green_Default.png@/bundle/media/models/Green_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Green_Blink.png@/bundle/media/models/Green_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Green_Rage.png@/bundle/media/models/Green_Rage.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Nergle_Default.png@/bundle/media/models/Nergle_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Nergle_Blink.png@/bundle/media/models/Nergle_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Nergle_Rage.png@/bundle/media/models/Nergle_Rage.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stumpy_Default.png@/bundle/media/models/Stumpy_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stumpy_Blink.png@/bundle/media/models/Stumpy_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stumpy_Rage.png@/bundle/media/models/Stumpy_Rage.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Charger_Default.png@/bundle/media/models/Charger_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Charger_Blink.png@/bundle/media/models/Charger_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Charger_Rage.png@/bundle/media/models/Charger_Rage.png"
    # Stalker has no *_Rage.png on disk (only Default/Blink) — not a gap this change introduces.
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stalker_Default.png@/bundle/media/models/Stalker_Default.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stalker_Blink.png@/bundle/media/models/Stalker_Blink.png"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/MASKS@/bundle/media/models/MASKS"
    # Creature models (pass 27). Classes/World.mm:389 calls LoadModels([[NSBundle mainBundle]
    # resourcePath] + "/"), and CPVRTResourceFile fopen()s <readPath><basename> — so these must
    # land at /bundle/<Name>.pod, FLAT, not under /bundle/media/models. That is also what Xcode's
    # Copy-Bundle-Resources phase does on device (it flattens the whole media/ tree), so naming
    # each file individually here reproduces the real bundle layout rather than inventing one.
    # ~1.6 MB total, hence preloaded rather than lazily fetched. Packaged exactly once now (B2) —
    # the skin-PNG rules above no longer pull these in a second time via their parent directory.
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Moof.pod@/bundle/Moof.pod"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Batty.pod@/bundle/Batty.pod"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Green.pod@/bundle/Green.pod"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Nergle.pod@/bundle/Nergle.pod"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stumpy.pod@/bundle/Stumpy.pod"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Charger.pod@/bundle/Charger.pod"
    "SHELL:--preload-file ${EDEN_PARENT_DIR}/media/models/Stalker.pod@/bundle/Stalker.pod"
    # P4: the pre-generated default world (repo-root Eden.eden, ~52MB RLE reference map). The
    # engine streams default-world terrain from it on first world-create (gen_default path in
    # FileManager::loadWorld -> FileManagerHelper::fmh_readColumnFromDefault), resolved via
    # NSBundle -pathForResource:@"Eden.eden". NSBundle.mm indexes /bundle/media by basename AND
    # stats /bundle/<name> as a fallback for this file.
    #
    # Pass 30: lazily fetched instead of --preload-file'd — see the "lazy Eden.eden fetch" comment
    # by the public/audio symlink above and src/seam/js/eden_default_world.pre.js. /bundle itself
    # must still exist as a preloaded directory (from the media preloads above) for the .pre.js
    # run dependency to write the fetched file into; nothing needs to preload Eden.eden's bytes.

    # ---- Audit row E4 (pass 62): keep authoring leftovers out of eden.data ----------------------
    # The directory rules above walk media/ subdirectories wholesale, which is the right shape —
    # but those directories are the 2010-2014 project's WORKING art folders, not a shipped asset
    # tree, so they also contain the sources and scratch files the art was made from. Measured
    # before this rule landed: 7 .psd files (334 KB — Texture2D only ever loads .png), two
    # .DS_Store, and media/textures' offline atlas generator (AtlasTool.java/.class + its
    # config*.txt inputs, which nothing at runtime reads). With media/icons dropped above, that is
    # 514 KB of 6.81 MB — 7.4% of every player's first download, none of it loadable.
    #
    # Patterns are fnmatch'd against the SOURCE path (file_packager.py `is_excluded`), so a leading
    # `*` is required — and note the same matching is why the .pod files below get their own
    # explicit rules rather than an exclude (see the row-B2 comment above).
    #
    # This is a deny-list on purpose: an allow-list of "only *.png" would silently drop a future
    # asset type (the port has already gained .pod and MASKS/ variants once), whereas a new .psd
    # left in an art folder is the failure this rule exists to absorb.
    "SHELL:--exclude-file *.psd"
    "SHELL:--exclude-file *.DS_Store"
    "SHELL:--exclude-file *AtlasTool*"
    "SHELL:--exclude-file *media/textures/config*.txt*"
  )
  if(EDEN_THREADED)
    # ---- Audit row 36/C1, pass 63 (2026-08-04): ARCHITECTURE CHANGED. Read this before adding
    # PROXY_TO_PTHREAD back. -------------------------------------------------------------------
    #
    # This branch used to be `-pthread -sPROXY_TO_PTHREAD=1 -sOFFSCREENCANVAS_SUPPORT=1
    # -sUSE_OFFSET_CONVERTER=1`, i.e. web-port-plan.md's original D1: move main() (and therefore
    # the whole engine and all GL) onto a worker, and hand it the canvas as a transferred
    # OffscreenCanvas. That was written in 2026-07, when the browser host was a canvas and a
    # keyboard listener. **It is now the wrong architecture, and it was measured wrong, not
    # reasoned wrong:** built as-is it links fine and then dies on the first `pthread_create` with
    #     pthread_create: could not find canvas with ID "#canvas" to transfer to thread!
    # under `node tools/headless-p1-gate.js build-thr` — the OffscreenCanvas handshake
    # (public/worker-bootstrap.js) was never written and the sketch for it was deleted in audit
    # row I5.
    #
    # WHY NOT JUST WRITE THE HANDSHAKE: the port grew a real DOM UI layer in passes 58-62. Ten
    # files under public/ (eden-menu, eden-settings, eden-hotbar, eden-input, eden-viewport,
    # eden-pausemenu, eden-console, eden-storage, eden-loaderror, eden-ui) call **72 distinct
    # `_eden_*` engine exports** directly and SYNCHRONOUSLY from the browser main thread — count
    # it with `grep -oh "_eden_[a-z_0-9]*" public/*.js | sort -u | wc -l`. Under PROXY_TO_PTHREAD
    # those calls do not become cross-thread messages by themselves: they still execute on the
    # main thread, against the same shared memory the engine worker is mutating, which is a data
    # race on engine state with no diagnostic. Making it correct means hand-proxying all 72 (plus
    # every future one) through emscripten_proxy_async/MAIN_THREAD_EM_ASM. THAT is where the
    # audit's "3-6 weeks" estimate lives — and it buys nothing toward this row's actual prize.
    #
    # WHAT THE PRIZE ACTUALLY NEEDS (audit row 36's own re-framing, pass 59): off-thread chunk
    # meshing. That is CPU work over `blockarray` producing vertex buckets — it needs *worker
    # threads sharing the engine's memory*, which is `-pthread` + SharedArrayBuffer + COOP/COEP.
    # It does NOT need the engine relocated off the main thread, and it does not need
    # OffscreenCanvas. So:
    #
    #   ENGINE + GL + DOM stay on the browser main thread. CLAUDE.md convention #4 ("all GL work
    #   on the main thread") survives intact and keeps meaning what it says. `-pthread` makes
    #   `pthread_create` real, which (a) makes the engine's existing world-load thread a genuine
    #   background thread instead of src/seam/pthread_sync_web.c's inline call, and (b) is the
    #   prerequisite a future meshing thread pool needs.
    #
    # This is strictly a subset of the old flag set, so it does not foreclose the OffscreenCanvas
    # architecture — it defers it. If it is ever revisited, the 72-export proxy layer is the work,
    # not the flags.
    #
    # PTHREAD_POOL_SIZE: pre-spawn the worker(s) at startup. Without a pooled worker,
    # `pthread_create` called from the browser MAIN thread cannot block waiting for a Worker to
    # boot (the Worker's own `onmessage` needs the main thread to return to the event loop first),
    # so Emscripten defers the thread's start until the next event-loop turn. The engine polls
    # `doneLoading` per frame rather than joining, so a deferred start is survivable — but a
    # pooled worker makes it deterministic, and the pool is what a meshing pool will grow out of.
    # 4 = 1 world-load thread + MP_WORKERS (2, Classes/MeshPool.mm) + 1 slot of slack. Pooled
    # workers that are never used still cost one idle Worker each: Phase M / M0 (2026-08-30)
    # measured the 8-slot pool at ~112 MB resident (~14 MB per idle Worker realm), so dropping 4
    # of them is a measured ~55 MB — the reason for the 8 → 4 cut here (Phase M / M2, 2026-08-31).
    # It was 8 from B3 Stage 2 through M1: the mesh pool creates its workers lazily on the first
    # dispatch, from the main thread, and a pool that is already exhausted turns that into a
    # deferred start (survivable — mp_start() falls back to inline meshing, exercised naturally in
    # the B3 Safari run — but B3's over-budget win erodes silently if it happens under load). 4
    # keeps one slot over the 3-concurrently-live count; don't go to 3 without re-checking the
    # inline-fallback counters in safari-threaded-mesher-check.js.
    #
    # -sALLOW_MEMORY_GROWTH + -pthread emits `-Wpthreads-mem-growth` (growing a SharedArrayBuffer
    # invalidates every JS-side heap view in every worker). Accepted, not ignored: this port's
    # heap demand is measured at 112-140 MB against a 100 MB initial (see -sINITIAL_MEMORY above),
    # so growth happens a handful of times per session, not per frame.
    list(APPEND EDEN_LINK_FLAGS
      -pthread
      -sPTHREAD_POOL_SIZE=4
    )
    target_compile_options(eden PRIVATE -pthread)
  else()
    # Single-thread fallback (plan D1 fallback note): main thread only, OPFS via WASMFS's async
    # access path, load runs synchronously with a progress yield (TODO P4 to actually implement
    # the yield — this flag alone doesn't provide it).
    message(STATUS "Eden web: EDEN_THREADED=OFF — single-thread fallback build (see web-port-plan.md D1 fallback note)")
  endif()

  # Q1 (dazzling-munching-bengio.md perf audit, C1): Release-only link flags. Kept separate from
  # EDEN_LINK_FLAGS (rather than $<CONFIG:Release> generator expressions mixed into that list)
  # because several of these are SHELL:-wrapped multi-token flags where generator-expression
  # quoting has historically been fragile in this file (see the --preload-file SHELL: comment
  # above for the same class of footgun) — a flat CONFIG-gated list is easier to audit.
  # `-sENVIRONMENT=web` was TRIED and REVERTED this pass: it makes the generated eden.js force
  # `ENVIRONMENT_IS_WEB=true` unconditionally, which breaks `node eden.js` — this port's own
  # documented fast headless regression gate (web/CLAUDE.md "Build & run") — because the node-only
  # branches (fetchRemotePackage falling back to `fs.readFileSync`, etc.) are compiled out and the
  # web-only `fetch`/`window` path runs under node instead and throws. Measured, not guessed: the
  # node run threw exactly there. Keeping the default (auto-detect) environment costs a few hundred
  # bytes of detection code and preserves the only automated correctness check this build has.
  # NOT included here: --closure 1 and -sINCOMING_MODULE_JS_API trimming (both from the audit's
  # Q1 step 2) — closure minification needs the public/*.js <-> Module surface audited for
  # bracket-vs-dot property access first (untested risk, no browser-based CI here to catch a
  # silently-renamed property). STACK_SIZE was the other item parked here, "left at the Debug-era
  # 8 MB rather than guessed smaller"; it is no longer guessed — measured at 694 KB peak and set to
  # 2 MB, see the -sSTACK_SIZE comment in EDEN_LINK_FLAGS above.
  target_link_options(eden PRIVATE
    ${EDEN_LINK_FLAGS}
    $<$<CONFIG:Release>:-sASSERTIONS=0>
    # Project audit 2026-07-30, row B1 — the rest of the Release flag sweep the prior audit's Q1
    # step 2 left undone. Measured as one pass: fresh Release+diagnostics builds at every point in
    # the bisect (single flag alone, and all four together) all pass the P1 headless gate and all
    # three other headless suites (menu-flow, lazy-world, gamepad) clean. -flto is the fifth flag,
    # landed pass 56 once the abort pass 53 blamed on it turned out to be our own uninitialised
    # C++ ivar — see the compile-options comment above. It MUST appear on both sides: -flto here
    # without -flto on the compile side means there is no bitcode to link-time-optimise, and the
    # reverse silently produces objects the linker treats as ordinary ones.
    #   -sMALLOC=emmalloc (smaller/faster than dlmalloc for this port's allocation profile —
    #     mostly fixed-size engine structs and short-lived shim objects, not the varied-size
    #     workload dlmalloc is tuned for)
    #   -sGL_TRACK_ERRORS=0 / -sGL_ASSERTIONS=0 (removes Emscripten's own per-GL-call bookkeeping
    #     in the GL glue it generates around this port's hand-written shim — directly on the
    #     shim's hot draw-call path; safe in Release since this port's OWN glGetError probe in
    #     gl_es1_shim.cpp, used by the first-5-frames diagnostic and EDEN_DIAGNOSTICS builds, is
    #     independent of Emscripten's glue-level tracking)
    #   -sTEXTDECODER=2 (use TextDecoder for UTF-8 where available, smaller JS glue)
    #   --emit-symbol-map (so a Release build — which has no -g/DWARF, see above — still has a
    #     name<->mangled-symbol map on disk for reading a stack trace after the fact)
    $<$<CONFIG:Release>:-flto>
    $<$<CONFIG:Release>:-sMALLOC=emmalloc>
    $<$<CONFIG:Release>:-sGL_TRACK_ERRORS=0>
    $<$<CONFIG:Release>:-sGL_ASSERTIONS=0>
    $<$<CONFIG:Release>:-sTEXTDECODER=2>
    $<$<CONFIG:Release>:--emit-symbol-map>
    # ROADMAP Phase M / M6: AllocTrace_web.c captures a JS callstack per big allocation, and
    # without the wasm name section every frame reads `wasm-function[1234]`. --profiling-funcs
    # keeps the names without pulling in DWARF.
    $<$<BOOL:${EDEN_ALLOC_TRACE}>:--profiling-funcs>
  )
  # --pre-js files are consumed at LINK time and CMake cannot see that from the flag string, so
  # without this an edit to eden_default_world.pre.js rebuilds NOTHING ("Built target eden") and
  # the stale JS keeps running — silently, which cost pass 46 a confusing debugging round.
  set_property(TARGET eden APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/seam/js/eden_default_world.pre.js")
  target_compile_definitions(eden PRIVATE $<$<BOOL:${EDEN_THREADED}>:EDEN_THREADED=1>)
else()
  message(WARNING "Eden web: not configured via emcmake/Emscripten toolchain — this target is only meaningful under emsdk (see web/cmake/README.md). Configuring anyway so source-list/flag correctness can be inspected without a working compiler.")
endif()
