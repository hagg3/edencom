// Prefs_native.cpp — G.2b-0 (WORKING/de-objc-plan-2026-10-24.md D7). The flat-file preferences store
// that used to live inside NSUserDefaults_native.mm, behind the C API in eden_prefs.h and compiled on
// EVERY target. See that header for the file format and locations.
//
// Changed in the move: the reader takes lines of any length (it was an fgets into 1024 bytes, which
// split a long console-history value and dropped its tail). Unchanged: the whole-file rewrite through
// scratch + rename, the memory-only fallback when there is no home, the location rule.
#include "../../../port/src/shim/foundation/eden_prefs.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <SDL3/SDL.h>
#include "../eden_app_identity.h"   // Emod is not Eden: see that file

namespace {

bool g_ephemeral = false;
bool g_loaded = false;
bool g_path_overridden = false;
std::string g_path_override;
std::map<std::string, std::string> g_table;   // key -> "n:<int>" | "s:<utf8>"

const std::string& platform_path() {
    static std::string path = [] {
        std::string dir;
#if defined(_WIN32)
        // %APPDATA%, deliberately NOT SDL_GetPrefPath(), which appends a second "Emod" and would put
        // preferences under the saves tree.
        const char* appdata = getenv("APPDATA");
        if (!appdata || !*appdata) return std::string();
        dir = std::string(appdata) + "\\" EDEN_APP_DIR_NAME;
#else
        const char* home = getenv("HOME");
        if (!home || !*home) return std::string();
#if defined(__APPLE__)
        dir = std::string(home) + "/Library/Application Support/" EDEN_APP_DIR_NAME;
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        dir = (xdg && *xdg) ? std::string(xdg) + "/" EDEN_APP_DIR_LOWER
                            : std::string(home) + "/.config/" EDEN_APP_DIR_LOWER;
#endif
#endif
        SDL_CreateDirectory(dir.c_str());   // creates parents; knows mingw's one-argument mkdir
        return dir + "/prefs";
    }();
    return path;
}

std::string current_path() { return g_path_overridden ? g_path_override : platform_path(); }

void load() {
    if (g_loaded) return;
    g_loaded = true;
    const std::string p = current_path();
    if (p.empty() || g_ephemeral) return;
    std::ifstream f(p, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string value = line.substr(tab + 1);
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        g_table[line.substr(0, tab)] = value;
    }
}

// Whole-file rewrite through a scratch file + rename: a crash mid-write leaves the previous file.
void flush() {
    const std::string p = current_path();
    if (p.empty() || g_ephemeral) return;
    std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    for (const auto& kv : g_table) {
        fwrite(kv.first.data(), 1, kv.first.size(), f);
        fputc('\t', f);
        fwrite(kv.second.data(), 1, kv.second.size(), f);
        fputc('\n', f);
    }
    fclose(f);
#if defined(_WIN32)
    remove(p.c_str());   // Windows' rename fails onto an existing file
#endif
    rename(tmp.c_str(), p.c_str());   // a failed rename leaves the old file: the safe direction
}

}  // namespace

extern "C" {

int eden_prefs_type(const char* key) {
    if (!key) return 0;
    load();
    auto it = g_table.find(key);
    if (it == g_table.end() || it->second.size() < 2 || it->second[1] != ':') return 0;
    return it->second[0] == 'n' ? 1 : it->second[0] == 's' ? 2 : 0;   // unknown tag: absent, don't guess
}

int eden_prefs_has(const char* key) { return eden_prefs_type(key) != 0; }

int eden_prefs_get_int(const char* key, int fallback) {
    if (eden_prefs_type(key) != 1) return fallback;
    return atoi(g_table[key].c_str() + 2);
}

void eden_prefs_set_int(const char* key, int value) {
    if (!key) return;
    load();
    char buf[32];
    snprintf(buf, sizeof(buf), "n:%d", value);
    g_table[key] = buf;
    flush();
}

const char* eden_prefs_get_str(const char* key) {
    if (eden_prefs_type(key) != 2) return nullptr;
    return g_table[key].c_str() + 2;
}

void eden_prefs_set_str(const char* key, const char* value) {
    if (!key || !value) return;
    load();
    g_table[key] = std::string("s:") + value;
    flush();
}

void eden_prefs_sync(void) {}   // every set already wrote through

// X.21 on every target since G.2b-1 (Apple had an NSUserDefaults suite swizzle, PrefsEphemeral_apple.mm).
void eden_prefs_set_ephemeral(int on) { g_ephemeral = (on != 0); }

void eden_prefs_reset_for_test(const char* path) {
    g_table.clear();
    g_loaded = false;
    if (path) g_ephemeral = false;   // a test that names a file wants it written (every --mode run is ephemeral)
    g_path_overridden = (path != nullptr);
    g_path_override = path ? path : "";
}

}  // extern "C"
