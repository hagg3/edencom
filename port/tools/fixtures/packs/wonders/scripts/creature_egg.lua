-- creature_egg.lua: a placed Creature Egg wobbles for a few seconds, then hatches a random creature.
-- Shows: eden.spawn, and checking the block is still there after each wait (you can take the egg back).
-- Creatures only appear with Settings > Gameplay > Creatures on.

local CREATURES = {{"moof", "Moof"}, {"batty", "Batty"}, {"green", "Green"}, {"nergle", "Nergle"},
                   {"stumpy", "Stumpy"}}

W.when("build", "creature_egg", function(x, y, z)
  eden.say("The egg is wobbling...")
  for _ = 1, 4 do
    eden.wait(0.8)
    local name = eden.get(x, y, z)
    if name ~= nil and not W.is(name, "creature_egg") then return end   -- picked back up
    eden.sound(W.SOUND.pickup)
  end
  eden.set(x, y, z, "air")
  local c = CREATURES[math.random(#CREATURES)]
  eden.spawn(c[1], x, y, z)
  eden.sound(W.SOUND.goop)
  eden.say("It hatched: a " .. c[2] .. "!")
end)
