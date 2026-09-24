-- Example Lua mod for Whompay's Mod Loader.
-- Everything the loader offers is in the global `wml` table; see
-- modding/README.md for the full list.

wml.log("Hello from Lua! Press F8 in game to say hi.")

local frames = 0

wml.on_frame(function()
  frames = frames + 1
  if wml.key_pressed("F8") then
    wml.log("Hi! Frames rendered so far: " .. frames)
  end
end)
