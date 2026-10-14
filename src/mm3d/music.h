// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <bit>
#include <atomic>
#include <cstdint>
#include <utility>
#include "mm3d/classic_saving.h"

namespace MM3D::Music {
using U32 = std::uint32_t;
inline std::atomic<bool> muted{false};
inline std::atomic<bool> disable_low_health_beep{false};
// Project Restoration game/sound.h: NA_SE_SY_HITPOINT_ALARM.
constexpr U32 LowHealthAlarm=0x01000787;
template<class M> bool IsLowHealthAlarm(M& m,U32 sound) {
    if(sound<0x100000||sound>0x3fffff50||(sound&3))return false;
    return m.Read(sound+0xac)==LowHealthAlarm;
}
// Temporary owl transition envelope; never changes the user volume setting.
inline std::atomic<float> transition_gain{1.f};

// USA Restoration v110: BasicSound::UpdateParam's final volume command.
// Original USA Rev1: 0x5d3fec. The sound is r4, command r5, gain s16.
// Changing only this command preserves playback, native fades and save states.
constexpr U32 PC=0x5d3e7c, Word=0xed858a05, SVC=0xffff9b;
constexpr U32 Manager=0x7cb49c, Initialized=0x6b0a3c;
inline U32 Instruction(U32 pc,U32 word) {
    return pc==PC&&word==Word ? 0xef000000|SVC : word;
}
inline constexpr std::array<U32,20> GainCode{
    0xe5940000,0xe5901020,0xe1a00004,0xe12fff31,0xe1a01000,0xe5851010,
    Word,0xedc5aa06,0xedc5ba09,0xe285101c,0xe1a00007,0xecc19a02,
    0xe1a01005,0xe5856028,0xed85ca0b,0xed85ba0c,0xed859a0d,
    0xedc58a0e,0xecbd8b0a,0xe8bd41f0};
inline constexpr std::array<std::pair<U32,U32>,10> Guards{{
    {0x5d3d14,0xe92d41f0},{0x5d3d18,0xe1a04000},{0x5d3d1c,0xed2d8b0a},
    {0x10a21c,Initialized},{0x10a220,Manager},
    // StreamMgr::GetCurrentStreamId indexes its five SoundHandles at +4cc.
    {0x1e11a0,0xe0800104},{0x1e11a4,0xe2801b01},{0x1e11a8,0xe28110cc},
    // BasicSound's archive ID is stored at +ac.
    {0x5d3654,0xe59000ac},{0x5d3aec,0xe58410ac}
}};
template<class M> bool CodeMatches(M& m) {
    for(unsigned i=0;i<GainCode.size();++i)
        if(m.Read(0x5d3e64+4*i)!=GainCode[i])return false;
    for(auto [at,word]:Guards)if(m.Read(at)!=word)return false;
    return true;
}

// Stream IDs documented by Project Restoration's game/sound.h. Stream and
// effect IDs overlap, so this classification is used ONLY for StreamMgr handles.
inline bool IsMusic(U32 id) {
    if(id<0x01000002||id>0x01000086)return false;
    switch(id&0xff) {
    // IDs15/16/17 are Clock Town days1/2/3 despite their DUMMY names.
    // Native StreamMgr remaps MARKET1d by day at1dc7c8..1dc7f8.
    // Failure/victory/item/chest/goal fanfares and unused entries.
    case 0x08: case 0x09: case 0x19:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x2b:
    case 0x37: case 0x39: case 0x3d: case 0x3f: case 0x41: case 0x56:
    // Ocarina demonstrations, played songs, warps and instrument sessions.
    case 0x32: case 0x33: case 0x34: case 0x35:
    case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c:
    case 0x4d: case 0x4e: case 0x4f: case 0x51: case 0x52: case 0x55:
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d:
    case 0x5e: case 0x5f: case 0x61: case 0x62: case 0x63: case 0x64:
    case 0x67: case 0x68: case 0x6c: case 0x6d: case 0x6e: case 0x7f:
    case 0x80: case 0x85: // cutscene effect sequence and Zora session
        return false;
    default:return true;
    }
}
template<class M> bool IsMusicSound(M& m,U32 sound) {
    if(sound<0x100000||sound>0x3fffff50||(sound&3))return false;
    const auto initialized=m.Read(Initialized);
    if(!initialized||!(*initialized&1))return false;
    bool stream=false;
    for(unsigned i=0;i<5;++i)if(m.Read(Manager+0x4cc+4*i)==sound){stream=true;break;}
    if(!stream)return false;
    // Read the active sound's ID rather than the requested track: the manager
    // clears its ID before a stopped track finishes fading out.
    const auto id=m.Read(sound+0xac);
    return id&&IsMusic(*id);
}
template<class M> bool ShouldMute(M& m,U32 sound) {
    return muted.load()&&IsMusicSound(m,sound);
}
// Used only on the guest CPU thread. Remember outgoing instances even after
// StreamMgr detaches their handles: their final volume commands can still run.
struct OutgoingTracks {
    std::array<std::pair<U32,U32>,5> tracks{};
    template<class M> bool Hold(M& m,U32 sound,bool music) {
        using ClassicSaving::ExitFade;
        const auto phase=ClassicSaving::exit_fade.load();
        if(phase==ExitFade::Idle){tracks={};return false;}
        const auto id=m.Read(sound+0xac);
        if(!id)return false;
        if(phase==ExitFade::Out||phase==ExitFade::Black) {
            if(music)for(auto& track:tracks) {
                if(track.first==sound||track.first==0){track={sound,*id};break;}
            }
            if(phase==ExitFade::Out)return false;
        }
        for(auto& track:tracks)if(track.first==sound) {
            if(track.second==*id)return true;
            track={}; // The native sound object has been reused for a new track.
        }
        return false;
    }
};
inline OutgoingTracks outgoing;
template<class M> void Store(M& m,U32 sound,U32 command,U32 volume_bits) {
    // Replay VSTR s16,[r5,#0x14], without changing any ARM/VFP register or flag.
    const float fade=std::clamp(transition_gain.load(),0.f,1.f);
    const bool mute=muted.load();
    const bool music=IsMusicSound(m,sound);
    if(outgoing.Hold(m,sound,music))volume_bits=0;
    else if((mute||fade<1.f)&&music)
        volume_bits=mute?0u:std::bit_cast<U32>(std::bit_cast<float>(volume_bits)*fade);
    if(disable_low_health_beep.load()&&IsLowHealthAlarm(m,sound))volume_bits=0;
    m.Write(command+0x14,volume_bits);
}
}
