// save_backup.cpp — see save_backup.h for what this is and why it is a shared file rather than a
// method on the shim's NSFileHandle (where it used to live, and still runs from on web).
//
// Portable C++ on purpose: stdio and one constant from Classes/Constants.h, no Foundation, no
// Emscripten, no Apple frameworks. The Linux leg gets it for free.

#include "save_backup.h"
#include "platform_shims.h"   // eden_fseek64/eden_ftell64 — see there for why not fseeko

#include "../../../../Classes/Constants.h"   // g_save_inplace_threshold

#include <cstdio>
#include <string>
#include <sys/types.h>

extern "C" void eden_save_backup_before_overwrite(const char* path) {
    if (!path || !*path) return;

    FILE* src = std::fopen(path, "rb");
    if (!src) return;                      // nothing to back up yet (first save of this world)

    if (eden_fseek64(src, 0, SEEK_END) != 0) { std::fclose(src); return; }
    const long long existing = eden_ftell64(src);

    // Above the in-place threshold there is nothing to duplicate onto: the disk cannot hold 2x a
    // multi-gigabyte world, and the rollback journal saveWorld writes covers the failure this slot
    // was protecting against. Below it, unchanged.
    if (existing >= 0 && (unsigned long long)existing >= g_save_inplace_threshold) {
        std::fclose(src);
        return;
    }
    // A zero-length file has nothing to back up, and backing it up is worse than skipping: it
    // leaves a 0-byte "<path>.bak" that eden_load_restore_backup() would offer as a restorable
    // previous save.
    if (existing <= 0) { std::fclose(src); return; }

    if (eden_fseek64(src, 0, SEEK_SET) != 0) { std::fclose(src); return; }

    const std::string bak = std::string(path) + ".bak";
    FILE* dst = std::fopen(bak.c_str(), "wb");
    if (!dst) { std::fclose(src); return; }

    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof(buf), src)) > 0) {
        if (std::fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    if (std::ferror(src)) ok = false;
    std::fclose(src);
    if (std::fclose(dst) != 0) ok = false;   // a write can still fail at close (buffered tail)
    if (!ok) std::remove(bak.c_str());        // never leave a truncated/misleading backup behind
}
