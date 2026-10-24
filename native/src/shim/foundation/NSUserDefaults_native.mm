// NSUserDefaults_native.mm — native twin of web-port-final:web/src/shim/foundation/NSUserDefaults.mm
// (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md).
//
// This is the ONE Foundation shim file that genuinely forks between the two targets: the web one
// is write-through to `localStorage` via EM_JS, and there is no localStorage here. Everything else
// under port/src/shim/foundation/ compiles into the native target unchanged.
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

#include <string>
#include <unordered_map>
#include "../../../../port/src/shim/foundation/eden_prefs.h"   // G.2b-0: the store itself

// G.2b-0: the flat-file store moved to native/src/seam/Prefs_native.cpp (C API in eden_prefs.h, all
// targets). This file is now only the ObjC wrapper over it, kept until G.3 deletes the Foundation shim.

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

    id value = nil;
    switch (eden_prefs_type(k.c_str())) {
        case 1: value = [NSNumber numberWithInt:eden_prefs_get_int(k.c_str(), 0)]; break;
        case 2: value = [NSString stringWithUTF8String:eden_prefs_get_str(k.c_str())]; break;
    }
    if (!value) return nil;          // absent or unknown tag: treat as absent, don't guess
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

    if (!value) return;
    // Type tag chosen by what the value responds to, NSNumber first — every engine write is one.
    if ([value isKindOfClass:[NSNumber class]])
        eden_prefs_set_int(k.c_str(), [(NSNumber *)value intValue]);
    else if ([value isKindOfClass:[NSString class]])
        eden_prefs_set_str(k.c_str(), [(NSString *)value UTF8String]);
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
    eden_prefs_sync();
    return YES;
}

@end
