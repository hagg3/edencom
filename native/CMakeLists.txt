# native/CMakeLists.txt — Eden native (SDL3 + desktop GL) build.
#
# Phase N Stages 1-3 (WORKING/native-migration-plan-2026-09-04.md,
# WORKING/phase-n-stage3plus-plan-2026-09-05.md). Three platforms: macOS, Linux, Windows.
#
# Phase N Stage 4.1 added a FOURTH platform, iOS, and it is deliberately not a fourth arm: iOS is
# the `if(APPLE)` arm (real libobjc, real Foundation, Apple's audio frameworks — all inherited
# wholesale from the macOS leg) plus an `if(IOS)` overlay for the four things that genuinely
# differ: the GL flavour (OpenGL ES 3.0, not desktop GL), the app BUNDLE, the ten UIKit class
# names that exist for real on that platform, and SDL3 as a static library. Everything the search
# for those four turns up says `if(IOS)`; if you are reading an `if(APPLE)` block, iOS is in it.
#
# *** THE ONE STRUCTURAL SPLIT IN THIS FILE IS `if(APPLE)` vs `else()`, AND IT IS NOT ARBITRARY.
# IT IS THE OBJECTIVE-C RUNTIME. *** Read the ObjC block below before touching either arm:
#
#   APPLE      real libobjc + real Foundation. FORCED, not chosen (clang cannot lower any GNU
#              ObjC ABI to Mach-O). 3 files from web/src/shim/foundation/, no objc_runtime.cpp,
#              EDEN_USE_SYSTEM_FOUNDATION=1, Apple frameworks for audio.
#   NON-APPLE  this port's OWN runtime and Foundation shim, unmodified: objc_runtime.cpp plus all
#              19 web/src/shim/foundation/*.mm, `-fobjc-runtime=gnustep-1.9
#              -fconstant-string-class=NSConstantString`, and a portable audio backend.
#              ELF and PE/COFF both have COMDATs, which is the whole difference (measured, Stage
#              3's plan doc SS1). So Linux and Windows are the leg that reuses the web port's
#              proven shim stack, and macOS is the odd one out.
#
# Every other difference between the three is small and local: a GL entry-point loader on the two
# non-Apple targets (native/src/shim/gl/gl_loader_native.h explains why Windows cannot do without
# one), the heap probe's three implementations, and the filesystem root.
#
# HOW THIS RELATES TO web/CMakeLists.txt — read this before adding a source here:
#   * The ENGINE source list comes from the SAME cmake/EngineSources.cmake function web uses
#     (Stage 0.1 extracted it for exactly this), with this target's own exclude list.
#   * MOST OF THE PORT'S SEAM AND SHIM CODE IS SHARED, not copied. web/src/shim/foundation/*,
#     web/src/shim/objc/objc_runtime.cpp, web/src/shim/gl/gl_fixed_function.cpp and eleven
#     web/src/seam/*.mm files compile into this target unmodified. That is the finding the whole
#     plan rests on (the investigation measured under ~120 lines of real Emscripten API in
#     ~11.5 kLOC of seam+shim), and it is why native gets B3/B5/B6/C3/M3a/M6 for free.
#   * Files under native/ exist ONLY where the platform genuinely differs. There are nine of them.
#
# THE OBJECTIVE-C DECISION, AND THE MEASUREMENT THAT FORCED IT — read this before trying to
# "simplify" the native target back onto the port's own ObjC runtime and Foundation shim:
#
#   **clang cannot lower ANY GNU Objective-C ABI to Mach-O.** Measured on this machine with Apple
#   clang 17 (a one-file spike containing a single message send):
#       -fobjc-runtime=gnustep-1.9  ->  fatal error: error in backend: MachO doesn't support
#       -fobjc-runtime=gnustep-1.8      COMDATs, '.objc_sel_name<sel>' cannot be lowered.
#       -fobjc-runtime=gcc
#       -fobjc-runtime=objfw
#   All four fail identically, in the MC layer, so it is an object-format limitation and not a
#   flag to hunt for. (Compiling a file with no message send succeeds, which is exactly why a
#   naive spike says "it works" — the selector-name globals are only emitted when something
#   actually sends a message.)
#
# That kills, on macOS specifically, the otherwise-attractive option of reusing
# web/src/shim/objc/objc_runtime.cpp plus the whole Foundation shim. So this target does what the
# plan doc said from the start: **Apple's real libobjc and real Foundation**. The plan's stated
# reason for doing macOS first — "it tests SDL3 + desktop GL with the ObjC question already
# answered" — is, with hindsight, backwards: macOS is the leg with the LARGEST Foundation change,
# and LINUX is the one that reuses the proven shim stack unmodified (ELF has COMDATs; the GNU ABI
# is exactly what objc_runtime.cpp implements). Worth knowing when Stage 1's Linux leg is planned.
#
# WHAT THAT COSTS, AND WHAT IT DOES NOT:
#   * NOT reused here: web/src/shim/objc/* and 17 of the 18 web/src/shim/foundation/*.mm files.
#     Their HEADERS are still the include path the seam files use — each now defers to
#     <Foundation/Foundation.h> under EDEN_USE_SYSTEM_FOUNDATION (they are #imported by relative
#     path from the seam files, so an -I redirect could not have done it).
#   * STILL OURS ON EVERY TARGET, deliberately: the UIKit stub layer AND the CGImage layer in
#     web/src/shim/foundation/uikit_stubs.h. macOS has no UIKit at all, and the CGImage layer is
#     what web/src/seam/Texture2D_web.mm and Classes/Resources.mm's recolour pipeline are written
#     against — one tightly-packed straight-alpha RGBA8 buffer, which CoreGraphics has no
#     non-premultiplied bitmap context to reproduce. Keeping it ours is what lets Texture2D_web.mm
#     compile here UNCHANGED. It works only because nothing pulls in <CoreGraphics/CGImage.h>:
#     real Foundation reaches CGBase/CGGeometry and stops.
#   * A REAL BEHAVIOURAL DIFFERENCE to carry forward: the shim's NSFileHandle write-guard (which
#     is what produces the `.bak` / `.savetmp.bak` copies the load-failure dialog offers) does not
#     exist under real Foundation. The engine-side save journal (B5/C3, Classes/FileManager.mm) is
#     unaffected and still runs — but native currently has no automatic previous-save backup.
#     Stage 2 owes that, and it is not a footnote: it is the port's second line of defence against
#     a corrupt world.
#
# 3.21 for $<TARGET_RUNTIME_DLLS:...> (the Windows DLL copy below). Every CI runner and every
# current distro is well past it; this is not a compatibility surface anyone is defending.
cmake_minimum_required(VERSION 3.21)
project(EdenNative LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------------
# iOS (Phase N Stage 4.1) — the stage Phase N exists for
# ---------------------------------------------------------------------------------------------
# Configured with -DCMAKE_SYSTEM_NAME=iOS, which is what sets CMake's own `IOS` variable (and
# leaves APPLE true — see this file's header for why that is the right shape). Two knobs get a
# default here rather than being left to the caller, because both are load-bearing claims:
#
#   IPHONEOS_DEPLOYMENT_TARGET = 12.0   The stage's whole point. Eden.xcodeproj says 15.6 today —
#                                       the community patch raised it — and 12.0 is what puts the
#                                       A7/A8 devices (iPad Air 2, iPhone 6) back in range. Those
#                                       devices are unreachable for the web build at all, because
#                                       it needs WebGL 2, i.e. Safari 15.
#   arm64                               iOS 11 dropped 32-bit, so armv7/armv7s (which the .xcodeproj
#                                       still lists in VALID_ARCHS) cannot even be deployment
#                                       targets here. One slice, not three.
if(IOS)
  if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "" FORCE)
  endif()
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
  endif()
endif()

set(EDEN_PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(EDEN_CLASSES_DIR "${EDEN_PARENT_DIR}/Classes")
set(EDEN_WEB_DIR "${EDEN_PARENT_DIR}/web")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)

# Same default and same meaning as web/CMakeLists.txt: the scripted-drive probes
# (eden_debug_*, the [eden-p1] prints, DevConsole) are ON for Debug, OFF otherwise. Stage 1's
# whole deliverable comes out of those probes, so a Stage 1 measurement tree wants
# -DEDEN_DIAGNOSTICS=ON explicitly on top of Release — the same `build-relwdiag` combination the
# web side uses, and for the same reason (Release codegen, probes still present).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(EDEN_DIAGNOSTICS_DEFAULT ON)
else()
  set(EDEN_DIAGNOSTICS_DEFAULT OFF)
endif()
option(EDEN_DIAGNOSTICS "Compile the scripted-drive debug probes (eden_debug_*, DevConsole, the [eden-p1] prints)." ${EDEN_DIAGNOSTICS_DEFAULT})

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
include("${EDEN_PARENT_DIR}/cmake/EngineSources.cmake")

# Files to EXCLUDE from the authoritative 81 and replace with a native seam. This list is
# web/CMakeLists.txt's EDEN_SEAM_EXCLUDE with TWO additions, and it is a per-target list on
# purpose — Stage 0.1 generalised the mechanism precisely so EDEN_IOS_SEAM_EXCLUDE could differ.
# IT DOES NOT: Stage 4.1 measured the guess this comment used to carry ("iOS should NOT exclude
# Classes/EAGLView.mm") and rejected it. See the EDEN_IOS_SEAM_EXCLUDE block below the list for
# the reason, which is that EdenViewController.mm creates an ES 1.1 context and this renderer has
# not been ES 1.1 since the web port.
set(EDEN_NATIVE_SEAM_EXCLUDE
  # --- Surface + loop + touches: replaced by native/src/seam/SDLView_native.mm,
  # EdenViewController_native.{h,cpp}, EdenAppDelegate_native.{h,cpp}, main_native.{h,cpp},
  # src/entry/eden_main_native.cpp. Same reasoning as web — there is no CAEAGLLayer,
  # UIApplicationMain or CADisplayLink here either.
  "Classes/EAGLView.mm"
  "Classes/EdenViewController.mm"
  "Classes/EdenAppDelegate.mm"
  "main.m"

  # --- Raster: decodes via UIImage/CoreGraphics and rasterises strings via UIFont. Replaced by
  # web/src/seam/Texture2D_web.mm, which this target compiles UNCHANGED (stb_image for PNGs); only
  # its one string-rasterisation call is per-platform, and native/src/seam/TextRaster_native.cpp
  # supplies that half with stb_truetype.
  "Classes/Texture2D.mm"

  # --- File I/O: as on web, FileManager.mm/FileManagerHelper.mm are NOT excluded — the real,
  # tested RLE-column/directory/creature logic is compiled as-is over the Foundation shim.
  # FileArchive.mm stays excluded (referenced only from commented-out compress/decompress code).
  "Classes/FileArchive.mm"

  # --- Audio: stubbed in native/src/seam/seam_link_stubs_native.mm for Stage 1. Stage 2 gives it
  # a real backend behind SimpleAudioEngine.h's ~13-call contract; the plan names the approach
  # (port SimpleAudioEngine_web.mm's hand-rolled LPCM .caf decoder — it is portable C).
  "Classes/SimpleAudioEngine.mm"

  # --- Networking: NSURLConnection-based world sharing. Same disposition as web.
  "Classes/FileDownload.mm"
  "Classes/FileUpload.mm"
  "Classes/SharedList.mm"
  "Classes/ShareUtil.mm"
  "Classes/ShareMenu.mm"
  "Classes/Alert.mm"

  # --- Strip entirely: rating-prompt nag + analytics remnants (its only caller, EdenAppDelegate,
  # is itself excluded above).
  "Classes/Appirater.mm"

  # --- CF file hashing: replaced by web/src/shim/foundation/md5_web.mm, compiled here too.
  "Classes/md5.c"

  # --- Native text entry for world renaming. Stage 2 (SDL_StartTextInput + the engine's GL UI).
  "Classes/VKeyboard.mm"

  # === THE TWO ADDITIONS OVER WEB'S LIST ====================================================
  # PVRTglesExt.cpp CANNOT COMPILE ON macOS, and the reason is worth knowing before anyone tries
  # to un-exclude it. PVRTglesExt.h selects `#define PVRGetProcAddress(x) ::x` when
  # `__APPLE__ && TARGET_OS_IPHONE`, and the .cpp then names ~50 ES1 extension entry points as
  # real global symbols (::glBindVertexArrayOES, ::glClipPlanefIMG, ...). On iOS those exist in
  # the ES1 framework; on desktop GL they do not exist at all, so it is a wall of
  # undeclared-identifier errors, not a link failure you could stub past. Web sidesteps it by not
  # being __APPLE__ (its macro expands to NULL). native/src/seam/pvrt_matrix_palette_native.cpp
  # provides the two functions the file defines, including the matrix-palette entry-point install
  # that web does with -Wl,--wrap= (ld64 has no --wrap).
  "Classes/PVRTglesExt.cpp"
)

# EDEN_IOS_SEAM_EXCLUDE == EDEN_NATIVE_SEAM_EXCLUDE, AND THAT IS A DECISION, NOT A DEFAULT.
# (Phase N Stage 4.1; the plan's §"Stage 4" argues it in full and this file's comment above
# recorded the OPPOSITE guess, that iOS should keep Classes/EAGLView.mm because it is the
# original CAEAGLLayer/EAGLContext setup "preserved".)
#
# The measurement that decided it: Classes/EdenViewController.mm:28 creates
# kEAGLRenderingAPIOpenGLES1 — a real ES 1.1 context. This port's renderer has not been ES 1.1
# since the web build; it is the ES1 -> ES3/GL translator in web/src/shim/gl/gl_fixed_function.cpp
# (~1,900 LOC). Un-excluding EAGLView.mm would therefore not save work — it would FORK the
# renderer, give iOS the project's only ES-1.1 path, and forfeit the byte-identical-geometry
# guarantee every other target is held to. So SDL3 supplies UIApplicationMain, the CAEAGLLayer,
# the display link and the touches on iOS exactly as it does the window and the message pump on
# the desktop, and the four excluded shell files stay excluded on all four platforms.
#
# EAGLView.mm remains in the repo as the reference for the ES-1.1 context attributes and the
# retina/IS_IPAD metric logic — which is what "preserved" should have meant.
set(EDEN_IOS_SEAM_EXCLUDE ${EDEN_NATIVE_SEAM_EXCLUDE})

if(IOS)
  set(EDEN_TARGET_SEAM_EXCLUDE ${EDEN_IOS_SEAM_EXCLUDE})
else()
  set(EDEN_TARGET_SEAM_EXCLUDE ${EDEN_NATIVE_SEAM_EXCLUDE})
endif()

eden_collect_engine_sources(
  OUT_VAR EDEN_ENGINE_SOURCES
  SOURCES_FILE "${EDEN_WEB_DIR}/tools/engine-sources.txt"
  BASE_DIR "${EDEN_PARENT_DIR}"
  EXCLUDE_LIST ${EDEN_TARGET_SEAM_EXCLUDE}
)

# ---------------------------------------------------------------------------------------------
# Shared port sources (compiled from web/, NOT copied — see the header comment)
# ---------------------------------------------------------------------------------------------
# Foundation is Apple's here (see the header). What remains from web/src/shim/foundation/ is the
# part that is NOT Foundation:
#   uikit_stubs.mm  — UITouch/UIEvent/UIView/UIImage/UIFont/UIColor/UIScreen/UIApplication/
#                     EAGLContext plus THIS PORT'S CGImage. None of it exists on macOS.
#   md5_web.mm      — replaces Classes/md5.c (which is excluded); plain stdio + RFC-1321.
set(EDEN_SHARED_FOUNDATION
  uikit_stubs.mm md5_web.mm
  # Phase N Stage 2: the previous-save ".bak" rules, lifted out of the shim's NSFileHandle so both
  # targets run one implementation. Portable C++ — stdio plus g_save_inplace_threshold.
  save_backup.cpp
)

# --- NON-APPLE: the whole Foundation shim, unmodified (Phase N Stage 3.2) --------------------
# This is the first time since the web port that these files run outside Emscripten, and it is the
# stage's single largest new surface: NSString.mm is 529 lines, NSFileHandle/NSFileManager/NSBundle
# carry the engine's entire file I/O, and all of it was written against one libc. Two consequences
# worth stating rather than discovering:
#   * every one of them is now a four-target regression obligation (web + macOS + Linux + Windows),
#     and macOS is the target that does NOT exercise them — so a break here is invisible on the
#     development machine and shows up only in CI.
#   * M6's leak class applies directly. A shim class with a C++ ivar owes a -dealloc, because this
#     runtime's object_dispose() is a bare free() with no .cxx_destruct. That is why Stage 3's
#     verification runs headless-alloc-leak-probe and --torture on the new platforms.
#
# NSUserDefaults is the ONE file that genuinely forks: web's is write-through to localStorage via
# EM_JS. native/src/shim/foundation/NSUserDefaults_native.mm replaces it (see EDEN_NATIVE_SOURCES).
if(NOT APPLE)
  list(APPEND EDEN_SHARED_FOUNDATION
    NSObject.mm
    NSAutoreleasePool.mm
    NSString.mm
    NSData.mm
    NSNumber.mm
    NSArray.mm
    NSDate.mm
    NSFileHandle.mm
    NSFileManager.mm
    NSThread.mm
    NSBundle.mm
    NSErrorException.mm
    NSTimer.mm
    NSOperation.mm
    NSLogSupport.mm
    CoreFoundation.mm
  )
endif()
list(TRANSFORM EDEN_SHARED_FOUNDATION PREPEND "${EDEN_WEB_DIR}/src/shim/foundation/")

# Seam files shared verbatim with the web build. Each is here because it was AUDITED as portable
# in Stage 0.4 or in this stage, not because it happened to compile:
#   Texture2D_web       PNG decode + the texture upload contract (its one DOM-shaped function is
#                       split out to TextRaster_native.cpp).
#   DisplayProfile_web  the derived point space + device profiles. Pure arithmetic.
#   Settings_web        the kSettings[] table — the single source of truth for all 22 settings,
#                       and what Stage 2.5's GL settings screen is built on. Its --wrap
#                       interposers are behind EDEN_LD_WRAP (web-only); see the guard comment
#                       in that file for exactly what native gives up and why it does not matter
#                       for Stage 1.
#   Movement_web        the move/look input exports. --wrap half likewise guarded out.
#   MeshTiming_web      the timing accumulators. Its whole point is the --wrap half, so on native
#                       this file contributes only its (harmless) accessors; kept compiled so the
#                       eden_debug_mesh_timing() export exists on both targets (plan rule 4).
#   LoadFailure_web     the corrupt/truncated-save signal and .bak restore. Real file I/O only.
#   AppLifecycle_web    Stage 4.2's eden_app_will_background(): the "is it safe to save right now?"
#                       policy for save-on-background. Portable by construction -- it names no DOM,
#                       no Emscripten and no SDL -- and shared precisely so web's pagehide, this
#                       target's SDL_EVENT_QUIT and iOS's SDL_EVENT_WILL_ENTER_BACKGROUND cannot
#                       drift into three different ideas of when a save is safe.
#   Menu_web / Storage_web  C accessors over Menu.mm/the world list. NOT replacements — they are
#                       how any host, DOM or native, drives the menu, and they are what the Stage 1
#                       harness in eden_main_native.cpp uses.
#   Input_web           the touch/pointer/keyboard translator. Compiled whole for Stage 1 because
#                       the harness taps HUD buttons through it; the plan's "split it into a
#                       shared core + a thin SDL translator" is Stage 2's job, once both callers
#                       exist and the split can be made against real requirements.
set(EDEN_SHARED_SEAM
  Texture2D_web.mm
  DisplayProfile_web.mm
  Settings_web.mm
  Movement_web.mm
  MeshTiming_web.mm
  LoadFailure_web.mm
  AppLifecycle_web.mm
  Menu_web.mm
  Storage_web.mm
  Input_web.mm
)
list(TRANSFORM EDEN_SHARED_SEAM PREPEND "${EDEN_WEB_DIR}/src/seam/")

set(EDEN_SHARED_OTHER
  "${EDEN_WEB_DIR}/src/shim/audio/audiotoolbox_stub.cpp"  # failing AudioToolbox stubs, as on web
  "${EDEN_WEB_DIR}/src/shim/foundation/platform_shims.cpp" # arc4random + the portable clock + FS roots
  "${EDEN_WEB_DIR}/src/shim/gl/gl_fixed_function.cpp"      # the ES1 -> GL translator, Stage 0.2's split
)
if(APPLE)
  # NO objc_runtime.cpp: Apple's libobjc is the runtime here (see this file's header).
else()
  # This port's own GNU-ABI runtime, compiled unchanged. Stage 3.3 re-measured its ABI structs at
  # LP64/LLP64 widths — they had only ever been read off wasm32 IR, where sizeof(long) ==
  # sizeof(void*) hides a real difference — and __objc_exec_class() now checks the compiler's own
  # sizeof against the header's on the first module, so a future target whose layout differs again
  # aborts at startup with a message instead of corrupting the heap.
  list(APPEND EDEN_SHARED_OTHER "${EDEN_WEB_DIR}/src/shim/objc/objc_runtime.cpp")
endif()

# ---------------------------------------------------------------------------------------------
# Native-only sources — one file per genuine platform difference
# ---------------------------------------------------------------------------------------------
set(EDEN_NATIVE_SOURCES
  src/entry/eden_main_native.cpp
  src/seam/SDLView_native.mm
  src/seam/DisplayMode_native.cpp
  src/seam/Input_native.cpp
  src/seam/EdenViewController_native.cpp
  src/seam/EdenAppDelegate_native.cpp
  src/seam/main_native.cpp
  src/seam/HeapProbe_native.cpp
  src/seam/seam_link_stubs_native.mm
  src/seam/SimpleAudioEngine_native.mm
  src/seam/TextRaster_native.cpp
  src/seam/pvrt_matrix_palette_native.cpp
  src/shim/gl/gl_context_native.cpp
  src/shim/gl/gl_es1_extras_native.cpp
)
if(APPLE)
  # Real Foundation HAS NSAutoreleasePool, but not one this port can drain deterministically from
  # a non-UIKit run loop — see that file's header. Non-Apple uses the shim's own, listed above.
  list(APPEND EDEN_NATIVE_SOURCES src/shim/foundation/NSAutoreleasePool_native.mm)
else()
  list(APPEND EDEN_NATIVE_SOURCES
    # Preferences: web's shim writes through to localStorage via EM_JS, which does not exist here.
    src/shim/foundation/NSUserDefaults_native.mm
    # The GL entry-point table. Mandatory on Windows (opengl32.dll is GL 1.1 and nothing more) and
    # used on Linux too, deliberately, so the two non-Apple legs differ in nothing but the object
    # format. See src/shim/gl/gl_loader_native.h.
    src/shim/gl/gl_loader_native.cpp
    # Audio without Apple's frameworks: dr_mp3/dr_wav plus a hand-written IMA4-CAF decoder, behind
    # the SAME plain-C wall AppleAudio_native.h defines. The mixer, voice pool, channel model and
    # facade above it are shared and unchanged.
    src/seam/PortableAudio_native.cpp
    # One unwinder symbol clang references from every ObjC++ file with a C++ destructor in it,
    # whether or not that file contains an @try. ELF only; see the file for why forwarding to the
    # C++ personality is correct here rather than a stub.
    src/shim/objc/objc_personality_native.cpp
  )
endif()

if(EDEN_DIAGNOSTICS)
  list(APPEND EDEN_SHARED_SEAM
    "${EDEN_WEB_DIR}/src/seam/DebugState_web.mm"
    "${EDEN_WEB_DIR}/src/seam/DevConsole_web.mm")
  if(APPLE)
    # NOT objc_selftest.mm: it exists to exercise web/src/shim/objc/objc_runtime.cpp's dispatch,
    # ivar layout, super and category merging. Apple's runtime needs no such proof from us.
  else()
    # These targets DO run that runtime, and they are the first to run it at 64-bit widths, so the
    # proof is exactly what is wanted — Stage 3.3 grew the file the empty-base ivar-bias and
    # @"literal" cases that only fail off wasm32.
    list(APPEND EDEN_SHARED_SEAM "${EDEN_WEB_DIR}/src/shim/objc/objc_selftest.mm")
  endif()
endif()

# ---------------------------------------------------------------------------------------------
# The Apple audio wall — its own object library, and that is load-bearing
# ---------------------------------------------------------------------------------------------
# src/seam/AppleAudio_native.mm is the ONE file that must see the REAL <AudioToolbox/...> and
# <AVFoundation/...>. It cannot be part of the main target, because that target puts
# web/src/shim/foundation/framework/ on the include path, and that directory deliberately SHADOWS
# both frameworks with this port's own type-only stubs — for a measured reason (CoreServices'
# CarbonCore/AIFF.h defines `ChunkHeader`, which the engine also does; see that AudioToolbox.h's
# header comment). Unshadowing them globally to reach two APIs would pull CoreServices into ~80
# engine translation units.
#
# So: separate compilation, SDK headers only, a plain-C interface (src/seam/AppleAudio_native.h)
# and no include path from this project except its own directory. If this ever fails to compile
# with a `ChunkHeader` redefinition, something has added an engine include path to it.
#
# NON-APPLE TARGETS HAVE NO SUCH PROBLEM AND NO SUCH LIBRARY. There is no CoreServices to collide
# with and nothing to shadow, so src/seam/PortableAudio_native.cpp is an ordinary member of the
# main target. It implements the same AppleAudio_native.h wall — which is why that wall was built.
if(APPLE)
  add_library(eden_apple_audio OBJECT src/seam/AppleAudio_native.mm)
  set_source_files_properties(src/seam/AppleAudio_native.mm PROPERTIES
    LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++")
  target_compile_features(eden_apple_audio PRIVATE cxx_std_17)
  set(EDEN_AUDIO_OBJECTS $<TARGET_OBJECTS:eden_apple_audio>)
else()
  set(EDEN_AUDIO_OBJECTS "")
endif()

# MACOSX_BUNDLE ON iOS ONLY, and the name of that property is CMake's, not a mistake: it is what
# produces a bundle on every Apple platform. On iOS there is no other choice — the OS installs
# .app directories, not executables — and the layout it produces is FLAT (App.app/eden_native,
# App.app/Eden.eden, App.app/Moof.pod, ...), which is exactly the layout the engine already
# assumes everywhere else: Classes/World.mm's LoadModels() fopen()s bare basenames against
# [[NSBundle mainBundle] resourcePath], and on iOS that path IS the .app directory.
#
# So the asset step further down needs no iOS special case beyond copying instead of symlinking.
# That is not a coincidence: Xcode's Copy-Bundle-Resources phase flattened media/ in 2010, the
# desktop legs reproduce it with symlinks beside the executable, and web reproduces it in software
# (web/src/shim/foundation/NSBundle.mm). Four targets, one layout.
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
# native/src/shim/gl/framework MUST come first: it supplies <GLES/gl.h> and <GLES2/gl2.h> over
# desktop GL, and those names would otherwise not resolve at all here. web's gl framework dir
# supplies the <OpenGLES/ES1/gl.h> and <OpenGLES/ES1/glext.h> trampolines the ENGINE headers
# include by name (Camera.h, World.h, Graphics.h, ... — angle-bracket includes, which is the only
# kind that can be redirected this way; see gl_es1_shim.h's framework/ note).
target_include_directories(eden_native PRIVATE
  src/shim/gl/framework
  src/shim                                          # CDXMacOSXSupport.h — see that file
  "${EDEN_WEB_DIR}/src/shim/foundation/framework"   # <Foundation/...>, <UIKit/...>, <Availability.h>
  "${EDEN_WEB_DIR}/src/shim/gl/framework"           # <OpenGLES/ES1/...>, <EGL/egl.h>
  "${EDEN_WEB_DIR}/src/shim/foundation"
  "${EDEN_WEB_DIR}/src/shim/gl"
  "${EDEN_WEB_DIR}/src/shim/vendor"                 # stb_image.h / stb_truetype.h
  "${EDEN_WEB_DIR}/src/seam"                        # DisplayProfile_web.h, LoadFailure_web.h
  "${EDEN_CLASSES_DIR}"
  "${EDEN_PARENT_DIR}"                              # a few Classes/*.mm quote root-level headers
)

# ---------------------------------------------------------------------------------------------
# Case-mismatched #import shims — identical to web/CMakeLists.txt's block
# ---------------------------------------------------------------------------------------------
# macOS's default filesystem is case-INsensitive, so these already resolve here; the links are
# created anyway so a case-sensitive volume (or Linux) behaves the same. Never edit Classes/*.h to
# fix the quoted name.
#
# COPY_ON_ERROR is for Windows, and it is not defensive padding: creating a symbolic link on
# Windows needs either Developer Mode or SeCreateSymbolicLinkPrivilege, and a GitHub Actions
# windows-latest runner has neither by default. Without the fallback this is a hard configure
# failure on that platform — and a COPY is perfectly correct here, because the target is a header
# that is only ever read.
foreach(pair IN ITEMS "Glu.h=glu.h" "input.h=Input.h" "terrain.h=Terrain.h" "Vkeyboard.h=VKeyboard.h" "GLobals.h=Globals.h")
  string(REPLACE "=" ";" pair "${pair}")
  list(GET pair 0 wrong_case)
  list(GET pair 1 real_file)
  if(NOT EXISTS "${EDEN_CLASSES_DIR}/${wrong_case}")
    file(CREATE_LINK "${real_file}" "${EDEN_CLASSES_DIR}/${wrong_case}" SYMBOLIC COPY_ON_ERROR)
  endif()
endforeach()

# ---------------------------------------------------------------------------------------------
# Objective-C++
# ---------------------------------------------------------------------------------------------
# As on web, .mm sources are forced to LANGUAGE CXX and given `-x objective-c++` explicitly.
# `LANGUAGE CXX` ALONE IS NOT ENOUGH — it does not select the ObjC frontend, and without
# `-x objective-c++` clang compiles the file as plain C++, `__OBJC__` is never defined, and
# Eden_Prefix.pch's `#ifdef __OBJC__` block silently gives the engine no Foundation at all. That
# cost the web port ~900 errors once; it is written down in both files now.
# NOTE the `.cpp` files in here, which is not an oversight and is what web/CMakeLists.txt does
# too: a plain-C++ seam file that transitively reaches a Foundation header still needs the ObjC
# FRONTEND to parse it, even though it contains no ObjC of its own. src/seam/
# EdenViewController_native.cpp is the live case (it includes Classes/World.h and the autorelease
# pool header). Compiled as C++ it reports "expected unqualified-id" on every `@interface` inside
# the SDK's own Foundation headers — an error that names Apple's files and not ours.
# The GL shim sources are deliberately NOT here: they include no Foundation and never should.
set(EDEN_MM_SOURCES ${EDEN_SHARED_FOUNDATION} ${EDEN_SHARED_SEAM})
foreach(src IN LISTS EDEN_NATIVE_SOURCES)
  if(src MATCHES "^src/(seam|entry)/")
    list(APPEND EDEN_MM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
  endif()
endforeach()
# Every src/shim/foundation/*.mm this target compiles is ObjC too; which ones those are depends on
# the platform (see EDEN_NATIVE_SOURCES), so filter the list rather than naming files twice.
foreach(src IN LISTS EDEN_NATIVE_SOURCES)
  if(src MATCHES "^src/shim/foundation/.*\\.mm$")
    list(APPEND EDEN_MM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
  endif()
endforeach()
# ...and two exceptions in the other direction. src/seam/PortableAudio_native.cpp is caught by the
# seam/ pattern above but is deliberately plain C++: it includes dr_mp3/dr_wav, reaches no
# Foundation, and has the same "no ObjC frontend, ever" rule the GL shim sources do.
if(NOT APPLE)
  list(REMOVE_ITEM EDEN_MM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/seam/PortableAudio_native.cpp")
  # ...and src/seam/HeapProbe_native.cpp, which on Windows includes <windows.h>. It reaches no
  # Foundation and needs no ObjC frontend, and keeping the two apart is exactly the containment
  # rule the GL header follows: windows.h is fine in a file that has no `BOOL` to collide with.
  list(REMOVE_ITEM EDEN_MM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/seam/HeapProbe_native.cpp")
endif()
foreach(src IN LISTS EDEN_ENGINE_SOURCES)
  if(src MATCHES "\\.mm$")
    list(APPEND EDEN_MM_SOURCES ${src})
  endif()
endforeach()
set_source_files_properties(${EDEN_MM_SOURCES} PROPERTIES LANGUAGE CXX)

# `-x objective-c++` selects the ObjC frontend (see the note above — LANGUAGE CXX alone does not).
# `-fno-objc-arc` is CLAUDE.md convention #6: this engine is manual retain/release throughout and
# predates ARC; do not enable it.
#
# WHAT IS ABSENT AND WHY, since web/CMakeLists.txt has two more flags here:
#   -fobjc-runtime=gnustep-1.9        cannot be lowered to Mach-O at all (this file's header).
#                                     The default on Darwin is the Apple runtime, which is what
#                                     libobjc implements, so no flag is needed.
#   -fconstant-string-class=NSConstantString
#                                     only meaningful for the GNU runtimes. Under Apple's runtime
#                                     `@"..."` lowers to a CFConstantString whose class symbol
#                                     CoreFoundation provides — i.e. it just works, and the shim's
#                                     three-word NSConstantString is not involved.
set(EDEN_OBJCXX_FLAGS
  -x objective-c++
  -fno-objc-arc
)
if(NOT APPLE)
  # The two flags the paragraph above explains are absent on Apple. Here they are REQUIRED and
  # they are exactly web/CMakeLists.txt's, because this is the same runtime.
  #
  #   -fobjc-runtime=gnustep-1.9  the newest GNU ABI clang can actually lower (gnustep-2.0 fails
  #                               at compile time, measured; see web/CMakeLists.txt). Stage 3
  #                               measured it lowering cleanly to both ELF and PE/COFF.
  #   -fconstant-string-class=NSConstantString
  #                               the engine's ~740 @"literal"s become instances of the shim's
  #                               three-word NSConstantString (web/src/shim/foundation/NSString.h).
  #                               Stage 3.3 re-verified that class's ivar offsets at LP64/LLP64 —
  #                               it is the highest-consequence instance of the empty-base bias.
  #   -fobjc-legacy-dispatch      *** THE FLAG WITHOUT WHICH NEITHER NEW PLATFORM LINKS, AND THE
  #                               ONE THE PLAN DID NOT ANTICIPATE. *** Measured on the Linux
  #                               runner and then in clang's own source: message sends are lowered
  #                               through CodeGenOptions::getObjCDispatchMethod(), which defaults
  #                               to `Mixed` on x86-64/aarch64 and to `Legacy` on wasm32. `Mixed`
  #                               emits a direct call to `objc_msgSend` — an assembly trampoline
  #                               the GNUstep runtime provides and THIS PORT DOES NOT, and cannot,
  #                               because forwarding an arbitrary signature is not expressible in
  #                               C++. `Legacy` emits `objc_msg_lookup_sender` +
  #                               `objc_slot_lookup_super`, which are exactly the two entry points
  #                               web/src/shim/objc/objc_runtime.cpp implements, and are exactly
  #                               what wasm32 has been emitting all along.
  #
  #                               So the shim was never wasm-specific — the DEFAULT was. Verified
  #                               that the flag changes nothing else: the emitted class struct and
  #                               every ivar offset are byte-identical with and without it.
  #
  #                               Stage 3's plan measured that the GNU ObjC ABI *lowers* to COFF
  #                               and called the Windows risk closed. That was true and it was not
  #                               the whole question: lowering the metadata is not the same as
  #                               emitting a dispatch this runtime answers. Worth carrying into
  #                               Stage 4/6 — check the entry points, not just the object format.
  list(APPEND EDEN_OBJCXX_FLAGS
    -fobjc-runtime=gnustep-1.9
    -fconstant-string-class=NSConstantString
    -fobjc-legacy-dispatch
  )
endif()
set_source_files_properties(${EDEN_MM_SOURCES} PROPERTIES COMPILE_OPTIONS "${EDEN_OBJCXX_FLAGS}")

# Force-include the UNMODIFIED ../Eden_Prefix.pch for ENGINE sources only, reproducing Xcode's
# GCC_PRECOMPILE_PREFIX_HEADER behaviour — including the NSLog/printg macros' __OPTIMIZE__-gated
# behaviour. Same split as web: this port's own shim/seam/entry files manage their own includes.
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
# Definitions
# ---------------------------------------------------------------------------------------------
# TARGET_OS_IPHONE / BUILD_OGLES are the shipped Xcode target's own settings (read out of
# Eden.xcodeproj), not this port's invention — missing them silently changes which code the
# vendored PVRT SDK compiles. EGL_NOT_PRESENT is honest here for the same reason it is on web:
# there is no EGL in this build.
#
# TARGET_OS_IPHONE matters MORE here than on web: `__APPLE__` is true, so PVRTContext.h and
# PVRTglesExt.h take their Apple branch and `#import <OpenGLES/ES1/gl.h>` — which resolves to the
# shim's trampoline. Defining it makes that deterministic rather than dependent on whether some
# system header happened to pull in <TargetConditionals.h> first (which would define it to 0, and
# `#ifdef` would still be true — the kind of accident that fails differently on two machines).
#
# EDEN_LD_WRAP is deliberately NOT defined: Apple's ld64 has no --wrap. See the guard comment in
# web/src/seam/Settings_web.mm.
target_compile_definitions(eden_native PRIVATE
  # NOTE THE ABSENCE OF `TARGET_OS_IPHONE`, which web/CMakeLists.txt does define. It must NOT be
  # forced here: real Foundation pulls in <TargetConditionals.h>, which defines it (to 0), and
  # overriding that would tell every Apple system header it is building for iOS. The PVRT headers
  # test `#ifdef TARGET_OS_IPHONE`, not its VALUE — so the SDK's own 0 already selects the
  # `#import <OpenGLES/ES1/gl.h>` branch this port wants, which resolves to the GL shim's
  # trampoline. Nothing is lost; the only consumer of the paired
  # `#if defined(__APPLE__) && defined(TARGET_OS_IPHONE)` macro is PVRTglesExt.cpp, which is
  # seam-excluded above.
  BUILD_OGLES
  EGL_NOT_PRESENT
  $<$<BOOL:${EDEN_DIAGNOSTICS}>:EDEN_DIAGNOSTICS=1>
)

if(IOS)
  # THE THREE DEFINES THAT MAKE iOS iOS, all of them consumed by files shared with other targets:
  #
  #   EDEN_PLATFORM_IOS    the one this project owns. It guards the GL flavour
  #                        (native/src/shim/gl/framework/GLES/gl.h -> <OpenGLES/ES3/gl.h>, and
  #                        gl_context_native.cpp's ES-profile request), the ten UIKit class
  #                        renames in web/src/shim/foundation/uikit_stubs.h, the SDL_main
  #                        include, the Documents save root, and the system-font scan.
  #                        NOT `TARGET_OS_IPHONE`: that one is Apple's, it is already 1 here from
  #                        <TargetConditionals.h>, and the PVRT headers read it — writing our own
  #                        meaning into a name the SDK owns is how a build stops being explicable.
  #   EDEN_GL_ES_DIALECT   the shader preamble in web/src/shim/gl/gl_fixed_function.cpp. Set here
  #                        because it is a property of the CONTEXT (ES 3.0) and not of the
  #                        compiler; Emscripten sets it for itself in that file.
  #   EDEN_BUILD_HAS_THREADS is NOT set here — it comes from platform_shims.h, which already gets
  #                        it right for any target with real pthreads (Stage 3.2).
  target_compile_definitions(eden_native PRIVATE EDEN_PLATFORM_IOS=1 EDEN_GL_ES_DIALECT=1)
endif()

if(APPLE)
  # Switches every web/src/shim/foundation/*.h from "declare this port's class" to "defer to
  # <Foundation/Foundation.h>", and the framework/ trampolines from shadowing the SDK to
  # #include_next-ing it. Defined ONLY on this arm; neither the web build nor the non-Apple
  # native ones ever see it, because they HAVE no system Foundation to defer to.
  target_compile_definitions(eden_native PRIVATE EDEN_USE_SYSTEM_FOUNDATION=1)
else()
  # TARGET_OS_IPHONE, which the Apple arm must NOT define (see the paragraph above: real
  # Foundation's <TargetConditionals.h> already defines it to 0, and forcing it would tell every
  # SDK header it is building for iOS). Off Apple there is no TargetConditionals.h at all, so the
  # PVRT headers' `#ifdef TARGET_OS_IPHONE` would be FALSE and PVRTContext.h would take its
  # desktop-GL branch — which is the wrong one for a target whose GL is the ES1 shim. web/
  # CMakeLists.txt defines it for exactly the same reason, and this is exactly web's situation.
  target_compile_definitions(eden_native PRIVATE TARGET_OS_IPHONE)
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
# in unmodified engine constant tables, and `register int i, j;` in TerrainGenerator.mm's Perlin
# noise (legal C++98, removed in C++17). `-Wno-macro-redefined` is native-only: the ES1 enum block
# in native/src/shim/gl/framework/GLES/gl.h and desktop <OpenGL/gl3.h> agree on values but not
# always on spelling, and a redefinition warning per token would bury real diagnostics.
target_compile_options(eden_native PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
  -Wno-macro-redefined
  # 2010-era C/C++ that has been getting away with type punning since -O0 never exploited it —
  # the same call web/CMakeLists.txt makes for its Release build, made unconditionally here
  # because native has no -O0 tree that would hide it.
  -fno-strict-aliasing
)

# ---------------------------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------------------------
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
  # through src/shim/gl/gl_loader_native.cpp, so this link line is smaller than it looks.
  find_package(OpenGL REQUIRED)
  target_link_libraries(eden_native PRIVATE OpenGL::GL)
endif()

if(APPLE)
  # Real Foundation + the runtime it sits on. CoreGraphics is NOT linked and NOT included beyond
  # CGBase/CGGeometry (which Foundation pulls in for NSGeometry) — see this file's header for why
  # the CGImage layer stays this port's own.
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(eden_native PRIVATE ${FOUNDATION_FRAMEWORK} objc)

  # Audio (Stage 2, src/seam/SimpleAudioEngine_native.mm). AudioToolbox decodes — including the
  # IMA4 .caf that 428 of the 436 effects use, which is exactly the format a browser cannot read
  # and the web port therefore hand-decodes. AVFoundation streams the five long channels.
  # BOTH ARE APPLE-ONLY BY DESIGN, and are the only pieces of that file that are: the mixer is
  # SDL, and the decode/stream calls sit behind one function each so the non-Apple legs replace
  # those bodies rather than the file. See the file's own header before swapping either out.
  find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  target_link_libraries(eden_native PRIVATE ${AUDIOTOOLBOX_FRAMEWORK} ${AVFOUNDATION_FRAMEWORK})
else()
  # NO RUNTIME AND NO FOUNDATION TO LINK. That is the finding, not an omission: on these targets
  # the ObjC runtime is web/src/shim/objc/objc_runtime.cpp and Foundation is
  # web/src/shim/foundation/*.mm, both compiled into the executable, and the audio decoders are
  # vendored single headers. Beyond SDL3 + OpenGL + zlib + pthreads there is exactly one library:
  if(WIN32)
    # psapi, for GetProcessMemoryInfo in src/seam/HeapProbe_native.cpp. (On Windows 7+ it is
    # really in kernel32 behind an API set, but naming psapi is what the header documents and
    # mingw ships the import library.)
    target_link_libraries(eden_native PRIVATE psapi)
  endif()
endif()

find_package(ZLIB REQUIRED)   # Classes' RLE Eden.eden path includes <zlib.h>
target_link_libraries(eden_native PRIVATE ZLIB::ZLIB)

find_package(Threads REQUIRED)
target_link_libraries(eden_native PRIVATE Threads::Threads)

# NOTE, because it is the quiet upside of this whole target: web/src/seam/pthread_sync_web.c is
# NOT built here. That file exists because Emscripten's non-pthread build has a stub
# pthread_create that never runs the routine, so World.mm's async world load would hang forever.
# Native has real pthreads, so Classes/World.mm's load thread and Classes/MeshPool.mm's worker
# pool both work unmodified — B3's off-thread mesher included.

# ---------------------------------------------------------------------------------------------
# The bundle: a FLAT directory of assets beside the executable
# ---------------------------------------------------------------------------------------------
# Real Foundation means real NSBundle, and for a plain (non-.app) executable
# `[[NSBundle mainBundle] resourcePath]` is the directory the executable sits in. So the assets
# have to be there — which is not a compromise: it is exactly the layout the shipped iOS app had.
# Xcode's Copy-Bundle-Resources phase FLATTENS the whole media/ tree, which is why
# Classes/World.mm's LoadModels() can do `[[NSBundle mainBundle] resourcePath] + "/"` and have
# CPVRTResourceFile fopen `<that>/Moof.pod` (docs/resources-and-audio.md). Web reproduces the same
# flat lookup in software (web/src/shim/foundation/NSBundle.mm indexes every basename once);
# native gets it from the filesystem instead.
#
# Symlinks, not copies: the assets are ~7 MB of art the repo already has, and a copy would go
# stale the moment anyone edits one. The set mirrors web/CMakeLists.txt's --preload-file rules
# exactly — the same six media/ subdirectories, the same loose PNGs, report_flag.png from the repo
# ROOT (it is not under media/ and was missing from the web build for months for that reason), the
# seven .pod models, and Eden.eden. The .psd/.DS_Store/AtlasTool exclusions web applies are
# unnecessary here: a symlink to an unread file costs nothing, where on web it was download weight.
set(EDEN_BUNDLE_DIR "$<TARGET_FILE_DIR:eden_native>")
set(EDEN_ASSET_GLOBS "")
foreach(sub textures menu menu_text ui ipad_menu)
  file(GLOB _f "${EDEN_PARENT_DIR}/media/${sub}/*.png")
  list(APPEND EDEN_ASSET_GLOBS ${_f})
endforeach()
file(GLOB _f "${EDEN_PARENT_DIR}/media/models/*.png" "${EDEN_PARENT_DIR}/media/models/*.pod"
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
list(APPEND EDEN_ASSET_GLOBS ${_f}
     "${EDEN_PARENT_DIR}/report_flag.png"
     "${EDEN_PARENT_DIR}/Eden.eden")
list(REMOVE_DUPLICATES EDEN_ASSET_GLOBS)
list(LENGTH EDEN_ASSET_GLOBS EDEN_ASSET_COUNT)

# One custom command rather than one per file: 1,500 separate POST_BUILD steps is a slow build and
# an unreadable log. The script is generated at configure time and re-run on every build, which is
# what keeps a newly added asset from needing a reconfigure.
#
# COPY_ON_ERROR, for Windows: a symbolic link there needs Developer Mode or
# SeCreateSymbolicLinkPrivilege, and a GitHub Actions runner has neither. The fallback copies
# instead, which costs one 52 MB Eden.eden and ~7 MB of art ONCE — the `if(NOT EXISTS)` guard means
# a rebuild does nothing. On macOS and Linux nothing changes and the links stay links, which is
# what keeps an edited texture live without a rebuild.
# ON iOS THE ASSETS ARE COPIED, NOT LINKED, and that is not the Windows fallback's reason.
# A symlink inside an .app points at a path in the BUILD MACHINE's filesystem; the bundle is then
# installed on a device or a simulator where that path does not exist, so every texture, model and
# Eden.eden would be a dangling link and the app would boot to a black screen with a missing-asset
# log. (It also breaks code signing, which walks the bundle.) The cost is real — ~60 MB copied
# into the .app, 52 of it Eden.eden — and it is what makes Stage 4.4's asset-payload row a row.
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
