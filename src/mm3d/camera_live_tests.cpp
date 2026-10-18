// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <map>
#include <stdexcept>
#include "mm3d/camera_live.h"
using namespace MM3D::CameraLive;
namespace {
void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct Memory {
    std::map<U32,U32> words;
    unsigned writes{};
    std::optional<U32> Read(U32 at) {
        auto i=words.find(at); if(i==words.end()) return {}; return i->second;
    }
    bool Write(U32 at,U32 value) {
        if(!words.contains(at)) return false;
        words[at]=value; ++writes; return true;
    }
    void Float(U32 at,float v) { words[at]=std::bit_cast<U32>(v); }
    float Float(U32 at) { return std::bit_cast<float>(words.at(at)); }
    void Camera(U32 base) {
        words[CameraPointer]=base;
        words[base+4]=words[base+12]=0; words[base+8]=400; words[base+16]=240;
        Float(base+0x18,45); Float(base+0x4c,60); Float(base+0x1c,5); Float(base+0x20,20000);
        for(unsigned i=0;i<6;++i) Float(base+0x28+4*i,float(i));
        Float(base+0xd0,2.41421356f); Float(base+0xbc,2.41421356f/(400.f/240.f));
        Float(base+0x110,2.41421356f); Float(base+0xfc,2.41421356f/(400.f/240.f));
    }
    Memory() {
        words={{0x18d3cc,0xe92d4010}, {0x18d414,0xed940a06}, {0x18d428,0xed940a06},
            {0x18d448,0xed940a13}, {0x1dd0d8,0x00656b44}, {0x18d524,0x3c8efa35}};
        Camera(0x08001000);
    }
};
}
int main() {
 try {
    Memory m; Engine engine;
    Service(m,nullptr,12345);
    const auto sampled=GetSnapshot();
    Check(sampled.valid && sampled.sampled_at_ms==12345 && sampled.eye[0]==0 && sampled.target[0]==3,
          "CPU camera snapshot publishes its sample time and direct eye/target readings");
    Check(m.writes==0,"timestamped camera sampling is read-only");
    m.words[0x72f5c0]=0x08010000;m.words[0x0801016c]=0x08100000;
    m.words[0x0810013c]=0x0402;m.words[0x08100a78]=0;
    const U32 cam=0x08100408;
    m.words[cam+0xa0]=0x08100000;m.words[cam+0x154]=1u<<16;
    for(U32 mode:{3u,4u,6u,12u,13u}) {
        m.words[cam+0x158]=mode;
        const auto aim=engine.Tick(m,55.f);
        Check(aim.camera_context_valid&&aim.NativeAiming()&&aim.camera_mode==mode,
              "bubble, targeted bow, subject and bow modes preserve native camera");
    }
    m.words[cam+0x158]=31;
    const auto free=engine.Tick(m,55.f);
    Check(free.camera_context_valid&&!free.NativeAiming()&&free.CameraContext()!=0,
          "free camera remains eligible for interpolation");
    m.words[cam+0xa0]=0;
    Check(!engine.Tick(m,55.f).camera_context_valid,"reject camera with wrong owner");
    Check(m.writes==0,"camera mode sampling never changes guest memory");

    Check(engine.Tick(m,55.f).active && m.writes==0,"inspection does not modify camera state");
    m.words[CameraPointer]=0;
    Check(engine.Tick(m,55.f).active,"projection hook does not wait for published camera");
    m.words[0x18d414]=0;
    Check(!engine.Tick(m,55.f).active,"unsupported code rejected");
    for(float target : {46.f,55.f,65.f}) {
        const float result=std::bit_cast<float>(ProjectionValue(std::bit_cast<U32>(45.f),target));
        Check(std::abs(result-target)<.0001f,"normal gameplay maps to selected FOV");
    }
    Check(ProjectionInstruction(0x18d448,0xed940a13)==0xed940a13,"auxiliary projection untouched");
    Check(ProjectionInstruction(0x18d414,0)==0,"unexpected instruction never replaced");
    const auto native=std::bit_cast<U32>(40.f);
    Check(ProjectionValue(native,45.f)==native,"reset exactly preserves native FOV");
    std::cout<<"PASS projection selection, code guards, reset and no pointer dependency\n";
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
