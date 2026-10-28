// PlayerReadme_native.cpp — C.9 (WORKING/ROADMAP.md Stage C): the player's README.txt.
//
// One source for the text so --console-selftest can check it against the registries: every
// `backticked` token that looks like a command or a variable must exist. Bump the version number on
// the first line whenever the text changes; the file is rewritten only when that line differs, so a
// player's own edits to a current file survive. Rule 11 (eden_app_identity.h): the directory belongs
// to Emod, so the text says Emod.
#include "PlayerReadme_native.h"

#include <cstdio>
#include <cstring>
#include <string>

static const char kReadme[] =
"Emod README v1\n"
"==============\n"
"\n"
"This folder is where Emod keeps your worlds. Each world is one file ending in .emod (or .eden,\n"
"if you imported an older one). Copy a file out to back it up; drop one in to add it. Before a\n"
"save replaces a small world, the old copy is kept next to it with .bak added to its name.\n"
"\n"
"THE CONSOLE\n"
"-----------\n"
"Open it while you play: tap Console in the pause menu, or press the backquote key (under Esc on most keyboards).\n"
"The world keeps running behind it. Type a line and press Return (or tap Run). Tab completes a\n"
"word, Up and Down bring back lines you typed before. Close it with Back, or press Escape twice.\n"
"\n"
"A line is one or more commands separated by ; so `time 0.5; heal` does both.\n"
"\n"
"COMMANDS WORTH KNOWING\n"
"----------------------\n"
"  `help`            lists every command; `help fill` explains one.\n"
"  `find`            searches commands and settings: `find fov`.\n"
"  `set`             puts one block down: `set 0 1 0 glass`.\n"
"  `fill`            fills a box, up to 4096 blocks: `fill 0 0 0 5 3 5 stone`.\n"
"  `tp`              moves you: `tp 0 20 0` puts you 20 blocks above your feet.\n"
"  `day`             makes it day; `night` makes it night.\n"
"  `spawn`           brings in a creature: `spawn moof`.\n"
"  `heal`            full health.\n"
"  `video.fov`       a setting: `video.fov 95` widens your view; `video.fov` alone shows it.\n"
"  `toggle`          flips a setting on or off: `toggle audio.music`.\n"
"  `reset`           puts a setting back: `reset video.fov`.\n"
"  `cvars`           lists every setting; `cvars video` lists one group.\n"
"\n"
"WHERE IS 0 0 0?\n"
"---------------\n"
"Numbers in a position are measured from the block under your feet: 0 0 0 is that block, 0 1 0\n"
"is the one above it, 3 0 0 is three blocks along X. `~` means the same as 0, and `=64` is the\n"
"world's own coordinate 64 instead of a distance from you. `where` shows your cell and the\n"
"block you are looking at. `anchor look` moves 0 0 0 to the block you look at; `anchor me` puts\n"
"it back under your feet.\n"
"\n"
"A line that has a mistake in it runs nothing at all, and says what is wrong. `wait 5; day`\n"
"waits five seconds without stopping the game; `jobs` shows what is waiting and `stop` ends it.\n"
"\n"
"More: docs/console-reference.md in the Emod source lists every command and setting.\n";

extern "C" const char* eden_player_readme_text(void) { return kReadme; }

extern "C" int eden_player_readme_write(const char* dir) {
    if (!dir || !*dir) return -1;
    const std::string path = std::string(dir) + "/README.txt";
    const char* nl = std::strchr(kReadme, '\n');
    const size_t verLen = (size_t)(nl - kReadme);
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char first[64] = {0};
        const bool got = std::fgets(first, sizeof(first), f) != NULL;
        std::fclose(f);
        size_t n = std::strlen(first);
        while (n && (first[n - 1] == '\n' || first[n - 1] == '\r')) first[--n] = 0;
        if (got && n == verLen && !std::memcmp(first, kReadme, verLen)) return 0;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return -1;
    const size_t len = sizeof(kReadme) - 1;
    const bool ok = std::fwrite(kReadme, 1, len, f) == len;
    return (std::fclose(f) == 0 && ok) ? 1 : -1;
}
