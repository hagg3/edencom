# Toolchain — building the Eden web port

Status: **not yet run**. `emcc` is not installed on this machine (only `cmake` 4.4, `node`,
`python3` are present — see `../../WORKING/archive/PORT-STATUS-2026-08-13.md`). This file pins down the exact commands
so a future session with `emsdk` available can go straight to building instead of
re-researching the invocation.

## 1. Install emsdk (pinned version)

```sh
git clone https://github.com/emscripten-core/emsdk.git ../emsdk-src   # sibling of web/, or
                                                                        # anywhere outside the
                                                                        # git sub-repo — see
                                                                        # web/.gitignore's
                                                                        # `/emsdk/` entry if you
                                                                        # prefer it vendored
                                                                        # inside web/ instead.
cd ../emsdk-src
./emsdk install 3.1.74
./emsdk activate 3.1.74
source ./emsdk_env.sh
```

**Version pin: 3.1.74.** Chosen (this pass, not yet validated) as a recent-enough stable release
with mature WASMFS/OPFS and pthreads+OffscreenCanvas support (both load-bearing for plan
decision D1). Revisit if P0.3's threads+FS spike finds a specific bug fixed only in a later
release — record the change here with the reason, don't silently bump it.

## 2. Configure + build

```sh
cd web
emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release   # or Debug, see CMakeLists.txt's
                                                      # -O2 vs -O0/-g/-gsource-map split
cmake --build build
```

`EDEN_THREADED` (default `ON`, see `CMakeLists.txt`) selects plan D1's real-pthreads +
OffscreenCanvas + `PROXY_TO_PTHREAD` path. To build the single-thread fallback instead:

```sh
emcmake cmake -B build-singlethread -DEDEN_THREADED=OFF
cmake --build build-singlethread
```

## 3. Known-unresolved before this will actually succeed

See `../../WORKING/archive/PORT-STATUS-2026-08-13.md` "Open questions / risks" for the full list; the two biggest:

- **P0.1 (Objective-C link)**: `-fobjc-runtime=gnustep-2.0` needs a real GNU objc runtime
  library to link against under Emscripten — this repo does not vendor one, and whether
  Emscripten's clang can even target that runtime ABI out of the box is untested. This is
  THE spike question (web-port-plan.md P0.1: "Does the engine link to WASM with approach
  D3(a)?").
- **`@"..."` string literals**: see `src/shim/foundation/NSString.h`'s own risk note — clang's
  `-fconstant-string-class` mechanism needs a class with a very specific ivar layout that
  conflicts with this shim's `NSObject`/`NSString` as currently written. Not solved in this
  pass. Whatever the P0.1 spike concludes should update `CMakeLists.txt`'s
  `EDEN_OBJCXX_FLAGS` (currently missing `-fconstant-string-class` entirely) and this file.
- **PVRT SDK extension calls**: `gl_es1_shim.h`'s header comment flags that `PVRTPrint3DAPI.cpp`/
  `PVRTBackground.cpp`/`PVRShellAPI.cpp` reference IMG/ARB GL extension entry points with no
  Emscripten/WebGL2 equivalent; grep suggests they're dead code (never called from any engine
  file) but this needs confirming once a real link is attempted (does `--gc-sections` drop
  them, or does the build need to exclude those 3 files from `tools/engine-sources.txt`'s
  consumption in `CMakeLists.txt`?).

## 4. Hosting requirement — COOP/COEP (plan D1)

`EDEN_THREADED=ON` needs `SharedArrayBuffer`, which browsers only expose on pages served with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Most static-file dev servers (`python3 -m http.server`, GitHub Pages, etc.) do **not** send
these by default. Options once there's a real build to serve:

- A small dev server that sets both headers (a 10-line `http.server` subclass, or
  `npx http-server --cors -c-1 -P … ` won't set COEP — write a tiny custom Python/Node server;
  not written yet, TODO once `public/` has real build output to serve).
- For static hosts that can't set response headers at all: the well-known
  ["coi-serviceworker"](https://github.com/gzuidhof/coi-serviceworker) pattern — a same-origin
  Service Worker that re-fetches every response and adds the two headers client-side. Not
  vendored here; note it as the fallback if the dev-server route proves inconvenient.
- If neither is workable for a given deployment target, fall back to `EDEN_THREADED=OFF`
  (single-thread build, no `SharedArrayBuffer` needed) per plan D1's documented escape hatch.

## 5. Debug builds

`CMakeLists.txt` adds `-O0 -g -gsource-map` for `CMAKE_BUILD_TYPE=Debug`, per
`web-port-plan.md`'s "Toolchain baseline" (`-O2`/`-O3` release, source maps for debug). Source
maps let browser devtools show original `.mm`/`.cpp` lines instead of WASM offsets — verify the
emitted `.wasm.map` is actually being served (and not stripped by whatever hosts it) before
relying on this for the P1/P2 spikes.
