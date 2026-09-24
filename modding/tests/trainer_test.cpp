#include "../mods/WhompaysTrainer/trainer.cpp"
#include <cassert>
#include <cstdio>
#include <map>
namespace {
std::map<uint32_t,uint32_t> memory;
unsigned char* fake_region;
constexpr uint32_t P=0x90001000;
uint8_t read8(uint32_t a){return static_cast<uint8_t>(memory[a]);}
uint32_t read32(uint32_t a){return memory[a];}
void write32(uint32_t a,uint32_t v){memory[a]=v;}
float readfloat(uint32_t a){return a==P+1912?100.f:0.f;}
void* pointer(uint32_t a){return a>=P && a<P+0x8000?fake_region+(a-P):nullptr;}
int calls=0;
bool keys[256]{};
std::string overlay;
WmlGuestFunction installed_hook=nullptr;
WmlFrameCallback installed_frame=nullptr;
int call(WmlContext* raw,uint32_t fn){
    assert(fn==0x821F56C0);
    auto& ctx=*reinterpret_cast<PPCContext*>(raw);
    assert(ctx.r3.u32==P+0x2018);
    ++calls; ctx.lr=123;ctx.r17.u64=666;ctx.cr6.eq=1;
    return 0;
}
void original(WmlContext*,uint8_t*){}
}
int main(){
    fake_region=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x8000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    assert(fake_region);
    WmlApi mock{}; mock.read_u8=read8;mock.read_u32=read32;mock.write_u32=write32;
    mock.log=[](const WmlMod*,const char*){};
    mock.read_f32=readfloat;mock.guest_pointer=pointer;mock.call=call;
    api=&mock;original_update=original;
    PPCContext ctx{};ctx.r1.u32=P+0x7000;ctx.r17.u64=42;ctx.lr=99;
    auto raw=reinterpret_cast<WmlContext*>(&ctx);
    request=MONEY;process(raw);assert(result==5 && memory[P+2320]==0);
    memory[PLAYER]=P;memory[P+72]=1;memory[P+68]=1234;memory[P+3860]=1000;
    request=MONEY;process(raw);assert(memory[P+2320]==10000000);
    memory[P+2320]=1999999900;request=MONEY;process(raw);assert(memory[P+2320]==2000000000);
    request=RESPECT;process(raw);assert(memory[P+3848]==99000);
    memory[P+216]=0x40;
    request=GOD;process(raw);assert(god && memory[P+216]==(INVULNERABLE|0x40));
    request=GOD;process(raw);assert(!god && memory[P+216]==0x40);
    memory[P+216]|=INVULNERABLE;
    request=GOD;process(raw);request=GOD;process(raw);assert(memory[P+216]&INVULNERABLE);
    memory[0x8370E9F6]=1;request=MONEY;process(raw);assert(result==5);
    memory[0x8370E9F6]=0;
    memory[CHEATS+512]=2;memory[CHEATS]=P+0x2000;memory[CHEATS+4]=P+0x2100;
    memory[P+0x2010]=0x821F56C0;memory[P+0x2110]=0x821F5290;
    PPCContext saved;std::memcpy(&saved,&ctx,sizeof(ctx));
    request=GUNS;process(raw);assert(calls==1);
    assert(std::memcmp(&ctx,&saved,sizeof(ctx))==0);
    process(raw);assert(result==2 && calls==1 && gun_index==-1);
    memory[CHEATS+512]=129;request=GUNS;process(raw);assert(result==6 && calls==1);
    memory[CHEATS+512]=2;request=GUNS;process(raw);memory[PLAYER]=0;
    process(raw);assert(gun_index==-1 && result==6);
    // Exercise the mod's actual input -> request -> recurring hook -> overlay path.
    WmlMod info{"Whompays Trainer", "."};self=&info;
    mock.version=WML_API_VERSION;mock.size=sizeof(mock);
    mock.log=[](const WmlMod*,const char*){};
    mock.key_pressed=[](int vk){bool pressed=keys[vk];keys[vk]=false;return int(pressed);};
    mock.overlay_text=[](const char* text){overlay=text;};
    mock.hook=[](uint32_t address,WmlGuestFunction fn,WmlGuestFunction* prev){
        assert(address==0x82209E30);installed_hook=fn;*prev=original;return 0;
    };
    mock.on_frame=[](WmlFrameCallback fn,void*){installed_frame=fn;};
    assert(wml_mod_init(&mock,&info)==0);
    assert(installed_hook && installed_frame);
    memory[PLAYER]=P;memory[P+2320]=0;
    keys[VK_F4]=true;installed_frame(nullptr);
    assert(visible && overlay.find("Whompays Trainer")!=std::string::npos);
    keys['3']=true;installed_frame(nullptr);assert(request==MONEY);
    installed_hook(raw,nullptr);assert(memory[P+2320]==10000000 && request==0);
    installed_frame(nullptr);assert(overlay.find("Added $100,000")!=std::string::npos);
    keys[VK_F4]=true;installed_frame(nullptr);assert(!visible && overlay.empty());
    keys['1']=true;installed_frame(nullptr);assert(request==0);
    keys['1']=false;
    request=MONEY;queued_at=GetTickCount64()-4001;installed_frame(nullptr);
    assert(request==0 && status.find("retry")!=std::string::npos);
    WmlApi old=mock;old.size=sizeof(mock)-sizeof(void*);
    assert(wml_mod_init(&old,&info)==1);
    VirtualFree(fake_region,0,MEM_RELEASE);
    puts("Trainer tests passed: readiness, cash/cap, respect, god toggle/restore, multiplayer guard, weapon dispatch/filtering/cancellation, full CPU-state preservation, F4/hotkey-to-update dispatch, overlay feedback, hidden-menu isolation, stale-request cancellation, old-host rejection.");
}
