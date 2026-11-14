// UX.8-3: the hotbar's model and its per-world sidecar (WORKING/hotbar-plan-2026-11-12.md §3.1, §3.2).
//
// Nine slots, each empty, a block (stock id, tool sentinel or a mod block by `ns:path`, with the paint
// colour a placed block carries) or a colour alone (paint mode). The hand itself stays in Hud
// (blocktype / block_paintcolor / paintColor / mode): a slot reaches it only through Hud::applyHand,
// and a hand change made elsewhere (pickers, paint->build combine, eyedropper) reaches the bar only
// through noteHand. Nothing here draws (the strip is UX.8-4) and nothing here is in the save format.
//
// Sidecar `Worlds/<id>/hotbar.json`:
//   { "v":1, "sel":3, "bars":[[ {"id":1}, {"id":57,"c":12}, {"mod":"wonders:marble"}, {"c":31}, null, ... ]] }
// Untrusted input: any field out of range, a wrong type, more than kMaxBytes or a higher "v" refuses the
// whole file (the bar falls back to the default; a higher-"v" file is never overwritten). A world with no
// folder (loose key, multiplayer session) keeps its bar in prefs as `hotbar.last`. Main thread only.
#ifndef EDEN_HOTBAR_H
#define EDEN_HOTBAR_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

enum { HOTBAR_EMPTY = 0, HOTBAR_BLOCK = 1, HOTBAR_COLOUR = 2 };

struct HotbarSlot {
    uint8_t kind = HOTBAR_EMPTY;
    int type = 0;            // BLOCK: stock id 1..127 or a tool sentinel (HUD_TOOL_*); 0 for a mod block
    std::string mod;         // BLOCK: "ns:path" when a mod block, else ""
    uint8_t colour = 0;      // BLOCK: block_paintcolor (0 = unpainted); COLOUR: 1..54
    bool operator==(const HotbarSlot& o) const { return kind == o.kind && type == o.type && colour == o.colour && mod == o.mod; }
    static HotbarSlot block(int type, int colour = 0);
    static HotbarSlot modBlock(const std::string& name, int colour = 0);
    static HotbarSlot paint(int colour);
};

class Hotbar {
public:
    enum { SLOTS = 9, MAX_BARS = 8, MAX_COLOUR = 54, MAX_NAME = 64, kMaxBytes = 64 * 1024 };
    struct Bar { HotbarSlot s[SLOTS]; };
    // What the sidecar holds. `sel` -1 = the hand matches no slot.
    struct State { int sel = -1; std::vector<Bar> bars; };

    static Hotbar& get();

    const HotbarSlot& slot(int i) const;      // an empty slot when out of range
    int selected() const { return m_state.sel; }
    // The slot the next picker commit lands in (-1 none). Set by the strip (UX.8-5), cleared by the commit
    // and by any select.
    int pickerTarget() const { return m_target; }
    void setPickerTarget(int i);

    // Puts slot i in the hand (Hud::applyHand). false: out of range, empty, or the item is refused (a mod
    // block this world lacks: applyHand has shown the toast); the selection is then unchanged.
    bool select(int i);
    // The next (dir > 0) / previous slot that can be held, wrapping; empty and unavailable slots are skipped.
    bool scroll(int dir);
    // The hand changed outside the bar. With a picker target: the item lands there (swapping with the slot
    // that already held it). Else smart replace (H2): the slot holding exactly this item is selected, else
    // the first empty slot is filled, else the selected slot (slot 1 when none) is replaced.
    void noteHand(const HotbarSlot& hand);
    void setSlot(int i, const HotbarSlot& s);   // raw store (selftest, load paths)
    // UX.8-5: explicit assign (1-9 over a picker cell): an item already in another slot swaps places with
    // slot i's old content, so the bar never holds the same item twice. Selection follows the item.
    void assign(int i, const HotbarSlot& s);
    // UX.8-5: clear a slot (right-click / long-press); the selection goes to -1 when it was that slot.
    void clear(int i);

    // World entry (FileManager::finishLoadOnMain). `dir` is the world folder, absolute, "" when it has
    // none. A bar read from the world's own file puts its selected slot back in the hand (mode kept);
    // otherwise the selection is the slot that matches the hand, if any.
    void load(const std::string& dir);
    // Writes hotbar.json (tmp + rename) and prefs `hotbar.last` when dirty. Called after a world save and
    // on leaving the world.
    void saveIfDirty();
    bool dirty() const { return m_dirty; }

    // ---- pure halves (the selftest calls these directly)
    static State defaultState();                       // the stock nine, nothing selected
    static std::string serialise(const State& st);
    // false: refused whole (*future set when only because "v" is newer than this build).
    static bool parse(const char* data, size_t len, State* out, bool* future = NULL);
    // The name the strip shows for a slot (UX.8-4): "Stone ramp 1", a mod block's path, "Sign", "Colour 12";
    // a painted block adds " (colour N)". "" for an empty slot.
    static std::string label(const HotbarSlot& s);
    static const char* fileName() { return "hotbar.json"; }
    static const char* prefsKey() { return "hotbar.last"; }

private:
    State m_state = defaultState();
    int m_target = -1;
    bool m_dirty = false;
    bool m_keepFile = false;   // the world's file is from a newer build: never overwrite it
    std::string m_dir;
    int find(const HotbarSlot& s) const;
};

#endif
