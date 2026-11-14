// UX.8-3: --hotbar-selftest, the hotbar's model and sidecar (WORKING/hotbar-plan-2026-11-12.md §3.1, §3.2).
// Headless; plays small 64z worlds, so give it a fresh --docs. Mode group "hotbar".
//
//   1. the file's pure halves: serialise -> parse is the identity (block, painted block, tool, mod block,
//      colour, empty; two bars); a short bar is padded; a bedrock slot reads empty
//   2. refusals: not JSON, wrong root, no/zero/future "v" (future flagged), oversize, colour/id/sel out of
//      range, ten slots, nine bars, a bad or doubled mod name, {}
//   3. a new world: the stock nine, the selection is the slot that matches the hand
//   4. select: block -> blocktype + MODE_BUILD + colour 0; painted -> block_paintcolor; colour -> MODE_PAINT +
//      paintColor; empty / out of range refused; scroll skips empty slots and wraps
//   5. smart replace: an item the bar holds selects its slot; a new item fills the first empty slot; with a
//      full bar it replaces the selected slot; painted and unpainted are different items
//   6. picker target: the pick lands there and swaps with the slot that held it; a new item replaces it
//   7. the Hud's own sites: a block-picker tap, a colour-picker tap and paint -> build all reach the bar
//   8. sidecar round trip: save, leave, re-enter: same slots, same selection, the hand restored
//   9. hostile files: corrupt, oversize, future "v" -> the default bar; the future file is left alone
//  10. mod slot by name: present -> the pending sentinel + the selection name; absent (pack off) -> kept in the
//      slot, refused with a toast, skipped by scroll
#include "harness.h"
#include "eden_fs.h"
#include "eden_prefs.h"
#include "../../../Classes/Hotbar.h"
#include "../../../Classes/Hud.h"
#include "../../../Classes/ContentPack.h"

using namespace harness;

namespace harness {
namespace g_hotbar {
namespace {

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}
std::string readFile(const std::string& path) {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}
bool parses(const std::string& t, bool* future = nullptr) {
    Hotbar::State st;
    return Hotbar::parse(t.data(), t.size(), &st, future);
}
bool sameBars(const Hotbar::State& a, const Hotbar::State& b) {
    if (a.sel != b.sel || a.bars.size() != b.bars.size()) return false;
    for (size_t k = 0; k < a.bars.size(); k++)
        for (int i = 0; i < Hotbar::SLOTS; i++)
            if (!(a.bars[k].s[i] == b.bars[k].s[i])) return false;
    return true;
}
// "B3 B5/12 C31 M:alpha:cube -" for the live bar.
std::string show() {
    std::string o;
    for (int i = 0; i < Hotbar::SLOTS; i++) {
        const HotbarSlot& s = Hotbar::get().slot(i);
        char b[96];
        if (s.kind == HOTBAR_EMPTY) std::snprintf(b, sizeof b, "-");
        else if (s.kind == HOTBAR_COLOUR) std::snprintf(b, sizeof b, "C%d", s.colour);
        else if (!s.mod.empty()) std::snprintf(b, sizeof b, "M:%s/%d", s.mod.c_str(), s.colour);
        else std::snprintf(b, sizeof b, "B%d/%d", s.type, s.colour);
        o += (i ? " " : "") + std::string(b);
    }
    return o + " sel " + std::to_string(Hotbar::get().selected());
}
// The slots alone ("B3/0 C31 -"), mod slots blanked when asked: what a world with no file of its own starts from.
std::string slots(bool dropMods) {
    std::string o;
    for (int i = 0; i < Hotbar::SLOTS; i++) {
        const HotbarSlot& s = Hotbar::get().slot(i);
        char b[96];
        if (s.kind == HOTBAR_EMPTY || (dropMods && !s.mod.empty())) std::snprintf(b, sizeof b, "-");
        else if (s.kind == HOTBAR_COLOUR) std::snprintf(b, sizeof b, "C%d", s.colour);
        else if (!s.mod.empty()) std::snprintf(b, sizeof b, "M:%s/%d", s.mod.c_str(), s.colour);
        else std::snprintf(b, sizeof b, "B%d/%d", s.type, s.colour);
        o += (i ? " " : "") + std::string(b);
    }
    return o;
}
bool isStockNine() {
    const Hotbar::State d = Hotbar::defaultState();
    for (int i = 0; i < Hotbar::SLOTS; i++)
        if (!(Hotbar::get().slot(i) == d.bars[0].s[i])) return false;
    return true;
}
// Put a block in the hand the way a pick does, and tell the bar.
void hand(int type, int colour) {
    Hud* hud = World::getWorld->hud;
    hud->blocktype = type;
    hud->block_paintcolor = colour;
    hud->mode = MODE_BUILD;
    hud->noteHand();
}
std::string worldDir() {
    std::string f = eden_debug_world_file();
    const size_t slash = f.rfind('/');
    return slash == std::string::npos ? std::string() : f.substr(0, slash);
}
std::string toast() {
    const std::string* t = World::getWorld->hud->sb->current();
    return t ? *t : std::string();
}

}  // namespace

int run_hotbar_selftest() {
    g_selftestTag = "eden-hotbar";
    char d[600];
    Hotbar& hb = Hotbar::get();

    // ---- 1. serialise / parse
    {
        Hotbar::State st;
        st.sel = 3;
        st.bars.resize(2);
        st.bars[0].s[0] = HotbarSlot::block(TYPE_STONE);
        st.bars[0].s[1] = HotbarSlot::block(TYPE_WOOD, 12);
        st.bars[0].s[2] = HotbarSlot::modBlock("wonders:marble");
        st.bars[0].s[3] = HotbarSlot::paint(31);
        st.bars[0].s[5] = HotbarSlot::block(HUD_TOOL_SIGN, 7);
        st.bars[0].s[8] = HotbarSlot::modBlock("wonders:tile", 54);
        st.bars[1].s[4] = HotbarSlot::block(TYPE_DOOR_TOP, 20);
        const std::string text = Hotbar::serialise(st);
        Hotbar::State back;
        const bool ok = Hotbar::parse(text.data(), text.size(), &back);
        std::snprintf(d, sizeof d, "%s", text.c_str());
        check(ok && sameBars(st, back) && Hotbar::serialise(back) == text, "serialise -> parse -> serialise is the identity", d);

        Hotbar::State sh;
        const std::string shortBar = "{\"v\":1,\"later\":[1,2],\"bars\":[[{\"id\":2},{\"id\":1},{\"c\":5,\"x\":true}]]}";
        const bool ok2 = Hotbar::parse(shortBar.data(), shortBar.size(), &sh);
        check(ok2 && sh.sel == -1 && sh.bars.size() == 1 && sh.bars[0].s[0] == HotbarSlot::block(2) &&
                  sh.bars[0].s[1].kind == HOTBAR_EMPTY && sh.bars[0].s[2] == HotbarSlot::paint(5) && sh.bars[0].s[8].kind == HOTBAR_EMPTY,
              "a short bar is padded, unknown keys are ignored, a bedrock slot reads empty", shortBar.c_str());
    }
    // ---- 2. refusals
    {
        static const char* const kBad[] = {
            "", "garbage", "[]", "{}", "{\"bars\":[[]]}", "{\"v\":0,\"bars\":[[]]}", "{\"v\":\"1\",\"bars\":[[]]}", "{\"v\":1}",
            "{\"v\":1,\"bars\":[]}", "{\"v\":1,\"bars\":{}}", "{\"v\":1,\"bars\":[{}]}", "{\"v\":1,\"sel\":9,\"bars\":[[]]}",
            "{\"v\":1,\"sel\":-2,\"bars\":[[]]}", "{\"v\":1,\"sel\":1.5,\"bars\":[[]]}", "{\"v\":1,\"bars\":[[{\"id\":128}]]}",
            "{\"v\":1,\"bars\":[[{\"id\":0}]]}", "{\"v\":1,\"bars\":[[{\"id\":999}]]}", "{\"v\":1,\"bars\":[[{\"id\":1002}]]}",
            "{\"v\":1,\"bars\":[[{\"id\":3,\"c\":55}]]}", "{\"v\":1,\"bars\":[[{\"c\":0}]]}", "{\"v\":1,\"bars\":[[{\"c\":-1}]]}",
            "{\"v\":1,\"bars\":[[{}]]}", "{\"v\":1,\"bars\":[[7]]}", "{\"v\":1,\"bars\":[[{\"mod\":\"nocolon\"}]]}",
            "{\"v\":1,\"bars\":[[{\"mod\":\"a:b\",\"id\":3}]]}", "{\"v\":1,\"bars\":[[{\"mod\":7}]]}", "{\"v\":1,\"bars\":[[{\"mod\":\"a: b\"}]]}",
            "{\"v\":1,\"bars\":[[null,null,null,null,null,null,null,null,null,null]]}",
            "{\"v\":1,\"bars\":[[],[],[],[],[],[],[],[],[]]}",
        };
        std::string passed;
        for (const char* t : kBad)
            if (parses(t)) passed += std::string(" <") + t + ">";
        const std::string longName = "{\"v\":1,\"bars\":[[{\"mod\":\"a:" + std::string(64, 'x') + "\"}]]}";
        if (parses(longName)) passed += " <long mod name>";
        const std::string big = "{\"v\":1,\"bars\":[[]]}" + std::string(Hotbar::kMaxBytes, ' ');
        if (parses(big)) passed += " <oversize>";
        bool future = false, futureOther = true;
        const bool f1 = parses("{\"v\":2,\"sel\":0,\"bars\":\"new shape\"}", &future);
        parses("garbage", &futureOther);
        if (f1 || !future || futureOther) passed += " <future v>";
        if (!parses("{\"v\":1,\"bars\":[[]]}")) passed += " <the minimal file was refused>";
        check(passed.empty(), "every malformed, out-of-range, oversize or newer file is refused whole", passed.c_str());
    }

    eden_prefs_set_str(Hotbar::prefsKey(), "");   // a mode run's prefs are in memory only; start with no last bar
    const std::string docs = eden_platform_documents_root();
    const std::string packs = docs + "/Packs";
    eden_fs_make_dirs(packs + "/alpha");
    writeFile(packs + "/alpha/pack.json",
              "{\"format\": 1, \"id\": \"alpha\", \"version\": \"1.0\", \"blocks\": [{\"id\": \"cube\", \"faces\": {\"all\": \"eden:stone\"},"
              " \"color\": [200, 200, 200], \"fallback\": \"eden:stone\"}]}\n");

    if (!open_world("hotbar-a", 64)) {
        check(false, "open the first world", "");
        return 1;
    }
    tick(10);
    Hud* hud = World::getWorld->hud;
    const std::string dirA = worldDir();
    const std::string fileA = dirA + "/" + Hotbar::fileName();

    // ---- 3. a new world
    {
        const HotbarSlot h = hud->handSlot();
        std::snprintf(d, sizeof d, "%s; hand type %d", show().c_str(), hud->blocktype);
        check(isStockNine() && hb.selected() >= 0 && hb.slot(hb.selected()) == h && hud->blocktype == TYPE_BRICK && !eden_fs_exists(fileA),
              "a new world has the stock nine, the hand's slot selected, the hand untouched and no file yet", d);
    }
    // ---- 4. select / scroll
    {
        hud->mode = MODE_MINE;
        hud->block_paintcolor = 9;
        const bool a = hb.select(1);
        const bool blockOk = a && hud->blocktype == TYPE_DIRT && hud->mode == MODE_BUILD && hud->block_paintcolor == 0 && hb.selected() == 1;
        hb.setSlot(2, HotbarSlot::block(TYPE_WOOD, 12));
        const bool b = hb.select(2);
        const bool paintedOk = b && hud->blocktype == TYPE_WOOD && hud->block_paintcolor == 12 && hud->mode == MODE_BUILD;
        hb.setSlot(3, HotbarSlot::paint(31));
        const bool c = hb.select(3);
        const bool colourOk = c && hud->mode == MODE_PAINT && hud->paintColor == 31 && hud->blocktype == TYPE_WOOD && hb.selected() == 3;
        hb.setSlot(4, HotbarSlot());
        hb.setSlot(5, HotbarSlot());
        const bool refused = !hb.select(4) && !hb.select(-1) && !hb.select(9) && hb.selected() == 3 && hud->mode == MODE_PAINT;
        hb.scroll(1);
        const int s1 = hb.selected();   // 4 and 5 are empty: 6
        hb.select(8);
        hb.scroll(1);
        const int s2 = hb.selected();   // wraps to 0
        hb.scroll(-1);
        const int s3 = hb.selected();   // back to 8
        hb.select(6);
        hb.scroll(-1);
        const int s4 = hb.selected();   // 3, the colour slot
        std::snprintf(d, sizeof d, "block %d painted %d colour %d refused %d scroll %d %d %d %d mode %d; %s", blockOk, paintedOk, colourOk,
                      refused, s1, s2, s3, s4, hud->mode, show().c_str());
        check(blockOk && paintedOk && colourOk, "a block slot sets the type and MODE_BUILD, a painted one block_paintcolor, a colour one MODE_PAINT", d);
        check(refused && s1 == 6 && s2 == 0 && s3 == 8 && s4 == 3 && hud->mode == MODE_PAINT,
              "an empty or out-of-range slot is refused; scroll skips empty slots and wraps", d);
    }
    // ---- 5. smart replace (bar: stone dirt wood/12 C31 - - sand glass lightbox)
    {
        hand(TYPE_SAND, 0);
        const bool jump = hb.selected() == 6 && hb.slot(4).kind == HOTBAR_EMPTY;
        hand(TYPE_WOOD, 0);   // unpainted wood is not the painted wood of slot 2
        const bool fill = hb.selected() == 4 && hb.slot(4) == HotbarSlot::block(TYPE_WOOD) && hb.slot(2) == HotbarSlot::block(TYPE_WOOD, 12);
        hand(TYPE_BRICK, 0);
        const bool fill2 = hb.selected() == 5 && hb.slot(5) == HotbarSlot::block(TYPE_BRICK);
        hb.select(7);
        hand(TYPE_TNT, 0);    // full bar: the selected slot
        const bool replace = hb.selected() == 7 && hb.slot(7) == HotbarSlot::block(TYPE_TNT);
        hand(TYPE_WOOD, 12);
        const bool painted = hb.selected() == 2;
        std::snprintf(d, sizeof d, "jump %d fill %d %d replace %d painted %d; %s", jump, fill, fill2, replace, painted, show().c_str());
        check(jump && fill && fill2 && replace && painted,
              "a held item selects its slot, a new one fills the first empty slot, then replaces the selected one", d);
    }
    // ---- 6. picker target (bar: stone dirt wood/12 C31 wood brick sand tnt lightbox)
    {
        hb.setPickerTarget(0);
        hand(TYPE_SAND, 0);
        const bool swap = hb.selected() == 0 && hb.slot(0) == HotbarSlot::block(TYPE_SAND) && hb.slot(6) == HotbarSlot::block(TYPE_STONE) &&
                          hb.pickerTarget() == -1;
        hb.setPickerTarget(8);
        hand(TYPE_GLASS, 0);
        const bool land = hb.selected() == 8 && hb.slot(8) == HotbarSlot::block(TYPE_GLASS);
        hb.setPickerTarget(1);
        hb.select(3);
        const bool cleared = hb.pickerTarget() == -1;
        std::snprintf(d, sizeof d, "swap %d land %d cleared %d; %s", swap, land, cleared, show().c_str());
        check(swap && land && cleared, "a targeted pick lands in the target, swapping with the slot that held it; a select drops the target", d);
    }
    // ---- 7. the Hud's own sites (bar: sand dirt wood/12 C31 wood brick stone tnt glass)
    {
        hb.select(1);
        tap_hud(1);
        tick(5);
        const bool open = hud->mode == MODE_PICK_BLOCK;
        ui_tap_rect(hud->blockBounds[2]);   // dark stone in the stock table: not in the bar
        tick(5);
        const int picked = hud->blocktype;
        const bool blockPick = open && hud->mode == MODE_BUILD && hb.selected() == 1 && hb.slot(1) == HotbarSlot::block(picked) && picked != TYPE_DIRT;
        tap_hud(2);
        tick(5);
        const bool copen = hud->mode == MODE_PICK_COLOR;
        ui_tap_rect(hud->colorBounds[30]);  // colour 31: slot 3 holds it
        tick(5);
        const bool colourJump = copen && hud->mode == MODE_PAINT && hud->paintColor == 31 && hb.selected() == 3;
        tap_hud(2);
        tick(5);
        ui_tap_rect(hud->colorBounds[4]);   // colour 5: new, the bar is full, so it replaces the selected slot 3
        tick(5);
        const bool colourNew = hud->paintColor == 5 && hb.selected() == 3 && hb.slot(3) == HotbarSlot::paint(5);
        tap_hud(1);                         // paint -> build: the current block, painted
        tick(5);
        const bool combine = hud->mode == MODE_BUILD && hud->block_paintcolor == 5 && hb.selected() == 3 && hb.slot(3) == HotbarSlot::block(picked, 5);
        std::snprintf(d, sizeof d, "block pick %d (type %d) colour jump %d new %d combine %d mode %d; %s", blockPick, picked, colourJump,
                      colourNew, combine, hud->mode, show().c_str());
        check(blockPick && colourJump && colourNew && combine,
              "a block-picker tap, a colour-picker tap and paint -> build each reach the bar", d);
    }
    // ---- 8. sidecar round trip
    Hotbar::State saved;
    std::string savedText;
    {
        hb.setSlot(8, HotbarSlot::modBlock("alpha:cube"));
        hb.select(2);   // wood/12
        const bool wasDirty = hb.dirty();
        save_world();
        tick(5);
        savedText = readFile(fileA);
        const bool parsed = Hotbar::parse(savedText.data(), savedText.size(), &saved);
        const std::string before = show();
        const bool wrote = wasDirty && !hb.dirty() && parsed && saved.sel == 2 && !eden_fs_exists(fileA + ".tmp");
        bool ok = quit_to_menu();
        hud->blocktype = TYPE_BRICK;
        hud->block_paintcolor = 0;
        hud->mode = MODE_MINE;
        ok = ok && open_world("hotbar-a", 64);
        tick(10);
        std::snprintf(d, sizeof d, "wrote %d reopened %d; before [%s] after [%s]; hand %d/%d mode %d; file %s", wrote, ok, before.c_str(),
                      show().c_str(), hud->blocktype, hud->block_paintcolor, hud->mode, savedText.c_str());
        check(wrote && ok && show() == before && hud->blocktype == TYPE_WOOD && hud->block_paintcolor == 12 && hud->mode == MODE_MINE,
              "save, leave, re-enter: the same slots and selection, the selected item back in the hand, the mode kept", d);
    }
    // ---- 9. hostile files
    {
        struct Case { const char* what; std::string text; bool future; };
        const Case cases[] = {
            {"corrupt", "{\"v\":1,\"bars\":[[{\"id\":3,", false},
            {"oversize", savedText + std::string(Hotbar::kMaxBytes, ' '), false},
            {"out of range", "{\"v\":1,\"sel\":2,\"bars\":[[{\"id\":4000}]]}", false},
            {"future v", "{\"v\":7,\"sel\":2,\"bars\":[[{\"id\":3}]],\"new\":true}", true},
        };
        std::string bad;
        for (const Case& c : cases) {
            const std::string expect = slots(true);   // the default: the last bar used, minus its mod slots
            bool ok = quit_to_menu();
            hud->blocktype = TYPE_BRICK;
            hud->block_paintcolor = 0;
            hud->mode = MODE_BUILD;
            ok = ok && writeFile(fileA, c.text) && open_world("hotbar-a", 64);
            tick(10);
            const bool def = ok && slots(false) == expect && hud->blocktype == TYPE_BRICK && hb.selected() == 5;
            hb.select(0);
            hb.setSlot(1, HotbarSlot::paint(9));
            save_world();
            tick(5);
            const std::string after = readFile(fileA);
            const bool kept = after == c.text;
            if (!def) bad += " [" + slots(false) + " sel " + std::to_string(hb.selected()) + " want " + expect + "]";
            if (!def || kept != c.future) bad += std::string(" ") + c.what + (def ? "" : ":not-default") + (kept != c.future ? ":file" : "");
        }
        check(bad.empty(), "a corrupt, oversize or out-of-range file gives the default bar and is replaced by the next save; a newer one is left alone",
              bad.c_str());
        bool ok = quit_to_menu();
        ok = ok && writeFile(fileA, savedText) && open_world("hotbar-a", 64);
        tick(10);
        check(ok, "reopen with the saved bar", "");
    }
    // ---- 10. mod slot by name
    {
        const bool there = hb.slot(8) == HotbarSlot::modBlock("alpha:cube");
        const bool sel = hb.select(8);
        const bool present = there && sel && hud_picker_is_mod(hud->blocktype) && Hud::modSelection() == "alpha:cube" &&
                             Hud::modCellName(hud->blocktype) == "alpha:cube" && hud->mode == MODE_BUILD;
        const bool roundTrip = hud->handSlot() == HotbarSlot::modBlock("alpha:cube");
        hb.setSlot(7, HotbarSlot::modBlock("ghost:cube"));
        const int typeBefore = hud->blocktype;
        const bool ghost = !hb.select(7) && hb.selected() == 8 && hud->blocktype == typeBefore && toast().find("not installed") != std::string::npos;
        std::snprintf(d, sizeof d, "slot %d select %d type %d sel \"%s\" hand round trip %d ghost refused %d toast \"%s\"", there, sel,
                      hud->blocktype, Hud::modSelection().c_str(), roundTrip, ghost, toast().c_str());
        check(present && roundTrip && ghost, "a mod slot whose block is loaded selects the pending mod block by name; an unknown name is refused", d);

        save_world();
        tick(5);
        bool ok = quit_to_menu();
        ContentPack::setPackEnabled(docs, "alpha", false);
        ok = ok && open_world("hotbar-a", 64);
        tick(10);
        const bool keptSlot = hb.slot(8) == HotbarSlot::modBlock("alpha:cube");
        hb.select(6);
        const int t6 = hud->blocktype;
        const bool absent = !hb.select(8) && hb.selected() == 6 && hud->blocktype == t6;
        hb.scroll(1);                       // 7 (ghost) and 8 (alpha, off) are skipped
        const int after = hb.selected();
        std::snprintf(d, sizeof d, "reopened %d kept %d refused %d scroll 6 -> %d; %s", ok, keptSlot, absent, after, show().c_str());
        check(ok && keptSlot && absent && after == 0, "with the pack off the slot keeps its name, cannot be selected and is skipped by scroll", d);
        ContentPack::setPackEnabled(docs, "alpha", true);
    }
    // A second world starts from the last bar without its mod slots (prefs `hotbar.last`).
    {
        hb.select(1);
        save_world();
        tick(5);
        bool ok = quit_to_menu();
        ok = ok && open_world("hotbar-b", 64);
        tick(10);
        bool noMods = true;
        for (int i = 0; i < Hotbar::SLOTS; i++) noMods = noMods && hb.slot(i).mod.empty();
        std::snprintf(d, sizeof d, "opened %d; %s", ok, show().c_str());
        check(ok && noMods && hb.slot(2) == HotbarSlot::block(TYPE_WOOD, 12) && hb.slot(8).kind == HOTBAR_EMPTY && worldDir() != dirA,
              "a new world starts from the last bar used, minus its mod slots", d);
        // UX.8-5: assign (with swap), clear, and the strip's activate rules.
        for (int i = 0; i < Hotbar::SLOTS; i++) hb.setSlot(i, HotbarSlot());
        hb.setSlot(0, HotbarSlot::block(TYPE_STONE));
        hb.setSlot(1, HotbarSlot::block(TYPE_DIRT));
        hb.setSlot(2, HotbarSlot::paint(12));
        hb.select(0);
        hb.assign(4, HotbarSlot::block(TYPE_DIRT));                      // held by slot 2: swaps with slot 5's old content (empty)
        const bool swapped = hb.slot(4) == HotbarSlot::block(TYPE_DIRT) && hb.slot(1).kind == HOTBAR_EMPTY;
        hb.assign(3, HotbarSlot::block(TYPE_SAND));                      // a new item just lands
        const bool landed = hb.slot(3) == HotbarSlot::block(TYPE_SAND);
        hb.clear(0);                                                     // the selected slot: selection goes to -1
        const bool cleared = hb.slot(0).kind == HOTBAR_EMPTY && hb.selected() == -1;
        hud->mode = MODE_BUILD;
        const bool openEmpty = hud->hotbarActivate(5) && hud->mode == MODE_PICK_BLOCK && hb.pickerTarget() == 5;
        const bool retarget = hud->hotbarActivate(3) && hb.pickerTarget() == 3;   // picker open: only retargets
        tick(5);
        hud->mode = MODE_BUILD;
        tick(2);
        const bool dropped = hb.pickerTarget() == -1;                    // closing the picker drops the target
        hb.select(2);                                                    // colour slot, selected and in use: opens the colour picker
        const bool reopen = hud->hotbarActivate(2) && hud->mode == MODE_PICK_COLOR && hb.pickerTarget() == 2;
        hud->mode = MODE_BUILD;
        tick(2);
        std::snprintf(d, sizeof d, "swap %d land %d clear %d openEmpty %d retarget %d dropped %d reopen %d; %s",
                      swapped, landed, cleared, openEmpty, retarget, dropped, reopen, show().c_str());
        check(swapped && landed && cleared && openEmpty && retarget && dropped && reopen,
              "assign swaps with the slot that held the item, clear empties, re-select/empty open a targeted picker", d);
        quit_to_menu();
    }
    eden_fs_remove_tree(packs);
    std::printf("[eden-hotbar] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_hotbar
}  // namespace harness
