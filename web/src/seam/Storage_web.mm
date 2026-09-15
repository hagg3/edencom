// Storage_web.mm — local-storage management panel data source (pass 29).
//
// Backs the Settings panel's "Storage" tab (public/eden-settings.js). Two exports:
//   * eden_storage_list_worlds() — JSON array of every world file in Documents, with the real
//     display name (reused from FileManager::getName, the exact lookup Menu::loadWorlds uses —
//     see docs/save-load.md) plus size/mtime from stat() and each world's real height (64 or 256,
//     via FileManager::probeWorldHeight -- added so eden-menu.js's Load World list stops
//     hardcoding "64z" now that 256z worlds can exist locally, and so the panel can warn about
//     the ~4x memory a 256z world costs once loaded, per archive/project-audit-2026-07-30.md's pass-67
//     follow-up item).
//   * eden_storage_delete_world_at(index) — INDEX into that same list, not a filename string, same
//     convention as Settings_web.mm's eden_settings_set(i, v): passing a C string in from JS would
//     need _malloc/_free added to the export list, and the JS already has the index from the row
//     it just rendered. Re-derives the identical directory scan (shared helper below) rather than
//     caching the previous list, so it can never delete the wrong file if Documents changed
//     between the two calls. Reuses FileManager::deleteWorld verbatim (removes the .eden and its
//     .png), then re-runs Menu::loadWorlds() so the in-game world picker cannot show a stale entry
//     if the panel is opened mid-session. Persistence to IndexedDB is automatic: the IDBFS mount in
//     public/eden-storage.js uses {autoPersist:true}, which queues a sync on any file
//     close-after-write, mkdir, unlink or rename under /documents (see library_idbfs.js) — no extra
//     flush call needed here.
//
// Deliberately reads the SAME directory (World::getWorld->fm->documents) and applies the SAME
// filters Menu::loadWorlds does (Classes/Menu.mm:269-282: skip non-.eden, skip the PNG previews,
// skip "Eden.eden.archive") so the panel's list never disagrees with the in-game picker.
#import "../shim/foundation/NSFileManager.h"
#import "../shim/foundation/NSString.h"
#import "../shim/foundation/NSArray.h"
#import "../../../Classes/World.h"
#import "../../../Classes/FileManager.h"
#import "../../../Classes/Menu.h"
#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)
#include <sys/stat.h>
#include <cstdio>
#include <string>

static void jsonEscape(std::string& out, const char* s) {
    out += '"';
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
            switch (*p) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (*p < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", *p); out += b; }
                    else out += (char)*p;
            }
        }
    }
    out += '"';
}

// Shared scan: every filename in Documents that passes Menu::loadWorlds' own filter, in
// contentsOfDirectoryAtPath's enumeration order. Both exports below walk this so "row N in the
// list" and "index N to delete" can never disagree about what N means.
static NSArray* eden_storage_scan(NSString** outDocuments) {
    if (!World::getWorld || !World::getWorld->fm) return nil;
    NSString* documents = World::getWorld->fm->documents;
    if (outDocuments) *outDocuments = documents;
    return [[NSFileManager defaultManager] contentsOfDirectoryAtPath:documents error:NULL];
}

static BOOL eden_storage_is_world_file(NSString* file_name) {
    if ([file_name isEqualToString:@"Eden.eden.archive"]) return NO;
    NSString* ext = [[file_name pathExtension] uppercaseString];
    return [ext isEqualToString:@"EDEN"];
}

extern "C" {

// One JSON array string, reused across calls (same static-buffer convention as
// Settings_web.mm's eden_settings_schema — no _malloc/_free on the export list).
EDEN_EXPORT
const char* eden_storage_list_worlds(void) {
    static std::string buf;
    buf = "[";
    bool first = true;

    NSString* documents = nil;
    NSArray* names = eden_storage_scan(&documents);
    FileManager* fm = World::getWorld ? World::getWorld->fm : NULL;
    int n = (int)[names count];
    for (int i = 0; i < n; ++i) {
        NSString* file_name = [names objectAtIndex:i];
        if (!eden_storage_is_world_file(file_name)) continue;

        NSString* real_name = fm->getName(file_name);
        const char* nameC = real_name ? [real_name UTF8String] : "Unknown World";

        NSString* full = [NSString stringWithFormat:@"%@/%@", documents, file_name];
        long long bytes = 0, mtimeMs = 0;
        struct stat st;
        if (stat([full UTF8String], &st) == 0) {
            bytes = (long long)st.st_size;
            mtimeMs = (long long)st.st_mtime * 1000LL;
        }
        // 256z ("New Dawn") worlds cost ~4x the resident memory of a 64z world once loaded (the
        // per-world arrays scale with T_HEIGHT) -- surfaced here so the JS layer can label/warn
        // about them before the player hits the load-time memory ceiling, not after. Reuses the
        // same header peek World::loadWorld already does before allocateMemory(); a fromArchive
        // value doesn't matter here (see probeWorldHeight's own header -- it ignores the flag
        // except for an existence check we've already passed via the directory scan).
        int height = fm->probeWorldHeight(file_name, FALSE);

        if (!first) buf += ",";
        first = false;
        buf += "{\"file\":";
        jsonEscape(buf, [file_name UTF8String]);
        buf += ",\"name\":";
        jsonEscape(buf, nameC);
        char nbuf[80];
        snprintf(nbuf, sizeof(nbuf), ",\"bytes\":%lld,\"mtime\":%lld,\"height\":%d}", bytes, mtimeMs, height);
        buf += nbuf;
    }
    buf += "]";
    return buf.c_str();
}

// Returns 1 on success, 0 if the index no longer resolves to a world file (list changed under us,
// or there was no world/FileManager yet) or the delete itself failed (FileManager::deleteWorld's
// own BOOL contract).
EDEN_EXPORT
int eden_storage_delete_world_at(int index) {
    if (index < 0 || !World::getWorld || !World::getWorld->fm) return 0;
    NSArray* names = eden_storage_scan(NULL);
    int seen = 0;
    NSString* target = nil;
    int n = (int)[names count];
    for (int i = 0; i < n; ++i) {
        NSString* file_name = [names objectAtIndex:i];
        if (!eden_storage_is_world_file(file_name)) continue;
        if (seen == index) { target = file_name; break; }
        seen++;
    }
    if (!target) return 0;

    BOOL ok = World::getWorld->fm->deleteWorld(target);
    // loadWorlds() rebuilds Menu's world_list from disk from scratch; it is what the menu itself
    // calls on construction (Menu::Menu -> loadWorlds), so re-running it is exactly "the picker as
    // if the app had just started", not a bespoke removal path.
    if (ok && World::getWorld->menu) World::getWorld->menu->loadWorlds();
    return ok ? 1 : 0;
}

// Quick-and-dirty test hook (pass 35): the Storage tab's "Import .eden file" button writes the
// picked file straight into Documents via `FS.writeFile` from JS (no wasm call needed for that
// part — IDBFS's autoPersist already hooks writes under /documents same as any real save) and
// then calls this so Menu's in-memory world_list picks it up without a full page reload. Same
// idea as eden_storage_delete_world_at's re-scan: Menu::loadWorlds() is exactly what the menu
// itself runs on construction, so re-running it is "as if the app had just started."
EDEN_EXPORT
void eden_storage_reload_worlds(void) {
    if (World::getWorld && World::getWorld->menu) World::getWorld->menu->loadWorlds();
}

// 256z Stage 3 item 5: "Convert to 64z" (space reclaim). INDEX into the same scan as the two
// exports above, same convention. Runs FileManager::convertWorldTo64() (Classes/FileManager.mm —
// a from-scratch port of web/tools/eden-convert.js's --to-64 direction over NSFileHandle instead
// of Node's fs) and returns one JSON object describing what happened, so the Storage tab can show
// the same kind of report the CLI tool prints. Re-runs Menu::loadWorlds() on success for the same
// reason eden_storage_delete_world_at does: the in-game picker must not show a stale height.
EDEN_EXPORT
const char* eden_storage_convert_to_64z_at(int index) {
    static std::string buf;
    buf = "{\"ok\":false,\"error\":\"no such world\"}";
    if (index < 0 || !World::getWorld || !World::getWorld->fm) return buf.c_str();
    NSArray* names = eden_storage_scan(NULL);
    int seen = 0;
    NSString* target = nil;
    int n = (int)[names count];
    for (int i = 0; i < n; ++i) {
        NSString* file_name = [names objectAtIndex:i];
        if (!eden_storage_is_world_file(file_name)) continue;
        if (seen == index) { target = file_name; break; }
        seen++;
    }
    if (!target) return buf.c_str();

    ConvertTo64Report r = World::getWorld->fm->convertWorldTo64(target);
    if (r.ok && World::getWorld->menu) World::getWorld->menu->loadWorlds();

    buf = "{\"ok\":";
    buf += r.ok ? "true" : "false";
    buf += ",\"error\":";
    jsonEscape(buf, r.error);
    char nbuf[256];
    snprintf(nbuf, sizeof(nbuf),
             ",\"columns\":%d,\"blocksDiscarded\":%d,\"columnsAffected\":%d,\"doorsOrphaned\":%d,"
             "\"creaturesDropped\":%d,\"creaturesRelocated\":%d,\"creaturesOverflow\":%d,"
             "\"posClamped\":%s,\"homeClamped\":%s}",
             r.columns, r.blocksDiscarded, r.columnsAffected, r.doorsOrphaned,
             r.creaturesDropped, r.creaturesRelocated, r.creaturesOverflow,
             r.posClamped ? "true" : "false", r.homeClamped ? "true" : "false");
    buf += nbuf;
    return buf.c_str();
}

}  // extern "C"
