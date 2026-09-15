// LoadFailure_web.mm — corrupt/truncated-save recovery signal (perf-audit C4, the second still-
// open piece from `.bak` era: "a load-failure recovery UI needs a corrupt-file signal from the
// load path, not the write path"). Now that Classes/ is editable (2026-07-25), FileManager.mm's
// loadWorld() sanity-checks a save's header and directory before trusting it (a truncated header
// read, or a directory_offset outside the file's real size -- both are exactly what a save
// interrupted mid-write looks like, the failure mode the .bak backup slot and saveWorld()'s new
// temp+rename atomicity exist to prevent) and calls eden_report_load_failure() below instead of
// reading garbage. This is a plain cross-TU C++ call declared in LoadFailure_web.h and included
// directly by FileManager.mm -- NOT a --wrap, since there is no existing call site to intercept,
// just a brand-new one.
//
// public/eden-loaderror.js polls eden_load_failed() once per frame tick (same shape as the pause
// menu's eden_hud_in_menu() poll) and, when it flips true, shows a DOM "this world could not be
// loaded" dialog offering to restore the `.bak` backup slot NSFileHandle.mm's
// +fileHandleForWritingAtPath: maintains (eden_load_restore_backup() below) and retry.
#import "LoadFailure_web.h"
#import "../shim/foundation/NSFileManager.h"
#include "../shim/foundation/platform_shims.h"
#import "../shim/foundation/NSString.h"
#include <cstdio>
#include <string>

static bool g_failed = false;
static std::string g_world_file; // on-disk file name (e.g. "MyWorld.eden"), not the display name
static std::string g_reason;

void eden_report_load_failure(const char* world_file_name, const char* reason) {
    g_failed = true;
    g_world_file = world_file_name ? world_file_name : "";
    g_reason = reason ? reason : "unknown";
    fprintf(stderr, "[eden-load] load failed for '%s': %s\n", g_world_file.c_str(), g_reason.c_str());
}

extern "C" {

EDEN_EXPORT
int eden_load_failed(void) { return g_failed ? 1 : 0; }

// Static-buffer convention (same as Settings_web.mm's eden_settings_schema): no _malloc/_free on
// the export list, so JS reads these with UTF8ToString right after the eden_load_failed() poll.
EDEN_EXPORT
const char* eden_load_failed_world(void) { return g_world_file.c_str(); }

EDEN_EXPORT
const char* eden_load_failed_reason(void) { return g_reason.c_str(); }

EDEN_EXPORT
void eden_load_failed_clear(void) { g_failed = false; g_world_file.clear(); g_reason.clear(); }

// Restores a backup over <world> so a subsequent retry of the menu's load flow picks up the last
// known-good save instead of the corrupt one. Resolves Documents the same fixed way
// Storage_web.mm's scan does when it has no live World to ask -- a load failure can happen before
// World::getWorld->fm is in a trustworthy state, so this only needs the fixed /documents root
// (NSSearchPathForDirectoriesInDomains's own placeholder, NSFileManager.mm).
//
// Two backup slots exist and only one of them is still live. `<world>.bak` was pass 35's slot,
// written by NSFileHandle.mm's +fileHandleForWritingAtPath: whenever it opened `file_name` itself
// for writing over existing content -- but pass 37's atomic-rename saveWorld() (FileManager.mm)
// never opens `file_name` for writing anymore, only `file_name.savetmp` (removed then rebuilt
// each save, then swapped in via remove+rename). That save path's own copyItemAtPath: seed of the
// scratch file (raw ofstream, no backup hook) is immediately followed by opening THAT file via
// fileHandleForUpdatingAtPath: -- which DOES exist already at that point (the copy just created
// it) -- so the write-guard fires there instead, producing `file_name.savetmp.bak`: a genuine,
// fresh backup of the pre-save content, just under the OTHER name. Found live (2026-07-26): a
// world saved since pass 37 has no `.bak` at all, only `.savetmp.bak`, so the old lookup silently
// reported "no backup available" despite a perfectly good one sitting right next to it. Prefer
// `.bak` when both exist (it's the older, occasionally more-conservative copy under the pre-37
// scheme); fall back to `.savetmp.bak` otherwise.
EDEN_EXPORT
int eden_load_restore_backup(void) {
    if (g_world_file.empty()) return 0;
    // Phase N Stage 1: the documents root is asked for, not hard-coded (see
    // platform_shims.h). On web this still resolves to "/documents".
    NSString* target = [NSString stringWithFormat:@"%s/%s",
                        eden_platform_documents_root(), g_world_file.c_str()];
    NSString* backup = [target stringByAppendingString:@".bak"];
    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:backup]) {
        backup = [target stringByAppendingString:@".savetmp.bak"];
        if (![fm fileExistsAtPath:backup]) return 0;
    }
    [fm removeItemAtPath:target error:NULL];
    BOOL ok = [fm copyItemAtPath:backup toPath:target error:NULL];
    if (ok) eden_load_failed_clear();
    return ok ? 1 : 0;
}

}
