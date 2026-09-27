// Object & Ped Spawner - dev mod for Whompay's Mod Loader.
//
// F2 menu:
//   1) select target (nearest human/vehicle the camera points at)
//   2) save selected address (+ archetype name) to mods\spawner_targets.txt
//   3) bring target to me (teleport it in front of the player)
//   4) spawn a vehicle near me (experimental: reuses the traffic spawner's
//      spot search + car spawn functions)
//
// Object facts used (see saintswiki): handle table at 0x830866C8
// (object = +12 + index*16, +68 = handle, +72 = type: 1 = human, 5 =
// vehicle), position vec3f at +20, archetype pointer at +3552 (name at +0).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wml.h"

namespace {

const WmlApi* api;
const WmlMod* self;

constexpr uint32_t kPlayerPtr = 0x8309ABEC;
constexpr uint32_t kObjectTable = 0x830866C8;
constexpr int kObjHandle = 68;
constexpr int kObjType = 72;
constexpr int kObjPos = 20;
constexpr int kObjArchetype = 3552;

// Camera (FirstPerson mod notes): state at 0x827D9778; +80 = orientation,
// row 3 (+104) points where the camera looks.
constexpr uint32_t kCameraState = 0x827D9778;

// Traffic spawn pipeline: find_spots (0x82412298) then car_spawn
// (0x82411310, returns the vehicle in r3).
constexpr uint32_t kFindSpots = 0x82412298;
constexpr uint32_t kCarSpawn = 0x82411310;

bool g_menu_open = false;
std::string g_note;
uint32_t g_target = 0;         // selected object (guest address)
std::string g_target_desc;

// Params captured from a live find_spots call (the traffic spawner builds
// them with helpers; reusing a captured copy avoids guessing the layout).
uint8_t g_spot_params[128] = {};
bool g_have_spot_params = false;
WmlGuestFunction g_orig_find_spots = nullptr;
WmlGuestFunction g_orig_present = nullptr;

float ObjDist(uint32_t a, uint32_t b) {
  const float dx = api->read_f32(a + kObjPos) - api->read_f32(b + kObjPos);
  const float dy = api->read_f32(a + kObjPos + 4) - api->read_f32(b + kObjPos + 4);
  const float dz = api->read_f32(a + kObjPos + 8) - api->read_f32(b + kObjPos + 8);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string ArchetypeName(uint32_t obj) {
  const uint32_t arch = api->read_u32(obj + kObjArchetype);
  if (arch < 0x82000000 || arch >= 0x84160000) return "?";
  char name[9] = {};
  for (int i = 0; i < 8; ++i) {
    const uint8_t c = api->read_u8(arch + i);
    if (!c) break;
    name[i] = char(c);
  }
  return name;
}

void RefreshMenu() {
  if (!g_menu_open) return;
  if (api->size < sizeof(WmlApi) || !api->overlay_text) return;
  char text[640];
  snprintf(text, sizeof(text),
           "Spawner (F2 close)\n"
           "1) Select target: %s\n"
           "2) Save target address\n"
           "3) Bring target to me\n"
           "4) Spawn vehicle near me (experimental)\n"
           "%s%s",
           g_target ? g_target_desc.c_str() : "(none)",
           g_note.empty() ? "" : "\n", g_note.c_str());
  api->overlay_text(text);
}

void ToggleMenu() {
  g_menu_open = !g_menu_open;
  g_note.clear();
  if (g_menu_open) {
    RefreshMenu();
  } else if (api->size >= sizeof(WmlApi) && api->overlay_text) {
    api->overlay_text("");
  }
}

// Nearest human/vehicle within 30 m in the camera's forward cone.
void SelectTarget() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  // Camera look direction (row 3 of the orientation at +104).
  const float lx = api->read_f32(kCameraState + 104);
  const float ly = api->read_f32(kCameraState + 108);
  const float lz = api->read_f32(kCameraState + 112);
  uint32_t best = 0;
  float best_score = 0.5f;  // minimum dot product (~60 deg cone)
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    const uint32_t type = api->read_u32(obj + kObjType);
    if (type != 1 && type != 5) continue;  // humans and vehicles
    const float dx = api->read_f32(obj + kObjPos) - api->read_f32(player + kObjPos);
    const float dy = api->read_f32(obj + kObjPos + 4) - api->read_f32(player + kObjPos + 4);
    const float dz = api->read_f32(obj + kObjPos + 8) - api->read_f32(player + kObjPos + 8);
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1.0f || dist > 30.0f) continue;
    const float dot = (dx * lx + dy * ly + dz * lz) / dist;
    if (dot > best_score) {
      best_score = dot;
      best = obj;
    }
  }
  g_target = best;
  if (best) {
    char desc[96];
    snprintf(desc, sizeof(desc), "0x%08X type %u \"%s\"", best,
             api->read_u32(best + kObjType), ArchetypeName(best).c_str());
    g_target_desc = desc;
    g_note = "target selected";
  } else {
    g_target_desc.clear();
    g_note = "nothing in front of you";
  }
  RefreshMenu();
}

void SaveTarget() {
  if (!g_target) {
    g_note = "no target selected";
    RefreshMenu();
    return;
  }
  char path[520];
  snprintf(path, sizeof(path), "%s\\..\\spawner_targets.txt", self->folder);
  FILE* f = fopen(path, "a");
  if (f) {
    fprintf(f, "0x%08X type %u archetype \"%s\" handle %08X\n", g_target,
            api->read_u32(g_target + kObjType), ArchetypeName(g_target).c_str(),
            api->read_u32(g_target + kObjHandle));
    fclose(f);
  }
  char note[160];
  snprintf(note, sizeof(note), "saved 0x%08X to spawner_targets.txt", g_target);
  g_note = note;
  api->log(self, note);
  RefreshMenu();
}

void BringTarget() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player || !g_target) {
    g_note = "no target selected";
    RefreshMenu();
    return;
  }
  // 2.5 m in front of the player, using the camera look direction.
  const float lx = api->read_f32(kCameraState + 104);
  const float lz = api->read_f32(kCameraState + 112);
  const float nx = api->read_f32(player + kObjPos) + lx * 2.5f;
  const float ny = api->read_f32(player + kObjPos + 4);
  const float nz = api->read_f32(player + kObjPos + 8) + lz * 2.5f;
  api->write_f32(g_target + kObjPos, nx);
  api->write_f32(g_target + kObjPos + 4, ny);
  api->write_f32(g_target + kObjPos + 8, nz);
  g_note = "target moved";
  RefreshMenu();
}

// Capture the spot-search params the traffic spawner built, for reuse.
void FindSpotsHook(WmlContext* ctx, uint8_t* base) {
  if (!g_have_spot_params) {
    const uint32_t params = static_cast<uint32_t>(api->get_r(ctx, 5));
    if (params) {
      for (int i = 0; i < 128; i += 4) {
        const uint32_t v = api->read_u32(params + i);
        memcpy(g_spot_params + i, &v, 4);  // keep big-endian bytes
      }
      g_have_spot_params = true;
      api->log(self, "spawner: captured find_spots params");
    }
  }
  g_orig_find_spots(ctx, base);
}

// Runs on the present hook (has a PPC context for api->call).
uint32_t ctx_r1(WmlContext* ctx) { return static_cast<uint32_t>(api->get_r(ctx, 1)); }

void SpawnVehicle(WmlContext* ctx, uint8_t* base) {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) {
    g_note = "not in gameplay";
    return;
  }
  if (!g_have_spot_params) {
    g_note = "no spot params yet (drive around a bit first)";
    return;
  }
  // Guest scratch space on the current guest stack.
  const uint32_t scratch = ctx_r1(ctx) - 4096;
  const uint32_t spots_out = scratch;       // find_spots writes spots here
  const uint32_t params = scratch + 1024;   // our captured params copy
  for (int i = 0; i < 128; i += 4) {
    uint32_t v;
    memcpy(&v, g_spot_params + i, 4);
    api->write_u32(params + i, v);
  }
  const uint64_t saved_r3 = api->get_r(ctx, 3);
  const uint64_t saved_r4 = api->get_r(ctx, 4);
  const uint64_t saved_r5 = api->get_r(ctx, 5);
  const uint64_t saved_r6 = api->get_r(ctx, 6);
  const uint64_t saved_r7 = api->get_r(ctx, 7);

  api->set_r(ctx, 3, spots_out);
  api->set_r(ctx, 4, player + kObjPos);  // search around the player
  api->set_r(ctx, 5, params);
  api->set_r(ctx, 6, 0);
  api->set_r(ctx, 7, 1);  // allow spots in view
  api->call(ctx, kFindSpots);
  const uint32_t result = static_cast<uint32_t>(api->get_r(ctx, 3));

  uint32_t vehicle = 0;
  if (result) {
    const uint32_t spot = api->read_u32(spots_out);  // first spot
    if (spot) {
      api->set_r(ctx, 3, spot);
      api->set_r(ctx, 4, 0);
      api->set_r(ctx, 5, 0);
      api->set_r(ctx, 6, 0);
      api->call(ctx, kCarSpawn);
      vehicle = static_cast<uint32_t>(api->get_r(ctx, 3));
    }
  }

  api->set_r(ctx, 3, saved_r3);
  api->set_r(ctx, 4, saved_r4);
  api->set_r(ctx, 5, saved_r5);
  api->set_r(ctx, 6, saved_r6);
  api->set_r(ctx, 7, saved_r7);

  char note[160];
  snprintf(note, sizeof(note), "find_spots -> %u, vehicle = 0x%08X", result, vehicle);
  g_note = note;
  api->log(self, note);
}

bool g_spawn_requested = false;

void PresentHook(WmlContext* ctx, uint8_t* base) {
  if (g_spawn_requested) {
    g_spawn_requested = false;
    SpawnVehicle(ctx, base);
    RefreshMenu();
  }
  g_orig_present(ctx, base);
}

void OnFrame(void*) {
  if (api->key_pressed(VK_F2)) {
    ToggleMenu();
  }
  if (g_menu_open) {
    if (api->key_pressed('1')) {
      SelectTarget();
    } else if (api->key_pressed('2')) {
      SaveTarget();
    } else if (api->key_pressed('3')) {
      BringTarget();
    } else if (api->key_pressed('4')) {
      g_spawn_requested = true;  // executed on the present hook (has ctx)
      g_note = "spawning...";
    }
    RefreshMenu();
  }
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  api->on_frame(OnFrame, nullptr);
  if (api->hook(0x825E54A8, PresentHook, &g_orig_present) != 0) {
    api->log(self, "WARNING: present hook failed; spawning disabled");
  }
  if (api->hook(kFindSpots, FindSpotsHook, &g_orig_find_spots) != 0) {
    api->log(self, "WARNING: find_spots hook failed");
  }
  api->log(self, "Spawner armed: F2 opens the menu.");
  return 0;
}
