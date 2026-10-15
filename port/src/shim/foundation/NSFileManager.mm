#import "NSFileManager.h"
#include "platform_shims.h"
#import "NSString.h"
#import "NSArray.h"
#import "NSData.h"
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>   // _mkdir — see eden_mkdir in platform_shims.h (Phase N Stage 3)
#endif
#include <dirent.h>
#include <cstdio>
#include <cstring>   // strcmp — see NSObject.mm (Phase N Stage 3.2)
#include <unistd.h>

@implementation NSFileManager

+ (NSFileManager *)defaultManager {
    static NSFileManager *shared = nil;
    if (!shared) shared = [[NSFileManager alloc] init];
    return shared;
}

- (BOOL)fileExistsAtPath:(NSString *)path {
    struct stat st;
    return stat([path UTF8String], &st) == 0 ? YES : NO;
}

- (BOOL)fileExistsAtPath:(NSString *)path isDirectory:(BOOL *)isDir {
    struct stat st;
    if (stat([path UTF8String], &st) != 0) return NO;
    if (isDir) *isDir = S_ISDIR(st.st_mode) ? YES : NO;
    return YES;
}

- (BOOL)createFileAtPath:(NSString *)path contents:(NSData *)contents attributes:(id)attrs {
    (void)attrs;
    return contents ? [contents writeToFile:path atomically:NO]
                     : [[NSData data] writeToFile:path atomically:NO];
}

- (BOOL)createDirectoryAtPath:(NSString *)path
   withIntermediateDirectories:(BOOL)createIntermediates
                    attributes:(id)attrs error:(id *)error {
    (void)createIntermediates; (void)attrs; (void)error;
    // TODO P1: no recursive mkdir -p — engine call sites observed create only single-level
    // Documents-equivalent dirs; revisit if a deeper path shows up.
    return eden_mkdir([path UTF8String]) == 0 ? YES : NO;
}

- (BOOL)removeItemAtPath:(NSString *)path error:(id *)error {
    (void)error;
    return remove([path UTF8String]) == 0 ? YES : NO;
}

- (BOOL)copyItemAtPath:(NSString *)src toPath:(NSString *)dst error:(id *)error {
    (void)error;
    NSData *d = [NSData dataWithContentsOfFile:src];
    return d ? [d writeToFile:dst atomically:NO] : NO;
}

- (BOOL)moveItemAtPath:(NSString *)src toPath:(NSString *)dst error:(id *)error {
    (void)error;
    return rename([src UTF8String], [dst UTF8String]) == 0 ? YES : NO;
}

- (NSArray *)contentsOfDirectoryAtPath:(NSString *)path error:(id *)error {
    (void)error;
    NSMutableArray *result = [NSMutableArray array];
    DIR *d = opendir([path UTF8String]);
    if (!d) return result;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        [result addObject:[NSString stringWithUTF8String:ent->d_name]];
    }
    closedir(d);
    return result;
}

@end

NSArray *NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory directory,
                                              NSSearchPathDomainMask domainMask,
                                              BOOL expandTilde) {
    (void)directory; (void)domainMask; (void)expandTilde;
    // TODO P4: this is the "Documents directory" lookup FileManager.mm uses to locate saves
    // (docs/save-load.md). Web analogue is an OPFS root, not a real path — P1 placeholder
    // returns a fixed virtual directory so headless construction doesn't crash.
    // Deliberately immortal (+alloc/-init, never released, one per process) and built without
    // the variadic +arrayWithObjects:. Precaution for when P4 un-excludes FileManager.mm: its
    // ctor does `documents = [paths objectAtIndex:0]` with NO retain (engine code, untouchable),
    // so an autoreleased array would leave `documents` dangling after the first pool drain, and
    // a send through a freed isa dies as a wasm "function signature mismatch" rather than a
    // null deref. Not the cause of anything observed so far — nothing drains a pool this early.
    static NSMutableArray *paths = nil;
    if (!paths) {
        paths = [[NSMutableArray alloc] init];
        // Phase N Stage 1: asked for rather than hard-coded, so a native process can
        // point it at a real directory. Default is "/documents" — the literal that
        // used to be here (see platform_shims.h).
        [paths addObject:[[NSString alloc] initWithUTF8String:eden_platform_documents_root()]];
        // P4: the real FileManager writes saves into this dir (saveWorld ->
        // createFileAtPath:@"/documents/<name>"). It must EXIST or every save silently no-ops
        // (fopen fails, and messaging the nil NSFileHandle is inert). This mkdir is a fallback for
        // when public/eden-storage.js's own earlier mkdir (in Module.preRun, before main() ever
        // reaches this) didn't run — headless `node eden.js` (no indexedDB) or a mount failure. In
        // the browser this directory is IDBFS-backed (pass 29): eden-storage.js mounts it with
        // {autoPersist:true} before main() starts, so writes here actually survive a reload — see
        // that file's header and docs/save-load.md. mkdir is idempotent-enough here — ignore EEXIST.
        eden_mkdir(eden_platform_documents_root());
    }
    return paths;
}
