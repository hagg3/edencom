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
"Emod README v2\n"
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
"MODS (CUSTOM BLOCKS)\n"
"--------------------\n"
"A mod is a pack: a folder holding one pack.json and the 32x32 PNG pictures it names. No code, no\n"
"rebuild. To add one, put its folder inside the Packs folder next to Worlds (make Packs if it is\n"
"missing; on iOS it shows in the Files app under Emod). Every pack there is read each time you open\n"
"a world, so adding, editing or removing one needs no restart: just reopen the world.\n"
"\n"
"Packs are on all the time; there is no switch. Place a mod block from the Mods pages of the block\n"
"picker, or by name: `set ~ ~ ~2 my_blocks:brick`. The first one you place in a world asks you to\n"
"confirm, because that world then holds blocks only the pack can draw. Take the pack away and they\n"
"show as magenta blocks; nothing is lost, and putting the pack back brings the look back.\n"
"`packs` lists every pack folder and any problem found in it; `packs my_blocks` lists its blocks.\n"
"\n"
"Making one:\n"
"  1. Make a folder in Packs, say my_blocks, with a textures folder inside it.\n"
"  2. Draw a 32x32 PNG into it: textures/brick.png.\n"
"  3. Save this as my_blocks/pack.json (lines starting // are comments):\n"
"\n"
"     {\n"
"       \"format\": 1,\n"
"       \"id\": \"my_blocks\",            // same as the folder; a-z, 0-9, _ ; not eden or emod\n"
"       \"name\": \"My blocks\",\n"
"       \"version\": \"1.0.0\",\n"
"       \"tiles\": {\n"
"         \"brick\": {\"png\": \"textures/brick.png\"}\n"
"       },\n"
"       \"blocks\": [\n"
"         {\"id\": \"brick\",               // its name in the game is my_blocks:brick\n"
"          \"faces\": {\"all\": \"brick\"},   // or sides / top / bottom\n"
"          \"color\": [255, 255, 255],     // white for full-colour art; grey art is tinted by this\n"
"          \"traits\": [\"solid\", \"hard\"],\n"
"          \"sounds\": \"eden:stone\",\n"
"          \"fallback\": \"eden:stone\"}   // what others see, and what exports to .eden, without the pack\n"
"       ]\n"
"     }\n"
"\n"
"  4. Open a world and try `set ~ ~ ~2 my_blocks:brick`. If nothing appears, `packs` says why.\n"
"\n"
"A complete example with six blocks ships in the game's packs/emod_example folder: copy it into\n"
"Packs to try it. The full list of traits and options is docs/content-packs.md in the Emod source.\n"
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
