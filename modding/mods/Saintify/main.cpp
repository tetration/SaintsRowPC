// Saintify - hitting an NPC converts them to the Saints ("Playas" team).
//
// Detection: NPC objects keep a damage-event counter at +236 that increments
// when they take damage (see the damage snapshot function sub_82483828). When
// it changes, the damage source fields at +1064/+1068/+1080 identify the
// attacker; we convert only when the attacker is the player. Fields are
// logged for the first events so the layout can be verified.
//
// Conversion is a direct write of the team id at object+232 - the same store
// the game's own set_team script function performs (thunk 0x824DB708). The
// team id is read from the player (the player is a Playas).

#include <cstdint>
#include <cstdio>
#include <cstring>
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
constexpr int kObjType = 72;      // object+72: type (1 = player, 5 = vehicle)
constexpr int kObjTeam = 232;     // object+232: team id (set_team thunk store)
constexpr int kObjDamageCount = 236;  // object+236: damage event counter
constexpr int kObjHealth = 1912;  // object+1912: health f32
// Damage source fields, written by the attacker before the damage snapshot
// (sub_82483828 copies them to the +4132 ring). Which one is the attacker
// handle is verified from the log.
constexpr int kObjDmgSrc0 = 1064;
constexpr int kObjDmgSrc1 = 1068;
constexpr int kObjDmgSrc2 = 1080;

struct NpcState {
  uint32_t damage_count = 0;
  bool seen = false;
};

std::unordered_map<uint32_t, NpcState> g_npcs;  // object -> state
int g_frame = 0;
int g_converted = 0;
int g_logged_events = 0;

void OnFrame(void*) {
  if (++g_frame % 3 != 0) return;
  if (api->read_u8(kMpFlag) != 0) return;  // no converting in multiplayer
  const uint32_t player = api->read_u32(kPlayerPtr);
  if (!player) return;
  const uint32_t player_handle = api->read_u32(player + kObjHandle);
  const uint32_t saints_team = api->read_u32(player + kObjTeam);

  for (uint32_t index = 0; index < 4096; ++index) {
    const uint32_t obj = api->read_u32(kObjectTable + 12 + index * 16);
    if (!obj || obj == player) continue;
    const uint32_t type = api->read_u32(obj + kObjType);
    if (type == 1 || type == 5) continue;  // player / vehicle
    if (api->read_f32(obj + kObjHealth) <= 0.0f) continue;  // dead

    const uint32_t dmg = api->read_u32(obj + kObjDamageCount);
    NpcState& st = g_npcs[obj];
    if (!st.seen) {
      st.seen = true;
      st.damage_count = dmg;
      continue;
    }
    if (dmg == st.damage_count) continue;
    st.damage_count = dmg;

    // Damage event. Identify the attacker.
    const uint32_t src0 = api->read_u32(obj + kObjDmgSrc0);
    const uint32_t src1 = api->read_u32(obj + kObjDmgSrc1);
    const uint32_t src2 = api->read_u32(obj + kObjDmgSrc2);
    const bool from_player =
        src0 == player_handle || src1 == player_handle || src2 == player_handle;

    if (g_logged_events < 40 || from_player) {
      char line[224];
      snprintf(line, sizeof(line),
               "dmg event obj 0x%08X team %u src 1064=%08X 1068=%08X 1080=%08X "
               "player_handle %08X %s",
               obj, api->read_u32(obj + kObjTeam), src0, src1, src2, player_handle,
               from_player ? "<-- FROM PLAYER" : "");
      api->log(self, line);
      ++g_logged_events;
    }
    if (!from_player) continue;
    if (api->read_u32(obj + kObjTeam) == saints_team) continue;  // already a Saint

    api->write_u32(obj + kObjTeam, saints_team);
    ++g_converted;
    char line[160];
    snprintf(line, sizeof(line), "SAINTIFIED object 0x%08X (team now %u, total %d)", obj,
             saints_team, g_converted);
    api->log(self, line);
  }
}

}  // namespace

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader_api, const WmlMod* mod) {
  api = loader_api;
  self = mod;
  if (api->version < WML_API_VERSION) return 1;
  api->on_frame(OnFrame, nullptr);
  api->log(self, "Saintify v2 armed: damage an NPC to convert them.");
  return 0;
}

