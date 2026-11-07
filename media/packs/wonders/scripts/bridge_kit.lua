-- bridge_kit.lua: place the Bridge Kit at the edge of a drop or water and it lays a 3-wide bridge away
-- from you until it reaches land on the other side (at most 48 blocks).
-- Shows: reading the world with eden.get to decide what to build, and stopping at the loaded area's edge
-- (eden.get answers nothing there).

local MAX = 48

W.when("build", "bridge_kit", function(x, y, z)
  local fx, fz = W.awayFrom(x, z)
  local at = W.frame(x, y, z, fx, fz)   -- the deck is at h = -1, level with the ground the kit sits on
  local planks, log = W.name("pine_planks"), W.name("pine_log")

  -- Survey first, build second: find where the gap starts and where the far bank is.
  local gapAt, last = nil, nil
  for v = 0, MAX do
    local deck, below = eden.get(at(0, -1, v)), eden.get(at(0, -2, v))
    if deck == nil or below == nil then break end  -- the end of the loaded world
    if W.isGap(deck) and W.isGap(below) then       -- nothing for the deck to rest on
      gapAt = gapAt or v
    elseif gapAt then
      break                                        -- solid ground again: the far bank
    elseif v >= 3 then
      break                                        -- no gap right ahead: nothing to bridge
    end
    last = v
  end
  if not gapAt then
    eden.say("Place the Bridge Kit at the edge of a drop or water")
    return                                         -- the kit stays where it is
  end
  last = math.min(last + 1, MAX)                   -- one plank onto the far bank

  eden.set(x, y, z, "air")
  for v = 0, last do
    W.box(at, -1, -1, v, 1, -1, v, planks)
    if v % 4 == 0 or v == last then                -- posts beside the deck, a lamp on every other one
      W.box(at, -2, -1, v, -2, 0, v, log)
      W.box(at, 2, -1, v, 2, 0, v, log)
      if v % 8 == 0 then
        W.box(at, -2, 1, v, -2, 1, v, "eden:lightbox")
        W.box(at, 2, 1, v, 2, 1, v, "eden:lightbox")
      end
    end
    eden.sound(W.SOUND.build_wood)
    eden.wait(0.08)
  end
  eden.say("Bridge built: " .. (last + 1) .. " blocks long")
end)
