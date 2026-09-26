// Saintify - hitting an NPC up close converts them to the Saints ("Playas").
//
// Detection: an NPC whose health drops while within melee range (~4.5 m) of
// the on-foot player is considered hit by the player. Positions are at
// object+20 (vec3f, from the FirstPerson mod's notes); health at +1912.
// Close-range gunfire also converts - the melee-weapon check needs the
// player's current-weapon field, not mapped yet.
//
// Conversion is a direct write of the team id at object+232 - the same store
// the game's own set_team script function performs (thunk 0x824DB708). The
// team id is read from the player (the player is a Playas).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "wml.h"

namespace {

const WmlApi* api;
const WmlMod* self;

constexpr uint32_t kPlayerPtr = 0x8309ABEC;
constexpr uint32_t kObjectTable = 0x830866C8;  // object = +12 + index*16
constexpr uint32_t kMpFlag = 0x8370E9F6;

constexpr int kObjHandle = 68;    // object+68: handle
constexpr int kObjType = 72;      // object+72: type (1 = human, 2/3 = props/corpses, 5 = vehicle)
constexpr int kObjTeam = 232;     // object+232: team id (set_team thunk store)
constexpr int kObjHealth = 1912;  // object+1912: health f32
constexpr int kObjPos = 20;       // object+20: position vec3f (FirstPerson notes)
constexpr int kObjCombatFlags = 3692;  // object+3692: bit 0x08 = combat disabled

constexpr float kMeleeRange = 4.5f;

std::unordered_map<uint32_t, float> g_health;  // object -> last seen health
int g_frame = 0;
int g_converted = 0;
uint32_t g_next_flee_mode = 4;  // calibrated: 4 = "never cower or flee"

bool PlayerOnFoot(uint32_t player) {
  if (api->read_u32(player + 3456) != 0) return false;
  const uint32_t vehicle_handle = api->read_u32(player + 2496);
  if (vehicle_handle && (api->read_u8(player + 2569) & 0x10)) return false;
  const uint32_t state = api->read_u32(player + 508);
  if (state == 9 || state == 12) return false;
  if (!vehicle_handle) return true;
  const uint32_t index = vehicle_handle & 0xFFFF;
  if (index >= 4096) return true;
  const uint32_t object = api->read_u32(kObjectTable + 12 + index * 16);
  if (!object) return true;
  return !(api->read_u32(object + kObjHandle) == vehicle_handle &&
           api->read_u32(object + kObjType) == 5);
}

float DistanceToPlayer(uint32_t obj, uint32_t player) {
  const float dx = api->read_f32(obj + kObjPos) - api->read_f32(player + kObjPos);
  const float dy = api->read_f32(obj + kObjPos + 4) - api->read_f32(player + kObjPos + 4);
  const float dz = api->read_f32(obj + kObjPos + 8) - api->read_f32(player + kObjPos + 8);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void DumpNearbyObjects() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  // The 5 cower/flee mode names from the image (0x821F9588 table).
  api->log(self, "--- cower/flee mode names ---");
  for (int i = 0; i < 5; ++i) {
    const uint32_t str_ptr = api->read_u32(0x821F9588 + i * 4);
    const char* s = str_ptr ? reinterpret_cast<const char*>(
                         static_cast<uint8_t*>(api->guest_pointer(str_ptr)))
                            : nullptr;
    char line[128];
    snprintf(line, sizeof(line), "  mode %d = \"%s\"", i, s ? s : "(unreadable)");
    api->log(self, line);
  }
  api->log(self, "--- nearby object dump ---");
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    const float dist = DistanceToPlayer(obj, player);
    if (dist > 12.0f) continue;
    char line[320];
    const uint32_t ai = api->read_u32(obj + 568);  // AI persona sub-object?
    snprintf(line, sizeof(line),
             "obj 0x%08X idx %u type %u handle %08X team@232 %d health %.1f "
             "ai@568 %08X flee@ai+3704 %u dist %.1f",
             obj, index, api->read_u32(obj + kObjType), api->read_u32(obj + kObjHandle),
             int32_t(api->read_u32(obj + kObjTeam)), api->read_f32(obj + kObjHealth), ai,
             ai ? api->read_u32(ai + 3704) : 0xFFFFFFFFu, dist);
    api->log(self, line);
  }
  api->log(self, "--- end dump ---");
}

// F8: for each nearby human NPC, dump pointer-looking fields in the AI region
// of the object (+3000..+4200). Fields that are equal within a behavior group
// (civilians vs gang members) but differ between groups are personality
// pointer candidates.
void DumpAiFields() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  api->log(self, "--- AI field scan (nearby humans) ---");
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    if (api->read_u32(obj + kObjType) != 1) continue;
    if (DistanceToPlayer(obj, player) > 12.0f) continue;
    char line[320];
    snprintf(line, sizeof(line), "obj 0x%08X team %d health %.1f:", obj,
             int32_t(api->read_u32(obj + kObjTeam)), api->read_f32(obj + kObjHealth));
    api->log(self, line);
    for (int off = 3000; off <= 4200; off += 4) {
      const uint32_t v = api->read_u32(obj + off);
      if (v < 0x82000000 || v >= 0x84160000) continue;  // pointers into image/data only
      snprintf(line, sizeof(line), "    +%d: 0x%08X", off, v);
      api->log(self, line);
    }
    // The archetype/character definition at +3552: dump every nonzero word so
    // the personality field can be spotted by comparing archetypes.
    const uint32_t arch = api->read_u32(obj + 3552);
    if (arch >= 0x82000000 && arch < 0x84160000) {
      snprintf(line, sizeof(line), "    archetype 0x%08X:", arch);
      api->log(self, line);
      for (int off = 0; off <= 1020; off += 4) {
        const uint32_t v = api->read_u32(arch + off);
        if (v != 0) {
          snprintf(line, sizeof(line), "      +%d: 0x%08X", off, v);
          api->log(self, line);
        }
      }
    }
  }
  api->log(self, "--- end AI field scan ---");
}

void OnFrame(void*) {
  // F7 is edge-triggered (true only the frame the key goes down), so check
  // it every frame - before the throttle below, or 2 of 3 presses are lost.
  if (api->key_pressed(VK_F7)) {
    DumpNearbyObjects();
    return;
  }
  if (api->key_pressed(VK_F8)) {
    DumpAiFields();
    return;
  }
  if (++g_frame % 3 != 0) return;
  if (api->read_u8(kMpFlag) != 0) return;  // no converting in multiplayer
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  const uint32_t saints_team = api->read_u32(player + kObjTeam);
  const bool on_foot = PlayerOnFoot(player);

  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    // Only humans (type 1; that includes the player, excluded above).
    if (api->read_u32(obj + kObjType) != 1) continue;
    const float health = api->read_f32(obj + kObjHealth);
    if (health <= 0.0f) continue;  // dead

    auto [it, inserted] = g_health.emplace(obj, health);
    const float before = it->second;
    it->second = health;
    if (inserted || !on_foot || health >= before - 0.5f) continue;
    if (DistanceToPlayer(obj, player) > kMeleeRange) continue;

    const uint32_t team = api->read_u32(obj + kObjTeam);
    if (team == saints_team) continue;  // already a Saint

    api->write_u32(obj + kObjTeam, saints_team);
    // combat_enable: clear the "combat disabled" bit (combat_disable sets
    // 0x08 at obj+3692, combat_enable clears it).
    api->write_u8(obj + kObjCombatFlags, api->read_u8(obj + kObjCombatFlags) & ~0x08u);
    // Cower/flee override: the AI persona (entity+568) keeps the mode at
    // +3704; 0 = personality default, 1-4 = overrides (set_cower_flee_mode).
    // Calibration: each conversion tries the next value so the log shows
    // which one makes civilians fight.
    const uint32_t ai = api->read_u32(obj + 568);
    uint32_t mode = 0;
    if (ai) {
      mode = g_next_flee_mode;
      api->write_u32(ai + 3704, mode);
      if (g_next_flee_mode < 4) ++g_next_flee_mode;
    }
    ++g_converted;
    char line[224];
    snprintf(line, sizeof(line),
             "SAINTIFIED object 0x%08X (team %u -> %u, flee mode %u, total %d)", obj, team,
             saints_team, mode, g_converted);
    api->log(self, line);
  }
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  api->on_frame(OnFrame, nullptr);
  api->log(self, "Saintify v5 armed: hit an NPC up close while on foot to convert them. F7 dumps nearby objects.");
  return 0;
}
