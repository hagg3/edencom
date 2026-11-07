//
//  ContentPack.h
//  Emod — K.2-3 (WORKING/data-driven-content-plan-2026-11-01.md D1/D2/D5, §3, §4): content packs.
//
//  A pack is one directory with one `pack.json` (schema: docs/content-packs.md). The bundled stock pack
//  `eden` (media/packs/eden/, shipped as `<bundle>/packs/eden/`) is the only source of rows 0..127 of every
//  per-type table: blockinfo, blockColor, blockTypeFaces, hudBlocksMap, blockTntMap, blockSoundSet,
//  blockPaintArt, the sound sets, tileTwin/tileParticleArt, the stock names and console aliases
//  (BlockRegistry's stock half) and the two picker pages (Hud). The C initialisers that held them are gone
//  (`pristine-engine` keeps them). main() calls loadStock() once the bundle root is known and before any
//  mode dispatch; until then the tables are zero (rows 128..255 are the missing block's, Globals.cpp).
//  A missing or invalid `eden` pack is fatal (D5): the host shows the error and exits 1.
//
//  Every refusal is one line `<pack>@<byte offset>: <message>`, the same line port/tools/pack.py prints
//  (the parity gate: `pack.py fixtures` and `--pack-selftest` read one fixture set). Mod packs (K.2-4)
//  load at world entry through their own validator (compileMod, below), same line format.
//
#ifndef EDEN_CONTENT_PACK_H
#define EDEN_CONTENT_PACK_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace ContentPack {

// What a stock pack compiles to. Compiled into a Stock first and copied into the live tables only when
// the whole pack validates, so a refused pack never leaves a half-filled table.
struct Stock;

// Parses and validates a stock pack's pack.json bytes. NULL + one line per error on refusal. The caller
// owns the result (freeStock).
Stock* compileStock(const char* data, size_t len, const char* label, std::vector<std::string>* errors);
void freeStock(Stock* s);
// Writes the compiled rows into the live tables (and rebuilds the shadow casters).
void commitStock(const Stock& s);
// True when the compiled rows equal the live tables (--pack-selftest's check after boot).
bool stockMatchesLive(const Stock& s, std::string* firstDiff);

// `<bundleRoot>/packs/eden/pack.json`, else `<bundleRoot>/media/packs/eden/pack.json` (a bundle root that
// is the repo). False + `errors`: the game cannot start.
bool loadStock(const std::string& bundleRoot, std::vector<std::string>* errors);
// The path the last loadStock read (or tried last).
const std::string& stockPath();
// Reads a whole pack.json (refuses one over the 1 MiB cap without reading it all). False: missing/unreadable.
bool readPackFile(const std::string& path, std::string* out);

// ---- mod packs (K.2-4; plan D1, D6, D10-D15; docs/content-packs.md "Mod packs")
//
// A mod pack is any other directory with a pack.json: blocks of kind `cube` with traits, own 32x32 PNG tiles
// (placed in the free atlas tiles: atlas.png 66..127, atlas2.png 32..127 in 8-slot strips) or `eden:` tiles, an `eden:` sound
// set and a placeable stock fallback. Its blocks become BlockRegistry definitions named `<id>:<block id>`.
// Lines are `<folder>@<offset>: <message>`; `warning:` lines (an unknown key) refuse nothing, an error in a
// block skips that block, an error at the top level skips the pack. port/tools/pack.py prints the same lines.
// Every atlas2 tile is an 8-frame strip: Terrain::render2 shifts the transparent pass's V by frame/16 (0..7
// tiles; water and lava animate, glass and weave repeat one image 8 times). So a transparent mod tile takes 8
// slots (its image 8 times): 12 of them fit in 32..127.
enum { ATLAS1_FREE_FIRST = 66, ATLAS2_FREE_FIRST = 32, ATLAS2_FRAMES = 8, TILE_PX = 32 };

struct ModFace { int eden; int local; };   // an eden: tile's atlas index, or (eden -1) a pack tile index
struct ModTileSrc {
    std::string name;
    std::vector<uint8_t> rgba;             // 32x32x4
    int twin;                              // pack tile index of the "unpainted" twin, -1 none
};
struct ModBlockSrc {
    std::string id;
    int info;
    ModFace faces[6];
    uint8_t color[3], soundSet, paintArt, fallbackType, fallbackPaint;
    bool picker;
};
// K.3 (WORKING/mod-manager-plan-2026-11-01.md M3): `"requires": ["id", "id>=1.2"]`, `"conflicts": ["id"]`.
struct ModRequire { std::string id, minVersion; };   // minVersion "" = any
// K.4 (WORKING/mod-scripting-plan-2026-11-03.md S2): `"scripts": ["scripts/main.lua", ...]`, read whole at
// compile time (a script never opens a file afterwards), run by Classes/ModScript in list order.
struct ModScriptSrc { std::string path, source; };
enum { SCRIPTS_MAX = 16, SCRIPT_MAX_BYTES = 256 * 1024, SCRIPTS_MAX_BYTES = 1024 * 1024 };
// Where a folder ended up after the last scan (K.3). Refused = compileMod refused it or its id is taken.
enum PackState { PACK_LOADED, PACK_DISABLED, PACK_REFUSED, PACK_NEEDS, PACK_CONFLICT };
struct ModPack {
    std::string folder, dir, id, name, version;
    std::vector<ModRequire> needs;       // "requires"
    std::vector<ModRequire> conflicts;     // ids only (minVersion always "")
    std::string hash;                      // sha256 hex of pack.json + its tile PNGs (plan M5) + its scripts (K.4)
    PackState state = PACK_REFUSED;
    bool fromExtraDir = false;             // --packs=: always enabled, not in the enable list
    std::string why;                       // PACK_NEEDS / PACK_CONFLICT: the loader line's reason
    size_t blocksInFile = 0;
    std::vector<ModTileSrc> tiles;
    std::vector<ModBlockSrc> blocks;       // the valid ones, in file order
    std::vector<ModScriptSrc> scripts;     // K.4: every listed script (a bad entry refuses the pack)
    std::vector<std::string> lines;        // every error and warning, in order
    bool refused = false;                  // a top-level error: nothing of the pack loads
};
// Validates one mod pack's pack.json (`dir` resolves its PNG paths; `label` heads every line). Needs the
// stock pack loaded (eden: tiles, sound sets and names). False = refused (out->refused).
bool compileMod(const char* data, size_t len, const std::string& dir, const std::string& label, ModPack* out);

// What the last loadMods did, for the console's `packs` verb, --content-dump and K.2-5's picker.
struct LoadedBlock { std::string name; int pack; bool picker, missingTexture; };
struct LoadedTile { int atlas, index; std::string name; };   // name `<pack id>:<tile>`
struct ModReport {
    bool ran = false;                      // loadMods has run (a world was entered)
    bool session = false;                  // a multiplayer session: no packs (D6)
    std::vector<ModPack> packs;            // every folder looked at, in load order (refused ones too)
    std::vector<std::string> lines;        // loader lines (skipped folders, duplicate ids, tile budget)
    std::vector<LoadedBlock> blocks;       // registered definitions, in load order
    std::vector<LoadedTile> tiles;         // allocated atlas tiles, in allocation order
};
// `--packs=DIR` (repeatable): a pack directory, or a directory of packs, loaded before Documents/Packs.
void setExtraPackDirs(const std::vector<std::string>& dirs);
// D6: at world entry, on the main thread, before the world-load pthread. Unregisters the previous load's
// definitions, scans the extra dirs then `<documents>/Packs/` (folders bytewise by name; D15), registers
// each valid block, allocates atlas tiles and re-uploads an atlas whose tiles changed. `session`: loads
// nothing (D6/K.1 D7) and puts the stock atlases back.
void loadMods(const std::string& documents, bool session);
const ModReport& modReport();

// ---- K.3: the manager's model (WORKING/mod-manager-plan-2026-11-01.md)
//
// The enable list `<documents>/Packs/mods.json`: {"format": 1, "packs": [{"id": "x", "enabled": true}, ...]}.
// Order = load order. A pack folder whose id is not listed loads after the listed ones, enabled (a pack
// dropped into Packs/ just works, U3). Entries for packs that are not installed are kept (re-installing
// restores the place and the toggle). Missing or unreadable = empty list. Packs from --packs= load first,
// always enabled, and are never written to the list.
struct ModListEntry { std::string id; bool enabled; };
std::vector<ModListEntry> readModList(const std::string& documents, std::string* problem = NULL);
bool writeModList(const std::string& documents, const std::vector<ModListEntry>& list);   // temp + rename
// Every pack folder (extra dirs, then Packs/ in enable-list order), compiled, de-duplicated and resolved
// (requires / conflicts), with `state` set; nothing registered (tile pixels are kept: <= 0.5 MiB). What loadMods uses, and
// what the manager screen lists. `lines` gets the loader lines.
std::vector<ModPack> scanPacks(const std::string& documents, std::vector<std::string>* lines);
// Rewrite the list from the current scan order with one change. False: no such installed pack / write failed.
bool setPackEnabled(const std::string& documents, const std::string& id, bool enabled);
bool movePack(const std::string& documents, const std::string& id, int delta);   // -1 earlier, +1 later
// Removes `<documents>/Packs/<folder>` (the folder, not the list entry). False: not a pack folder there.
bool removePackFolder(const std::string& documents, const std::string& folder);
// WB (2026-11-07): the packs Emod puts in every player's Packs folder (`kBundledPacks`: the Wonder Blocks
// example). For each id not yet recorded in `<documents>/Packs/.bundled`, copies `<bundle>/packs/<id>` (else
// `<bundle>/media/packs/<id>`) to `Packs/<id>` through `Packs/.seed.partial` and one no-replace rename, then
// records the id, so it happens once per Documents folder and a player who removes the pack keeps it removed.
// An id some Packs folder already holds is recorded without copying. New ids added to the list in a later
// release reach existing players once too. Only interactive runs call it (main.cpp): harness runs leave their
// --docs folders as they found them. Returns the number of packs copied; `lines` gets one line per action.
int seedBundledPacks(const std::string& documents, const std::string& bundleRoot, std::vector<std::string>* lines);
// Version order: dotted numbers, each the leading digits of its part (none = 0), missing parts 0.
int compareVersions(const std::string& a, const std::string& b);

// The mod list of the last loadMods (plan M5): sha256 hex over "<id> <version> <hash>\n" per loaded pack in
// load order; "" when no pack is loaded (vanilla). What a multiplayer server advertises as INFO `mods=`.
std::string modsHash();

// What the current world's palette names (plan M4: the per-world "mods used" record is BLOCK_PALETTE's
// namespaces; nothing new on disk). Call after the world loaded.
enum WorldModState { WMOD_LOADED, WMOD_DISABLED, WMOD_MISSING };
struct WorldMod { std::string id; WorldModState state; int blocks; };   // blocks: palette ids it names
std::vector<WorldMod> worldMods(const std::string& documents);
// The stock pack's tile names (eden: tiles) -> {atlas, index}; empty before loadStock.
bool edenTile(const std::string& name, int* atlas, int* index);

}  // namespace ContentPack

#endif
