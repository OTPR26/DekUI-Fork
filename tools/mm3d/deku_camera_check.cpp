// clang++ -std=c++20 -Isrc tools/mm3d/deku_camera_check.cpp -o /tmp/deku-camera-check
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include "mm3d/form_camera.h"
using MM3D::CameraLive::U32;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 p){auto i=words.find(p);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 p,U32 v){assert(words.contains(p));words[p]=v;}
};
static void FreeCameraChecks() {
    namespace F=MM3D::FormCamera;namespace C=MM3D::CameraLive;
    const U32 ctx=0x900000,game=0xa00000,actor=0xb00000,cam=game+0x408,modes=0xc00000;
    Memory base;
    base.words={{0x72f5c0,ctx},{ctx+0x16c,game},{game+0x13c,0x400},{game+0xa78,0},
        {game+0x20dc,actor},{game+0x148,1},{actor,0x20000},{actor+0x1fc,3u<<24},
        {actor+0xdfc,0},{actor+0x11db0,0},{actor+0x11db8,0x2000},{actor+0x94,0},
        {cam+0xa0,game},{cam+0xa4,actor},{cam+0x158,31},{cam+0x170,0x10000},
        {cam+0x110,std::bit_cast<U32>(79.82338f)},{cam+0xf0,std::bit_cast<U32>(134.71121f)}};
    for(U32 setting:{1u,50u})base.words[0x667894+setting*12+8]=modes;
    for(U32 mode:{0u,1u,5u}) {
        const U32 p=modes+0x100+mode*0x40;
        base.words[modes+mode*8]=0xa0002;base.words[modes+mode*8+4]=p;
        base.words[p+8]=0x20140;base.words[p+28]=0x70000|(mode==5?80:45);
    }
    const auto lens=[&](Memory& m){return std::bit_cast<float>(*m.Read(cam+0x110));};
    for(U32 setting:{1u,50u})for(float zoom:{45.f,65.f,70.f}) {
        Memory m=base;m.words[cam+0x154]=setting<<16;F::DekuFreeCamera correction;
        const auto before=m.words;correction.Step(m,cam);assert(correction.armed&&m.words==before);
        // Captured free-camera requests grow to227.5; native flight tops out
        // at135.68. Replace only S0 and leave all guest memory untouched.
        for(float request:{189.76141f,214.f,227.49765f,100.f}) {
            std::array<U32,64> fp;fp.fill(0x12345678);fp[0]=std::bit_cast<U32>(request);
            auto expected=fp;expected[0]=std::bit_cast<U32>(std::min(request,135.68f));
            F::DekuFreeCamera::Distance(m,cam,fp);
            assert(std::abs(std::bit_cast<float>(fp[0])-std::bit_cast<float>(expected[0]))<.0001f);
            expected[0]=fp[0];assert(fp==expected&&m.words==before);
        }
        // Runtime order: replay the displaced S1 load, then adjust S0 before
        // native smoothing. The recorded ~134.7 orbit must not jump to168.
        std::array<U32,64> fp{};fp[0]=std::bit_cast<U32>(227.49765f);
        F::Hook hook;hook.Distance(m,cam,fp);F::DekuFreeCamera::Distance(m,cam,fp);
        const float previous=std::bit_cast<float>(fp[1]),target=std::bit_cast<float>(fp[0]);
        assert(previous==134.71121f&&previous+(target-previous)*.38902783f<135.68f);
        // Reproduce fall while remaining in mode31, then land mid-recovery.
        m.words[actor+0x11db8]=0;
        float expected=79.82338f;
        for(unsigned update=0;update<130;++update) {
            if(update==14)m.words[actor+0x94]=1;
            const auto old=m.words;auto wanted=old;
            expected=expected-45<.1f?45:expected+(45-expected)*.05f;
            correction.Step(m,cam);wanted[cam+0x110]=std::bit_cast<U32>(expected);
            assert(m.words==wanted&&std::abs(lens(m)-expected)<.0001f);
            if(update==0)assert(lens(m)<79&&correction.recovering); // before landing
            const float output=std::bit_cast<float>(C::ProjectionValue(*m.Read(cam+0x110),zoom));
            assert(output>=zoom-.0001f);
            if(expected==45)assert(std::abs(output-zoom)<.0001f);
            // Fall/ground distance remains the original native request.
            std::array<U32,64> fp{};fp[0]=std::bit_cast<U32>(227.49765f);auto original=fp;
            F::DekuFreeCamera::Distance(m,cam,fp);assert(fp==original);
        }
        assert(lens(m)==45&&!correction.armed);
        // Ordinary hops cannot re-arm lens correction.
        m.words[actor+0x94]=0;m.words[cam+0x110]=std::bit_cast<U32>(60.f);
        auto unchanged=m.words;correction.Step(m,cam);assert(m.words==unchanged);
        // Relaunch can arm again; leaving free camera disarms immediately.
        m.words[actor+0x11db8]=0x200;correction.Step(m,cam);assert(correction.armed);
        correction.Observe(cam,1);assert(!correction.armed);
    }
    // Ownership, target, form, flower, malformed table and invalid lens guards.
    for(unsigned bad=0;bad<14;++bad) {
        Memory m=base;m.words[cam+0x154]=1u<<16;F::DekuFreeCamera correction;
        correction.Step(m,cam);assert(correction.armed);
        switch(bad) {
        case 0:m.words[actor+0x1fc]=4u<<24;break;
        case 1:m.words[game+0xa78]=1u<<16;break;
        case 2:m.words[cam+0xa0]=game+4;break;
        case 3:m.words[cam+0x170]|=8;break;
        case 4:m.words[actor+0xdfc]=0xdeadbeef;break;
        case 5:m.words[actor+0x11db0]=0x8000;break;
        case 6:m.words[actor+0x11db8]=0x100;break;
        case 7:m.words[cam+0x158]=5;break; // no-input flight stays native
        case 8:m.words[modes+5*8]=0xa0099;break;
        case 9:m.words[modes+0x100+5*0x40+8]=0x30000;break;
        case 10:m.words.erase(actor+0x94);break;
        case 11:m.words[actor+0x11db0]=0x800000;break;
        case 12:m.words[actor+0x94]=1;break;
        case 13:m.words[actor+0x11db8]=0;break;
        }
        const auto before=m.words;std::array<U32,64> fp{};fp[0]=std::bit_cast<U32>(227.f);auto expected=fp;
        F::DekuFreeCamera::Distance(m,cam,fp);assert(fp==expected&&m.words==before);
        if(bad<8||bad==10||bad==11){correction.Step(m,cam);assert(!correction.armed&&m.words==before);}
    }
    for(unsigned change=0;change<5;++change) {
        Memory m=base;m.words[cam+0x154]=1u<<16;F::DekuFreeCamera correction;
        correction.Step(m,cam);m.words[actor+0x11db8]=0;
        switch(change) {
        case 0:m.words[game+0x148]=2;break;
        case 1:m.words[cam+0x154]=50u<<16;break;
        case 2:m.words[cam+0x110]=0x7fc00000;break;
        case 3:m.words[cam+0x110]=std::bit_cast<U32>(90.f);break;
        case 4:correction.Step(m,cam);m.words[cam+0x110]=std::bit_cast<U32>(60.f);break;
        }
        const auto before=m.words;correction.Step(m,cam);assert(m.words==before&&!correction.armed);
    }
}
int main(int argc,char** argv) {
    namespace C=MM3D::CameraLive;
    namespace F=MM3D::FormCamera;
    Memory m;
    FreeCameraChecks();
    const U32 context=0x900000,game=0xa00000,player=0xb00000,projection=0xc00000;
    const U32 cam=game+0x408,stack=0xd00000;
    m.words[0x72f5c0]=context;m.words[context+0x16c]=game;
    m.words[game+0x13c]=0x400;m.words[game+0xa78]=0;m.words[game+0x20dc]=player;
    m.words[player]=0x20000;m.words[player+0x1fc]=3u<<24;
    m.words[player+0xdfc]=0;m.words[player+0x11db0]=0;
    m.words[cam+0xa0]=game;m.words[cam+0xa4]=player;m.words[cam+0xbc]=0;
    m.words[cam+0x110]=std::bit_cast<U32>(45.f);
    m.words[C::CameraPointer]=projection;
    m.words[projection+4]=0;m.words[projection+8]=400;
    m.words[projection+12]=0;m.words[projection+16]=240;
    // Walk, burrow, launch, air stick, fall, land, target, and orbit again.
    // All remaining hooks must leave native Deku modes and distance untouched.
    struct Frame {U32 mode,flags,ground;float radius,lens;};
    constexpr Frame frames[]{
        {0,0,1,180,45},{15,0x100,1,160,80},{5,0x200,0,135,80},
        {31,0x2000,0,214,80},{1,0,0,150,70},{0,0,1,120,45},
        {9,0,1,85,45},{31,0,1,180,45}};
    for(float target:{45.f,65.f,70.f})for(U32 setting:{1u,50u}) {
        C::SetFov(target);assert(C::desired_fov==target);
        assert(std::abs(std::bit_cast<float>(C::ProjectionValue(std::bit_cast<U32>(45.f),target))-target)<.0001f);
        F::Hook hook;F::ArrivalLens arrival;F::OcarinaReturnLens ocarina;
        m.words[cam+0x154]=setting<<16;
        for(auto frame:frames) {
            m.words[player+0x11db8]=frame.flags;m.words[player+0x94]=frame.ground;
            m.words[cam+0x158]=frame.mode;m.words[cam+0x170]=0;
            m.words[cam+0xf0]=m.words[stack]=std::bit_cast<U32>(frame.radius);
            const auto before=m.words;
            std::array<U32,16> regs{};regs[0]=cam;regs[1]=frame.mode;regs[4]=cam;regs[13]=stack;
            assert(hook.RequestedMode(m,cam,frame.mode)==frame.mode);
            hook.ChangeMode(m,cam,frame.mode);arrival.Observe(m,cam,frame.mode);ocarina.Observe(m,cam,frame.mode);
            hook.Entry(m,regs);arrival.Step(m,cam);ocarina.Step(m,cam);
            hook.Radius(m,regs);arrival.Radius(m,regs,hook);
            assert(!hook.saved&&m.words==before);
            // No flight option/session replaces or delays the user's zoom.
            const float configured=C::ConfiguredFov(m,projection);assert(configured==target);
            const U32 native=std::bit_cast<U32>(frame.lens);
            const U32 result=C::ProjectionValue(native,configured);
            if(target==45)assert(result==native);
            else {
                constexpr float rad=3.14159265358979323846f/360.f;
                const float scale=std::tan(target*rad)/std::tan(45.f*rad);
                assert(std::abs(std::tan(std::bit_cast<float>(result)*rad)/std::tan(frame.lens*rad)-scale)<.00001f);
            }
            assert(m.words==before);
        }
    }
    // The rejected normal-camera math hook is no longer translated.
    assert(F::Instruction(0x1b04a0,0xe1a06001)==0xe1a06001);
    // Preserve the separate scripted-camera framing rule.
    for(U32 setting:{9u,10u}) {
        m.words[cam+0x154]=setting<<16;C::SetFov(65);
        assert(C::ConfiguredFov(m,projection)==51);
        assert(C::ConfiguredFov(m,projection+4)==65);
    }
    if(argc>1) {
        Memory binary;std::ifstream code(argv[1],std::ios::binary);assert(code);
        U32 word;for(U32 at=0x100000;code.read(reinterpret_cast<char*>(&word),4);at+=4)binary.words[at]=word;
        assert(C::ChangeModeCodeMatches(binary));
        assert(F::CodeMatches(binary));
        for(U32 setting:{1u,50u}) {
            assert(F::DekuFreeCamera::Parameter(binary,setting,5,2)==320.f);
            assert(F::DekuFreeCamera::Parameter(binary,setting,5,7)==80.f);
            assert(F::DekuFreeCamera::Parameter(binary,setting,1,7)==45.f);
            assert(F::DekuFreeCamera::Parameter(binary,setting,0,7)==45.f);
        }
        binary.words[C::ChangeModePC]^=1;assert(!C::ChangeModeCodeMatches(binary));
    }
    C::SetFov(45);
    std::cout<<"Native Deku camera, free-camera flight correction, and user zoom checks passed\n";
}
