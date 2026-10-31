//
//  BlockRegistry.h
//  Emod — K.1-3 (WORKING/block-registry-plan-2026-10-31.md §4): block names and the world's palette.
//
//  Type bytes 0..127 are the stock blocks, fixed forever (rule 4); each has a canonical name
//  `eden:<enum minus TYPE_, lowercased>` and some have console aliases (`lamp`, `oresand`, ...). Bytes
//  128..255 are world-local: the world's BLOCK_PALETTE record (docs/emod-file-format.md) maps each to a
//  `namespace:path` name. A byte is never rewritten on load (D11): rows 128..255 of the per-type tables
//  (blockinfo, blockColor, blockTypeFaces, hudBlocksMap, blockTntMap, the shadow casters) are rebuilt
//  from the palette at every world load, from the definition registered under each name, or the
//  "missing" placeholder rows when there is none (D4/D10/D12: solid, breakable like stone, unpainted,
//  magenta/black). Main thread, or the world-load pthread while it loads (nothing else runs then).
//
//  K.1 registers no definitions in the game: only --registry-selftest's `emod:test_*` pack (D18). K.2
//  fills the registry from data.
//
#ifndef EDEN_BLOCK_REGISTRY_H
#define EDEN_BLOCK_REGISTRY_H

#include <stdint.h>
#include <string>
#include <vector>

namespace BlockRegistry {

enum { FIRST_LOCAL = 128, LOCAL_IDS = 128 };

// ---- stock half
// `eden:stone_ramp1` for 0..127, NULL otherwise.
const char* stockName(int id);
// A lower-case canonical `eden:` name or a console alias (`stone`, `lamp`) -> 0..127; -1 otherwise.
int stockByName(const std::string& lowerName);
// The console aliases in table order (CmdScript::blockNames, completion).
std::vector<std::string> aliasNames();

// ---- definitions (keyed by name)
struct BlockDef {
    std::string name;            // D15 `namespace:path`, not `eden:`
    uint8_t fallbackType;        // the stock block export/upload write instead (K.1-4), 1..127
    uint8_t fallbackPaint;       // 0..54
    int info;                    // blockinfo row
    int faces[6];                // blockTypeFaces row (atlas tiles)
    uint8_t color[3];            // blockColor row
    int hudMap, tntMap;          // hudBlocksMap / blockTntMap rows
};
// A definition with every row copied from stock block `like` (the test pack's shape).
BlockDef defLike(const std::string& name, int like, int fallbackPaint = 0);
// Adds or replaces. False (nothing changed) for a bad name or fallback. Takes effect at the next
// resolve() (every world load).
bool registerDef(const BlockDef& def);
bool unregisterDef(const std::string& name);
void clearDefs();
const BlockDef* findDef(const std::string& name);

// ---- the world's palette (ids 128..255)
// Before any world load and before a multiplayer join: an empty palette, not dirty, not locked,
// rows 128..255 the placeholder's.
void resetWorld();
// A local world's live BLOCK_PALETTE record (NULL/0: none). `flagged`: the file has FileHeader flags
// bit 0. A flagged file whose record is missing or does not parse LOCKS the palette (no allocation:
// an id's owner is unknown); its bytes still draw as placeholders. Resolves the rows. False = locked.
bool loadPalette(const uint8_t* rec, size_t len, bool flagged);
bool locked();
// Locks the palette as loaded (an older version decoded, the newest did not: an id may be taken).
void lock();
int count();
// -1 when the name has no id in this world.
int idOf(const std::string& name);
// NULL when unassigned (or not a local id).
const char* nameOf(int id);
bool assigned(int id);
// Placeholder test: a local byte drawing as the missing block (unassigned, or its def not loaded).
bool missing(int id);
// The lowest free id for a name whose definition is loaded (or its existing id). -1 and `why`:
// no definition ("Unknown block: X"), palette full (D17), locked. Marks the palette dirty. Never
// called directly by game code: acquire() runs the flag compaction first.
int allocate(const std::string& name, std::string* why);
// What the console and command blocks call to place `name`: its id, allocating one when the
// definition is loaded, after FileManager::preparePaletteAllocation (the D13/D14 flag compaction on
// an unflagged world). Refused in a multiplayer session (D7). -1 + `why` on refusal.
int acquire(const std::string& name, std::string* why);
// The BLOCK_PALETTE record's bytes for the current palette.
std::vector<uint8_t> encode();
bool dirty();
void markSaved();
// Rebuilds rows 128..255 of every per-type table (and the shadow casters) from the palette.
void resolve();

// Test hook (--registry-selftest): how many times resolve() ran.
int resolveCount();

}  // namespace BlockRegistry

#endif
