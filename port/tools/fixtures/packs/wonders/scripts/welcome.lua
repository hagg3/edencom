-- welcome.lua: the "load" event runs once each time a world opens with this pack on.
-- eden.say shows a short message on screen for about three seconds.

eden.on("load", function()
  eden.wait(3)   -- let the world finish appearing first
  eden.say("Wonder Blocks: place the blue Guide Stone for a tour")
end)
