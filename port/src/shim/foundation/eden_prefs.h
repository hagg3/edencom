// eden_prefs.h — G.2b-0 (plan D7). The engine's preferences: a C API over ONE flat-file store on every
// platform. File: `key<TAB>n:<int>` or `key<TAB>s:<utf8>` per line, written whole through scratch +
// rename. Keys must not contain TAB/CR/LF and string values must not contain CR/LF (as before).
// Location: macOS/iOS ~/Library/Application Support/Emod/prefs, Linux $XDG_CONFIG_HOME/emod/prefs,
// Windows %APPDATA%\Emod\prefs. No path (no HOME): memory-only for the session.
#ifndef EDEN_PREFS_H
#define EDEN_PREFS_H

#ifdef __cplusplus
extern "C" {
#endif

int  eden_prefs_has(const char* key);
int  eden_prefs_type(const char* key);      // 0 absent, 1 int, 2 string
int  eden_prefs_get_int(const char* key, int fallback);
void eden_prefs_set_int(const char* key, int value);
// Returns a pointer valid until the next set/path change, or NULL if absent / not a string.
const char* eden_prefs_get_str(const char* key);
void eden_prefs_set_str(const char* key, const char* value);
void eden_prefs_sync(void);                 // every set already writes through; kept for callers' symmetry
// Call before the first access: nothing is read from or written to disk (X.21: selftests).
void eden_prefs_set_ephemeral(int on);
// Tests: drop all state and use `path` (NULL = back to the platform path).
void eden_prefs_reset_for_test(const char* path);

#ifdef __cplusplus
}
#endif
#endif
