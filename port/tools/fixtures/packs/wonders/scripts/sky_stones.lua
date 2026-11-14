-- sky_stones.lua: three stones that change the sky when placed, then crumble away.
--   Sun Stone:  midday, clear skies.        Moon Stone: midnight.
--   Rain Stone: the next weather in the cycle clear -> rain -> storm -> snow -> clear.
-- Shows: eden.time (0..1, 0.25..0.75 is day), eden.weather, and telling the player what to switch on
-- when a feature is off instead of failing silently.

local SPELL = 120   -- seconds a Rain Stone's weather lasts

local function crumble(x, y, z)
  eden.wait(1)
  eden.set(x, y, z, "air")
  eden.sound(W.SOUND.swoosh)
end

W.when("build", "sun_stone", function(x, y, z)
  eden.flash()
  eden.time(0.5)
  W.setWeather("clear", SPELL)                       -- fine if weather is off: it is clear then anyway
  eden.sound(W.SOUND.day)
  eden.say("The sun shines at midday")
  crumble(x, y, z)
end)

W.when("build", "moon_stone", function(x, y, z)
  eden.time(0)
  eden.sound(W.SOUND.night)
  eden.say("Midnight. Light some lamps!")
  crumble(x, y, z)
end)

local NEXT = {clear = "rain", overcast = "rain", rain = "storm", storm = "snow", snow = "clear"}
local SAY = {rain = "Rain is coming", storm = "A storm rolls in!", snow = "Snow begins to fall",
             clear = "The skies clear"}

W.when("build", "rain_stone", function(x, y, z)
  local kind = NEXT[W.weather()] or "rain"
  if not W.setWeather(kind, SPELL) then
    eden.say("Turn on Settings > Gameplay > Weather first")
    return                                           -- the stone stays: try again after switching it on
  end
  eden.sound(W.SOUND.sky)
  eden.say(SAY[kind])
  crumble(x, y, z)
end)
