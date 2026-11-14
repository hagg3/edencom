-- warp_pads.lua: step onto a Warp Pad and you are carried to the next pad of the same paint colour.
-- Two pads make a pair; three or more make a loop. Unpainted pads link to unpainted pads.
-- Shows: a "tick" loop watching what the player stands on, eden.tp, the paint event, and the one thing a
-- script can't do, keep data after the world closes: pads are re-found by a slow scan around the player.

local PAD = W.name("warp_pad")
local SCAN_RADIUS, SCAN_HEIGHT, SCAN_EVERY = 32, 12, 20

local pads = {}        -- "x,y,z" -> {x, y, z}: every pad this script knows about
local onPad = nil      -- the pad under the player's feet, so a warp needs stepping off and on again

local function key(x, y, z) return x .. "," .. y .. "," .. z end
local function learn(x, y, z) pads[key(x, y, z)] = {x = x, y = y, z = z} end

-- Every known pad of `colour`, in a fixed order, so pads of one colour always form the same loop.
-- Pads that turn out to be gone are forgotten; pads too far away to check are skipped for now.
local function padsOfColour(colour)
  local list = {}
  for k, p in pairs(pads) do
    local name, c = eden.get(p.x, p.y, p.z)
    if name ~= nil and name ~= PAD then
      pads[k] = nil
    elseif name == PAD and c == colour then
      list[#list + 1] = p
    end
  end
  table.sort(list, function(a, b)
    if a.x ~= b.x then return a.x < b.x end
    if a.y ~= b.y then return a.y < b.y end
    return a.z < b.z
  end)
  return list
end

-- The pad after (x, y, z) in its colour's loop, or nil when it has no partner.
local function nextPad(x, y, z, colour)
  local list = padsOfColour(colour)
  for i, p in ipairs(list) do
    if p.x == x and p.y == y and p.z == z then
      local n = list[i % #list + 1]
      if n ~= p then return n end
    end
  end
  return nil
end

-- What to tell the player about a pad with no partner.
local function lonelyText(x, y, z)
  local me = key(x, y, z)
  for k in pairs(pads) do
    if k ~= me then return "No other pad of this colour: paint one to match" end
  end
  return "Place a second Warp Pad to link them"
end

local function warp(x, y, z, colour)
  local dest = nextPad(x, y, z, colour)
  if not dest then
    if W.cooldown("lonely_pad", 4) then eden.say(lonelyText(x, y, z)) end
    return
  end
  if eden.get(dest.x, dest.y + 1, dest.z) ~= W.AIR or eden.get(dest.x, dest.y + 2, dest.z) ~= W.AIR then
    if W.cooldown("blocked_pad", 4) then eden.say("The other pad is covered up") end
    return
  end
  eden.sound(W.SOUND.portal)
  eden.wait(0.4)                                   -- a moment to step off if you didn't mean it
  local name, _, x2, y2, z2 = W.standing()
  if x2 ~= x or y2 ~= y or z2 ~= z then return end
  eden.flash()
  if eden.tp(dest.x, dest.y + 1, dest.z) then
    onPad = key(dest.x, dest.y, dest.z)            -- arriving on a pad must not warp you straight back
  end
end

eden.on("tick", function()
  local name, colour, x, y, z, p = W.standing()
  if name == PAD and not p.dead then
    local k = key(x, y, z)
    if k ~= onPad then
      onPad = k
      learn(x, y, z)
      warp(x, y, z, colour)
    end
  else
    onPad = nil
  end
  eden.wait(0.15)    -- a tick handler that is waiting skips frames: this checks about 6 times a second
end)

W.when("build", "warp_pad", function(x, y, z)
  learn(x, y, z)
  local _, colour = eden.get(x, y, z)
  eden.say(nextPad(x, y, z, colour) and "Warp Pad linked! Step on one to warp" or lonelyText(x, y, z))
end)

W.when("paint", "warp_pad", function(x, y, z, colour)
  learn(x, y, z)
  eden.say(nextPad(x, y, z, colour) and "Linked to the pads of this colour" or "Paint another pad this colour to link it")
end)

W.when("mine", "warp_pad", function(x, y, z)
  pads[key(x, y, z)] = nil
end)

-- Scripts keep nothing when the world closes, so after reopening a world the pads are found again by
-- looking at the blocks around the player: one slice a frame, every SCAN_EVERY seconds.
local function scan()
  local p = eden.player()
  local cx, cy, cz = math.floor(p.x), p.cell_y, math.floor(p.z)
  for x = cx - SCAN_RADIUS, cx + SCAN_RADIUS do
    for z = cz - SCAN_RADIUS, cz + SCAN_RADIUS do
      for y = math.max(0, cy - SCAN_HEIGHT), cy + SCAN_HEIGHT do
        if eden.get(x, y, z) == PAD then learn(x, y, z) end
      end
    end
    eden.wait(0)     -- next slice next frame, so the scan never slows the game
  end
end

eden.on("load", function()
  while true do
    scan()
    eden.wait(SCAN_EVERY)
  end
end)
