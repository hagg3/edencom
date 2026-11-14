// UX.8-3: see Hotbar.h.
#include "Hotbar.h"
#include "Hud.h"
#include "World.h"
#include "Json.h"
#include "Resources.h"
#include "BlockRegistry.h"
#include "Constants.h"
#include "EdenWorldStore.h"                            // emod::io::fopen_utf8
#include "port/src/shim/foundation/eden_fs.h"
#include "port/src/shim/foundation/eden_prefs.h"
#include <cstdio>
#include <cstring>
#include <utility>

HotbarSlot HotbarSlot::block(int type, int colour) {
    HotbarSlot s;
    s.kind = HOTBAR_BLOCK;
    s.type = type;
    s.colour = (uint8_t)colour;
    return s;
}
HotbarSlot HotbarSlot::modBlock(const std::string& name, int colour) {
    HotbarSlot s;
    s.kind = HOTBAR_BLOCK;
    s.mod = name;
    s.colour = (uint8_t)colour;
    return s;
}
HotbarSlot HotbarSlot::paint(int colour) {
    HotbarSlot s;
    s.kind = HOTBAR_COLOUR;
    s.colour = (uint8_t)colour;
    return s;
}

Hotbar& Hotbar::get() {
    static Hotbar h;
    return h;
}

Hotbar::State Hotbar::defaultState() {
    static const int kStock[SLOTS] = {TYPE_STONE, TYPE_DIRT, TYPE_GRASS, TYPE_WOOD, TYPE_BRICK,
                                      TYPE_COBBLESTONE, TYPE_SAND, TYPE_GLASS, TYPE_LIGHTBOX};
    State st;
    st.bars.resize(1);
    for (int i = 0; i < SLOTS; i++) st.bars[0].s[i] = HotbarSlot::block(kStock[i]);
    return st;
}

// ---- the file ---------------------------------------------------------------------------------------

static Json::Value jnum(int n) {
    Json::Value v;
    v.type = Json::Number;
    v.num = n;
    return v;
}

std::string Hotbar::serialise(const State& st) {
    Json::Value root;
    root.type = Json::Object;
    root.obj.push_back(Json::Member("v", jnum(1)));
    root.obj.push_back(Json::Member("sel", jnum(st.sel)));
    Json::Value bars;
    bars.type = Json::Array;
    for (const Bar& b : st.bars) {
        Json::Value jb;
        jb.type = Json::Array;
        for (int i = 0; i < SLOTS; i++) {
            const HotbarSlot& s = b.s[i];
            Json::Value js;   // Null = empty
            if (s.kind != HOTBAR_EMPTY) {
                js.type = Json::Object;
                if (s.kind == HOTBAR_BLOCK) {
                    if (!s.mod.empty()) {
                        Json::Value name;
                        name.type = Json::String;
                        name.str = s.mod;
                        js.obj.push_back(Json::Member("mod", name));
                    } else {
                        js.obj.push_back(Json::Member("id", jnum(s.type)));
                    }
                }
                if (s.colour) js.obj.push_back(Json::Member("c", jnum(s.colour)));
            }
            jb.arr.push_back(js);
        }
        bars.arr.push_back(jb);
    }
    root.obj.push_back(Json::Member("bars", bars));
    return Json::write(root);
}

static bool validModName(const std::string& n) {
    if (n.size() < 3 || n.size() > (size_t)Hotbar::MAX_NAME) return false;
    const size_t colon = n.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == n.size()) return false;
    for (unsigned char c : n)
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    return true;
}

static bool parseSlot(const Json::Value& v, HotbarSlot* out) {
    *out = HotbarSlot();
    if (v.isNull()) return true;
    if (!v.isObject()) return false;
    const Json::Value* mod = v.get("mod");
    const Json::Value* id = v.get("id");
    const Json::Value* c = v.get("c");
    int64_t colour = 0;
    if (c && !c->asInt(&colour, 0, Hotbar::MAX_COLOUR)) return false;
    if (mod) {
        if (id || !mod->isString() || !validModName(mod->str)) return false;
        *out = HotbarSlot::modBlock(mod->str, (int)colour);
        return true;
    }
    if (id) {
        int64_t t = 0;
        if (!id->asInt(&t, 1, HUD_TOOL_CMD)) return false;
        if (t > STOCK_BLOCK_MAX && t != HUD_TOOL_SIGN && t != HUD_TOOL_CMD) return false;
        if (t != TYPE_BEDROCK) *out = HotbarSlot::block((int)t, (int)colour);   // FT.1: never held; the slot reads empty
        return true;
    }
    if (colour < 1) return false;   // {} or a colour slot with no colour
    *out = HotbarSlot::paint((int)colour);
    return true;
}

bool Hotbar::parse(const char* data, size_t len, State* out, bool* future) {
    if (future) *future = false;
    if (!data || len == 0 || len > (size_t)kMaxBytes) return false;
    Json::Value root;
    Json::Error err;
    if (!Json::parse(data, len, &root, &err) || !root.isObject()) return false;
    const Json::Value* v = root.get("v");
    int64_t n = 0;
    if (!v || !v->asInt(&n, 1, INT32_MAX)) return false;
    if (n > 1) {
        if (future) *future = true;
        return false;
    }
    State st;
    if (const Json::Value* sel = root.get("sel")) {
        if (!sel->asInt(&n, -1, SLOTS - 1)) return false;
        st.sel = (int)n;
    }
    const Json::Value* bars = root.get("bars");
    if (!bars || !bars->isArray() || bars->arr.empty() || bars->arr.size() > (size_t)MAX_BARS) return false;
    for (const Json::Value& jb : bars->arr) {
        if (!jb.isArray() || jb.arr.size() > (size_t)SLOTS) return false;   // a short bar is padded with empties
        Bar b;
        for (size_t i = 0; i < jb.arr.size(); i++)
            if (!parseSlot(jb.arr[i], &b.s[i])) return false;
        st.bars.push_back(b);
    }
    *out = st;
    return true;
}

// The whole file when it is at most kMaxBytes; false when missing, unreadable or larger.
static bool readSmall(const std::string& path, std::string* text) {
    FILE* f = emod::io::fopen_utf8(path.c_str(), "rb");
    if (!f) return false;
    text->resize((size_t)Hotbar::kMaxBytes + 1);
    const size_t n = fread(&(*text)[0], 1, text->size(), f);
    fclose(f);
    text->resize(n);
    return n > 0 && n <= (size_t)Hotbar::kMaxBytes;
}

void Hotbar::load(const std::string& dir) {
    m_dir = dir;
    m_target = -1;
    m_dirty = false;
    m_keepFile = false;
    m_state = defaultState();
    Hud* hud = World::getWorld ? World::getWorld->hud : NULL;
    bool own = false;   // this world's own bar (its file, or the prefs copy of a world with no folder)
    std::string text;
    State st;
    if (!dir.empty()) {
        const std::string path = dir + "/" + fileName();
        if (eden_fs_exists(path)) {
            bool future = false;
            if (readSmall(path, &text) && parse(text.data(), text.size(), &st, &future)) {
                m_state = st;
                own = true;
            } else {
                m_keepFile = future;
                fprintf(stderr, "[hotbar] %s: %s; using the default bar\n", path.c_str(),
                        future ? "written by a newer Emod" : "unreadable");
            }
        }
    }
    if (!own) {
        const char* last = eden_prefs_get_str(prefsKey());
        if (last && *last && parse(last, strlen(last), &st)) {
            if (!dir.empty()) {   // a folder world starts from the last bar without its mod slots (they are per world)
                st.bars.resize(1);
                for (int i = 0; i < SLOTS; i++)
                    if (!st.bars[0].s[i].mod.empty()) st.bars[0].s[i] = HotbarSlot();
            }
            m_state = st;
        }
        m_state.sel = -1;
    }
    if (!hud) return;
    if (own && m_state.sel >= 0 && hud->applyHand(slot(m_state.sel), true)) return;
    m_state.sel = find(hud->handSlot());
}

void Hotbar::saveIfDirty() {
    if (!m_dirty) return;
    const std::string text = serialise(m_state);
    eden_prefs_set_str(prefsKey(), text.c_str());
    if (m_dir.empty() || m_keepFile) {
        m_dirty = false;
        return;
    }
    if (!eden_fs_is_dir(m_dir)) return;   // before the world's first save: the next one writes it
    if (eden_fs_write_atomic(m_dir + "/" + fileName(), text.data(), text.size())) m_dirty = false;   // QA.17
}

// ---- the model --------------------------------------------------------------------------------------

const HotbarSlot& Hotbar::slot(int i) const {
    static const HotbarSlot kEmpty;
    return (i >= 0 && i < SLOTS) ? m_state.bars[0].s[i] : kEmpty;
}

int Hotbar::find(const HotbarSlot& s) const {
    if (s.kind == HOTBAR_EMPTY) return -1;
    for (int i = 0; i < SLOTS; i++)
        if (m_state.bars[0].s[i] == s) return i;
    return -1;
}

void Hotbar::setPickerTarget(int i) { m_target = (i >= 0 && i < SLOTS) ? i : -1; }

void Hotbar::setSlot(int i, const HotbarSlot& s) {
    if (i < 0 || i >= SLOTS || m_state.bars[0].s[i] == s) return;
    m_state.bars[0].s[i] = s;
    m_dirty = true;
}

void Hotbar::assign(int i, const HotbarSlot& s) {
    if (i < 0 || i >= SLOTS || s.kind == HOTBAR_EMPTY) return;
    HotbarSlot* b = m_state.bars[0].s;
    const int have = find(s);
    if (have == i) return;
    if (have >= 0) {
        std::swap(b[have], b[i]);
        if (m_state.sel == have) m_state.sel = i;
        else if (m_state.sel == i) m_state.sel = have;
    } else {
        b[i] = s;
    }
    m_dirty = true;
}

void Hotbar::clear(int i) {
    if (i < 0 || i >= SLOTS || m_state.bars[0].s[i].kind == HOTBAR_EMPTY) return;
    m_state.bars[0].s[i] = HotbarSlot();
    if (m_state.sel == i) m_state.sel = -1;
    if (m_target == i) m_target = -1;
    m_dirty = true;
}

bool Hotbar::select(int i) {
    m_target = -1;
    if (i < 0 || i >= SLOTS || !World::getWorld || !World::getWorld->hud) return false;
    const HotbarSlot& s = m_state.bars[0].s[i];
    if (s.kind == HOTBAR_EMPTY) return false;
    if (!World::getWorld->hud->applyHand(s)) return false;
    if (m_state.sel != i) {
        m_state.sel = i;
        m_dirty = true;
    }
    Resources::getResources->playSound(S_MENU_BUTTON_PRESS);   // UX.8-5: one sound per select, any route
    return true;
}

bool Hotbar::scroll(int dir) {
    if (!World::getWorld || !World::getWorld->hud) return false;
    const int step = dir > 0 ? 1 : -1;
    int i = m_state.sel >= 0 ? m_state.sel : (step > 0 ? -1 : SLOTS);
    for (int n = 0; n < SLOTS; n++) {
        i = ((i + step) % SLOTS + SLOTS) % SLOTS;
        if (World::getWorld->hud->handAvailable(m_state.bars[0].s[i])) return select(i);
    }
    return false;
}

void Hotbar::noteHand(const HotbarSlot& hand) {
    if (hand.kind == HOTBAR_EMPTY) return;
    HotbarSlot* b = m_state.bars[0].s;
    const int have = find(hand);
    int to;
    if (m_target >= 0) {
        to = m_target;
        m_target = -1;
        if (have == to) {
            if (m_state.sel == to) return;
        } else if (have >= 0) {
            std::swap(b[have], b[to]);
        } else {
            b[to] = hand;
        }
    } else if (have >= 0) {
        if (m_state.sel == have) return;
        to = have;
    } else {
        to = -1;
        for (int i = 0; i < SLOTS && to < 0; i++)
            if (b[i].kind == HOTBAR_EMPTY) to = i;
        if (to < 0) to = m_state.sel >= 0 ? m_state.sel : 0;
        b[to] = hand;
    }
    m_state.sel = to;
    m_dirty = true;
}

// ---- names ------------------------------------------------------------------------------------------

// "eden:stone_ramp1" / "wonders:marble_slab" -> "Stone ramp 1" / "Marble slab" (the namespace is shown only
// when `keepNs`, for a mod block whose path collides with a stock one).
static std::string prettyName(const std::string& full, bool keepNs) {
    std::string path = full;
    const size_t colon = full.find(':');
    std::string ns;
    if (colon != std::string::npos) {
        ns = full.substr(0, colon);
        path = full.substr(colon + 1);
    }
    std::string out;
    for (size_t i = 0; i < path.size(); i++) {
        char c = path[i];
        if (c == '_') c = ' ';
        if (i > 0 && c >= '0' && c <= '9' && !(path[i - 1] >= '0' && path[i - 1] <= '9') && path[i - 1] != '_') out += ' ';
        out += c;
    }
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return keepNs && !ns.empty() ? ns + ":" + out : out;
}

std::string Hotbar::label(const HotbarSlot& s) {
    if (s.kind == HOTBAR_COLOUR) return "Colour " + std::to_string((int)s.colour);
    if (s.kind != HOTBAR_BLOCK) return std::string();
    std::string n;
    if (!s.mod.empty()) {
        const size_t colon = s.mod.find(':');
        const std::string path = colon == std::string::npos ? s.mod : s.mod.substr(colon + 1);
        n = prettyName(s.mod, BlockRegistry::stockByName("eden:" + path) >= 0);
    } else if (s.type == HUD_TOOL_SIGN) {
        n = "Sign";
    } else if (s.type == HUD_TOOL_CMD) {
        n = "Command block";
    } else {
        const char* st = BlockRegistry::stockName(s.type);
        n = st ? prettyName(st, false) : "Block " + std::to_string(s.type);
    }
    if (s.colour) n += " (colour " + std::to_string((int)s.colour) + ")";
    return n;
}
