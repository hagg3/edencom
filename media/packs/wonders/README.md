# Wonder Blocks: an example Emod mod pack

Wonder Blocks adds 23 blocks to Emod. Thirteen of them do something when you place, mine, paint or stand on
them. Everything works from the block picker, so you never need the console.

## Install it

Emod installs Wonder Blocks for you the first time it starts, so it's already in the Mods list. You can switch
it off or remove it on the Mods screen; once removed, it stays removed. To install it again, or on another device:

1. Copy this `wonders` folder into Emod's `Packs` folder:
   - **macOS:** `~/Library/Application Support/Emod/Packs/`
   - **iPhone / iPad:** Files › On My iPhone (or iPad) › Emod › `Packs`
   - **Windows / Linux:** the `Packs` folder inside Emod's data folder

   Or zip the folder and install the zip from the **Mods** screen.
2. Open a world. Packs load each time a world opens, so you don't need to restart.
3. Open the block picker and swipe past the two stock pages to the **Mods** page. The first time you place a
   mod block in a world, Emod asks you to confirm it.

Start with the blue **i** block, the Guide Stone. Place it for a tour of the pack, and mine it to stop the tour.

## The wonder blocks

| Block | Looks like | What it does |
|---|---|---|
| Guide Stone | blue circle with an **i** | Place it for a tour of the pack. Mine it to stop. |
| Cabin Kit | crate with a house | Builds a cabin where you place it, with the door facing you. It has windows, a red roof, a lamp, a bench and a climbing vine. |
| Bridge Kit | crate with a bridge | Place it at the edge of a drop or water. It lays a bridge away from you until it reaches land, with posts and lamps along the way. |
| Sapling | small green sprout | Grows into a pine tree after about 8 seconds, or twice as fast in rain. Mine it first if you change your mind. |
| Warp Pad | grey pad with a white swirl | Step on it to jump to another Warp Pad of the same **paint colour**. Two pads make a pair. Three or more make a loop. Paint pads different colours for separate networks. |
| Lucky Block | yellow block with **?** | Mine it to get one of eight surprises. |
| Sun Stone | sun on stone | Changes the time to midday and clears the weather, then crumbles. |
| Moon Stone | moon on dark stone | Changes the time to midnight, then crumbles. |
| Rain Stone | cloud on stone | Moves the weather on one step: clear → rain → storm → snow → clear. Needs Settings › Gameplay › **Weather** on. |
| Gold Ore | stone with gold flecks | Mine it for 1 to 3 golden cubes. |
| Creature Egg | spotted egg | Wobbles for a few seconds, then hatches a creature. Needs Settings › Gameplay › **Creatures** on. |
| First Aid | white block with a red cross | Stand on it to heal fully. You can only get hurt with Settings › Gameplay › **Health** on. |
| Bounce Pad | green pad | Bouncy. Paint one, and the colour ripples through every pad touching it. |

Kits and sky stones are used up when they work. If one can't work, it stays where you put it and a message
tells you why. For example, a Bridge Kit with no gap in front of it, or a Rain Stone while weather is off.

**Building blocks** (no script, just data): Pine Log, Pine Planks, Pine Needles, Paintable Brick, Sandstone
Bricks, Stone Bricks, Obsidian Bricks (blast-proof and unbreakable), Slick Ice (slippery), Framed Glass and Vine
Ladder (climbable).

**Without the pack:** remove it and its blocks show as the magenta "missing" block. Your world keeps them,
so putting the pack back restores them. A world exported to `.eden` gets each block's stock `fallback` instead.

## For mod makers: what's in this folder

```
wonders/
  pack.json          the blocks, their textures and traits, and the list of scripts
  textures/*.png     26 tiles, 32x32 PNG
  scripts/lib.lua    shared helpers (loaded first)
  scripts/*.lua      one file per feature
  CREDITS.txt        where the art comes from
```

Each feature file is short and commented, and covers one part of Emod's modding:

| File | What it teaches |
|---|---|
| `pack.json` | Tiles on all faces, sides/top/bottom, a paintable tile with its unpainted twin, tinting with `color`, reusing stock tiles at no cost, transparent and walk-through blocks, and every trait. |
| `lib.lua` | Sharing code between files through a global table. `W.when(event, block, fn)` routes events to features by block. Cooldowns, the block under the player, and a facing-aware build grid (`W.frame`/`W.box`). |
| `welcome.lua` | The `load` event and `eden.say`. |
| `guide.lua` | Long-running handlers with `eden.wait`, cancelling them, and an optional console command (`mod wonders`). |
| `cabin_kit.lua` | Timed builds with `eden.fill`, using the pack's own blocks and stock ones. |
| `bridge_kit.lua` | Reading the world with `eden.get` before changing it, and handling the edge of the loaded area. |
| `sapling.lua` | Waiting while watching the world, the `weather` and `burn` events, and growing only into air. |
| `warp_pads.lua` | A `tick` loop, `eden.tp`, the `paint` event, and rebuilding state after a reload with a background scan. |
| `lucky_block.lua` | The `mine` event and a random table of functions that use most of the API. |
| `sky_stones.lua` | `eden.time` and `eden.weather`, and telling the player which setting to turn on. |
| `gold_ore.lua` | The smallest useful script. |
| `creature_egg.lua` | `eden.spawn`, and checking that a block is still there after a wait. |
| `first_aid.lua` | A second `tick` loop, `eden.player()` and `eden.heal`. |
| `bounce_pad.lua` | A flood fill over `eden.get`, with a cap. |

**Make your own:** copy this folder, rename it, change `"id"` in `pack.json` to the new folder name, then delete
what you don't need. Scripts build block names from `eden.pack`, so they keep working after the rename. Check
your pack with `python3 port/tools/pack.py validate <folder>`, or with the console's `packs` command in the game.

The full reference is in the Emod repository: `docs/content-packs.md` (pack.json) and `docs/scripting.md` (the
Lua sandbox, its budgets, every `eden.*` call and event).

**Things scripts can't do, by design:** keep data after the world closes, read or write files, use the network,
or run in multiplayer worlds. Edits made by scripts don't fire events, so a script can't trigger itself in a loop.
