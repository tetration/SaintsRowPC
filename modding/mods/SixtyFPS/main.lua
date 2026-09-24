-- 60 FPS - port of the community "Unlock FPS" patch for Saints Row (Xbox 360,
-- by illusion and Tervel).
--
-- Every frame the game stores a minimum and maximum frame time as a pair of
-- floats at 0x827AA6D8 / 0x827AA6DC. The minimum (1/30 s) is what holds it at
-- 30 fps. The original patch replaces the instructions that store the pair;
-- here the two functions that do it are hooked and the pair is rewritten the
-- same way the patched code would: minimum 0, maximum the game's own limit.
-- The game's presentation still waits for the display's vertical blank, so
-- it runs at up to 60.

local FRAME_TIME = 0x827AA6D8

local sites = {
  { func = 0x82201668, limit = 0x8208A064 },  -- per-frame update
  { func = 0x82370A68, limit = 0x8202073C },  -- after loading
}

for _, site in ipairs(sites) do
  wml.hook(site.func, function(ctx)
    ctx:call_original()
    wml.write_f32(FRAME_TIME, 0.0)
    wml.write_f32(FRAME_TIME + 4, wml.read_f32(site.limit))
  end)
end

wml.log("Frame rate limit raised to 60")
