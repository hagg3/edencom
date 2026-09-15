// NSUserDefaults_native.mm — native twin of web/src/shim/foundation/NSUserDefaults.mm
// (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md).
//
// This is the ONE Foundation shim file that genuinely forks between the two targets: the web one
// is write-through to `localStorage` via EM_JS, and there is no localStorage here. Everything else
// under web/src/shim/foundation/ compiles into the native target unchanged.
//
// The store is a single flat text file, one `key<TAB>tagged-value` line per entry, written whole
// on each set. That is the right shape for the traffic: Classes/SettingsMenu.mm's save() is the
// only engine writer (five toggles plus new_world_counter), so a write is a handful of short lines
// a few times per session, and a whole-file rewrite is both atomic-enough via rename and immune to
// the partial-line corruption an append log would have.
//
// Location follows the platform convention rather than the engine's Documents directory, because
// these are preferences, not user content:
//   macOS   ~/Library/Application Support/Emod/prefs
//   Linux   $XDG_CONFIG_HOME/eden/prefs, else ~/.config/eden/prefs
//   Windows %APPDATA%\Emod\prefs                                  (Phase N Stage 3.2)
//
// VALUE ENCODING IS IDENTICAL TO THE WEB TWIN'S — "n:<int>" for NSNumber, "s:<utf8>" for NSString,
// unknown tags read back as nil rather than guessing. Keeping the two encodings the same is not
// cosmetic: it is what would let a future stage move a preference file between targets, and it
// keeps the two files diffable, which is how this one stays correct.
//
// NON-POD IVARS ARE NOT SAFE IN THIS PORT — the hand-written ObjC runtime's class_createInstance()
// is a bare calloc with no .cxx_construct/.cxx_destruct, so a std::unordered_map ivar is never
// constructed (a zeroed one has max_load_factor 0.0 and aborts on first insert). The map hangs off
// an explicitly-new'd pointer for exactly that reason; read the long note at the top of the web
// twin before adding an ivar here. Same hazard, same shape, same two files in the tree.

#import "NSUserDefaults.h"
#import "NSString.h"
#import "NSNumber.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <SDL3/SDL.h>
#include "../../eden_app_identity.h"   // Emod is not Eden — see that file

namespace {

// Creates every missing component of `path`. This was a hand-rolled mkdir(2) walk, because the
// leaf's parent ("~/Library/Application Support") exists on macOS but "$HOME/.config" often does
// not on Linux. Phase N Stage 3.2 replaced it with SDL's, which already creates parents and
// already knows that mingw's mkdir() takes one argument where POSIX's takes two — a difference
// that is a compile error on Windows, not a portability footnote.
void eden_prefs_mkdirs(const std::string& path) {
    SDL_CreateDirectory(path.c_str());
}

const std::string& eden_prefs_path() {
    static std::string path = [] {
        std::string dir;
#if defined(_WIN32)
        // %APPDATA% is the roaming per-user config root and is always set for an interactive
        // session. Deliberately NOT SDL_GetPrefPath(), which would append a second "Emod"
        // component and put the prefs under the same tree as the SAVES — these are preferences,
        // not user content, and keeping them apart is the point of this file's location rule.
        const char* appdata = getenv("APPDATA");
        if (!appdata || !*appdata) return std::string();   // no profile: run memory-only, below
        dir = std::string(appdata) + "\\" EDEN_APP_DIR_NAME;
#else
        const char* home = getenv("HOME");
        if (!home || !*home) return std::string();   // no home: run memory-only, see below
#if defined(__APPLE__)
        dir = std::string(home) + "/Library/Application Support/" EDEN_APP_DIR_NAME;
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        dir = (xdg && *xdg) ? std::string(xdg) + "/" EDEN_APP_DIR_LOWER
                            : std::string(home) + "/.config/" EDEN_APP_DIR_LOWER;
#endif
#endif
        eden_prefs_mkdirs(dir);
        return dir + "/prefs";
    }();
    return path;
}

// Read once, lazily. A missing file is not an error — it is a first run.
std::unordered_map<std::string, std::string>& eden_prefs_disk() {
    static std::unordered_map<std::string, std::string> table = [] {
        std::unordered_map<std::string, std::string> t;
        const std::string& p = eden_prefs_path();
        if (p.empty()) return t;
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return t;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char* tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            std::string value(tab + 1);
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
            t[line] = value;
        }
        fclose(f);
        return t;
    }();
    return table;
}

// Whole-file rewrite through a scratch file + rename, so a crash mid-write leaves the previous
// preferences intact rather than a truncated one. Same reasoning as the engine's own save path
// below its in-place threshold (docs/save-load.md) — this file is small enough that the simple
// version is also the correct one.
void eden_prefs_flush() {
    const std::string& p = eden_prefs_path();
    if (p.empty()) return;
    std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    for (const auto& kv : eden_prefs_disk()) {
        fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
    }
    fclose(f);
    // Windows' rename(2) fails if the destination exists, unlike POSIX's — so remove first. The
    // window this opens (crash between remove and rename) costs the prefs file, not user content,
    // and the alternative (MoveFileEx) would mean pulling in windows.h for one call.
#if defined(_WIN32)
    remove(p.c_str());
#endif
    // A failed rename leaves the old file in place, which is the safe direction; a leftover .tmp
    // is harmless and is overwritten next time.
    rename(tmp.c_str(), p.c_str());
}

// Mirrors the web twin's eden_prefs_available(): when there is nowhere to persist, everything
// still works in memory for the session rather than failing. (There, it is a private-browsing
// localStorage; here it is a process with no HOME, e.g. some CI sandboxes.)
bool eden_prefs_available() { return !eden_prefs_path().empty(); }

}  // namespace

@implementation NSUserDefaults {
    std::unordered_map<std::string, id> *_store;   // calloc'd to null; allocated by -init
}

+ (NSUserDefaults *)standardUserDefaults {
    static NSUserDefaults *shared = nil;
    if (!shared) shared = [[NSUserDefaults alloc] init];
    return shared;
}

- (id)init {
    self = [super init];
    if (self) {
        _store = new std::unordered_map<std::string, id>();
    }
    return self;
}

- (id)objectForKey:(NSString *)key {
    if (!key) return nil;
    std::string k = [key UTF8String];
    auto it = _store->find(k);
    if (it != _store->end()) return it->second;

    if (!eden_prefs_available()) return nil;
    auto& disk = eden_prefs_disk();
    auto d = disk.find(k);
    if (d == disk.end()) return nil;
    const std::string& raw = d->second;
    id value = nil;
    if (raw.size() >= 2 && raw[0] == 'n' && raw[1] == ':')      value = [NSNumber numberWithInt:atoi(raw.c_str() + 2)];
    else if (raw.size() >= 2 && raw[0] == 's' && raw[1] == ':') value = [NSString stringWithUTF8String:raw.c_str() + 2];
    if (!value) return nil;          // unknown tag: treat as absent, don't guess
    [value retain];                  // the map owns a reference, same as -setObject:forKey:
    (*_store)[k] = value;
    return value;
}

- (void)setObject:(id)value forKey:(NSString *)key {
    if (!key) return;
    if (value) [value retain];
    id old = [self objectForKey:key];
    std::string k = [key UTF8String];
    (*_store)[k] = value;
    if (old) [old release];

    if (!value || !eden_prefs_available()) return;
    // Type tag chosen by what the value responds to, NSNumber first — every engine write is one.
    if ([value isKindOfClass:[NSNumber class]]) {
        char buf[32];
        snprintf(buf, sizeof(buf), "n:%d", [(NSNumber *)value intValue]);
        eden_prefs_disk()[k] = buf;
        eden_prefs_flush();
    } else if ([value isKindOfClass:[NSString class]]) {
        std::string s = "s:";
        s += [(NSString *)value UTF8String];
        eden_prefs_disk()[k] = s;
        eden_prefs_flush();
    }
}

- (NSInteger)integerForKey:(NSString *)key {
    id v = [self objectForKey:key];
    return v ? (NSInteger)[(NSNumber *)v intValue] : 0;
}

- (void)setInteger:(NSInteger)value forKey:(NSString *)key {
    [self setObject:[NSNumber numberWithInt:(int)value] forKey:key];
}

- (BOOL)boolForKey:(NSString *)key {
    id v = [self objectForKey:key];
    return v ? [(NSNumber *)v boolValue] : NO;
}

- (void)setBool:(BOOL)value forKey:(NSString *)key {
    [self setObject:[NSNumber numberWithBool:value] forKey:key];
}

- (NSString *)stringForKey:(NSString *)key {
    return (NSString *)[self objectForKey:key];
}

// Every -setObject: already wrote through, so there is nothing to flush. Returns YES for the same
// reason as the web twin: Classes/SettingsMenu.mm calls it after each save.
- (BOOL)synchronize {
    return YES;
}

@end
