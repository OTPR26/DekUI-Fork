// SPDX-License-Identifier: GPL-2.0-or-later
// clang++ -std=c++20 -Isrc tools/mm3d/music_check.cpp -o /tmp/dekui-music-check
#include "mm3d/music.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_map>
using namespace MM3D::Music;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at) const {
        auto i=words.find(at);
        return i==words.end()?std::nullopt:std::optional<U32>(i->second);
    }
    void Write(U32 at,U32 value){words[at]=value;}
};
int main(int argc,char** argv) {
    Memory code;
    for(unsigned i=0;i<GainCode.size();++i)code.words[0x5d3e64+4*i]=GainCode[i];
    for(auto [at,word]:Guards)code.words[at]=word;
    assert(CodeMatches(code));
    for(auto& [at,word]:code.words){word^=1;assert(!CodeMatches(code));word^=1;}
    assert(Instruction(PC,Word)==(0xef000000|SVC));
    assert(Instruction(PC+4,Word)==Word&&Instruction(PC,Word^1)==(Word^1));

    constexpr U32 sound=0x9000000, command=0x9100000;
    for(unsigned slot=0;slot<5;++slot) {
        Memory m;
        transition_gain=1.f;
        m.words[Initialized]=1;
        m.words[Manager+0x4cc+slot*4]=sound;
        for(U32 id:{0x01000002u,0x01000015u,0x01000016u,0x01000017u,
                    0x0100001du,0x0100001au,0x0100001bu,0x0100001cu,
                    0x01000065u,0x01000066u,0x01000069u,0x01000086u}) {
            m.words[sound+0xac]=id;
            for(bool mute:{false,true,false}) {
                muted.store(mute);
                for(U32 gain:{0u,0x3f000000u,0x3f800000u}) {
                    auto expected=m.words;
                    expected[command+0x14]=mute?0u:gain;
                    Store(m,sound,command,gain);
                    assert(m.words==expected); // Only final gain changes; no playback/fade writes.
                }
            }
        }
        muted=false; m.words[sound+0xac]=0x01000015;
        for(float fade:{1.f,.75f,.5f,.25f,0.f,1.f}) {
            transition_gain=fade;
            Store(m,sound,command,std::bit_cast<U32>(.5f));
            assert(m.Read(command+0x14)==std::bit_cast<U32>(.5f*fade));
        }
        transition_gain=.5f;
        muted.store(true);
        // Item/heart jingles, ocarina/warp/session music, cutscene effects, unknown IDs.
        for(U32 id:{0x01000022u,0x01000024u,0x01000034u,0x01000047u,
                    0x01000055u,0x01000062u,0x01000080u,0x01000087u,0xffffffffu}) {
            m.words[sound+0xac]=id;
            Store(m,sound,command,0x3f400000);
            assert(m.Read(command+0x14)==0x3f400000);
        }
        m.words[sound+0xac]=0x01000002;
        m.words[Initialized]=0;assert(!ShouldMute(m,sound));
        m.words.erase(Initialized);assert(!ShouldMute(m,sound));
        m.words[Initialized]=1;
        m.words.erase(sound+0xac);assert(!ShouldMute(m,sound));
        m.words[sound+0xac]=0x01000002;
        m.words.erase(Manager+0x4cc+slot*4);
        assert(!ShouldMute(m,sound)); // Effects may share the same numeric ID as music.
        for(U32 invalid:{0u,0xfffffffcu,sound+1})assert(!ShouldMute(m,invalid));
    }
    muted.store(false);transition_gain=1.f;
    // The title fade-in must not revive an outgoing track, including a
    // detached native sound still submitting its final volume commands.
    {
        using namespace MM3D::ClassicSaving;
        Memory m;m.words[Initialized]=1;
        m.words[Manager+0x4cc]=sound;m.words[sound+0xac]=0x01000015;
        exit_fade=ExitFade::Out;transition_gain=.5f;
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0x3f000000);
        exit_fade=ExitFade::Black;transition_gain=0;
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0);
        // StreamMgr may detach before the black/loading phase is over.
        m.words.erase(Manager+0x4cc);
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0);
        exit_fade=ExitFade::In;transition_gain=.5f;
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0);
        m.words[Manager+0x4cc]=sound+0x1000;
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0);
        m.words[sound+0x1000+0xac]=0x01000002;
        Store(m,sound+0x1000,command,0x3f800000);assert(m.Read(command+0x14)==0x3f000000);
        // Native object reuse for the new title track is also allowed.
        m.words[Manager+0x4cc]=sound;m.words[sound+0xac]=0x01000002;
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0x3f000000);
        // Cancellation/normal play always restore the ordinary music path.
        exit_fade=ExitFade::Idle;transition_gain=1;
        m.words[sound+0xac]=0x01000015;
        Store(m,sound,command,0x3f800000);assert(m.Read(command+0x14)==0x3f800000);
    }
    // Low-heart suppression is independent of music mute/fades and changes only
    // this exact effect's gain; recovery, countdown, Tatl and adjacent IDs survive.
    {
        using namespace MM3D::ClassicSaving;
        exit_fade=ExitFade::Idle;
        Memory m;
        for(bool disabled:{false,true,false})for(bool music_muted:{false,true}) {
            disable_low_health_beep=disabled;muted=music_muted;
            for(float fade:{0.f,.5f,1.f})for(U32 id:{LowHealthAlarm,LowHealthAlarm-1,
                    LowHealthAlarm+1,0x01000777u,0x010007dfu,0x010007dau}) {
                transition_gain=fade;m.words[sound+0xac]=id;
                auto expected=m.words;
                expected[command+0x14]=(disabled&&id==LowHealthAlarm)?0u:0x3f400000u;
                Store(m,sound,command,0x3f400000);
                assert(m.words==expected);
            }
        }
        disable_low_health_beep=true;
        for(U32 invalid:{0u,0xfffffffcu,sound+1})assert(!IsLowHealthAlarm(m,invalid));
        m.words.erase(sound+0xac);assert(!IsLowHealthAlarm(m,sound));
        disable_low_health_beep=false;muted=false;transition_gain=1.f;
    }
    // Optional signature validation against the locally installed Restoration code.
    for(int arg=1;arg<argc;++arg) {
        std::ifstream f(argv[arg],std::ios::binary);assert(f);
        Memory actual;
        for(auto [at,word]:code.words) {
            U32 value=0;f.seekg(at-0x100000);f.read(reinterpret_cast<char*>(&value),4);
            assert(f);actual.words[at]=value;
        }
        assert(CodeMatches(actual));
        // Real game's MARKET request becomes one of the three misleadingly
        // named DUMMY streams according to the current day.
        for(auto [at,expected]:std::array<std::pair<U32,U32>,7>{{
            {0x1dc7dc,0xe3500001},{0x1dc7e8,0xe3500002},{0x1dc7f4,0xe3500003},
            {0x1dcb64,0x0100001d},{0x1dcb6c,0x01000015},
            {0x1dcb70,0x01000016},{0x1dcb74,0x01000017}}}) {
            U32 value=0;f.seekg(at-0x100000);f.read(reinterpret_cast<char*>(&value),4);
            assert(f&&value==expected);
        }
    }
    std::cout<<"Music mute, native gain restoration, effects/song exclusions and code guards passed.\n";
}
