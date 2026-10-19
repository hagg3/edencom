# Eden: Community Edition (Emod)
This project is a community-maintained fork of Eden: World Builder. Eden is a voxel sandbox for iPhone and iPad, created by Ari Ronen (Kingly Games) and released in 2010 on the App Store. It was the first mainstream voxel sandbox of its kind for iOS, and predated Minecraft: Pocket Edition for about a year.

# Eden: Community Edition (Emod)
This project is a community-maintained fork of Eden: World Builder. Eden is a voxel sandbox for iPhone and iPad, created by Ari Ronen (Kingly Games) and released in 2010 on the App Store. It was the first mainstream voxel sandbox of its kind for iOS, and predated Minecraft: Pocket Edition for about a year.

Eden: World Builder is a voxel sandbox for iPhone and iPad, made by Ari Ronen (Kingly Games) and released in 2010. A lot of people grew up on it.

Emod is a community fork that gets the 2.1 source code (released in 2018) running cross-platform and on modern devices.

For support, queries and updates, join the official Eden Discord server, which is owned by Ari Ronen and managed by community members.[Discord](http://discord.gg/rjYXwBC).

## Rationale

Despite sometimes being lumped in with the litany of 'Minecraft clones' on the App Store, Eden maintained a position in the top charts on iOS for years, and still sustains an active community today. The game's unique features set it apart from clones, including an in-game 'Shared Worlds' list allowing users to search and browse user-created worlds and upload their own. The game also features unique bouncy physics, ice slides, and touch first controls that make building straightforward on a phone or tablet.

The source code for version 2.1 (released in 2015) was made available in 2018, and it is that code that this game is forked from. The game has received closed source updates since then - namely a new texture pack and 4x greater world height limit in 2021, and the addition of multiplayer as well as new block types; signs which can be painted; command blocks; and a space exploration themed adventure mode in 2026. Whilst we welcome the new additions to the game, especially given that they constitute an attempt at further carving out a niche for the game, we felt motivated to preserve and update a version of the game that is more faithful to the classic sandbox gameplay and art style. 

Community Edition therefore implements a range of bug fixes and quality-of-life changes to the 2015 era code, as well as back-porting multiplayer and the new block types. At present, it currently contains the foundations for community add-on and modding support - i.e. a developer console. We have also made wide ranging improvements to the code, further optimizing what was already quite an efficient codebase in 2.1. 

## Platform support

As well as iOS, the code at present can be compiled and run on the following:

- **macOS, Linux and Windows** (SDL3 + OpenGL)
- **iOS / iPadOS** (unsigned builds you sideload; see Releases)
- Android support is planned.

The original 2.1 engine has been iterated on, rather than remade. There is a light compatibility layer that stands in for iOS/ObjC APIs on non-Apple platforms.


## Playing

Cross-platform beta test builds are posted under [Releases](https://github.com/hagg3/edencom/releases) as unsigned `.ipa` files.

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
