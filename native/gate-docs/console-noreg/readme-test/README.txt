Emod README v2
==============

This folder is where Emod keeps your worlds. Each world is one file ending in .emod (or .eden,
if you imported an older one). Copy a file out to back it up; drop one in to add it. Before a
save replaces a small world, the old copy is kept next to it with .bak added to its name.

THE CONSOLE
-----------
Open it while you play: tap Console in the pause menu, or press the backquote key (under Esc on most keyboards).
The world keeps running behind it. Type a line and press Return (or tap Run). Tab completes a
word, Up and Down bring back lines you typed before. Close it with Back, or press Escape twice.

A line is one or more commands separated by ; so `time 0.5; heal` does both.

COMMANDS WORTH KNOWING
----------------------
  `help`            lists every command; `help fill` explains one.
  `find`            searches commands and settings: `find fov`.
  `set`             puts one block down: `set 0 1 0 glass`.
  `fill`            fills a box, up to 4096 blocks: `fill 0 0 0 5 3 5 stone`.
  `tp`              moves you: `tp 0 20 0` puts you 20 blocks above your feet.
  `day`             makes it day; `night` makes it night.
  `spawn`           brings in a creature: `spawn moof`.
  `heal`            full health.
  `video.fov`       a setting: `video.fov 95` widens your view; `video.fov` alone shows it.
  `toggle`          flips a setting on or off: `toggle audio.music`.
  `reset`           puts a setting back: `reset video.fov`.
  `cvars`           lists every setting; `cvars video` lists one group.

WHERE IS 0 0 0?
---------------
Numbers in a position are measured from the block under your feet: 0 0 0 is that block, 0 1 0
is the one above it, 3 0 0 is three blocks along X. `~` means the same as 0, and `=64` is the
world's own coordinate 64 instead of a distance from you. `where` shows your cell and the
block you are looking at. `anchor look` moves 0 0 0 to the block you look at; `anchor me` puts
it back under your feet.

A line that has a mistake in it runs nothing at all, and says what is wrong. `wait 5; day`
waits five seconds without stopping the game; `jobs` shows what is waiting and `stop` ends it.

MODS (CUSTOM BLOCKS)
--------------------
A mod is a pack: a folder holding one pack.json and the 32x32 PNG pictures it names. No code, no
rebuild. To add one, put its folder inside the Packs folder next to Worlds (make Packs if it is
missing; on iOS it shows in the Files app under Emod). Every pack there is read each time you open
a world, so adding, editing or removing one needs no restart: just reopen the world.

Packs are on all the time; there is no switch. Place a mod block from the Mods pages of the block
picker, or by name: `set ~ ~ ~2 my_blocks:brick`. The first one you place in a world asks you to
confirm, because that world then holds blocks only the pack can draw. Take the pack away and they
show as magenta blocks; nothing is lost, and putting the pack back brings the look back.
`packs` lists every pack folder and any problem found in it; `packs my_blocks` lists its blocks.

Making one:
  1. Make a folder in Packs, say my_blocks, with a textures folder inside it.
  2. Draw a 32x32 PNG into it: textures/brick.png.
  3. Save this as my_blocks/pack.json (lines starting // are comments):

     {
       "format": 1,
       "id": "my_blocks",            // same as the folder; a-z, 0-9, _ ; not eden or emod
       "name": "My blocks",
       "version": "1.0.0",
       "tiles": {
         "brick": {"png": "textures/brick.png"}
       },
       "blocks": [
         {"id": "brick",               // its name in the game is my_blocks:brick
          "faces": {"all": "brick"},   // or sides / top / bottom
          "color": [255, 255, 255],     // white for full-colour art; grey art is tinted by this
          "traits": ["solid", "hard"],
          "sounds": "eden:stone",
          "fallback": "eden:stone"}   // what others see, and what exports to .eden, without the pack
       ]
     }

  4. Open a world and try `set ~ ~ ~2 my_blocks:brick`. If nothing appears, `packs` says why.

A complete example with six blocks ships in the game's packs/emod_example folder: copy it into
Packs to try it. The full list of traits and options is docs/content-packs.md in the Emod source.

More: docs/console-reference.md in the Emod source lists every command and setting.
