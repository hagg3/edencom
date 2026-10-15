#import "NSBundle.h"
#include "platform_shims.h"
#import "NSString.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

// P2: real asset lookup. Xcode's "Copy Bundle Resources" phase adds every media/**/*.png as an
// INDIVIDUAL PBXFileReference (not a folder reference) — that flattens the whole media/ tree to
// the bundle root on-device, so the engine's `@"atlas.png"`/`@"menu_back.png"`/... lookups are
// always plain basenames, never subpaths (grep-confirmed against every `new Texture2D(@"...")`
// call site — none contains a `/`). CMakeLists.txt preloads media/{textures,menu,menu_text,ui,
// icons,ipad_menu} into the virtual FS at /bundle/media, PRESERVING subdirectories (Emscripten's
// --preload-file doesn't flatten), so this does the flattening step Xcode does for free: walk
// /bundle/media once, index every file by basename, and resolve against that index.
// Known gap (not hit by any current call site): 3 basenames collide across subdirs in media/
// (analog_top.png, quit.png, copy.png) — whichever directory is visited first during the walk
// wins. Not worth resolving until a real call site needs the losing one.
static std::unordered_map<std::string, std::string> &EdenBundleIndex() {
    static std::unordered_map<std::string, std::string> index;
    static bool built = false;
    if (!built) {
        built = true;
        // Phase N Stage 1: the root is asked for rather than hard-coded, so a native
        // process can point it at a real asset directory. Default is "/bundle" — the
        // literal that used to be here (see platform_shims.h).
        std::string root = std::string(eden_platform_bundle_root()) + "/media";
        // Iterative stack-based walk — no recursion depth surprises, plain POSIX dirent/stat,
        // works identically under MEMFS (browser) and NODEFS-backed preload (node build-st).
        std::string stack[64];
        int sp = 0;
        stack[sp++] = root;
        while (sp > 0) {
            std::string dir = stack[--sp];
            DIR *d = opendir(dir.c_str());
            if (!d) continue;
            struct dirent *ent;
            while ((ent = readdir(d)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                std::string full = dir + "/" + ent->d_name;
                struct stat st;
                if (stat(full.c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    if (sp < 64) stack[sp++] = full;
                } else if (index.find(ent->d_name) == index.end()) {
                    index[ent->d_name] = full;
                }
            }
            closedir(d);
        }
        // Pass 13: cheap, one-time diagnostic — a browser run showed textured draws succeeding
        // (glErr=0) with a still-black canvas, and the leading hypothesis is that this index
        // came up empty or the preload mount path doesn't match what CMakeLists.txt's
        // --preload-file actually produced. If this prints 0 (or far fewer than the ~1500 files
        // under the 6 preloaded media/ subdirs), the bug is here or in the preload config, not
        // in Texture2D_web.mm's decode path. Printed unconditionally (not just on failure)
        // because "silently correct" and "silently empty" look identical otherwise.
        fprintf(stderr, "NSBundle: indexed %zu files under %s\n", index.size(), root.c_str());
    }
    return index;
}

@implementation NSBundle

+ (NSBundle *)mainBundle {
    static NSBundle *shared = nil;
    if (!shared) shared = [[NSBundle alloc] init];
    return shared;
}

- (NSString *)pathForResource:(NSString *)name ofType:(NSString *)ext {
    std::string fname = [name UTF8String];
    if (ext && [ext length] > 0) {
        fname += ".";
        fname += [ext UTF8String];
    }
    auto &idx = EdenBundleIndex();
    auto it = idx.find(fname);
    if (it != idx.end()) return [NSString stringWithUTF8String:it->second.c_str()];
    // P4: assets that sit at the /bundle ROOT rather than under media/ (the pre-generated default
    // world Eden.eden — CMakeLists preloads it to /bundle/Eden.eden). The index above only walks
    // /bundle/media, so stat /bundle/<name> directly before giving up. FileManagerHelper's
    // fmh_init does exactly `[[NSBundle mainBundle] pathForResource:@"Eden.eden" ofType:nil]`.
    {
        std::string root = std::string(eden_platform_bundle_root()) + "/" + fname;
        struct stat st;
        if (stat(root.c_str(), &st) == 0)
            return [NSString stringWithUTF8String:root.c_str()];
    }
    // Not found (e.g. the ipad~ retina-variant probe, which is expected to miss on most assets)
    // — return a path that resolves to nothing rather than crash; callers check -fileExistsAtPath:
    // or tolerate a decode failure.
    return [NSString stringWithUTF8String:
        (std::string(eden_platform_bundle_root()) + "/media/" + fname).c_str()];
}

- (NSString *)bundlePath { return [NSString stringWithUTF8String:eden_platform_bundle_root()]; }
- (NSString *)resourcePath { return [NSString stringWithUTF8String:eden_platform_bundle_root()]; }

@end
