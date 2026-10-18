// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/camera_distance.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
using namespace MM3D::CameraDistance;
struct Memory {
    std::unordered_map<U32,U32> words;unsigned writes{};
    std::optional<U32> Read(U32 p){auto i=words.find(p);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 p,U32 value){assert(words.contains(p));words[p]=value;++writes;}
};
constexpr U32 Root=0x08000000,Game=0x08100000,Player=0x08200000,Cam=Game+0x408,Stack=0x08300000,Modes=0x08400000,Params=0x08500000;
U32 Bits(float f){return std::bit_cast<U32>(f);}
Memory Fixture(U32 mode=0,U32 form=4) {
    Memory m;m.words={{0x72f5c0,Root},{Root+0x16c,Game},{Game+0x13c,0x402},{Game+0xa78,0},
        {Game+0x20dc,Player},{Cam+0xa0,Game},{Cam+0xa4,Player},{Cam+0x154,1u<<16},{Cam+0x158,mode},
        {Player,0x20000},{Player+0x1fc,form<<24},{Player+0x11db0,0},{Player+0x11db8,0},
        {Cam+4,Bits(150.f)},{Cam+8,Bits(300.f)},{Cam+0xf0,Bits(250.f)},{Stack,Bits(200.f)},
        {0x667894+12+8,Modes},{Modes,0xa0002},{Modes+4,Params},{Params+8,0x20000|300}};return m;
}
int main(int argc,char** argv) {
    assert(argc>=2);
    for(int file=1;file<argc;++file) {
        std::ifstream f(argv[file],std::ios::binary);assert(f);std::vector<unsigned char> data{std::istreambuf_iterator<char>(f),{}};
        Memory code;
        for(auto [start,size]:{std::pair<U32,U32>{0x447fe8,0x1318},{0x3e3e54,0xaa0}})for(U32 at=start;at<start+size;at+=4) {
            assert(at-0x100000+4<=data.size());U32 word=0;
            for(unsigned byte=0;byte<4;++byte)word|=U32(data[at-0x100000+byte])<<(byte*8);
            code.words[at]=word;
        }
        for(unsigned i=0;i<Loads.size();++i) {
            const auto& load=Loads[i];assert(code.Read(load.pc)==load.word&&CodeMatches(code,load.pc));
            assert(Instruction(load.pc,load.word)==(0xef000000|FirstSVC+i));
            code.words[load.pc]^=1;assert(!CodeMatches(code,load.pc));code.words[load.pc]^=1;
            assert(Instruction(load.pc,load.word^1)==(load.word^1));
        }
    }
    for(unsigned hook=0;hook<Loads.size();++hook) {
        auto m=Fixture();const auto original=m.words;
        const auto& load=Loads[hook];std::array<U32,16> r{};r[4]=Cam;
        if(load.count==2)r[load.pointer_register]=Cam+4;
        std::array<U32,64> fp{};fp.fill(0x12345678);
        // Native 1x replays the displaced load, touching only its FPU outputs.
        auto expected=fp;
        for(unsigned i=0;i<load.count;++i)expected[load.destination+i]=*m.Read(r[load.pointer_register]+load.offset+4*i);
        ApplyLoad(m,r,fp,hook,1.f);assert(fp==expected&&m.words==original&&m.writes==0);
        // Scaling remains transient over thousands of frames. No camera position,
        // target, collision output, cached parameter or FOV writes are allowed.
        for(unsigned frame=0;frame<1000;++frame) {
            ApplyLoad(m,r,fp,hook,2.f);
            for(unsigned i=0;i<load.count;++i)assert(fp[load.destination+i]==Bits(std::bit_cast<float>(expected[load.destination+i])*2.f));
        }
        assert(m.words==original&&m.writes==0);
        ApplyLoad(m,r,fp,hook,1.f);assert(fp==expected);
        for(unsigned scenario=0;scenario<7;++scenario) {
            auto bad=m;
            switch(scenario) {
            case 0:bad.words[Game+0xa78]=1u<<16;break; // subcamera/cutscene
            case 1:bad.words[Cam+0x158]=11;break; // dialogue
            case 2:bad.words[Cam+0x158]=6;break; // first person
            case 3:bad.words[Cam+0x154]=9u<<16;break; // scripted setting
            case 4:bad.words[Player+0x11db0]=0x800000;break; // mounted
            case 5:bad.words[Cam+0xa4]=Player+4;break; // wrong target
            case 6:bad.words[Game+0x13c]=0;break; // non-play state
            }
            ApplyLoad(bad,r,fp,hook,2.f);assert(fp==expected&&bad.writes==0);
        }
    }
    for(U32 form:{0u,1u,2u,3u,4u}) {
        auto m=Fixture(31,form);Orbit orbit;MM3D::FormCamera::Hook native;
        native.camera=Cam;native.player=Player;native.form=form;native.radius=200;native.saved=true;
        std::array<U32,16> r{};r[4]=Cam;r[13]=Stack;
        auto original=m.words;orbit.Apply(m,r,native,1.f);assert(m.words==original&&m.writes==0);
        const float baseline=200.f;
        for(unsigned frame=0;frame<1000;++frame) {
            // Native/form hooks may feed the previous enlarged distance back;
            // the user factor always derives from untouched form parameters.
            orbit.Apply(m,r,native,2.f);assert(m.Read(Stack)==Bits(baseline*2.f));
        }
        orbit.Apply(m,r,native,3.f);assert(m.Read(Stack)==Bits(baseline*3.f));
        orbit.Apply(m,r,native,1.f);assert(m.Read(Stack)==Bits(baseline)&&native.radius==baseline);
        original[Stack]=Bits(baseline);assert(m.words==original);
        const auto writes=m.writes;orbit.Apply(m,r,native,1.f);assert(m.writes==writes);
        m.words[Cam+0x158]=6;original=m.words;orbit.Apply(m,r,native,2.f);assert(m.words==original);
    }
    // Normal -> orbit handover must not multiply an already enlarged radius.
    {
        auto m=Fixture();Orbit orbit;MM3D::FormCamera::Hook form;
        std::array<U32,16> r{};std::array<U32,64> fp{};r[4]=Cam;r[13]=Stack;
        ApplyLoad(m,r,fp,0,2.f,&orbit);
        m.words[Cam+0x158]=31;m.words[Stack]=Bits(400.f);
        form.saved=true;form.camera=Cam;form.player=Player;form.radius=400.f;
        orbit.Apply(m,r,form,2.f);assert(m.Read(Stack)==Bits(400.f));
        orbit.Apply(m,r,form,3.f);assert(m.Read(Stack)==Bits(600.f));
        orbit.Apply(m,r,form,1.f);assert(m.Read(Stack)==Bits(200.f)&&form.radius==200.f);
        orbit.Observe(Cam,6);assert(orbit.normal_camera==0&&orbit.baseline==0);
        m.words[Stack]=Bits(50.f);orbit.Apply(m,r,form,2.f);assert(m.Read(Stack)==Bits(100.f));
    }
    // Recover a shortened orbit without a scene change or resetting the user's
    // multiplier. Rebuild from native form parameters, retaining collision output.
    for(U32 form:{1u,2u,3u,4u}) {
        auto m=Fixture(31,form);Orbit orbit;MM3D::FormCamera::Hook native;
        std::array<U32,16> r{};r[4]=Cam;r[13]=Stack;
        m.words[Cam+0x5c]=Bits(25.f); // wall-adjusted eye must remain untouched
        orbit.camera=Cam;orbit.player=Player;orbit.form=form;orbit.setting=1;
        orbit.baseline=35;orbit.applied=true;
        native.saved=true;native.camera=Cam;native.player=Player;native.form=form;native.radius=35;
        MM3D::FormCamera::ArrivalLens params;
        const auto expected=params.NormalRadius(m,1,Player,form);assert(expected);
        const auto before=m.words;
        RequestReset();assert(orbit.Recover(m,r,native));assert(!reset_requested);
        orbit.Apply(m,r,native,1.66f);
        assert(m.Read(Stack)==Bits(*expected*1.66f));
        assert(native.saved&&native.radius==*expected&&orbit.baseline==*expected);
        for(const auto& [at,value]:before)if(at!=Stack)assert(m.Read(at)==value);
        for(unsigned frame=0;frame<100;++frame) {
            orbit.Apply(m,r,native,1.66f);assert(m.Read(Stack)==Bits(*expected*1.66f));
        }
        // Aiming/cutscenes defer the request and never receive a reset write.
        m.words[Cam+0x158]=6;const auto special=m.words;
        RequestReset();assert(!orbit.Recover(m,r,native)&&reset_requested);
        assert(m.words==special);
        m.words[Cam+0x158]=31;assert(orbit.Recover(m,r,native));
        orbit.Apply(m,r,native,1.f);assert(m.Read(Stack)==Bits(*expected));
    }
    for(U32 bits:{Bits(-0.f),0x7fc12345u,0x7f800000u})assert(Scale(bits,1.f)==bits&&Scale(bits,2.f)==bits);
    assert(Scale(Bits(100.f),100.f)==Bits(300.f));assert(Scale(Bits(100.f),0.f)==Bits(50.f));
    std::cout<<"Camera distance: exact code guards, native 1x, isolated radius operands, repeated-frame stability, live reset, all forms and special-camera exclusions passed\n";
}
