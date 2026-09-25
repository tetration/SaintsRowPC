-- Living Stilwater - runtime part.
--
-- The game decides how many cars and people it wants around the player as
--
--     wanted = min(script_density, area_density) * maximum
--
-- where area_density comes from spawn_info_categories.xtbl (edited by
-- patch.lua) and script_density is a global the game's missions set with
-- set_traffic_density / set_ped_density (1.0 normally, lower in some
-- missions). The min() means area values above 1.0 would do nothing, so while
-- the game asks "how many do I want", a normal script density is raised for
-- that one call. Densities lowered by a mission are left alone.
--
-- People have one more limit: the game never keeps more than 26 of its
-- ambient pedestrians at once. The ped spawner compares "how many are there"
-- against "how many do I want" and against that 26, so both numbers are
-- shown to it scaled down by the same factor; it then fills the streets up to
-- the real number. The game's own limit on characters in total (75) still
-- applies and keeps memory safe.

local TRAFFIC_WANTED   = 0x824194A8  -- () -> number of traffic cars wanted
local PEDS_WANTED      = 0x82419208  -- () -> number of ambient people wanted
local PEDS_COUNT       = 0x8240DB70  -- () -> number of ambient people now
local TRAFFIC_DENSITY  = 0x827AD060  -- float set by set_traffic_density
local PED_DENSITY      = 0x827AD064  -- float set by set_ped_density
local TRAFFIC_COUNT    = 0x8309A1C0  -- number of traffic cars now

local AMBIENT_PED_LIMIT = 26         -- the game's fixed limit
local LIFTED = 100.0

local TRAFFIC_RADIUS   = 0x82832724  -- how far from the camera traffic is placed

local max_cars   = math.floor(wml.setting("max_traffic_cars", 28))
local max_people = math.floor(wml.setting("max_people", 60))
local debug      = wml.setting("debug", false)
local traffic_distance = wml.setting("traffic_distance", 1.5)

-- Place traffic further out, so cars are already there before you get close.
-- This runs before the game starts, so values the game derives from the
-- radius at start-up (like how far away cars are removed) follow along.
local radius = wml.read_f32(TRAFFIC_RADIUS)
if traffic_distance > 0 and traffic_distance ~= 1 then
  wml.write_f32(TRAFFIC_RADIUS, radius * traffic_distance)
end

local function to_int(v)
  v = v & 0xFFFFFFFF
  if v >= 0x80000000 then v = v - 0x100000000 end
  return v
end

-- Traffic despawn check (0x82412878, run for every traffic car): true when
-- the car is too far from the camera. Most of its distances follow the
-- traffic radius, but a car that has been on screen is removed beyond a fixed
-- 140 m (and 130 m in another case). With a larger radius, cars were placed
-- beyond that and removed again almost straight away, so scale those two for
-- the duration of the check.
local TRAFFIC_DESPAWN_CHECK = 0x82412878
local SEEN_DESPAWN_DISTANCE = 0x82089EE4   -- 140.0
local OTHER_DESPAWN_DISTANCE = 0x82089EE0  -- 130.0
-- Cars behind the camera are removed at a much shorter distance than cars in
-- front of it (0x83AD2644 vs 0x83AD2640, both set up from the traffic
-- radius). Turning around for a moment was enough to delete the cars you had
-- just been looking at. With keep_cars_behind, cars behind you stay as long
-- as they would in front of you.
local FRONT_DESPAWN_DISTANCE = 0x83AD2640
local BEHIND_DESPAWN_DISTANCE = 0x83AD2644
local keep_behind = wml.setting("keep_cars_behind", true)
local despawned = 0
local scale = (traffic_distance > 0) and traffic_distance or 1
if scale ~= 1 or keep_behind then
  local seen = wml.read_f32(SEEN_DESPAWN_DISTANCE)
  local other = wml.read_f32(OTHER_DESPAWN_DISTANCE)
  wml.hook(TRAFFIC_DESPAWN_CHECK, function(ctx)
    local behind = wml.read_f32(BEHIND_DESPAWN_DISTANCE)
    wml.write_f32(SEEN_DESPAWN_DISTANCE, seen * scale)
    wml.write_f32(OTHER_DESPAWN_DISTANCE, other * scale)
    if keep_behind then wml.write_f32(BEHIND_DESPAWN_DISTANCE, math.max(behind, wml.read_f32(FRONT_DESPAWN_DISTANCE))) end
    ctx:call_original()
    wml.write_f32(SEEN_DESPAWN_DISTANCE, seen)
    wml.write_f32(OTHER_DESPAWN_DISTANCE, other)
    if keep_behind then wml.write_f32(BEHIND_DESPAWN_DISTANCE, behind) end
    if (ctx:r(3) & 0xFF) ~= 0 then despawned = despawned + 1 end
  end)
end

-- Runs the original with a normal script density lifted out of the way.
local function call_lifted(ctx, address)
  local density = wml.read_f32(address)
  local lift = density >= 0.999
  if lift then wml.write_f32(address, LIFTED) end
  ctx:call_original()
  if lift then wml.write_f32(address, density) end
  return to_int(ctx:r(3))
end

local last = { cars = 0, people = 0, people_shown = 0 }

wml.hook(TRAFFIC_WANTED, function(ctx)
  local wanted = math.min(call_lifted(ctx, TRAFFIC_DENSITY), max_cars)
  last.cars = wanted
  ctx:set_r(3, wanted)
end)

-- The traffic spawner also trims traffic back to 12 cars (a number compiled
-- into the game) by removing the car furthest from the camera. Only let it
-- trim above our own maximum.
local TRAFFIC_TRIM = 0x82411E30   -- removes the furthest traffic car; r3 = removed
local trims = 0
wml.hook(TRAFFIC_TRIM, function(ctx)
  if to_int(wml.read_u32(TRAFFIC_COUNT)) < max_cars then
    ctx:set_r(3, 0)
    return
  end
  ctx:call_original()
  trims = trims + 1
end)

-- When picking a car model for a new traffic car, the game skips any model
-- that already has 3 cars on the road (another number compiled into the
-- game). Only a handful of car models are loaded at a time, so this held
-- traffic to about 12 cars. The counting function (0x82413AD8) is only used
-- for this check; its result is scaled so the limit becomes same_model_limit.
local SAME_MODEL_COUNT = 0x82413AD8
local same_model_limit = math.max(3, math.floor(wml.setting("same_model_limit", 8)))
local same_model_max = 0
wml.hook(SAME_MODEL_COUNT, function(ctx)
  ctx:call_original()
  local n = to_int(ctx:r(3))
  if n > same_model_max then same_model_max = n end
  ctx:set_r(3, (n * 3) // same_model_limit)
end)

-- The spawner may place cars in plain view when few of its cars are on
-- screen: fewer than a quarter of the cars it wants. With more cars wanted
-- that happened far more often, so cars popped up in front of you. Keep the
-- game's own threshold (a quarter of 12 = 3) for that decision. The flag is
-- the 4th argument (r7) of the spot search called at 0x82412F2C; the count of
-- cars on screen is in the spawner's r22.
local FIND_SPAWN_SPOTS = 0x82412298
local SPAWNER_SPOT_CALL = 0x82412F30  -- return address of that call
local VANILLA_ON_SCREEN_THRESHOLD = 3
local stat = { throttled = 0, checks = 0, spots = 0, spot_calls = 0, tries = 0, spawned = 0,
               in_view = 0, model_fail = 0 }
wml.hook(FIND_SPAWN_SPOTS, function(ctx)
  local from_spawner = ctx:lr() == SPAWNER_SPOT_CALL
  if from_spawner then
    local on_screen = to_int(ctx:r(22))
    local allow_in_view = on_screen < VANILLA_ON_SCREEN_THRESHOLD and 1 or 0
    ctx:set_r(7, allow_in_view)
    stat.in_view = stat.in_view + allow_in_view
  end
  ctx:call_original()
  if from_spawner then
    stat.spot_calls = stat.spot_calls + 1
    stat.spots = stat.spots + to_int(ctx:r(3))
  end
end)

wml.hook(PEDS_WANTED, function(ctx)
  local wanted = math.min(call_lifted(ctx, PED_DENSITY), max_people)
  last.people = wanted
  local shown = math.min(wanted, AMBIENT_PED_LIMIT)
  last.people_shown = shown
  ctx:set_r(3, shown)
end)

-- The spawner always asks PEDS_WANTED right before PEDS_COUNT.
wml.hook(PEDS_COUNT, function(ctx)
  ctx:call_original()
  local count = to_int(ctx:r(3))
  if last.people > last.people_shown and last.people > 0 then
    count = math.floor(count * last.people_shown / last.people)
  end
  ctx:set_r(3, count)
end)

wml.log(string.format("Running (up to %d traffic cars, %d people)", max_cars, max_people))
if debug then
  wml.log(string.format("Game maximums at density 1.0: traffic %.1f, people %.1f",
    wml.read_f32(0x8208978C), wml.read_f32(0x82089998)))
  wml.log(string.format("Traffic radius %.1f -> %.1f", radius, wml.read_f32(TRAFFIC_RADIUS)))

  -- Pedestrian placement distance (read by the ped spawner, 0x8240E538).
  -- (This used to hook a helper the whole game calls constantly just to log
  -- it; that cost frame rate on every thread even after logging stopped.)
  wml.log(string.format("Ped radius const %.2f", wml.read_f32(0x820897A8)))
end

if debug then
  -- Traffic spawner (0x824129C8) internals, counted per log line.
  wml.hook(0x82594028, function(ctx)   -- "vehicle near player" check -> 4-6 s throttle
    local from_spawner = ctx:lr() == 0x82412EE0
    ctx:call_original()
    if from_spawner then
      stat.checks = stat.checks + 1
      if (ctx:r(3) & 0xFF) ~= 0 then stat.throttled = stat.throttled + 1 end
    end
  end)
  wml.hook(0x824195E8, function(ctx)   -- picks a car model for a spot, r3 = found
    local from_spawn = ctx:lr() == 0x8241158C
    ctx:call_original()
    if from_spawn and (ctx:r(3) & 0xFF) == 0 then stat.model_fail = stat.model_fail + 1 end
  end)
  wml.hook(0x82411310, function(ctx)   -- spawns a car at a spot, r3 = vehicle
    local from_spawner = ctx:lr() == 0x82412FF8
    ctx:call_original()
    if from_spawner then
      stat.tries = stat.tries + 1
      if (ctx:r(3) & 0xFFFFFFFF) ~= 0 then stat.spawned = stat.spawned + 1 end
    end
  end)

  local frames = 0
  wml.on_frame(function()
    frames = frames + 1
    if frames % 300 ~= 0 then return end
    wml.log(string.format(
      "cars %d/%d wanted (vehicles total %d), people wanted %d | spot searches %d (avg %.1f spots), " ..
      "spawn tries %d, spawned %d (no model %d, most of one model %d), in-view allowed %d, throttle %d/%d, trimmed %d, despawned %d | density %.2f/%.2f",
      wml.read_u32(TRAFFIC_COUNT), last.cars, wml.read_u32(0x83710238), last.people,
      stat.spot_calls, stat.spot_calls > 0 and stat.spots / stat.spot_calls or 0,
      stat.tries, stat.spawned, stat.model_fail, same_model_max, stat.in_view, stat.throttled, stat.checks, trims, despawned,
      wml.read_f32(TRAFFIC_DENSITY), wml.read_f32(PED_DENSITY)))
    for k in pairs(stat) do stat[k] = 0 end
    trims = 0
    despawned = 0
    same_model_max = 0
  end)
end

-- Detail distance: the game's own tuning values for how far away car parts
-- are drawn, street lights fade and trees switch to their simple versions.
-- They are set once the game has filled them in.
local detail = wml.setting("detail_distance", 2.0)
if detail > 0 and detail ~= 1 then
  local tuning = {
    { 0x827AC0C8, detail },       -- vehicle_component_cull_distance
    { 0x827AC0C4, 1 / detail },   -- vehicle component cull size
    -- level_lights_fade_dist_cap (0x827AD184) is left alone: raising it
    -- draws light pools on roads far away with broken, rainbow colours.
    { 0x827AC480, detail },       -- Tree_lod_scale
  }
  local applied = false
  wml.on_frame(function()
    if applied then return end
    for _, t in ipairs(tuning) do
      if not (wml.read_f32(t[1]) > 0) then return end
    end
    for _, t in ipairs(tuning) do
      wml.write_f32(t[1], wml.read_f32(t[1]) * t[2])
    end
    applied = true
  end)
end

-- Car draw distance: every car model has four detail distances in the vehicle
-- table; past the last one the car is not drawn at all.
local VEHICLE_INFO, VEHICLE_INFO_SIZE, VEHICLE_INFO_COUNT = 0x83E8ABD8, 1196, 0x8371023C
local LAST_LOD = 280
local car_draw = wml.setting("car_draw_distance", 600)
if car_draw > 0 then
  local frames = 0
  wml.on_frame(function()
    frames = frames + 1
    if frames % 120 ~= 1 then return end
    local count = to_int(wml.read_u32(VEHICLE_INFO_COUNT))
    if count <= 0 or count > 256 then return end
    for i = 0, count - 1 do
      local a = VEHICLE_INFO + i * VEHICLE_INFO_SIZE + LAST_LOD
      local d = wml.read_f32(a)
      if d > 0 and d < car_draw and d > wml.read_f32(a - 4) then wml.write_f32(a, car_draw) end
    end
  end)
end

-- Traffic spacing: the spawner picks one of the free road spots near the edge
-- of the traffic area at random (up to 3 tries). A spot too close to another
-- traffic car is refused, so the next try lands somewhere else.
local SPAWN_AT_SPOT = 0x82411310
local SPAWNER_SPAWN_CALL = 0x82412FF8
local TRAFFIC_LIST = 0x8309A374      -- first node; node+0 next, node+8 handle
local OBJECT_TABLE = 0x830866C8
local spacing = wml.setting("traffic_spacing", 30)
if spacing > 0 then
  local min2 = spacing * spacing
  wml.hook(SPAWN_AT_SPOT, function(ctx)
    if ctx:lr() == SPAWNER_SPAWN_CALL then
      local spot = ctx:r(3)
      local sx, sz = wml.read_f32(spot), wml.read_f32(spot + 8)
      local node = wml.read_u32(TRAFFIC_LIST)
      local guard = 0
      while node ~= 0 and guard < 64 do
        guard = guard + 1
        local handle = wml.read_u32(node + 8)
        local obj = wml.read_u32(OBJECT_TABLE + 12 + (handle & 0xFFFF) * 16)
        if obj ~= 0 and (handle & 0xFFFF) < 4096 and wml.read_u32(obj + 68) == handle then
          local dx, dz = wml.read_f32(obj + 20) - sx, wml.read_f32(obj + 28) - sz
          if dx * dx + dz * dz < min2 then
            ctx:set_r(3, 0)
            return
          end
        end
        node = wml.read_u32(node)
      end
    end
    ctx:call_original()
  end)
end
