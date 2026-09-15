#import "NSUserDefaults.h"
#import "NSString.h"
#import "NSNumber.h"
#include <emscripten/emscripten.h>
#include <unordered_map>
#include <string>
#include <cstdlib>
#include <cstdio>

// PERSISTENCE (pass 28). Backed by localStorage, one entry per key under an "eden.prefs."
// namespace. This is the P7 TODO in the header, done early because the settings menu is not worth
// much if it forgets everything on reload: `SettingsMenu::save()` is the ONLY writer in the engine
// (Classes/SettingsMenu.mm — its five toggles plus `new_world_counter`; Appirater is the only other
// caller and is seam-excluded), and the port's own preferences are routed through the same store so
// there is one persistence story rather than two.
//
// Scope is deliberately narrow: NSNumber (stored as a decimal string, tagged "n:") and NSString
// (tagged "s:"). Nothing in the engine stores anything else here — grep `forKey:@"` under Classes/
// before widening it. An unknown/legacy value reads back as nil rather than guessing a type.
//
// Values live in the in-memory map exactly as before; localStorage is a write-through cache read
// once at first access. That keeps `objectForKey:` allocation-free on the hot path and means the
// whole thing degrades to the previous in-memory behaviour when there is no DOM — which is the
// `node eden.js` case, and the reason every JS call below is guarded (see PORT-STATUS's note on
// `eden_audio_js_init`: anything touching `window`/`document` unconditionally throws under node).
EM_JS(int, eden_prefs_available, (), {
    // `typeof` alone is not enough: node 22 DEFINES localStorage but throws on first touch unless
    // --localstorage-file was passed, and Safari throws in some private-browsing configurations.
    // Probe it for real.
    try { return (typeof localStorage !== 'undefined' && localStorage.length >= 0) ? 1 : 0; }
    catch (e) { return 0; }
});

// Returns a malloc'd C string the caller frees, or 0 when the key is absent. (_malloc/stringToUTF8
// are the standard emscripten JS->C string hand-off; nothing here needs EXPORTED_RUNTIME_METHODS
// because both are used from inside the JS glue itself.)
EM_JS(char*, eden_prefs_get, (const char* keyC), {
    if (typeof localStorage === 'undefined') return 0;
    var v = null;
    try { v = localStorage.getItem('eden.prefs.' + UTF8ToString(keyC)); } catch (e) { return 0; }
    if (v === null) return 0;
    var len = lengthBytesUTF8(v) + 1;
    var p = _malloc(len);
    stringToUTF8(v, p, len);
    return p;
});

EM_JS(void, eden_prefs_set, (const char* keyC, const char* valC), {
    if (typeof localStorage === 'undefined') return;
    // Quota/private-mode failures must not take the game down — a preference that does not stick
    // is a far smaller problem than an abort in the middle of SettingsMenu::save().
    try { localStorage.setItem('eden.prefs.' + UTF8ToString(keyC), UTF8ToString(valC)); } catch (e) {}
});

// NON-POD IVARS ARE NOT SAFE IN THIS PORT. src/shim/objc/objc_runtime.cpp's
// class_createInstance() is `calloc(1, instance_size)` and there is no `.cxx_construct` /
// `.cxx_destruct` support anywhere in the hand-written runtime — a C++ ivar's constructor NEVER
// runs and its destructor NEVER runs. For `std::unordered_map` the all-zero state is not a valid
// empty map: `__max_load_factor_` reads back as 0.0f, so the first insert computes a bucket count
// of `ceil((size+1) / 0.0f)` = +inf, and libc++'s `__next_prime()` throws — which, with exceptions
// off, is a bare `abort()`. Measured, not reasoned: an `fprintf` probe at the first
// -setObject:forKey: reports `bucket_count=0 mlf=0.000000` in EVERY build, Debug and Release.
//
// This is why `-flto` "broke" the Release build (audit row B1 follow-up): LTO's cross-TU inlining
// lets the compiler constant-fold that division and commit to the throw path, whereas the non-LTO
// -O2 build happened to keep limping. The UB was always there; LTO only made it fatal. Anything
// added here — or to any other @implementation in src/ — must hold POD ivars only, with real C++
// objects hung off an explicitly-allocated pointer like `_store` below. NSAutoreleasePool.mm has
// the same note and the same shape, and they are the only two instances in the tree.
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
    char *raw = eden_prefs_get(k.c_str());
    if (!raw) return nil;
    id value = nil;
    if (raw[0] == 'n' && raw[1] == ':')      value = [NSNumber numberWithInt:atoi(raw + 2)];
    else if (raw[0] == 's' && raw[1] == ':') value = [NSString stringWithUTF8String:raw + 2];
    std::free(raw);
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
    // Type tag chosen by what the value actually responds to. NSNumber is checked first because
    // every engine write is one (Classes/SettingsMenu.mm); NSString is here for the port's own
    // keys. Anything else is kept in memory but not persisted, which is the safe direction.
    if ([value isKindOfClass:[NSNumber class]]) {
        char buf[32];
        snprintf(buf, sizeof(buf), "n:%d", [(NSNumber *)value intValue]);
        eden_prefs_set(k.c_str(), buf);
    } else if ([value isKindOfClass:[NSString class]]) {
        std::string s = "s:";
        s += [(NSString *)value UTF8String];
        eden_prefs_set(k.c_str(), s.c_str());
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

// Every -setObject: already wrote through, so there is nothing to flush. Kept returning YES
// because Classes/SettingsMenu.mm calls it after each save and ignores nothing else.
- (BOOL)synchronize {
    return YES;
}

@end
