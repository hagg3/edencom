//
//  Console.mm
//  Eden -- Stage C / C.2: the console's registry, cvars, verbs, history and completion.
//  See Console.h and docs/console.md.
//
#import "Console.h"
#import "Globals.h"
#import "World.h"
#import "Terrain.h"
#import "TerrainGenerator.h"
#import "ClassicOptions.h"
#import "FileManager.h"
#import "Hud.h"
#import "Player.h"
#import "Menu.h"
#import "Resources.h"
#import "Util.h"
#include "WorldTrailer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

// The port's Settings / keybind tables (port/src/seam/Settings_web.mm, both targets link them).
extern "C" {
int         eden_settings_count(void);
const char* eden_settings_key(int i);
const char* eden_settings_label(int i);
const char* eden_settings_group(int i);
const char* eden_settings_hint(int i);
int         eden_settings_kind(int i);
float       eden_settings_min(int i);
float       eden_settings_max(int i);
float       eden_settings_step(int i);
float       eden_settings_def(int i);
int         eden_settings_enum_count(int i);
const char* eden_settings_enum_label(int i, int j);
int         eden_settings_native_hidden(int i);
int         eden_settings_apply(int i);
float       eden_settings_get(int i);
void        eden_settings_set(int i, float v);
void        eden_settings_reset_all(void);
int         eden_keybind_count(void);
const char* eden_keybind_action(int i);
const char* eden_keybind_label(int i);
int         eden_keybind_native_hidden(int i);
int         eden_keybind_default(int i);
int         eden_keybind_get(int i);
void        eden_keybind_set(int i, int code);
const char* eden_keybind_code_name(int code);
const char* eden_keybind_code_label(int code);
}

extern int fwc_result;

namespace Console {

bool testNoBridge = false;

namespace {

// ---- small string helpers ----------------------------------------------------------------------
std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}
std::string squash(const std::string& s) {   // lower case, no spaces: "2 min" == "2min"
    std::string r;
    for (char c : s) if (c != ' ') r += (char)std::tolower((unsigned char)c);
    return r;
}
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}
std::string join(const std::vector<std::string>& w, size_t from = 0) {
    std::string r;
    for (size_t i = from; i < w.size(); ++i) r += (i > from ? " " : "") + w[i];
    return r;
}
std::vector<std::string> splitCommas(const char* s) {
    std::vector<std::string> r;
    if (!s || !*s) return r;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == ',') { r.push_back(cur); cur.clear(); } else cur += *p;
    }
    r.push_back(cur);
    return r;
}
std::string num(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%g", v);
    return b;
}
bool parseNumber(const std::string& t, double* out) {
    if (t.empty()) return false;
    char* end = NULL;
    const double v = std::strtod(t.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// ---- the registry -------------------------------------------------------------------------------
struct CommandRec {
    std::string name, usage, help;
    int flags;
    std::vector<int> kinds;
    CommandFn fn;
};
struct CvarRec {
    Cvar c;                                  // string members point into the std::strings below
    std::string name, lname, label, help, options;
    std::vector<std::string> opts;
};

std::vector<CommandRec> g_cmds;
std::deque<CvarRec> g_cvars;   // a deque: Cvar::name etc. point into the records' std::strings (SSO), so a record must never move
std::unordered_map<std::string, int> g_index;   // lower-case name -> Ext index
bool g_inited = false;
enum { CVAR_BASE = 4096 };

#ifdef EDEN_DIAGNOSTICS
const bool kDiag = true;
#else
const bool kDiag = false;
#endif

// ---- scrollback, history, anchor ------------------------------------------------------------------
std::vector<Line> g_scroll;
void (*g_mirror)(int, const char*) = NULL;
std::vector<std::string> g_history;
bool g_anchorFixed = false;
int g_anchor[3] = {0, 0, 0};

struct ConsoleSink : CmdScript::Sink {
    void line(int kind, const std::string& text) override { Console::print(kind, text); }
};
ConsoleSink g_sink;

World* world() {
    World* w = World::getWorld;
    return (w && w->terrain && w->terrain->loaded && w->fm) ? w : NULL;
}

// ---- cvar value text / parse ----------------------------------------------------------------------
std::string valueText(const CvarRec& r) {
    const Cvar& c = r.c;
    switch (c.kind) {
    case KIND_TEXT: {
        const char* t = c.getText ? c.getText(c.tag) : "";
        return t ? t : "";
    }
    case KIND_TOGGLE: return c.get(c.tag) != 0 ? "on" : "off";
    case KIND_ENUM: {
        const int i = (int)std::lround(c.get(c.tag));
        return (i >= 0 && i < (int)r.opts.size()) ? r.opts[i] : num(i);
    }
    default: return num(c.get(c.tag));
    }
}

std::string defaultText(const CvarRec& r) {
    const Cvar& c = r.c;
    switch (c.kind) {
    case KIND_TOGGLE: return c.def != 0 ? "on" : "off";
    case KIND_ENUM: {
        const int i = (int)std::lround(c.def);
        return (i >= 0 && i < (int)r.opts.size()) ? r.opts[i] : num(i);
    }
    case KIND_TEXT: return "";
    default: return num(c.def);
    }
}

std::string applyNote(int apply) {
    if (apply == APPLY_NEXT_WORLD) return "applies when a world opens";
    if (apply == APPLY_NEXT_CONVERT) return "applies at the next conversion";
    return "";
}

std::string describeCvar(const CvarRec& r) {
    const Cvar& c = r.c;
    std::string s = r.name + " = " + valueText(r);
    std::string meta;
    switch (c.kind) {
    case KIND_TOGGLE: meta = "on/off, default " + defaultText(r); break;
    case KIND_RANGE: meta = num(c.min) + ".." + num(c.max) + ", default " + defaultText(r); break;
    case KIND_ENUM:
        meta = "one of: ";
        for (size_t i = 0; i < r.opts.size(); ++i) meta += (i ? ", " : "") + r.opts[i];
        meta += "; default " + defaultText(r);
        break;
    default: break;
    }
    if (c.apply == APPLY_READONLY) meta += (meta.empty() ? "" : "; ") + std::string("read-only in game");
    else { const std::string n = applyNote(c.apply); if (!n.empty()) meta += (meta.empty() ? "" : "; ") + n; }
    if (!meta.empty()) s += "  (" + meta + ")";
    if (!r.help.empty()) s += " " + r.help;
    else if (!r.label.empty()) s += " " + r.label;
    return s;
}

// ---- the reference tables (C.10) ---------------------------------------------------------------------------
std::string mdCell(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '|') o += "\\|"; else o += ch; }
    return o;
}

std::string mdValues(const CvarRec& r) {
    const Cvar& c = r.c;
    switch (c.kind) {
    case KIND_TOGGLE: return "on / off";
    case KIND_RANGE: return num(c.min) + " to " + num(c.max);
    case KIND_ENUM: {
        std::string s;
        for (size_t i = 0; i < r.opts.size(); ++i) s += (i ? ", " : "") + r.opts[i];
        return s;
    }
    default: return r.lname.compare(0, 5, "keys.") == 0 ? "a key name, or none" : "text";
    }
}

}  // namespace (reopened below)
std::string referenceMarkdown(const char* section) {
    const std::string sec = section ? section : "";
    std::string o;
    if (sec == "block-verbs") {
        o += "| Verb | Usage | What it does |\n|---|---|---|\n";
        size_t n = 0;
        const CmdScript::Verb* vs = CmdScript::verbs(&n);
        for (size_t i = 0; i < n; ++i)
            o += "| `" + std::string(vs[i].name) + "` | " + (*vs[i].usage ? "`" + mdCell(vs[i].usage) + "`" : std::string("-")) + " | " + mdCell(vs[i].help) + " |\n";
    } else if (sec == "console-verbs") {
        o += "| Verb | Usage | What it does | Needs |\n|---|---|---|---|\n";
        for (const CommandRec& r : g_cmds) {
            std::string need;
            if (r.flags & CF_WORLD) need += "a world";
            if (r.flags & CF_DIAG) need += std::string(need.empty() ? "" : ", ") + "a diagnostics build";
            o += "| `" + r.name + "` | " + (r.usage.empty() ? "-" : "`" + mdCell(r.usage) + "`") + " | " + mdCell(r.help) + " | " + (need.empty() ? "-" : need) + " |\n";
        }
    } else if (sec == "variables") {
        std::vector<std::string> groups;
        for (const CvarRec& r : g_cvars) {
            const std::string g = r.lname.substr(0, r.lname.find('.'));
            if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
        }
        for (const std::string& g : groups) {
            o += "#### `" + g + ".`\n\n| Variable | Values | Default | Applies | What |\n|---|---|---|---|---|\n";
            for (const CvarRec& r : g_cvars) {
                if (r.lname.compare(0, g.size() + 1, g + ".") != 0) continue;
                const Cvar& c = r.c;
                const std::string def = c.kind == KIND_TEXT ? "-" : defaultText(r);
                const std::string app = c.apply == APPLY_READONLY ? "read-only in game"
                                      : c.apply == APPLY_NEXT_WORLD ? "when a world opens"
                                      : c.apply == APPLY_NEXT_CONVERT ? "at the next conversion" : "at once";
                std::string what = r.help.empty() ? r.label : r.help;
                if (c.flags & CF_DIAG) what += " (diagnostics builds)";
                o += "| `" + r.name + "` | " + mdCell(mdValues(r)) + " | " + mdCell(def) + " | " + app + " | " + mdCell(what) + " |\n";
            }
            o += "\n";
        }
    }
    return o;
}
namespace {

// ---- Settings bridge ---------------------------------------------------------------------------------
float setGet(int tag) { return eden_settings_get(tag); }
void setSet(int tag, float v) { eden_settings_set(tag, v); }
void setReset(int tag) { eden_settings_set(tag, eden_settings_def(tag)); }

const char* keyText(int tag) {
    static std::string s;
    const int code = eden_keybind_get(tag);
    const char* l = code ? eden_keybind_code_label(code) : "";
    s = (code && *l) ? l : "none";
    return s.c_str();
}
bool keySet(int tag, const char* text) {
    const std::string t = squash(text);
    int code = -1;
    if (t == "none" || t == "0" || t == "unbound") code = 0;
    for (int c = 1; c < 512 && code < 0; ++c) {
        const char* l = eden_keybind_code_label(c);
        const char* n = eden_keybind_code_name(c);
        if (l && *l && (squash(l) == t || squash(n ? n : "") == t)) code = c;
    }
    if (code < 0) return false;
    eden_keybind_set(tag, code);
    return eden_keybind_get(tag) == code;
}
void keyReset(int tag) { eden_keybind_set(tag, eden_keybind_default(tag)); }

void bridgeSettings() {
    const int n = eden_settings_count();
    for (int i = 0; i < n; ++i) {
        Cvar c = {};
        const std::string group = lower(eden_settings_group(i));
        const std::string name = group + "." + eden_settings_key(i);
        c.name = name.c_str();
        c.label = eden_settings_label(i);
        c.help = eden_settings_hint(i)[0] ? eden_settings_hint(i) : NULL;
        c.kind = eden_settings_kind(i);
        c.min = eden_settings_min(i);
        c.max = eden_settings_max(i);
        c.step = eden_settings_step(i);
        c.def = eden_settings_def(i);
        std::string opts;
        if (c.kind == KIND_ENUM)
            for (int j = 0, m = eden_settings_enum_count(i); j < m; ++j)
                opts += (j ? "," : "") + std::string(eden_settings_enum_label(i, j));
        c.options = opts.c_str();
        c.apply = eden_settings_apply(i);
        c.flags = eden_settings_native_hidden(i) ? CF_DIAG : 0;   // the product decided not to offer these
        c.tag = i;
        c.get = setGet;
        c.set = setSet;
        c.reset = setReset;
        addCvar(c);
    }
    const int kn = eden_keybind_count();
    for (int i = 0; i < kn; ++i) {
        if (eden_keybind_native_hidden(i)) continue;
        Cvar c = {};
        const std::string name = std::string("keys.") + eden_keybind_action(i);
        c.name = name.c_str();
        c.label = eden_keybind_label(i);
        c.help = c.label;
        c.kind = KIND_TEXT;
        c.apply = APPLY_LIVE;
        c.tag = i;
        c.getText = keyText;
        c.setText = keySet;
        c.reset = keyReset;
        addCvar(c);
    }
}

// ---- world.* / gen.* ---------------------------------------------------------------------------------
std::string g_text;
const char* wName(int) {
    World* w = world();
    WorldNode* sel = w && w->menu ? w->menu->selected_world : NULL;
    g_text = (sel && sel->display_name) ? [sel->display_name UTF8String] : "";
    return g_text.c_str();
}
bool wSetName(int, const char* text) {
    World* w = world();
    WorldNode* sel = w && w->menu ? w->menu->selected_world : NULL;
    if (!sel) return false;
    const size_t len = std::strlen(text);
    if (len == 0 || len > 49) return false;                       // WorldFileHeader::name is 50 bytes
    for (size_t i = 0; i < len; ++i) if ((unsigned char)text[i] < 0x20) return false;
    NSString* nn = [[NSString stringWithUTF8String:text] retain];
    if (!nn) return false;
    sel->display_name = nn;   // the next save takes the name from here; the old string is left to its owner
    if (w->fm->emodOpen() && sel->file_name) w->fm->renameWorld(sel->file_name, nn);   // the open store, one batch
    return true;
}
const char* wFile(int) {
    World* w = world();
    WorldNode* sel = w && w->menu ? w->menu->selected_world : NULL;
    g_text = (sel && sel->file_name) ? [sel->file_name UTF8String] : "";
    return g_text.c_str();
}
const char* wSeed(int) { World* w = world(); g_text = w ? std::to_string(w->terrain->level_seed) : ""; return g_text.c_str(); }
const char* wHeight(int) { World* w = world(); g_text = w ? std::to_string(g_world_height) : ""; return g_text.c_str(); }
const char* wType(int) {
    World* w = world();
    if (!w) g_text = "";
    else if (w->fm->genflat) g_text = "flat";
    else if (w->terrain->tgen && w->terrain->tgen->classicState == TerrainGenerator::CLASSIC_STATE_SET) g_text = "classic";
    else if (w->terrain->tgen && w->terrain->tgen->classicState == TerrainGenerator::CLASSIC_STATE_VOID) g_text = "void";
    else g_text = "normal";
    return g_text.c_str();
}
const char* wSigns(int) { World* w = world(); g_text = w ? std::to_string(w->fm->trailer()->signCount()) : ""; return g_text.c_str(); }
const char* wCmds(int) { World* w = world(); g_text = w ? std::to_string(w->fm->trailer()->cmdCount()) : ""; return g_text.c_str(); }
float wGold(int) { World* w = world(); return w ? (float)w->hud->goldencubes : 0.0f; }
void wSetGold(int, float v) {
    World* w = world();
    if (!w) return;
    w->hud->goldencubes = (int)std::lround(std::max(0.0f, std::min(999.0f, v)));
}

const char* genText(int tag) {
    World* w = world();
    if (!w || !w->terrain->tgen || w->fm->genflat ||
        w->terrain->tgen->classicState != TerrainGenerator::CLASSIC_STATE_SET) return "n/a";
    const ClassicGen& g = w->terrain->tgen->classic;
    switch (tag) {
    case 0: return kHills[classic_option_index(kHills, 5, g.hilliness)].label;
    case 1: return kCaves[classic_option_index(kCaves, 3, g.caves)].label;
    case 2: return kTrees[classic_option_index(kTrees, 4, g.trees)].label;
    case 3: return g.flowers ? "on" : "off";
    case 4: return g.clouds ? "on" : "off";
    case 5: return kSizes[classic_option_index(kSizes, 2, g.size)].label;
    default: return kScales[classic_option_index(kScales, 5, g.scale4)].label;
    }
}

void bridgeWorld() {
    struct T { const char* name; const char* label; const char* (*get)(int); bool (*set)(int, const char*); int apply; };
    static const T txt[] = {
        {"world.name", "World name", wName, wSetName, APPLY_LIVE},
        {"world.file", "File name", wFile, NULL, APPLY_READONLY},
        {"world.seed", "Seed", wSeed, NULL, APPLY_READONLY},
        {"world.height", "Height (64 or 256)", wHeight, NULL, APPLY_READONLY},
        {"world.type", "Generator", wType, NULL, APPLY_READONLY},
        {"world.signs", "Signs", wSigns, NULL, APPLY_READONLY},
        {"world.cmdblocks", "Command blocks", wCmds, NULL, APPLY_READONLY},
    };
    for (const T& t : txt) {
        Cvar c = {};
        c.name = t.name; c.label = t.label; c.help = t.label;
        c.kind = KIND_TEXT; c.apply = t.apply; c.getText = t.get; c.setText = t.set;
        addCvar(c);
    }
    Cvar g = {};
    g.name = "world.gold"; g.label = "Golden cubes"; g.help = "The purse (0..999).";
    g.kind = KIND_RANGE; g.min = 0; g.max = 999; g.step = 1; g.def = 0; g.apply = APPLY_LIVE;
    g.get = wGold; g.set = wSetGold;
    addCvar(g);
    static const char* const gen[] = {"gen.hills", "gen.caves", "gen.trees", "gen.flowers", "gen.clouds", "gen.size", "gen.scale"};
    static const char* const genLabel[] = {"Hills", "Caves", "Trees", "Flowers", "Clouds", "World size", "Scale"};
    for (int i = 0; i < 7; ++i) {
        Cvar c = {};
        c.name = gen[i]; c.label = genLabel[i]; c.help = genLabel[i];
        c.kind = KIND_TEXT; c.apply = APPLY_READONLY; c.tag = i; c.getText = genText;
        addCvar(c);
    }
}

// ---- argument helpers for the verbs ---------------------------------------------------------------------
bool parseCoord(const std::string& w, int rel, int* out) {   // "=N" absolute, "N"/"~N"/"~" relative to `rel`
    std::string t = w;
    bool abs = false;
    if (!t.empty() && t[0] == '=') { abs = true; t = t.substr(1); }
    else if (!t.empty() && t[0] == '~') t = t.substr(1);
    if (t.empty() && !abs) { *out = rel; return true; }
    double v;
    if (!parseNumber(t, &v) || v != std::floor(v) || std::fabs(v) > 10000000) return false;
    *out = abs ? (int)v : rel + (int)v;
    return true;
}
bool parseCell(const Ctx& c, size_t from, int* x, int* y, int* z) {
    return c.args.size() >= from + 3 && parseCoord(c.args[from], c.ax, x) && parseCoord(c.args[from + 1], c.ay, y) &&
           parseCoord(c.args[from + 2], c.az, z);
}
std::string cellText(int x, int y, int z) { return std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z); }
std::string relText(const Ctx& c, int x, int y, int z) {
    return "(" + std::to_string(x - c.ax) + " " + std::to_string(y - c.ay) + " " + std::to_string(z - c.az) + " from the anchor)";
}

// ---- the verbs ------------------------------------------------------------------------------------------------
const CvarRec* findCvarRec(const std::string& n) {
    const auto it = g_index.find(lower(n));
    return (it != g_index.end() && it->second >= CVAR_BASE) ? &g_cvars[it->second - CVAR_BASE] : NULL;
}
const CommandRec* findCmdRec(const std::string& n) {
    const auto it = g_index.find(lower(n));
    return (it != g_index.end() && it->second < CVAR_BASE) ? &g_cmds[it->second] : NULL;
}

bool contains(const std::string& hay, const std::string& needle) { return lower(hay).find(needle) != std::string::npos; }

void cmdHelp(Ctx& c) {
    if (c.args.empty()) {
        std::string blk, con;
        size_t n = 0;
        const CmdScript::Verb* vs = CmdScript::verbs(&n);
        for (size_t i = 0; i < n; ++i) blk += (i ? " " : "") + std::string(vs[i].name);
        for (const CommandRec& r : g_cmds) con += (con.empty() ? "" : " ") + r.name;
        c.print("Block verbs (work in command blocks too): %s", blk.c_str());
        c.print("Console verbs: %s", con.c_str());
        c.print("Variables are group.name: try `cvars`. `help VERB`, `help video.fov`, `find TEXT`.");
        return;
    }
    const std::string a = lower(c.args[0]);
    if (const CmdScript::Verb* v = CmdScript::findVerb(a.c_str())) {
        c.print("%s %s", v->name, v->usage);
        c.print("  %s (also works in a command block)", v->help);
    } else if (const CommandRec* r = findCmdRec(a)) {
        c.print("%s %s", r->name.c_str(), r->usage.c_str());
        c.print("  %s", r->help.c_str());
    } else if (const CvarRec* v = findCvarRec(a)) {
        c.print("%s", describeCvar(*v).c_str());
    } else {
        c.error("No verb or variable named %s (try `find %s`)", c.args[0].c_str(), c.args[0].c_str());
    }
}

void cmdFind(Ctx& c) {
    if (c.args.empty()) { c.error("find needs TEXT"); return; }
    const std::string t = lower(join(c.args));
    std::vector<std::string> out;
    size_t n = 0;
    const CmdScript::Verb* vs = CmdScript::verbs(&n);
    for (size_t i = 0; i < n; ++i)
        if (contains(vs[i].name, t) || contains(vs[i].help, t))
            out.push_back(std::string(vs[i].name) + " " + vs[i].usage + "  - " + vs[i].help);
    for (const CommandRec& r : g_cmds)
        if (contains(r.name, t) || contains(r.help, t)) out.push_back(r.name + " " + r.usage + "  - " + r.help);
    for (const CvarRec& r : g_cvars)
        if (contains(r.name, t) || contains(r.label, t) || contains(r.help, t))
            out.push_back(r.name + " = " + valueText(r) + "  - " + (r.help.empty() ? r.label : r.help));
    if (out.empty()) { c.print("Nothing matches \"%s\"", t.c_str()); return; }
    const size_t cap = 30;
    for (size_t i = 0; i < out.size() && i < cap; ++i) c.print("%s", out[i].c_str());
    if (out.size() > cap) c.print("... %zu more (be more specific)", out.size() - cap);
}

void cmdCvars(Ctx& c) {
    const std::string filter = c.args.empty() ? "" : lower(c.args[0]);
    std::vector<std::string> groups;
    for (const CvarRec& r : g_cvars) {
        const std::string g = r.lname.substr(0, r.lname.find('.'));
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    if (!filter.empty() && std::find(groups.begin(), groups.end(), filter) == groups.end()) {
        c.error("No variable group named %s. Groups: %s", filter.c_str(), join(groups).c_str());
        return;
    }
    for (const std::string& g : groups) {
        if (!filter.empty() && g != filter) continue;
        std::string line = g + ":";
        for (const CvarRec& r : g_cvars) {
            if (r.lname.compare(0, g.size() + 1, g + ".") != 0) continue;
            if (!filter.empty()) c.print("%s = %s", r.name.c_str(), valueText(r).c_str());
            else line += " " + r.name.substr(g.size() + 1) + "=" + valueText(r);
        }
        if (filter.empty()) c.print("%s", line.c_str());
    }
}

void cmdToggle(Ctx& c) {
    if (c.args.empty()) { c.error("toggle needs a variable"); return; }
    const int i = cvarIndex(c.args[0].c_str());
    if (i < 0) { c.error("No variable named %s", c.args[0].c_str()); return; }
    const CvarRec& r = g_cvars[i];
    std::string next;
    if (r.c.kind == KIND_TOGGLE) next = valueText(r) == "on" ? "off" : "on";
    else if (r.c.kind == KIND_ENUM && !r.opts.empty()) {
        const int cur = (int)std::lround(r.c.get(r.c.tag));
        next = r.opts[(cur + 1) % (int)r.opts.size()];
    } else { c.error("%s is not an on/off or choice variable", r.name.c_str()); return; }
    std::string note;
    const std::string err = cvarSet(i, next.c_str(), &note);
    if (!err.empty()) { c.error("%s", err.c_str()); return; }
    c.print("%s = %s%s", r.name.c_str(), valueText(r).c_str(), note.empty() ? "" : ("  (" + note + ")").c_str());
}

void cmdReset(Ctx& c) {
    if (c.args.empty()) { c.error("reset needs a variable, a group or all"); return; }
    const std::string a = lower(c.args[0]);
    if (a == "all") {
        if (c.args.size() < 2 || lower(c.args[1]) != "yes") {
            c.print("This resets every setting and every key. Type `reset all yes` to do it.");
            return;
        }
        eden_settings_reset_all();
        c.print("Every setting and key is back to its default");
        return;
    }
    int n = 0;
    const int one = cvarIndex(a.c_str());
    for (size_t i = 0; i < g_cvars.size(); ++i) {
        const CvarRec& r = g_cvars[i];
        const bool hit = one >= 0 ? (int)i == one : r.lname.compare(0, a.size() + 1, a + ".") == 0;
        if (!hit) continue;
        if (r.c.apply == APPLY_READONLY || (r.c.flags & CF_CHEAT)) {
            if (one >= 0) { c.error("%s is read-only in game", r.name.c_str()); return; }
            continue;
        }
        if (r.c.reset) { r.c.reset(r.c.tag); ++n; }
        else if (r.c.kind != KIND_TEXT && r.c.set) { r.c.set(r.c.tag, r.c.def); ++n; }
        else if (one >= 0) { c.error("%s has no default to go back to", r.name.c_str()); return; }
    }
    if (one < 0 && n == 0) { c.error("No variable or group named %s", c.args[0].c_str()); return; }
    if (one >= 0) c.print("%s = %s", g_cvars[one].name.c_str(), valueText(g_cvars[one]).c_str());
    else c.print("%d variables reset", n);
}

void cmdClear(Ctx&) { Console::clear(); }

void cmdHistory(Ctx& c) {
    if (g_history.empty()) { c.print("No history yet"); return; }
    for (size_t i = 0; i < g_history.size(); ++i) c.print("%zu  %s", i + 1, g_history[i].c_str());
}

void cmdEcho(Ctx& c) { c.print("%s", join(c.args).c_str()); }

void cmdWhere(Ctx& c) {
    int px, py, pz;
    CmdScript::playerAnchor(&px, &py, &pz);
    c.print("you stand on %s %s", cellText(px, py, pz).c_str(), relText(c, px, py, pz).c_str());
    c.print("anchor %s%s", cellText(c.ax, c.ay, c.az).c_str(), g_anchorFixed ? "" : " (under you)");
    int lx, ly, lz;
    if (lookCell(&lx, &ly, &lz)) c.print("looking at %s %s", cellText(lx, ly, lz).c_str(), relText(c, lx, ly, lz).c_str());
    else c.print("looking at nothing in reach");
}

void cmdAnchor(Ctx& c) {
    if (!c.args.empty()) {
        const std::string a = lower(c.args[0]);
        if (a == "me") setAnchorMe();
        else if (a == "look") {
            int x, y, z;
            if (!lookCell(&x, &y, &z)) { c.error("Nothing in front of you to anchor on"); return; }
            setAnchorFixed(x, y, z);
        } else {
            int x, y, z;
            if (c.args.size() != 3 || !parseCell(c, 0, &x, &y, &z)) {
                c.error("anchor takes me, look or X Y Z (=N for an absolute coordinate)");
                return;
            }
            setAnchorFixed(x, y, z);
        }
    }
    int x, y, z;
    currentAnchor(&x, &y, &z);
    c.print("anchor %s%s", cellText(x, y, z).c_str(), g_anchorFixed ? "" : " (the cell under you)");
}

void cmdJobs(Ctx& c) {
    std::vector<CmdScript::JobInfo> js;
    for (const CmdScript::JobInfo& j : CmdScript::jobs()) if (j.id != c.job) js.push_back(j);   // not this very line
    if (js.empty()) { c.print("No jobs waiting"); return; }
    for (const CmdScript::JobInfo& j : js)
        c.print("[job %d] %.1f s left, statement %zu of %zu: %s", j.id, j.wait, j.pc + 1, j.count, j.src.c_str());
}

void cmdStop(Ctx& c) {
    int n;
    if (c.args.empty()) n = CmdScript::stop(CmdScript::STOP_JOBS);
    else if (lower(c.args[0]) == "all") n = CmdScript::stop(CmdScript::STOP_ALL);
    else {
        double v;
        if (!parseNumber(c.args[0], &v) || v < 1 || v != std::floor(v)) { c.error("stop takes a job number or all"); return; }
        n = CmdScript::stop((int)v);
        if (!n) { c.error("No job %s", c.args[0].c_str()); return; }
    }
    if (!n) c.print("Nothing to stop");
}

void cmdSave(Ctx& c) {
    World* w = world();
    if (!w) { c.error("No world is open"); return; }
    w->fm->saveWorld();
    Resources::getResources->playSound(S_WORLD_SAVED);
    c.print("World saved");
}

void cmdGetblock(Ctx& c) {   // diagnostics: the raw type:colour of a cell
    World* w = world();
    int x, y, z;
    if (!w || !parseCell(c, 0, &x, &y, &z)) { c.error("getblock needs X Y Z in an open world"); return; }
    c.print("%s = %d:%d", cellText(x, y, z).c_str(), w->terrain->getLand(x, z, y), w->terrain->getColor(x, z, y));
}
void cmdCmdtrace(Ctx& c) {
    if (c.args.empty()) { c.print("cmdtrace is %s", CmdScript::trace ? "on" : "off"); return; }
    CmdScript::trace = c.args[0] != "0" && lower(c.args[0]) != "off";
    c.print("cmdtrace is %s", CmdScript::trace ? "on" : "off");
}
void cmdStats(Ctx& c) { c.print("%s", CmdScript::describe()); }

// ---- the Ext ---------------------------------------------------------------------------------------------------
bool cheatsAllowed() {
    const int i = cvarIndex("experiments.allow_cheats");
    return i >= 0 && g_cvars[i].c.get && g_cvars[i].c.get(g_cvars[i].c.tag) != 0;
}

int extFind(const char* verb) {
    const auto it = g_index.find(verb ? verb : "");
    return it == g_index.end() ? -1 : it->second;
}

void extRun(int index, const CmdScript::ExtCall& call) {
    Ctx ctx{call.ax, call.ay, call.az, call.args, call.sink, call.source, call.job};
    if (index >= CVAR_BASE) {
        const int i = index - CVAR_BASE;
        const CvarRec& r = g_cvars[i];
        if (call.args.empty()) { ctx.print("%s", describeCvar(r).c_str()); return; }
        if (r.c.kind != KIND_TEXT && call.args.size() > 1) { ctx.error("%s takes one value", r.name.c_str()); return; }
        std::string note;
        const std::string err = cvarSet(i, (r.c.kind == KIND_TEXT ? join(call.args) : call.args[0]).c_str(), &note);
        if (!err.empty()) { ctx.error("%s", err.c_str()); return; }
        ctx.print("%s = %s%s", r.name.c_str(), valueText(g_cvars[i]).c_str(), note.empty() ? "" : ("  (" + note + ")").c_str());
        return;
    }
    const CommandRec& r = g_cmds[index];
    if ((r.flags & CF_CHEAT) && !cheatsAllowed()) { ctx.error("Turn on Allow cheats in Settings > Experiments"); return; }
    if ((r.flags & CF_WORLD) && !world()) { ctx.error("No world is open"); return; }
    r.fn(ctx);
}

const CmdScript::Ext kExt = {extFind, extRun};

}  // namespace

// ---- Ctx ------------------------------------------------------------------------------------------------------------
void Ctx::print(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (sink) sink->line(CmdScript::Sink::OUT, b);
}
void Ctx::error(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (sink) sink->line(CmdScript::Sink::ERR, b);
}

// ---- registration ---------------------------------------------------------------------------------------------------
void addCommand(const char* name, const char* usage, const char* help, int flags,
                std::initializer_list<int> argKinds, CommandFn fn) {
    if ((flags & CF_DIAG) && !kDiag) return;   // absent, not refused
    const std::string ln = lower(name ? name : "");
    if (ln.empty() || ln.find('.') != std::string::npos || CmdScript::findVerb(ln.c_str()) || g_index.count(ln)) {
        std::fprintf(stderr, "[eden-console] verb '%s' is empty, dotted, a block verb or already registered: dropped\n", name ? name : "");
        return;
    }
    g_index[ln] = (int)g_cmds.size();
    g_cmds.push_back({ln, usage ? usage : "", help ? help : "", flags, std::vector<int>(argKinds), fn});
}

void addCvar(const Cvar& c) {
    if ((c.flags & CF_DIAG) && !kDiag) return;
    const std::string ln = lower(c.name ? c.name : "");
    if (ln.find('.') == std::string::npos || ln.find(' ') != std::string::npos || g_index.count(ln)) {
        std::fprintf(stderr, "[eden-console] cvar '%s' has no group, a space or is already registered: dropped\n", c.name ? c.name : "");
        return;
    }
    g_index[ln] = CVAR_BASE + (int)g_cvars.size();
    g_cvars.emplace_back();
    CvarRec& r = g_cvars.back();
    r.c = c;
    r.name = c.name;
    r.lname = ln;
    r.label = c.label ? c.label : "";
    r.help = c.help ? c.help : "";
    r.options = c.options ? c.options : "";
    r.opts = splitCommas(r.options.c_str());
    r.c.name = r.name.c_str(); r.c.label = r.label.c_str(); r.c.help = r.help.c_str(); r.c.options = r.options.c_str();
}

void init() {
    if (g_inited) return;
    g_inited = true;
    addCommand("help", "[VERB or VARIABLE]", "Lists the verbs, or explains one.", 0, {ARG_VERB}, cmdHelp);
    addCommand("find", "TEXT", "Every verb and variable whose name or description has TEXT.", 0, {CmdScript::ARG_TEXT}, cmdFind);
    addCommand("cvars", "[GROUP]", "Lists the variables (every one of a GROUP).", 0, {}, cmdCvars);
    addCommand("toggle", "VARIABLE", "Flips an on/off variable; steps a choice to the next option.", 0, {ARG_CVAR}, cmdToggle);
    addCommand("reset", "VARIABLE | GROUP | all", "Back to the default. `reset all yes` resets every setting and key.", 0, {ARG_CVAR}, cmdReset);
    addCommand("clear", "", "Empties the scrollback.", 0, {}, cmdClear);
    addCommand("history", "", "The lines you ran last.", 0, {}, cmdHistory);
    addCommand("echo", "[TEXT]", "Prints TEXT here only (say also toasts).", 0, {CmdScript::ARG_TEXT}, cmdEcho);
    addCommand("where", "", "Your cell, the anchor and the cell you look at, with their offsets.", CF_WORLD, {}, cmdWhere);
    addCommand("anchor", "[me | look | X Y Z]", "What 0 0 0 means: the cell under you (me), the cell you look at, or a fixed cell.", CF_WORLD, {}, cmdAnchor);
    addCommand("jobs", "", "The lines still waiting on a `wait`.", 0, {}, cmdJobs);
    addCommand("stop", "[JOB | all]", "Stops one job, every job, or all (blocks too).", 0, {CmdScript::ARG_NUMBER}, cmdStop);
    addCommand("save", "", "Saves the world (the pause menu's Save).", CF_WORLD, {}, cmdSave);
    addCommand("getblock", "X Y Z", "The raw type:colour of a cell.", CF_DIAG | CF_WORLD, {CmdScript::ARG_COORD, CmdScript::ARG_COORD, CmdScript::ARG_COORD}, cmdGetblock);
    addCommand("cmdtrace", "[0|1]", "Prints every statement a script runs.", CF_DIAG, {CmdScript::ARG_NUMBER}, cmdCmdtrace);
    addCommand("stats", "", "The command interpreter's counters.", CF_DIAG, {}, cmdStats);
    if (!testNoBridge) bridgeSettings();
    bridgeWorld();
    historyLoad();
}

void reset() {
    setAnchorMe();
}

const CmdScript::Ext* ext() { init(); return &kExt; }

// ---- scrollback --------------------------------------------------------------------------------------------------------
const std::vector<Line>& scrollback() { return g_scroll; }
static unsigned g_serial = 0;   // lines ever printed: scrollback()[i] is line number (g_serial - size + i)
unsigned scrollbackSerial() { return g_serial; }
void print(int kind, const std::string& text) {
    g_scroll.push_back({kind, text});
    ++g_serial;
    if (g_scroll.size() > (size_t)MAX_SCROLLBACK) g_scroll.erase(g_scroll.begin(), g_scroll.begin() + (g_scroll.size() - MAX_SCROLLBACK));
    if (g_mirror) g_mirror(kind, text.c_str());
}
void clear() { g_scroll.clear(); }
void setMirror(void (*hook)(int, const char*)) { g_mirror = hook; }

// ---- history -----------------------------------------------------------------------------------------------------------
size_t historyCount() { return g_history.size(); }
const std::string& historyGet(size_t i) { static const std::string none; return i < g_history.size() ? g_history[i] : none; }
void historyAdd(const std::string& line) {
    if (line.empty() || (!g_history.empty() && g_history.back() == line)) return;
    g_history.push_back(line);
    if (g_history.size() > (size_t)MAX_HISTORY) g_history.erase(g_history.begin(), g_history.begin() + (g_history.size() - MAX_HISTORY));
}
void historyClear() { g_history.clear(); }
void historySave() {
    std::string all;
    for (size_t i = 0; i < g_history.size(); ++i) all += (i ? "\x1f" : "") + g_history[i];
    NSUserDefaults* prefs = [NSUserDefaults standardUserDefaults];
    [prefs setObject:[NSString stringWithUTF8String:all.c_str()] forKey:@"console.history"];
    [prefs synchronize];
}
void historyLoad() {
    g_history.clear();
    NSString* s = [[NSUserDefaults standardUserDefaults] stringForKey:@"console.history"];
    if (!s) return;
    const std::string all = [s UTF8String];
    size_t i = 0;
    while (i <= all.size()) {
        size_t j = all.find('\x1f', i);
        if (j == std::string::npos) j = all.size();
        if (j > i) historyAdd(all.substr(i, j - i));
        i = j + 1;
    }
}

// ---- anchor --------------------------------------------------------------------------------------------------------------
bool anchorIsFixed() { return g_anchorFixed; }
void setAnchorMe() { g_anchorFixed = false; }
void setAnchorFixed(int x, int y, int z) { g_anchorFixed = true; g_anchor[0] = x; g_anchor[1] = y; g_anchor[2] = z; }
void currentAnchor(int* x, int* y, int* z) {
    if (g_anchorFixed) { *x = g_anchor[0]; *y = g_anchor[1]; *z = g_anchor[2]; }
    else if (world()) CmdScript::playerAnchor(x, y, z);
    else *x = *y = *z = 0;
}
bool lookCell(int* x, int* y, int* z) {
    World* w = world();
    if (!w) return false;
    const Point3D hit = findWorldCoords((int)(SCREEN_HEIGHT / 2), (int)(SCREEN_WIDTH / 2), FC_DESTROY);
    if (hit.x == -1 || fwc_result != -1) return false;
    if (w->terrain->getLand(hit.x, hit.z, hit.y) <= 0) return false;
    *x = hit.x; *y = hit.y; *z = hit.z;
    return true;
}

// ---- running a line ---------------------------------------------------------------------------------------------------------
int run(const char* line) {
    init();
    std::string l = line ? line : "";
    while (!l.empty() && std::isspace((unsigned char)l.back())) l.pop_back();
    size_t k = 0;
    while (k < l.size() && std::isspace((unsigned char)l[k])) ++k;
    l = l.substr(k);
    if (l.empty()) return CmdScript::RUN_EMPTY;
    historyAdd(l);
    historySave();
    int ax, ay, az;
    currentAnchor(&ax, &ay, &az);
    return CmdScript::runLine(l.c_str(), ax, ay, az, CmdScript::SRC_CONSOLE, &g_sink, &kExt);
}

std::string storedWorldName(const std::string& file) {
    World* w = World::getWorld;
    if (!w || !w->fm) return "";
    NSString* n = w->fm->getName([NSString stringWithUTF8String:file.c_str()]);
    return n ? std::string([n UTF8String]) : std::string();
}

// ---- cvars ----------------------------------------------------------------------------------------------------------------------
size_t cvarCount() { return g_cvars.size(); }
const Cvar* cvarAt(size_t i) { return i < g_cvars.size() ? &g_cvars[i].c : NULL; }
int cvarIndex(const char* name) {
    const auto it = g_index.find(lower(name ? name : ""));
    return (it != g_index.end() && it->second >= CVAR_BASE) ? it->second - CVAR_BASE : -1;
}
std::string cvarValue(int i) { return (i >= 0 && i < (int)g_cvars.size()) ? valueText(g_cvars[i]) : std::string(); }

std::string cvarSet(int i, const char* text, std::string* note) {
    if (i < 0 || i >= (int)g_cvars.size()) return "No such variable";
    const CvarRec& r = g_cvars[i];
    const Cvar& c = r.c;
    if (note) note->clear();
    if (c.apply == APPLY_READONLY) {
        std::string e = r.name + " is read-only in game";
        if (r.lname.compare(0, 4, "gen.") == 0 || r.lname == "world.seed" || r.lname == "world.height" || r.lname == "world.type")
            e += ". Change terrain in World Properties (world list)";
        return e;
    }
    if (c.flags & CF_CHEAT) {
        if (!cheatsAllowed()) return "Turn on Allow cheats in Settings > Experiments";
    }
    const std::string t = text ? text : "";
    switch (c.kind) {
    case KIND_TEXT:
        if (!c.setText || !c.setText(c.tag, t.c_str())) {
            if (r.lname == "world.name") return "world.name needs 1 to 49 characters in an open world";
            if (r.lname.compare(0, 5, "keys.") == 0) return "Not a key name: " + t + " (try F1, Space, LShift, `, or none)";
            return r.name + " can't take that";
        }
        break;
    case KIND_TOGGLE: {
        const std::string v = squash(t);
        if (v == "1" || v == "on" || v == "true" || v == "yes") c.set(c.tag, 1);
        else if (v == "0" || v == "off" || v == "false" || v == "no") c.set(c.tag, 0);
        else return r.name + " takes on or off (or 1 / 0)";
        break;
    }
    case KIND_ENUM: {
        const std::string v = squash(t);
        int idx = -1;
        for (size_t k = 0; k < r.opts.size() && idx < 0; ++k) if (squash(r.opts[k]) == v) idx = (int)k;
        double d;
        if (idx < 0 && parseNumber(t, &d) && d == std::floor(d) && d >= 0 && d < (double)r.opts.size()) idx = (int)d;
        if (idx < 0) return r.name + " takes one of: " + join(r.opts, 0);
        c.set(c.tag, (float)idx);
        break;
    }
    default: {
        double d;
        if (!parseNumber(t, &d)) return r.name + " takes a number from " + num(c.min) + " to " + num(c.max);
        c.set(c.tag, (float)d);
        const float now = c.get(c.tag);
        if (note && std::fabs(now - d) > 1e-4) *note = "clamped to " + num(now);
        break;
    }
    }
    if (note) {
        const std::string a = applyNote(c.apply);
        if (!a.empty()) *note = (note->empty() ? "" : *note + "; ") + a;
    }
    return "";
}

// ---- verbs ----------------------------------------------------------------------------------------------------------------------
size_t commandCount() { init(); return g_cmds.size(); }
CommandInfo commandAt(size_t i) {
    static const CommandInfo none = {"", "", "", 0};
    if (i >= g_cmds.size()) return none;
    return {g_cmds[i].name.c_str(), g_cmds[i].usage.c_str(), g_cmds[i].help.c_str(), g_cmds[i].flags};
}

// ---- completion -------------------------------------------------------------------------------------------------------------------
Completion complete(const std::string& line) {
    init();
    Completion r;
    r.line = line;
    const size_t semi = line.rfind(';');
    const std::string stmt = semi == std::string::npos ? line : line.substr(semi + 1);
    std::vector<std::string> w = words(stmt);
    const bool fresh = stmt.empty() || std::isspace((unsigned char)stmt.back());
    if (fresh) w.push_back("");
    const size_t pos = w.size() - 1;
    const std::string cur = w.back();
    const std::string lcur = lower(cur);

    std::vector<std::string> cand;
    auto addVerbs = [&](bool cvars) {
        size_t n = 0;
        const CmdScript::Verb* vs = CmdScript::verbs(&n);
        for (size_t i = 0; i < n; ++i) cand.push_back(vs[i].name);
        for (const CommandRec& c : g_cmds) cand.push_back(c.name);
        if (cvars) for (const CvarRec& c : g_cvars) cand.push_back(c.name);
    };
    auto addKinds = [&](int kind) {
        switch (kind) {
        case CmdScript::ARG_BLOCK: for (const std::string& s : CmdScript::blockNames()) cand.push_back(s); break;
        case CmdScript::ARG_CREATURE: for (const std::string& s : CmdScript::creatureNames()) cand.push_back(s); break;
        case ARG_CVAR: for (const CvarRec& c : g_cvars) cand.push_back(c.name); break;
        case ARG_VERB: addVerbs(false); break;
        case ARG_KEY:
            for (int code = 1; code < 512; ++code) { const char* l = eden_keybind_code_label(code); if (l && *l) cand.push_back(l); }
            break;
        default: break;
        }
    };
    if (pos == 0) {
        addVerbs(true);
    } else {
        const std::string verb = lower(w[0]);
        if (const CmdScript::Verb* v = CmdScript::findVerb(verb.c_str())) {
            if (pos - 1 < 9 && v->kinds[pos - 1]) addKinds(v->kinds[pos - 1]);
        } else if (const CommandRec* c = findCmdRec(verb)) {
            if (pos - 1 < c->kinds.size()) addKinds(c->kinds[pos - 1]);
        } else if (const CvarRec* v = findCvarRec(verb)) {
            if (pos == 1) {
                if (v->c.kind == KIND_TOGGLE) { cand.push_back("on"); cand.push_back("off"); }
                else if (v->c.kind == KIND_ENUM) for (const std::string& s : v->opts) cand.push_back(s);
                else if (v->lname.compare(0, 5, "keys.") == 0) addKinds(ARG_KEY);
            }
        }
    }
    std::vector<std::string> hit;
    for (const std::string& s : cand)
        if (lower(s).compare(0, lcur.size(), lcur) == 0) hit.push_back(s);
    std::sort(hit.begin(), hit.end());
    hit.erase(std::unique(hit.begin(), hit.end()), hit.end());
    r.matches = hit;
    if (hit.empty()) return r;
    const std::string before = line.substr(0, line.size() - cur.size());
    if (hit.size() == 1) { r.line = before + hit[0] + " "; return r; }
    size_t common = hit[0].size();
    for (const std::string& s : hit) {
        size_t k = 0;
        while (k < common && k < s.size() && std::tolower((unsigned char)s[k]) == std::tolower((unsigned char)hit[0][k])) ++k;
        common = k;
    }
    r.line = before + hit[0].substr(0, std::max(common, cur.size()));
    return r;
}

}  // namespace Console
