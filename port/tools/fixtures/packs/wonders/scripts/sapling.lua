-- sapling.lua: a planted Sapling grows into a pine tree after about 8 seconds, twice as fast in the rain.
-- Shows: a handler that keeps checking the world while it waits (mine the sapling and it stops), reading
-- the weather, the "weather" event, and growing only into air so it never eats your builds.

local GROW_SECONDS = 8
local growing = 0      -- saplings waiting to grow right now

-- Needles per layer from the tip down: 0 = one block, "+" = a plus shape, n = a square of radius n with
-- the corners cut off.
local CROWN = {0, "+", 1, 2, 1, 2, 2}

local function needlesLayer(x, y, z, r, needles)
  if r == 0 then
    W.setIfAir(x, y, z, needles)
    return
  end
  if r == "+" then
    for _, d in ipairs({{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}) do W.setIfAir(x + d[1], y, z + d[2], needles) end
    return
  end
  for dx = -r, r do
    for dz = -r, r do
      if not (math.abs(dx) == r and math.abs(dz) == r) then W.setIfAir(x + dx, y, z + dz, needles) end
    end
  end
end

local function growPine(x, y, z)
  local height = math.random(5, 8)
  local log, needles = W.name("pine_log"), W.name("pine_needles")
  eden.set(x, y, z, log)                       -- the sapling becomes the trunk's first block
  for h = 1, height - 1 do
    W.setIfAir(x, y + h, z, log)
    eden.wait(0.1)
  end
  for d = #CROWN, 1, -1 do                     -- the crown, bottom layer first
    local ly = y + height - d + 1
    if ly >= y + 2 then
      needlesLayer(x, ly, z, CROWN[d], needles)
      eden.wait(0.1)
    end
  end
  eden.sound(W.SOUND.build_leaves)
end

W.when("build", "sapling", function(x, y, z)
  growing = growing + 1
  eden.say(W.isRaining() and "Planted! The rain will help it grow" or "Planted! It grows faster in the rain")
  local grown = 0
  while grown < GROW_SECONDS do
    eden.wait(0.5)
    local name = eden.get(x, y, z)
    if name ~= nil and not W.is(name, "sapling") then   -- mined or burned (nil = out of range: keep waiting)
      growing = growing - 1
      return
    end
    grown = grown + (W.isRaining() and 1 or 0.5)
  end
  growing = growing - 1
  growPine(x, y, z)
end)

-- The weather changed (on its own, or by a Rain Stone).
eden.on("weather", function(kind)
  if growing > 0 and (kind == "rain" or kind == "storm") and W.cooldown("sapling_rain", 30) then
    eden.say("Rain! Saplings grow twice as fast now")
  end
end)

-- The "burn" event: you set fire to something. Pine burns because its blocks have the "flammable" trait.
for _, id in ipairs({"pine_log", "pine_planks", "pine_needles", "sapling"}) do
  W.when("burn", id, function()
    if W.cooldown("pine_fire", 30) then eden.say("Pine burns! Build with stone bricks to be safe") end
  end)
end
