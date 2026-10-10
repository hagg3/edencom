// eden_app_identity.h — WHAT THIS BUILD CALLS ITSELF ON DISK, in one place.
//
// **Emod is not Eden.** This is a community fork of the 2.1.1 source release, not the shipped
// game, and the two must never share a directory: a user who has (or had) the official app is
// entitled to have its files left alone, and a bug in this fork must not be able to touch them.
// The port had it wrong until 2026-09-06 — every desktop target wrote its worlds into an
// "Eden"-named application-data directory, which is exactly the merge this file exists to
// prevent (user report; see the migration in eden_main_native.cpp).
//
// The rule, stated once so the next platform inherits it rather than re-deciding it:
//
//   * **Anything the OS uses to identify the application is "Emod"** — the application-data
//     directory on all three desktop platforms, the iOS bundle identifier and display name, the
//     window title.
//   * **Anything that names the GAME, the ENGINE or the FILE FORMAT stays "Eden"** — `.eden`
//     saves, `Eden.eden`, the `eden_*` C seam, `EdenAppDelegate`, every symbol and every doc.
//     Those are the thing this fork IS a port of, and renaming them would be both a lie and a
//     format break.
//
//   * **A world FILE EXTENSION follows the same rule (Stage S / S.5, 2026-10-10).** `.eden` names the
//     game's format and stays. `.emod` is the fork's own container: the shipped game cannot read
//     it, Files/Finder/Explorer show the extension, and so it is OS-facing and fork-owned —
//     **Emod**. It must never be `.eden`: the shipped app lists every `*.eden`, reads a foreign
//     header's version field as garbage and runs its in-place 1.x rewrite over the file. Symbols
//     still say Eden (`EdenWorldStore`, `eden_*`), and the bundled map in the new container is
//     `Eden.emod` — Eden's map in Emod's container. The constant is `EMOD_WORLD_EXTENSION` in
//     Classes/Constants.h, where the engine can see it.
//
// So: `~/Library/Application Support/Emod/`, `%APPDATA%\Emod\`, `~/.local/share/emod/`, holding
// `prefs` and `Documents/<world>.eden` / `<world>.emod`. On iOS the container is per-bundle-identifier and the
// question does not arise inside it, but the identifier itself still has to be distinct — see
// native/ios/Info.plist.in.
#ifndef EDEN_APP_IDENTITY_H
#define EDEN_APP_IDENTITY_H

// Capitalised for the platforms whose application-data roots are capitalised by convention
// (macOS, Windows), lower-case for XDG, which is not.
#define EDEN_APP_DIR_NAME  "Emod"
#define EDEN_APP_DIR_LOWER "emod"

// The window title. Both halves on purpose: "Emod" is what this build is, "Eden: World Builder"
// is what it plays.
#define EDEN_APP_WINDOW_TITLE "Emod — Eden: World Builder"

// The directory names this fork used before 2026-09-06, migrated out of once at startup. Keep
// them here rather than in the migration: the migration is the code, this is the fact.
#define EDEN_APP_DIR_NAME_LEGACY  "Eden"
#define EDEN_APP_DIR_LOWER_LEGACY "eden"

#endif  // EDEN_APP_IDENTITY_H
