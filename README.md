# Eden: Community Edition (Emod)

Eden: World Builder was a voxel sandbox for iPhone and iPad, made by Ari Ronen (Kingly Games) and
released in 2010. You walked around a big blocky world, dug it up, built things, set fire to them,
and flooded the rest. A lot of people grew up on it. It was pulled from the App Store a while ago,
and the iOS version it shipped on doesn't run on much anymore.

Emod is a community fork that gets the original game running again, natively, on modern machines:

- **macOS, Linux and Windows** (SDL3 + OpenGL)
- **iOS / iPadOS** (unsigned builds you sideload; see Releases)
- Android is planned, not started

It's the real engine, not a remake. The original 2.1.1 source (released by Ari in 2018) is compiled
as-is, and a thin layer underneath stands in for the iOS APIs it expected. The default world,
the blocks, the creatures, the lighting and the water all behave the way you remember them,
because it's the same code doing it.

Join the Eden community on [Discord](http://discord.gg/rjYXwBC).

## Playing

There are no prebuilt desktop downloads yet, so for now you build it (below). iOS test builds are
posted under [Releases](https://github.com/hagg3/edencom/releases) as unsigned `.ipa` files.

Default controls on desktop (rebindable in Settings):

| | |
|---|---|
| Move | WASD / arrow keys |
| Look | Mouse |
| Mine / build / pick block | Left / right / middle click |
| Jump / fly up | Space |
| Crouch / fly down | Ctrl |
| Sprint / walk | Shift / Alt |
| Hotbar | Mouse wheel, or 1–9 |
| Block / colour picker | E / C |
| Fire | F |
| Fly mode | V |
| Menu | Esc |

Gamepads work too (sticks to move and look, triggers to mine and build, Start for the menu). On
iPhone and iPad you get the original touch controls.

Your worlds are saved here, and are never shared with the old Eden app's data:

- macOS: `~/Library/Application Support/Emod`
- Linux: `~/.local/share/emod/Documents` (or `$XDG_DATA_HOME/emod/Documents`)
- Windows: `%APPDATA%\Emod\Documents`

## What's different from the original

The aim is to leave the game alone, but a few things had to change to keep it alive:

- **A new world format, `.emod`.** Saves are append-only and checksummed, so a crash mid-save can't
  corrupt a world. Old `.eden` worlds still import and export.
- **The default world ships as `Eden.emod`**, baked from the original `Eden.eden` map.
- **Extras behind settings:** adjustable render distance and a fly mode (Settings › Experiments).
- The engine's OpenGL ES 1.1 calls are translated to modern GL/ES3 at runtime, since desktop
  drivers don't speak ES 1.1.

## Building

You need a C++/Objective-C++ compiler, CMake and SDL3.

```
cd native
cmake -B build -DCMAKE_BUILD_TYPE=Release -DEDEN_DIAGNOSTICS=ON
cmake --build build -j8
./build/eden_native
```

On macOS, `brew install sdl3` first. On Linux you also need the usual X11/Wayland, audio and GL
dev packages, and SDL3 itself (Ubuntu only packages it from 25.04). Windows builds with MSYS2's
CLANG64 toolchain, because MSVC can't compile Objective-C. iOS needs Xcode and a static SDL3.

Don't take the exact package lists from this README: [`.github/workflows/native.yml`](.github/workflows/native.yml)
is the recipe that actually builds all four platforms on every push, and it's the one that's kept
working.

## Hacking on it

Where things live:

| Path | What's there |
|---|---|
| `Classes/`, `Lighting.mm` | The Eden engine, in Objective-C++. Editable; the untouched import is tagged `pristine-engine`. |
| `port/` | The shared compatibility layer: an ES 1.1 → ES3 translator and, on Linux/Windows, a small Objective-C runtime and Foundation subset. Also build and format tools. |
| `native/` | The SDL3 app: window, input, audio, file I/O, networking, and the CMake build. |
| `docs/` | Engine reference. Voxel storage, meshing, lighting, save formats, worldgen and so on. |
| `media/` | Textures, models, sounds and music. |

Start with [`docs/architecture-overview.md`](docs/architecture-overview.md), then
[`docs/conventions-and-pitfalls.md`](docs/conventions-and-pitfalls.md) before you change anything.
The engine has sharp edges: terrain calls take `(x, z, y)` with y vertical, chunk data lives in
toroidal arrays, block ids are full at 127 and can't be renumbered, and several structs are written
to disk byte-for-byte. The pitfalls doc covers all of it.

The docs were written for the original iOS target. The engine internals in them are accurate; the
build and platform framing is out of date. `git diff pristine-engine -- Classes/` shows exactly what
this fork has changed in the engine.

There's no test framework. The binary tests itself: run it with one of the `--*-selftest` flags
(`--emod-selftest`, `--touch-selftest`, and so on) and CI runs the lot on every platform. Linux, Windows and
iOS builds only exist in CI, so for those the workflow is your test run.

## Credits

- **Ari Ronen / Kingly Games**, who made Eden: World Builder and open-sourced it.
  [Eden on the App Store](https://apps.apple.com/us/app/eden-world-builder/id405743220).
- [BarrelDevelopment/EdenWorldBuilder](https://github.com/BarrelDevelopment/EdenWorldBuilder), the
  fork of the 2018 source release this one started from.
- Third-party code is listed in [`docs/third-party.md`](docs/third-party.md).

Emod is a fan project, not affiliated with Kingly Games.
