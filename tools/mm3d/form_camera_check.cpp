// clang++ -std=c++20 -Isrc tools/mm3d/form_camera_check.cpp -o /tmp/form-camera-check
#include <cassert>
#include <fstream>
#include <unordered_map>
#include "mm3d/form_camera.h"
using namespace MM3D::FormCamera;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 p){auto i=words.find(p);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 p,U32 v){assert(words.contains(p));words[p]=v;}
};
static void DoorEntryChecks() {
    const U32 ctx=0x900000,game=0xa00000,actor=0xb00000,cam=game+0x408,stack=0xc00000;
    for(U32 form:{1u,2u,3u,4u}) {
        Memory m;m.words={{0x72f5c0,ctx},{ctx+0x16c,game},{game+0x13c,0x400},
            {game+0xa78,0},{game+0x20dc,actor},{cam+0xa0,game},{cam+0xa4,actor},
            {actor,0x20000},{actor+0x1fc,form<<24},{actor+0x11db0,0},{actor+0x11db8,0},{actor+0xdfc,0},
            {cam+0x158,0},{cam+0x154,8u<<16},{cam+0x170,5u<<16},
            {cam+0x110,std::bit_cast<U32>(45.f)},{stack,0}};
        const U32 modes=0xd00000,params=0xe00000;
        m.words[0x667894+50*12+8]=modes;m.words[modes]=0xa0002;m.words[modes+4]=params;
        m.words[params+8]=0x2012c;m.words[params+28]=0x7002d;
        for(U32 i=0;i<3;++i){m.words[cam+0x50+i*4]=0;m.words[cam+0x74+i*4]=std::bit_cast<U32>(i==2?61.752655f:0.f);}
        EntryRecoveryOrbit door;Hook hook;std::array<U32,16> regs{};regs[0]=regs[4]=cam;regs[13]=stack;
        const auto observe=[&](U32 request){hook.ChangeMode(m,cam,request);door.Observe(m,cam,request);};
        // Recorded DOORC -> DUNGEON0 -> setting50, with repeated rejected
        // freecam requests and a native NORMAL request in the same update.
        observe(31);observe(0);m.words[cam+0x154]=17u<<16;observe(0);
        m.words[cam+0x170]=20u<<16;observe(31);
        m.words[cam+0x154]=50u<<16;m.words[cam+0x158]=31;m.words[cam+0x170]=0;observe(31);
        for(float native:{151.81636f,170.35558f,207.88098f,214.45802f}) {
            m.words[stack]=std::bit_cast<U32>(native);const auto before=m.words;
            const bool bypass=door.Native(m,cam);assert(bypass==(form!=3));
            hook.Entry(m,regs,bypass);hook.Radius(m,regs);
            assert(m.words==before&&!hook.saved); // no61.75 override; lens untouched
            m.words[cam+0x170]=1u<<16;observe(31);
        }
        // Leaving this freecam session restores ordinary orbit retention.
        observe(0);m.words[cam+0x158]=0;observe(31);m.words[cam+0x158]=31;
        m.words[cam+0x170]=0;hook.Entry(m,regs,door.Native(m,cam));
        assert(hook.saved==(form!=3));
        // Entering without stick must not exempt a much later freecam.
        door={};m.words[cam+0x154]=8u<<16;observe(0);
        m.words[cam+0x154]=50u<<16;m.words[cam+0x158]=0;m.words[cam+0x170]=1u<<16;observe(0);
        m.words[cam+0xf0]=std::bit_cast<U32>(300.f);observe(0);
        observe(31);m.words[cam+0x158]=31;assert(!door.Native(m,cam));
        // Dialogue exit: direct TALK->FREECAMERA and early/late takeover
        // during NORMAL's lens/distance recovery must all retain native orbit.
        for(unsigned normal_updates:{0u,1u,15u}) {
            door={};hook={};m.words[cam+0x158]=11;m.words[cam+0x170]=0x10008;
            m.words[cam+0x110]=std::bit_cast<U32>(30.f);m.words[cam+0xf0]=std::bit_cast<U32>(61.752655f);
            observe(normal_updates?0:31);
            for(unsigned i=0;i<normal_updates;++i) {
                m.words[cam+0x158]=0;m.words[cam+0x170]=1u<<16;observe(0);
            }
            observe(31);m.words[cam+0x158]=31;m.words[cam+0x170]=0x10008;
            bool bypass=door.Native(m,cam);assert(bypass==(form!=3));
            hook.Entry(m,regs,bypass);assert(!hook.saved&&!(m.words[cam+0x170]&8));
            m.words[stack]=std::bit_cast<U32>(151.81636f);const auto before=m.words;
            hook.Radius(m,regs);assert(m.words==before); // stale target cleanup still works
        }
        // Recorded first-person exit: Human goes directly6->31 at ~50 units;
        // Deku goes6->0->31 and already uses native distance. Also cover delayed
        // stick input during NORMAL recovery for Goron/Zora/Human. Bow/Hookshot
        // modes12/13 use the same native SUBJECT handler with different parameters.
        for(U32 aim_mode:{6u,12u,13u})for(unsigned normal_updates:{0u,1u,15u}) {
            door={};hook={};m.words[cam+0x154]=50u<<16;
            m.words[cam+0x158]=aim_mode;m.words[cam+0x170]=1u<<16;
            m.words[cam+0x110]=std::bit_cast<U32>(45.f);
            m.words[cam+0xf0]=std::bit_cast<U32>(49.99977f);
            m.words[cam+0x74+8]=std::bit_cast<U32>(49.99977f);
            observe(normal_updates?0:31);
            for(unsigned i=0;i<normal_updates;++i) {
                m.words[cam+0x158]=0;observe(0);
            }
            observe(31);m.words[cam+0x158]=31;m.words[cam+0x170]=0;
            for(float native:{151.81636f,170.35558f,207.88098f,227.49765f}) {
                hook.Entry(m,regs,door.Native(m,cam));assert(!hook.saved);
                m.words[stack]=std::bit_cast<U32>(native);const auto before=m.words;
                hook.Radius(m,regs);assert(m.words==before);
                m.words[cam+0x170]=1u<<16;observe(31);
            }
        }
        // A completed first-person return must not exempt unrelated later orbits.
        door={};hook={};m.words[cam+0x158]=6;observe(0);
        m.words[cam+0x158]=0;m.words[cam+0x170]=1u<<16;
        m.words[cam+0xf0]=std::bit_cast<U32>(300.f);observe(0);
        observe(31);m.words[cam+0x158]=31;
        assert(!door.Native(m,cam));hook.Entry(m,regs,false);
        assert(hook.saved==(form!=3));
        // Camera/form ownership changes cannot carry the exemption forward.
        for(unsigned invalid=0;invalid<3;++invalid) {
            door={};m.words[cam+0x154]=8u<<16;m.words[game+0xa78]=0;
            m.words[actor+0x1fc]=form<<24;m.words[cam+0xa0]=game;observe(31);
            m.words[cam+0x154]=50u<<16;observe(31);
            if(invalid==0)m.words[game+0xa78]=1u<<16;
            if(invalid==1)m.words[actor+0x1fc]=(form==4?2u:4u)<<24;
            if(invalid==2)m.words[cam+0xa0]=game+4;
            assert(!door.Native(m,cam));
        }
    }
}
int main(int argc,char** argv){
    DoorEntryChecks();
    Memory m;Hook hook;const U32 context=0x900000,game=0xa00000,player=0xb00000,cam=game+0x408,stack=0xc00000;
    m.words[0x72f5c0]=context;m.words[context+0x16c]=game;
    m.words[game+0x13c]=0x400;m.words[game+0xa78]=0;
    m.words[cam+0xa0]=game;m.words[cam+0xa4]=player;m.words[game+0x20dc]=player;
    m.words[player]=0x20000;
    std::array<U32,16> regs{};regs[0]=cam;regs[13]=stack;
    for(U32 form:{1u,2u,4u}){
        hook={};m.words[player+0x1fc]=form<<24;m.words[cam+0x158]=31;m.words[cam+0x170]=0;
        for(unsigned i=0;i<3;++i){m.words[cam+0x50+i*4]=0;m.words[cam+0x74+i*4]=std::bit_cast<U32>(i==2?230.f:0.f);}
        // +F0 can still carry a stale lock-on/interpolation sample.
        m.words[cam+0xf0]=std::bit_cast<U32>(195.f);
        auto before=m.words;hook.Entry(m,regs);assert(m.words==before&&regs[4]==cam&&hook.radius==230);
        m.words[cam+0x170]=1u<<16;
        // Replay both observed branches: 277 -> 184 -> 165, and normal ~160.
        for(float calculated:{277.f,184.f,165.f,160.f,170.65639f,159.63138f,157.42638f}){
            m.words[stack]=std::bit_cast<U32>(calculated);m.words[stack+4]=0x12345678;
            before=m.words;auto expected=before;expected[stack]=std::bit_cast<U32>(230.f);
            hook.Radius(m,regs);assert(m.words==expected&&regs[2]==stack);
            hook.Entry(m,regs);assert(hook.radius==230);
        }
        // Distance diagnostic replays only S1; S0 must not be clobbered there.
        std::array<U32,64> fp{};fp.fill(0xabcdef01);auto expected=fp;expected[1]=m.words[cam+0xf0];
        hook.Distance(m,cam,fp);assert(fp==expected);
        // A second real lock-on escapes Restoration's sticky free camera.
        regs[0]=cam;regs[1]=9;regs[14]=0x7d08fc;regs[15]=LockPC+4;
        hook.LockMode(m,regs);assert(regs[0]==0&&regs[15]==regs[14]);
        // Battle mode (14), unlock, then orbit captures the new visible distance.
        hook.ChangeMode(m,cam,14);assert(!hook.saved);
        hook.ChangeMode(m,cam,0);
        regs[0]=cam;m.words[cam+0x74+8]=std::bit_cast<U32>(240.f);m.words[cam+0x170]=0;
        hook.Entry(m,regs);assert(hook.saved&&hook.radius==240);
        // Switching forms cannot write the old form's radius into the new one.
        m.words[player+0x1fc]=3u<<24;m.words[stack]=std::bit_cast<U32>(120.f);
        before=m.words;hook.Radius(m,regs);assert(m.words==before);
    }
    for(U32 form:{0u,3u,5u}){
        m.words[player+0x1fc]=form<<24;regs[0]=cam;regs[1]=9;regs[15]=LockPC+4;
        hook.LockMode(m,regs);assert(regs[3]==0&&regs[0]==cam&&regs[15]==LockPC+4);
        hook.Entry(m,regs);assert(!hook.saved);
    }
    m.words[player+0x1fc]=2u<<24;
    for(U32 mode:{0u,10u,11u,14u,31u}){
        regs[0]=cam;regs[1]=mode;regs[15]=LockPC+4;hook.LockMode(m,regs);
        assert(regs[3]==mode-9&&regs[0]==cam&&regs[15]==LockPC+4);
    }
    // Recorded 0 -> 31 transition retained lock-on bit3 for exactly one
    // frame. Clear just that bit before the native handler chooses its aim.
    for(U32 previous:{0u,9u,14u,31u}) {
        hook={};regs[0]=cam;regs[4]=cam;regs[1]=31;
        m.words[cam+0x158]=previous;m.words[cam+0x170]=0x10048;
        hook.ChangeMode(m,cam,31);m.words[cam+0x158]=31;
        const auto before=m.words;auto expected=before;
        if(previous==0)expected[cam+0x170]&=~8u;
        hook.Entry(m,regs);assert(m.words==expected);
    }
    // Recorded second enemy lock: mode31 remains requested even though bit3
    // rises again. NPC locks and stale target pointers must remain unchanged.
    const U32 enemy=0xd00000;m.words[cam+0xbc]=enemy;
    m.words[player+0x11db0]=0x8000;m.words[player+0xdfc]=enemy;
    for(U32 form:{0u,1u,2u,3u,4u,5u})for(U32 category:{4u,5u,6u,9u})
    for(U32 active:{0u,8u})for(U32 previous:{0u,9u,10u,14u,31u}) {
        m.words[player+0x1fc]=form<<24;m.words[enemy]=(category<<16)|0x123;
        m.words[cam+0x170]=0x10000|active;m.words[cam+0x158]=previous;
        const auto before=m.words;
        const bool battle=SupportedForm(form)&&active&&(category==5||category==9)&&
                          (previous==14||previous==31);
        assert(hook.RequestedMode(m,cam,31)==(battle?14u:31u));
        for(U32 requested:{0u,9u,10u,14u,5u})assert(hook.RequestedMode(m,cam,requested)==requested);
        // First orbit frame after unlock must still take the stale-flag fix.
        m.words[cam+0x158]=0;assert(hook.RequestedMode(m,cam,31)==31);
        m.words[cam+0x158]=previous;assert(m.words==before);
    }
    // Replay held-stick frames: enter battle once and never alternate 14/31.
    for(U32 form:{1u,2u,4u})for(U32 category:{5u,9u}) {
        hook={};m.words[player+0x1fc]=form<<24;m.words[enemy]=(category<<16)|0x123;
        // Captured active enemy lock has bit15, with bit16 absent.
        m.words[player+0x11db0]=0x8000;m.words[player+0xdfc]=enemy;
        m.words[cam+0x170]=0x10008;m.words[cam+0x158]=31;
        unsigned transitions=0;
        for(unsigned frame=0;frame<120;++frame) {
            const auto effective=hook.RequestedMode(m,cam,31);
            assert(effective==14);
            transitions+=m.words[cam+0x158]!=effective;
            hook.ChangeMode(m,cam,effective);m.words[cam+0x158]=effective;
        }
        assert(transitions==1);
        // Unlock keeps its native transition, then clears only the stale flag
        // and recaptures the free-camera distance without retaining battle.
        assert(hook.RequestedMode(m,cam,0)==0);hook.ChangeMode(m,cam,0);
        m.words[cam+0x158]=0;
        assert(hook.RequestedMode(m,cam,31)==31);hook.ChangeMode(m,cam,31);
        m.words[cam+0x158]=31;regs[0]=cam;hook.Entry(m,regs);
        assert(m.words[cam+0x170]==0x10000&&hook.saved&&hook.radius==240);
        assert(hook.RequestedMode(m,cam,31)==31);
    }
    // Clock Town dog capture: Deku NORMAL -> FREECAMERA retained bit3,
    // then native radius jumped151.38 ->280.21 ->183.83. Deku needs stale
    // target cleanup too, without adopting the other forms' radius override.
    for(U32 previous:{0u,14u}) {
        hook={};m.words[player+0x1fc]=3u<<24;m.words[cam+0x158]=previous;
        m.words[cam+0x170]=0x10048;m.words[player+0x11db0]=0;m.words[player+0xdfc]=0;
        const auto before=m.words;
        hook.ChangeMode(m,cam,31);assert(m.words==before&&hook.release_target);
        m.words[cam+0x158]=31;regs[0]=cam;hook.Entry(m,regs);
        auto expected=before;expected[cam+0x158]=31;expected[cam+0x170]=0x10040;
        assert(m.words==expected&&!hook.saved&&!hook.release_target);
        m.words[stack]=std::bit_cast<U32>(159.7f);const auto untouched=m.words;
        hook.Radius(m,regs);assert(m.words==untouched);
    }
    // An active Deku target and flower-flight takeover retain native state.
    for(U32 previous:{14u,5u}) {
        hook={};m.words[cam+0x158]=previous;m.words[cam+0x170]=0x10008;
        m.words[player+0x11db0]=0x8000;m.words[player+0xdfc]=enemy;
        hook.ChangeMode(m,cam,31);assert(!hook.release_target);
        m.words[cam+0x158]=31;const auto before=m.words;
        hook.Entry(m,regs);assert(m.words==before&&!hook.saved);
    }
    // Recorded release at 222.084: mode14/request31, camera bit3 and enemy
    // pointer linger, but player_flags/player_target both become zero.
    for(U32 form:{1u,2u,4u}) {
        hook={};m.words[player+0x1fc]=form<<24;m.words[cam+0x158]=14;
        m.words[cam+0x170]=0x10048;m.words[player+0x11db0]=0;
        m.words[player+0xdfc]=0;
        const auto before=m.words;
        assert(hook.RequestedMode(m,cam,31)==31);
        hook.ChangeMode(m,cam,31);assert(m.words==before&&hook.release_target);
        m.words[cam+0x158]=31;regs[0]=cam;hook.Entry(m,regs);
        auto expected=before;expected[cam+0x158]=31;expected[cam+0x170]=0x10040;
        assert(m.words==expected&&hook.saved&&hook.radius==240);
        for(unsigned frame=0;frame<60;++frame) {
            assert(hook.RequestedMode(m,cam,31)==31);
            hook.ChangeMode(m,cam,31);hook.Entry(m,regs);
            assert(m.words==expected&&hook.radius==240);
        }
        // A fresh enemy lock must still enter battle, and stay there with stick.
        m.words[player+0x11db0]=0x8000;m.words[player+0xdfc]=enemy;
        m.words[cam+0x170]|=8;
        assert(hook.RequestedMode(m,cam,31)==14);
        hook.ChangeMode(m,cam,14);m.words[cam+0x158]=14;
        assert(hook.RequestedMode(m,cam,31)==14);
    }
    m.words[cam+0xbc]=0;assert(hook.RequestedMode(m,cam,31)==31);
    // Replay arrival at the actual free-camera handler, including a handover
    // delayed until after the mode request and temporary loss of ground contact.
    const U32 modes=0xe00000,params=0xe00100;
    m.words[0x667894+12+8]=modes;m.words[0x667894+24+8]=modes;
    m.words[modes+4]=params;m.words[params+28]=(7u<<16)|45;
    m.words[player+0x11db0]=0;m.words[player+0x11db8]=0;
    m.words[cam+0xe8]=std::bit_cast<U32>(.015810627f);
    for(U32 f:{1u,2u,3u,4u})for(U32 kind:{2u,24u})
    for(float starting_radius:{300.f,183.42578f,172.50275f}) {
        ArrivalLens arrival;Hook orbit;m.words[player+0x1fc]=f<<24;
        m.words[modes]=(10u<<16)|kind;
        const U32 maximum=kind==2?320:300;m.words[params+8]=(2u<<16)|maximum;
        m.words[cam+0x154]=1u<<16;m.words[cam+0x158]=0;
        m.words[cam+0x170]=0;m.words[cam+0x110]=std::bit_cast<U32>(59.84f);
        m.words[game+0xa78]=1u<<16; // Soaring subcamera still active
        arrival.Observe(m,cam,31);assert(!arrival.active);
        m.words[cam+0x158]=31;arrival.Step(m,cam);assert(!arrival.active);
        m.words[game+0xa78]=0;
        for(unsigned axis=0;axis<3;++axis) {
            m.words[cam+0x50+axis*4]=0;
            m.words[cam+0x74+axis*4]=std::bit_cast<U32>(axis==2?starting_radius:0.f);
        }
        const float heights[]={0,80,74,36,44};
        const float destination=float(maximum)*(.8f*heights[f]+13.6f)*.01f;
        assert(std::abs(*arrival.NormalRadius(m,1,player,f)-destination)<.0001f);
        if(f==4&&kind==2)assert(std::abs(destination-156.16f)<.0001f);
        if(f==3&&kind==24)assert(std::abs(destination-127.2f)<.0001f);
        const float orbit_target=std::min(starting_radius,destination);
        float expected_fov=59.84f,expected_radius=starting_radius;
        regs[0]=cam;regs[4]=cam;regs[13]=stack;
        for(unsigned frame=0;frame<180;++frame) {
            m.words[player+0x94]=frame%4==0?0:0x81;
            m.words[stack]=std::bit_cast<U32>(270.f);
            auto before=m.words;
            const auto ease=[](float from,float to){return std::abs(from-to)<.1f?to:from+(to-from)*.05f;};
            expected_fov=ease(expected_fov,45.f);expected_radius=ease(expected_radius,orbit_target);
            arrival.Observe(m,cam,31);arrival.Step(m,cam);
            orbit.Entry(m,regs);orbit.Radius(m,regs);arrival.Radius(m,regs,orbit);
            before[cam+0x110]=std::bit_cast<U32>(expected_fov);
            // Arrival lens recovery must not replace the native distance.
            before[stack]=m.words[stack];assert(m.words==before);
            m.words[cam+0x170]=0x10000;
        }
        assert(expected_fov==45.f&&std::abs(arrival.radius-orbit_target)<.0001f);
        assert(arrival.active); // keep corrected baseline while orbiting
        arrival.Observe(m,cam,9);assert(!arrival.active);
    }
    // Ordinary gameplay, active lock-on, and flight never start arrival recovery.
    for(U32 f:{1u,2u,3u,4u})for(float native:{45.f,65.f,79.800156f,80.f}) {
        ArrivalLens arrival;m.words[player+0x1fc]=f<<24;
        m.words[cam+0x158]=31;m.words[cam+0x170]=0;
        m.words[cam+0x110]=std::bit_cast<U32>(native);
        const auto before=m.words;arrival.Step(m,cam);assert(!arrival.active&&m.words==before);
    }
    m.words[player+0x1fc]=3u<<24;m.words[cam+0x110]=std::bit_cast<U32>(59.84f);
    for(U32 flags:{0x100u,0x200u,0x2000u}) {
        ArrivalLens arrival;m.words[player+0x11db8]=flags;
        arrival.Step(m,cam);assert(!arrival.active);
    }
    m.words[player+0x11db8]=0;
    for(U32 phase:{8u,0x10000u}) {
        ArrivalLens arrival;m.words[cam+0x170]=phase;
        arrival.Step(m,cam);assert(!arrival.active);
    }
    ArrivalLens arrival;m.words[cam+0x170]=0;arrival.Step(m,cam);assert(arrival.active);
    // Do not apply stale recovery to a new form or a native lens override.
    m.words[player+0x1fc]=4u<<24;
    auto before=m.words;arrival.Step(m,cam);assert(!arrival.active&&m.words==before);
    arrival.Step(m,cam);assert(arrival.active);
    m.words[cam+0x110]=std::bit_cast<U32>(80.f);
    before=m.words;arrival.Step(m,cam);assert(!arrival.active&&m.words==before);
    // Ocarina capture: main FREECAMERA retains phase1 and its173.02684 orbit,
    // but the returning ITEM1 subcamera overwrites FOV45.198944 with69.160095.
    // Recover only that lens, even though there is no fresh mode31 entry.
    const U32 subcam=cam+0x198;
    m.words[subcam+0xa0]=game;m.words[subcam+0xa4]=player;
    // Disable only Restoration's movable ocarina camera. The main camera,
    // other subcamera states, and all non-free-camera requests remain native.
    for(U32 index:{0u,1u,2u})for(U32 state:{1u,0x13u,0x14u,0x15u})
    for(U32 candidate:{cam,subcam})for(U32 requested:{0u,5u,9u,10u,14u,31u}) {
        m.words[game+0xa78]=index<<16;m.words[subcam+0x154]=state<<16;
        const auto before=m.words;
        const bool ocarina=index==1&&state==0x13&&candidate==subcam&&requested==31;
        assert(hook.RequestedMode(m,candidate,requested)==(ocarina?0u:requested));
        assert(m.words==before);
    }
    m.words[game+0xa78]=1u<<16;m.words[subcam+0x154]=0x13u<<16;
    m.words[subcam+0xa0]=0;
    assert(hook.RequestedMode(m,subcam,31)==31);
    m.words[subcam+0xa0]=game;
    m.words[game+0x13c]=0;
    assert(hook.RequestedMode(m,subcam,31)==31);
    m.words[game+0x13c]=0x400;
    for(U32 f:{1u,2u,3u,4u}) {
        OcarinaReturnLens lens;m.words[player+0x1fc]=f<<24;
        m.words[player+0x11db8]=0;m.words[cam+0x154]=1u<<16;
        m.words[cam+0x158]=31;m.words[cam+0x170]=0x10000;
        m.words[cam+0x110]=std::bit_cast<U32>(45.198944f);
        m.words[cam+0xf0]=std::bit_cast<U32>(173.02684f);
        m.words[game+0xa78]=1u<<16;m.words[subcam+0x154]=0x13u<<16;
        auto before=m.words;lens.Observe(m,cam,31);
        assert(lens.pending&&!lens.active&&m.words==before);
        m.words[game+0xa78]=0;m.words[cam+0x110]=std::bit_cast<U32>(69.160095f);
        float expected=69.160095f;
        for(unsigned frame=0;frame<180;++frame) {
            before=m.words;lens.Observe(m,cam,31);lens.Step(m,cam);
            expected=expected-45.f<.1f?45.f:expected+(45.f-expected)*.05f;
            before[cam+0x110]=std::bit_cast<U32>(expected);
            assert(m.words==before); // Lens only: no radius/eye/target changes.
        }
        assert(expected==45.f&&!lens.active&&!lens.pending);
        assert(std::abs(std::bit_cast<float>(MM3D::CameraLive::ProjectionValue(
            m.words[cam+0x110],70.f))-70.f)<.001f);
    }
    // Do not recover unrelated subcameras, active targets, flight, changed
    // identities or a later script that supplies a different lens.
    for(unsigned scenario=0;scenario<8;++scenario) {
        OcarinaReturnLens lens;m.words[player+0x1fc]=4u<<24;m.words[player+0x11db8]=0;
        m.words[cam+0x158]=31;m.words[cam+0x170]=0x10000;
        m.words[game+0xa78]=1u<<16;m.words[subcam+0x154]=0x13u<<16;
        if(scenario==0)m.words[subcam+0x154]=0x14u<<16;
        if(scenario==1)m.words[game+0xa78]=2u<<16;
        if(scenario==2)m.words[cam+0x158]=0;
        if(scenario==5)m.words[player+0x1fc]=3u<<24;
        lens.Observe(m,cam,31);
        m.words[game+0xa78]=0;m.words[cam+0x158]=31;
        m.words[cam+0x110]=std::bit_cast<U32>(69.160095f);
        if(scenario==3)m.words[cam+0x170]|=8;
        if(scenario==4)m.words[player+0x1fc]=1u<<24;
        if(scenario==5)m.words[player+0x11db8]=0x200;
        if(scenario==6)lens.Observe(m,cam,9);
        if(scenario==7){lens.Step(m,cam);assert(lens.active);m.words[cam+0x110]=std::bit_cast<U32>(60.f);}
        const auto before=m.words;lens.Step(m,cam);
        assert(m.words==before&&!lens.pending&&!lens.active);
    }
    assert(Instruction(PC,Word)==(0xef000000|SVC)&&Instruction(PC,0)==0);
    if(argc>1){
        Memory code;std::ifstream file(argv[1],std::ios::binary);assert(file);
        U32 word,address=0x100000;while(file.read(reinterpret_cast<char*>(&word),4)){code.words[address]=word;address+=4;}
        assert(CodeMatches(code));
        // Verify aiming IDs against native NORMAL0/DUNGEON0/DUNGEON2 tables:
        // Pictograph, Bow and Hookshot all select the SUBJECT camera handler.
        for(U32 setting:{1u,17u,50u}) {
            auto modes=code.Read(0x667894+setting*12+8);assert(modes);
            for(U32 mode:{6u,12u,13u})assert(code.Read(*modes+mode*8)==0x90011u);
        }
        for(U32 pc:{PC,EntryPC,RadiusPC,LockPC}){code.words[pc]^=1;assert(!CodeMatches(code));code.words[pc]^=1;}
    }
}
