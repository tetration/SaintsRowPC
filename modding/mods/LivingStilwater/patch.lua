-- Living Stilwater - file part.
--
-- Edits the game's spawn tables in the player's own packfiles:
--   spawn_info_categories.xtbl  pedestrian/traffic density and group caps per area
--   special_spawns.xtbl         chance of the game's random street encounters
--
-- Densities are boosted relative to how busy a place already is: the busiest
-- times and places get the full multiplier, quieter ones (nights, the
-- airport, industrial areas, the suburbs) get much less of it. main.lua makes
-- the game honour area densities above 1.0.

local pedestrians = wml.setting("pedestrians", 2.5)
local traffic     = wml.setting("traffic", 2.5)
local busy_bias   = wml.setting("busy_bias", 1.5)
local outskirts   = wml.setting("outskirts", 0.35)
local encounters  = wml.setting("encounters", 2.5)

-- Areas that get only `outskirts` of the boost. Everything else is treated
-- as city (full boost).
local OUTSKIRTS = {
  ["Airport"] = true, ["Docks"] = true, ["Factory"] = true,
  ["Truck Yard"] = true, ["Train Yard"] = true, ["Race Track"] = true,
  ["WR Suburb"] = true, ["VK Suburb"] = true,
}

-- The game's editor allows group caps up to 20.
local MAX_GROUP_CAP = 20

local function fmt(x)
  local s = string.format("%.3f", x):gsub("0+$", ""):gsub("%.$", "")
  return s
end

-- How much a vanilla density `v` (0..1) is multiplied by.
local function boost(v, multiplier, weight)
  if v <= 0 then return 1 end
  return 1 + (multiplier - 1) * weight * (math.min(v, 1) ^ busy_bias)
end

local function patch_category(body, name)
  local weight = OUTSKIRTS[name] and outskirts or 1.0
  local largest = 1
  local function scale(tag, multiplier)
    body = body:gsub("(<" .. tag .. ">)%s*([%d%.]+)%s*(</" .. tag .. ">)", function(open, value, close)
      local v = tonumber(value)
      if not v then return nil end
      local m = boost(v, multiplier, weight)
      if m > largest then largest = m end
      return open .. fmt(v * m) .. close
    end, 1)
  end
  scale("CarDay", traffic)
  scale("CarNight", traffic)
  scale("PedDay", pedestrians)
  scale("PedNight", pedestrians)
  -- Each kind of person/vehicle has a cap; raise them in step so the extra
  -- density can actually be filled.
  body = body:gsub("(<(%a+Cap)>)%s*(%d+)%s*(</%2>)", function(open, _, value, close)
    local v = tonumber(value)
    if v > 0 then v = math.min(MAX_GROUP_CAP, math.max(v, math.ceil(v * largest))) end
    return open .. v .. close
  end)
  return body, largest
end

-- Top-level categories are the <Category> elements directly under <Table>;
-- they contain nested <Category> references, so walk them by depth.
local function patch_categories(xml)
  local out, pos, areas = {}, 1, 0
  local table_start = xml:find("<Table>", 1, true)
  if not table_start then return xml, 0 end
  out[#out + 1] = xml:sub(1, table_start + 6)
  pos = table_start + 7
  while true do
    local s = xml:find("<Category>", pos, true)
    local table_end = xml:find("</Table>", pos, true)
    if not s or (table_end and table_end < s) then break end
    -- Find the matching </Category>.
    local depth, i, e = 0, s, nil
    while true do
      local o = xml:find("<Category>", i, true)
      local c = xml:find("</Category>", i, true)
      if not c then break end
      if o and o < c then
        depth = depth + 1
        i = o + 10
      else
        depth = depth - 1
        i = c + 11
        if depth == 0 then e = c + 10 break end
      end
    end
    if not e then break end
    out[#out + 1] = xml:sub(pos, s - 1)
    local body = xml:sub(s, e)
    local name = body:match("^<Category>%s*<Name>([^<]*)</Name>") or ""
    local patched = patch_category(body, name)
    out[#out + 1] = patched
    areas = areas + 1
    pos = e + 1
  end
  out[#out + 1] = xml:sub(pos)
  return table.concat(out), areas
end

local function patch_special_spawns(xml)
  -- Chances are probabilities, so they stop at 1.
  local count = 0
  xml = xml:gsub("<Spawn_chance>%s*([%d%.]+)%s*</Spawn_chance>", function(value)
    local v = tonumber(value)
    if not v then return nil end
    count = count + 1
    return "<Spawn_chance>" .. fmt(math.min(1.0, v * encounters)) .. "</Spawn_chance>"
  end)
  return xml, count
end

local packs = { "misc.vpp_xbox2", "misc2.vpp_xbox2" }
for _, pack in ipairs(packs) do
  local categories = wml.packfile_read(pack, "spawn_info_categories.xtbl")
  if categories then
    local xml, areas = patch_categories(categories)
    wml.packfile_write(pack, "spawn_info_categories.xtbl", xml)
    wml.log(string.format("%s: people up to x%s, traffic up to x%s, busy bias %s, outskirts %s (%d areas)",
      pack, fmt(pedestrians), fmt(traffic), fmt(busy_bias), fmt(outskirts), areas))
  end
  local specials = wml.packfile_read(pack, "special_spawns.xtbl")
  if specials then
    local xml, count = patch_special_spawns(specials)
    wml.packfile_write(pack, "special_spawns.xtbl", xml)
    wml.log(string.format("%s: %d random encounters, chance x%s", pack, count, fmt(encounters)))
  end
end
