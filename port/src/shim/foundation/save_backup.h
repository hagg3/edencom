// save_backup.h — "copy the last-known-good save aside before it is overwritten", as one portable
// function shared by both targets.
//
// It started life inside this shim's NSFileHandle (perf-audit C4, "no backup slot"), which is
// where it belongs on web: the shim OWNS every file open, so the guard sits at the one choke point
// and needs no engine cooperation. Native has no such choke point — it links Apple's real
// Foundation, whose NSFileHandle is a class cluster we do not implement and should not swizzle
// (`-writeData:` lives on a private concrete subclass, so the deferred-until-first-write behaviour
// the web guard depends on cannot be reproduced by swizzling NSFileHandle itself). So on native
// the same rules are hung off an explicit engine hook at the top of FileManager::saveWorld
// instead, and this file is what both call.
//
// The rules are the shim's, unchanged, and each one is there because of a specific failure:
//   * the "save_backup" setting (kSettings[] in Settings_web.mm, default ON) is off -> skip. An
//     explicit opt-out, not a new failure mode -- but note eden_load_restore_backup() (the
//     corrupted-load recovery prompt) has nothing to offer once this is off;
//   * no source file yet -> nothing to back up (first save of a world);
//   * a ZERO-LENGTH source -> skip, because a 0-byte "<path>.bak" is worse than none:
//     eden_load_restore_backup() would offer it as a restorable previous save;
//   * source at or above `g_save_inplace_threshold` -> skip. Above that line saveWorld stops
//     making its own scratch copy for exactly the same reason (a whole-file duplicate of a
//     multi-gigabyte world), and the rollback journal covers the failure this slot protects
//     against. tools/headless-save-io-probe.js measured both firing on one save: 3x the world
//     file written, on a 279 MB specimen;
//   * a short/failed copy -> delete the partial .bak rather than leave a misleading one.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Best-effort and silent: a missing backup is the status quo, never a new failure mode. Safe to
// call with a path that does not exist.
void eden_save_backup_before_overwrite(const char* path);

#ifdef __cplusplus
}
#endif
