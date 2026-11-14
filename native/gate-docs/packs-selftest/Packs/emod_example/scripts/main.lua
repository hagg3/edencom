-- emod_example/scripts/main.lua: what a mod script looks like (docs/scripting.md).
-- It runs in its own sandbox when a world opens. Coordinates are x, height, z, like the console's =N.

-- `mod pillar 5` (console): a jelly pillar two blocks east of you, one block every tenth of a second.
eden.command("pillar", "HEIGHT: a jelly pillar two blocks east of you", function(h)
  h = math.max(1, math.min(tonumber(h) or 5, 32))
  local p = eden.player()
  local x, y, z = math.floor(p.x) + 2, p.cell_y + 1, math.floor(p.z)
  for i = 0, h - 1 do
    eden.set(x, y + i, z, "emod_example:jelly")   -- the same refusals as the `set` verb
    eden.wait(0.1)                                 -- pauses this handler only; the game carries on
  end
end)

-- Every tenth vault stone the player places gets a message.
local placed = 0
eden.on("build", function(x, y, z, block)
  if block == "emod_example:vault_stone" then
    placed = placed + 1
    if placed % 10 == 0 then eden.say(placed .. " vault stones placed") end
  end
end)
