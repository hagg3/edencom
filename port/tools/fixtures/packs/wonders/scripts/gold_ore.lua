-- gold_ore.lua: mining Gold Ore pays 1 to 3 golden cubes (the purse holds 999).
-- The smallest useful script: one event, one block, one reward.

W.when("mine", "gold_ore", function()
  local n = math.random(1, 3)
  eden.gold(n)
  eden.say("+" .. n .. (n == 1 and " golden cube" or " golden cubes"))
end)
