-- first_aid.lua: stand on a First Aid block to heal fully (needs Settings > Gameplay > Health on;
-- without it you never get hurt). Shows a second "tick" loop: each pack may have up to 32 per event.

eden.on("tick", function()
  local name, _, _, _, _, p = W.standing()
  if W.is(name, "first_aid") and not p.dead and p.health < 1 then
    eden.heal()
    eden.sound(W.SOUND.switch_on)
    if W.cooldown("healed", 5) then eden.say("Healed!") end
    eden.wait(2)
  end
  eden.wait(0.25)
end)
