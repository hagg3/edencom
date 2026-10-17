// PlayerReadme_native.h — C.9: the README.txt written into the app's documents directory.
#ifndef EDEN_PLAYER_README_NATIVE_H
#define EDEN_PLAYER_README_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

// The shipped text (first line is the version line). Every `backticked` word in it is checked by
// --console-selftest against the verb tables and the cvar registry, so it cannot name a command
// that no longer exists.
const char* eden_player_readme_text(void);

// Writes <dir>/README.txt unless it already carries this text's version line (a player's edits to a
// file of the current version are left alone). Returns 1 written, 0 already current, -1 failed.
int eden_player_readme_write(const char* dir);

#ifdef __cplusplus
}
#endif
#endif
