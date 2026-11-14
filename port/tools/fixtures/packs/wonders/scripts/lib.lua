-- lib.lua: helpers shared by every script in this pack.
--
-- A pack's scripts run in ONE sandbox, in the order pack.json lists them, so the global table W made here is
-- seen by every later file (there is no `require`). Each other file is one feature: copy a file and its
-- block into your own pack to reuse it. The API is documented in docs/scripting.md.
--
-- Coordinates are the engine's (x, y, z) with y = height, absolute, like the console's =N.

W = {}

-- Paint colours are the paint byte, 0 = unpainted .. 54, the paint grid read row by row, nine to a row.
-- Row 3 is the bright rainbow.
W.COLOUR = {red = 19, orange = 20, yellow = 21, green = 22, cyan = 23, blue = 24, purple = 25, pink = 26,
            white = 9, black = 54}
W.RAINBOW = {19, 20, 21, 22, 23, 24, 25}

-- Sound effect numbers for eden.sound (the SOUND_TYPES list in Classes/Resources.h).
W.SOUND = {bounce = 2, paint = 5, build_wood = 9, build_stone = 11, build_leaves = 12, build_glass = 14,
           swoosh = 28, pickup = 31, treasure = 42, treasure_place = 43, portal = 50, goop = 51,
           day = 55, night = 56, sky = 57, warp_set = 59, switch_on = 62}

W.AIR = "eden:none"

-- "sapling" -> "wonders:sapling": eden.pack is this pack's id, so the files survive a rename of the pack.
function W.name(id) return eden.pack .. ":" .. id end
function W.is(block, id) return block == eden.pack .. ":" .. id end

-- W.when(event, block_id, fn): call fn only for events on one of this pack's blocks.
--   build, mine, burn:  fn(x, y, z, block)
--   paint:              fn(x, y, z, colour, block)
-- It registers ONE eden.on handler per event however many blocks use it, and looks the block up in a
-- table, which is cheaper than every feature checking every event. Each event still runs in its own
-- coroutine, so fn may call eden.wait without holding anything else up.
local routes = {}
function W.when(event, id, fn)
  if not routes[event] then
    local byBlock = {}
    routes[event] = byBlock
    eden.on(event, function(x, y, z, a, b)
      local block = (event == "paint") and b or a
      local fns = byBlock[block]
      if fns then
        for _, f in ipairs(fns) do f(x, y, z, a, b) end
      end
    end)
  end
  local full = W.name(id)
  routes[event][full] = routes[event][full] or {}
  table.insert(routes[event][full], fn)
end

-- True at most once every `secs` seconds of game time for a key: keeps messages from spamming.
local last = {}
function W.cooldown(key, secs)
  local now = eden.clock()
  if last[key] and now - last[key] < secs then return false end
  last[key] = now
  return true
end

-- The block the player stands on: name, colour, x, y, z, and the eden.player() table.
-- name is nil when that cell is not loaded.
function W.standing()
  local p = eden.player()
  local x, y, z = math.floor(p.x), p.cell_y, math.floor(p.z)
  local name, colour = eden.get(x, y, z)
  return name, colour, x, y, z, p
end

-- The axis direction (fx, fz) that points from the player towards (x, z): kits build away from you.
function W.awayFrom(x, z)
  local p = eden.player()
  local dx, dz = x + 0.5 - p.x, z + 0.5 - p.z
  if math.abs(dx) >= math.abs(dz) then
    return (dx >= 0) and 1 or -1, 0
  end
  return 0, (dz >= 0) and 1 or -1
end

-- A local grid at (x, y, z) facing (fx, fz): at(u, h, v) -> world x, y, z, where u is across (left/right),
-- h is up and v is forward. Lets a build be written once and placed facing any of the four directions.
function W.frame(x, y, z, fx, fz)
  return function(u, h, v)
    return x + v * fx - u * fz, y + h, z + v * fz + u * fx
  end
end

-- Fills the local box (u1, h1, v1)..(u2, h2, v2) of a frame. One eden.fill: at most 4096 cells.
function W.box(at, u1, h1, v1, u2, h2, v2, block, colour)
  local ax, ay, az = at(u1, h1, v1)
  local bx, by, bz = at(u2, h2, v2)
  return eden.fill(math.min(ax, bx), math.min(ay, by), math.min(az, bz),
                   math.max(ax, bx), math.max(ay, by), math.max(az, bz), block, colour)
end

-- Places a block only where there is air, so growing things never eat your builds.
function W.setIfAir(x, y, z, block, colour)
  if eden.get(x, y, z) == W.AIR then return eden.set(x, y, z, block, colour) end
  return false
end

-- Air, water and lava count as a gap; anything else is ground.
function W.isGap(name)
  return name == W.AIR or name:match("^eden:water") ~= nil or name:match("^eden:lava") ~= nil
end

-- The weather's kind ("clear", "overcast", "rain", "storm", "snow").
function W.weather()
  return (eden.weather())
end
function W.isRaining()
  local k = W.weather()
  return k == "rain" or k == "storm"
end

-- Starts a weather spell; false when the player has weather switched off (Settings > Gameplay > Weather).
-- pcall catches the error eden.weather raises then, instead of stopping the handler.
function W.setWeather(kind, secs)
  return (pcall(eden.weather, kind, secs))
end
