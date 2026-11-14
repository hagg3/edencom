-- lucky_block.lua: mine a Lucky Block for one of eight surprises.
-- Shows: the "mine" event (the block is already gone when it fires), a table of functions picked at
-- random, and a tour of the API: gold, sounds, painted builds, golden cubes, creatures, the sky, weather.

local LUCKY = W.name("lucky_block")

local SURPRISES = {
  function()
    eden.gold(5)                                       -- plays the treasure sound by itself
    eden.say("Jackpot! +5 golden cubes")
  end,
  function(x, y, z)
    eden.say("Treasure ring! Collect the golden cubes")
    for _, d in ipairs({{2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 2}, {2, -2}, {-2, 2}, {-2, -2}}) do
      W.setIfAir(x + d[1], y, z + d[2], "eden:golden_cube")
      eden.sound(W.SOUND.treasure_place)
      eden.wait(0.15)
    end
  end,
  function(x, y, z)
    eden.say("A rainbow tower!")
    for i, colour in ipairs(W.RAINBOW) do
      W.setIfAir(x, y + i - 1, z, W.name("paint_brick"), colour)
      eden.sound(W.SOUND.paint)
      eden.wait(0.2)
    end
  end,
  function(x, y, z)
    eden.sound(W.SOUND.goop)
    eden.spawn("moof", x, y, z)
    eden.say("A Moof jumped out!")
  end,
  function(x, y, z)
    eden.flash()
    eden.time(0)                                       -- midnight
    eden.sound(W.SOUND.night)
    eden.spawn("batty", x, y + 2, z)
    eden.say("Spooky! Night falls (a Sun Stone fixes it)")
  end,
  function(x, y, z)
    if W.setWeather("snow", 60) then
      eden.say("Let it snow!")
    else                                               -- weather is switched off: pay out instead
      eden.gold(3)
      eden.say("+3 golden cubes")
    end
  end,
  function(x, y, z)
    eden.say("Lucky again! Two more Lucky Blocks")
    eden.wait(0.3)
    W.setIfAir(x, y, z, LUCKY)
    W.setIfAir(x, y + 1, z, LUCKY)
    eden.sound(W.SOUND.switch_on)
  end,
  function(x, y, z)
    eden.say("Bouncy castle!")
    local pad = W.name("bounce_pad")
    for dx = -2, 2 do
      for dz = -2, 2 do
        if math.abs(dx) == 2 or math.abs(dz) == 2 then
          W.setIfAir(x + dx, y, z + dz, pad, W.RAINBOW[math.random(#W.RAINBOW)])
        end
      end
    end
    eden.sound(W.SOUND.bounce)
  end,
}

W.when("mine", "lucky_block", function(x, y, z)
  SURPRISES[math.random(#SURPRISES)](x, y, z)
end)
