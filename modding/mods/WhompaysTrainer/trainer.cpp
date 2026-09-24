#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <rex/ppc/context.h>
#include "wml.h"

namespace {
const WmlApi* api;
const WmlMod* self;
WmlGuestFunction original_update;
std::atomic<uint64_t> update_ticks{0};
std::atomic<unsigned> request{0};
std::atomic<int> result{0};
std::atomic<bool> god{false};
constexpr uint32_t PLAYER = 0x8309ABEC;
constexpr uint32_t CHEATS = 0x82B2E0B0;
constexpr uint32_t INVULNERABLE = 0x00100000;
constexpr unsigned RESPECT=1, GUNS=2, MONEY=3, GOD=4;
uint32_t god_player=0, god_handle=0, old_god=0;
uint32_t guns_player=0, guns_handle=0;
int gun_index=-1, gun_count=0;
bool visible=false;
std::string status="Load a single-player game.";
uint64_t queued_at=0;

bool readable(uint32_t address, size_t length) {
    if (!address || address + uint64_t(length) > UINT32_MAX) return false;
    auto p = reinterpret_cast<uintptr_t>(api->guest_pointer(address));
    const auto end = p + length;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) ||
            m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return false;
        auto next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

uint32_t player() {
    // The same single-player pointer used by cash, health and weapon cheats.
    if (api->read_u8(0x8370E9F6)) return 0; // multiplayer session
    uint32_t p=api->read_u32(PLAYER);
    if (!readable(p, 3864) || api->read_u32(p+72)!=1) return 0;
    int threshold=static_cast<int>(api->read_u32(p+3860));
    if (threshold<=0 || threshold>1000000 || !(api->read_f32(p+1912)>0)) return 0;
    return p;
}

void invulnerability(uint32_t p, bool enabled) {
    uint32_t flags=api->read_u32(p+216);
    api->write_u32(p+216, (flags & ~INVULNERABLE) | (enabled?INVULNERABLE:0));
    api->write_u32(p+212,api->read_u32(p+212)|1); // game's dirty flag
}

void process(WmlContext* raw) {
    const unsigned command=request.exchange(0);
    const uint32_t p=player();
    if (!p) {
        if (command) {
            result=5;
            uint32_t candidate=api->read_u32(PLAYER);
            if (readable(candidate,3864)) {
                char details[160];
                std::snprintf(details,sizeof(details),"Player not ready: ptr=%08X type=%u health=%.1f respect-bar=%u multiplayer=%u",
                    candidate,api->read_u32(candidate+72),api->read_f32(candidate+1912),
                    api->read_u32(candidate+3860),api->read_u8(0x8370E9F6));
                api->log(self,details);
            }
        }
        if (gun_index>=0) {gun_index=-1; result=6;}
        return;
    }
    const uint32_t handle=api->read_u32(p+68);
    if (command==RESPECT) {
        // The HUD and game cap respect at 99 full mission bars.
        api->write_u32(p+3848,api->read_u32(p+3860)*99);
        result=1;
    } else if (command==MONEY) {
        // Cash is stored in cents; game cash_add clamps at 2,000,000,000.
        int64_t value=static_cast<int32_t>(api->read_u32(p+2320));
        value=std::clamp<int64_t>(value+10000000,0,2000000000);
        api->write_u32(p+2320,static_cast<uint32_t>(value));
        result=3;
    } else if (command==GUNS) {
        guns_player=p; guns_handle=handle; gun_index=0; gun_count=0;
        result=7;
    } else if (command==GOD) {
        if (god.exchange(!god.load())) {
            if (god_player==p && god_handle==handle) invulnerability(p,old_god!=0);
            god_player=0;
        }
        result=4;
    }
    if (god) {
        if (god_player!=p || god_handle!=handle) {
            god_player=p; god_handle=handle;
            old_god=api->read_u32(p+216)&INVULNERABLE;
        }
        invulnerability(p,true);
    }
    if (gun_index<0) return;
    if (guns_player!=p || guns_handle!=handle) {gun_index=-1;result=6;return;}
    uint32_t count=api->read_u32(CHEATS+512);
    if (count>128) {gun_index=-1; result=6; return;}
    while (gun_index<static_cast<int>(count)) {
        uint32_t entry=api->read_u32(CHEATS+4*gun_index++);
        if (!readable(entry,32) || api->read_u32(entry+16)!=0x821F56C0) continue;
        // One weapon per update, on the guest game thread. Keep ALL CPU state
        // untouched, including LR/CR/vector registers, and reserve stack space.
        PPCContext ctx;
        std::memcpy(&ctx,raw,sizeof(ctx));
        if (ctx.r1.u32<0x1000 || !readable(ctx.r1.u32-0x1000,0x1000)) {
            gun_index=-1; result=6; return;
        }
        ctx.r1.u32-=0x200;
        ctx.r3.u64=entry+24;
        if (api->call(reinterpret_cast<WmlContext*>(&ctx),0x821F56C0)!=0) {
            gun_index=-1; result=6; return;
        }
        ++gun_count;
        return;
    }
    gun_index=-1;
    result=gun_count?2:6;
}

void update_hook(WmlContext* ctx,uint8_t* base) {
    original_update(ctx,base);
    if (update_ticks.fetch_add(1)==0) api->log(self,"Recurring update hook reached.");
    process(ctx);
}

void on_frame(void*) {
    if (api->key_pressed(VK_F4)) visible=!visible;
    if (visible) {
        for (unsigned id=RESPECT;id<=GOD;++id) {
            if (!api->key_pressed('0'+id) && !api->key_pressed(VK_NUMPAD0+id)) continue;
            unsigned expected=0;
            if (request.compare_exchange_strong(expected,id)) {
                queued_at=GetTickCount64();
                status="Applying...";
                char message[96];
                std::snprintf(message,sizeof(message),"Action %u requested; update ticks %llu",id,
                    static_cast<unsigned long long>(update_ticks.load()));
                api->log(self,message);
            }
        }
    }
    const int code=result.exchange(0);
    switch(code) {
        case 1:status="Respect: 99 bars.";break;
        case 2:status="Guns granted. Normal slot limits apply.";break;
        case 3:status="Added $100,000 (up to the cash limit).";break;
        case 4:status=god?"God Mode enabled.":"God Mode disabled.";break;
        case 5:status="Load a single-player game first.";break;
        case 6:status="Guns stopped: player/data not ready.";break;
        case 7:status="Giving guns...";break;
    }
    if (code) api->log(self,status.c_str());
    if (request.load() && GetTickCount64()-queued_at>3000) {
        // Never leave a deferred cheat armed across a later loading transition.
        request.exchange(0);
        status="No gameplay update. Resume the game and retry.";
        api->log(self,status.c_str());
    }
    if (!visible) {api->overlay_text("");return;}
    std::string text="Whompays Trainer\n\n"
        "1   Max Reputation\n"
        "2   All Guns\n"
        "3   Money  +$100,000\n"
        "4   God Mode  ";
    text+=god?"ON":"OFF";
    text+="\n\nF4  Close    |    Press 1-4\n\n"+status;
    api->overlay_text(text.c_str());
}
}

extern "C" WML_EXPORT int wml_mod_init(const WmlApi* loader,const WmlMod* mod) {
    if (!loader || loader->version!=WML_API_VERSION || loader->size<sizeof(WmlApi) ||
        !loader->overlay_text) return 1;
    api=loader; self=mod;
    // 82201668 is state ENTRY, not a frame tick. 82209E30 is the recurring
    // state-machine update called by the game loop in normal play and menus.
    if (api->hook(0x82209E30,update_hook,&original_update)!=0) return 2;
    api->on_frame(on_frame,nullptr);
    api->log(self,"Loaded text-overlay trainer v0.2. F4 to toggle; keys 1-4 activate.");
    return 0;
}
