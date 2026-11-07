-- bounce_pad.lua: Bounce Pads are bouncy by data alone (the "bouncy" trait in pack.json). The script adds
-- one trick: paint a pad and the colour ripples out through every pad touching it.
-- Shows: the "paint" event and a breadth-first flood fill over eden.get, capped so it can't run away.

local LIMIT = 400

local SIDES = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}

W.when("paint", "bounce_pad", function(x, y, z, colour, block)
  local seen = {[x .. "," .. y .. "," .. z] = true}
  local ring = {{x, y, z}}
  local done = 0
  while #ring > 0 and done < LIMIT do          -- one ring of neighbours per step: a visible ripple
    local nextRing = {}
    for _, c in ipairs(ring) do
      for _, d in ipairs(SIDES) do
        local a, b, e = c[1] + d[1], c[2] + d[2], c[3] + d[3]
        local k = a .. "," .. b .. "," .. e
        if not seen[k] then
          seen[k] = true
          if eden.get(a, b, e) == block then
            eden.paint(a, b, e, colour)
            nextRing[#nextRing + 1] = {a, b, e}
            done = done + 1
          end
        end
      end
    end
    ring = nextRing
    if #ring > 0 then
      eden.sound(W.SOUND.paint)
      eden.wait(0.06)
    end
  end
end)
