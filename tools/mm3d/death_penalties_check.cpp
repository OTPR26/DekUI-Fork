// SPDX-License-Identifier: GPL-2.0-or-later
// clang++ -std=c++20 -Isrc tools/mm3d/death_penalties_check.cpp -o /tmp/death-check
#include "mm3d/death_penalties.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace MM3D::DeathPenalties;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at) const {
        auto it=words.find(at);return it==words.end()?std::nullopt:std::optional<U32>(it->second);
    }
    void Write(U32 at,U32 value) { words[at]=value; }
};
constexpr U32 Ctx=0x10000000, BankSentinel=0x775ad0;
Memory Live(U32 scene,U32 state,U32 rupees) {
    Memory m;
    m.words[Ctx+0x13c]=0xbeef0402;
    m.words[Ctx+0x148]=0xcafe0000|scene;
    m.words[Ctx+GameOverOffset]=0xfeed0000|state;
    m.words[Wallet]=0x12340000|rupees;
    m.words[Accumulator]=0x56780063;
    m.words[Health]=0x9abc0000;
    m.words[BankSentinel]=0x00001387; // unrelated save bytes must never change
    return m;
}
std::array<U32,16> Registers() {
    std::array<U32,16> r;
    for(unsigned i=0;i<r.size();++i)r[i]=0x20000000+i*4;
    return r;
}
Memory Clock(U32 day, U32 time) {
    auto m = Live(0x6f, 3, 200);
    m.words[Day] = day;
    m.words[TotalDay] = 12;
    m.words[Time] = 0x98760000 | time;
    m.words[Night] = time < 0x4000 || time >= 0xc000;
    m.words[TimeOverride] = time | (time << 16);
    m.words[PreviousTime] = 0x76540000 | time;
    m.words[DailyFlags & ~3u] = 0xabcdef98;
    m.words[(DailyFlags & ~3u) + 4] = 0x76543210;
    m.words[DayFlagA & ~3u] = 0x1234ff56;
    m.words[DayFlagB & ~3u] = 0x9876ff54;
    m.words[DayEvent] = 0x12345678;
    return m;
}
std::array<U32,16> ContinueRegisters() {
    auto r = Registers();
    r[0] = Ctx;
    r[9] = Ctx;
    r[6] = Ctx + GameOverOffset;
    return r;
}
void ClockChecks() {
    // Every representable time on all three days. Midnight wraps the clock;
    // only a departure between midnight and 06:00 crosses the next dawn.
    for (U32 day = 1; day <= 3; ++day) for (U32 time = 0; time <= 0xffff; ++time) {
        const auto target = SixHoursLater(day, time);
        assert(target);
        assert(target->time == (time + 16384) % 65536);
        assert(target->day == day + (time < 16384));
        assert(target->dawn == (time < 16384));
        assert(target->crash == (day == 3 && time < 16384));
        assert(target->dusk == (time >= 32768 && time < 49152));
    }
    for (U32 day : {0u, 4u, 0xffffffffu}) assert(!SixHoursLater(day, 0x8000));
    assert(!SixHoursLater(1, 0x10000));
    // Integration checks at midnight, dawn, noon, dusk, and either side of
    // the cutoff. Compare all memory/registers to catch unrelated writes.
    for (U32 day : {1u, 2u, 3u})
      for (U32 time : {0u, 1u, 0x3fffu, 0x4000u, 0x8000u, 0xbfffu, 0xc000u, 0xffffu})
        for (bool enabled : {false, true}) for (bool end : {false, true}) {
            auto m = Clock(day, time), expected = m;
            auto r = ContinueRegisters(), expected_r = r;
            const bool crash = end || (enabled && day == 3 && time < 0x4000);
            if (crash) {
                expected.words[Health] = 0x9abc0030;
                expected.words[Ctx + GameOverOffset] = 0xfeed0004;
            } else if (enabled) {
                // Exactly 06:00 is the game's pending-dawn marker. After our
                // day advance, skip that marker to avoid another day advance.
                const U32 next = time == 0 ? 0x4001 : time == 0x8000 ? 0xc001 : (time + 0x4000) % 0x10000;
                expected.words[Time] = 0x98760000 | next;
                expected.words[TimeOverride] = NoTimeOverride | (next << 16);
                expected.words[PreviousTime] = 0x76540000 | next;
                expected.words[Night] = next < 0x4000 || next >= 0xc000;
                if (time < 0x4000) {
                    expected.words[Day] = day + 1;
                    expected.words[TotalDay] = 13;
                    expected.words[DailyFlags & ~3u] = 0xef98;
                    expected.words[(DailyFlags & ~3u) + 4] = 0;
                    expected.words[DayFlagA & ~3u] = 0x1234ef56;
                    expected.words[DayFlagB & ~3u] = 0x9876fd54;
                    expected.words[DayEvent] = 0x27b;
                }
            }
            expected_r[14] = crash ? ContinueReturn : ContinuePC + 4;
            expected_r[15] = crash ? NativeMoonCrash : NativeRespawn;
            Continue(m, r, end, enabled);
            assert(m.words == expected.words && r == expected_r);
            // Native ordinary respawn restores health and advances game-over
            // state before another Continue could be reached.
            m.words[Health] = 0x9abc0030;
            m.words[Ctx + GameOverOffset] = 0xfeed0004;
            const auto after = m.words;
            r = ContinueRegisters();
            Continue(m, r, end, enabled);
            assert(m.words == after && r[15] == NativeRespawn);
        }
    // Daily event notification is conditional; the unrelated flag bits survive.
    for (bool a : {false, true}) for (bool b : {false, true}) {
        auto m = Clock(1, 0x1000);
        m.words[DayFlagA & ~3u] = 0x1234ef56 | (a ? 0x1000 : 0);
        m.words[DayFlagB & ~3u] = 0x9876fd54 | (b ? 0x0200 : 0);
        auto r = ContinueRegisters();
        Continue(m, r, false, true);
        assert(m.words[DayEvent] == (a || b ? 0x27bu : 0x12345678u));
        assert(m.words[DayFlagA & ~3u] == 0x1234ef56);
        assert(m.words[DayFlagB & ~3u] == 0x9876fd54);
    }
    // No partially applied penalty if any required save address is unavailable.
    for (U32 at : {Day, TotalDay, Time, Night, TimeOverride, PreviousTime,
                   DailyFlags & ~3u, (DailyFlags & ~3u) + 4, DayFlagA & ~3u,
                   DayFlagB & ~3u, DayEvent}) {
        auto m = Clock(1, 0x1000);
        m.words.erase(at);
        const auto before = m.words;
        auto r = ContinueRegisters();
        Continue(m, r, false, true);
        assert(m.words == before && r[15] == NativeRespawn);
    }
    for (int invalid = 0; invalid < 11; ++invalid) {
        auto m = Clock(1, 0x1000);
        auto r = ContinueRegisters();
        if (invalid == 0) m.words[Day] = 0;
        if (invalid == 1) m.words[Day] = 4;
        if (invalid == 2) m.words[TotalDay] = 0x7fffffff;
        if (invalid == 3) m.words[Ctx + 0x13c] = 0x0400;
        if (invalid == 4) m.words.erase(Ctx + 0x148);
        if (invalid == 5) m.words[Health] |= 1;
        if (invalid == 6) m.words[Ctx + GameOverOffset] = 20; // fairy revival
        if (invalid == 7) r[9] += 4;
        if (invalid == 8) r[6] += 4;
        if (invalid == 9) m.words[Ctx + GameOverOffset] = 0; // pit/void, no death
        if (invalid == 10) m.words[Ctx + 0x148] = 0x75;
        const auto before = m.words;
        Continue(m, r, false, true);
        assert(m.words == before && r[15] == NativeRespawn);
    }
    for (U32 scene : {0x0bu, 0x2au, 0x3fu, 0x47u, 0x66u, 0x67u}) {
        auto m = Clock(3, 0x1000);
        m.words[Ctx + 0x148] = scene;
        const auto before = m.words;
        auto r = ContinueRegisters();
        Continue(m, r, true, true);
        assert(m.words == before && r[15] == NativeRespawn);
    }
}
// Model the verified scene-loader read, later initialization, and delayed
// sound countdown. Checking just the clock stores missed the original bug.
void RespawnSoundChecks() {
    for (U32 time=0;time<=0xffff;++time) {
        auto m=Clock(1,time);auto r=ContinueRegisters();RespawnSound sound;
        Continue(m,r,false,true,&sound);
        const U32 destination=time==0?0x4001:time==0x8000?0xc001:(time+0x4000)&0xffff;
        assert(Half(m,Time)==destination);
        assert(Half(m,TimeOverride)==NoTimeOverride);
        // 0x12ac40: no override means scene load keeps our destination time.
        const auto before=m.words;
        r=Registers();r[0]=TimeOverride-0xa0;const auto original=r;
        sound.SceneInit(m,r);
        assert(r[1]==NoTimeOverride); // native normalization must be skipped
        for(unsigned i=0;i<16;++i)if(i!=1)assert(r[i]==original[i]);
        auto expected=before;
        const U32 marker=time<0x4000?DawnSound:
                         time>=0x8000&&time<0xc000?DuskSound:NoTimeOverride;
        expected[TimeOverride]=(expected[TimeOverride]&0xffff0000)|marker;
        assert(m.words==expected);
        unsigned rooster=0,howl=0;
        for(unsigned frame=0;frame<32;++frame) {
            U32 value=*Half(m,TimeOverride);
            if(value>=0xff00&&value!=0xffff) {
                value=(value-16)&0xffff;
                if(value==0xff0e){++rooster;value=0xffff;}
                if(value==0xff0d){++howl;value=0xffff;}
                StoreHalf(m,TimeOverride,value);
            }
        }
        assert(rooster==unsigned(time<0x4000));
        assert(howl==unsigned(time>=0x8000&&time<0xc000));
        assert(Half(m,Time)==destination);
        // A later scene load must not play the death call again.
        r[0]=TimeOverride-0xa0;sound.SceneInit(m,r);
        assert(Half(m,TimeOverride)==NoTimeOverride);
    }
    // A normal scene load / Song of Double Time keeps native sound behavior.
    for(U32 value:{0x4000u,0x8000u,0xc000u,DawnSound,DuskSound,NoTimeOverride}) {
        auto m=Clock(1,0x9000);StoreHalf(m,TimeOverride,value);
        auto r=Registers();r[0]=TimeOverride-0xa0;RespawnSound sound;
        const auto before=m.words;sound.SceneInit(m,r);
        assert(m.words==before&&r[1]==value);
    }
    for(int invalid=0;invalid<5;++invalid) {
        auto m=Clock(1,0x1000);auto r=ContinueRegisters();RespawnSound sound;
        Continue(m,r,false,true,&sound);
        r=Registers();r[0]=TimeOverride-0xa0;
        if(invalid==0)m.words[Day]=3;
        if(invalid==1)StoreHalf(m,Time,0x6000);
        if(invalid==2)StoreHalf(m,TimeOverride,0x8000);
        if(invalid==3)m.words.erase(Day);
        if(invalid==4)r[0]+=4;
        const auto before=m.words;sound.SceneInit(m,r);
        assert(m.words==before&&sound.marker==NoTimeOverride);
    }
    // Disabled, invalid, Moon and moon-crash continuations cancel stale calls.
    for(int mode=0;mode<5;++mode) {
        auto m=Clock(mode==3?3:1,0x1000);auto r=ContinueRegisters();RespawnSound sound;
        sound.Arm(*SixHoursLater(1,0x1000));
        if(mode==1)m.words[Day]=0;
        if(mode==2)m.words[Ctx+0x148]=0x0b;
        Continue(m,r,mode==4,mode!=0,&sound);
        assert(sound.marker==NoTimeOverride);
    }
}
int main(int argc,char** argv) {
    ClockChecks();
    RespawnSoundChecks();
    assert(Instruction(DeathPC,DeathWord)==(0xef000000|FirstSVC));
    assert(Instruction(ContinuePC,ContinueWord)==(0xef000000|(FirstSVC+1)));
    assert(Instruction(SceneSoundPC,SceneSoundWord)==(0xef000000|SceneSoundSVC));
    assert(Instruction(SceneSoundPC,SceneSoundWord^1)==(SceneSoundWord^1));
    assert(Instruction(DeathPC,DeathWord^1)==(DeathWord^1));
    assert(Instruction(ContinuePC+4,ContinueWord)==ContinueWord);
    // Wallet edges, all contexts, both settings. Preserve every neighboring byte,
    // bank sentinel and register, replaying the original game-over store exactly.
    for(U32 scene=0;scene<=0x74;++scene)for(bool enabled:{false,true})
      for(U32 rupees:{0u,1u,99u,200u,500u,999u}) {
        auto m=Live(scene,0,rupees), expected=m;
        auto r=Registers();r[0]=Ctx+0x8600;r[1]=1;r[6]=Ctx;
        const auto original=r;
        Death(m,r,enabled);
        expected.words[Ctx+GameOverOffset]=0xfeed0001;
        if(enabled){expected.words[Wallet]=0x12340000;expected.words[Accumulator]=0x56780000;}
        assert(m.words==expected.words && r==original);
      }
    // A changed/malformed context must not affect the wallet; native replay remains.
    for(int invalid=0;invalid<5;++invalid) {
        auto m=Live(0x6f,0,200);auto r=Registers();r[0]=Ctx+0x8600;r[1]=1;r[6]=Ctx;
        if(invalid==0)m.words[Ctx+0x13c]=0x0602;
        if(invalid==1)m.words.erase(Ctx+0x13c);
        if(invalid==2)m.words[Health]|=1;
        if(invalid==3)m.words.erase(Accumulator);
        if(invalid==4)r[6]+=4;
        const auto before=m.words[Wallet];Death(m,r,true);assert(m.words[Wallet]==before);
    }
    // Every scene: all six Moon destinations keep native respawn. Rupee penalties
    // remain independent, including on the Moon. Disabled mode is bit-exact BL.
    for(U32 scene=0;scene<=0x100;++scene)for(bool end:{false,true}) {
        auto m=Live(scene,3,200),expected=m;
        auto r=Registers();r[0]=Ctx;r[9]=Ctx;r[6]=Ctx+GameOverOffset;
        const auto before=r;
        const bool moon=scene==11||scene==42||scene==63||scene==71||scene==102||scene==103;
        Continue(m,r,end);
        const bool crash=end && scene<=0x74 && !moon;
        if(crash){expected.words[Health]=0x9abc0030;expected.words[Ctx+GameOverOffset]=0xfeed0004;}
        assert(m.words==expected.words);
        for(unsigned i=0;i<14;++i)assert(r[i]==before[i]);
        assert(r[14]==(crash?ContinueReturn:ContinuePC+4));
        assert(r[15]==(crash?NativeMoonCrash:NativeRespawn));
    }
    for(int invalid=0;invalid<6;++invalid) {
        auto m=Live(0x6f,3,200);auto r=Registers();r[0]=Ctx;r[9]=Ctx;r[6]=Ctx+GameOverOffset;
        if(invalid==0)m.words[Ctx+0x13c]=0x0400;
        if(invalid==1)m.words.erase(Ctx+0x148);
        if(invalid==2)m.words[Health]|=1;
        if(invalid==3)m.words[Ctx+GameOverOffset]=20; // fairy revival
        if(invalid==4)r[9]+=4;
        if(invalid==5)r[6]+=4;
        const auto before=m.words;Continue(m,r,true);
        assert(m.words==before && r[15]==NativeRespawn && r[14]==ContinuePC+4);
    }
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);assert(f);
    Memory code;
    for(auto sig:Signatures) {
        f.seekg(sig.start-0x100000);
        for(U32 off=0;off<sig.size;off+=4){U32 word;f.read(reinterpret_cast<char*>(&word),4);assert(f);code.words[sig.start+off]=word;}
    }
    assert(CodeMatches(code));
    for(auto sig:Signatures)for(U32 off=0;off<sig.size;off+=4){code.words[sig.start+off]^=1;assert(!CodeMatches(code));code.words[sig.start+off]^=1;}
    // Verify no-fairy/fairy split, native crash time/entrance, and return epilogue.
    assert(code.Read(0x2062c0)==0x0a00001eu); // BEQ to death-only store
    assert(code.Read(0x2062c4)==0xe3a02014u); // fairy state20, bypasses death hook
    assert(code.Read(ContinueReturn)==0xe8bd9ff0u);
    assert(code.Read(0x1bff80)==0x400au && code.Read(0x1bff88)==0x54c0u);
    assert(code.Read(0x3a8f24)==0xe3500901u); // CMP time,#0x4000
    assert(code.Read(0x3a8f28)==0x0a000015u); // BEQ to pending-dawn path
    std::cout<<"Death penalty checks passed against native binary\n";
}
