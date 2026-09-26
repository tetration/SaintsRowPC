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

// F3 toggles the mod in game (default: enabled); a notice is shown for 2 s.
bool g_enabled = true;
ULONGLONG g_notice_until = 0;

// Hook on sub_824470D0: the character damage function (r3 = victim object,
// r4 = attacker object; identified by hook-testing every function that
// subtracts from health at +1912). When the attacker is the player object,
// the victim is marked briefly and converted when its health actually drops.
WmlGuestFunction g_orig_damage_fn = nullptr;
std::unordered_map<uint32_t, ULONGLONG> g_hit_by_player;  // victim -> tick
int g_hook_log_budget = 20;
bool g_attribution_proven = false;  // set once the hook sees attacker == player

void ConvertNpc(WmlContext* ctx, uint8_t* base, uint32_t obj, uint32_t team, uint32_t player);
void ConvertLoop(uint32_t player, uint32_t saints_team, bool on_foot);
bool PlayerOnFoot(uint32_t player);
float DistanceToPlayer(uint32_t obj, uint32_t player);

void DamageHook(WmlContext* ctx, uint8_t* base) {
  const uint32_t victim = static_cast<uint32_t>(api->get_r(ctx, 3));
  const uint32_t attacker = static_cast<uint32_t>(api->get_r(ctx, 4));
  const uint32_t player = api->read_u32(kPlayerPtr);
  g_orig_damage_fn(ctx, base);
  if (!attacker || attacker != player || !victim || victim == player) return;
  if (!g_enabled) return;
  if (api->read_u8(kMpFlag) != 0) return;
  if (api->read_u32(victim + kObjType) != 1) return;       // humans only
  if (api->read_f32(victim + kObjHealth) <= 0.0f) return;  // dead
  const uint32_t saints_team = api->read_u32(player + kObjTeam);
  if (api->read_u32(victim + kObjTeam) == saints_team) return;  // already a Saint
  if (!PlayerOnFoot(player)) return;
  if (DistanceToPlayer(victim, player) > kMeleeRange) return;
  if (!g_attribution_proven) {
    g_attribution_proven = true;
    api->log(self, "player attribution confirmed (attacker == player); enforcing it");
  }
  ConvertNpc(ctx, base, victim, saints_team, player);
}

void ToggleEnabled() {
  g_enabled = !g_enabled;
  if (api->size >= sizeof(WmlApi) && api->overlay_text) {
    api->overlay_text(g_enabled ? "Saintify: ON (F3)" : "Saintify: OFF (F3)");
    g_notice_until = GetTickCount64() + 2000;
  }
}

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

// F9: snapshot the nearest NPC's entity and AI persona memory to a file, so
// two snapshots (e.g. before and after recruit+dismiss) can be diffed offline
// to find exactly which fields the game changes.
int g_snapshot_num = 0;

void SnapshotNearestNpc() {
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  uint32_t best = 0;
  float best_dist = 1e9f;
  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    if (api->read_u32(obj + kObjType) != 1) continue;
    const float d = DistanceToPlayer(obj, player);
    if (d < best_dist) {
      best_dist = d;
      best = obj;
    }
  }
  if (!best) {
    api->log(self, "snapshot: no NPC nearby");
    return;
  }
  char path[520];
  snprintf(path, sizeof(path), "%s\\..\\npc_snap_%d.txt", self->folder, g_snapshot_num++);
  FILE* f = fopen(path, "w");
  if (!f) return;
  const uint32_t ai = api->read_u32(best + 568);
  fprintf(f, "obj 0x%08X ai 0x%08X\n", best, ai);
  for (int off = 0; off <= 4200; off += 4) {
    const uint32_t v = api->read_u32(best + off);
    if (v) fprintf(f, "obj +%-5d 0x%08X\n", off, v);
  }
  if (ai) {
    for (int off = 0; off <= 4200; off += 4) {
      const uint32_t v = api->read_u32(ai + off);
      if (v) fprintf(f, "ai  +%-5d 0x%08X\n", off, v);
    }
  }
  fclose(f);
  char line[160];
  snprintf(line, sizeof(line), "snapshot %d written for obj 0x%08X (dist %.1f)",
           g_snapshot_num - 1, best, best_dist);
  api->log(self, line);
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
  // F3 toggles the mod; the notice auto-hides after 2 s. Both run every
  // frame (edge-triggered keys die under the throttle below).
  if (api->key_pressed(VK_F3)) {
    ToggleEnabled();
  }
  if (g_notice_until && GetTickCount64() > g_notice_until) {
    g_notice_until = 0;
    if (api->size >= sizeof(WmlApi) && api->overlay_text) {
      api->overlay_text("");
    }
  }
  if (!g_enabled) return;
  if (api->key_pressed(VK_F7)) {
    DumpNearbyObjects();
    return;
  }
  if (api->key_pressed(VK_F8)) {
    DumpAiFields();
    return;
  }
  if (api->key_pressed(VK_F9)) {
    SnapshotNearestNpc();
    return;
  }
  if (++g_frame % 3 != 0) return;
  if (api->read_u8(kMpFlag) != 0) return;  // no converting in multiplayer
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  const uint32_t saints_team = api->read_u32(player + kObjTeam);
  const bool on_foot = PlayerOnFoot(player);
  ConvertLoop(player, saints_team, on_foot);
}
// Applies the full conversion to an NPC object. When called from the damage
// hook (ctx != nullptr) the NPC's AI state is also reset to idle via
// sub_8257C400(obj, 19) - the core of npc_go_idle - so an in-flight flee
// action is cancelled and the brain re-evaluates with the new settings.
void ConvertNpc(WmlContext* ctx, uint8_t* base, uint32_t obj, uint32_t team, uint32_t player) {
  const uint32_t old_team = api->read_u32(obj + kObjTeam);
  api->write_u32(obj + kObjTeam, team);
  // combat_enable: clear the "combat disabled" bit (combat_disable sets
  // 0x08 at obj+3692, combat_enable clears it).
  api->write_u8(obj + kObjCombatFlags, api->read_u8(obj + kObjCombatFlags) & ~0x08u);
  // Cower/flee override: the AI persona (entity+568) keeps the mode at
  // +3704; 0 = personality default. Mode 4 = "never cower or flee"
  // (calibrated in game).
  const uint32_t ai = api->read_u32(obj + 568);
  if (ai) {
    api->write_u32(ai + 3704, g_next_flee_mode);
  } else {
    char note[128];
    snprintf(note, sizeof(note), "  note: obj 0x%08X has no AI persona (+568 null)", obj);
    api->log(self, note);
  }
  // Recruit/dismiss leaves an ex-civilian combat-ready; the snapshot diff
  // showed these entity fields change during that cycle. Replicate them.
  const uint32_t flags = api->read_u32(obj + 216);
  api->write_u32(obj + 216, flags & ~0x04000000u);
  api->write_u32(obj + 512, 1);
  api->write_u32(obj + 3568, 2);
  api->write_u32(obj + 3972, 1);
  // Leader links the recruit cycle wrote (is_in_party reads a leader handle
  // via the inner object; gunshot panic is suppressed for the leader's
  // party). Dismissed NPCs keep these without following.
  const uint32_t player_handle = api->read_u32(player + kObjHandle);
  api->write_u32(obj + 1128, player_handle);
  api->write_u32(obj + 1176, player_handle);
  api->write_u32(obj + 3976, player_handle);
  // Remaining recruit/dismiss changes from the snapshot diff: cleared
  // target/goal handles and one mode field.
  api->write_u32(obj + 292, 0xFFFFFFFFu);
  api->write_u32(obj + 772, 0xFFFFFFFFu);
  api->write_u32(obj + 776, 0xFFFFFFFFu);
  api->write_u32(obj + 2404, 0xFFFFFFFFu);
  api->write_u32(obj + 4200, 0x10);
  ++g_converted;
  char line[224];
  snprintf(line, sizeof(line), "SAINTIFIED object 0x%08X (team %u -> %u, total %d)%s", obj,
           old_team, team, g_converted, ctx ? " +ai reset" : "");
  api->log(self, line);
  if (ctx) {
    const uint64_t saved_r3 = api->get_r(ctx, 3);
    const uint64_t saved_r4 = api->get_r(ctx, 4);
    // AI state switch to 19 (the core of npc_go_idle, sub_8257C400). The
    // other calls in npc_go_idle (action cancel / brain reset) scrambled
    // converted NPCs, so only the state switch is kept.
    api->set_r(ctx, 3, obj);
    api->set_r(ctx, 4, 19);
    api->call(ctx, 0x8257C400);
    api->set_r(ctx, 3, saved_r3);
    api->set_r(ctx, 4, saved_r4);
  }
}

void ConvertLoop(uint32_t player, uint32_t saints_team, bool on_foot) {
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
    // Attribution: once the damage hook has proven that its attacker arg is
    // the player object, only convert NPCs the player actually damaged
    // (within the last 1.5 s). Until then, fall back to proximity-only.
    if (g_attribution_proven) {
      auto hit = g_hit_by_player.find(obj);
      if (hit == g_hit_by_player.end() ||
          GetTickCount64() - hit->second > 1500) {
        continue;
      }
      g_hit_by_player.erase(hit);
    }

    const uint32_t team = api->read_u32(obj + kObjTeam);
    if (team == saints_team) continue;  // already a Saint
    ConvertNpc(nullptr, nullptr, obj, saints_team, player);
  }
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  api->on_frame(OnFrame, nullptr);
  if (api->hook(0x824470D0, DamageHook, &g_orig_damage_fn) != 0) {
    api->log(self, "WARNING: damage hook failed; player attribution disabled");
  }
  api->log(self, "Saintify v6 armed: hit an NPC up close while on foot to convert them. F3 toggles, F7/F8 dump.");
  return 0;
}
