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

#include <rex/ppc/context.h>
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

// Traffic spawn pipeline: car_spawn (0x82411310, returns the vehicle in r3).
constexpr uint32_t kCarSpawn = 0x82411310;

bool g_menu_open = false;
std::string g_note;
uint32_t g_target = 0;         // selected object (guest address)
std::string g_target_desc;

void TeleportObject(uint32_t obj, float x, float y, float z);

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
           "5) Dump cheat table (log)\n"
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
  TeleportObject(g_target, nx, ny, nz);
  g_note = "target moved";
  RefreshMenu();
}

// Writes the new position everywhere the object caches it: the game keeps
// position copies in several places (render at +20, physics body, bone/anim
// caches), and writing only +20 lets them snap the object back. We find every
// float triple currently equal to the object's position and rewrite them all.
void TeleportObject(uint32_t obj, float x, float y, float z) {
  const float cx = api->read_f32(obj + kObjPos);
  const float cy = api->read_f32(obj + kObjPos + 4);
  const float cz = api->read_f32(obj + kObjPos + 8);
  for (int off = 0; off <= 4200 - 12; off += 4) {
    const float fx = api->read_f32(obj + off);
    const float fy = api->read_f32(obj + off + 4);
    const float fz = api->read_f32(obj + off + 8);
    if (std::fabs(fx - cx) < 0.01f && std::fabs(fy - cy) < 0.01f && std::fabs(fz - cz) < 0.01f) {
      api->write_f32(obj + off, x);
      api->write_f32(obj + off + 4, y);
      api->write_f32(obj + off + 8, z);
    }
  }
}

// Capture a REAL car_spawn call (spot struct contents + arg registers) and
// replay it on demand. find_spots alone gives bare positions; the model/group
// context only exists in a genuine spawner call.
WmlGuestFunction g_orig_car_spawn = nullptr;
uint64_t g_car_args[4] = {};    // r3..r6 of the captured call
uint8_t g_spot_copy[256] = {};  // host-order copy of the spot struct
bool g_have_car_call = false;

void CarSpawnHook(WmlContext* ctx, uint8_t* base) {
  if (!g_have_car_call) {
    const uint32_t spot = static_cast<uint32_t>(api->get_r(ctx, 3));
    if (spot) {
      for (int i = 0; i < 256; i += 4) {
        const uint32_t v = api->read_u32(spot + i);  // host order
        memcpy(g_spot_copy + i, &v, 4);
      }
      for (int r = 3; r <= 6; ++r) g_car_args[r - 3] = api->get_r(ctx, r);
      g_have_car_call = true;
      api->log(self, "spawner: captured a live car spawn call");
    }
  }
  g_orig_car_spawn(ctx, base);
}

// Runs on the present hook (has a PPC context for api->call).
void SpawnVehicle(WmlContext* ctx, uint8_t* base) {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) {
    g_note = "not in gameplay";
    return;
  }
  if (!g_have_car_call) {
    g_note = "no car spawn captured yet (drive around first)";
    return;
  }
  // Synthetic context from the present hook's state (trainer pattern).
  PPCContext sctx;
  std::memcpy(&sctx, ctx, sizeof(sctx));
  if (sctx.r1.u32 < 0x2000) return;
  const uint32_t spot = sctx.r1.u32 - 8192;  // scratch below the frame
  sctx.r1.u32 -= 512;
  for (int i = 0; i < 256; i += 4) {
    uint32_t v;
    memcpy(&v, g_spot_copy + i, 4);
    api->write_u32(spot + i, v);
  }
  sctx.r3.u64 = spot;
  sctx.r4.u64 = g_car_args[1];
  sctx.r5.u64 = g_car_args[2];
  sctx.r6.u64 = g_car_args[3];
  api->call(reinterpret_cast<WmlContext*>(&sctx), kCarSpawn);
  const uint32_t vehicle = static_cast<uint32_t>(sctx.r3.u64);
  if (vehicle) {
    // The captured spot is wherever the real spawn was; bring it here.
    const float lx = api->read_f32(kCameraState + 104);
    const float lz = api->read_f32(kCameraState + 112);
    TeleportObject(vehicle, api->read_f32(player + kObjPos) + lx * 6.0f,
                   api->read_f32(player + kObjPos + 4),
                   api->read_f32(player + kObjPos + 8) + lz * 6.0f);
  }
  char note[160];
  snprintf(note, sizeof(note), "car spawn -> vehicle 0x%08X", vehicle);
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

// Dumps the cheat table (0x82B2E0B0, count at +512, entry pointers from +0).
// Weapon-grant cheats have function 0x821F56C0 (per the trainer); vehicle
// spawn cheats will have their own - this finds them by name/fn pointer.
void DumpCheats() {
  const uint32_t count = api->read_u32(0x82B2E0B0 + 512);
  char line[256];
  snprintf(line, sizeof(line), "--- cheat table: %u entries ---", count);
  api->log(self, line);
  for (uint32_t i = 0; i < count && i < 128; ++i) {
    const uint32_t entry = api->read_u32(0x82B2E0B0 + i * 4);
    if (!entry) continue;
    const uint32_t fn = api->read_u32(entry + 16);
    // Try to read a name string at a few likely offsets.
    char name[64] = {};
    for (int off : {0, 4, 8, 24}) {
      const uint32_t sp = api->read_u32(entry + off);
      if (sp < 0x82000000 || sp >= 0x84160000) continue;
      const char* s = reinterpret_cast<const char*>(
          static_cast<uint8_t*>(api->guest_pointer(sp)));
      int j = 0;
      for (; j < 63; ++j) {
        const char c = s[j];
        if (!c) break;
        if (c < 0x20 || c > 0x7E) break;
        name[j] = c;
      }
      name[j] = 0;
      if (j >= 3) break;
    }
    snprintf(line, sizeof(line), "  [%u] entry 0x%08X fn 0x%08X name \"%s\"", i, entry, fn, name);
    api->log(self, line);
  }
  g_note = "cheat table dumped to wml.log";
  RefreshMenu();
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
    } else if (api->key_pressed('5')) {
      DumpCheats();
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
  if (api->hook(kCarSpawn, CarSpawnHook, &g_orig_car_spawn) != 0) {
    api->log(self, "WARNING: car spawn hook failed");
  }
  api->log(self, "Spawner armed: F2 opens the menu.");
  return 0;
}
