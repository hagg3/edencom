-- guide.lua: the Guide Stone. Place it and it walks you through the pack, one message at a time;
-- mine it to stop. Also registers the console command `mod wonders` (optional: nothing needs the console).

W.GUIDE = {
  "Welcome to Wonder Blocks! (mine this stone to stop)",
  "Every Wonder Block is on the Mods page of the picker",
  "Cabin Kit (crate with a house): builds a cabin",
  "Bridge Kit (crate with a bridge): spans a gap",
  "Sapling: grows into a pine tree, faster in rain",
  "Warp Pad (swirl): step on it to jump to another",
  "Pads of the same paint colour are linked",
  "Lucky Block (?): mine it for a surprise",
  "Sun Stone: midday. Moon Stone: midnight",
  "Rain Stone: rain, then storm, then snow, then sun",
  "Gold Ore: mine it for golden cubes",
  "Creature Egg: place it and wait for it to hatch",
  "First Aid (red cross): stand on it to heal",
  "Bounce Pad: paint one to colour every pad it touches",
  "The rest are building blocks. Have fun!",
}

-- Placing a second stone restarts the tour: each tour remembers its number and stops when a newer one
-- (or mining the stone) changes `tour`.
local tour = 0

W.when("build", "guide_stone", function(x, y, z)
  tour = tour + 1
  local mine = tour
  eden.sound(W.SOUND.warp_set)
  for _, line in ipairs(W.GUIDE) do
    if tour ~= mine then return end
    eden.say(line)
    eden.wait(3.5)
  end
end)

W.when("mine", "guide_stone", function()
  tour = tour + 1
  eden.say("Tour stopped. Place the stone again to restart it")
end)

-- `mod wonders` in the console (Settings > Experiments > Developer console) prints the same list.
eden.command("wonders", "what each Wonder Block does", function()
  for _, line in ipairs(W.GUIDE) do print(line) end
end)
