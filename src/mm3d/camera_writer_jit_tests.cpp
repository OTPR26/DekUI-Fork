// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include "mm3d/camera_instruction_probe.h"
#include "mm3d/camera_live.h"

namespace {
void Check(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
struct Callbacks : Dynarmic::A32::UserCallbacks {
    std::array<std::uint8_t, 0x4000> memory{};
    MM3D::CameraWriter::Instruction instruction;
    std::vector<MM3D::CameraWriter::Instruction> writes;
    Dynarmic::A32::Jit* jit{};
    bool instrument{};
    bool live_camera{};
    bool stopped{};
    std::uint64_t ticks{1000};
    template<class T> T Read(std::uint32_t p) {
        Check(std::uint64_t(p)+sizeof(T)<=memory.size(), "read bounds");
        T value; std::memcpy(&value,memory.data()+p,sizeof(T)); return value;
    }
    template<class T> void Write(std::uint32_t p,T value) {
        Check(std::uint64_t(p)+sizeof(T)<=memory.size(), "write bounds");
        std::memcpy(memory.data()+p,&value,sizeof(T));
        if(instrument) writes.push_back(instruction);
    }
    std::uint8_t MemoryRead8(std::uint32_t p) override { return Read<std::uint8_t>(p); }
    std::uint16_t MemoryRead16(std::uint32_t p) override { return Read<std::uint16_t>(p); }
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t p) override {
        const auto value=Read<std::uint32_t>(p);
        if(live_camera && (p==0x1004 || p==0x100c))
            return MM3D::CameraLive::ProjectionInstruction(p==0x1004 ? 0x18d414 : 0x18d428,value);
        return value;
    }
    std::uint32_t MemoryRead32(std::uint32_t p) override { return Read<std::uint32_t>(p); }
    std::uint64_t MemoryRead64(std::uint32_t p) override { return Read<std::uint64_t>(p); }
    void MemoryWrite8(std::uint32_t p,std::uint8_t v) override { Write(p,v); }
    void MemoryWrite16(std::uint32_t p,std::uint16_t v) override { Write(p,v); }
    void MemoryWrite32(std::uint32_t p,std::uint32_t v) override { Write(p,v); }
    void MemoryWrite64(std::uint32_t p,std::uint64_t v) override { Write(p,v); }
    void InterpreterFallback(std::uint32_t, size_t) override { throw std::runtime_error("fallback"); }
    void CallSVC(std::uint32_t svc) override {
        if(live_camera && svc==MM3D::CameraLive::ProjectionSVC) {
            jit->ExtRegs()[0]=MM3D::CameraLive::ProjectionValue(Read<std::uint32_t>(jit->Regs()[4]+0x18),
                MM3D::CameraLive::desired_fov.load());
        } else { stopped=true; jit->HaltExecution(); }
    }
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override { throw std::runtime_error("exception"); }
    void AddTicks(std::uint64_t n) override { ticks = n>ticks ? 0 : ticks-n; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
};
void RunLiveCamera() {
    Callbacks cb; cb.live_camera=true;
    const std::uint32_t code[]{0xe5840018,0xed940a06,0xed820a00,0xed940a06,0xed820a01,0xef000000};
    std::memcpy(cb.memory.data()+0x1000,code,sizeof(code));
    auto pages=std::make_unique<std::array<std::uint8_t*,Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
    for(unsigned i=0;i<4;++i) (*pages)[i]=cb.memory.data()+i*0x1000;
    Dynarmic::A32::UserConfig config; config.callbacks=&cb; config.page_table=pages.get();
    config.code_cache_size=16*1024*1024;
    Dynarmic::A32::Jit jit(config); cb.jit=&jit;
    for(float configured : {55.f,60.f,45.f}) for(float native : {45.f,40.f,60.f}) {
        MM3D::CameraLive::SetFov(configured);
        jit.Regs()[0]=std::bit_cast<std::uint32_t>(native);
        jit.Regs()[4]=native==40.f ? 0x2100 : 0x2000;
        jit.Regs()[2]=0x3000; jit.Regs()[15]=0x1000;
        jit.ExtRegs()[1]=0x3f123456; jit.Regs()[14]=0x12345678;
        jit.SetCpsr(0x60000010); cb.ticks=1000; cb.stopped=false;
        jit.Run();
        Check(cb.stopped,"projection hook completes with normal JIT and fast memory");
        const auto expected=MM3D::CameraLive::ProjectionValue(std::bit_cast<std::uint32_t>(native),configured);
        Check(cb.Read<std::uint32_t>(0x3000)==expected && cb.Read<std::uint32_t>(0x3004)==expected,
            "both projections use chosen zoom on first update, with no published camera pointer");
        Check(cb.Read<float>(jit.Regs()[4]+0x18)==native,"native camera field remains untouched");
        Check(jit.ExtRegs()[1]==0x3f123456 && jit.Regs()[14]==0x12345678 &&
            (jit.Cpsr()&0xf0000000)==0x60000000,"hook preserves other registers and condition flags");
    }
}
void Run(bool thumb) {
    std::array<std::uint8_t,0x4000> expected{};
    std::array<std::uint32_t,16> expected_regs{};
    for(bool instrument : {false,true}) {
        Callbacks cb;
        // Change LR/SP inside the block, then perform two stores. A stale backing
        // register snapshot would give the wrong LR even if its PC looked plausible.
        const std::uint32_t arm[]{0xe3a0e080,0xe28dd004,0xe5810000,0xe28ee004,0xe5810004,
            0xe3520000,0x15810008,0x0581000c,0xe2533001,0x1afffffd,0xef000000};
        const std::uint16_t th[]{0x2280,0x4696,0xb001,0x6008,0x3204,0x4696,0x6048,
            0x2a84,0xbf18,0x6088,0xbf08,0x60c8,0x3b01,0xd1fd,0xdf00};
        if(thumb) std::memcpy(cb.memory.data()+0x1000,th,sizeof(th));
        else std::memcpy(cb.memory.data()+0x1000,arm,sizeof(arm));
        auto pages=std::make_unique<std::array<std::uint8_t*,Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>>();
        for(unsigned i=0;i<4;++i) (*pages)[i]=cb.memory.data()+i*0x1000;
        cb.instrument=instrument;
        Dynarmic::A32::UserConfig config;
        config.callbacks=&cb; config.code_cache_size=16*1024*1024;
        if(!instrument) config.page_table=pages.get();
        Dynarmic::A32::Jit jit(config); cb.jit=&jit;
        jit.Regs()[0]=0x401a827a; jit.Regs()[1]=0x2000; jit.Regs()[3]=3;
        jit.Regs()[13]=0x3000; jit.Regs()[14]=0xdeadbeef; jit.Regs()[15]=0x1000;
        jit.SetCpsr(thumb ? 0x30 : 0x10);
        if(instrument) {
            for(unsigned i=0;i<128 && !cb.stopped;++i)
                MM3D::CameraWriter::StepWithSnapshot(jit,cb.instruction);
            Check(cb.stopped,"conditional instructions/loops make progress within step bound");
        } else jit.Run();
        Check(cb.Read<std::uint32_t>(0x2008)==0 && cb.Read<std::uint32_t>(0x200c)==0x401a827a,
            "failed conditions skip stores; passed conditions execute stores");
        Check(cb.Read<std::uint32_t>(0x2000)==0x401a827a && cb.Read<std::uint32_t>(0x2004)==0x401a827a,"stores executed");
        if(!instrument) { expected=cb.memory; expected_regs=jit.Regs(); }
        else {
            Check(cb.memory==expected && jit.Regs()==expected_regs,"instrumentation preserves memory and CPU registers");
            Check(cb.writes.size()==3,"all writes captured with direct page access disabled");
            Check(cb.writes[0].pc==(thumb ? 0x1006u : 0x1008u) && cb.writes[1].pc==(thumb ? 0x100cu : 0x1010u),"exact store PCs");
            Check(cb.writes[0].lr==0x80 && cb.writes[1].lr==0x84,"live LR values, not stale backing registers");
            Check(cb.writes[0].sp==0x3004 && cb.writes[1].sp==0x3004,"live SP values");
            Check(cb.writes[0].thumb==thumb,"instruction mode preserved");
            Check(cb.writes[2].pc==(thumb ? 0x1016u : 0x101cu), "conditional store exact PC");
        }
    }
}
}
int main() {
    try { RunLiveCamera(); Run(false); Run(true); std::cout<<"PASS ARM/Thumb conditional stores, loops, exact PCs, LR/SP, unchanged execution\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
