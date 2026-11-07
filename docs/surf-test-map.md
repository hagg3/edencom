# Surf test map (Srclike movement)

A two-ramp valley for testing surfing (Experiments › *Srclike movement* on). Two 8-block ramp staircases face each
other across a 6-block gap, 61 blocks long, with a flat launch platform at the top of each side. Surf on a ramp's
slope: build speed on the platform, step onto the slope (or jump off), hold the strafe key *into* the ramp and turn
the mouse the same way; a fast player stays on the slope instead of landing (the U2 rule: fast onto a ramp = no
ground), so you slide, climb with the strafe, and can cross the gap from ramp to ramp.

## Build it

1. New world → **Flat**. Stand on open flat ground with ~40 blocks free in +X and ~65 in +Z; the map is built
   relative to the block under your feet (the console anchor). Turn on Experiments › *Dev console* (`experiments.dev_console`) for the console button.
2. Open the Console and paste the lines below one at a time (each is one command; the longest fills 61 blocks).
3. The last line teleports you onto the west launch platform. Turn on Experiments › *Srclike movement*.

```
say building surf test map
fill -12 1 -2 -9 8 62 stone
fill 14 1 -2 17 8 62 stone
fill -8 1 0 -8 7 60 stone
fill -8 8 0 -8 8 60 eden:stone_ramp2
fill -7 1 0 -7 6 60 stone
fill -7 7 0 -7 7 60 eden:stone_ramp2
fill -6 1 0 -6 5 60 stone
fill -6 6 0 -6 6 60 eden:stone_ramp2
fill -5 1 0 -5 4 60 stone
fill -5 5 0 -5 5 60 eden:stone_ramp2
fill -4 1 0 -4 3 60 stone
fill -4 4 0 -4 4 60 eden:stone_ramp2
fill -3 1 0 -3 2 60 stone
fill -3 3 0 -3 3 60 eden:stone_ramp2
fill -2 1 0 -2 1 60 stone
fill -2 2 0 -2 2 60 eden:stone_ramp2
fill -1 1 0 -1 1 60 eden:stone_ramp2
fill 6 1 0 6 1 60 eden:stone_ramp4
fill 7 1 0 7 1 60 stone
fill 7 2 0 7 2 60 eden:stone_ramp4
fill 8 1 0 8 2 60 stone
fill 8 3 0 8 3 60 eden:stone_ramp4
fill 9 1 0 9 3 60 stone
fill 9 4 0 9 4 60 eden:stone_ramp4
fill 10 1 0 10 4 60 stone
fill 10 5 0 10 5 60 eden:stone_ramp4
fill 11 1 0 11 5 60 stone
fill 11 6 0 11 6 60 eden:stone_ramp4
fill 12 1 0 12 6 60 stone
fill 12 7 0 12 7 60 eden:stone_ramp4
fill 13 1 0 13 7 60 stone
fill 13 8 0 13 8 60 eden:stone_ramp4
tp -10 9 5
```

Layout (anchor-relative `X height Z`): west staircase `x -8..-1` (`stone_ramp2`, high side at -X), gap `x 0..5`,
east staircase `x 6..13` (`stone_ramp4`, high side at +X), platforms `x -12..-9` and `x 14..17`, all `z 0..60`
(platforms `-2..62`). To test the other slope directions, swap in `stone_ramp1` (high at +Z) / `stone_ramp3` (high at
+X) rows; `ice_ramp*` gives an ice surf. Tuning while you surf: Experiments › *Srclike settings*, or `move.airaccelerate`.

Untested by hand: the lines are generated from the verified ramp orientations in `--srclike-selftest=trace`, but
nobody has surfed this exact map yet.
