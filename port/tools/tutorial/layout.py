"""The tutorial world's stations, as code (UX.10-1). Read by port/tools/tutorial.py, which owns the builder `b`.

Coordinates are (u, h, v) relative to the site origin: u = engine x - ORIGIN_X, v = engine z - ORIGIN_Z, h = height
above the surveyed ground (h = 0 is the grass surface, h = 1 the first cell a player stands in). That is a command
block script's order (X HEIGHT Z), so a script's offsets are plain differences of these triples.

Site (survey 2026-11-15, `tutorial.py --survey`): the nearest dry, flat land to the map centre (65536, 65536) that
holds a 132 x 20 strip is the grass plateau at x 65488..65620, z 65464..65484, surface y = 30 (height s.d. 0.14,
range 2, centre ~64 blocks north-west of the map centre). The centre itself is the shore of the central lake. The
path runs +x (yaw 0) from the start pad; each station's sign stands on a post on the path's -z side and faces -x,
toward a player walking the path.
"""

ORIGIN_X = 65490
ORIGIN_Z = 65474
GROUND_Y = 30
SPAWN = (0, 1, 0)        # the start pad's centre cell, feet height
SPAWN_YAW = 0.0          # facing +x, down the path

# Block ids (Classes/Constants.h; never renumbered) and paint bytes (Hud::genColorTable: byte k = swatch k-1;
# 9 = white, 21 = yellow, 25 = purple, 45 = charcoal, 19..27 the full-saturation row).
AIR, STONE, DIRT, SAND, LEAVES, TREE, WOOD, GRASS = 0, 2, 3, 4, 5, 6, 7, 8
BRICK, COBBLE, ICE, CRYSTAL, WATER, LAVA, SHINGLE, GLASS = 13, 14, 15, 16, 20, 23, 56, 58
DOOR4, DOOR_TOP, GOLDEN_CUBE, STEEL, PORTAL4, PORTAL_TOP = 69, 70, 71, 74, 78, 79
GRASS_PAINT, DIRT_PAINT = 49, 47   # what the bundled map paints its grass and dirt here
WHITE, YELLOW, PURPLE = 9, 21, 25

# Sound numbers (Classes/Resources.h): S_SWOOSH 28, S_SWITCH_TOGGLE_ON 62, S_SWITCH_TOGGLE_OFF 63.
SWOOSH, SWITCH_ON, SWITCH_OFF = 28, 62, 63

PATH_END = 118           # the cobblestone path runs u -2 .. PATH_END on v -1 .. 1; the finish plot is u 119..130


def rel(frm, to):
    """A script's X HEIGHT Z from the block at `frm` to the cell `to`."""
    return '%d %d %d' % (to[0] - frm[0], to[1] - frm[1], to[2] - frm[2])


def station_sign(b, u, key, v=-3):
    """A two-block wooden post on the path's -z side with the station's sign on its -x face, at eye height."""
    b.box(u, 1, v, u, 2, v, WOOD)
    b.sign(u, 2, v, '-x', key)


def build(b):
    # ---- the ground: level the corridor, clear the trees around it, lay the path
    b.clear_trees(-14, -16, 142, 16)
    b.level(-2, -9, 130, 9)        # the plateau's flat top is x 65488..65620; past it the land falls away
    b.box(-2, 0, -1, PATH_END, 0, 1, COBBLE)

    # 1. Start pad
    b.box(-2, 0, -2, 2, 0, 2, COBBLE)
    station_sign(b, 4, 'sign.start', v=-2)   # in view from the spawn

    # 2. Walk and jump: three steps up, a one-block gap, three steps down (open at the sides: no trap)
    station_sign(b, 7, 'sign.walk')
    for u, top in ((8, 1), (9, 2), (10, 3), (12, 3), (13, 2), (14, 1)):
        b.box(u, 1, -1, u, top, 1, WOOD)

    # 3. Break and build: a wall across the path, then a trench to bridge (stairs at its +z end climb out)
    station_sign(b, 17, 'sign.build')
    b.box(19, 1, -4, 19, 3, 4, BRICK)
    b.box(22, -2, -4, 23, 0, 4, AIR)
    b.box(22, -2, 3, 23, -2, 4, DIRT, DIRT_PAINT)
    b.box(22, -1, 4, 23, -1, 4, DIRT, DIRT_PAINT)

    # 4. Blocks and colour: a shelf of block types, a white wall to paint
    station_sign(b, 27, 'sign.blocks')
    for i, t in enumerate((STONE, BRICK, WOOD, COBBLE, ICE, CRYSTAL, SHINGLE, GLASS, SAND)):
        b.block(28 + i, 1, 3, t)
    b.box(30, 1, -4, 34, 3, -4, STONE, WHITE)

    # 5. Hotbar: nine pedestals, one colour each
    station_sign(b, 39, 'sign.hotbar')
    for i in range(9):
        b.block(40 + i, 1, 3, COBBLE)
        b.block(40 + i, 2, 3, STONE, 19 + i)

    # 6. Signs: the station's own sign is the one to read; a bare post to put one on
    station_sign(b, 51, 'sign.signs')
    b.box(54, 1, 3, 54, 2, 3, WOOD)

    # 7. Command blocks
    station_sign(b, 57, 'sign.cmd_intro')
    # (a) a @step plate in the path lifts the player onto a lookout; a plate up there brings them back
    plate_a, land_up = (59, 0, 0), (60, 8, 4)
    plate_b, land_down = (62, 7, 6), (65, 1, 0)
    b.box(61, 1, 5, 61, 6, 5, COBBLE)                      # the lookout's pillar
    b.box(60, 7, 4, 62, 7, 6, COBBLE)                      # its 3 x 3 floor
    b.ring(59, 8, 3, 63, 7, GLASS)                         # a glass rail around it
    b.cmd(*plate_a, script='@step;say %s;sound %d;tp %s' % (b.say('say.lookout_up'), SWOOSH, rel(plate_a, land_up)))
    b.cmd(*plate_b, script='@step;say %s;sound %d;tp %s' % (b.say('say.lookout_down'), SWOOSH, rel(plate_b, land_down)))
    # (b) a @touch block lights a row of lamps for a few seconds
    station_sign(b, 66, 'sign.cmd_touch')
    touch = (67, 1, -3)
    b.box(65, 1, 3, 69, 1, 3, COBBLE)
    b.box(65, 2, 3, 69, 2, 3, GLASS)
    lamps = '%s %s' % (rel(touch, (65, 2, 3)), rel(touch, (69, 2, 3)))
    b.cmd(*touch, script='@touch;fill %s lamp %d;sound %d;wait 5;fill %s glass;sound %d' % (
        lamps, YELLOW, SWITCH_ON, lamps, SWITCH_OFF))
    # (c) a @near block talks; (d) the sign beside it says to open it
    station_sign(b, 71, 'sign.cmd_read')
    b.cmd(72, 1, -3, script='@near 4;say %s' % b.say('say.near'))

    # 8. Doors and portals: a door in a wall across the path; a portal pair, ground to a raised platform
    station_sign(b, 75, 'sign.doors')
    b.box(77, 1, -3, 77, 3, 3, BRICK)
    b.block(77, 1, 0, DOOR4)                               # DOOR4 = placed facing +x (Terrain::buildBlock)
    b.block(77, 2, 0, DOOR_TOP)
    # PORTAL4's mouth faces -x and its exit is 1.2 blocks to -x (Portal::enterPortal); same colour = linked
    b.block(81, 1, 3, PORTAL4, PURPLE)
    b.block(81, 2, 3, PORTAL_TOP, PURPLE)
    b.box(83, 1, 2, 86, 3, 4, BRICK)
    b.block(86, 4, 3, PORTAL4, PURPLE)
    b.block(86, 5, 3, PORTAL_TOP, PURPLE)

    # 9. Water, lava, fire
    station_sign(b, 89, 'sign.liquids')
    b.box(91, -1, 2, 94, 0, 5, WATER)
    b.box(98, 0, 3, 99, 0, 4, LAVA)
    b.ring(97, 1, 2, 100, 5, GLASS)
    b.ring(97, 2, 2, 100, 5, GLASS)
    station_sign(b, 102, 'sign.fire')
    fire_block, campfire = (103, 1, -3), (105, 1, 3)
    b.box(104, 0, 2, 106, 0, 4, COBBLE)                   # nothing that burns next to the campfire
    b.block(*campfire, t=WOOD)
    b.cmd(*fire_block, script='@touch;set %s wood;fire %s' % (rel(fire_block, campfire), rel(fire_block, campfire)))

    # 10. Fly: a tall pillar with a golden cube on its cap
    station_sign(b, 109, 'sign.fly')
    b.box(112, 1, 3, 112, 19, 3, BRICK)
    b.box(111, 20, 2, 113, 20, 4, BRICK)
    b.block(112, 21, 3, GOLDEN_CUBE)

    # 11. Finish: an open plot, outlined
    station_sign(b, 117, 'sign.finish')
    b.ring(119, 0, -7, 130, 7, COBBLE)
