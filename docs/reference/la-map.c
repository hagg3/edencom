/*     la-map.c  - locus amoenus  - A mapping program for Eden World Builder

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.


    This program and its creator, Robert Munafo, have no affiliation with
    Kingly Games (the creators of Eden World Builder). It is published as
    an example of data analysis and recovery, and will be a valuable resource
    in the inevitable event that the game and/or the iOS operating system
    are no longer available.
*/

/*
                        INSTRUCTIONS FOR USE

I wrote an article about my Eden savefile analysis at
mrob.com/pub/vidgames/eden-file-format.html and a similar article on
Mooncraft/Lunacraft at mrob.com/pub/vidgames/mooncraft-lunacraft-format.html
The following is just a quick guide explaining how to compile and run
the map program on your Mac, Linux or Windows PC.

You do NOT need to "jail-break" your device, and I do not recommend it.
No "unauthorized" actions are involved in making maps of Eden worlds.

  * Use a UNIX-compatible shell with the basic commands, including a C
    compiler. Linux, MacOS X, and Cygwin are all good.

    If you have Microsoft Windows, install Cygwin (the free UNIX system
    that runs under Windows). When you select your packages, make sure to
    include the "gcc" package in the "devel" section.

  * If you've never learned anything about software development, now is
    your chance! Read these:

       en.wikipedia.org/wiki/Command-line_interface
       en.wikipedia.org/wiki/Compiler

    and use Google or Bing to find any of the thousands of tutorials and
    educational resources related to computer programming. In addition,
    Cygwin-specific tutorials exist, do an internet search for
    "cygwin gcc programming tutorial"

  * Make a new directory (also called a "folder") where you'll do your Eden
    mapping work. You can do this using whatever method you're familiar with,
    e.g. in the File Explorer (Windows) or the Finder (Apple).

  * Go to mrob.com/pub/vidgames/eden-file-format.html and look at the
    "Source Code" link to get this file "la-map.c.txt". Put the file
    in your new directory. Change its name to "la-map.c".

  * Now you need to use the terminal, also called a "shell" or "console",
    to type commands. First you need to get into the directory that has
    la-map.c. The command to change directories is called "cd". Use the
    command "cd" to go to your home directory, or "cd foo" to go to the
    directory "foo", or "cd .." to go to the directory that is one level
    higher from the current directory.

  * Compile the source code with a command like:

       gcc la-map.c -o locus-amoenus

  * On your iDevice, run Eden, go into a world, take a photo, save, and quit.

  * Use iTunes to back up your iPad (or iPod Touch etc.), then look in
    C:\Users\(username)\AppData\Roaming\Apple Computer\MobileSync\Backup .
    If you're on a Mac, it'll be in (your home)/Library/Application Support/
    MobileSync/Backup .  Look for a big, recently-modified file. That's
    probably your saved Eden game.
       Instead of iTunes, you can also use a utility like iExplorer or
    iFunBox to get the saved game file directly out of the device. Look
    in Apps > Eden.app > Documents. The photo you took will help you
    identify which world is the one you want. For example, if the photo
    is called "1440305296.eden.png", then the game data is "1440305296.eden".
    Once you find the saved game data, copy it into the directory you made
    in step 1.

  * Run the mapping program with a command like:

      ./locus-amoenus 1440305296.eden fmg q

    where "1440305296.eden" is the name of the saved-game file.

    It will produce an output file called "World_1.ppm", where
    "World_1" is the name of the world (with spaces changed to '_'
    and quotes changed to '.'). PPM is an image file format.

    If you get an error like "world not found", make sure your world
    file is actually in the current directory, make sure you spelled its
    name properly, and make sure you types "fmg" and not "fmq" or something
    else.

  * Use ImageMagick or another image conversion utility to convert
    "World_1.ppm" to JPG or PNG or whatever format you want.

  * There are other functions available in this program. Leave off the
    'fmg q' part of the command, and you'll see a command prompt like this:

      locus-amoenus command (? for help) >

    then enter '?' to see a list of commands.

You can supply many commands as command-line arguments, like this:

      ./locus-amoenus 1440305296.eden 'wh 22' 'w 865' \
         'n 480' tl 'e 1120' 's 735' br fmg q

which will set the bounds using 'tl' and 'br' then produce a graphical
map and exit. The bounds limit what gets output, so it makes a map of only
part of the world, which might be useful.

*/

/*

REVISION HISTORY
 20130331 Begin preliminary investigations into the Eden file format
using iTunes' backup to get the world data files onto my PC (they are
in ~/Library/Application Support/MobileSync/Backup. A new "flat" world
is 4733088 bytes, and this size doesn't change if you do simple things
(digging one block and replacing it with something else). Conclusion:
the map data is not compressed.
 20130402 Start using the UNIX "od" tool to inspect the data. The
initial 192 bytes contains the world name and some other text. Figure
out the format for the blocks and colours (but not animals).
 20130403 Write the basic file scanning and cache functions.
 20130404 Get srs() working; begin filling in g_blt[] array. Get fmg()
working. Implement colour output and axonometric projection.
 20130405 Add max_lt block attribute, because things are darker than
expected when coloured. Put sky-blue around the edges. Parse out the
world name and print it right after opening the file.
 20130406 Add command loop; output filename is now based on world name
(but changing ' ' to _ and ' to .). Fix colour table indexes for the
darkest row of colours; attempt some fixes for colouring of light colours
applied to rock and other materials that are originally a shade of
gray.
 20130407 Change colouring algorithm again, this time it seems to work
well for both light and dark colours, including the main problem cases
I was dealing with yesterday.
 20130408 Add hash lists for the TOC and the chunk cache. This
massively improves performance for big maps, like the Mega City Tokyo
worlds I am working with most of the time.
 20130410 Add block types for liquid "slabs". Each has a slightly
different colour so flowing water (and deliberate wavy oceans) will
have a distinctive pattern.
  Add commands 'n', 's', 'e', and 'w'; and add ANSI terminal colours.
 20130411 Add 'wh' command.
 20130412 Add fnum and world fields/parameters; rename many "x,y"
variables to "ns,we" respectively.
 20130413 Add 'world' command; get multiple files working (but not
tested too much yet).
  Adjust ANSI colours so all blocks are easily distinguishable from air
and water (the two 'permeable' block types).
  Add 'nw', 'ne', 'se', and 'sw' commands.
  Add 'plan' and 'lw' commands.
 20130414 Add 2-parameter 'wh' to set width and height separately; add
1-parameter movement ('n', 'ne', 'e', etc.) commands to move in units
of a block.
 20130415 Add colour modes 'norm', 'ortho', and 'diff W' (where W is a
world selector from 'lw'). 'ortho' gives a straight-down view without
anti-aliasing; 'diff' is used to show where the map was changed between
world A and world B (which should be related).
 20130416 Add coords_init and brief_help. Add legend, fix a bug in
cache.punt1
 20130417 Add 'slice', 'u' and 'd' commands to easily view and navigate
in cross-section view; 'fmg' also works in slice mode.
  More error-checks, and better messages, to handle damaged Eden files,
or cases where all the chunks are far from the normal (4096,4096) origin.
 20130419 Add scancol and CM.STRUCT colourmode.
 20130421 Refactor scancol (working towards a CM.STRUCT mode for the
'slice' and 'plan' commands).
 20130424 More refactoring of scancol; improve help and legend.
 20130428 Still hacking away at scancol, trying to get it right.
 20130429 I think I have scancol the way it should be. Rename it to
'struct.col'. Add struct.zz and enable struct colouring in 'plan'
command.
 20130430 Rework the mode settings to separate 'selection mode'
(g_selmode) from 'colouring mode' (g.colourmode).
 20130501 Alter 'fmg' output filename based on which colourmode and selmode
is used to generate it.
  Increase cache size to 102,400 chunks, but don't abort if we can't
calloc that much memory (because that's over 3 G bytes, and we want
this program to be usable on smaller machines)
  Add 'tun' command to show tunnels/caves colour-coded by depth (like
the 'tun' command in my Mooncraft mapping program)
  Add 3-way diff mode, and routines d3_blk, d3_col. This is for
comparing three worlds (that are presumably all related). Given a
"base" world and derivatives A and B, it will colour things:
  - blue, if changed in A as compared to base
  - green, if changed in B as compared to base
  - blue, if changed in both but to the same thing
  - red, if changed in both and not to the same thing (a "conflict")
  - no colour (dark gray) if all three are the same
  - additive combination of colours if more than one type in the same column
  Increase hashtable size to 2^14.
 20130504 Add the fnv1x and fnv0 functions, then run some benchmarks
to see if it will serve as a better hashfunction for the chunk cache.
These benchmarks, use the command:

      time ./la-map 1365380295.eden 'wh 38 4' fmg tun fmg q q

where "1365380295.eden" is the world called "The Hidden Galaxy v2'8"
dated 04/08/2013.
  These benchmarks are the "real" (not "user" or "system")
measurement, running on SB4 with no other programs using the CPU. To
verify that the Sandy Bridge Turbo 2.0 and/or the OS disk cache is not
affecting the results, I run the test 3 times each time and keep the
middle result. The -O2 results were done after I added the -O2 compile
option, and "MH-64K" shows the effect of increasing MAXHASH from 1<<14
to 1<<16.
  hashfunction    no opt      -O2       MH-64K      -O3
    simple-1     1m41.326s  0m57.876s  0m58.022s  0m50.583s
    simple-2     1m40.712s  0m58.352s  0m56.850s  0m48.976s
    fnv1x        2m01.869s  1m03.778s  1m01.855s  0m54.736s

Here is the code for hash_wxy() and hash_wxyz() for each of the three
benchmarks:
  simple-1:
    return(((w*143) ^ (x*7) ^ y) % MAXHASH);
    return(((w*143) ^ (x*27) ^ (y*7) ^ z) % MAXHASH);
  simple-2:
    return(((w*1011) ^ (x*143) ^ (y*27)) % MAXHASH);
    return(((w*1011) ^ (x*143) ^ (y*27) ^ (z*7)) % MAXHASH);
  fnv1x:
    int ht[3]; unsigned int rv; ht[0] = w; ht[1] = x; ht[2] = y;
    rv = fnv1x(((char *)(ht)), 3L*((long)(sizeof(int))), 143);
    return ((int)(rv % MAXHASH));
    // (the wxyz version is very similar)

Based on the measurements, I went with the "simple-2" version. FNV
adds too much to the time to be worth it. However, FNV will be needed
for the world chunk checksums list command that I plan to add later.

 20130505 add font_init and fontfunction, PPM output now includes a
caption.
 20130506 Slight adjustments to caption position and margins.
  Add 'inset', 'ins3', 'outset', and 'out3' commands. Make captions work
properly in multi-file (diff and dif3) cases.
 20130507 Add triptych colourmode. In the process I adjust a few things and
decide that the file ordering of normal 2-file diff mode is confusing, so I
reverse it.
 20130508 Add 'spawn1' command and clean things up a little.
 20130510 Improve triptych display by adding subheaders for each pane.
 20130515 Add 'colours' command.
 20130528 In randi, 'Fold' fnv result if modulus is 16-bit or less
 20130611 Add 'tpt' command; adjust print messages a little.
 20140101 Enable other display types in triptych mode; for example
'tt B C' followed by 'struct' to see all 3 worlds side-by-side in struct
display mode.
 20140102 'tt' without an argument now attempt to toggle the display
between single and 3-world display. Similarly, 'diff' without an argument
toggles diff display. In any case, it re-displays in the new mode unless
it could not change the mode.
 20140104 'wh' without argument shows the current width and height.
 20140118 Add 'sm' and 'gm'; Minor improvements to 'smc'.
 20140122 Add msl_mem and msl_rpt

 20141005 It works on riverbeta now, but I still need to put in
smarter checks for the alignment of the TOC, mob data, chunks and
chunk pointers.
 20141007 Somewhat better checking of TOC and block address correctness
 20141008 Add 'gmag' command and associated changes to cmd.fmg,
dth.clrat, colo.rat, etc.
 20141013 Relax the block-baseaddress restrictions again, because I've
found a world (ID 1322340479, name "super world by kevin") where the
blocks do not align with the mob start (blocks are at the normal 00c0,
but tocstart is at 034080c0 and mob start is 034051e0).
 20141017 Add 'fon' (fixed output name) command
 20141019 Draw wedges more precisely in axonometric views (very
noticable when using gmag)
 20141020 Add transparency to color.at/ftop.most. Maps look really nice now!
 20141106 Fix bug in struct mode: it was ignoring changes in colour only (e.g.
if there is an uncoloured dirt block above a gray dirt block, the change to
gray was being ignored)
 20141107 Improve struct mode to no longer treat coloured tree-trunks as
man-made in the limited cases where such are natural.
 20141108 Significant improvements to struct mode, now showing
 20150127 Add some code to figure out the new 1.9.4 file format (which uses
run-length encoding within each chunk)
 20150305 Add Eden 2.0 block types
 20150309 In 'srs' command, except in triptych modes (srs.top3), make
air recognizably different from the total absence of a block.
 20150316 'src' now shows "?" for coloured air; natural.p now treats
coloured air as natural.
 20150322 Add 'outliers' command, and unused routine show_outl_by_fpos
that is similar but gives the chunks in file-position order.
 20150326 'lw' now puts brackets around world name for easier parsing
 20150327 Render caves in different shades of blue depending on their
internal height.
 20150328 Add 'header' command; hilight caves with a little purple (for
shallow caves, which are more likely to be man-made) or green (for
tall caves)
 20150331 Read the TOC in a much more sensible way; this fixes some
worlds that previously had not been handled, including:
    20110913.0043  1315874595  58.2 KiB  foundation shake
    20110910.2238  1315708726  1.52 MiB  ZACK'S HOUSE
    20121227.1336  1356633372   248 KiB  JO YO Super City V5
    20110928.1125  1317209127  1.05 MiB  "a best city"
    20110916.1858  1316199537  2.74 MiB  "CAVE TOWN"
 This also handles 1322340479 "super world by kevin" (a Tokyo cousin)
where there seems to be a gap between the last block and the mobs, but
the blocks are aligned with 00c0 in the normal way.
 20150403 Add 'gc' and 'toc' commands; refactor gochunk and add
blockxy_to_chunkxy (which is not yet used); it is now possible to view
outliers with 'srs' and similar commands (but not yet with 'fmg').
 20150407 Avoid spamming stdout with TOC lines for worlds like
1318633823, which seems to be mostly null bytes.
 20150710 show.toc() now also does the 'outliers' command.
 20150711 Add 2-parameter forms of 'tl' and 'br' commands; add 'tlc'
and 'brc' commands. This is all to support #mapit#. Add 'tsum' command.
 20150901 In bc_col, ignore colour if the block is air.
 20160617 Add 'frame' command.
 20160625 srs.topmost, srs.1s, srs.slices, and srs.dispatch now show
colours in decimal (with 'C+' if 100 or more)
 20170129 Fix some compiler warnings
 20190701 Add 'crc' command, which prints the summary stats of the
'toc' command (number of connected blocks, outlier blocks, and
"shapeCRC").
 20200122 Add get.le_u64 and make changes to handle filesizes and TOC
offsets bigger than 2^32. I can now map colossal worlds like the
"Geometry_World_PHX" series, an Eden-1.7 world in which the player
explored all the way to the northeast and southeast limits.
 20200308 Add more checks when loading a world; add 'mobs' command

BUGS and TTD:

In struct_zz() I have a test "if (z1 < z2)" ... which causes the
compiler warning "warning: assuming signed overflow does not occur
when assuming that (X + c) < X is always false [-Wstrict-overflow]".
This happens because of a very spohisticated optimisation:
  * All of the calls to struct_zz are such that z1 is not less than z2, viz.:
      rv = struct_zz(w, x, y, 63, 0, 1);
      ansicol = struct_zz(tfil[tti], ns, we, level+2, level, 0);
  * The -O3 optimiser therefore removes the "if (z1 < z2)" block
  * Warning is generated, because it thinks I must have put that code block
    in there for a reason. Maybe I's trying to check for signed overflow?
For a more thorough explanation, see stackoverflow.com/questions/12984861
  In fact, the test is there merely because I want to be able to call
the function with z1,z2 in either order, and I'm not yet using that
flexibility.

Other simple stats that can be printed for a world:
  * number of 'outlier' blocks
  * "shape": a checksum of a sorted list of the X,Y coordinates of all
chunks
  * uncompressed file size (or number of chunks in TOC)
  * number of chunks that point to the same data block as another
chunk (might not happen, but will be useful to find out)

When loading a world, find the spawn point (the present avatar
position) and center the view on that point.
  Add a 'home' command that jumps to the home position
  Add a 'spawn' command that jumps to the spawn point.

Coordinate alignment, blockxy_to_chunkxy and related functions don't
quite work yet. To figure this out it will be easiest if I add a raw
chunk dump command that dumps the contents of a chunk in raw byte
order.

'height map' colormode, picks a colour based on the highest stone
block (for natural terrain, this will show the original Perlin noise).
I could use full rainbows, or perhaps use
red->yellow->green->brown->red in a cycle and reserve the blue channel
for cave thickness as in the present normal colour mode. This should
be useful for identifying where the scrambled blocks belong.

struct mode:
 sc_one should ignore dark gray steel at bedrock level (global_blockmod)
 coloured tree-trunk is getting counted as man-made, but it should not
be if it is part of a tree (bug in Eden's tree generation)
 coloured dirt should be counted as man-made

Shadows, so that 3-d structures of identical structure and material
can be seen (for example see world ID 1318432593). This would require
"ray tracing" similar to the existing ftopmost algorithm, towards the
"sun" which should be in the southeast, starting at whatever is the
first solid block as currently determined by ftopmost.

Identify existence of caves with e.g. a statistical study of blocks at
z=1. Compare e.g. 1316247619.eden, 1319293148.eden and 1321516129.eden
(which have caves) to 1318787945.eden and 1319484282.eden, and
1320587676.eden (which do not). Other early worlds can be found in the
trawl collection. Some early worlds, like 1316412188.eden,
1411771638.eden, 1315468827.eden, and 1321124139.eden, are hybrids
that started with caves and later do not (or vice-versa)

Examine TOC for duplicate blocks. For example world 1322340479 has
multiple identical-looking blocks, making it seem likely it has
multiple TOC entries pointing to the same block.

Detect unused data-ranges in the file, i.e. block-address values that
should appear somewhere in the TOC but don't.

Figure out how I can handle riverbeta (where the alignment of chunks
is 00bc but there is no unused gap between chunks and TOC)

Test with other artificial/modded worlds.

Parse the creatures (mobs or animals) data. I learned from Ari by way
of Vuenc that the creature data is "a section between the block data
and the chunk pointers which is always 12000 bytes long". This
suggests a round number of creature spots (perhaps 500 creatures with
24 bytes each). As in Lunacraft the mobs must have 4 floating-point
values (x, y, z, rot), plus a creature type, colour, and health.

'move' command, with either 2 or 3 arguments, to move by an arbitrary
amount (vector) in 2 or 3 dimensions; 'rm' (repeat move) command to
move the same amount again.

'sm' (set mark) and 'gm' (goto mark) commands.

'cks' (checksum) command, prints out a list of 16x16x64 chunks, each
with a CRC and x and y coordinates. This will be very useful for
quickly comparing worlds.

New display modes:
  * griefing detection diff mode: only display diffs that involve
removing blocks en masse (TNT) or adding water/lava. To really work,
this needs to show removal only in cases where the removed block is
within a TNT-sized sphere of nothing and that might be hard to
calculate.
  * a natural vs. artificial colour mode, detects man-made stuff like
the existing 'struct' colourmode, but displays manmade objects in its
real colour, and natural terrain in grayscale. The colour for man-made
objects would be the object's actual (painted or unpainted) colour, but
using bright/dark to distinguish things: natural stuff is dark gray,
man-made stuff medium gray or brighter. Since we'd be showing the
"topmost" colour, and natrual topmost is dark gray, artificial
underground excavations would need to be highlighted in artificial
colour (but natural caves and overhangs would *not* be visible at all).

TEST COMMANDS:

My standard dimensions for a Tokyo map are 1200x800:

  ../eden-mapping-tool ../data/my-latest.eden \
    'w 945' 'n 530' tl 'e 1200' 's 800' br fmg

Three-way diff contrasting my changes to those by Djasiantech:

  ../eden-mapping-tool \
    ../popular-worlds-zipfiles/MCTE-20130202-MEGA_CITY_TOKYO_EXTENDED_V3-1359786771.eden \
    ../data/my-latest.eden \
    ../popular-worlds-zipfiles/MCTE-20130327-MEGA_CITY_TOKYO_EXTENDED_V4-1364392401.eden \
    'w 945' 'n 530' tl 'e 1200' 's 800' br 'world a' 'diff b c' fmg

Here's a long command to generate a bunch of maps for Hidden Galaxy:

  ../eden-mapping-tool ../dig-temp/hidden-galaxy/1365380295.eden \
    'w 728' 'n 684' tl 'e 1470' 's 1150' br \
    'slice 8' fmg 'slice 16' fmg 'slice 24' fmg 'slice 32' fmg \
    'slice 40' fmg 'slice 48' fmg 'slice 56' fmg \
    norm fmg tun fmg struct fmg

NOTES

20150404: I have investigated how to unscramble worlds. All of the
Tokyo relatives have scrambled blocks, and also 1315684012.eden "hotel
awesome".
   I started by looking at how a brand-new world is created: there are
144 blocks; in what order do they appear in the TOC, in the file, and
on the map? World 20110910.0124 1315617877 90.2 KiB "Sky LAND" has
exactly 12x12 chunks (192x192 blocks), caves, and clouds; so it was
created, edited, saved, and uploaded without movement beyond the home
block. Upon inspection however, the chunks seemed to be in a
completely random order.
   In Tokyo, the TOC seems to be in a different order every time a
world is uploaded; I suspect chunks are reordered in the process of
flushing the dirty blocks back to the file. The TOC entries for the
outlier blocks seem to have X and Y coordinates that are of the form
A*256+B, where A and B are typically small values including 0, 5, 8,
10-15, 23. The file positions are all unique, but seem to mostly
contain identical copies of the same data, and that data is itself
scrambled, as if it contains the last N bytes of one chunk and the
first 32768-N bytes of some other chunk. So it seems that these
"outliers" (54 outliers in the case of Tokyo cousins and
1315684012.eden) and not the missing puzzle-pieces.
   In world 1315684012 the scrambled area is about 14 wide and 10
high; but in Tokyo and all Tokyo variants, there is a rectangle to the
south of home that is 12 wide by 5 high, several blocks to the
northwest, north, and east, and east and a smallq group in the
southeast including the prominent diagonal triplet of glitch blocks.
Throughout all of these one can identify duplicates of other blocks
that are properly placed.
   I might still be able to repair the terrain by making a map that
has the seed 115115 and no chunks in the areas that need repair; then
walk into those areas forcing it to make terrain. If any of the
scrambled blocks are missing pieces of the puzzle, they could then be
identified up more easily (in order to get the cave terrain back).

 */
#define HASH_SIMPLE2

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


typedef int boolean;
#define B_FALSE (1==0)
#define B_TRUE (1==1)

typedef signed char s8;
typedef unsigned char u8;

/* Our function validate_types() checks the sized integer types at runtime
/  and may instruct the user to recompile with -DSHORT_IS_S16, etc. so if
/  one of these is defined, we use it to explicitly define the types.      */
#ifdef SHORT_IS_S16
  typedef short s16;
  typedef unsigned short u16;
# define HAVE_S16
#elif INT_IS_S16
  typedef int s16;
  typedef unsigned int u16;
# define HAVE_S16
#endif
#ifndef HAVE_S16
  typedef signed short s16;
  typedef unsigned short u16;
#endif

#ifdef INT_IS_S32
  typedef int s32;
  typedef unsigned int u32;
# define HAVE_S32
#elif LONG_IS_S32
  typedef long s32;
  typedef unsigned long u32;
# define HAVE_S32
#endif
#ifndef HAVE_S32
  typedef signed int s32;
  typedef unsigned int u32;
#endif

/* Eden 1.7 file format (presumably also valid for earlier versions,
back at least as far as version 1.1.1, because this program can read
files that were shared prior to the release of version 1.5. */

#define E17_HEADER_SIZE 0300

/* Block Type Attributes for Eden 1.7 */
typedef struct blta {
  int type;    /* The block ype, as a value found in the blocks[] chunk */
  char * longname; /* Human-readable name */
  char * nam2; /* 2-character name for SRS */
  float col_h; /* Default colour: hue */
  float col_l; /*              : lightness */
  float col_s; /*              : saturation */
  float max_lt; /* Maximum lightness when coloured */
  int ansicol; /* hex-encoded ANSI terminal colour for text mode maps */
  int attr;    /* Attributes, e.g. "natural" */
} blta;

/* Attribute bits */
#define ATTR_NATURAL     1
#define ATTR_PERMEABLE   2
#define ATTR_CAVETOP     4
#define ATTR_TRANSPARENT 8
#define ATTR_OHANGTOP   16

/* struct.col return bits */
#define SC_MANMADE 1
#define SC_OVERHANG  2
#define SC_CAVE    4
#define SC_MAN_TUNNEL 8
#define SC_CVHT1 16
#define SC_CVHT2 32
#define SC_CVHT3 48

#define BL_AIR 0
#define BL_BEDROCK 1
#define BL_STONE 2
#define BL_DIRT 3
#define BL_SAND 4
#define BL_LEAF 5
#define BL_TRUNK 6
#define BL_GRASS 8
#define BL_DKSTONE 0x0a
#define BL_WEED 0x0b
#define BL_FLOWER 0x0c
#define BL_ICE 0x0f
#define BL_CLOUD 0x13
#define BL_WATER 0x14
#define BL_FENCE 0x15
#define BL_IVY 0x16

#define BL_SNG_S 0x20
#define BL_SNG_W 0x21
#define BL_SNG_N 0x22
#define BL_SNG_E 0x23

#define BL_NEW_FLW 0x49
#define BL_STEEL 0x4a

blta g_blt[] = {
  /* TYP  Description     2ch   HUE  LIGHT  SAT  maxlt  ANSI  attr */
  {    0, "air (empty)",  " '", 210., 0.8,  1.0,  1.0,  0370,   3  },
  {    1, "bedrock",      "Ad", 0.0,  0.2,  0.0,  0.6,  0107,   1  }, /* "Adminium" */
  {    2, "stone/rock",   ",'", 0.0,  0.6,  0.0,  0.8,  0107,   5  },
  {    3, "dirt",         "::", 30.,  0.2,  1.0,  0.6,  0110, 0x11  },
  {    4, "sand",         "ss", 50.,  0.8,  0.5,  0.8,  0073,   0  },
  {    5, "leaf/leaves",  "{}", 120., 0.2,  0.8,  0.65, 0102,   1  },
  {    6, "trunk",        "()", 30.,  0.2,  1.0,  0.7,  0113,   1  },
  {    7, "wood",         "ww", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  {    8, "grass",        "!!", 120., 0.25, 0.8,  0.6,  0072, 0x11  },
  {    9, "TNT",          "**", 30.,  0.5,  0.7,  0.7,  0113,   0  },
  { 0x0a, "dark stone",   "~/", 0.0,  0.45, 0.0,  0.5,  0007,   5  },
  { 0x0b, "weeds",        ";:", 120., 0.25, 0.8,  0.6,  0072, 0x11  },
  { 0x0c, "flowers",      "!8", 150., 0.25, 0.7,  0.6,  0062, 0x11  },
  { 0x0d, "brick",        "=#", 0.0,  0.4,  0.8,  0.7,  0171,   0  },
  { 0x0e, "slate",        "EH", 210., 0.25, 0.25, 0.4,  0147,   0  },
  { 0x0f, "ice",          "i!", 210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x10, "wallpaper",    "^v", 0.0,  0.8,  0.0,  0.8,  0007,   0  },
  { 0x11, "bouncy",       "@@", 0.0,  0.2,  0.0,  0.4,  0070,   0  }, /* "Trampoline" */
  { 0x12, "ladder",       "!_", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x13, "cloud",        ". ", 0.0,  1.0,  0.0,  1.0,  0177,   1  },
  { 0x14, "water",        "~_", 225., 0.4,  0.9,  0.9,  0240, 0xA  },
  { 0x15, "fence",        "##", 50.,  0.75, 0.5,  0.8,  0137,   8  },
  { 0x16, "ivy",          "%&", 120., 0.6,  0.3,  0.6,  0027,   0  },
  { 0x17, "lava",         "?%", 20.,  0.4,  0.7,  0.6,  0101,   0  },
  /* 0x18 through 0x27 are "ramps" */
  /* TYP  Description     2ch   HUE  LIGHT  SAT  maxlt  ANSI  attr */
  { 0x18, "rock .S",      ",'", 0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x19, "rock .W",      ",'", 0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x1a, "rock .N",      ",'", 0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x1b, "rock .E",      ",'", 0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x1c, "wood .S",      "ww", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x1d, "wood .W",      "ww", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x1e, "wood .N",      "ww", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x1f, "wood .E",      "ww", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x20, "shing .S",     "$$", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x21, "shing .W",     "$$", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x22, "shing .N",     "$$", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x23, "shing .E",     "$$", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x24, "ice .S",       "i!", 210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x25, "ice .W",       "i!", 210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x26, "ice .N",       "i!", 210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x27, "ice .E",       "i!", 210., 0.8,  0.7,  0.9,  0176,   0  },
  /* 0x28 through 0x37 are "wedges" */
  /* TYP  Description     2ch   HUE  LIGHT  SAT  maxlt  ANSI  attr */
  { 0x28, "rock SE",      "/'", 0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x29, "rock SW",      "`\\",0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x2a, "rock NW",      ",/", 0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x2b, "rock NE",      "\\.",0.0,  0.6,  0.0,  0.8,  0107,   0  },
  { 0x2c, "wood SE",      "/'", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x2d, "wood SW",      "`\\",50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x2e, "wood NW",      ",/", 50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x2f, "wood NE",      "\\.",50.,  0.75, 0.5,  0.7,  0003,   0  },
  { 0x30, "shing SE",     "/'", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x31, "shing SW",     "`\\",0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x32, "shing NW",     ",/", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x33, "shing NE",     "\\.",0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x34, "ice SE",       "/'", 210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x35, "ice SW",       "`\\",210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x36, "ice NW",       ",/", 210., 0.8,  0.7,  0.9,  0176,   0  },
  { 0x37, "ice NE",       "\\.",210., 0.8,  0.7,  0.9,  0176,   0  },
  /* TYP  Description     2ch   HUE  LIGHT  SAT  maxlt  ANSI  attr */
  { 0x38, "shingles",     "$$", 0.0,  0.4,  0.0,  0.45, 0070,   0  },
  { 0x39, "tile/square",  "[]", 0.0,  0.9,  0.0,  0.9,  0007,   0  }, /* or "NeonSquare" */
  { 0x3a, "glass",        "//", 0.0,  0.6,  0.0,  0.6,  0077,   8  },
  /* 0x3b through 0x40 are liquid "slabs" */
  { 0x3b, "water 3/4",    "~_", 225., 0.5,  0.9,  0.8,  0240, 0xA  },
  { 0x3c, "water 1/2",    "~_", 225., 0.6,  0.9,  0.85, 0240, 0xA  },
  { 0x3d, "water 1/4",    "~_", 225., 0.7,  0.9,  0.9,  0240, 0xA  },
  { 0x3e, "lava 3/4",     "?%", 20.,  0.5,  0.7,  0.5,  0101,   0  },
  { 0x3f, "lava 1/2",     "?%", 20.,  0.6,  0.7,  0.55, 0101,   0  },
  { 0x40, "lava 1/4",     "?%", 20.,  0.7,  0.7,  0.6,  0101,   0  },

  /* TYP  Description     2ch   HUE  LIGHT  SAT  maxlt  ANSI  attr */
  { 0x41, "fireworks",    "^*", 30.,  0.5,  0.7,  0.7,  0113,   0  },
  { 0x42, "door N",       "__", 40.,  0.5,  0.7,  0.7,  0113, 0xA  },
  { 0x43, "door E",       "| ", 40.,  0.5,  0.7,  0.7,  0113, 0xA  },
  { 0x44, "door S",       "--", 40.,  0.5,  0.7,  0.7,  0113, 0xA  },
  { 0x45, "door W",       " |", 40.,  0.5,  0.7,  0.7,  0113, 0xA  },
  { 0x46, "door top",     "  ", 40.,  0.5,  0.7,  0.7,  0113, 0xA  },
  { 0x47, "trsr. cube",   "<>", 0.0,  0.7,  0.0,  0.7,  0007,   0  },
  { 0x48, "light",        "XX", 50.,  0.8,  0.5,  0.9,  0073,   0  },
  { 0x49, "new flwr",     " 8",225.,  0.5,  0.7,  0.7,  0240, 0xB  },
  { 0x4a, "steel",      "\\\\", 0.0,  0.6,  0.0,  0.7,  0107,   0  },
  { 0x4b, "portal N",     "pN", 0.0, 0.45,  0.0,  0.5,  0007,   2  },
  { 0x4c, "portal E",     "pE", 0.0, 0.45,  0.0,  0.5,  0007,   2  },
  { 0x4d, "portal S",     "pS", 0.0, 0.45,  0.0,  0.5,  0007,   2  },
  { 0x4e, "portal W",     "pW", 0.0, 0.45,  0.0,  0.5,  0007,   2  },
  { 0x4f, "portl top",    "pT", 0.0, 0.45,  0.0,  0.5,  0007,   2  },

  {   -1, "end of g_blt list",  "", 0., 0., 0., 0., 0, 0 }
}; /* End of g_blt[] */
int g_num_blts = 0;

/* Colour attributes */
typedef struct clra {
  int   ix;
  char  * description;
  float hue;     /* 0=red, 120=green, 240=blue */
  float lgt;     /* lightness: 1=totally white, 0.5=brilliant colour, 0=black */
  float sat;     /* Saturation: 0 for grayscale, 1 for colour */
} clra;

#define CO_WHITE   9
#define CO_GRAY40  0x24
#define CO_DK_GRN  0x28
#define CO_GRAY20  0x2d

clra g_clrs[] = {         /* HUE  LIGHT  SAT */
  {    0, "unpainted",       0.0,  0.0,  0.0 },

  {    1, "lt pink",         0.0, 0.85,  1.0 },  /* lt = light */
  {    2, "lt orng",         30., 0.85,  1.0 },
  {    3, "lt yel",          60., 0.85,  1.0 },
  {    4, "lt grn",         120., 0.85,  1.0 },
  {    5, "lt cyan",        180., 0.85,  1.0 },
  {    6, "lt blue",        240., 0.85,  1.0 },
  {    7, "lt purp",        270., 0.85,  1.0 },
  {    8, "lt mag",         300., 0.85,  1.0 },

  {    9, "white",           0.0,  1.0,  0.0 },

  { 0x0a, "lt red/pink",      0.0, 0.7,  1.0 },  /* mlt = medium-light */
  { 0x0b, "mlt orng",         30., 0.7,  1.0 },
  { 0x0c, "mlt yel",          60., 0.7,  1.0 },
  { 0x0d, "mlt grn",         120., 0.7,  1.0 },
  { 0x0e, "mlt cyan",        180., 0.7,  1.0 },
  { 0x0f, "mlt blue",        240., 0.7,  1.0 },
  { 0x10, "mlt purp",        270., 0.7,  1.0 },
  { 0x11, "mlt mag",         300., 0.7,  1.0 },

  { 0x12, "80% gray",        0.0,  0.8,  0.0 },

  { 0x13, "red",              0.0, 0.5,  1.0 },
  { 0x14, "orng",             30., 0.5,  1.0 },
  { 0x15, "yel",              60., 0.5,  1.0 },
  { 0x16, "grn",             120., 0.5,  1.0 },
  { 0x17, "cyan",            180., 0.5,  1.0 },
  { 0x18, "blue",            240., 0.5,  1.0 },
  { 0x19, "purp",            270., 0.5,  1.0 },
  { 0x1a, "mag",             300., 0.5,  1.0 },

  { 0x1b, "60% gray",        0.0,  0.6,  0.0 },

  { 0x1c, "mdk red",          0.0, 0.35, 1.0 },  /* mdk = medium-dark */
  { 0x1d, "mdk orng",         30., 0.35, 1.0 },
  { 0x1e, "mdk yel",          60., 0.35, 1.0 },
  { 0x1f, "mdk grn",         120., 0.35, 1.0 },
  { 0x20, "mdk cyan",        180., 0.35, 1.0 },
  { 0x21, "mdk blue",        240., 0.35, 1.0 },
  { 0x22, "mdk purp",        270., 0.35, 1.0 },
  { 0x23, "mdk mag",         300., 0.35, 1.0 },

  { 0x24, "40% gray",        0.0,  0.4,  0.0 },

  { 0x25, "dk red",           0.0, 0.25, 1.0 },  /* dark */
  { 0x26, "dk orng",          30., 0.25, 1.0 },
  { 0x27, "dk yel",           60., 0.25, 1.0 },
  { 0x28, "dk grn",          120., 0.25, 1.0 },
  { 0x29, "dk cyan",         180., 0.25, 1.0 },
  { 0x2a, "dk blue",         240., 0.25, 1.0 },
  { 0x2b, "dk purp",         270., 0.25, 1.0 },
  { 0x2c, "dk mag",          300., 0.25, 1.0 },

  { 0x2d, "20% gray",        0.0,  0.2,  0.0 },

  { 0x2e, "vdk red",           0.0, 0.15, 1.0 },  /* vdk = very dark */
  { 0x2f, "vdk orng",          30., 0.15, 1.0 },
  { 0x30, "vdk yel",           60., 0.15, 1.0 },
  { 0x31, "vdk grn",          120., 0.15, 1.0 },
  { 0x32, "vdk cyan",         180., 0.15, 1.0 },
  { 0x33, "vdk blue",         240., 0.15, 1.0 },
  { 0x34, "vdk purp",         270., 0.15, 1.0 },
  { 0x35, "vdk mag",          300., 0.15, 1.0 },

  { 0x36, "black",            0.0,  0.0,  0.0 },

  {   -1, "end of colour table", 0.0, 0.0, 0.0 },
}; /* End of g_clrs[] */
int g_num_clrs = 0;

/* Table of Contents data */
typedef struct tc_entry {
  u16 w;  /* World, i.e. which file this TOC entry is from */
  u16 x;
  u16 y;
  long fpos;
  int adjf;   /* Adjacency flag used by trickle.toc */
  int mark1;  /* Mark for use in e.g. show.outl_by_fpos */
  long hl_next;
} tc_entry;

typedef tc_entry * tc_array;

typedef struct cc_el {
  int w;  /* World */
  int x;
  int y;
  int z;  /* vertical coordinate */
  unsigned char * blocks;  /* 16*16*16 bytes of block IDs */
  unsigned char * colors;  /* 16*16*16 bytes of block colours */
  int qlru; /* quasi LRU count */
  long hl_next;
} cc_el;

#ifndef PROGNAME
# define PROGNAME "locus-amoenus"
#endif

/* I have also figured out the Mooncraft/Lunacraft file format. At some point
   in the future I might want to add another module that reads Mooncraft
   worlds, so I can use all the features of this program with Mooncraft maps.
   These #defines are for each of the functions that might have to be changed
   or replaced in order to open different types of maps. For example, Mooncraft
   spreads the world data across many files. cache_punt1 might want to check
   to see if we're "done" using a file, and close its filehandle when so,
   in order to minimize the number of open filehandles. */
#ifndef V_E17_TRY_OPEN
# define V_E17_TRY_OPEN e17_try_open
#endif
#ifndef V_CACHE_SETUP
# define V_CACHE_SETUP  cache_setup
#endif
#ifndef V_CACHE_PUNT1
# define V_CACHE_PUNT1  cache_punt1
#endif


#define CHUNKSZ (16L*16L*16L)
#define TRY_CC_SIZE 102400
#define MAX_QLRU 5
#define CMDLEN 100
/* 36 characters is the maximum name length when publishing worlds in
   Eden 1.7 */
#define MAX_NAME_LEN 36

void validate_types(void);
u32 fnv1x(char * buf, long buflen, u32 basis);
u32 fnv0(char * buf, long buflen);
u32 buf_fnv(u8 *buf, long int len);
void adler_init(void);
u32 crc32(u32 crc, u8 *buf, long int len);
u32 crc32_add_u16(u32 crc, u16 x);
u32 buf_crc(u8 *buf, long int len);
int iabs(int x);
int minmax(int x, int min, int max);
int min(int a, int b);
int max(int a, int b);
int char_to_fnum(char c, int * valid);
char fnum_to_char(int fnum);
void die1(const char * s);
int get_le_s16(FILE * f);
unsigned long get_le_u32(FILE * f);
unsigned long get_le_u64(FILE * f);
void toc_init(void);
void check_nfnum(void);
void scan_name(char * dst, int fnum, int verbose,
  char * ofname, char * of_suffix, int vncol);
void cmd_header(int fnum);
float getfloat(unsigned char *p);
void cmd_hf(int fnum);
void allnames(void);
int hash_wxy(int w, int x, int y);
void hl_init(void);
int e17_get_tcent(int fnum, long tcnum,
  int *x, int *z1, int *y, int * z2, long * cpos, long mobstart,
  int verbose);
int e17_tc_valid(int x, int z1, int y, int z2, long cpos,
                 long mobstart, long hdrend);
int e17_get_toc(int fnum);
long toc_lookup(int fnum, int x, int y);
long toc_hlu(int fnum, int x, int y);
void toc_trytag(int fnum, int x, int y, int t);
void trickle_toc(int fnum, int setpos);
void show_outl_by_fpos(int fnum);
u32 xy_lexo(long i);
void show_toc(int fnum, int flags);
void e17_getname(int fnum);
int e17_try_open(char * fname, int setpos);
int hash_wxyz(int w, int x, int y, int z);
int randi(int a, int b, int c, unsigned int lim);
void cch_init(void);
void cache_setup(void);
int cache_find(int w, int x, int y, int z);
int cache_hf(int fnum, int x, int y, int z);
int cache_punt1(void);
int cache_chunk3(int fnum, int x, int y, int z);
void font_load(int ch1, int ch2, char * data);
void font_init(void);
int fontfunction(int x, int y, char * msg, int mx, int my);
int blockval(int fnum, int ns, int we, int z, int *color, int errval);
int fblkval(int fnum, float fns, float fwe, int z, int *color);
int topmost(int fnum, int ns, int we, int *color, int cloudok, int errval);
int diffblock(int w1, int w2, int x, int y, int z);
int natural_p(int w, int x, int y, int z);
int toprock(int w, int x, int y);
int sc_one(int w, int x, int y, int z, int init);
int struct_col(int w, int x, int y);
int tm_col(int w, int x, int y, int dbg, int * first_top);
int tm_one(int w, int x, int y, int z);
int struct_zz(int w, int x, int y, int z1, int z2, int or_all);
int diffcol(int w1, int w2, int x, int y);
int d3_blk(int base, int w1, int w2, int x, int y, int z);
int d3_col(int base, int w1, int w2, int x, int y);
int ftopmost(int fnum, int * startz, float *fns, float *fwe, int * color,
  int cloudok, float ski);
void blix_scan(void);
blta * blix(int blt);
void cix_scan(void);
clra * cix(int col);
char * blnam2(int blt);
int blansi(int blt);
void blcol(int blt, float *ch, float *cl, float *cs, float *maxlt);
void cocol(int col, float *ch, float *cl, float *cs);
void ansiset(int mode);
void one_cc_line(int col, char *msg1, char *msg2, char *msg3);
void legend(void);
void colors(void);
void init_marks(void);
void set_limits(void);
void blockxy_to_chunkxy(int bx, int by, int * cx, int * cy);
void chunkxy_to_blockxy(int x, int y, int bias, int * bx, int * by);
void gochunk(int sx, int sy);
void go_1st_spawn(void);
void srs_topmost(int fnum, int clnum);
void srs_1s(int fnum, int clnum, int level);
void srs_slices(int fnum, int clnum, int lev1, int lev2);
void srs_dispatch(int fnum, int clnum);
void hsl_to_rgb(float ch, float cl, float cs, float *fr, float *fg, float *fb);
void bc_col(int b, int c, float *pcr, float *pcg, float *pcb);
void colorat(int fnum, float we, float ns, float *pcr, float *pcg, float *pcb,
  float ski, int cloudok);
void dthclrat(int fnum, float we, float ns, float *pcr, float *pcg, float *pcb,
  float mul, int cloudok);
void cmd_fmg(int fnum, int dif, int mag);
int get_input(char *s, int lim, int prompt);
void show_cur_world(int vncol);
void check_z_bounds(int z1, int z2);
void msl(int delta_ns, int delta_we, int delta_z);
void msl_mem(int delta_ns, int delta_we, int delta_z);
void msl_rpt(void);
void move_srs(int delta_ns, int delta_we);
void coords_init(void);
void brief_help(void);
void isos(int dxy, int dz);
void cmd_plan(int z1, int z2);
int cmd_world(char c1);
int cmdloop(int one_only);
int main(int argc, char **argv);

char g_name[MAX_NAME_LEN+1];
char of_name[MAX_NAME_LEN*2]; /* Name for output file, plus extension */
char * g_fixed_name = 0;

cc_el g_cc[TRY_CC_SIZE];
int g_cc_size = 0;
int g_reaper = 0; /* For LRU punting algorithm */

#define MAXFILES 26
long g_nc[MAXFILES]; /* Number of chunks (16x16 map segments) */
long g_tocbase[MAXFILES];
char * g_fnames[MAXFILES];
char * g_wnames[MAXFILES];
FILE * g_edat[MAXFILES];
long g_flen[MAXFILES];
long g_tocstart[MAXFILES];
long g_mobstart[MAXFILES];
int g_n_fnum = 0; /* Number of open files */
int g_cur_file = -1;
tc_entry * g_toc;
long g_trickle_found;

int g_xmin[MAXFILES];
int g_xmax[MAXFILES];
int g_ymin[MAXFILES];
int g_ymax[MAXFILES];

/* We define sized datatypes (boolean and two sizes of integer), and
/  in some cases (old compilers) we need to define these with a custom
/  typedef. This function makes sure the definitions work the way we need
/  and complains if not. In the 16-bit and 32-bit integer cases, we can also
/  tell the user which ifdef flag to use when re-compiling.                 */
void validate_types(void)
{
  boolean flag1, flag2;
  flag1 = B_TRUE;
  flag2 = (1==1);

  if (flag1 == flag2) {
    /* All is well */
  } else {
    /* Non-standard compiler and/or non-standard definition of true: This
    /  happens if "bool" is int, "true" is 1, and "(1==1)" is __INT_MIN__,
    /  or something similar. */
    printf("validate_types: true does not match (1==1).\n");
    exit(-1);
  }

  if (sizeof(s16) != 2) {
    printf("validate_types: s16 is not 2 bytes (got %d).\n",
      (int) sizeof(s16));
    if (sizeof(short) == 2) {
      printf(
        "  To fix this error, compile with the flag -DSHORT_IS_S16\n");
    } else if (sizeof(int) == 2) {
      printf(
        "  To fix this error, compile with the flag -DINT_IS_S16\n");
    } else {
      printf(
        "  (presently, sizeof(short)==%d and sizeof(int)==%d)\n",
        (int) sizeof(short), (int) sizeof(int));
    }
    exit(-1);
  }

  if (sizeof(s32) != 4) {
    printf("validate_types: s32 is not 4 bytes (got %d).\n",
      (int) sizeof(s32));
    if (sizeof(int) == 4) {
      printf(
        "  To fix this error, compile with the flag -DINT_IS_S32\n");
    } else if (sizeof(long) == 4) {
      printf(
        "  To fix this error, compile with the flag -DLONG_IS_S32\n");
    } else {
      printf(
        "  (presently, sizeof(int)==%d and sizeof(long)==%d)\n",
        (int) sizeof(int), (int) sizeof(long));
    }
    exit(-1);
  }
} /* End of validate.types */

/* -------------------------- FNV-1x hashfunction -------------------------- "/

FNV-1x hashing function:

The pseudo-code is:

  u32 hash = offset_basis;
  for(i=0; i<buflen; i++) {
    hash = (hash ^ ((u32) buf[i])) * FNV_prime;
  }
  return hash;

FNV-0 uses 0 as the offset_basis value
  For FNV-1a, offset_basis is is the FNV-0 hash of a specific string
containing "chongo"
  For FNV-1x, offset_basis is arbitrary, and can be used as a hash
selector input (e.g. by having it be the CRC or FNV-0 hash of a seed
string)
  FNV_prime is 16777619 for 32-bit FNV, or 1099511628211 for 64-bit
FNV. In general, it is the smallest prime of the form: 256^(int((5 +
2^s)/12)) + 2^8 + b; where 4<s<11, 0<b<2^8, and the number of one-bits
in b is 4 or 5. The sequence given by 256^(int((5 + 2^s)/12)) + 2^8,
starting with s=5, is: 256^3+256 = 16777472, 256^5+256 =
1099511628032, 256^11 + 256 = 309485009821345068724781312, 256^21+256,
256^43+256, 256^85+256, ... but for s=11 and above, the sparseness of
primes means that more requirements would need to be added to choose a
good prime for hashing.

*/

#define FNV32_PRIME 16777619

u32 fnv1x(char * buf, long buflen, u32 basis)
{
  long i;
  u32 hash = basis;
  /* printf("fnv1x(%lX, %ld, %d)\n", (long)buf, buflen, basis); */
  for(i=0; i<buflen; i++) {
    hash = (hash ^ ((u32) (buf[i]))) * FNV32_PRIME;
  }
  return hash;
}

u32 fnv0(char * buf, long buflen)
{
  return (fnv1x(buf, buflen, 0));
}

u32 buf_fnv(u8 *buf, long int len)
{
  return(fnv0(((char *)buf), len));
}

/* --------------------------- Adler/zlib CRC32  --------------------------- */

u32 crc32_table[256];

/* Not copyrighted 1990 Mark Adler */

/*
  Generate a table for a byte-wise 32-bit CRC calculation on the polynomial:
  x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1.

  Polynomials over GF(2) are represented in binary, one bit per coefficient,
  with the lowest powers in the most significant bit.  Then adding polynomials
  is just exclusive-or, and multiplying a polynomial by x is a right shift by
  one.  If we call the above polynomial p, and represent a byte as the
  polynomial q, also with the lowest power in the most significant bit (so the
  byte 0xb1 is the polynomial x^7+x^3+x+1), then the CRC is (q*x^32) mod p,
  where a mod b means the remainder after dividing a by b.

  This calculation is done using the shift-register method of multiplying and
  taking the remainder.  The register is initialized to zero, and for each
  incoming bit, x^32 is added mod p to the register if the bit is a one (where
  x^32 mod p is p+x^32 = x^26+...+1), and the register is multiplied mod p by
  x (which is shifting right by one and adding x^32 mod p if the bit shifted
  out is a one).  We start with the highest power (least significant bit) of
  q and repeat for all eight bits of q.

  The table is simply the CRC of all possible eight bit values.  This is all
  the information needed to generate CRC's on data a byte at a time for all
  combinations of CRC register values and incoming bytes.
*/
void adler_init()
{
  u32 c;          /* crc shift register */
  u32 e;          /* polynomial exclusive-or pattern */
  unsigned int i; /* counter for all possible eight bit values */
  int k;          /* byte being shifted into crc apparatus */

  /* terms of polynomial defining this crc (except x^32): */
  static int p[] = {0,1,2,4,5,7,8,10,11,12,16,22,23,26};

  /* Make exclusive-or pattern from polynomial (0xedb88320) */
  e = 0;
  for (i = 0; i < sizeof(p)/sizeof(int); i++) {
    e |= 1L << (31 - p[i]);
  }

  /* Compute table of CRC's */
  for (i = 1; i < 256; i++) {
    c = i;
    /* The idea to initialize the register with the byte instead of
     * zero was stolen from Haruhiko Okumura's ar002 */
    for (k = 8; k; k--) {
      c = c & 1 ? (c >> 1) ^ e : c >> 1;
    }
    crc32_table[i] = c;
  }
} /* End of adler.init */

/* Add some bytes to an existing CRC */
u32 crc32 (u32 crc, u8 *buf, long int len)
{
  u8 *end; int i;
  crc = ~crc;
  for (end = buf + len; buf < end; ++buf) {
    i = (crc ^ *buf) & 0xff;
    crc = crc32_table[i] ^ (crc >> 8);
  }
  return ~crc;
} /* End of crc.32 */

u32 crc32_add_u16(u32 crc, u16 x)
{
  u8 t[2];
  /* Convert u16 to 2 bytes (little-endian) */
  t[0] = x & 0xFF; t[1] = (x >> 8) & 0xFF;
  return crc32(crc, t, 2);
}

/* Two-argument form of crc32, for use when you already have all the bytes
   assembled in one buffer. */
u32 buf_crc(u8 *buf, long int len)
{
  u32 crc;
  crc = 0;
  crc = crc32(crc, buf, len);
  return crc;
}

/* ----------------------- simple utility functions  ----------------------- */

int iabs(int x)
{
  if (x < 0) { return (0 - x); }
  return x;
}

int minmax(int x, int min, int max)
{
  if (x < min) { return min; }
  if (x > max) { return max; }
  return x;
}

int min(int a, int b)
{
  if (a < b) { return a; }
  return b;
}

int max(int a, int b)
{
  if (a > b) { return a; }
  return b;
}

/* Convert a single-letter world name like 'C' to the fnum index (in
this case 2). The second return value 'valid' (if given) tells whether
the input was in range. */
int char_to_fnum(char c, int * valid)
{
  int rv;

  if (valid) { *valid = 1; }
  if (c >= 'a') {
    c = c - ('a' - 'A');
  }
  rv = c - 'A';
  if (rv < 0) {
    rv = 0;
    if (valid) { *valid = 0; }
  }
  if (rv >= g_n_fnum) {
    rv = g_n_fnum - 1;
    if (valid) { *valid = 0; }
  }
  return rv;
} /* End of char.to_fnum */

char fnum_to_char(int fnum)
{
  if (fnum < 0) {
    return '-';
  } else if (fnum > ('Z'-'A')) {
    return '-';
  }
  fnum = minmax(fnum, 0, ('Z'-'A'));
  return('A' + fnum);
} /* End of fnum.to_char */

void die1(const char * s)
{
  fflush(stdout);
  fprintf(stderr, "%s", s);
  exit(-1);
}

/* Get a little-endian signed 16-bit quantity from the file. */
int get_le_s16(FILE * f)
{
  int rv;
  unsigned char c[2];
  size_t got;
  got = fread(c, 1, 2, f);
  if (got < 2) {
    die1("get_le_s16: could not read 2 chars.\n");
  }
  rv = (c[1] << 8) | c[0];
  if (rv & 0x8000) {
    rv -= 0x10000;
  }
  return rv;
} /* End of get.le_s16 */

/* Get a little-endian unsigned 32-bit quantity from the file. */
unsigned long get_le_u32(FILE * f)
{
  unsigned long rv;
  unsigned char c[4];
  size_t got;
  got = fread(c, 1, 4, f);
  if (got < 4) {
    die1("get_le_u32: could not read 4 chars.\n");
  }
  rv = c[3];
  rv = (rv << 8) | c[2];
  rv = (rv << 8) | c[1];
  rv = (rv << 8) | c[0];
  return rv;
} /* End of get.le_u32 */

/* Get a little-endian unsigned 64-bit quantity from the file. */
unsigned long get_le_u64(FILE * f)
{
  unsigned long rv;
  unsigned char c[8];
  size_t got;
  got = fread(c, 1, 8, f);
  if (got < 8) {
    die1("get_le_u64: could not read 8 chars.\n");
  }
  rv = c[7];
  rv = (rv << 8) | c[6];
  rv = (rv << 8) | c[5];
  rv = (rv << 8) | c[4];
  rv = (rv << 8) | c[3];
  rv = (rv << 8) | c[2];
  rv = (rv << 8) | c[1];
  rv = (rv << 8) | c[0];
  return rv;
} /* End of get.le_u64 */

#define MAXTOC 1048576L
long g_toc_used = 0;
void toc_init(void)
{
  g_toc = (tc_entry *) calloc(MAXTOC, sizeof(tc_entry));
  if (g_toc == 0) {
    fflush(stdout);
    fprintf(stderr, "toc_init: Could not allocate %ld bytes"
          " for %ld-entry TOC\n", ((long)(MAXTOC * sizeof(tc_entry))), MAXTOC);
    exit(-1);
  }
}

long g_num_tocdump;

/* Check to make sure there is at least one open file */
void check_nfnum(void)
{
  if (g_n_fnum < 0) {
    die1("check_nfnum: g_n_fnum negative.\n");
  }
}

void scan_name(char * dst, int fnum, int verbose,
 char * ofname, char * of_suffix, int vncol)
{
  int err, gg;
  int got;
  char * s; char * o;

  check_nfnum();

  /* Try to get the name of the world */
  err = fseek(g_edat[fnum], 40, SEEK_SET);
  if (err) {
    die1("scan.name: failed seek to posn 40 for name.\n");
  }
  {
    got = fread(dst, 1, MAX_NAME_LEN, g_edat[fnum]);
    dst[MAX_NAME_LEN] = 0;
    if (got > 0) {
      s = dst;
      o = ofname;
      gg = 1;
      while (gg) {
        char c;
        c = *s;
        if (c) {
          if (c == '\'') {
            c = '.';
            *o++ = c;
          } else if (c == ' ') {
            c = '_';
            *o++ = c;
          } else if ((c >= '0') && (c <= '9')) {
            /* Digit, okay */
            *o++ = c;
          } else if ((c >= 'a') && (c <= 'z')) {
            /* LC letter, okay */
            *o++ = c;
          } else if ((c >= 'A') && (c <= 'Z')) {
            /* UC letter, okay */
            *o++ = c;
          } else {
            /* Illegal char, what to do? */
          }

          s++;
        } else {
          /* End of input string */
          gg = 0;
        }
        *o = 0;
      }
      if (of_suffix) {
        strncat(ofname, of_suffix, sizeof(of_name)-strlen(ofname)-1);
      }
      strncat(ofname, ".ppm", sizeof(of_name)-strlen(ofname)-1);
      if (verbose) {
        printf("World %c, name: [", fnum_to_char(fnum));
        ansiset(vncol);
        printf("%s", dst);
        ansiset(0);
        printf("]\n");
      }
      if (verbose > 1) {
        printf("'fmg' output filename will be: %s\n", ofname);
      }
    }
  }

  if (verbose > 2) {
    int i;
    char header[E17_HEADER_SIZE];

    /* Display all printable characters in the header */
    err = fseek(g_edat[fnum], 0, SEEK_SET);
    if (err) {
      die1("scan.name: failed seek to posn 0 for header.\n");
    }
    printf ("All printable chars: '");
    got = fread(header, 1, E17_HEADER_SIZE, g_edat[fnum]);
    for(i=040; i<got; i++) {
      char c;
      c = header[i];
      if ((c >= ' ') && (c <= '~')) {
        printf("%c", c);
      } else {
        /* printf(" "); */
      }
    }
    printf("'\n");
  }
} /* End of scan.name */

/* Eden header format (for versions 1.1.1 through 1.7)

  A header typically looks like this example (taken from world
1316353956.eden "__FOOTBALL_STADIUM_by_MATTHEW"):
  0000: ab c1 01 00 ae fc 7f 47 66 66 c7 41 fe 2d 80 47          Gff A - G
  0010: 00 04 80 47 00 00 58 42 00 04 80 47 00 00 bf c2      G  XB   G    
  0020: c0 00 ff 00 00 00 00 00 20 20 46 4f 4f 54 42 41             FOOTBA
  0030: 4c 4c 20 53 54 41 44 49 55 4d 20 62 79 20 4d 41   LL STADIUM by MA
  0040: 54 54 48 45 57 00 2f 00 49 00 4d 00 47 00 5f 00   TTHEW / I M G _ 
  0050: 30 00 30 00 34 00 31 00 2e 00 4a 00 02 00 00 00   0 0 4 1 . J     
  0060: 65 31 36 33 37 63 61 66 32 34 61 31 61 63 33 34   e1637caf24a1ac34
  0070: 33 65 39 39 31 66 38 35 65 61 39 66 63 31 64 35   3e991f85ea9fc1d5
  0080: 00 00 65 00 64 00 69 00 61 00 2f 00 44 00 43 00     e d i a / D C 
  0090: 49 00 4d 00 2f 00 31 00 30 00 30 00 41 00 50 00   I M / 1 0 0 A P 
  00a0: 50 00 4c 00 45 00 2f 00 2e 00 4d 00 49 00 53 00   P L E / . M I S 
  00b0: 43 00 00 00 00 00 00 00 00 00 00 00 00 00 0c 00   C               

Known fields include:
  0000: "ab c1 01 00": unknown, presumed to be terrain seed (32-bit
    unsigned integer, 0001c1ab = 115115)
  0004: "ae fc 7f 47": east-west position as a little-endian 32-bit
    floating-point number (477ffcae = 0.10001110.11111111111110010101110 =
    65532.68)
  0008: "66 66 c7 41": vertical position (41c76666 = 24.925)
  000c: "fe 2d 80 47": north-south position (47802dfe = 65627.984)
  0010: "00 04 80 47": east-west position of warp-home (47800400 = 65544.0)
  0014: "00 00 58 42": vertical position of warp-home (42580000 = 54.0)
  0018: "00 04 80 47": north-south position of warp-home (47800400 = 65544.0)
  001c: "00 00 bf c2": rotation of camera (view) around vertical axis
    (c2bf0000 = -95.5) (note that the game does not change your rotation
    when you warp home, and it forgets your "tilt" on exit/reload)
  0020: "c0 00 ff 00 00 00 00 00": base address of chunk table-of-contents
    (file offset 0x00ff00c0) This can be huge (in theory up to 35 TB)
  0028: "20 20 46 4f 4f ...": world name in ASCII, null-terminated
    (may be up to 36 ASCII characters = 37 bytes total)
  004d-00bf: unknown, presumed unused (seems to contain ASCII and 16-bit
    Unicode characters including parts of filenames and pathnames,
    as seen in the example here)

 */
void cmd_header(int fnum)
{
  int row, col, i, got, err;
  unsigned char header[E17_HEADER_SIZE];
  unsigned char c;

  /* Display all printable characters in the header */
  err = fseek(g_edat[fnum], 0, SEEK_SET);
  if (err) {
    fflush(stdout);
    fprintf(stderr, "cmd.header: failed seek to posn 0 for header.\n");
    return;
  }
  printf ("Header for world %d:\n", fnum);
  got = fread(header, 1, E17_HEADER_SIZE, g_edat[fnum]);
  for(row=0; (16*row)<got; row++) {
    printf("  %04x: ", 16*row);
    for(col=0; col<16; col++) {
      i = 16*row+col;
      if ((16*row+col) < got) {
        if (col>0) { printf(" "); }
        c = header[i];
        printf("%02x", ((unsigned int) c));
      } else {
        printf("   ");
      }
    }

    printf("   ");

    for(col=0; (col<16) && ((16*row+col)<got); col++) {
      i = 16*row+col;
      c = header[i];
      if ((c >= ' ') && (c <= '~')) {
        printf("%c", ((char) c));
      } else {
        printf(" ");
      }
    }

    printf("\n");
  }
} /* End of cmd.header */

float getfloat(unsigned char *p)
{
  volatile float x;
  float * px;
  unsigned int t;
  unsigned int * pi;
  int i;
  float rv;

  x = 0.0f;
  rv = x;
  if (p) {
    px = (float *) (&x);
    pi = (unsigned int *) px;
    t = 0;
    /* Convert little-endian bytes into 32-bit unsigned integer */
    for(i=3; i>=0; i--) {
      t = (t << 8) | ((int) p[i]);
    }
    *pi = t;
    rv = x;
  }
  return rv;
} /* End of get.float */

/* Display the known fields of the world's header */
void cmd_hf(int fnum) /* formerly cmd_hfields */
{
  int seed, got, err;
  unsigned char header[E17_HEADER_SIZE];

  /* Display all printable characters in the header */
  err = fseek(g_edat[fnum], 0, SEEK_SET);
  if (err) {
    fflush(stdout);
    fprintf(stderr, "cmd.hfields: failed seek to posn 0 for header.\n");
    return;
  }
  printf ("Known fields in header of world %d:\n", fnum);
  got = fread(header, 1, E17_HEADER_SIZE, g_edat[fnum]);
  if (got > 48) {
    /* We got enough */
  }
  seed = ((int) header[3]);
  seed <<= 8;
  seed |= ((int) header[2]);
  seed <<= 8;
  seed |= ((int) header[1]);
  seed <<= 8;
  seed |= ((int) header[0]);

  printf("      Seed: %d \n", seed);

  printf("  Posn N-S: %f \n", getfloat(header+0x0c));
  printf("  Posn E-W: %f \n", getfloat(header+0x04));
  printf("  Pos vert: %f \n", getfloat(header+0x08));
  printf("  Rotation: %f \n", getfloat(header+0x1c));

  printf("  Home N-S: %f \n", getfloat(header+0x18));
  printf("  Home E-W: %f \n", getfloat(header+0x10));
  printf("  Hme vert: %f \n", getfloat(header+0x14));

  /* %%% Do a safe_strncpy of the world name, remove non-ASCII characters
     and print */
} /* End of cmd.hfields */

s32 get_s32(unsigned char *p)
{
  u32 t;

  t = ((u32) p[3]);
  t <<= 8;
  t |= ((int) p[2]);
  t <<= 8;
  t |= ((int) p[1]);
  t <<= 8;
  t |= ((int) p[0]);

  return((s32)t);
}

#define MOBRECLEN 60

/* Display mobs (creatures) The data format (per creature) is:

   0 position east-west
   4 position z
   8 position north-south
  12 velocity east-west
  16 velocity z
  20 velocity north-south
  24 angle
  28 type (0=Moof, 1=Batty, 2=Green, 3=Nergle, 4=Stumpy, 5=Charger, 6=Stalker)
  32 colour
  36 health
  40 extra2 (float)
  44 extra3 (float)
  48 extra4 (three floats)
  (total length: 60 bytes)

The original Eden source code defines this as struct "EntityData" in
Classes/Vector.h and the "species" numbers are in Constants.h
 */
void cmd_mobs(int fnum, int verbose)
{
  long mobstart;
  long mo, i;
  int got, err;
  unsigned char amob[MOBRECLEN];
  float x, z, y, vx, vz, vy, rot, health;
  int type, colour, nerr, population;

  mobstart = g_mobstart[fnum];
  err = fseek(g_edat[fnum], mobstart, SEEK_SET);
  if (err) {
    fflush(stdout);
    fprintf(stderr, "cmd.mobs: failed seek to posn 0 for header.\n");
    return;
  }
  nerr = 0; population = 0;
  for(mo=0; mo<12000; mo+=MOBRECLEN) {
    got = fread(amob, 1, MOBRECLEN, g_edat[fnum]);
    if (got < MOBRECLEN) {
      /* We got enough */
      fflush(stdout);
      fprintf(stderr, "cmd.mobs: Could not read %d bytes.\n", MOBRECLEN);
      return;
    }
    type = get_s32(amob+28); /* creature's "species" */
    colour = get_s32(amob+32);
    if (type == -1) {
      /* Unused mob. Check the rest of the bytes to make sure they're all 0 */
      err = 0;
      for(i=0; i<28; i++) { if (amob[i] != 0) { err = 1; } }
      for(i=32; i<MOBRECLEN; i++) { if (amob[i] != 0) { err = 1; } }
      if (err) {
        nerr++;
        if (nerr <= 5) {
          printf("Nonzero data in unused mob #%ld\n", mo/MOBRECLEN);
        }
      }
    } else if ((colour < 0) || (colour > 56)) {
      /* Illegal colour */
      nerr++;
      if (nerr <= 5) {
        printf("Illegal colour for mob #%ld\n", mo/MOBRECLEN);
      }
    } else {
      population++;
      x = getfloat(amob);
      z = getfloat(amob+4);
      y = getfloat(amob+8);

      vx = getfloat(amob+12);
      vz = getfloat(amob+16);
      vy = getfloat(amob+20);
      rot = getfloat(amob+24);
      health = getfloat(amob+36);

      if (verbose) {
        printf("(x, y, z) = (%-8.2g, %-8.2g, %-8.2g)", x, y, z);
        printf(" type %d colour %2d", type, colour);
        if (vx != 0) { printf(" vx=%-8.2g", vx); } else { printf("%12s", ""); }
        if (vy != 0) { printf(" vy=%-8.2g", vy); } else { printf("%12s", ""); }
        if (vz != 0) { printf(" vz=%-8.2g", vz); } else { printf("%12s", ""); }
        if (rot !=0) { printf(" rot=%-8.2g",rot); } else {printf("%13s", ""); }
        if (health!=0){printf(" H=%-8.2g", health);}else {printf("%11s", ""); }
        printf("\n");
      }

      err = 0;
      for(i=40; i<MOBRECLEN; i++) { if (amob[i] != 0) { err = 1; } }
      if (err) {
        nerr++;
        if (nerr <= 5) {
          printf("Nonzero data at end of mob #%ld\n", mo/MOBRECLEN);
        }
      }
    }
    if (verbose > 1) {
      for (i=0; i<MOBRECLEN; i++) {
        printf(" %02hhx", amob[i]);
      }
      printf("\n");
    }
  }
  printf("%d active mobs", population);
  if (nerr) {
    printf(", %d mob entries have errors\n", nerr);
  } else {
    printf(", no errors found.\n");
  }
} /* End of cmd.mobs */

/* Show all worlds and their names */
void allnames(void) /* a.k.a. cmd_lw */
{
  int i;
  char namtemp[50];

  printf("All available worlds ('*' is current selection):\n");
  for(i=0; i<g_n_fnum; i++) {
    printf("World '%c' ", fnum_to_char(i));

    if (i == g_cur_file) {
      printf("* ");
    } else {
      printf("  ");
    }

    snprintf(namtemp, sizeof(namtemp), "[%s]", g_wnames[i]);
    printf("%38s  %s\n", namtemp, g_fnames[i]);
  }
} /* End of all.names */

#define MAXHASH (1<<16)

/* Perform a hashfunction on three integers, which can be arbitrary but
   should change from one call to the next, and return an integer in
   the range [0..MAXHASH) */
int hash_wxy(int w, int x, int y)
{
#ifdef HASH_SIMPLE1
  return(((w*143) ^ (x*7) ^ y) % MAXHASH);
#else
# ifdef HASH_SIMPLE2
  return(((w*1011) ^ (x*143) ^ (y*27)) % MAXHASH);
# else
  int ht[3]; unsigned int rv;
  ht[0] = w; ht[1] = x; ht[2] = y;
  rv = fnv1x(((char *)(ht)), 3L*((long)(sizeof(int))), 143);
  return ((int)(rv % MAXHASH));
# endif
#endif
} /* End of hash.wxy */

int randi(int a, int b, int c, unsigned int lim)
{
  int ht[3]; unsigned int rv;
  ht[0] = a; ht[1] = b; ht[2] = c;
  rv = fnv1x(((char *)(ht)), 3L*((long)(sizeof(int))), 143);
  if (lim <= 65536) {
    /* Chongo 16-bit fold */
    rv = (rv >> 16) ^ rv;
  }
  return ((int)(rv % lim));
}

long hl_first[MAXHASH];

void hl_init(void)
{
  int i;
  for(i=0; i<MAXHASH; i++) {
    hl_first[i] = -1;
  }
}

/* Get a single TOC entry */
int e17_get_tcent(int fnum, long tcnum,
  int *x, int *z1, int *y, int * z2, long * cpos, long mobstart,
  int verbose)
{
  int err;
  long tpos;

  tpos = g_flen[fnum] - (16L * tcnum);
  if (tpos < 192) {
    return -1;
  }

  err = fseek(g_edat[fnum], tpos, SEEK_SET);
  if (err) {
    fflush(stdout);
    fprintf(stderr, "e17.get_tcent: failed seek to %ld in '%s'.\n",
      tpos, g_fnames[fnum]);
    exit(-1);
  }

  *x = get_le_s16(g_edat[fnum]); *z1 = get_le_s16(g_edat[fnum]);
  *y = get_le_s16(g_edat[fnum]); *z2 = get_le_s16(g_edat[fnum]);
  *cpos = get_le_u64(g_edat[fnum]);

  /* Show a few of the TOC entries as a sanity check. Here we show the
     first few blocks and the last few blocks */
  if (verbose && ((*cpos < 0x20000) || (*cpos >= mobstart - 0x20000))) {
    if (g_num_tocdump < 20) {
      if (g_num_tocdump == 0) {
        printf(" Showing first and last TOC entries:\n");
        printf(" %4s %4s %4s %4s %16s\n",
                                  ".X.", "-", ".Y.", "-", "data loc in file");
      }
      printf(" %4X %4X %4X %4X %16lX\n",
                                 *x, *z1, *y, *z2, ((unsigned long) (*cpos)));
      fflush(stdout);
    }
    g_num_tocdump++;
  }

  return 0;
} /* End of e17.get_tcent */

long g_nerr_cpos;

/* Test a table-of-contents entry to see if we consider it 'valid'. This
   includes having 0 in the unused bytes, and "reasonable" coordinates. */
int e17_tc_valid(int x, int z1, int y, int z2, long cpos,
                 long mobstart, long hdrend)
{
  int rv;

  rv = 1;

  if (z1 || z2) {
    /* If any of these fields is nonzero it can't be a TOC entry */
    rv = 0;
  } else if ((x < 0) || (x > 32767) || (y < 0) || (y > 32767)) {
    /* Eden only supports chunk addresses from 0 to 32767. I know of only
       one series of worlds that demonstrate this. An example from 20170329
       is called "Geometry_World_PHX_676". The file is 129184494 bytes
       compressed and 10557115872 bytes (10.5 GB) when uncompressed.
       Its chunk addresses range from 0 to 4214 in X and 0 to 32767 in Y.
       It has a total of 322020 chunks.
         More typically, worlds have all their chunk X and Y near 0x1000,
       which is 4096 in decimal. The biggest worlds, like Hidden Galaxy,
       Atlanta GA Skyline, riverbeta, Tokyo Unified, etc. only go a few
       hundred chunks away from 4096 in any direction. riverbeta is only
       180x180 chunks in size which is smaller than Hidden Galaxy or
       Atlanta (it's a bigger file because the whole 180x180 is filled in
       and Atlanta or Hidden Galaxy have a lot of empty space).
         The size of a world in "pixels" (individual blocks) is 16 times
       as big, so for example Atlanta is about 5800 in the longer direction
       and Geometry_World_PHX_676 is a staggering 524288 blocks from north
       to south. At game scale, assuming each block is 1 meter, that's
       524 km or 326 miles. */
    if (g_nerr_cpos < 5) {
      printf("Got TOC entry saying that (%3d, %3d) is at %lX.\n", x, y, cpos);
      printf("The (x,y) looks invalid.\n");
    }
    g_nerr_cpos++;
    rv = 0;
  } else if (cpos < hdrend) {
    /* We found a block that starts too low */
    if (g_nerr_cpos < 5) {
      printf("Got TOC entry saying that (%3d, %3d) is at %lX.\n", x, y, cpos);
      printf("The cpos is too low.\n");
    }
    g_nerr_cpos++;
    rv = 0;
  } else if (cpos >= mobstart) {
    /* We found a block that starts too high */
    if (g_nerr_cpos < 5) {
      printf("Got TOC entry saying that (%3d, %3d) is at %lX.\n", x, y, cpos);
      printf("The cpos is too high.\n");
    }
    g_nerr_cpos++;
    rv = 0;
  } else if (0 && ((cpos & 0x7fff) != (hdrend & 0x7fff)) ) {
    /* This chunk does not align with the end of the file header */
    if (g_nerr_cpos < 5) {
      printf("Got TOC entry saying that (%3d, %3d) is at %lX.\n", x, y, cpos);
      printf("The cpos is misaligned.\n");
    }
    g_nerr_cpos++;
    rv = 0;
  }
  return rv;
} /* End of e17.tc_valid */

long g_next_2439;

/* Get the table of contents from an Eden 1.7 file */
int e17_get_toc(int fnum)
{
  int err;
  long numtoc, tocbase, tocsize;
  long i, j, k, flen, num_invalid;
  int x, z1, y, z2; long cpos;
  long hdrend, tocstart, mobstart, fsz_chunks;

  /* printf("e17.get_toc: fnum %d\n", fnum); */

  hdrend = 0x00C0L;

  /* Look for world name */
  scan_name(g_name, fnum, 3, of_name, 0, 0);

  printf("e17.get_toc: Checking length...\n");

  /* Find out how long the file is */
  err = fseek(g_edat[fnum], 0, SEEK_END);
  if (err) {
    fflush(stdout);
    fprintf(stderr, "e17.get_toc: failed seek to EOF of '%s'.\n",
                                                             g_fnames[fnum]);
    exit(-1);
  }
  flen = ftell(g_edat[fnum]);
  if (flen < 0) {
    fflush(stdout);
    fprintf(stderr, "e17.get_toc: ftell failed in '%s'.\n", g_fnames[fnum]);
    exit(-1);
  }
  g_flen[fnum] = flen;
  if (flen < 0x10000) {
    /* This is ridiculously small, not even big enough to be a new blank
       world */
    fprintf(stderr, "e17.get_toc: file EOF is at %ld", flen);
    die1(", this world is too small.\n");
  }

#if 0
  if (flen >= 4294967296L) {
    die1("e17.get_toc: cannot handle world files larger than 4 GiB!\n");
  }
#endif

  i = 0xc0;
  printf("e17.get_toc: first chunk should be %3ld == %016lX\n",
                                                        i, (unsigned long) i);

  /* TOC start pointer should be a 32-bit little-endian value starting
     at offset 0x20 in the header. */
  err = fseek(g_edat[fnum], 0x20, SEEK_SET);
  tocstart = get_le_u64(g_edat[fnum]);
  g_tocstart[fnum] = tocstart;

  /* Mobs should be stored in the 12,000 bytes after the last block and before
     the TOC. */
  mobstart = tocstart - 12000;

  fsz_chunks = (mobstart - hdrend) / 0x8000L;
  printf("e17.get_toc: there is room for %7ld chunks.\n", fsz_chunks);

  printf("e17.get_toc: mob start is  %11ld == %016lX\n",
                                          mobstart, (unsigned long) mobstart);
  g_mobstart[fnum] = mobstart;
  if ((mobstart & 0x7FFFL) != hdrend) {
    die1("e17.get_toc: mobstart % 32768 != 0x00C0\n");
  }

  printf("             "); cmd_mobs(fnum, 0);

  printf("e17.get_toc: TOC begins at %11ld == %016lX\n",
                                          tocstart, (unsigned long) tocstart);

  /* This should be well past the middle of the file, but less than the file
     size, and by a multiple of 16 */
  if (tocstart < ((flen / 4L) * 3L)) {
    die1("e17.get_toc: TOC pointer is way too low!\n");
  }
  if (tocstart >= flen) {
    die1("e17.get_toc: TOC pointer is past end of file!\n");
  }
  if (tocstart >= (flen - 64)) {
    die1("e17.get_toc: TOC pointer is too close to end of file!\n");
  }
  k = flen - tocstart;
  if (k & 0x0f) {
    fflush(stdout);
    fprintf(stderr, "e17.get_toc: TOC length is not a multiple of 16"
                            " (extra %d bytes at end)\n", ((int)(k & 0x0f)));
    exit(-1);
  }
  tocsize = k / 16L;
  printf("e17.get_toc: scanning      %11ld TOC entries...\n", tocsize);
  if (fsz_chunks > tocsize) {
    die1("e17.get_toc: there are more chunks than TOC entries!\n");
  } else if (fsz_chunks < tocsize) {
    die1("e17.get_toc: there are fewer chunks than TOC entries!\n");
  }

  /* Store base pointer */
  tocbase = g_toc_used;
  g_tocbase[fnum] = tocbase;
  /* Make sure we'll have enough room for a TOC of this size */
  if ((tocbase + tocsize) > MAXTOC) {
    die1("e17.get_toc: I can't handle this many TOC entries!\n");
  }

  /* All blocks should be at multiples-of-0x2000 offsets below the mob table */
  /* Scan backwards from EOF for chunk pointers. */
  j = 0; num_invalid = 0; g_nerr_cpos = 0; g_num_tocdump = 0;
  g_next_2439 = 0x00004a89;
  for(i=1; i<=tocsize; i++) {
    int hash;

    err = e17_get_tcent(fnum, i, &x, &z1, &y, &z2, &cpos, mobstart, 1);
    if (err) {
      /* Skip this one */
    } else {
      k = e17_tc_valid(x, z1, y, z2, cpos, mobstart, hdrend);
      if (k) {
        /* It seems this is a good one */
        g_toc[tocbase+j].w = fnum;
        g_toc[tocbase+j].x = x;
        g_toc[tocbase+j].y = y;
        g_toc[tocbase+j].fpos = cpos;
        g_toc[tocbase+j].adjf = 0;

        /* Add ourselves to the start of the linked list for our hash */
        hash = hash_wxy(fnum, x, y);
        g_toc[tocbase+j].hl_next = hl_first[hash];
        hl_first[hash] = tocbase+j;

        /* This code saves the file-position of the first chunk whose
           data comes after 0x00002439; we use this to dump an Eden 2.0
           compressed chunk */
        if ((cpos > 0x00002439) && (cpos < g_next_2439)) {
          g_next_2439 = cpos;
        }

        j++;
      } else {
        num_invalid++;
      }
    }
  }
  numtoc = j;
  if (num_invalid) {
    printf("There are %ld invalid TOC entries\n", num_invalid);
  } else {
    printf("All TOC entries are valid.\n");
  }

  if (numtoc <= 0) {
    fflush(stdout);
    fprintf(stderr,
         "e17.get_toc: I found no valid TOC entries; this file is invalid.\n");
    return 0;
  }

  printf("e17.get_toc: file EOF is at %11ld == %016lX\n",
                                                flen, ((unsigned long) flen));

  /* Save the number of chunks for this file */
  g_nc[fnum] = numtoc;

  /* printf("e17.get_toc: Found %ld chunks in TOC.\n", numtoc); */
  /* printf("e17.get_toc: Took %ld TOC bytes for file %d.\n",
    numtoc * sizeof(tc_entry), fnum); */

  /* Update g_toc_used with the amount we actually used */
  g_toc_used += numtoc;

  printf("e17.get_toc: Loaded %ld TOC entries.\n", g_nc[fnum]);

  return 1;
} /* End of e17.get_toc */

#define TTUF 200000000
long trickle_ops;

long toc_lookup(int fnum, int x, int y)
{
  long i;

  for(i=0; i<g_toc_used; i++) {
    trickle_ops++;
    if ((g_toc[i].w == fnum) && (g_toc[i].x == x) && (g_toc[i].y == y)) {
      return i;
    }
  }
  return -1;
} /* End of toc.lookup */

/* Hashed lookup */
long toc_hlu(int fnum, int x, int y)
{
  long i;
  int hash;

  hash = hash_wxy(fnum, x, y);
  i = hl_first[hash];
  while (i >= 0) {
    trickle_ops++;
    if ((g_toc[i].w == fnum) && (g_toc[i].x == x) && (g_toc[i].y == y)) {
      return i;
    }
    i = g_toc[i].hl_next;
  }
  return -1;
} /* End of toc.hlu */

void toc_trytag(int fnum, int x, int y, int t)
{
  long i;
  if ((x < 0) || (y < 0)) { return; }
  i = toc_hlu(fnum, x, y); /* i = toc_lookup(x, y); */
  if (i >= 0) {
    if (g_toc[i].adjf == 0) {
      g_toc[i].adjf = t;
    }
  }
  trickle_ops++;
} /* End of toc.trytag */

int cur_ns;
int cur_we;
int g_no_4096 = 0;
int ns_max, ns_min, we_max, we_min; /* edges of 'srs' */
int g_cur_wid = (80 - 4)/2;
int g_cur_hei = (80 - 4)/2;
#define MAX_MARKS 10
int marks_ns[MAX_MARKS];
int marks_we[MAX_MARKS];

void init_marks(void)
{
  int i;
  for(i=0; i<MAX_MARKS; i++) {
    marks_ns[i] = 65536;
    marks_we[i] = 65536;
  }
}

void set_limits(void)
{
  /* printf("%d < ns < %d  ;  %d < we < %d\n",ns_min,ns_max,we_min,we_max); */
  ns_max = cur_ns+(g_cur_hei/2);
  ns_min = ns_max - g_cur_hei;
  we_max = cur_we+(g_cur_wid/2);
  we_min = we_max - g_cur_wid;
  /* printf("%d < ns < %d  ;  %d < we < %d\n",ns_min,ns_max,we_min,we_max); */
}

/* In Eden 1.7 and earlier, when you make a brand-new world, Eden
   always puts you on the block that is in the "northwest" corner of
   chunk (4096, 4096). The floating-point coordinates for this are
   (65544.0, 65544.0).

Coordinates increase to the north and west. The most "natural" block
coordinate would be just 16 times the chunk coordinate, and 16*4096 is
65536. If you view the (4096,4096) chunk from above looking down (like
on a map) with north at the top, then the home position (65544.0,
65544.0) would be in the upper-left corner (the 1st block in the 1t
row). The block with coordinates (65536,65536) would be the 9th block
in the 9th row.
  All blocks from 65544 to 65529 fall within chunk 4096.
  To convert a block number to a chunk number, add 7, then divide by
  16, and discard the fraction.
*/
void blockxy_to_chunkxy(int bx, int by, int * cx, int * cy)
{
  if (cx) { *cx = (bx+8)/16; }
  if (cy) { *cy = (by+8)/16; }
}

/*
  To convert a chunk number to the block at its northwest corner,
  multily by 16 and add 8.
*/
void chunkxy_to_blockxy(int x, int y, int bias, int * bx, int * by)
{
  if (bx) { *bx = x*16+bias; }
  if (by) { *by = y*16+bias; }
}

void gochunk(int sx, int sy)
{
  chunkxy_to_blockxy(sx, sy, 7, &cur_we, &cur_ns);
  set_limits();
}

void go_1st_spawn(void) /* a.k.a. cmd_spawn1 */
{
  gochunk(4096, 4096);
}

void trickle_toc(int fnum, int setpos)
{
  long i, j, pass;
  int sx, sy, lastx, lasty;
  int gg;

  /* %%% This can be used if the compiler refuses to cast "time(0)' to a long:
  struct tm tm0;
  time_t tm0t, tm1t;
  long tmdf;

  tm0.tm_sec = 0;
  tm0.tm_min = 0;
  tm0.tm_hour = 0;
  tm0.tm_mday = 1;
  tm0.tm_mon = 0;
  tm0.tm_year = 70;
  tm0.tm_isdst = 0;
  tm0.tm_gmtoff = 0;
  tm0t = mktime(&tm0);
  tm1t = time(0);
  tmdf = (long)(difftime(tm1t, tm0t));
  */

  /* printf("trickle.toc starting on file %d (%s)\n", fnum, g_fnames[fnum]); */

  trickle_ops = TTUF - 100000;
  sx = sy = 4096; g_no_4096 = 0;
  i = toc_hlu(fnum, sx, sy);
  if (i < 0) {
    g_no_4096 = 1;
    printf("trickle.toc: There is no chunk (%d, %d)", sx, sy);
    /* j = tmdf % g_nc[fnum]; */
    j = ((long)(time(0))) % g_nc[fnum];
    printf("g_nc[%d] == %ld, base==%ld\n", fnum, g_nc[fnum], g_tocbase[fnum]);
    sx = g_toc[g_tocbase[fnum]+j].x;
    sy = g_toc[g_tocbase[fnum]+j].y;
    printf("; using chunk %ld (%d, %d) instead.\n", j, sx, sy);
    i = toc_hlu(fnum, sx, sy);
    printf(" got index %ld, should be %ld\n", i, g_tocbase[fnum]+j);
  }
  g_xmin[fnum] = g_xmax[fnum] = sx;
  g_ymin[fnum] = g_ymax[fnum] = sy;
  if (setpos) {
    gochunk(sx, sy);
  }

  /* sx and sy will get re-used as temps in the trickle algorithm */

  g_toc[i].adjf = 1;
  g_toc[i].mark1 = 0;
  gg = 1; pass = 0;
  g_trickle_found = 0;
  lastx = lasty = 0;
  while(gg) {
    long tocbase;
    pass++;
    /* printf("trickle.toc: pass %ld, found %ld so far...\n",
      pass, g_trickle_found); */

    gg = 0;

    tocbase = g_tocbase[fnum];
    /* Find each cell that is active */
    for(j=0; j<g_nc[fnum]; j++) {
      trickle_ops++;
      if (g_toc[tocbase+j].adjf == 1) {
         gg = 1;  /* Found an active cell, so keep the search alive */

         /* Try to tag the 4 neighbours */
         sx = g_toc[tocbase+j].x; sy = g_toc[tocbase+j].y;

         toc_trytag(fnum, sx+1, sy, 3);
         toc_trytag(fnum, sx-1, sy, 3);
         toc_trytag(fnum, sx, sy+1, 3);
         toc_trytag(fnum, sx, sy-1, 3);

         if (sx < g_xmin[fnum]) { g_xmin[fnum] = sx; }
         if (sx > g_xmax[fnum]) { g_xmax[fnum] = sx; }
         if (sy < g_ymin[fnum]) { g_ymin[fnum] = sy; }
         if (sy > g_ymax[fnum]) { g_ymax[fnum] = sy; }

         /* Done with this one */
         g_toc[tocbase+j].adjf = 2;
         g_trickle_found++;
         lastx = sx; lasty = sy;
      }
      if (trickle_ops > TTUF) {
        printf("trickle.toc: identified %ld chunks so far\r", g_trickle_found);
        fflush(stdout);
        trickle_ops -= TTUF;
      }
    }

    /* Flip any 3's back to 1's */
    for(j=0; j<g_nc[fnum]; j++) {
      trickle_ops++;
      if (g_toc[tocbase+j].adjf == 3) {
        g_toc[tocbase+j].adjf = 1;
      }
    }
  }
  printf("\n");

  i = g_nc[fnum]-g_trickle_found;
  printf("             Found %ld contiguous chunks (and %ld outlier%s)\n",
    g_trickle_found, i, (i==1) ? "" : "s");
  printf("             Dimensions: %d in X (%d to %d); %d in Y (%d to %d)\n",
    g_xmax[fnum] + 1 - g_xmin[fnum], g_xmin[fnum], g_xmax[fnum],
    g_ymax[fnum] + 1 - g_ymin[fnum], g_ymin[fnum], g_ymax[fnum]);
  printf("             outermost is at (%d, %d), %ld steps from start\n",
    lastx, lasty, pass);
  if ((g_trickle_found < 40) && g_no_4096) {
    printf("\n\n");
    printf(
"NOTE: The area I found is very small (less than 40 chunks). If you try\n"
"again, I might find a larger contiguous region.\n");
  }
} /* End of trickle.toc */

void show_outl_by_fpos(int fnum)
{
  long i, j, tocbase, maxpos, lowpos, lowi;
  int n, gg;

  tocbase = g_tocbase[fnum];
  lowi = 0;

  /* Find the fpos of the last chunk in the file */
  maxpos = 0;
  for(i=0; i<g_nc[fnum]; i++) {
    if (g_toc[tocbase+i].fpos > maxpos) {
      maxpos = g_toc[tocbase+i].fpos;
    }
  }

  printf(" %3s %6s %8s %4s %4s\n", "n", "chnk#", "filepos", "w-e", "n-s");
  n = 0; gg = 1;
  for(i=0; gg && (i<g_nc[fnum]); i++) {
    lowpos = maxpos+1; lowi = -1;
    for(j=0; j<g_nc[fnum]; j++) {
      if (g_toc[tocbase+j].adjf == 0) {
        /* This is an outlier */
        if (g_toc[tocbase+j].mark1 == 0) {
          /* Not yet printed */
          if (g_toc[tocbase+j].fpos < lowpos) {
            lowpos = g_toc[tocbase+j].fpos;
            lowi = j;
          }
        }
      }
    }
    if (lowi < 0) {
      gg = 0;
    } else {
      printf(" %3d %6ld %08lX %4d %4d\n", n, lowi, g_toc[tocbase+lowi].fpos,
                                g_toc[tocbase+lowi].x, g_toc[tocbase+lowi].y);
      g_toc[tocbase+lowi].mark1 = 1;
      n++;
    }
  }
  printf("There are %d outlier blocks.\n", n);

  /* Set the mark1 fields back to 0 */
  for(i=0; i<g_nc[fnum]; i++) {
    if (g_toc[tocbase+i].mark1) {
      g_toc[tocbase+lowi].mark1 = 0;
    }
  }
} /* End of show.outl_by_fpos */

/* Given an X and a Y, return a long int that combines the two values */
u32 xy_lexo(long i)
{
  u32 rv;

  rv = (((u32) (g_toc[i].y)) << 16) | (((u32) (g_toc[i].x)) & 0xFFFF);

  return rv;
}

#define TOC_CONNECTED 1
#define TOC_OUTLIERS  2
#define TOC_ALL (TOC_CONNECTED | TOC_OUTLIERS)
#define TOC_COUNTS    4

void show_toc(int fnum, int flags) /* a.k.a. cmd_outliers, cmd_toc */
{
  long i, j, tocbase, lowi;
  u32 tpos, maxpos, lowpos, crc;
  int n, no, nt, gg, pr;

  tocbase = g_tocbase[fnum];
  lowi = 0;

  /* Find the lexo of the lexically last chunk in the map */
  maxpos = 0;
  for(i=0; i<g_nc[fnum]; i++) {
    tpos = xy_lexo(tocbase+i);
    if (tpos > maxpos) {
      maxpos = tpos;
    }
  }
  if (flags & TOC_COUNTS) {
    printf("maxpos = %ld\n", ((long) maxpos));
  }

  if (flags & TOC_ALL) {
    printf(" %3s %6s %8s %4s %4s\n", "n", "chnk#", "filepos", "n-s", "w-e");
  }
  n = no = nt = 0; gg = 1;
  crc = 0;
  /* printf("%ld chunks to scan, maxpos == %ld\n", g_nc[fnum], maxpos); */
  for(i=0; gg && (i<g_nc[fnum]); i++) {
    /* Select a single chunk from those that haven't yet been checked.
       We select the one with the lowest xy_lexo() value. */
    lowpos = maxpos+1; lowi = -1;
    for(j=0; j<g_nc[fnum]; j++) {
      if (g_toc[tocbase+j].mark1 == 0) {
        tpos = xy_lexo(tocbase+j);
        if (tpos <= lowpos) {
          lowpos = tpos;
          lowi = j;
        }
      }
    }
    if (lowi < 0) {
      /* We found no lowest eligible chunk, so we're done */
      gg = 0;
    } else {
      crc = crc32_add_u16(crc, g_toc[tocbase+lowi].x);
      crc = crc32_add_u16(crc, g_toc[tocbase+lowi].y);
      /* Show the block we found */
      pr = 0;
      if (g_toc[tocbase+lowi].adjf) {
        /* This is an outlier */
        if (flags & TOC_CONNECTED) {
          pr = 1;
        }
      } else {
        if (flags & TOC_OUTLIERS) {
          pr = 1;
        }
      }
      if (pr) {
        printf(" %3d %6ld %08lX %4d %4d\n",
                                n, lowi, g_toc[tocbase+lowi].fpos,
                                g_toc[tocbase+lowi].y, g_toc[tocbase+lowi].x);
        n++;
      }
      g_toc[tocbase+lowi].mark1 = 1;
      if (g_toc[tocbase+lowi].adjf == 0) {
        no++;
      } else {
        nt++;
      }
    }
  }
  if (flags & TOC_COUNTS) {
    printf("TOC summary:\n");
    printf("  %d connected blocks\n", nt);
    printf("  %d outlier blocks\n", no); // formerly "There are %d outlier blocks."
    printf("  shape crc: %08lX\n", ((long) crc));
  }

  /* Set the mark1 fields back to original values */
  for(i=0; i<g_nc[fnum]; i++) {
    if (g_toc[tocbase+i].mark1) {
      g_toc[tocbase+i].mark1 = 0;
    }
  }
} /* End of show.toc */

void e17_getname(int fnum)
{
  char * n;
  char scratch[MAX_NAME_LEN*2];

  n = calloc((size_t)(MAX_NAME_LEN+1), 1);
  if (n) {
    scan_name(n, fnum, 0, scratch, 0, 0);
    g_wnames[fnum] = n;
  }
}

/* Try to open a world; if successful the fnum is returned */
int e17_try_open(char * fname, int setpos)
{
  int fnum;
  char * fn;

  if (fname == 0) {
    fflush(stdout);
    fprintf(stderr, "e17.try_open: fname is null.\n");
    return(-1);
  }
  if (g_n_fnum >= MAXFILES) {
    fflush(stdout);
    fprintf(stderr, "e17.try_open: too many files open.\n");
    return(-1);
  }

  fnum = g_n_fnum;

  fn = calloc((size_t)1000, 1);
  if (fn) {
    fn[0] = 0;
    strncat(fn, fname, 1000-strlen(fn)-1);
  } else {
    fn = fname;
  }
  g_fnames[fnum] = fn;

  g_wnames[fnum] = 0;
  g_edat[fnum] = fopen(fname, "r");
  if (g_edat[fnum] == 0) {
    fflush(stdout);
    fprintf(stderr, "e17.try_open: could not open '%s' for reading.\n", fname);
    return(-1);
  }

  e17_getname(fnum);

  if (e17_get_toc(fnum)) {
    /* Success */
    g_n_fnum++;
    trickle_toc(fnum, setpos);
    return(fnum);
  }
  return -1;
} /* End of e17.try_open */

int hash_wxyz(int w, int x, int y, int z)
{
#ifdef HASH_SIMPLE1
  return(((w*143) ^ (x*27) ^ (y*7) ^ z) % MAXHASH);
#else
# ifdef HASH_SIMPLE2
  return(((w*1011) ^ (x*143) ^ (y*27) ^ (z*7)) % MAXHASH);
# else
  int ht[4]; unsigned int rv;
  ht[0] = w; ht[1] = x; ht[2] = y; ht[3] = z;
  rv = fnv1x(((char *)(ht)), 4L*((long)(sizeof(int))), 143);
  return((int)(rv % MAXHASH));
# endif
#endif
}

long cch_first[MAXHASH];
long chf_trys;
long chf_hits;

void cch_init(void)
{
  int i;
  for(i=0; i<MAXHASH; i++) {
    cch_first[i] = -1;
  }
  chf_trys = chf_hits = 0;
}


void cache_setup(void)
{
  int i, gg;
  g_cc_size = 0; gg = 1;
  for(i=0; gg && (i<TRY_CC_SIZE); i++) {
    g_cc[i].blocks = calloc(CHUNKSZ, 1);
    g_cc[i].colors = calloc(CHUNKSZ, 1);
    if ((g_cc[i].blocks == 0) || (g_cc[i].colors == 0)) {
      gg = 0; /* Stop allocating */
    } else {
      /* We successfully allocated blocks; add to the cache data structures */
      g_cc[i].w = g_cc[i].x = g_cc[i].y = g_cc[i].z = 0;
      g_cc[i].qlru = MAX_QLRU;
      g_cc[i].hl_next = -1;
      g_cc_size++;
    }
  }
  if (g_cc_size < 100) {
    /* Cache is just too darn small */
    fflush(stdout);
    fprintf(stderr,
      "cache_setup: failed to allocate %ld bytes for cache cell %d\n",
      CHUNKSZ, i);
    exit(-1);
  }
  printf("cache_setup: created %d cache entries.\n", g_cc_size);
} /* End of cache.setup */

/* Lookup a chunk in the cache and return its pointer. */
int cache_find(int w, int x, int y, int z)
{
  int i;
  for (i=0; i<g_cc_size; i++) {
    if ((g_cc[i].w == w) && (g_cc[i].x == x)
     && (g_cc[i].y == y) && (g_cc[i].z == z)) {
      g_cc[i].qlru = 0;
      return i;
    }
  }
  return -1;
}

/* hashed version of cache_find */
int cache_hf(int w, int x, int y, int z)
{
  int i, hash;
  hash = hash_wxyz(w, x, y, z);
  i = cch_first[hash];
  while (i >= 0) {
    if ((g_cc[i].w == w) && (g_cc[i].x == x)
     && (g_cc[i].y == y) && (g_cc[i].z == z))
    {
      /* A hit. Reset the quasi-LRU counter */
      g_cc[i].qlru = 0;
      return i;
    }
    i = g_cc[i].hl_next;
  }
  return -1;
} /* End of cache.hf */

/* Choose a cache entry to "punt" (discard) to make way for a new one */
int cache_punt1(void)
{
  int i;

  i = g_reaper;
  while (1) {
    i = (i+1) % g_cc_size;
    (g_cc[i].qlru)++;
    if (g_cc[i].qlru > MAX_QLRU) {
      int hash, j;
      /* Reap this one */
      hash = hash_wxyz(g_cc[i].w, g_cc[i].x, g_cc[i].y, g_cc[i].z);
      j = cch_first[hash];
      if (j == i) {
        /* Remove from head */
        cch_first[hash] = g_cc[i].hl_next;
      } else {
        /* Remove from elsewhere in list */
        while (j >= 0) {
          if (g_cc[j].hl_next == i) {
            g_cc[j].hl_next = g_cc[i].hl_next;
            j = -1; /* exit while loop */
          } else {
            j = g_cc[j].hl_next;
          }
        }
      }
      g_reaper = (i+1) % g_cc_size;
      return i;
    }
  }
} /* End of cache.punt1 */

size_t g_total_reads = 0;

/* Try to load a chunk into an entry of the local cache, and return its
   slot number, or negative on error. */
int cache_chunk3(int fnum, int x, int y, int z)
{
  int rv, toci, cci;
  long cpos;
  size_t fres = 0; /* file IO results */

  /* Bounds check */
  if ((fnum < 0) || (fnum >= g_n_fnum)
     /*    || (x < g_xmin[fnum]) || (x > g_xmax[fnum]) */
     /*    || (y < g_ymin[fnum]) || (y > g_ymax[fnum]) */
         || (z < 0) || (z > 3)) {
    /*
    if ((fnum < 0) || (fnum >= g_n_fnum)) {
      printf("cache.chunk3: fnum %d out of range.\n", fnum);
    }
    if ((x < g_xmin[fnum]) || (x > g_xmax[fnum])) {
      printf("cache.chunk3: x %d out of range (%d-%d).\n",
        x, g_xmin[fnum], g_xmax[fnum]);
    }
    if ((y < g_ymin[fnum]) || (y > g_ymax[fnum])) {
      printf("cache.chunk3: y %d out of range (%d-%d).\n",
        y, g_ymin[fnum], g_ymax[fnum]);
    } */
    return -1;
  }

  /* First check if it's in the cache */
  rv = cache_hf(fnum, x, y, z); /* rv = cache_find(x, y, z); */
  if (rv >= 0) {
    /* Found it in the cache */
    chf_trys++; chf_hits++; /* A successful try */
    return rv;
  }
  /* printf("no (%d, %d, %d, %d) in cache\n", fnum, x, y, z); */
  /* Now see if it's in the file */
  toci = toc_hlu(fnum, x, y); /*  toci = toc_lookup(x, y); */
  if (toci < 0) {
    /* Nope */
    /* printf("cache.chunk3: no (%d, %d, %d) in TOC\n", fnum, x, y);  */
    /* We do not increment chf_trys because this chunk isn't cacheable */
    return -1;
  }
  /* Okay, we need to make room for this chunk in the cache */
  cci = V_CACHE_PUNT1();
  chf_trys++; /* tried but wasn't in cache */

  /* Now actually read the requested data */
  {
    int hash;
    /* Compute file position: base chunk location plus (16*16*16*2)*z */
    cpos = g_toc[toci].fpos + ((unsigned long) z) * 2L * CHUNKSZ;

    /* printf("reading fpos %lo into  cci %d for (%d, %d, %d, %d)\n",
         cpos, cci, fnum, x, y, z); */

    fseek(g_edat[fnum], cpos, SEEK_SET);
    fres += fread(g_cc[cci].blocks, 1, CHUNKSZ, g_edat[fnum]);
    fres += fread(g_cc[cci].colors, 1, CHUNKSZ, g_edat[fnum]);
    g_cc[cci].w = fnum; g_cc[cci].x = x; g_cc[cci].y = y; g_cc[cci].z = z;
    g_cc[cci].qlru = 0;
    hash = hash_wxyz(fnum, x, y, z);
    g_cc[cci].hl_next = cch_first[hash];
    cch_first[hash] = cci;
  }

  /* NEW (Eden 1.9.4) compressed chunk format
     See 20150127-Eden-1.9.4-data-dump.png and Eden-1.9.4-data-format.txt
     Basically, it's a run-length compression scheme with handy offset pointers
     so I can extract each 16x16x16 chunk without having to also get the other
     3. I ought to be able to place the code here in cache.chunk3 and
     everything else will just work. */

  if (g_toc[toci].fpos == 0x00002439) {
    int i;
    char buf[16];
    printf(" >>> Got cpos 00002439...\n");
    fseek(g_edat[fnum], 0x00002439, SEEK_SET);
    for(i=0x00002439; i<g_next_2439; i++) {
      fres += fread(buf, 1, 1, g_edat[fnum]);
      printf("%02x ", ((unsigned int) buf[0]) & 0xff);
    }
    printf("\n");
  }
  g_total_reads += fres;

  return cci;
} /* End of cache.chunk3 */


/* Font scanning functions */
#define FONT_WIDTH (128 * 4)
#define FONT_HEIGHT 8
char * fontbits;

#define FONTBIT(ch, px, py) ( fontbits[ FONT_WIDTH*(py) + (ch)*4 + (px) ] )

void font_load(int ch1, int ch2, char * data)
{
  int ch, px, py; char c1, c2;

  for(py=0; py<8; py++) {
    for(ch=ch1; ch<=ch2; ch++) {
      for(px=0; px<4; px++) {
        c1 = *data++; c2 = *data++;
        if ((c1 == ' ') && (c2 == ' ')) {
          // white or blank
          FONTBIT(ch, px, py) = 0;
        } else {
          FONTBIT(ch, px, py) = 1;
        }
      }
    }
  }
} /* End of font.load */

void font_init(void)
{
  size_t sz;

  sz = FONT_WIDTH * FONT_HEIGHT;
  fontbits = calloc(sz, 1);
  if (!(fontbits)) {
    fflush(stdout);
    fprintf(stderr, "font_init: could not allocate %ld bytes for font.\n", ((long)sz));
    exit(-1);
  }

  /* This is my Monaco-6 font from the late 1980's. The character cell is 4
     pixels wide and 8 high; if we use a 128-character ASCII character set
     it's 512 pixels wide. The 32 control characters and the final character
     127 are blank, but these could be used to add a few special characters
     later, if needed. */
  font_load(0x00, 0x07, /* (unused) */
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
  );
  font_load(0x08, 0x0F, /* (unused) */
    "  xx      xx  xxxxxx      xxxx    xxxxxx      xxxx    xxxxxxxx  "
    "  xxxx  xxxx  xx    xx  xx    xx  xx    xx  xx    xx        xx  "
    "  xx  xx  xx  xxxxxx    xx    xx  xxxxxx          xx      xx    "
    "  xx  xx  xx  xx  xx    xx    xx  xx    xx      xx      xx      "
    "  xx      xx  xx    xx  xx    xx  xx    xx    xx        xx      "
    "  xx      xx  xx    xx    xxxx    xxxxxx    xxxxxxxx    xx      "
    "                                                                "
    "                                                                "
  );
  font_load(0x10, 0x17, /* (unused) */
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
  );
  font_load(0x18, 0x1F, /* (unused) */
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
    "                                                                "
  );
  font_load(0x20, 0x27, /*  !"#$%&' */
    "          ##    ##  ##  ##  ##    ##    ##  ##    ####    ##    "
    "          ##    ##  ##  ######    ####      ##  ##        ##    "
    "          ##            ##  ##  ##        ##      ####          "
    "          ##            ######    ##      ##    ##  ##          "
    "                        ##  ##      ##  ##      ##  ##          "
    "          ##                    ####    ##  ##    ####          "
    "                                  ##                            "
    "                                                                "
  );
  font_load(0x28, 0x2F, /* ()*+,-./ */
    "    ##  ##      ##  ##                                      ##  "
    "  ##      ##      ##      ##                                ##  "
    "  ##      ##    ######    ##                              ##    "
    "  ##      ##      ##    ######          ######            ##    "
    "  ##      ##    ##  ##    ##                            ##      "
    "  ##      ##              ##      ##              ##    ##      "
    "    ##  ##                      ##                              "
    "                                                                "
  );
  font_load(0x30, 0x37, /* 01234567 */
    "  ##      ##    ####    ####        ##  ######    ####  ######  "
    "##  ##  ####        ##      ##    ####  ##      ##          ##  "
    "##  ##    ##        ##    ##    ##  ##  ####    ####        ##  "
    "##  ##    ##      ##        ##  ######      ##  ##  ##    ##    "
    "##  ##    ##    ##          ##      ##      ##  ##  ##    ##    "
    "  ##      ##    ######  ####        ##  ####      ##      ##    "
    "                                                                "
    "                                                                "
  );
  font_load(0x38, 0x3F, /* 89:;<=>? */
    "  ##      ##                                              ##    "
    "##  ##  ##  ##                      ##          ##      ##  ##  "
    "  ##    ##  ##    ##      ##      ##    ######    ##        ##  "
    "##  ##    ####                  ##                  ##    ##    "
    "##  ##      ##                    ##    ######    ##            "
    "  ##    ####      ##      ##        ##          ##        ##    "
    "                        ##                                      "
    "                                                                "
  );
  font_load(0x40, 0x47, /* @ABCDEFG */
    "          ##    ####      ####  ####    ######  ######    ####  "
    "  ####  ##  ##  ##  ##  ##      ##  ##  ##      ##      ##      "
    "##  ##  ##  ##  ####    ##      ##  ##  ####    ####    ##      "
    "##  ##  ######  ##  ##  ##      ##  ##  ##      ##      ##  ##  "
    "##      ##  ##  ##  ##  ##      ##  ##  ##      ##      ##  ##  "
    "  ####  ##  ##  ####      ####  ####    ######  ##        ####  "
    "                                                                "
    "                                                                "
  );
  font_load(0x48, 0x4F, /* HIJKLMNO */
    "##  ##  ######      ##  ##  ##  ##      ##  ##  ##  ##  ######  "
    "##  ##    ##        ##  ##  ##  ##      ######  ##  ##  ##  ##  "
    "######    ##        ##  ####    ##      ######  ######  ##  ##  "
    "##  ##    ##        ##  ####    ##      ##  ##  ######  ##  ##  "
    "##  ##    ##        ##  ##  ##  ##      ##  ##  ##  ##  ##  ##  "
    "##  ##  ######  ####    ##  ##  ######  ##  ##  ##  ##  ######  "
    "                                                                "
    "                                                                "
  );
  font_load(0x50, 0x57, /* PQRSTUVW */
    "####    ######  ####      ####  ######  ##  ##  ##  ##  ##  ##  "
    "##  ##  ##  ##  ##  ##  ##        ##    ##  ##  ##  ##  ##  ##  "
    "##  ##  ##  ##  ##  ##    ##      ##    ##  ##  ##  ##  ##  ##  "
    "####    ##  ##  ####        ##    ##    ##  ##  ##  ##  ######  "
    "##      ##  ##  ##  ##      ##    ##    ##  ##    ##    ######  "
    "##      ####    ##  ##  ####      ##      ####    ##    ##  ##  "
    "            ##                                                  "
    "                                                                "
  );
  font_load(0x58, 0x5F, /* XYZ[\]^_ */
    "##  ##  ##  ##  ######    ####  ##      ####      ##            "
    "##  ##  ##  ##      ##    ##    ##        ##    ##  ##          "
    "  ##    ##  ##    ##      ##      ##      ##                    "
    "  ##      ##    ##        ##      ##      ##                    "
    "##  ##    ##    ##        ##        ##    ##                    "
    "##  ##    ##    ######    ##        ##    ##                    "
    "                          ####          ####            ######  "
    "                                                                "
  );
  font_load(0x60, 0x67, /* `abcdefg */
    "  ##            ##                  ##              ##          "
    "    ##          ##                  ##            ##            "
    "          ####  ####      ####    ####    ####  ######    ####  "
    "        ##  ##  ##  ##  ##      ##  ##  ######    ##    ##  ##  "
    "        ##  ##  ##  ##  ##      ##  ##  ##        ##    ##  ##  "
    "          ####  ####      ####    ####    ####    ##      ####  "
    "                                                            ##  "
    "                                                        ####    "
  );
  font_load(0x68, 0x6F, /* hijklmno */
    "##        ##      ##    ##        ##                            "
    "##                      ##        ##                            "
    "####      ##      ##    ##  ##    ##    ######  ####      ##    "
    "##  ##    ##      ##    ####      ##    ######  ##  ##  ##  ##  "
    "##  ##    ##      ##    ####      ##    ##  ##  ##  ##  ##  ##  "
    "##  ##    ##      ##    ##  ##    ##    ##  ##  ##  ##    ##    "
    "                  ##                                            "
    "                ##                                              "
  );
  font_load(0x70, 0x77, /* pqrstuvw */
    "                                  ##                            "
    "                                  ##                            "
    "####      ####  ######    ####  ######  ##  ##  ##  ##  ##  ##  "
    "##  ##  ##  ##  ##      ####      ##    ##  ##  ##  ##  ##  ##  "
    "##  ##  ##  ##  ##          ##    ##    ##  ##    ##    ######  "
    "####      ####  ##      ####        ##    ####    ##    ######  "
    "##          ##                                                  "
    "##          ##                                                  "
  );
  font_load(0x78, 0x7F, /* xyz{|}~ */
    "                            ##    ##    ##        ##  ##        "
    "                          ##      ##      ##    ##  ##          "
    "##  ##  ##  ##  ######    ##      ##      ##                    "
    "  ##    ##  ##    ##    ##                  ##                  "
    "  ##    ##  ##  ##        ##      ##      ##                    "
    "##  ##    ####  ######    ##      ##      ##                    "
    "            ##              ##    ##    ##                      "
    "        ####                                                    "
  );
}

/* Return 1 or 0 to indicate if the current (x,y) position should be
drawn black to display a caption. */
int fontfunction(int x, int y, char * msg, int mx, int my)
{
  int rx, ry, sl, rv;
  int mwid;

  rx = mx - x; ry = 7 - (y - my);
  if ((rx<-1) || (ry<-1) || (ry>7)) {
    return -1;
  }
  sl = strlen(msg);
  mwid = sl * 4;
  if (rx >= mwid) {
    return -1;
  }

  /* Now do the borders */
  if ((rx<0) || (rx>mwid) || (ry<0) || (ry>7)) {
    return 0;
  }

  rv = rx/4;
  rv = msg[rv];
  rv = FONTBIT(rv, rx%4, ry);
  return rv;
} /* End of font.function */


int lbcw = -1;
int lbcx = -1;
int lbcy = -1;
int lbcz = -1;
int lbcci;

/* Return the block type of the block at the given coordinates (w, x, y, z)
   == (world_num, north_south, west_east, height).
   If "int * color" is provided, the block's colour will be returned there.
   If the block does not exist (because the coordinates do not fall within
   any extant chunk), return the value 'errval'; depending on your purpose,
   you might want errval to be 0 (air), 1 (adminium), or a distinct error
   code like -1. */
int blockval(int fnum, int ns, int we, int z, int * color, int errval)
{
  int cx, cy, cz;
  int cci, ix;

  if ((z<0) || (z>63) || (ns<0) || (we<0)) {
    if (color) { *color = 0; }
    return errval;
  }

  /* Convert to chunk coordinates and do coordinate transform */
  cx = we/16; cy = ns/16; cz = z/16;
  if ((fnum == lbcw) && (cx == lbcx) && (cy == lbcy) && (cz == lbcz)) {
    /* Still in the same chunk as the last time this function was called */
    cci = lbcci;
    /* %%% In theory we would also need to verify that g_cc[cci].w == fnum,
       g_cc[cci].x == x, etc. in case calls to emt_set_block intervene
       and cause our chunk to be punted. In practise, we read
       everything first into the scratch buffer, then emt_paste it all
       at once. */
  } else {
    cci = cache_chunk3(fnum, cx, cy, cz);
    lbcw = fnum; lbcx = cx; lbcy = cy; lbcz = cz; lbcci = cci;
  }
  if (cci < 0) {
    /* cache_chunk3 failed -- the chunk does not exist */
    if (color) { *color = 0; }
    return errval;
  }
  ns &= 0xf; we &= 0xf; z &= 0xf;
  ix = (we*16 + ns)*16 + z;

  if (color) {
    *color = (int) ((g_cc[cci].colors)[ix]);
  }

  return((int)((g_cc[cci].blocks)[ix]));
} /* End of block.val */

/* Given a world number and block coordinates in floating-point, return the
 * block's type and colour */
int fblkval(int fnum, float fns, float fwe, int z, int *color)
{
  int ns, we;
  int bl1, cl1, wt;

  fns += 0.5; fwe += 0.5;

  ns = (int) (fns); fns -= ((float) ns); /* fns is now "fraction of ns" */
  we = (int) (fwe); fwe -= ((float) we); /* fwe is now "fraction of we" */

  bl1 = blockval(fnum, ns, we, z, &cl1, 0);
  if ( (bl1 >= 0x28) && (bl1 <= 0x37) ) {
    wt = bl1 & 3; /* Wedge type */
    if ( ( (wt == 0) && (fns + fwe < 1.0) )
      || ( (wt == 1) && (fns < fwe) )
      || ( (wt == 2) && (fns + fwe > 1.0) )
      || ( (wt == 3) && (fns > fwe) )
    ) {
      cl1 = bl1 = 0;
    }
  }
  if (color) {
    *color = cl1;
  }
  return bl1;
} /* End of fblk.val */

/* Given a world number and global coordinates, return the blocktype and colour
 * of the topmost non-air block. Clouds at z=60 and above are ignored unless
 * the cloudok parameter is true, but in all cases a coloured block is not
 * ignored.
 *    The 'errval' parameter is passed through to topmost(). Pass 0 for this
 * to treat non-existent blocks as air. */
int topmost(int fnum, int ns, int we, int * color, int cloudok, int errval)
{
  int tb, tc, rv, z;

  /* Start with a return value (block type) of 0 (air) */
  rv = 0; z = 64;
  /* Search as long as we're still looking at air */
  while ((rv == 0) && (z>0)) {
    z--;
    tb = blockval(fnum, ns, we, z, &tc, errval);
    if     (cloudok
        || (z<60)           // all blocks below z=60 count
        || (tb != BL_CLOUD) // all non-cloud blocks count
        || tc)              // all coloured blocks count
    {
      rv = tb; if (color) { *color = tc; }
    }
  }
  /* Now z is the height of the top.most nonzero block. */
  return rv;
} /* End of top.most */

#define DB_REMOVE 1
#define DB_ADD    2
#define DB_CHANGE 4

int diffblock(int w1, int w2, int x, int y, int z)
{
  int b1, b2, c1, c2, rv;

  rv = 0;
  b1 = blockval(w1, x, y, z, &c1, 0);
  b2 = blockval(w2, x, y, z, &c2, 0);
  if (b1 == 0) {
    if (b2) {
      /* A block was added */
      rv |= DB_ADD;
    }
  } else if (b2 == 0) {
    if (b1) {
      /* A block was removed */
      rv |= DB_REMOVE;
    }
  } else {
    if ((b1 != b2) || (c1 != c2)) {
      /* A block was changed */
      rv |= DB_CHANGE;
    }
  }
  return rv;
} /* End of diff.block */

/* Persistent state for sc.one */
int sc1_cur_b1, sc1_cur_c1;
int sc1_last_b1, sc1_last_c1;
int sc1_this_attr, sc1_last_attr;
int sc1_this_nl, sc1_last_nl;
int sc1_this_top;
int sc1_last_height, sc1_this_height;
int sc1_caveflag;

#define BLATR(b) (g_blt[(b)].attr)

int natural_p(int w, int x, int y, int z)
{
  int b, c, treecolours, b2, c2;

  b = blockval(w, x, y, z, &c, BL_AIR);
  treecolours = ((c == 0) || (c == 0x13) || (c == 0x14) || (c==0x15));

  /* Uncoloured blocks are pretty easy. This case also covers coords out
     of bounds or within an undefined chunk */
  if (c == 0) {
    if (BLATR(b) & ATTR_NATURAL) {
      return 1;
    }
    return 0;
  }

  if ((z == 0) && (b == BL_STEEL) && (c == CO_GRAY20)) {
    return 1;
  }

  /* coloured blocks are *sometimes* natural */
  if (b == BL_AIR) {
    /* We have to treat this as 'natural' now, because in Eden 2.0 if you
       destroy coloured blocks using either the pickaxe or TNT, the result is
       "coloured air". */
    return 1;
  } else if (b == BL_LEAF) {
    /* Check for the three valid colours */
    return (treecolours ? 1 : 0);
  } else if (b == BL_TRUNK) {
    if (treecolours) {
      /* Coloured tree-trunk with one of the 3 natural leaf colours. This
         might be natural (there's a bug in Eden's tree generator), but we
         need to look at the block below */
      if (z <= 1) {
        /* Trunk in or near bedrock, not natural! */
        return 0;
      }
      b2 = blockval(w, x, y, z-2, &c2, BL_AIR);
      if ((b2 == BL_TRUNK) && (c2 == 0)) {
        /* Above normal trunk, natural */
        return 1;
      }
      /* Block below is not a tree trunk or is coloured */
      return 0;
    }
    /* Trunk coloured any other colour */
    return 0;
  }
  /* Any other block type with colour */
  return 0;
} /* End of natural.p */

int toprock(int w, int x, int y)
{
  int z, b, c;

  for (z=63; z>=0; z--) {
    b = blockval(w, x, y, z, &c, BL_AIR);
    if (b == BL_STONE) {
      return z;
    }
  }
  return 0;
} /* End of toprock */

/* sc.one does CM.STRUCT mode at a single z level */
int sc_one(int w, int x, int y, int z, int init)
{
  int rv;
  int b1, c1;

  rv = 0;
  if (init) {
    /* Initialize */
    b1 = blockval(w, x, y, z, &c1, BL_AIR);
    sc1_last_b1 = sc1_cur_b1 = b1;
    sc1_last_c1 = sc1_cur_c1 = c1;
    sc1_last_attr = sc1_this_attr = BLATR(b1);
    sc1_last_nl = sc1_this_nl = natural_p(w, x, y, z); /*ATR_NATLEAF(b1,c1);*/
    sc1_this_top = z;
    sc1_last_height = sc1_this_height = 1;
    sc1_caveflag = 0;
    if (! sc1_this_nl) {
      /* An unnatural block or a coloured block */
      rv |= SC_MANMADE;
    }
    return rv;
  }

  /* We default to Adminium because the state machine works by scanning
     downwards, and when we go below 0 we want to treat that as bedrock.
     If we scanned upwards instead we would default to air. */
  b1 = blockval(w, x, y, z, &c1, BL_BEDROCK);

  if ((b1 != sc1_cur_b1) || (c1 != sc1_cur_c1)) {
    /* We found a change. Save the previous state */
    sc1_last_b1 = sc1_cur_b1;
    sc1_last_c1 = sc1_cur_c1;
    sc1_last_attr = sc1_this_attr;
    sc1_last_nl = sc1_this_nl;
    sc1_last_height = sc1_this_height;

    if (sc1_caveflag) {
      /* Last extent was a cave; measure its height and set the extra bits */
      if (sc1_this_height > 16) {
        rv |= SC_CVHT3;
      } else if (sc1_this_height > 13) {
        rv |= SC_CVHT2;
      } else if (sc1_this_height > 10) {
        rv |= SC_CVHT1;
      }
    }

    /* Then get the new attributes */
    sc1_cur_b1 = b1; sc1_cur_c1 = c1;
    sc1_this_attr = BLATR(b1);
    sc1_this_nl = natural_p(w, x, y, z); /*ATR_NATLEAF(b1,c1);*/
    sc1_this_top = z;
    sc1_caveflag = 0;
  }

  /* The z coord, and thus the height, changes every time */
  sc1_this_height = (sc1_this_top - z) + 1;

  if (! sc1_this_nl) {
    /* An unnatural block or a coloured block */
    rv |= SC_MANMADE;
  }

  /* Tag the air and permeable (water-containing) blocks */
  if ((b1 == BL_AIR)
    && (sc1_last_attr & ATTR_CAVETOP) && (sc1_last_c1 == 0)
    && (sc1_this_height >= 2)) {
    /* We're in a natural cave */
    rv |= SC_CAVE;
    sc1_caveflag = 1;
  } else if ((b1 == BL_AIR)
     && (sc1_last_attr & ATTR_OHANGTOP)
     && sc1_last_nl ) {
    /* Under a dirt overhang, or in a manmade tunnel under dirt */
    rv |= SC_OVERHANG;
  } else if ((sc1_this_attr & ATTR_PERMEABLE)
    && (!(sc1_last_attr & ATTR_PERMEABLE))
    && (! sc1_last_nl)
    && (sc1_this_height >= 2)) {
    /* Inside a building or man-made tunnel, possibly flooded */
    rv |= SC_MAN_TUNNEL;
  }

  return rv;
} /* End of sc.one */

/* Scan a column of blocks for 'struct' attributes, like tunnel and
   artificial, between two given z coordinates. */
int struct_zz(int w, int x, int y, int z1, int z2, int or_all)
{
  int b1, rv, z;

  rv = 0; /* No attributes */

  /* z1 should be the higher */
  if (z1 < z2) {
    z = z2; z2 = z1; z1 = z;
  }

  /* Find out if this column even exists */
  b1 = blockval(w, x, y, 63, 0, -1);
  if (b1 < 0) { return 0; }

  /* Start with top block and init */
  z = z1;
  rv = sc_one(w, x, y, z, 1);

  /* scan down to the lower z coordinate */
  while (z>z2) {
    z--;
    if (or_all) {
      rv = rv | sc_one(w, x, y, z, 0);
    } else {
      rv = sc_one(w, x, y, z, 0);
    }
  }

  /* Now rv is a bitmask of attributes in this column. */
  return rv;
} /* End of struct.zz */

/* Scan a column of blocks for 'struct' attributes, like tunnel and
   artificial. */
int struct_col(int w, int x, int y)
{
  int rv;
  rv = struct_zz(w, x, y, 63, 0, 1);
  return rv;
} /* End of struct.col */



#define TM_DEEP    1
#define TM_MEDIUM  2
#define TM_SHALLOW 4

/* Scan a full column of blocks for tunnels */
int tm_col(int w, int x, int y, int dbg, int * first_top)
{
  int rv, b1, c1, z, ss, mh;
  int in_solid, was_solid, tun_top;

  if (first_top) {
    *first_top = -1;
  }

  /* Find out if this column even exists */
  b1 = blockval(w, x, y, 63, 0, -1);
  if (b1 < 0) { return 0; }

  rv = ss = was_solid = in_solid = tun_top = 0;
  for(z=63; z>=0; z--) {
    /* We default to Adminium for the same reason as in sc.one */
    b1 = blockval(w, x, y, z, &c1, BL_BEDROCK);

    was_solid = in_solid;
    if (BLATR(b1) & ATTR_PERMEABLE) {
      in_solid = 0;
    } else {
      if (ss) {
        /* Leaves below another solid count as solid */
        in_solid = 1;
      } else if ((b1 == BL_LEAF)
              || ((b1 == BL_CLOUD) && (c1 == 0)) ) {
        /* leaves above ground don't count */
        in_solid = 0;
      } else {
        in_solid = 1;
      }
    }

    if (dbg) { printf("%2d %2x %d %d", z, b1, was_solid, in_solid); }

    if (was_solid != in_solid) {
      /* This is a transition */
      if (dbg) { printf(" tr"); }
      if (in_solid) {
        /* We're in solid. See if there has been a prior transition from
           solid to permeable */
        if (dbg) { printf(" is"); }
        if (ss) {
          /* We've just found the bottom of a tunnel. Check if it's tall
             enough */
          if (dbg) { printf(" ss"); }
          if ((tun_top - z) >= 2) {
            /* Figure out the mean height */
            if (dbg) { printf(" >2"); }
            mh = (tun_top + z)/2;
            if (mh < 10) {
              rv |= TM_DEEP;
            } else if (mh < 18) {
              rv |= TM_MEDIUM;
            } else {
              rv |= TM_SHALLOW;
            }
            if (dbg) { printf(" -> %d", rv); }
          }
        }
      } else {
        /* We've entered the top of a tunnel. Save the top coordinate */
        tun_top = z;
        if (first_top) { *first_top = z; }
        ss = 1;
        if (dbg) { printf(" tt=%d", z); }
      }
    }

    if (dbg) { printf("\n"); }
  }

  return rv;
} /* End of tm.col */


/* tm_one does CM_TUNNEL mode for a single z level */
int tm_one(int w, int x, int y, int z)
{
  int rv, ft;

  /* Get the tunnel bitmap for the whole column */
  rv = tm_col(w, x, y, 0, &ft);

  if (ft < 0) {
    return 0;
  } else if (z > ft) {
    return 0;
  }

  /* Mask out the bit for this height */
  if (z < 10) {
    rv &= TM_DEEP;
  } else if (z < 18) {
    rv &= TM_MEDIUM;
  } else {
    rv &= TM_SHALLOW;
  }

  /* If this block is solid, return 0 */
  if (rv) {
    int b1;
    b1 = blockval(w, x, y, z, 0, BL_BEDROCK);
    if (!(BLATR(b1) & ATTR_PERMEABLE)) {
      rv = 0;
    }
  }

  return rv;
} /* End of tm.one */



/* Compare a column of blocks in world w1 to the corresponding column in w2 */
int diffcol(int w1, int w2, int x, int y)
{
  int rv, z;

  rv = 0; z = 64;
  while (z>0) {
    z--;
    rv |= diffblock(w1, w2, x, y, z);
  }
  /* Now rv is a bitmask of actions taken in this column. */
  return rv;
} /* End of diffcol */

/* Three-way diff a single block */
int d3_blk(int base, int w1, int w2, int x, int y, int z)
{
  int b1, b2, b3, c1, c2, c3, a1, a2, a3;

  b1 = blockval(base, x, y, z, &c1, 0);
  b2 = blockval(w1, x, y, z, &c2, 0);
  b3 = blockval(w2, x, y, z, &c3, 0);

  /* It's easier if we combine the block and colour into a single int */
  a1 = (b1<<8) | c1;
  a2 = (b2<<8) | c2;
  a3 = (b3<<8) | c3;

  if ((a1 == a2) && (a1 == a3)) {
    /* All the same */
    return 0;
  } else if (a1 == a3) {
    /* Only the blue world changed */
    return 4;
  } else if (a1 == a2) {
    /* Only the green world changed */
    return 2;
  } else if (a2 == a3) {
    /* Both worlds changed to the same thing. Treat this as a non-conflict
       blue. Technically we should return cyan (blue and green both changed,
       but not conflict) but I want to use diff3 in a non-symmetrical way */
    return 4;
  } else if ((b2 == BL_GRASS) && (b3 == BL_WEED) && ((c2 | c3) == 0)) {
    /* b2 vs. b3 is grass vs. weed. Treat this as a2==a3, i.e. not a conflict */
    return 4;
  } else if ((b2 == BL_WEED) && (b3 == BL_GRASS) && ((c2 | c3) == 0)) {
    /* weed vs. grass */
    return 4;
  }

  /* Both worlds changed, and *not* to the same thing. */
  return 1;
} /* End of d3.blk */

int d3_col(int base, int w1, int w2, int x, int y)
{
  int rv, z;

  rv = 0; z = 64;
  while (z>0) {
    z--;
    rv |= d3_blk(base, w1, w2, x, y, z);
  }
  /* Now rv is a bitmask of all diffs in this column. */
  return rv;
} /* End of d3.col */



/* Like top.most, but does axonometric projection. We return the block type,
   colour, and z coordinate of the first block we find. If we get all the way
   to the bottom without finding anything, wqe return blocktype 0, color 0,
   z=0. */
int ftopmost(int fnum, int * startz, float *fns, float *fwe, int * color,
   int cloudok, float ski)
{
  int tb, tc, rv, z;
  float sk; /* skew */
  float ns, we;

  sk = 0.0;
  rv = 0; z = *startz;
  if (color) { *color = 0; }
  ns = *fns; we = *fwe;
  while ((rv == 0) && (z>0)) {
    z--; sk += ski;
    ns = (*fns) + sk; we = (*fwe) + -0.5 * sk;
    tb = fblkval(fnum, ns, we, z, &tc);
    if (cloudok || (z<60) || (tb != BL_CLOUD) || tc) {
      rv = tb;
      if (color) {
        *color = tc;
      }
    }
  }
  /* Now z is the height of the top.most nonzero block. */
  *startz = z;
  *fns = ns; *fwe = we;
  return rv;
} /* End of ftop.most */

void blix_scan(void)
{
  int i, gg;
  blta *t;

  gg = 1;
  for (i=0; gg; i++) {
    t = g_blt + i;
    if (t->type < 0 ) {
      /* Successfully reached the end */
      g_num_blts = i;
      gg = 0;
    } else if (t->type != i) {
      /* Indices out of order */
      die1("blix_scan: g_blt array is out of order!\n");
    }
  }
} /* End of blix.scan */

/* Get the index into the block table of the given block ID */
blta * blix(int blt)
{
  if (blt >= g_num_blts) {
    /* Block type out of range */
    return 0;
  }
  /* else */
  return (g_blt+blt);
}

void cix_scan(void)
{
  int i, gg;
  clra *t;

  gg = 1;
  for (i=0; gg; i++) {
    t = g_clrs + i;
    if (t->ix < 0 ) {
      /* Successfully reached the end */
      g_num_clrs = i;
      gg = 0;
    } else if (t->ix != i) {
      /* Indices out of order */
      die1("cix_scan: g_clrs array is out of order!\n");
    }
  }
} /* End of cix.scan */

clra * cix(int col)
{
  if (col >= g_num_clrs) {
    /* Colour index out of range, so just return index 0 */
    return g_clrs;
  }
  /* else */
  return (g_clrs + col);
}

char * blnam2(int blt)
{
  blta * t;
  t = blix(blt);
  if (t) {
    return (t->nam2);
  }
  /* else */
  return 0;
}

int blansi(int blt)
{
  blta * t;
  t = blix(blt);
  if (t) {
    return (t->ansicol);
  }
  /* else */
  return 0;
}

/* Get the natural colour of a block (in HLS colour space). In addition,
 * each block has a "maximum lightness". These are all declared as literal
 * constants in the "blta g_blt[] = { ... }" section near the beginning
 * of this file. */
void blcol(int blt, float *ch, float *cl, float *cs, float *maxlt)
{
  blta * t;

  t = blix(blt); if (t == 0) { t = blix(0); }
  if (ch) { *ch = t->col_h; }
  if (cl) { *cl = t->col_l; }
  if (cs) { *cs = t->col_s; }
  if (maxlt) { *maxlt = t->max_lt; }
} /* End of bl.col */

/* Get the colour of a paint-colour (in HLS colour space).  These are
 * declared as literal constants in the "clra g_clrs[] = { ... }" section
 * near the beginning of this file. */
void cocol(int col, float *ch, float *cl, float *cs)
{
  clra * t;

  t = cix(col);
  if (ch) { *ch = t->hue; }
  if (cl) { *cl = t->lgt; }
  if (cs) { *cs = t->sat; }
} /* End of co.col */

int g_cur_ansi_mode = 0;
void ansiset(int mode)
{
  if (mode == g_cur_ansi_mode) {
    return;
  }

  printf("%c[0", 27);
  if (mode & 0300) {
    printf(";1");
  }
  if (mode) {
    printf(";3%d", (mode >> 3) & 07);
    if ((mode & 0200) == 0) {
      printf(";4%d", mode & 07);
    }
  }
  printf("m");
  g_cur_ansi_mode = mode;
} /* End of ansi.set */

/* Print one line of the "colour codes for special modes" */
void one_cc_line(int col, char *msg1, char *msg2, char *msg3)
{
  printf("  ");
  ansiset(col);
  printf("%19s", msg1);
  ansiset(0);
  printf("    ");
  ansiset(col);
  printf("%19s", msg2);
  ansiset(0);
  printf("    ");
  ansiset(col);
  printf("%19s", msg3);
  ansiset(0);
  printf("\n");
} /* End of one.cc_line */

void legend(void)
{
  int i;
  char t[100];

  printf("In 'srs' display, the block types are represented as:\n  ");
  for(i=0; i<g_num_blts; i++) {
    int l;
    sprintf(t, "%2d %2s %-12s  ", i, g_blt[i].nam2, g_blt[i].longname);
    if ((i+1) % 4 == 0) {
      l = strlen(t);
      while(t[l-1] == ' ') {
        l--; t[l] = 0;
      }
    }
    printf("%s", t);
    if ((i+1) % 4 == 0) {
      printf("\n  ");
    }
  }
  printf("\n");
  printf("Here are the colour codes for the special modes:\n");
  one_cc_line(0, "    'diff' mode    ", "  3-way diff mode  ", "   'struct' mode   ");
  one_cc_line(4, "  block(s) changed ", "1:changed in world1", "    natural cave   ");
  one_cc_line(2, "  block(s) added   ", "2:changed in world2", "   manmade tunnel  ");
  one_cc_line(1, "  block(s) removed ", "C:conflict         ", " manmade structure ");
  one_cc_line(6, " added and changed ", " 1+2 in same column", "  tunnel and cave  ");
  one_cc_line(5, "removed and changed", " C+1 in same column", "structure and cave ");
  one_cc_line(3, " added and removed ", " C+2 in same column", " struct. and tunnel");
  one_cc_line(7, "     all three     ", "     all three     ", "     all three     ");
} /* End of legend() */

void colors(void)
{
  int i, i1, i2, i3;
  char t[100];
  int l;

  printf("Block colour names:\n");
  printf("  ");
  for(i1=0; i1<=4; i1+=4) {
    for(i3=0; i3<6; i3++) {
      for(i2=0; i2<4; i2++) {
        i = 1 + i1 + i2 + i3*9;
        sprintf(t, "%2d %-16s  ", i, g_clrs[i].description);
        if (i2 >= 3) {
          l = strlen(t);
          while(t[l-1] == ' ') {
            l--; t[l] = 0;
          }
        }
        printf("%s", t);
        if (i2 >= 3) {
          printf("\n  ");
        }
      }
    }
    printf("\n  ");
  }
  for(i=0; i<=54; i+= 9) {
    sprintf(t, "%2d %-16s  ", i, g_clrs[i].description);
    if (i == 27) {
      l = strlen(t);
      while(t[l-1] == ' ') {
        l--; t[l] = 0;
      }
    }
    printf("%s", t);
    if (i == 27) {
      printf("\n  ");
    }
  }
  printf("\n");
} /* End of colors() */

int ns_lim1, ns_lim2, ew_lim1, ew_lim2; /* for 'fmg' */

/* Selection modes determine which blocks are displayed by srs and fmg */
#define SM_TOPMOST 1  /* normal display of the topmost non-air block */
#define SM_PLAN    2  /* several consecutive slices */
#define SM_SLICE   3  /* a single slice */
/* If we want a 'two worlds side-by-side' mode, it would be an SM_XXX mode. */
int g_selmode = SM_TOPMOST;
int g_slicemode_z1 = 63;
int g_slicemode_z2 =  0;

/* Colouring modes determine how those blocks are rendered/displayed */
#define CM_ORTHO  0   /* Only used in fmg_mode */
#define CM_NORMAL 1
#define CM_DIFF   2
#define CM_DIF3   3
#define CM_STRUCT 4
#define CM_TUNNEL 5
int g_colormode = CM_NORMAL;
int g_fmg_mode = CM_NORMAL;
int g_fmg_mag = 1;
int g_diffmode_fnum = -1;
int g_diffmode_f2 = -1;
int g_no_wcbt = 0;

/* Short-range scan for topmost selmode. This implements all the colourmodes
using the "column" version of the colouring functions where appropriate. */
void srs_topmost(int fnum, int clnum)
{
  int ns, we, c;
  int c_ns, c_we;

  check_nfnum();

  c_ns = cur_ns/16; c_we = cur_we/16;

  if (!(g_no_wcbt)) {
    printf("world %d, centered at block (%d,%d) within chunk (%d,%d,0..3)\n",
      fnum,   cur_ns, cur_we,  c_ns, c_we);
  }
  g_no_wcbt = 0;

  for (ns = ns_max; ns > ns_min; ns--) {
    for (we = we_max; we > we_min; we--) {
      int b; char * n; int ansicol;
      b = topmost(fnum, ns, we, &c, 1, -1);
      if (b < 0) {
        /* No blocks here */
        ansiset(0);
        printf("==");
      } else if (clnum) {
        /* Just show colour as a number */
        if (c < 100) {
          printf("%2d", c);
        } else {
          printf("C+"); // this means "100 or more"
        }
      } else {
        if (0) {
        } else if (g_colormode == CM_TUNNEL) {
          ansicol = tm_col(fnum, ns, we, 0, 0);
        } else if (g_colormode == CM_STRUCT) {
          ansicol = struct_col(fnum, ns, we);
        } else if (g_colormode == CM_DIFF) {
          ansicol = diffcol(fnum, g_diffmode_fnum, ns, we);
        } else if (g_colormode == CM_DIF3) {
          ansicol = d3_col(fnum, g_diffmode_fnum, g_diffmode_f2, ns, we);
        } else {
          /* just show the block's natural colour using ANSI colour */
          ansicol = blansi(b);
        }
        ansiset(ansicol);
        n = blnam2(b);
        if (n) {
          printf("%s", n);
        } else {
          printf("%2x", b);
        }
      }
    }
    ansiset(0);
    printf("\n");
  }
} /* End of srs.topmost */

int g_triptych = 0;

/* Short-range scan, top-down, 3 worlds (triptych display). */
void srs_top3(int fnum, int clnum)
{
  int ns, we, c, cmod;
  int tti, tryp;
  int tcm[3];
  int tdf[3];

  check_nfnum();

  /* Set up the colourmode and diff fnum variables for normal modes and
     for triptych mode. */
  tryp = 3;
  cmod = g_colormode;
  if (cmod == CM_DIF3) { cmod = CM_NORMAL; }
  tcm[0] = (cmod == CM_DIFF) ? CM_NORMAL : cmod; tdf[0] = fnum;
  tcm[1] = cmod; tdf[1] = g_diffmode_fnum;
  tcm[2] = cmod; tdf[2] = g_diffmode_f2;

  /* Print little sub-headers for the triptych */
  for(tti=0; tti<tryp; tti++) {
    char ttmp[100]; int wid;
    wid = (we_max - we_min) * 2;
    snprintf(ttmp, 100, "%sworld %c [%s]",
      ((tti && (cmod == CM_DIFF)) ? "vs. " : ""),
      fnum_to_char(tdf[tti]), g_wnames[tdf[tti]]);
    if (((signed)(strlen(ttmp))) > wid) {
      ttmp[wid] = 0;
      printf("%s  ", ttmp);
    } else {
      printf("%s  ", ttmp);
      wid -= strlen(ttmp);
      while(wid > 0) {
        printf(" "); wid--;
      }
    }
  }
  printf("\n");

  for (ns = ns_max; ns > ns_min; ns--) {
    for(tti=0; tti<tryp; tti++) {
      for (we = we_max; we > we_min; we--) {
        int b; char * n; int ansicol;
        b = topmost(tdf[tti], ns, we, &c, 1, 0);
        if (clnum) {
          /* Just show colour as a number */
          if (c < 100) {
            printf("%2d", c);
          } else {
            printf("C+"); // this means "100 or more"
          }
        } else {
          if (0) {
          } else if (tcm[tti] == CM_TUNNEL) {
            ansicol = tm_col(tdf[tti], ns, we, 0, 0);
          } else if (tcm[tti] == CM_STRUCT) {
            ansicol = struct_col(tdf[tti], ns, we);
          } else if (tcm[tti] == CM_DIFF) {
            ansicol = diffcol(fnum, tdf[tti], ns, we);
          } else if (0 && (tcm[tti] == CM_DIF3)) {
            /* dif3 mode in tryptych would allow us to show more than 3
               worlds in the display, but I don't know why I'd want to
               do that */
          } else {
            /* just show the block's natural colour using ANSI colour */
            ansicol = blansi(b);
          }
          ansiset(ansicol);
          n = blnam2(b);
          if (n) {
            printf("%s", n);
          } else {
            printf("%2x", b);
          }
        }
      }
      if (tti+1<tryp) {
        ansiset(0);
        printf("  ");
      }
    }
    ansiset(0);
    printf("\n");
  }
} /* End of srs.top3 */

/* Short-range scan for a single slice. This implements the colourmodes
using a suitable single-block or short column of blocks. */
void srs_1s(int fnum, int clnum, int level)
{
  int ns, we, c;
  int tti, tryp;
  int tfil[3];
  int tcm[3];
  int tdf[3];

  check_nfnum();

#if 0
  int c_ns, c_we;
  c_ns = cur_ns/16; c_we = cur_we/16;
  printf("world %d, centered at block (%d,%d) within chunk (%d,%d,0..3)",
    fnum,   cur_ns, cur_we,  c_ns, c_we);
  printf(", level %d:\n", level);
#endif

  /* Set up the colormode and diff fnum variables for normal modes and
     for triptych mode. */
  if (g_triptych) {
    tryp = 3;
    if ((g_colormode == CM_DIFF) || (g_colormode == CM_DIF3)) {
      tfil[0] = fnum; tcm[0] = CM_NORMAL; tdf[0] = fnum;
      tfil[1] = fnum; tcm[1] = CM_DIFF; tdf[1] = g_diffmode_fnum;
      tfil[2] = fnum; tcm[2] = CM_DIFF; tdf[2] = g_diffmode_f2;
    } else {
      /* Set up triptych display with a non-diff colouring */
      tfil[0] = tdf[0] = fnum;            tcm[0] = g_colormode;
      tfil[1] = tdf[1] = g_diffmode_fnum; tcm[1] = g_colormode;
      tfil[2] = tdf[2] = g_diffmode_f2;   tcm[2] = g_colormode;
    }
  } else {
    tryp = 1;
    tfil[0] = fnum;
    tcm[0] = g_colormode;
    tdf[0] = g_diffmode_fnum;
  }

  if (tryp == 1) {
    printf("World [%s], level %d:\n", g_wnames[fnum], level);
  } else {
    /* Print little sub-headers for the triptych */
    for(tti=0; tti<tryp; tti++) {
      char ttmp[100]; int wid;
      wid = (we_max - we_min) * 2;
      snprintf(ttmp, 100, "%s %c [%s] z=%d  ",
                              ((tti && (tcm[tti] == CM_DIFF)) ? "vs. " : ""),
                          fnum_to_char(tdf[tti]), g_wnames[tdf[tti]], level);
      if (((signed)(strlen(ttmp))) > wid) {
        ttmp[wid] = 0;
        printf("%s  ", ttmp);
      } else {
        printf("%s  ", ttmp);
        wid -= strlen(ttmp);
        while(wid > 0) {
          printf(" "); wid--;
        }
      }
    }
    printf("\n");
  }

  for (ns = ns_max; ns > ns_min; ns--) {
    for(tti=0; tti<tryp; tti++) {
      for (we = we_max; we > we_min; we--) {
        int b; char * n; int ansicol;

        b = blockval(tfil[tti], ns, we, level, &c, -1);

        if (b < 0) {
          /* No blocks here */
          ansiset(0);
          printf("  ");
        } else if (clnum) {
          /* Just show colour as a number */
          if (b==0) {
            /* air sometimes has a colour; show this. */
            if (c == 0) {
              printf("  ");
            } else {
              printf(" ?");
            }
          } else if (c == 0) {
            /* uncoloured */
            printf(" :");
          } else {
            /* Just show colour as a number */
            if (c < 100) {
              printf("%2d", c);
            } else {
              printf("C+"); // this means "100 or more"
            }
          }
        } else {
          if (0) {
          } else if (tcm[tti] == CM_TUNNEL) {
            ansicol = tm_one(tfil[tti], ns, we, level);
          } else if (tcm[tti] == CM_STRUCT) {
            ansicol = struct_zz(tfil[tti], ns, we, level+2, level, 0);
          } else if (tcm[tti] == CM_DIFF) {
            ansicol = diffblock(tfil[tti], tdf[tti], ns, we, level);
            b = blockval(tdf[tti], ns, we, level, 0, 0);
          } else if (tcm[tti] == CM_DIF3) {
            /* NOTE: This will only be seen in non-triptych modes */
            ansicol = d3_blk(fnum, g_diffmode_fnum, g_diffmode_f2,
                                                                ns, we, level);
          } else {
            /* just show the block's natural colour using ANSI colour */
            ansicol = blansi(b);
          }
          ansiset(ansicol);
          n = blnam2(b);
          if (n) {
            printf("%s", n);
          } else {
            printf("%2x", b);
          }
        }
      }
      if (tti+1<tryp) {
        ansiset(0);
        printf("  ");
      }
    }
    ansiset(0);
    printf("\n");
  }
} /* End of srs.1s */

/* Short-range scan for multiple slice selmode. This just calls srs.1s
multiple times. */
void srs_slices(int fnum, int clnum, int lev1, int lev2)
{
  int z, c_ns, c_we;

  /* lev1 should be the higher */
  if (lev1 < lev2) {
    z = lev1; lev1 = lev2; lev2 = z;
  }

  c_ns = cur_ns/16; c_we = cur_we/16;
  printf("world %d, centered at block (%d,%d) within chunk (%d,%d,0..3)",
    fnum,   cur_ns, cur_we,  c_ns, c_we);
  printf(", levels %d - %d:\n", lev1, lev2);

  /* Do a series of plan (horizontal cross-section) views */
  for(z=lev1; z >= lev2; z--) {
    srs_1s(fnum, clnum, z);
    /* If this is not the last slice, print a placeholder blank line to
       fill the space that will be used by the command prompt after the final
       level. */
    if (z > lev2) {
      printf("\n");
    }
  }
} /* End of srs.slices */


/* srs.dispatch selects one of the srs_xxx functions based on the current
   selection mode */
void srs_dispatch(int fnum, int clnum)
{
  if (g_selmode == SM_TOPMOST) {
    if (g_triptych) {
      srs_top3(fnum, clnum);
    } else {
      srs_topmost(fnum, clnum);
    }
  } else if (g_selmode == SM_PLAN) {
    srs_slices(fnum, clnum, g_slicemode_z1, g_slicemode_z2);
  } else {
    srs_1s(fnum, clnum, g_slicemode_z1);
  }
} /* End of srs.dispatch */

void hsl_to_rgb(float ch, float cl, float cs, float *pfr, float *pfg, float *pfb)
{
  float tc, hp, hm2, tx, m;
  float fr, fg, fb;

  /* HSL/HLS to RGB conversion, from en.wikipedia.org/wiki/HSL_and_HSV */
  tc = (1.0 - fabs(2.0*cl - 1.0)) * cs;
  hp = ch / 60.0;
  if ((hp < 0.0) || (hp >= 6.0)) {
    fr = fg = fb = 0.0;
  } else {
    if (hp >= 4.0) { hm2 = hp - 4.0; }
    else if (hp >= 2.0) { hm2 = hp - 2.0; }
    else { hm2 = hp; }
    tx = tc * (1.0 - fabs(hm2 - 1.0));

    if      (hp < 1.0) { fr = tc; fg = tx; fb = 0.; }
    else if (hp < 2.0) { fr = tx; fg = tc; fb = 0.; }
    else if (hp < 3.0) { fr = 0.; fg = tc; fb = tx; }
    else if (hp < 4.0) { fr = 0.; fg = tx; fb = tc; }
    else if (hp < 5.0) { fr = tx; fg = 0.; fb = tc; }
    else               { fr = tc; fg = 0.; fb = tx; }
  }
  m = cl - 0.5*tc;
  fr = fr+m;
  fg = fg+m;
  fb = fb+m;

  if (pfr) { *pfr = fr; }
  if (pfg) { *pfg = fg; }
  if (pfb) { *pfb = fb; }
} /* End of hsl.to_rgb */

/* Given a block type and block colour, return the RGB colour. This does
   blcol, cocol, and the colour space conversion. */
void bc_col(int b, int c, float *pcr, float *pcg, float *pcb)
{
  float ch, cs, cl, maxlt;
  float fr, fg, fb;

  /* Get natural colours */
  blcol(b, &ch, &cl, &cs, &maxlt);
  if (c && b) {
    /* Use painted colour (we'll darken it in RGB space) */
    cocol(c, &ch, &cl, &cs);
  }

  hsl_to_rgb(ch, cl, cs, &fr, &fg, &fb);

  if (c) {
    fr = fr * maxlt;
    fg = fg * maxlt;
    fb = fb * maxlt;
  }

  fr = 255.0 * fr;
  fg = 255.0 * fg;
  fb = 255.0 * fb;

  if (pcr) { *pcr = fr; }
  if (pcg) { *pcg = fg; }
  if (pcb) { *pcb = fb; }
} /* End of bc.col */

/* Compute the colour that should be drawn at a certain pixel position, with
   options for axonometric. This effectively does ray-tracing (via the
   subroutine ftop.most) and handles transparency by finding the next-lower
   block of a differing type or colour. */
void colorat(int fnum, float we, float ns, float *pcr, float *pcg, float *pcb,
  float ski, int cloudok)
{
  int b; int c = 0; int z = 64;
  float r1, g1, b1;
  float r2, g2, b2;
  float we_pos, ns_pos;

  /* Find the topmost block and colour */
  we_pos = we; ns_pos = ns;
  b = ftopmost(fnum, &z, &we_pos, &ns_pos, &c, cloudok, ski);
  bc_col(b, c, &r1, &g1, &b1);

  /* See if we reached the bottom or hit a solid block: in either case we're
     done; but if we got a transparent block, we need to keep looking */
  if (z <= 0) {
    /* We got all the way to the bottom, so there's nothing left to find. */
    *pcr = r1; *pcg = g1; *pcb = b1;
  } else if ( (BLATR(b) & ATTR_TRANSPARENT) == 0 ) {
    /* We found an opaque block. */
    *pcr = r1; *pcg = g1; *pcb = b1;
  } else {
    /* Continue down looking for the next block that is not transparent.
       (%%% if we have another transparent block but its colour is different,
       we would have multiple-transparency, but we ignore that for now)  */
    int ub, uc; float upalpha;
    ub = b; uc = c; /* underblock, undercolour */
    while ((z > 0) && (BLATR(ub) & ATTR_TRANSPARENT) ) {
      ub = ftopmost(fnum, &z, &we_pos, &ns_pos, &uc, cloudok, ski);
      bc_col(ub, uc, &r2, &g2, &b2);
    }
    /* %%% alpha should be a field in the g_blt[] structure */
    if (b == BL_FENCE) {
      upalpha = 0.9;
    } else if (b == BL_NEW_FLW) {
      upalpha = 0.25;
    } else {
      upalpha = 0.5;
    }
    *pcr = upalpha*r1 + (1.0-upalpha)*r2;
    *pcg = upalpha*g1 + (1.0-upalpha)*g2;
    *pcb = upalpha*b1 + (1.0-upalpha)*b2;
  }
} /* End of color.at */

/* Call color.at 4 times to create a "dithered" (really supersampled
   anti-aliased) approximation */
void dthclrat(int fnum, float we, float ns, float *pcr, float *pcg, float *pcb,
  float mul, int cloudok)
{
  float cr, cg, cb;
  float tr, tg, tb;
  float t1, t2, inc, max;

  inc = 1.0 / (2.0 * mul); /* Normally 0.5 */
  max = 0.6 / mul; /* Normally 0.6 */
  tr = tg = tb = 0.0;
  for(t1=0.3; t1<0.3+max; t1+=inc) {
    for(t2=0.0; t2<0.0+max; t2+=inc) {
      colorat(fnum, we-t1, ns-t2, &cr, &cg, &cb, 0.2, cloudok);
      tr += cr; tg += cg; tb += cb;
    }
  }

  if (pcr) { *pcr = tr/4.0; }
  if (pcg) { *pcg = tg/4.0; }
  if (pcb) { *pcb = tb/4.0; }
} /* End of dth.clrat */

const char * sig = "Eden Maps by Robert Munafo "
                   "(\010\011\012\013\014\015\016\017)";

/* full map, graphical.
  The 'mode' parameter is a colour mode. This also tests the global selmode
to show a single horizontal slice at g_slicemode_z1 if the colour mode is set
to normal; the other combinations of selmodes and colour modes are not
supported. */
void cmd_fmg(int fnum, int mode, int mag)
{
  int we_min, we_max, ns_min, ns_max;
  int hgt, wid, we, ns;
  int rsub, csub, pixrow, pixcol;
  FILE * ofile;
  int cap_x, cap_y;
  char tnam1[MAX_NAME_LEN+1];
  char tnam2[MAX_NAME_LEN+1];
  char tofn[2*MAX_NAME_LEN];
  char caption[200];

  check_nfnum();

  if (ns_lim1 && ns_lim2 && ew_lim1 && ew_lim2
    && (ns_lim1 != ns_lim2) && (ew_lim1 != ew_lim2))
  {
    ns_min = min(ns_lim1, ns_lim2);
    ns_max = max(ns_lim1, ns_lim2);
    we_min = min(ew_lim1, ew_lim2);
    we_max = max(ew_lim1, ew_lim2);
  } else {
    we_min = (g_xmin[fnum] * 16) - 16;
    we_max = (g_xmax[fnum] * 16) + 32;
    ns_min = (g_ymin[fnum] * 16) - 16;
    ns_max = (g_ymax[fnum] * 16) + 16;
  }

  /* Position the caption */
  if (mode == CM_NORMAL) {
    /* Add space for axonometric projection */
    rsub = (16 / mag);
    if ((rsub * mag) < 16) {
      rsub += 1;
    }
    ns_min -= rsub;
  }
  cap_x = we_max - 8; cap_y = ns_min + 5;

  hgt = ns_max - ns_min; wid = we_max - we_min;
  printf("Generating %s, %d x %d pixels\n", of_name, mag*wid, mag*hgt);
  if (0) {
  } else if (g_selmode == SM_SLICE) {
    char suf[20];
    printf("  mode: horizontal slice at z=%d.\n", g_slicemode_z1);
    sprintf(suf, "-slice-%02d", g_slicemode_z1);
    scan_name(g_name, fnum, 2, of_name, suf, 0);
    sprintf(caption, "%s -- World: [%s]   Slice at z=%d",
      sig, g_name, g_slicemode_z1);
  } else if (mode == CM_DIFF) {
    printf("  mode: diff from world %c to world %c.\n",
      fnum_to_char(fnum), fnum_to_char(g_diffmode_fnum));
    scan_name(g_name, g_diffmode_fnum, 2, of_name, "-diff", 0);
    scan_name(tnam1, fnum, 0, tofn, 0, 0);
    sprintf(caption, "%s -- Changes from [%s] to [%s]", sig, tnam1, g_name);
  } else if (mode == CM_DIF3) {
    printf("  mode: 3-way diff, base %c, blue %c, green %c.\n",
      fnum_to_char(fnum),
      fnum_to_char(g_diffmode_fnum), fnum_to_char(g_diffmode_f2));
    scan_name(g_name, fnum, 2, of_name, "-dif3", 0);
    scan_name(tnam1, g_diffmode_fnum, 0, tofn, 0, 0);
    scan_name(tnam2, g_diffmode_f2, 0, tofn, 0, 0);
    sprintf(caption, "%s -- [%s] vs.[%s] in blue, vs. [%s] in green",
      sig, g_name, tnam1, tnam2);
  } else if (g_colormode == CM_TUNNEL) {
    printf("  mode: height-map of tunnels\n");
    scan_name(g_name, fnum, 2, of_name, "-tun", 0);
    sprintf(caption, "%s -- World: [%s]   tunnel height-map", sig, g_name);
  } else if (mode == CM_STRUCT) {
    printf("  mode: colormap of structures (caves, buildings and tunnels)\n");
    scan_name(g_name, fnum, 2, of_name, "-struct", 0);
    sprintf(caption, "%s -- World: [%s]   with structures color-coded",
      sig, g_name);
  } else if (mode == CM_ORTHO) {
    printf("  mode: normal color, orthographic, not anti-aliased.\n");
    scan_name(g_name, fnum, 2, of_name, "-ortho", 0);
    sprintf(caption, "%s -- World: [%s]   orthographic", sig, g_name);
  } else { /* (mode == CM_NORMAL) */
    printf("  mode: normal color, axonometric, anti-aliased.\n");
    scan_name(g_name, fnum, 2, of_name, "", 0);
    sprintf(caption, "%s -- World: [%s]   axonometric", sig, g_name);
  }

  if (g_fixed_name) {
    printf("Using fixed output name %s instead.\n", g_fixed_name);
    ofile = fopen(g_fixed_name, "w");
  } else {
    ofile = fopen(of_name, "w");
  }
  fprintf(ofile, "P3\n");
  fprintf(ofile, "# Eden world map conversion by Robert Munafo (MROB27)\n");
  fprintf(ofile, "# Source: %s\n", g_fnames[fnum]);
  fprintf(ofile, "# Caption: %s\n", caption + strlen(sig));
  fprintf(ofile, "%d %d\n", mag*wid, mag*hgt);
  fprintf(ofile, "255\n");
  pixrow = (1 - mag) * (ns_max - ns_min);
  for (ns = ns_max; ns > ns_min; ns--) {
    printf("fmg: on row %d of %d...\r", ns_max-ns, hgt); fflush(stdout);
    for(rsub=0; rsub<mag; rsub++) {
      pixcol = 0;
      for (we = we_max; we > we_min; we--) {
        for (csub=0; csub<mag; csub++) {
          float fr, fg, fb;
          int cr, cg, cb, dcmap, b, c, cloudok;
          if ( ((ns*mag-rsub) & (we*mag-csub) & 0x01) == 0x01 ) {
            cloudok = 1;
          } else {
            cloudok = 0;
          }
          b = fontfunction(we_max - pixcol, ns_max - pixrow,
            caption, cap_x, cap_y);
          if (0) {
          } else if (b >= 0) {
            if (b) {
              /* Font text: black */
              fr = fg = fb = 0.0;
            } else {
              /* On a white background */
              fr = fg = fb = 255.0;
            }
          } else if (g_selmode == SM_SLICE) {
            /* Single horizontal slice */
            b = blockval(fnum, ns, we, g_slicemode_z1, &c, 0);
            bc_col(b, c, &fr, &fg, &fb);
          } else if (mode == CM_DIFF) {
            /* Compare two worlds. Start with colour of world 1 in grayscale */
            colorat(fnum, ns, we, &fr, &fg, &fb, 0.0, cloudok);
            fr = fg = fb = 0.6 * ((0.299*fr) + (0.587*fg) + (0.114*fb));
            dcmap = diffcol(fnum, g_diffmode_fnum, ns, we);
            if (dcmap & DB_REMOVE) { fr += 128.; }
            if (dcmap & DB_ADD) { fg += 128.; }
            if (dcmap & DB_CHANGE) { fb += 128.; }
          } else if (mode == CM_DIF3) {
            /* Compare three worlds. Start with colour of world 1 in
               grayscale */
            colorat(fnum, ns, we, &fr, &fg, &fb, 0.0, cloudok);
            fr = fg = fb = 0.6 * ((0.299*fr) + (0.587*fg) + (0.114*fb));
            dcmap = d3_col(fnum, g_diffmode_fnum, g_diffmode_f2, ns, we);
            if (dcmap & 1) { fr += 128.; }
            if (dcmap & 2) { fg += 128.; }
            if (dcmap & 4) { fb += 128.; }
          } else if (mode == CM_TUNNEL) {
            /* Show world in grayscale with tunnel depth-map in colour */
            colorat(fnum, ns, we, &fr, &fg, &fb, 0.0, cloudok);
            fr = fg = fb = 0.6 * ((0.299*fr) + (0.587*fg) + (0.114*fb));
            dcmap = tm_col(fnum, ns, we, 0, 0);
            if (dcmap & TM_DEEP) { fr += 128.; }
            if (dcmap & TM_MEDIUM) { fg += 128.; }
            if (dcmap & TM_SHALLOW) { fb += 128.; }
          } else if (mode == CM_STRUCT) {
            /* Show world in grayscale with structures highlighted by colour */
            colorat(fnum, ns, we, &fr, &fg, &fb, 0.0, cloudok);
            fr = fg = fb = 0.6 * ((0.299*fr) + (0.587*fg) + (0.114*fb));
            dcmap = struct_col(fnum, ns, we);
            if (dcmap & SC_MANMADE) { fr += 128.; }
            if (dcmap & SC_OVERHANG) { fg += 64.; }
              if (dcmap & SC_MAN_TUNNEL) { fg += 64.; }
            if (dcmap & SC_CAVE) {
              fb += 80.;
              /* Now check the extra bits telling the height of the cave */
              if ((dcmap & SC_CVHT3) == 0) { fr += 16.; }
              if (dcmap & SC_CVHT1) { fb += 48.; }
              if (dcmap & SC_CVHT2) { fb += 96.; fg += 16.; }
              if ((dcmap & SC_CVHT3) == SC_CVHT3) { fg += 8.; }
            }
          } else if (mode == CM_ORTHO) {
            /* Ortho colour mode: Non-antialiased, top-down single-world map */
            colorat(fnum, ns, we, &fr, &fg, &fb, 0.0, cloudok);
          } else { /* (mode == CM_NORMAL) */
            /* Normal single-world map */
            dthclrat(fnum,
              ((float) ns) - (((float) rsub)/((float) mag)),
              ((float) we) - (((float) csub)/((float) mag)),
              &fr, &fg, &fb, ((float)mag), cloudok);
          }
          cr = fr;
          cg = fg;
          cb = fb;
    
          /* Clip RGB components to the range 0..255 */
          cr = minmax(cr, 0, 255);
          cg = minmax(cg, 0, 255);
          cb = minmax(cb, 0, 255);
    
          fprintf(ofile, "%d %d %d\n", cr, cg, cb);

          pixcol++;
        } /* End of csub loop */
      } /* End of we loop */
      pixrow++;
    } /* End of rsub loop */
  } /* End of ns loop */
  fclose(ofile);
  printf("\rfmg done: wrote %d rows of pixels\n", mag*hgt);
  printf("cache hit rate: %ld hits, %ld miss, total %ld\n", chf_hits,
      chf_trys - chf_hits, chf_trys);
} /* End of cmd.fmg */

int g_verbose = 0;

char g_cmdbuf[CMDLEN+1];
char * g_cmd;

int g_argc;
char * * g_argv;

char * gi_xtra_cmd = 0;

int get_input(char *s, int lim, int prompt)
{
  char *res;
  int ilen;
  char *arg;

  if (g_argc > 0) {
    /* Get a command-line argument */
    arg = *g_argv;
    ilen = strlen(arg);
    g_cmd = arg;
    printf("cmdline arg: '%s'\n", arg);
    g_argv++; g_argc--;
  } else if (gi_xtra_cmd) {
    /* Caller injected a command */
    g_cmd = gi_xtra_cmd;
    if (g_verbose) {
      printf("xtra cmd: '%s'\n", g_cmd);
    }
    ilen = strlen(g_cmd);
    gi_xtra_cmd = 0;
  } else {
    /* Prompt user for a command */
    if (prompt) {
      printf("%s command (? for help) > ", PROGNAME);
    }
    fflush(stdout);
    res = fgets(s, lim, stdin);
    if (!res) {
      exit(0);
    }
    ilen = strlen(s) - 1; /* -1 removes the \n at the end */
    s[ilen]=0;
    g_cmd = s;
    if (g_verbose) {
      printf("cmd: '%s'\n", s);
    }
  }
  return(ilen);
} /* End of get.input */

void show_cur_world(int vncol)
{
  printf("Current world is: ");
  scan_name(g_name, g_cur_file, 1, of_name, 0, vncol);
}

void check_z_bounds(int z1, int z2)
{
  if ((z1 < 0) || (z2 < 0)) {
    printf("Note: the Z bounds are currently off-scale low.\n");
  }
  if ((z1 > 63) || (z2 > 63)) {
    printf("Note: the Z bounds are currently off-scale high.\n");
  }
}

/* Move by a given amount in ns, we, and z, then call set.limits and
   check_z_bounds. */
void msl(int delta_ns, int delta_we, int delta_z)
{
  if ((delta_ns != 0) || (delta_we != 0)) {
    cur_ns += delta_ns;
    cur_we += delta_we;
    set_limits();
  }

  /* To avoid redundent zbounds error messages, we only do this part when a
     nonzero zbound has been passed. */
  if (delta_z) {
    g_slicemode_z2 += delta_z; g_slicemode_z1 += delta_z;
    check_z_bounds(g_slicemode_z1, g_slicemode_z2);
  }
} /* End of msl */

int g_last_msl_dns = 0;
int g_last_msl_dwe = 0;
int g_last_msl_dz = 0;

/* Perform a msl() operation and remember the deltas */
void msl_mem(int delta_ns, int delta_we, int delta_z)
{
  msl(delta_ns, delta_we, delta_z);
  g_last_msl_dns = delta_ns;
  g_last_msl_dwe = delta_we;
  g_last_msl_dz = delta_z;
}

/* Repeat the previous msl() */
void msl_rpt(void)
{
  msl(g_last_msl_dns, g_last_msl_dwe, g_last_msl_dz);
}

/* Move in ns and/or we, then do an 'srs' */
void move_srs(int delta_ns, int delta_we)
{
  msl_mem(delta_ns, delta_we, 0);
  srs_dispatch(g_cur_file, 0);
}

void coords_init(void)
{
  g_colormode = CM_NORMAL;
  g_selmode = SM_TOPMOST;
  g_cur_wid = g_cur_hei = (80 - 4)/2;
  chunkxy_to_blockxy(4096, 4096, 7, &cur_ns, &cur_we);
  set_limits();
  ns_lim1 = ns_lim2 = ns_max;
  ew_lim1 = ew_lim2 = we_max;
} /* End of coords.init */

void brief_help(void)
{
  printf("%s",
    "  ?            help\n"
    "  colors       show table of Eden colours\n"
    "  crc          show map shape signature (CRC of blocks' coordinates)\n"
    "  diff B       display map as colour-coded differences between current\n"
    "               world and world 'B'\n"
    "  diff B C     display map as colour-coded differences between current\n"
    "               world and worlds 'B' and 'C'\n"
    "  diff         cancel or re-enable diff display\n"
    "  d, u         move down or up (works in 'plan' and 'slice' views)\n"
    "  e, n, s, w,  move one chunk in the given direction and do 'srs'\n"
    "  ne,nw,se,sw\n"
    "               all ten direction commands take an argument to move by\n"
    "               blocks, like 'ne 7' to move northeast 7 blocks\n"
    "  fmg          full map graphical (makes a PPM file of the whole world)\n"
    "                 MODES for 'fmg':\n"
    "                 tl , br    set top-left and bottom-right\n"
    "                 slice 10   show single layer at level 10\n"
    "                 top        show top view\n"
    "                 struct     show natural and manmade structures\n"
    "                 tun        show deep, mid and shallow tunnels\n"
    "  frame        set tl and br to edges of currently-selected world\n"
    "  gm 7         go mark: return to position at last 'sm 7' command\n"
    "  gmag 3       graphics magnification: 'fmg' will make an image 3x as big\n"
    "  inset 3      decrease current view by 3 blocks on all sides\n"
    "  ins3 2       decrease current view by 2 blocks on all sides,\n"
    "               including vertically\n"
    "  legend       show tables of block symbols and colouring used in\n"
    "               special display modes\n"
    "  lw           list all currently-open worlds (files)\n"
    "  name         tell the name of the current world\n"
    "  norm         cancel any special display node\n"
    "  ortho        use orthographic projection (straight-down) in 'fmg'\n"
    "  outset 3     increase current view by 3 blocks on all sides\n"
    "  out3 2       increase current view by 2 blocks on all sides,\n"
    "               including vertically\n"
    "  plan 12 4    series of horizontal cross-sections from height 12 to 4\n"
    "  plan 32      plan view (horizontal cross-section) at height 32\n"
    "  plan         equivalent to 'plan 63 0'\n"
    "  q            quit\n"
    "  slice 32     show a horizontal slice at height 32\n"
    "  sm 7         set mark: remember the current position as marker #7\n"
    "  spawn1       go to initial game spawn point\n"
    "  src          short-range scan (small text map), colours\n"
    "  srs          short-range scan (small text map), blocks\n"
    "  struct       display map with colour-coding for manmade structures,\n"
    "               tunnels, and caves.\n"
    "  toc          show 'Table of Contents' (list of map chunks)\n"
    "  top          show topmost block instead of plan or slice\n"
    "  tl, br       set current center coordinates as the 'top-left' and\n"
    "               'bottom-right' (respectively) for use by 'fmg' command\n"
    "  tl 65367 65703 Set top-left to specific block address\n"
    "  tlc 4095 4102  Set top-left to specific chunk address\n"
    "  tpt          Do 'top', 'plan', then 'top'\n"
    "  tt B C       'srs' will show three worlds side-by-side using diff3\n"
    "               colouring as if by command 'diff B C'\n"
    "  tt           cancel or re-enable triptych display\n"
    "  tun          display map with tunnels, caves and excavations\n"
    "               highlighted in colour and colour-coded by depth.\n"
    "  wh 38 20     set width of 'srs' output to 38 blocks, and height to\n"
    "               20 blocks; center position remains unchanged\n"
    "  wh 27        set width and height of 'srs' output to 27 blocks each;\n"
    "               center position remains unchanged\n"
    "  world A      switch to world 'A' (use 'lw' for list)\n"
  );
} /* End of brief.help */

void isos(int dxy, int dz)
{
  /* printf("isos(%d, %d)\n", dxy, dz); */
  /* 3-d inset */
  if ( ( (g_slicemode_z1 - g_slicemode_z2) >= dz*2)
      && (g_cur_wid > dxy*2) && (g_cur_hei > dxy*2) ) {
    g_slicemode_z2 += dz; g_slicemode_z1 -= dz;
    g_cur_wid -= (2*dxy); g_cur_hei -= (2*dxy);
    set_limits();
    check_z_bounds(g_slicemode_z1, g_slicemode_z2);
    srs_dispatch(g_cur_file, 0);
  } else if ( (g_cur_wid > dxy*2) && (g_cur_hei > dxy*2) ) {
    /* dxy is OK, so it must be the z direction that's the problem */
    printf("Selection is too short in the Z direction to inset by %d\n", dz);
  } else {
    printf("Selection is too small to inset by %d\n", dxy);
  }
} /* End of is.os */

void cmd_plan(int z1, int z2)
{
  int t;
  /* z1 should be the higher */
  if (z1 < z2) {
    t = z1; z1 = z2; z2 = t;
  }
  g_slicemode_z1 = minmax(z1, 0, 63);
  g_slicemode_z2 = minmax(z2, 0, 63);
  if (g_slicemode_z1 == g_slicemode_z2) {
    g_selmode = SM_SLICE;
  } else {
    g_selmode = SM_PLAN;
  }
  srs_dispatch(g_cur_file, 0);
} /* End of cmd.plan */

/* Set the current world, and reset any special multi-world display mode(s).
   Return values are:
     -1 Could not set world, given world does not exist
     1  World was already set to this value
     2  Successfully changed world
 */
int cmd_world(char c1)
{
  int p1, p2, rv;

  /* Switch worlds */
  rv = 0;
  p1 = char_to_fnum(c1, &p2);
  if (!(p2)) {
    printf("There is no world '%c'.\n", c1);
    rv = -1;
  } else if (p1 == g_cur_file) {
    printf("World '%c' is already the current world.\n",
      fnum_to_char(g_cur_file));
    rv = 1;
  } else {
    g_cur_file = p1;
    rv = 2;
    printf("Switching to world %c.\n", fnum_to_char(g_cur_file));
    scan_name(g_name, g_cur_file, 2, of_name, 0, 06);
    /* When we switch worlds, the diff and triptych modes no longer
       make sense until you re-select what to diff against. */
    if ((g_colormode == CM_DIFF) || (g_colormode == CM_DIF3)
        || (g_triptych)) {
      printf("Returning to normal display mode\n");
      g_triptych = 0;
      g_colormode = CM_NORMAL;
    }
    srs_dispatch(g_cur_file, 0);
  }
  return(rv);
} /* End of cmd.world */

int cmdloop(int one_only)
{
  int gg;

  gg = 1;
  while(gg) {
    int nv; int p1, p2; char c1, c2;

    nv = get_input(g_cmdbuf, CMDLEN, 1);

    if (nv < 0) {
      /* EOF or other error */
      printf("get_input returned negative result, exiting.\n");
      gg = 0;
    } else if (nv==0) {
      printf("Input not understood. Enter ? for list of commands, or q to quit.\n");
    } else {
      /* Parse commands */
      /* printf("g_cmd: '%s'\n", g_cmd); */
      if (0) {
      } else if ( (strcmp(g_cmd, "?") == 0)
               || (strcmp(g_cmd, "help") == 0) ) {
        /* help */
        printf("%s commands:\n", PROGNAME);
        brief_help();

      } else if (nv = sscanf(g_cmd, "br %d %d", &p1, &p2),
                (nv == 2)) {
        ns_lim2 = p1; ew_lim2 = p2;
        printf("bottom-right set to (%d, %d)\n", ns_lim2, ew_lim2);
      } else if (strcmp(g_cmd, "br") == 0) {
        ns_lim2 = cur_ns; ew_lim2 = cur_we;
        printf("bottom-right set to (%d, %d)\n", ns_lim2, ew_lim2);

      } else if (nv = sscanf(g_cmd, "brc %d %d", &p1, &p2),
                (nv == 2)) {
        ns_lim2 = p1*16; ew_lim2 = p2*16;
        printf("bottom-right set to chunk (%d,%d)'s bottom-right = (%d, %d)\n",
                                              p1, p2, ns_lim2, ew_lim2);

      } else if (strcmp(g_cmd, "colors") == 0) {
        colors();

      } else if (strcmp(g_cmd, "crc") == 0) {
        show_toc(g_cur_file, TOC_COUNTS);

      } else if (nv = sscanf(g_cmd, "d %d", &p1), (nv == 1)) {
        /* Down with parameter */
        msl_mem(0, 0, -p1);
        srs_dispatch(g_cur_file, 0);

      } else if (strcmp(g_cmd, "d") == 0) {
        /* Move down one slice */
        msl_mem(0, 0, -1);
        srs_dispatch(g_cur_file, 0);

      } else if (nv = sscanf(g_cmd, "diff %c %c", &c1, &c2), (nv == 2)) {
        int v1, v2;
        /* three-way diff mode: changes from present world to worlds c1, c2*/
        p1 = char_to_fnum(c1, &v1);
        p2 = char_to_fnum(c2, &v2);
        if (v1 && v2) {
          if (g_cur_file == p1) {
            printf("%c is the current world, no diff.\n", c1);
          } else if (g_cur_file == p2) {
            printf("%c is the current world, no diff.\n", c2);
          } else {
            if (g_triptych) {
              printf("Triptych mode cancelled.\n");
              g_triptych = 0;
            }
            printf("colour will show three-way diffs, "
              "with %c as base; %c in blue and %c in green\n",
                fnum_to_char(g_cur_file),
                fnum_to_char(p1), fnum_to_char(p2) );
            g_fmg_mode = g_colormode = CM_DIF3;
            g_diffmode_fnum = p1;
            g_diffmode_f2 = p2;
            srs_dispatch(g_cur_file, 0);
          }
        } else {
          printf("At least one of '%c' and '%c' is not valid. Use 'lw' to see list.\n", c1, c2);
        }
      } else if (nv = sscanf(g_cmd, "diff %c", &c1), (nv == 1)) {
        /* set mode to diff: changes from world c1 to present world */
        p1 = char_to_fnum(c1, &p2);
        if (p2) {
          if (g_cur_file == p1) {
            printf("%c is the current world, no diff.\n", c1);
          } else {
            if (g_triptych) {
              printf("Triptych mode cancelled.\n");
              g_triptych = 0;
            }
            printf("colour will show diffs from %c to %c\n", 
              fnum_to_char(g_cur_file), fnum_to_char(p1));
            g_fmg_mode = g_colormode = CM_DIFF;
            g_diffmode_fnum = p1;
            srs_dispatch(g_cur_file, 0);
          }
        } else {
          printf("There is no world '%c'.\n", c1);
        }
      } else if (strcmp(g_cmd, "diff") == 0) {
        /* Toggle diff mode (if possible) */
        if (g_colormode == CM_DIFF) {
          printf("Diff mode cancelled, back to normal display.\n");
          g_fmg_mode = g_colormode = CM_NORMAL;
          srs_dispatch(g_cur_file, 0);
        } else if ((g_diffmode_fnum >= 0) && (g_diffmode_f2 >= 0)) {
          printf("Back to three-way diff mode, %c vs. %c and %c\n", 
              fnum_to_char(g_cur_file), fnum_to_char(g_diffmode_fnum),
              fnum_to_char(g_diffmode_f2));
          g_fmg_mode = g_colormode = CM_DIF3;
          srs_dispatch(g_cur_file, 0);
        } else if (g_diffmode_fnum >= 0) {
          printf("Resuming diff display of %c against %c\n", 
            fnum_to_char(g_cur_file), fnum_to_char(g_diffmode_fnum));
          g_fmg_mode = g_colormode = CM_DIFF;
          srs_dispatch(g_cur_file, 0);
        } else {
          printf(
  "Cannot re-enable diff mode because selected world has changed or diff\n"
  "world(s) have not been chosen. To display in diff mode, specify one or\n"
  "two worlds to diff against, e.g. 'diff B' or 'diff C D'.\n");
        }

      } else if (nv = sscanf(g_cmd, "e %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(0, -p1);
      } else if (strcmp(g_cmd, "e") == 0) {
        move_srs(0, -16);
      } else if (strcmp(g_cmd, "fmg") == 0) {
        /* full map, graphical */
        printf("calling fmg(%d, %d)\n", g_cur_file, g_fmg_mode);
        cmd_fmg(g_cur_file, g_fmg_mode, g_fmg_mag);

      } else if (strcmp(g_cmd, "fon") == 0) {
        /* Fixed output name */
        g_fixed_name = "temp-map.ppm";

      } else if (strcmp(g_cmd, "frame") == 0) {
        /* Set full frame based on present world */
        ns_lim1 = (g_ymin[g_cur_file] * 16) - 16;
        ns_lim2 = (g_ymax[g_cur_file] * 16) + 16;
        ew_lim1 = (g_xmin[g_cur_file] * 16) - 16;
        ew_lim2 = (g_xmax[g_cur_file] * 16) + 32;

        printf("setting fmg bounds to show all of world '%c'\n",
          fnum_to_char(g_cur_file));
        printf("  top-left set to (%d, %d)\n", ns_lim1, ew_lim2);
        printf("  bottom-right set to (%d, %d)\n", ns_lim2, ew_lim1);

      } else if (nv = sscanf(g_cmd, "gc %d %d", &p1, &p2),
                (nv == 2)) {
        long t; int ns, we;
        /* Go to a specific chunk */
        ns = p1; we = p2;
        t = toc_hlu(g_cur_file, we, ns);
        if (t >= 0) {
          gochunk(we, ns);
          g_cur_wid = 16; g_cur_hei = 16;
          set_limits();
          printf("Moved to chunk (%d, %d)\n", ns, we);
          srs_dispatch(g_cur_file, 0);
        } else {
          printf("There is no chunk at (%d, %d)\n", ns, we);
        }

      } else if (nv = sscanf(g_cmd, "gm %d", &p1), (nv == 1)) {
        /* go mark */
        p1 = minmax(p1, 0, MAX_MARKS-1);
        move_srs(marks_ns[p1] - cur_ns, marks_we[p1] - cur_we);

      } else if (nv = sscanf(g_cmd, "gmag %d", &p1), (nv == 1)) {
        /* Graphics magnification */
        if ((p1 >= 1) && (p1 <= 4)) {
          g_fmg_mag = p1;
        } else {
          printf("gmag can only be from 1 to 4\n");
        }

      } else if (nv = sscanf(g_cmd, "header %c", &c1), (nv == 1)) {
        /* Show header of world c1 */
        p1 = char_to_fnum(c1, &p2);
        if (p2) {
          cmd_header(p1);
        } else {
          printf("There is no world '%c'.\n", c1);
        }
      } else if (strcmp(g_cmd, "header") == 0) {
        /* Show header of current world */
        cmd_header(g_cur_file);

      } else if (strcmp(g_cmd, "hf") == 0) {
        /* Show header fields */
        cmd_hf(g_cur_file);

      } else if (nv = sscanf(g_cmd, "ins3 %d", &p1), (nv == 1)) {
        /* Inset by p1 in all dimensions */
        isos(p1, p1);
      } else if (strcmp(g_cmd, "ins3") == 0) {
        /* Inset by 1 in all dimensions */
        isos(1, 1);
      } else if (nv = sscanf(g_cmd, "inset %d", &p1), (nv == 1)) {
        /* Inset by p1 in x and y dimensions */
        isos(p1, 0);
      } else if (strcmp(g_cmd, "inset") == 0) {
        /* Inset by 1 in x and y dimensions */
        isos(1, 0);
      } else if (strcmp(g_cmd, "legend") == 0) {
        legend();
      } else if (strcmp(g_cmd, "lw") == 0) {
        allnames();
      } else if (strcmp(g_cmd, "mobs") == 0) {
        cmd_mobs(g_cur_file, 1);
      } else if (nv = sscanf(g_cmd, "n %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(p1, 0);
      } else if (strcmp(g_cmd, "n") == 0) {
        move_srs(16, 0);
      } else if (strcmp(g_cmd, "name") == 0) {
        scan_name(g_name, g_cur_file, 1, of_name, 0, 06);
      } else if (nv = sscanf(g_cmd, "ne %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(p1, -p1);
      } else if (strcmp(g_cmd, "ne") == 0) {
        move_srs(16, -16);
      } else if (strcmp(g_cmd, "norm") == 0) {
        /* switch mode to standard axonometric antialiased */
        g_fmg_mode = g_colormode = CM_NORMAL;
        if (g_triptych && (g_selmode != SM_TOPMOST)) {
          /* We were in triptych slice display, let's stay that way but just
             change the colouring mode */
        } else {
          /* In non-slice triptych mode, or in non-triptych slice mode, what
             we want is to return both the colouring and the selmode to normal
             (because one of them is already normal, the command would be
             pointless unless it also returns the other thing to normal). */
          g_selmode = SM_TOPMOST;
        }
        printf("mode set to axonometric antialiased.\n");
        srs_dispatch(g_cur_file, 0);
      } else if (nv = sscanf(g_cmd, "nw %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(p1, p1);
      } else if (strcmp(g_cmd, "nw") == 0) {
        move_srs(16, 16);
      } else if (strcmp(g_cmd, "ortho") == 0) {
        /* switch mode to orthographic parallel, non-antialiased */
        g_fmg_mode = CM_ORTHO;
        g_colormode = CM_NORMAL;
        printf("'fmg' mode set to parallel orthographic non-antialiased.\n");
        srs_dispatch(g_cur_file, 0);
      } else if (nv = sscanf(g_cmd, "out3 %d", &p1), (nv == 1)) {
        /* Outset by p1 in all dimensions */
        isos(-p1, -p1);
      } else if (strcmp(g_cmd, "out3") == 0) {
        /* Outset by 1 in all dimensions */
        isos(-1, -1);

      } else if (strcmp(g_cmd, "outliers") == 0) {
        show_toc(g_cur_file, TOC_OUTLIERS | TOC_COUNTS);

      } else if (nv = sscanf(g_cmd, "outset %d", &p1), (nv == 1)) {
        /* Outset by p1 in x and y dimensions */
        isos(-p1, 0);
      } else if (strcmp(g_cmd, "outset") == 0) {
        /* Outset by 1 in x and y dimensions */
        isos(-1, 0);

      } else if (nv = sscanf(g_cmd, "plan %d %d", &p1, &p2),
                (nv == 2)) {
        /* Do a series of plan (horizontal cross-section) views */
        cmd_plan(p1, p2);
      } else if (nv = sscanf(g_cmd, "plan %d", &p1),
                (nv == 1)) {
        /* Do a single plan view */
        cmd_plan(p1, p1);
      } else if (strcmp(g_cmd, "plan") == 0) {
        /* Do plan 63-0 (this is the version  of 'plan' I use most) */
        cmd_plan(63, 0);
      } else if (strcmp(g_cmd, "q") == 0) {
        /* quit */
        gg = 0;
      } else if (nv = sscanf(g_cmd, "s %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(-p1, 0);
      } else if (strcmp(g_cmd, "s") == 0) {
        move_srs(-16, 0);
      } else if (nv = sscanf(g_cmd, "se %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(-p1, -p1);
      } else if (strcmp(g_cmd, "se") == 0) {
        move_srs(-16, -16);
      } else if (nv = sscanf(g_cmd, "slice %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 63);
        g_selmode = SM_SLICE;
        g_slicemode_z1 = g_slicemode_z2 = p1;
        printf("mode set to slice (at z=%d).\n", g_slicemode_z1);
        srs_dispatch(g_cur_file, 0);
      } else if (nv = sscanf(g_cmd, "sm %d", &p1), (nv == 1)) {
        /* set mark */
        p1 = minmax(p1, 0, MAX_MARKS-1);
        marks_ns[p1] = cur_ns;
        marks_we[p1] = cur_we;
      } else if (strcmp(g_cmd, "spawn1") == 0) {
        /* Go to initial spawn point */
        go_1st_spawn();
        srs_dispatch(g_cur_file, 0);
      } else if (strcmp(g_cmd, "src") == 0) {
        /* Short range scan, colours */
        srs_dispatch(g_cur_file, 1);
      } else if (strcmp(g_cmd, "srs") == 0) {
        /* Short range scan */
        srs_dispatch(g_cur_file, 0);
      } else if (strcmp(g_cmd, "struct") == 0) {
        /* switch mode to colour-coded structures */
        g_fmg_mode = g_colormode = CM_STRUCT;
        printf("mode set to colour-coded structures.\n");
        srs_dispatch(g_cur_file, 0);
      } else if (nv = sscanf(g_cmd, "sw %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(-p1, p1);
      } else if (strcmp(g_cmd, "sw") == 0) {
        move_srs(-16, 16);

      } else if (nv = sscanf(g_cmd, "tl %d %d", &p1, &p2),
                (nv == 2)) {
        ns_lim1 = p1; ew_lim1 = p2;
        printf("top-left set to (%d, %d)\n", ns_lim1, ew_lim1);
      } else if (strcmp(g_cmd, "tl") == 0) {
        ns_lim1 = cur_ns; ew_lim1 = cur_we;
        printf("top-left set to (%d, %d)\n", ns_lim1, ew_lim1);

      } else if (nv = sscanf(g_cmd, "tlc %d %d", &p1, &p2),
                (nv == 2)) {
        ns_lim1 = p1*16+16; ew_lim1 = p2*16+16;
        printf("top-left set to chunk (%d,%d)'s top-left = (%d, %d)\n",
                                              p1, p2, ns_lim1, ew_lim1);

      } else if (strcmp(g_cmd, "toc") == 0) {
        show_toc(g_cur_file, TOC_ALL | TOC_COUNTS);

      } else if (strcmp(g_cmd, "top") == 0) {
        /* switch selmode to topmost */
        g_selmode = SM_TOPMOST;
        printf("display topmost block in each position.\n");
        srs_dispatch(g_cur_file, 0);
      } else if (strcmp(g_cmd, "tpt") == 0) {
        /* do top, plan, top */
        g_selmode = SM_TOPMOST; g_no_wcbt = 1; srs_dispatch(g_cur_file, 0);
        cmd_plan(63, 0);
        printf("\n"); /* Makes it align properly */
        g_selmode = SM_TOPMOST; srs_dispatch(g_cur_file, 0);

      } else if (strcmp(g_cmd, "tsum") == 0) {
        show_toc(g_cur_file, TOC_COUNTS);

      } else if (nv = sscanf(g_cmd, "tt %c %c", &c1, &c2), (nv == 2)) {
        int v1, v2;
        /* triptych diff mode: changes from present world to worlds c1, c2*/
        p1 = char_to_fnum(c1, &v1);
        p2 = char_to_fnum(c2, &v2);
        if (v1 && v2) {
          if (g_cur_file == p1) {
            printf("%c is the current world, no diff.\n", c1);
          } else if (g_cur_file == p2) {
            printf("%c is the current world, no diff.\n", c2);
          } else {
            printf("triptych colour mode, world %c on left;"
                " diffs to %c in center, diffs to %c on right\n",
                fnum_to_char(g_cur_file),
                fnum_to_char(p1), fnum_to_char(p2) );
            g_triptych = 1;
            g_colormode = CM_DIFF;
            g_diffmode_fnum = p1;
            g_diffmode_f2 = p2;
            srs_dispatch(g_cur_file, 0);
          }
        } else {
          printf("At least one of '%c' and '%c' is not valid."
            " Use 'lw' to see list of worlds.\n", c1, c2);
        }
      } else if (strcmp(g_cmd, "tt") == 0) {
        /* Toggle triptych mode (if possible) */
        if (g_triptych) {
          printf("Returning to normal display mode\n");
          g_triptych = 0;
          g_colormode = CM_NORMAL;
          srs_dispatch(g_cur_file, 0);
        } else if ((g_diffmode_fnum >= 0) && (g_diffmode_f2 >= 0)
          && (g_diffmode_fnum != g_cur_file)
          && (g_diffmode_f2 != g_cur_file)) {
          printf("Returning to triptych display, world %c on left;"
              " diffs to %c in center, diffs to %c on right\n",
              fnum_to_char(g_cur_file),
              fnum_to_char(p1), fnum_to_char(p2) );
          printf("(To see all 3 worlds in the same colouring, use 'norm',"
                 " 'struct' or 'tun')\n");
          g_triptych = 1;
          g_colormode = CM_DIFF;
          srs_dispatch(g_cur_file, 0);
        } else {
          printf(
  "Cannot re-enable triptych mode because selected world(s) have changed.\n"
  "Use three-argument form 'tt B C' to re-enable triptych display.\n");
        }

      } else if (strcmp(g_cmd, "tun") == 0) {
        /* switch mode to colour-coded tunnels */
        g_fmg_mode = g_colormode = CM_TUNNEL;
        printf("mode set to colour-coded tunnels.\n");
        srs_dispatch(g_cur_file, 0);

      } else if (nv = sscanf(g_cmd, "u %d", &p1), (nv == 1)) {
        msl_mem(0, 0, p1);
        srs_dispatch(g_cur_file, 0);
      } else if (strcmp(g_cmd, "u") == 0) {
        msl_mem(0, 0, 1);
        srs_dispatch(g_cur_file, 0);

      } else if (nv = sscanf(g_cmd, "w %d", &p1), (nv == 1)) {
        p1 = minmax(p1, 0, 3000); move_srs(0, p1);
      } else if (strcmp(g_cmd, "w") == 0) {
        move_srs(0, 16);

      } else if (nv = sscanf(g_cmd, "wh %d %d", &p1, &p2),
                (nv == 2)) {
        /* Set width and height separately */
        p1 = minmax(p1, 4, 320);
        p2 = minmax(p2, 4, 320);
        g_cur_wid = p1; g_cur_hei = p2;
        set_limits();
        printf("Width and height set to (%d, %d)\n", g_cur_wid, g_cur_hei);
        srs_dispatch(g_cur_file, 0);
      } else if (nv = sscanf(g_cmd, "wh %d", &p1),
                (nv == 1)) {
        /* Set width and height for 'srs' and 'src' commands */
        p1 = minmax(p1, 4, 320);
        g_cur_wid = g_cur_hei = p1;
        set_limits();
        printf("Width and height set to %d\n", g_cur_wid);
        srs_dispatch(g_cur_file, 0);
      } else if (strcmp(g_cmd, "wh") == 0) {
        printf("Width and height are currently (%d, %d)\n",
          g_cur_wid, g_cur_hei);

      } else if (nv = sscanf(g_cmd, "world %c", &c1), (nv == 1)) {
        cmd_world(c1);

      } else {
        nv = V_E17_TRY_OPEN(g_cmd, 1);
        if (nv >= 0) {
          g_cur_file = nv;
          show_cur_world(06);
        } else {
          printf("Unknown command '%s', or invalid Eden file, or file not found.\n", g_cmd);
          printf("Enter ? for list of commands, or q to quit.\n");
        }
      }
      if (one_only) { return gg; }
    }
  }
  return 1;
} /* End of cmd.loop() */

int main(int argc, char **argv)
{
  validate_types();
  adler_init();

  init_marks();
  font_init();
  toc_init();
  hl_init();
  cch_init();

  blix_scan();
  cix_scan();
  V_CACHE_SETUP();

  argv++; argc--; /* Skip our program name */
  g_argv = argv; g_argc = argc;

  coords_init();

  cmdloop(0);

  return 0;
}
