-- Saints Reborn logo: writes the Saints Reborn logo over the Saints Row logo
-- in the player's own copy of the textures that show it (all in
-- pegfiles.vpp_xbox2):
--   - interface-legal_<region> and interface-legal1: the startup legal screen
--     and the main menu backdrop (1280 wide, DXT1). What is left of the old
--     logo's orange flourish just below the new one is cleared.
--   - hud_sheet_15 in interface-backend: the main menu logo (DXT3)
--   - interface-loading-ui: the loading screen logo (DXT3)
-- logo.bin holds the logo as DXT blocks for each area (made from logo.png by
-- make_logo.py).

local PACK = "pegfiles.vpp_xbox2"
-- One entry per area in logo.bin: the pegs, the texture in them and its size.
local AREAS = {
  { pegs = { "interface-legal_us", "interface-legal_uk", "interface-legal_eu", "interface-legal_de",
             "interface-legal_aus", "interface-legal_nz", "interface-legal1" },
    w = 1280, h = { 768, 720 }, clean_rows = 6 },
  { pegs = { "interface-backend" }, texture = "hud_sheet_15.tga", w = 768, h = { 512 } },
  { pegs = { "interface-loading-ui" }, w = 512, h = { 256 } },
}

local f = io.open(wml.mod_folder .. "/logo.bin", "rb")
if not f then wml.log("logo.bin is missing") return end
local logo = f:read("a")
f:close()

local magic, count, pos = string.unpack("<c4I4", logo)
if magic ~= "SRL2" or count ~= #AREAS then wml.log("logo.bin is damaged") return end
for _, area in ipairs(AREAS) do
  if pos + 20 > #logo + 1 then wml.log("logo.bin is damaged") return end
  area.fmt, area.bx0, area.by0, area.bw, area.bh, pos = string.unpack("<I4I4I4I4I4", logo, pos)
  area.bs = area.fmt == 0x190 and 8 or 16
  area.data = pos
  pos = pos + area.bw * area.bh * area.bs
end
if pos ~= #logo + 1 then wml.log("logo.bin is damaged") return end

-- Byte offset of block (x, y) in a tiled Xbox 360 texture `w` blocks wide,
-- for 2^lb-byte blocks.
local function tiled(x, y, w, lb)
  local aw = (w + 31) & ~31
  local macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7)
  local micro = ((x & 7) + ((y & 6) << 2)) << lb
  local off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
  return ((((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb) << lb
end

-- A DXT1 block (console byte order) with its strongly coloured or dark end
-- points made black, so orange and red go while grey and white stay.
local function unsaturate(b)
  local c0, c1, i0, i1 = string.unpack(">I2I2I2I2", b)
  local function fix(c)
    local r, g, bl = (c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
    local hi, lo = math.max(r, g, bl), math.min(r, g, bl)
    return (hi - lo > 30 or hi < 70) and 0 or c
  end
  local n0, n1 = fix(c0), fix(c1)
  if n0 == c0 and n1 == c1 then return b end
  local idx = i0 | (i1 << 16)  -- 2 bits per pixel, pixel 0 lowest
  if c0 > c1 and n0 <= n1 then
    if n0 == n1 then
      n0, n1, idx = 0, 0, 0
    else  -- keep four-colour mode: swap the end points and remap 0<->1, 2<->3
      n0, n1, idx = n1, n0, idx ~ 0x55555555
    end
  end
  return string.pack(">I2I2I2I2", n0, n1, idx & 0xFFFF, idx >> 16)
end

-- The texture entry called `name` (or the first) in a peg: data offset,
-- width, height and format.
local function find_texture(peg, name)
  local n = string.unpack(">I2", peg, 0x10 + 1)
  for i = 0, n - 1 do
    local e = 0x18 + i * 0x48
    if #peg < e + 0x48 then return nil end
    local off, w, h, fmt = string.unpack(">I4I2I2I2", peg, e + 1)
    local tex = peg:sub(e + 0x16 + 1, e + 0x46):match("^[\1-\31]*([^%z]*)")
    if not name or tex:lower() == name:lower() then return off, w, h, fmt end
  end
end

local changed = 0
for _, area in ipairs(AREAS) do
  for _, peg_name in ipairs(area.pegs) do
    local file = peg_name .. ".peg_xbox2"
    local peg = wml.packfile_read(PACK, file)
    local off, w, h, fmt
    if peg and #peg >= 0x60 then off, w, h, fmt = find_texture(peg, area.texture) end
    local rows = h and ((h // 4 + 31) & ~31)  -- stored padded to 32 block rows
    local ok = off and w == area.w and fmt == area.fmt and #peg >= off + (w // 4) * rows * area.bs
    local h_ok = false
    for _, hh in ipairs(area.h) do h_ok = h_ok or h == hh end
    if peg and ok and h_ok then
      local lb = area.bs == 8 and 3 or 4
      local blocks = {}
      for by = 0, area.bh - 1 do
        for bx = 0, area.bw - 1 do
          local src = area.data + (by * area.bw + bx) * area.bs
          blocks[#blocks + 1] = { off + tiled(area.bx0 + bx, area.by0 + by, w // 4, lb),
                                  logo:sub(src, src + area.bs - 1) }
        end
      end
      for by = area.by0 + area.bh, area.by0 + area.bh + (area.clean_rows or 0) - 1 do
        for bx = area.bx0, area.bx0 + area.bw - 1 do
          local at = off + tiled(bx, by, w // 4, lb)
          local b = peg:sub(at + 1, at + 8)
          local nb = unsaturate(b)
          if nb ~= b then blocks[#blocks + 1] = { at, nb } end
        end
      end
      table.sort(blocks, function(a, b) return a[1] < b[1] end)
      local parts, at = {}, 0
      for _, b in ipairs(blocks) do
        parts[#parts + 1] = peg:sub(at + 1, b[1])
        parts[#parts + 1] = b[2]
        at = b[1] + #b[2]
      end
      parts[#parts + 1] = peg:sub(at + 1)
      wml.packfile_write(PACK, file, table.concat(parts))
      changed = changed + 1
    elseif peg then
      wml.log(file .. " is not the expected texture; left alone")
    end
  end
end
wml.log(string.format("Logo replaced in %d textures", changed))
