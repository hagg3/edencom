# native/CMakeLists.txt — Eden native (SDL3 + desktop GL / GL ES 3) build.
#
# Phase N Stages 1-3 (WORKING/native-migration-plan-2026-09-04.md,
# WORKING/phase-n-stage3plus-plan-2026-09-05.md). Four platforms: macOS, Linux, Windows, iOS.
# iOS is the `if(APPLE)` arm plus an `if(IOS)` overlay for what genuinely differs: the GL flavour
# (OpenGL ES 3.0, not desktop GL), the app BUNDLE and SDL3 as a static library.
#
# THE ENGINE AND EVERY SHARED SEAM ARE PLAIN C++ ON EVERY TARGET (Stage G: G.2b took the engine and
# the seams off Objective-C, G.3 deleted the port's own GNU-ABI ObjC runtime, its Foundation/UIKit
# shims and the framework/ header trampolines; plan WORKING/de-objc-plan-2026-10-24.md). The only
# Objective-C left is on Apple, in three platform walls compiled as their own object libraries with
# real SDK headers (AppleAudio, AppleNet, IOSKeyboard; see "The Apple audio wall" below). Until G.3
# the `if(APPLE)`/`else()` split here was the ObjC runtime (clang cannot lower any GNU ObjC ABI to
# Mach-O, so Apple used libobjc + real Foundation and Linux/Windows ran the shim); that history is
# in git and in port/docs/history/objc-runtime.md.
#
# The remaining differences between the targets are small and local: a GL entry-point loader on the
# two non-Apple targets (port/src/render/gl/gl_loader_native.h explains why Windows cannot do without
# one), the audio and HTTP backends (Apple frameworks vs PortableAudio + the portable GET), the heap
# probe's three implementations, and the filesystem root.
#
# HOW THIS RELATES TO THE RETIRED web-port-final:web/CMakeLists.txt — read this before adding a source here.
# (The Emscripten build was retired on 2026-10-13; web-port-final:web/CMakeLists.txt, cited throughout this file as
# the origin of a list or a flag, is readable at tag web-port-final. The shared seam/shim code it
# compiled now lives in port/src/ — see port/CLAUDE.md.)
#   * The ENGINE source list is port/tools/engine-sources.txt (one line per file).
#   * MOST OF THE PORT'S SEAM AND SHIM CODE IS SHARED, not copied: port/src/shim/foundation/
#     (eden_base/eden_fs/eden_prefs/eden_image, platform_shims, save_backup, md5),
#     port/src/render/ (the renderer, R::) and the port/src/seam/*.cpp files.
#   * Files under native/ exist ONLY where the platform genuinely differs.
#
# 3.21 for $<TARGET_RUNTIME_DLLS:...> (the Windows DLL copy below). Every CI runner and every
# current distro is well past it; this is not a compatibility surface anyone is defending.
cmake_minimum_required(VERSION 3.21)
project(EdenNative LANGUAGES C CXX)

# The game's version: the repo-root VERSION.txt file (written by ./bump-version.sh). It reaches the
# menu/HUD corner tag (EMOD_VERSION_STRING, GLW::drawVersionTag) and the iOS Info.plist.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt" EMOD_VERSION LIMIT_COUNT 1)
string(STRIP "${EMOD_VERSION}" EMOD_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt")
message(STATUS "Emod version ${EMOD_VERSION}")

# ---------------------------------------------------------------------------------------------
# iOS (Phase N Stage 4.1) — the stage Phase N exists for
# ---------------------------------------------------------------------------------------------
# Configured with -DCMAKE_SYSTEM_NAME=iOS, which is what sets CMake's own `IOS` variable (and
# leaves APPLE true — see this file's header for why that is the right shape). Two knobs get a
# default here rather than being left to the caller, because both are load-bearing claims:
#
#   IPHONEOS_DEPLOYMENT_TARGET = 15.0   The floor (SM.1). Eden.xcodeproj says 15.6 today; 15.0 is
#                                       the oldest iOS that runs the ES 3.0 context and the libc++
#                                       the engine links against. iPad Air 2 (last iOS 15.8) stays in range.
#   arm64                               iOS 11 dropped 32-bit, so armv7/armv7s (which the .xcodeproj
#                                       still lists in VALID_ARCHS) cannot even be deployment
#                                       targets here. One slice, not three.
if(IOS)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "" FORCE)
  endif()
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
  endif()
endif()

set(EDEN_PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(EDEN_CLASSES_DIR "${EDEN_PARENT_DIR}/Classes")
set(EDEN_PORT_DIR "${EDEN_PARENT_DIR}/port")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)

# Diagnostics are always compiled in (G.0 decision, 2026-10-23; the OFF configuration had been broken
# since d92fd8c and is deleted): the scripted-drive probes, DevConsole, `--live-cmds` and every
# `--*-selftest` mode are in every build. `-DEDEN_DIAGNOSTICS=ON` on the command line is accepted and ignored.
set(EDEN_DIAGNOSTICS ON)

# X.1: the Stage F.3 timing hooks ([F3] lines, the F3_DRIVE / f3-drive.txt driver). Off by default so
# a shipped build pays nothing; native/README.md "Timing hooks" says how to use them. The hooks are
# Classes/F3Timing.h; with the option off they compile to no-ops.
option(EDEN_F3_HOOKS "Compile the F.3 timing hooks and the scripted warp/walk driver." OFF)

# G.2s: ASan + UBSan (the `sanitize` job in native.yml). Applied to eden_native's own sources only, not SDL.
# -fno-sanitize-recover makes the first UB report fatal, so a gate fails on it instead of logging past it.
option(EDEN_SANITIZE "Build with -fsanitize=address,undefined (CI's sanitizer leg)." OFF)

# ---------------------------------------------------------------------------------------------
# SDL3
# ---------------------------------------------------------------------------------------------
# The plan's locked platform-layer choice (zlib licence, deploys to iOS 11+ and Android API 21+,
# so it does not constrain Stage 4). Found via its own package config:
#   macOS    brew install sdl3
#   Linux    libsdl3-dev, or the vendored build .github/workflows/native.yml falls back to
#   Windows  MSYS2 clang64's mingw-w64-clang-x86_64-sdl3
find_package(SDL3 REQUIRED CONFIG)

# WHICH SDL3 TARGET, and why this is not always `SDL3::SDL3`. SDL's package config exports
# `SDL3::SDL3` for a shared build and `SDL3::SDL3-static` for a static one, and the iOS leg is
# static by necessity rather than taste: a dylib inside an .app has to be embedded, re-signed and
# declared, and every one of those is a step that fails on a device rather than in CI. A static
# SDL is one binary with nothing to embed. The desktop legs keep the shared library they have
# always used (Homebrew's, the distro's, MSYS2's), so this picks rather than prescribes.
if(TARGET SDL3::SDL3)
  set(EDEN_SDL3_TARGET SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
  set(EDEN_SDL3_TARGET SDL3::SDL3-static)
else()
  message(FATAL_ERROR "SDL3 was found but exports neither SDL3::SDL3 nor SDL3::SDL3-static")
endif()

# ---------------------------------------------------------------------------------------------
# Engine sources
# ---------------------------------------------------------------------------------------------
# port/tools/engine-sources.txt is THE engine source list: one path per line, relative to the repo root,
# stock 2.1.1 files and the files added since. Adding an engine file is one line there. (G.1 moved the
# files the port replaces with a seam, and the never-compiled iOS shell, to archive/stock-unbuilt/, so
# there is no exclude list.) The seam replacements live in port/src/seam.
file(STRINGS "${EDEN_PARENT_DIR}/port/tools/engine-sources.txt" _eden_engine_rel)
set(EDEN_ENGINE_SOURCES "")
foreach(rel IN LISTS _eden_engine_rel)
  list(APPEND EDEN_ENGINE_SOURCES "${EDEN_PARENT_DIR}/${rel}")
endforeach()
# zstd 1.5.7 single-file amalgamation (docs/third-party.md): ONE pinned copy on every target, because
# byte-identical `.emod` output across targets depends on it. ASM off and the strategies above level 3's
# (dfast) compiled out; level 19 is tools-only (emod.py), the engine never encodes above 4.
set_source_files_properties("${EDEN_PARENT_DIR}/port/src/shim/vendor/zstd/zstd.c" PROPERTIES
  COMPILE_DEFINITIONS "ZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR;ZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR;ZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR;ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR;ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR;ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR")
list(LENGTH EDEN_ENGINE_SOURCES _eden_engine_count)
message(STATUS "Eden: ${_eden_engine_count} engine sources")

# ---------------------------------------------------------------------------------------------
# Shared port sources (compiled from port/src/, NOT copied — see the header comment)
# ---------------------------------------------------------------------------------------------
# port/src/shim/foundation/ since G.3 holds no Foundation: only portable C++ the seams and the engine
# share on every target.
#   md5.cpp     — eden_file_md5_hex (replaced the stock Classes/md5.c); plain stdio + RFC-1321.
#   save_backup.cpp — Phase N Stage 2: the previous-save ".bak" rules. Portable C++ — stdio plus
#                     g_save_inplace_threshold.
# (eden_fs.cpp and platform_shims.cpp are in EDEN_SHARED_OTHER below.)
set(EDEN_SHARED_FOUNDATION
  md5.cpp
  save_backup.cpp
)
list(TRANSFORM EDEN_SHARED_FOUNDATION PREPEND "${EDEN_PORT_DIR}/src/shim/foundation/")

# Seam files shared verbatim with the web build. Each is here because it was AUDITED as portable
# in Stage 0.4 or in this stage, not because it happened to compile:
#   Texture2D       PNG decode + the texture upload contract (its one DOM-shaped function is
#                       split out to TextRaster_native.cpp).
#   DisplayProfile  the derived point space + device profiles. Pure arithmetic.
#   Settings        the kSettings[] table — the single source of truth for all 22 settings,
#                       and what Stage 2.5's GL settings screen is built on.
#   Movement        the move/look input exports (frame-rate-independent walk speed).
#   MeshTiming      the timing accumulators behind the eden_debug_mesh_timing() export.
#   LoadFailure     the corrupt/truncated-save signal and .bak restore. Real file I/O only.
#   AppLifecycle    Stage 4.2's eden_app_will_background(): the "is it safe to save right now?"
#                       policy for save-on-background. Portable by construction -- it names no DOM,
#                       no Emscripten and no SDL -- and shared precisely so web's pagehide, this
#                       target's SDL_EVENT_QUIT and iOS's SDL_EVENT_WILL_ENTER_BACKGROUND cannot
#                       drift into three different ideas of when a save is safe.
#   Menu / Storage  C accessors over Menu.cpp/the world list. NOT replacements — they are
#                       how any host, DOM or native, drives the menu, and they are what the Stage 1
#                       harness in port/src/harness/ uses.
#   Input           the touch/pointer/keyboard translator. Compiled whole for Stage 1 because
#                       the harness taps HUD buttons through it; the plan's "split it into a
#                       shared core + a thin SDL translator" is Stage 2's job, once both callers
#                       exist and the split can be made against real requirements.
set(EDEN_SHARED_SEAM
  Texture2D.cpp
  DisplayProfile.cpp
  Settings.cpp
  Movement.cpp
  MeshTiming.cpp
  LoadFailure.cpp
  AppLifecycle.cpp
  Menu.cpp
  Storage.cpp
  Input.cpp
)
list(TRANSFORM EDEN_SHARED_SEAM PREPEND "${EDEN_PORT_DIR}/src/seam/")

set(EDEN_SHARED_OTHER
  "${EDEN_PORT_DIR}/src/shim/foundation/eden_fs.cpp"        # G.2b-0: EdenFile, eden_fs_*, eden_bundle_path
  "${EDEN_PORT_DIR}/src/shim/foundation/platform_shims.cpp" # arc4random + the portable clock + FS roots
  # G.5b-0: the engine-owned renderer's foundation (WORKING/renderer-interface-plan-2026-10-25.md).
  "${EDEN_PORT_DIR}/src/render/render_passes.cpp"           # pass tracker + --scene-shot's draw mask
  "${EDEN_PORT_DIR}/src/render/render_core.cpp"             # R:: matrix stacks (R2), frame brackets, dispatch
  "${EDEN_PORT_DIR}/src/render/render_formats.cpp"          # R::kFmt* (prefixed: engine headers; see below)
  "${EDEN_PORT_DIR}/src/render/gl/gl_state.cpp"             # the GL state cache (R3) + draw accounting
  "${EDEN_PORT_DIR}/src/render/gl/gl_stream.cpp"            # the Q.5 streaming rings
  "${EDEN_PORT_DIR}/src/render/gl/gl_textures.cpp"          # texture uploads (Q.2/Q.4/N.4.9 rules)
  "${EDEN_PORT_DIR}/src/render/gl/gl_programs.cpp"          # shader compile + the five programs (+ lit-flat)
  "${EDEN_PORT_DIR}/src/render/gl/gl_renderer.cpp"          # the GL backend's draws and resources
  "${EDEN_PORT_DIR}/src/render/gl/gl_selftest.cpp"          # --render-selftest-gl's GL unit checks + --lit-oracle
  "${EDEN_PORT_DIR}/src/render/null/null_renderer.cpp"      # the no-context backend
  # Stage MP (MP.1/MP.2): the multiplayer client's TCP seam, framer, SNAPZ codec and protocol session.
  # Plain C++ over zlib; no engine includes (the network thread owns it). Winsock links below.
  "${EDEN_PORT_DIR}/src/net/NetSock.cpp"
  "${EDEN_PORT_DIR}/src/net/Snapz.cpp"
  "${EDEN_PORT_DIR}/src/net/MpSession.cpp"
  "${EDEN_PORT_DIR}/src/net/ServerList.cpp"                 # MP.6: servers.txt + the probe
  "${EDEN_PORT_DIR}/src/net/NetSelftest.cpp"                # --net-selftest / --mp-selftest
)
# ---------------------------------------------------------------------------------------------
# Native-only sources — one file per genuine platform difference
# ---------------------------------------------------------------------------------------------
# G.4: the --*-selftest harness, one file per mode group (port/src/harness/harness.h)
set(EDEN_HARNESS_SOURCES main core mode_boot mode_shots mode_world mode_props mode_cmds mode_light mode_mp
  mode_look mode_display mode_input mode_ui mode_browser mode_session mode_render)
list(TRANSFORM EDEN_HARNESS_SOURCES PREPEND "${EDEN_PORT_DIR}/src/harness/")
list(TRANSFORM EDEN_HARNESS_SOURCES APPEND ".cpp")
set(EDEN_NATIVE_SOURCES
  ${EDEN_HARNESS_SOURCES}
  ${EDEN_PORT_DIR}/src/seam/SDLView_native.cpp
  ${EDEN_PORT_DIR}/src/seam/DisplayMode_native.cpp
  ${EDEN_PORT_DIR}/src/seam/Input_native.cpp
  ${EDEN_PORT_DIR}/src/seam/EdenViewController_native.cpp
  ${EDEN_PORT_DIR}/src/seam/EdenAppDelegate_native.cpp
  ${EDEN_PORT_DIR}/src/seam/main_native.cpp
  ${EDEN_PORT_DIR}/src/seam/HeapProbe_native.cpp
  ${EDEN_PORT_DIR}/src/seam/seam_link_stubs_native.cpp
  ${EDEN_PORT_DIR}/src/seam/SimpleAudioEngine_native.cpp
  ${EDEN_PORT_DIR}/src/seam/TextRaster_native.cpp
  # Stage S / S.2: --codec-bench (zstd vs zlib per-column timing; no engine dependency).
  ${EDEN_PORT_DIR}/src/seam/CodecBench_native.cpp
  ${EDEN_PORT_DIR}/src/seam/WorldTools_native.cpp
  # G.2b-0: the de-ObjC replacement layer's flat prefs store (all targets) and its --base-selftest.
  ${EDEN_PORT_DIR}/src/seam/Prefs_native.cpp
  ${EDEN_PORT_DIR}/src/seam/BaseSelftest_native.cpp
  # C.9: the player's README.txt written into the documents directory at startup.
  ${EDEN_PORT_DIR}/src/seam/PlayerReadme_native.cpp
  # ROADMAP 5.9: the eden_net_* seam the world browser polls (job table, worker threads, .part
  # files, offline fixtures). The blocking GET under it is per platform — see the block after
  # this list for which backend each target links.
  ${EDEN_PORT_DIR}/src/seam/Net_native.cpp
  # MP.3b: the multiplayer join flow, MpSession (port/src/net) <-> Classes/MpWorld.
  ${EDEN_PORT_DIR}/src/seam/MpLink_native.cpp
  # MP.6: the saved server list + probe (port/src/net/ServerList) behind Classes/MpServers.h.
  ${EDEN_PORT_DIR}/src/seam/MpServers_native.cpp
  ${EDEN_PORT_DIR}/src/render/gl/gl_context_native.cpp   # the context, window, drawable, scene pass
)
# The frame's autorelease-pool push/drain: libobjc's pool on Apple (the seam walls below still
# autorelease), a no-op elsewhere (G.3). Plain C++ on every target.
list(APPEND EDEN_NATIVE_SOURCES ${EDEN_PORT_DIR}/src/seam/AutoreleasePool_native.cpp)
if(NOT APPLE)
  list(APPEND EDEN_NATIVE_SOURCES
    # The GL entry-point table. Mandatory on Windows (opengl32.dll is GL 1.1 and nothing more) and
    # used on Linux too, deliberately, so the two non-Apple legs differ in nothing but the object
    # format. See src/render/gl/gl_loader_native.h.
    ${EDEN_PORT_DIR}/src/render/gl/gl_loader_native.cpp
    # Audio without Apple's frameworks: dr_mp3/dr_wav plus a hand-written IMA4-CAF decoder, behind
    # the SAME plain-C wall AppleAudio_native.h defines. The mixer, voice pool, channel model and
    # facade above it are shared and unchanged.
    ${EDEN_PORT_DIR}/src/seam/PortableAudio_native.cpp
  )
endif()

if(EDEN_DIAGNOSTICS)
  list(APPEND EDEN_SHARED_SEAM
    "${EDEN_PORT_DIR}/src/seam/DebugState.cpp"
    "${EDEN_PORT_DIR}/src/seam/DevConsole.cpp")
  # --live-cmds=<file>: DevConsole's commands for an interactive native session, fed from a file.
  list(APPEND EDEN_NATIVE_SOURCES ${EDEN_PORT_DIR}/src/seam/LiveCommands_native.cpp)
endif()

# ---------------------------------------------------------------------------------------------
# The Apple audio wall — its own object library, and that is load-bearing
# ---------------------------------------------------------------------------------------------
# src/seam/AppleAudio_native.mm is the ONE file that must see the REAL <AudioToolbox/...> and
# <AVFoundation/...>. It is kept out of the main target because those frameworks pull in CoreServices,
# whose CarbonCore/AIFF.h defines `ChunkHeader`, which the engine also does (measured; until G.3 the
# main target also shadowed both frameworks with type-only stubs in port/src/shim/foundation/framework/).
# Mixing the two would put CoreServices into ~80 engine translation units.
#
# So: separate compilation, SDK headers only, a plain-C interface (src/seam/AppleAudio_native.h)
# and no include path from this project except its own directory. If this ever fails to compile
# with a `ChunkHeader` redefinition, something has added an engine include path to it. These walls
# (with AppleNet and IOSKeyboard) are the only Objective-C the project compiles (G.3).
#
# NON-APPLE TARGETS HAVE NO SUCH PROBLEM AND NO SUCH LIBRARY. There is no CoreServices to collide
# with, so src/seam/PortableAudio_native.cpp is an ordinary member of the
# main target. It implements the same AppleAudio_native.h wall — which is why that wall was built.
if(APPLE)
  add_library(eden_apple_audio OBJECT ${EDEN_PORT_DIR}/src/seam/AppleAudio_native.mm)
  set_source_files_properties(${EDEN_PORT_DIR}/src/seam/AppleAudio_native.mm PROPERTIES
    LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++")
  target_compile_features(eden_apple_audio PRIVATE cxx_std_17)
  # ROADMAP 5.9: the world browser's HTTP backend (NSURLSession), behind the same kind of plain-C
  # wall (src/seam/NetBackend_native.h) and for the same reason — real SDK headers, kept apart from
  # the engine's. ARC, because it is new code with blocks and delegates and no reason to count.
  add_library(eden_apple_net OBJECT ${EDEN_PORT_DIR}/src/seam/AppleNet_native.mm)
  set_source_files_properties(${EDEN_PORT_DIR}/src/seam/AppleNet_native.mm PROPERTIES
    LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
  target_compile_features(eden_apple_net PRIVATE cxx_std_17)
  set(EDEN_AUDIO_OBJECTS $<TARGET_OBJECTS:eden_apple_audio> $<TARGET_OBJECTS:eden_apple_net>)
  # G.5b-4: --lit-oracle's fixed-function half (a CGL legacy 2.1 context), macOS only. Its own object
  # library for the same reason as the walls above: it needs the real <OpenGL/gl.h>, none of the shim's.
  if(NOT IOS)
    add_library(eden_lit_oracle OBJECT ${EDEN_PORT_DIR}/src/render/gl/lit_oracle_legacy_macos.cpp)
    target_compile_features(eden_lit_oracle PRIVATE cxx_std_17)
    list(APPEND EDEN_AUDIO_OBJECTS $<TARGET_OBJECTS:eden_lit_oracle>)
    set(EDEN_HAVE_LIT_ORACLE ON)
  endif()
  # C.4: the iOS keyboard-frame observer (real UIKit, the same wall shape).
  if(IOS)
    add_library(eden_ios_keyboard OBJECT ${EDEN_PORT_DIR}/src/seam/IOSKeyboard_native.mm)
    set_source_files_properties(${EDEN_PORT_DIR}/src/seam/IOSKeyboard_native.mm PROPERTIES
      LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
    target_compile_features(eden_ios_keyboard PRIVATE cxx_std_17)
    list(APPEND EDEN_AUDIO_OBJECTS $<TARGET_OBJECTS:eden_ios_keyboard>)
  endif()
else()
  set(EDEN_AUDIO_OBJECTS "")
endif()

# MACOSX_BUNDLE ON iOS ONLY, and the name of that property is CMake's, not a mistake: it is what
# produces a bundle on every Apple platform. On iOS there is no other choice — the OS installs
# .app directories, not executables — and the layout it produces is FLAT (App.app/eden_native,
# App.app/Eden.eden, App.app/Moof.edm, ...), which is exactly the layout the engine already
# assumes everywhere else: Classes/World.cpp's LoadModels() fopen()s bare basenames against
# eden_platform_bundle_root(), and on iOS that path IS the .app directory.
#
# So the asset step further down needs no iOS special case beyond copying instead of symlinking.
# That is not a coincidence: Xcode's Copy-Bundle-Resources phase flattened media/ in 2010, the
# desktop legs reproduce it with symlinks beside the executable (eden_bundle_path looks up flat names
# under eden_platform_bundle_root()). Four targets, one layout.
if(IOS)
  set(EDEN_EXE_KIND MACOSX_BUNDLE)
else()
  set(EDEN_EXE_KIND "")
endif()

add_executable(eden_native ${EDEN_EXE_KIND}
  ${EDEN_ENGINE_SOURCES}
  ${EDEN_SHARED_FOUNDATION}
  ${EDEN_SHARED_SEAM}
  ${EDEN_SHARED_OTHER}
  ${EDEN_NATIVE_SOURCES}
  ${EDEN_AUDIO_OBJECTS}
)
if(EDEN_HAVE_LIT_ORACLE)
  # gl_selftest.cpp links the real eden_lit_oracle_legacy instead of its "unavailable" stub.
  set_source_files_properties("${EDEN_PORT_DIR}/src/render/gl/gl_selftest.cpp" PROPERTIES
    COMPILE_DEFINITIONS EDEN_LIT_ORACLE_LEGACY=1)
endif()

# ---------------------------------------------------------------------------------------------
# The iOS app bundle (Phase N Stage 4.1)
# ---------------------------------------------------------------------------------------------
# Info.plist is generated from native/ios/Info.plist.in rather than written by CMake's default
# template, because four of its keys are requirements rather than metadata and CMake's template
# has none of them:
#
#   MinimumOSVersion            the installer reads THIS, not the Mach-O load command. A bundle
#                               without it is rejected by both the device and the simulator.
#   UILaunchScreen              iOS 14+ runs an app with no launch storyboard/scene in a
#                               letterboxed 4:3 window on every modern device — the classic
#                               "why are there black bars and why is my touch coordinate wrong"
#                               failure. An EMPTY dict is the documented way to say "plain black,
#                               full size", which is right for a game that draws its own splash.
#   UISupportedInterfaceOrientations
#                               Eden is landscape-only and always has been (the HUD, the menu
#                               and the touch layout are all authored for it).
#   UIFileSharingEnabled +      the only way a player gets a .eden world OFF an iOS device. See
#   LSSupportsOpeningDocumentsInPlace
#                               default_docs()'s iOS branch: this is what makes `Documents` the
#                               right save directory there and Application Support the wrong one.
#
# CFBundleSupportedPlatforms is configured rather than hard-coded because the same source tree
# builds both slices and the two spellings differ (iPhoneOS / iPhoneSimulator).
if(IOS)
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(EDEN_IOS_PLATFORM "iPhoneSimulator")
  else()
    set(EDEN_IOS_PLATFORM "iPhoneOS")
  endif()
  set(EDEN_IOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  set(EDEN_IOS_EXECUTABLE "Emod")   # see the note on OUTPUT_NAME below; both come from here
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
  # THE PRODUCT IS CALLED "Emod" ON iOS, and only on iOS. The CMake target keeps its name
  # (eden_native, as on every other platform, so the source tree and the docs do not fork), but
  # the bundle a person installs is `Emod.app` running `Emod` — because on this platform the
  # product name IS the OS-facing identity, which src/eden_app_identity.h says must not be "Eden".
  # CFBundleExecutable in ios/Info.plist.in is configured from the same variable; the two must
  # agree or the app will not launch.
  set_target_properties(eden_native PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    OUTPUT_NAME "${EDEN_IOS_EXECUTABLE}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "io.github.hagg3.emod"
    # CI has no signing identity and needs none: an unsigned .app installs on the SIMULATOR, which
    # is what the workflow gates on. A device build is a link/compile proof there; signing it is
    # the user's Xcode step (Stage 4.6).
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
endif()

# ---------------------------------------------------------------------------------------------
# Include paths — ORDER MATTERS
# ---------------------------------------------------------------------------------------------
# port/src/render/gl/framework MUST come first: it supplies <GLES/gl.h> and <GLES2/gl2.h> (the
# platform's real GL, for the GL backend in port/src/render/gl/ only), and the <OpenGLES/ES1/gl.h>
# and <OpenGLES/ES1/glext.h> the ENGINE headers include by name (Camera.h, World.h, Graphics.h, ...
# — angle-bracket includes, the only kind that can be redirected this way). Since G.5b-7 the engine
# pair declares GL types and a few enum values and no function, so an engine GL call cannot compile.
target_include_directories(eden_native PRIVATE
  "${EDEN_PORT_DIR}/src/shim/vendor/zstd"           # <zstd.h>: the PINNED 1.5.7, never Homebrew's/the SDK's
  "${EDEN_PORT_DIR}/src/render/gl/framework"         # <GLES/...>, <GLES2/...>, <OpenGLES/ES1/...>
  "${EDEN_PORT_DIR}/src/shim/foundation"
  "${EDEN_PORT_DIR}/src/render/gl"                   # gl_context.h (the host's context API)
  "${EDEN_PORT_DIR}/src/shim/vendor"                 # stb_image.h / stb_truetype.h
  "${EDEN_PORT_DIR}/src/seam"                        # DisplayProfile.h, LoadFailure.h
  "${EDEN_CLASSES_DIR}"
  "${EDEN_PARENT_DIR}"                              # a few Classes/*.mm quote root-level headers
)

# ---------------------------------------------------------------------------------------------
# ---------------------------------------------------------------------------------------------
# The prefix header
# ---------------------------------------------------------------------------------------------
# No source in the main target is Objective-C (G.3; the Apple walls are their own object libraries
# above), so there is no ObjC frontend, no `-x objective-c++`, no GNU-runtime flags
# (`-fobjc-runtime=gnustep-1.9`, `-fconstant-string-class`, `-fobjc-legacy-dispatch`) any more.
#
# What stays is Xcode's GCC_PRECOMPILE_PREFIX_HEADER behaviour: port/src/shim/foundation/eden_prefix.h
# force-included into the engine and the seam files that include engine headers, giving them the implicit
# environment the engine is written against (BOOL, CGRect, printg and its __OPTIMIZE__ gating, the
# arc4random hook). The rule: every C++ engine source and every seam/harness source gets it, MINUS the
# named exclusions below, which are plain C++ that never had it (and C files, which cannot take it).
# A new engine or seam .cpp needs no edit here; a plain-C++ file that must stay clean is added to a regex.
set(EDEN_PREFIX_SOURCES ${EDEN_ENGINE_SOURCES} ${EDEN_SHARED_SEAM} ${EDEN_HARNESS_SOURCES} ${EDEN_NATIVE_SOURCES}
  "${EDEN_PORT_DIR}/src/shim/foundation/md5.cpp")
list(REMOVE_DUPLICATES EDEN_PREFIX_SOURCES)
list(FILTER EDEN_PREFIX_SOURCES INCLUDE REGEX "\\.cpp$")
list(FILTER EDEN_PREFIX_SOURCES INCLUDE REGEX "/(Classes|src/seam|src/harness|src/shim/foundation)/|/Lighting\\.cpp$")
# Engine plain C++ (CreatureModel, EdenWorld*, WorldTrailer, StrUtil, ColorUtil, VectorUtil) and the seams/
# natives that wrap a platform API or stand alone (no engine headers).
list(FILTER EDEN_PREFIX_SOURCES EXCLUDE REGEX "/Classes/(CreatureModel|EdenWorld|WorldTrailer|StrUtil|ColorUtil|VectorUtil)")
list(FILTER EDEN_PREFIX_SOURCES EXCLUDE REGEX "/src/seam/(DisplayMode|Input|EdenAppDelegate|main|HeapProbe|TextRaster|CodecBench|WorldTools|Prefs|BaseSelftest|PlayerReadme|Net|PortableAudio|AutoreleasePool)_native\\.cpp$")
list(APPEND EDEN_PREFIX_SOURCES "${EDEN_PORT_DIR}/src/render/render_formats.cpp")   # R::kFmt* from the engine's vertex structs
if(MSVC)
  set(EDEN_PREFIX_INCLUDE "/FI${EDEN_PORT_DIR}/src/shim/foundation/eden_prefix.h")   # clang-cl / cl spelling
else()
  set(EDEN_PREFIX_INCLUDE "-include;${EDEN_PORT_DIR}/src/shim/foundation/eden_prefix.h")
endif()
set_source_files_properties(${EDEN_PREFIX_SOURCES} PROPERTIES
  COMPILE_OPTIONS "${EDEN_PREFIX_INCLUDE}")
# Stage H / H.4: the Classic generator's float noise must give the same bits on every target, so
# no fused multiply-add in its translation unit (clang's default contracts on arm64; x86-64 SSE2
# has no FMA to fuse into). With contraction off, IEEE + - * / and roundf/floorf are exact
# everywhere; --classic-selftest's hashes, pinned identically on every leg, are the proof.
# A mismatch there is a stop, and a flag lost here is the first suspect.
if(MSVC)
  set(EDEN_NO_FMA "/clang:-ffp-contract=off;/fp:precise")   # G.3's experimental clang-cl leg
else()
  set(EDEN_NO_FMA "-ffp-contract=off;-fno-fast-math")
endif()
set_property(SOURCE "${EDEN_PARENT_DIR}/Classes/TerrainGenerator.cpp" APPEND PROPERTY
  COMPILE_OPTIONS "${EDEN_NO_FMA}")

# ---------------------------------------------------------------------------------------------
# Definitions
# ---------------------------------------------------------------------------------------------
# TARGET_OS_IPHONE / BUILD_OGLES / EGL_NOT_PRESENT, which the shipped Xcode target set for the
# vendored PowerVR SDK, went with the SDK (LG.7, 2026-10-27): nothing else read them.
# TARGET_OS_IPHONE stays Apple's own (<TargetConditionals.h>); port/src/seam/DevConsole.cpp reads it
# only behind `defined(__APPLE__)`.
target_compile_definitions(eden_native PRIVATE
  EMOD_VERSION_STRING="${EMOD_VERSION}"
  $<$<BOOL:${EDEN_DIAGNOSTICS}>:EDEN_DIAGNOSTICS=1>
  $<$<BOOL:${EDEN_F3_HOOKS}>:EDEN_F3_HOOKS=1>
)

if(IOS)
  # THE THREE DEFINES THAT MAKE iOS iOS, all of them consumed by files shared with other targets:
  #
  #   EDEN_PLATFORM_IOS    the one this project owns. It guards the GL flavour
  #                        (port/src/render/gl/framework/GLES/gl.h -> <OpenGLES/ES3/gl.h>, and
  #                        gl_context_native.cpp's ES-profile request), the SDL_main
  #                        include, the Documents save root, and the system-font scan.
  #                        NOT `TARGET_OS_IPHONE`: that one is Apple's, it is already 1 here from
  #                        <TargetConditionals.h> — writing our own meaning into a name the SDK
  #                        owns is how a build stops being explicable.
  #   EDEN_GL_ES_DIALECT   the shader preamble in port/src/render/gl/gl_programs.cpp. Set here
  #                        because it is a property of the CONTEXT (ES 3.0) and not of the
  #                        compiler.
  #   EDEN_BUILD_HAS_THREADS is NOT set here — it comes from platform_shims.h, which already gets
  #                        it right for any target with real pthreads (Stage 3.2).
  target_compile_definitions(eden_native PRIVATE EDEN_PLATFORM_IOS=1 EDEN_GL_ES_DIALECT=1)
endif()

if(WIN32)
  # IGNORE_EXPORT — Classes/Export.h's own switch, and the one this build wants.
  #
  # That file is vendored cocos2d-x (docs/third-party.md), and on _WIN32 it defaults EXPORT_DLL to
  # `__declspec(dllimport)`: the header assumes anyone compiling for Windows is CONSUMING cocos2d
  # as a DLL. Classes/SimpleAudioEngine.h wears it, so every use of the class asked the linker for
  # an imported vtable that does not exist -- `undefined symbol: __declspec(dllimport) vtable for
  # CocosDenshion::SimpleAudioEngine`, which was the whole Windows link, and only Windows sees it
  # because every other arm of that header expands EXPORT_DLL to nothing.
  #
  # This port compiles SimpleAudioEngine INTO the executable, so neither import nor export is
  # right; Export.h spells that third case IGNORE_EXPORT. Defining it here rather than editing the
  # vendored header keeps `git diff pristine-engine` honest.
  target_compile_definitions(eden_native PRIVATE IGNORE_EXPORT)

  # BUILD_GLU32 — the same shape of problem in the other vendored header, Classes/glu.h (Mesa).
  # Its Windows branch is `(_MSC_VER || __MINGW32__) && _DLL -> __declspec(dllimport)`, and `_DLL`
  # is defined by any toolchain linking the CRT as a DLL, which MSYS2 CLANG64 does. So every
  # gluLookAt/gluPerspective/gluUnProject/gluErrorString call site declared an import while
  # Classes/project.c and Classes/error.c DEFINED the same symbol locally: 30-odd
  # -Winconsistent-dllimport warnings and an LNK4217 import-thunk fixup per call.
  #
  # It links either way -- this is the warning half of the IGNORE_EXPORT error above -- but the
  # noise sits in the only build this port has for Windows, and buried diagnostics are how the
  # real one gets missed. glu.h offers no "neither" switch (the plain-`extern` branch is
  # unreachable once `_DLL` is set, because the dllimport arm #undefs GLAPI first), so this takes
  # the dllexport arm: the NAME is a lie -- nothing here builds glu32.dll -- but it is the one
  # setting under which the declaration and the definition agree. The executable ends up with four
  # exported symbols, which costs nothing.
  #
  # Rule 2 of web/CLAUDE.md is why this is a compile definition rather than one line in glu.h:
  # a Windows linkage attribute is a platform difference, and those do not go in Classes/.
  target_compile_definitions(eden_native PRIVATE BUILD_GLU32)
endif()

# Same two suppressions web needs, for the same 2010-era-C++ reasons: ~90 narrowing initialisers
# in unmodified engine constant tables, and `register int i, j;` in TerrainGenerator.cpp's Perlin
# noise (legal C++98, removed in C++17). `-Wno-macro-redefined` is native-only: the ES1 enum block
# in port/src/render/gl/framework/GLES/gl.h and desktop <OpenGL/gl3.h> agree on values but not
# always on spelling, and a redefinition warning per token would bury real diagnostics.
target_compile_options(eden_native PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
  -Wno-macro-redefined
  # 2010-era C/C++ that has been getting away with type punning since -O0 never exploited it —
  # the same call web-port-final:web/CMakeLists.txt makes for its Release build, made unconditionally here
  # because native has no -O0 tree that would hide it.
  -fno-strict-aliasing
)

# ---------------------------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------------------------
# G.1(a): drop unreferenced code and data on every leg (Mac 3.77 -> 3.12 MB measured, G.0 report §2.5).
if(APPLE)
  target_link_options(eden_native PRIVATE -Wl,-dead_strip)
elseif(MSVC)
  target_compile_options(eden_native PRIVATE /Gy /Gw)
  target_link_options(eden_native PRIVATE /OPT:REF /OPT:ICF)
else()
  target_compile_options(eden_native PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(eden_native PRIVATE -Wl,--gc-sections)
endif()
# (W.3's Windows `-Wl,--allow-multiple-definition` went in G.3: the duplicate it allowed was the GNU ObjC
# runtime's per-function weak class-ref alias, and no ObjC is compiled off Apple any more.)
if(EDEN_SANITIZE)
  # -fno-omit-frame-pointer + -g for readable reports out of a Release (-O2) build. vptr is off: it was
  # useless against the shim's hand-written ObjC runtime (gone in G.3); turning it on is a separate change.
  target_compile_options(eden_native PRIVATE -fsanitize=address,undefined -fno-sanitize=vptr
    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  target_link_options(eden_native PRIVATE -fsanitize=address,undefined)
endif()

target_link_libraries(eden_native PRIVATE ${EDEN_SDL3_TARGET})
if(IOS)
  # OpenGLES, not OpenGL: iOS never had desktop GL. It is deprecated in favour of Metal and is
  # still present on every device this stage targets — and unlike the macOS deprecation, this one
  # has a clear successor path for later (SDL3 renders through Metal already; only this port's
  # ES1 translator would need one).
  #
  # UIKit and QuartzCore are SDL's, and they are named here because this leg links SDL STATICALLY:
  # a static library carries no dependencies of its own, so anything its objects reference has to
  # be on the final link line. SDL's package config does export them, and naming them anyway costs
  # nothing and turns a config regression into a duplicate rather than an undefined symbol.
  find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
  find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
  find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
  find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  target_link_libraries(eden_native PRIVATE ${OPENGLES_FRAMEWORK} ${UIKIT_FRAMEWORK}
                        ${QUARTZCORE_FRAMEWORK} ${COREGRAPHICS_FRAMEWORK})
elseif(APPLE)
  # Apple's GL is a framework, and it is genuinely deprecated — that is a Stage 3+ question
  # (Metal, or an ANGLE/MoltenGL layer), not something to paper over here.
  find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
  target_link_libraries(eden_native PRIVATE ${OPENGL_FRAMEWORK})
else()
  # Linux: libGL. Windows: opengl32, which exports GL 1.1 only — everything above it arrives
  # through src/render/gl/gl_loader_native.cpp, so this link line is smaller than it looks.
  find_package(OpenGL REQUIRED)
  target_link_libraries(eden_native PRIVATE OpenGL::GL)
endif()

if(APPLE)
  # Foundation + libobjc, for the Apple walls (AppleNet's NSURLSession, AppleAudio) and the frame's
  # autorelease pool. The main target includes only <objc/objc.h> and CGGeometry.h (eden_base.h), and
  # images are the port's own EdenImage, so CoreGraphics is not linked on macOS.
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(eden_native PRIVATE ${FOUNDATION_FRAMEWORK} objc)

  # Audio (Stage 2, src/seam/SimpleAudioEngine_native.cpp). AudioToolbox decodes — including the
  # IMA4 .caf that 428 of the 436 effects use, which is exactly the format a browser cannot read
  # and the web port therefore hand-decodes. AVFoundation streams the five long channels.
  # BOTH ARE APPLE-ONLY BY DESIGN, and are the only pieces of that file that are: the mixer is
  # SDL, and the decode/stream calls sit behind one function each so the non-Apple legs replace
  # those bodies rather than the file. See the file's own header before swapping either out.
  find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  target_link_libraries(eden_native PRIVATE ${AUDIOTOOLBOX_FRAMEWORK} ${AVFOUNDATION_FRAMEWORK})
else()
  # NO OBJC RUNTIME AND NO FOUNDATION: these targets compile no Objective-C at all (G.3), and the
  # audio decoders are vendored single headers. Beyond SDL3 + OpenGL + zlib + pthreads there is
  # exactly one library:
  if(WIN32)
    # psapi, for GetProcessMemoryInfo in src/seam/HeapProbe_native.cpp. (On Windows 7+ it is
    # really in kernel32 behind an API set, but naming psapi is what the header documents and
    # mingw ships the import library.)
    target_link_libraries(eden_native PRIVATE psapi)
  endif()
endif()

# ---------------------------------------------------------------------------------------------
# The world browser's HTTP backend (ROADMAP 5.9) — one per platform, see NetBackend_native.h
# ---------------------------------------------------------------------------------------------
# Apple: NSURLSession, already in eden_apple_net above (Foundation is linked anyway).
# Windows: WinHTTP, part of the OS — no DLL joins the release zip.
# Linux: libcurl when its headers are present (CI installs libcurl4-openssl-dev); without them
# the build still succeeds and the browser is simply not offered (NetNone_native.cpp).
if(APPLE)
  # nothing more to link
elseif(WIN32)
  target_sources(eden_native PRIVATE ${EDEN_PORT_DIR}/src/seam/NetWinHttp_native.cpp)
  target_link_libraries(eden_native PRIVATE winhttp)
else()
  find_package(CURL)
  if(CURL_FOUND)
    target_sources(eden_native PRIVATE ${EDEN_PORT_DIR}/src/seam/NetCurl_native.cpp)
    target_link_libraries(eden_native PRIVATE CURL::libcurl)
  else()
    message(STATUS "Eden native: libcurl not found — the world browser will not be offered")
    target_sources(eden_native PRIVATE ${EDEN_PORT_DIR}/src/seam/NetNone_native.cpp)
  endif()
endif()

find_package(ZLIB REQUIRED)   # Classes' RLE Eden.eden path includes <zlib.h>
target_link_libraries(eden_native PRIVATE ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(eden_native PRIVATE Threads::Threads)
if(MSVC)
  # G.3's experimental MSVC-ABI leg (clang-cl + vcpkg, CI job `windows-msvc`; adopted only if it passes
  # the Stage G gates, else MSYS2 CLANG64 stays the Windows build). The UCRT has no pthreads, <dirent.h>,
  # <unistd.h> or <strings.h>: vcpkg's pthreads4w and dirent, and two tiny headers in src/compat/msvc.
  find_package(PThreads4W REQUIRED)
  target_link_libraries(eden_native PRIVATE PThreads4W::PThreads4W)
  find_path(EDEN_DIRENT_DIR dirent.h REQUIRED)
  target_include_directories(eden_native PRIVATE ${EDEN_PORT_DIR}/src/compat/msvc "${EDEN_DIRENT_DIR}")
  target_compile_definitions(eden_native PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                             NOMINMAX PTW32_STATIC_LIB)
endif()
if(WIN32)
  target_link_libraries(eden_native PRIVATE ws2_32)   # port/src/net/NetSock.cpp (Winsock)
endif()

# Native has real pthreads, so Classes/World.cpp's load thread works unmodified (the web build needed
# a pthread stub; docs/web-port-archive.md).

# ---------------------------------------------------------------------------------------------
# The bundle: a FLAT directory of assets beside the executable
# ---------------------------------------------------------------------------------------------
# eden_platform_bundle_root() is the directory the executable sits in (the .app on iOS), and
# eden_bundle_path() looks bare basenames up there. So the assets have to be there — which is not a
# compromise: it is exactly the layout the shipped iOS app had. Xcode's Copy-Bundle-Resources phase
# FLATTENS the whole media/ tree, which is why Classes/World.cpp's LoadModels() can prefix the bundle
# root and have CreatureModel fopen `<that>/Moof.edm` (docs/resources-and-audio.md).
#
# Symlinks, not copies: the assets are ~7 MB of art the repo already has, and a copy would go
# stale the moment anyone edits one. The set mirrors web-port-final:web/CMakeLists.txt's --preload-file rules
# exactly — the same six media/ subdirectories, the same loose PNGs, report_flag.png from the repo
# ROOT (it is not under media/ and was missing from the web build for months for that reason), the
# seven creature models (.edm since LG.7; the .pod art they are converted from does not ship), and Eden.emod. The .psd/.DS_Store/AtlasTool exclusions web applies are
# unnecessary here: a symlink to an unread file costs nothing, where on web it was download weight.
set(EDEN_BUNDLE_DIR "$<TARGET_FILE_DIR:eden_native>")
set(EDEN_ASSET_GLOBS "")
foreach(sub textures menu menu_text ui ipad_menu)
  file(GLOB _f "${EDEN_PARENT_DIR}/media/${sub}/*.png")
  list(APPEND EDEN_ASSET_GLOBS ${_f})
endforeach()
file(GLOB _f "${EDEN_PARENT_DIR}/media/models/*.png" "${EDEN_PARENT_DIR}/media/models/*.edm"
             "${EDEN_PARENT_DIR}/media/models/MASKS/*.png" "${EDEN_PARENT_DIR}/media/*.png")
list(APPEND EDEN_ASSET_GLOBS ${_f})
# Audio (Stage 2). Flat, like everything else, because the engine passes BARE FILENAMES to
# SimpleAudioEngine ("explosion.caf", "Eden_title.mp3") — the iOS bundle was flat and that is what
# those call sites assume. ~560 more symlinks; a link to an unread file costs nothing here, where
# on web the same set needed a manifest and HTTP because it is ~45 MB of download.
file(GLOB _f "${EDEN_PARENT_DIR}/media/sound/*.caf" "${EDEN_PARENT_DIR}/media/sound/*.wav"
             "${EDEN_PARENT_DIR}/media/sound/*.aif"
             "${EDEN_PARENT_DIR}/media/sound/creatures/*.caf"
             "${EDEN_PARENT_DIR}/media/sound/game/*.caf" "${EDEN_PARENT_DIR}/media/sound/game/*.wav"
             "${EDEN_PARENT_DIR}/media/sound/ambientloops/*.wav"
             "${EDEN_PARENT_DIR}/media/new_sound/*.mp3"
             "${EDEN_PARENT_DIR}/media/music/*.mp3")
list(APPEND EDEN_ASSET_GLOBS ${_f} "${EDEN_PARENT_DIR}/report_flag.png")
# The design system's display face (Jersey 10, SIL OFL — OFL.txt beside it; the web UI self-hosted
# the same file until the web build was retired). TextRaster_native.cpp loads it by bare name from
# the bundle root for GL-kit chrome.
list(APPEND EDEN_ASSET_GLOBS "${EDEN_PARENT_DIR}/media/fonts/Jersey10-Regular.ttf")
# Rubik Regular (SIL OFL, OFL-Rubik.txt beside it): the body face for all non-Jersey text.
list(APPEND EDEN_ASSET_GLOBS "${EDEN_PARENT_DIR}/media/fonts/Rubik-Regular.ttf")

# S.6: the bundled default map ships as Eden.emod (11.8 MB, zstd-19, baked by `emod.py bake-default`
# from the repo-root Eden.eden, which stays as the source and is NOT bundled). It is read in place, so
# iOS no longer gzips it (Stage 4.4's Eden.eden.gz, 8.4 MB) nor inflates a 52.5 MiB Documents cache
# from it; a desktop bundle links it like every other asset.
list(APPEND EDEN_ASSET_GLOBS "${EDEN_PARENT_DIR}/Eden.emod")
list(REMOVE_DUPLICATES EDEN_ASSET_GLOBS)
list(LENGTH EDEN_ASSET_GLOBS EDEN_ASSET_COUNT)

# One custom command rather than one per file: 1,500 separate POST_BUILD steps is a slow build and
# an unreadable log. The script is generated at configure time and re-run on every build, which is
# what keeps a newly added asset from needing a reconfigure.
#
# COPY_ON_ERROR, for Windows: a symbolic link there needs Developer Mode or
# SeCreateSymbolicLinkPrivilege, and a GitHub Actions runner has neither. The fallback copies
# instead, which costs one 12 MB Eden.emod and ~7 MB of art ONCE — the `if(NOT EXISTS)` guard means
# a rebuild does nothing. On macOS and Linux nothing changes and the links stay links, which is
# what keeps an edited texture live without a rebuild.
# ON iOS THE ASSETS ARE COPIED, NOT LINKED, and that is not the Windows fallback's reason.
# A symlink inside an .app points at a path in the BUILD MACHINE's filesystem; the bundle is then
# installed on a device or a simulator where that path does not exist, so every texture, model and
# Eden.eden would be a dangling link and the app would boot to a black screen with a missing-asset
# log. (It also breaks code signing, which walks the bundle.) The cost is real — ~19 MB copied
# into the .app, 12 of it Eden.emod — and it is what makes Stage 4.4's asset-payload row a row.
set(EDEN_LINK_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/link-assets.cmake")
file(WRITE "${EDEN_LINK_SCRIPT}" "# generated by native/CMakeLists.txt — do not edit\n")
foreach(asset IN LISTS EDEN_ASSET_GLOBS)
  get_filename_component(base "${asset}" NAME)
  if(IOS)
    file(APPEND "${EDEN_LINK_SCRIPT}"
         "if(NOT EXISTS \"\${DEST}/${base}\")\n  file(COPY_FILE \"${asset}\" \"\${DEST}/${base}\")\nendif()\n")
  else()
    file(APPEND "${EDEN_LINK_SCRIPT}"
         "if(NOT EXISTS \"\${DEST}/${base}\")\n  file(CREATE_LINK \"${asset}\" \"\${DEST}/${base}\" SYMBOLIC COPY_ON_ERROR)\nendif()\n")
  endif()
endforeach()
add_custom_command(TARGET eden_native POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DDEST=${EDEN_BUNDLE_DIR} -P "${EDEN_LINK_SCRIPT}"
  COMMENT "Eden native: linking ${EDEN_ASSET_COUNT} bundle assets beside the executable")

# Windows: SDL3.dll (and anything else an imported target brings) beside the executable, or the
# build tree is not runnable and neither is the zip. The mingw runtime DLLs are NOT covered by
# this — CMake only knows about imported targets — and are collected by the packaging step in
# .github/workflows/native.yml, which can see the toolchain.
if(WIN32)
  add_custom_command(TARGET eden_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_RUNTIME_DLLS:eden_native>" "${EDEN_BUNDLE_DIR}"
    COMMAND_EXPAND_LISTS
    COMMENT "Eden native: copying imported-target DLLs beside the executable")
endif()

message(STATUS "Eden native: SDL3 ${SDL3_VERSION}, diagnostics=${EDEN_DIAGNOSTICS}, build=${CMAKE_BUILD_TYPE}, ${EDEN_ASSET_COUNT} bundle assets")
