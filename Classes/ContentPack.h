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
// K.7-3c (V13 step 2): `"icons": {"wire": "icons/wire.png"}`, 64x64 white-on-transparent PNGs tinted like the built-in
// icons; named `"<pack id>:wire"` in eden.menu. At most ICONS_PER_PACK a pack and ICONS_PER_WORLD a world.
struct ModIconSrc {
    std::string name;
    std::vector<uint8_t> rgba;             // 64x64x4 (freed once the world's icon atlas has them)
};
enum { ICON_PX = 64, ICONS_PER_PACK = 8, ICONS_PER_WORLD = 32 };
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
// K.7-2a (V14): `"settings": [...]`, the pack's settings page. Validated at compile time; the page and the values
// are K.7-2b. `type`: heading | toggle | slider | choice | text. A heading has only a label (id empty).
struct ModSetting {
    std::string id, type, label, hint;
    bool defBool = false;                  // toggle
    double min = 0, max = 0, step = 0, defNum = 0;   // slider
    std::vector<std::string> options;      // choice
    std::string defText;                   // choice (the option) and text
    int maxLen = 0;                        // text: bytes
};
enum { SETTINGS_MAX = 32 };
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
    std::vector<ModIconSrc> icons;         // K.7-3c: the valid ones, in file order
    std::vector<ModSetting> settings;      // K.7-2a: the valid ones, in file order
    std::vector<std::string> lines;        // every error and warning, in order
    bool refused = false;                  // a top-level error: nothing of the pack loads
};
// Validates one mod pack's pack.json (`dir` resolves its PNG paths; `label` heads every line). Needs the
// stock pack loaded (eden: tiles, sound sets and names). False = refused (out->refused).
bool compileMod(const char* data, size_t len, const std::string& dir, const std::string& label, ModPack* out);

// K.7-2b (V15): a pack's setting values live outside the pack, in `<documents>/Packs/.settings/<id>.json`
// ({"format": 1, "values": {"speed": 2, "glow": false}}), so reinstalling the pack keeps them. Written with
// eden_fs_write_atomic; ids the pack no longer declares are kept; a value that fails the pack's current schema
// reads as its default. Values do not enter the pack hash. A heading has no value (its entry is all default).
struct SettingValue { bool b = false; double n = 0; std::string s; };   // toggle: b; slider: n; choice and text: s
extern bool testSettingsInPack;   // mutation (--ui-mutate=settingsinpack): the file lives in the pack's folder
SettingValue settingDefault(const ModSetting& st);
std::string settingsFilePath(const std::string& documents, const std::string& packId);
// One value per `pk.settings` entry, in order.
std::vector<SettingValue> settingValues(const std::string& documents, const ModPack& pk);
// Writes entry `index` (a slider value is snapped to its step, a choice must be an option, text is cut to `max`
// bytes). False: bad index, a heading, or the write failed.
bool saveSetting(const std::string& documents, const ModPack& pk, size_t index, const SettingValue& v);

// What the last loadMods did, for the console's `packs` verb, --content-dump and K.2-5's picker.
struct LoadedBlock { std::string name; int pack; bool picker, missingTexture; };
struct LoadedIcon { std::string name; int pack; };   // name `<pack id>:<icon>`; index in modReport().icons = the cell
struct LoadedTile { int atlas, index; std::string name; };   // name `<pack id>:<tile>`
struct ModReport {
    bool ran = false;                      // loadMods has run (a world was entered)
    bool session = false;                  // a multiplayer session: no packs (D6)
    std::vector<ModPack> packs;            // every folder looked at, in load order (refused ones too)
    std::vector<std::string> lines;        // loader lines (skipped folders, duplicate ids, tile budget)
    std::vector<LoadedBlock> blocks;       // registered definitions, in load order
    std::vector<LoadedTile> tiles;         // allocated atlas tiles, in allocation order
    std::vector<LoadedIcon> icons;         // K.7-3c: the world's pack icons, in load order (cell i of the icon atlas)
};
// `--packs=DIR` (repeatable): a pack directory, or a directory of packs, loaded before Documents/Packs.
void setExtraPackDirs(const std::vector<std::string>& dirs);
// D6: at world entry, on the main thread, before the world-load pthread. Unregisters the previous load's
// definitions, scans the extra dirs then `<documents>/Packs/` (folders bytewise by name; D15), registers
// each valid block, allocates atlas tiles and re-uploads an atlas whose tiles changed. `session`: loads
// nothing (D6/K.1 D7) and puts the stock atlases back.
void loadMods(const std::string& documents, bool session);
const ModReport& modReport();
// K.7-3c: the id (UIICON_PACK_BASE + cell) of the loaded pack icon `<pack>:<name>`, or -1.
int packIconByName(const std::string& name);
// K.8-7 (`scripts reload`): re-reads the script files of the loaded pack `id` from disk with compileMod's caps and
// refusals (unreadable, over 256 KiB, over 1 MiB together, precompiled). False + `lines`: refused, the sources
// read at world entry stay. Not pack.json, tiles or blocks, and not the pack's hash (stale until the next world).
bool rereadScripts(const std::string& id, std::vector<std::string>* lines);

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
