// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <limits>
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
    Check(engine.Tick(m,55.f).active && m.writes==0,"inspection does not modify camera state");
    {
        Memory scripted;
        constexpr U32 base=0x08001000;
        // Actual vertical lens terms from the Song of Time falling sequence.
        // Reconstruct their native FOV without changing any projection values.
        for(const float lens : {2.423694849f,.9494589567f,.5159825087f,
                               .1044527665f,.04366129637f,.06434336305f}) {
            const float fov=2.f*std::atan(1.f/lens)*180.f/3.14159265358979323846f;
            scripted.Float(base+0x18,fov);scripted.Float(base+0x4c,fov);
            scripted.Float(base+0xd0,lens);
            const auto snapshot=engine.Tick(scripted,45.f);
            Check(snapshot.valid,"Song of Time wide-angle camera remains available for interpolation");
            Check(std::abs(snapshot.matrix_fov-fov)<.001f && snapshot.fov==fov,
                  "native cutscene FOV is reported without clamping");
        }
        Check(scripted.writes==0,"wide-angle snapshot validation is read-only");
        for(const U32 offset : {0x18u,0x4cu}) {
            for(const float invalid : {0.f,-1.f,180.f,181.f,
                    std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
                scripted.Camera(base);scripted.Float(base+offset,invalid);
                Check(!engine.Tick(scripted,45.f).valid,"degenerate and nonfinite FOV still rejected");
            }
        }
        scripted.Camera(base);scripted.Float(base+0x18,175.f);
        scripted.Float(base+0x1c,0.f);
        Check(!engine.Tick(scripted,45.f).valid,"wide-angle camera still requires valid clipping");
        scripted.Camera(base);scripted.Float(base+0x18,175.f);
        scripted.Float(base+0xd0,-.04366129637f);
        Check(!engine.Tick(scripted,45.f).valid,"wide-angle camera still requires positive projection");
    }
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
