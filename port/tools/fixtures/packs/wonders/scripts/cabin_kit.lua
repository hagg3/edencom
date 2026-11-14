-- cabin_kit.lua: the Cabin Kit builds a 7x7 cabin where you place it, door facing you.
-- Shows: timed builds (eden.wait between steps, so you watch it go up), building with this pack's own
-- blocks and stock ones, and W.frame/W.box from lib.lua so one plan works facing any direction.

W.when("build", "cabin_kit", function(x, y, z)
  local fx, fz = W.awayFrom(x, z)
  local at = W.frame(x, y, z, fx, fz)   -- u: -3..3 across, h: up from the kit, v: 0..6 away from you
  local planks, log, glass = W.name("pine_planks"), W.name("pine_log"), W.name("framed_glass")
  eden.say("Building a cabin...")

  W.box(at, -3, 0, 0, 3, 5, 6, "air")      -- make room (this also removes the kit)
  W.box(at, -3, -1, 0, 3, -1, 6, planks)   -- floor

  for h = 0, 3 do                          -- walls, one layer at a time, logs on the corners
    W.box(at, -3, h, 0, 3, h, 0, planks)
    W.box(at, -3, h, 6, 3, h, 6, planks)
    W.box(at, -3, h, 0, -3, h, 6, planks)
    W.box(at, 3, h, 0, 3, h, 6, planks)
    for _, c in ipairs({{-3, 0}, {3, 0}, {-3, 6}, {3, 6}}) do
      W.box(at, c[1], h, c[2], c[1], h, c[2], log)
    end
    eden.sound(W.SOUND.build_wood)
    eden.wait(0.3)
  end

  W.box(at, 0, 0, 0, 0, 1, 0, "air")       -- the doorway
  W.box(at, -3, 1, 2, -3, 2, 4, glass)     -- windows: both sides and the back
  W.box(at, 3, 1, 2, 3, 2, 4, glass)
  W.box(at, -1, 1, 6, 1, 2, 6, glass)
  eden.sound(W.SOUND.build_glass)
  eden.wait(0.3)

  for k = 0, 3 do                          -- a stepped roof of red shingle, overhanging by one
    W.box(at, -4 + k, 4 + k, -1 + k, 4 - k, 4 + k, 7 - k, "eden:shingle", W.COLOUR.red)
    eden.sound(W.SOUND.build_stone)
    eden.wait(0.3)
  end

  W.box(at, 0, 3, 3, 0, 3, 3, "eden:lightbox")             -- a ceiling lamp
  W.box(at, -2, 0, 5, -1, 0, 5, log)                       -- a bench along the back wall
  W.box(at, 2, 0, 5, 2, 2, 5, W.name("vine_ladder"))       -- and a climbing vine in the corner
  eden.say("Your cabin is ready! (kits are used up)")
end)
