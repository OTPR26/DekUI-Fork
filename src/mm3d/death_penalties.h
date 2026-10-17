// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace MM3D::DeathPenalties {
using U32 = std::uint32_t;
inline std::atomic<bool> lose_rupees{false};
inline std::atomic<bool> world_ends{false};
inline std::atomic<bool> advance_six_hours{false};
constexpr U32 FirstSVC = 0xfffc80;
constexpr U32 DeathPC = 0x206344, DeathWord = 0xe1c01abc;
constexpr U32 ContinuePC = 0x16fa18, ContinueWord = 0xeb00841f;
constexpr U32 SceneSoundSVC = 0xfffc7f;
constexpr U32 SceneSoundPC = 0x13852c, SceneSoundWord = 0xe1d01ab0;
constexpr U32 NativeRespawn = 0x190a9c, NativeMoonCrash = 0x1bfdc8;
constexpr U32 ContinueReturn = 0x16fa64;
constexpr U32 Wallet = 0x775318, Accumulator = 0x7751fc, Health = 0x775314;
constexpr U32 GameOverOffset = 0x86ac;
constexpr U32 Common = 0x7751d8;
constexpr U32 Day = Common + 0x14, TotalDay = Common + 0x18, Time = Common + 0x20;
constexpr U32 Night = Common + 0xc;
constexpr U32 ScheduleTime = Common + 0x36a2, TimeOverride = Common + 0x36a0;
constexpr U32 NoTimeOverride = 0xffff, DawnSound = 0xfffe, DuskSound = 0xfffd;
constexpr U32 PreviousTime = Common + 0x13760;
constexpr U32 DailyFlags = Common + 0x137a, DayFlagA = Common + 0x1299;
constexpr U32 DayFlagB = Common + 0x12a5, DayEvent = 0x68d55c;
struct Signature { U32 start, size, hash; };
// USA Restoration binary: fairy lookup/consumption, the no-fairy branch,
// death-state progression, ordinary respawn, and the complete moon-crash routine.
// Hash the original guest code, never translated SVC instructions.
constexpr std::array<Signature, 12> Signatures{{
    {0x2062b0, 0x38, 0x995c1962}, {0x206340, 0x44, 0x6eaeefab},
    {0x16f9b0, 0xb8, 0x251abe27}, {0x1bfdc8, 0x1c4, 0xefb7f3e9},
    {0x20ebd8, 0x110, 0x5766bf1a}, {0x190a9c, 0x64, 0x1965b18b},
    {0x138c78, 0xa8, 0xef676085}, // day counters and daily-event cleanup
    {0x172a68, 0x40, 0x9bb971b2}, // previous time and day/night classification
    {0x3a8f1c, 0x110, 0x9b4bc98a}, // exact-dawn initialization can advance the day again
    {0x12ac40, 0x1c, 0xbfa1de27}, // scene load consumes the optional time override
    {0x138520, 0x30, 0xcaf292be}, // later initialization converts it to a sound marker
    {0x172874, 0x80, 0x16d4a5eb}, // marker countdown and dawn/dusk sound calls
}};
inline U32 Instruction(U32 pc, U32 word) {
    if (pc == DeathPC && word == DeathWord) return 0xef000000 | FirstSVC;
    if (pc == ContinuePC && word == ContinueWord) return 0xef000000 | (FirstSVC + 1);
    if (pc == SceneSoundPC && word == SceneSoundWord) return 0xef000000 | SceneSoundSVC;
    return word;
}
template<class M> bool CodeMatches(M& m) {
    for (const auto& signature : Signatures) {
        U32 hash = 2166136261u;
        for (U32 offset = 0; offset < signature.size; offset += 4) {
            auto word = m.Read(signature.start + offset);
            if (!word) return false;
            for (unsigned byte = 0; byte < 4; ++byte)
                hash = (hash ^ ((*word >> (8 * byte)) & 255)) * 16777619u;
        }
        if (hash != signature.hash) return false;
    }
    return true;
}
inline bool OnMoon(U32 scene) {
    // Majora, Deku/Goron/Zora/Link trials, and the Moon field.
    return scene == 0x0b || scene == 0x2a || scene == 0x3f ||
           scene == 0x47 || scene == 0x66 || scene == 0x67;
}
template<class M> std::optional<U32> Half(M& m, U32 at) {
    auto word = m.Read(at & ~3u);
    if (!word) return std::nullopt;
    return (*word >> ((at & 2) * 8)) & 0xffff;
}
template<class M> void StoreHalf(M& m, U32 at, U32 value) {
    const U32 aligned = at & ~3u, shift = (at & 2) * 8;
    if (auto word = m.Read(aligned))
        m.Write(aligned, (*word & ~(0xffffu << shift)) | ((value & 0xffff) << shift));
}
template<class M> bool PlayContext(M& m, U32 ctx) {
    // State::status=Running, State::type=Play. Reject stale/unmapped contexts.
    auto state = m.Read(ctx + 0x13c);
    return ctx != 0 && (ctx & 3) == 0 && state && (*state & 0xffff) == 0x0402;
}
template<class M> void ClearWallet(M& m) {
    // Clear pending rupee animation too, so a pickup before death cannot refill
    // the wallet afterwards. Preserve the adjacent form/equipment halfwords.
    if (!m.Read(Wallet) || !m.Read(Accumulator)) return;
    StoreHalf(m, Wallet, 0);
    StoreHalf(m, Accumulator, 0);
}
struct ClockDestination { U32 day, time; bool dawn, crash; bool dusk; };
inline std::optional<ClockDestination> SixHoursLater(U32 day, U32 time) {
    if (day < 1 || day > 3 || time > 0xffff) return {};
    // Midnight wraps the 16-bit time, but the cycle day advances at 06:00.
    const U32 since_dawn = (time + 0xc000) & 0xffff;
    const bool dawn = since_dawn + 0x4000 >= 0x10000;
    return ClockDestination{day + U32(dawn), (time + 0x4000) & 0xffff, dawn,
                            day == 3 && dawn, time >= 0x8000 && time < 0xc000};
}
inline U32 DestinationTime(const ClockDestination& target) {
    return (target.dawn && target.time == 0x4000) || (target.dusk && target.time == 0xc000) ?
        target.time + 1 : target.time;
}
// Owned by the guest CPU callback, not global state shared across games. Queue
// a single call after scene loading has consumed the optional time override.
struct RespawnSound {
    U32 day{}, time{}, marker{NoTimeOverride};
    void Arm(const ClockDestination& target) {
        day = target.day;
        time = DestinationTime(target);
        marker = target.crash ? NoTimeOverride :
                 target.dawn ? DawnSound : target.dusk ? DuskSound : NoTimeOverride;
    }
    template<class M, class R> void SceneInit(M& m, R& r) {
        // Displaced LDRH r1,[r0,#0xa0]. Leave its result unchanged so the
        // native comparison skips normalization (which would force a howl).
        r[1] = Half(m, r[0] + 0xa0).value_or(0);
        if (marker != NoTimeOverride && r[0] + 0xa0 == TimeOverride &&
            r[1] == NoTimeOverride && m.Read(Day) == day && Half(m, Time) == time)
            StoreHalf(m, TimeOverride, marker);
        *this = {};
    }
};
template<class M> bool AdvanceClock(M& m, const ClockDestination& target) {
    // Read everything before writing so an unsupported/unmapped save cannot
    // receive only part of the penalty. Native respawn reloads the scene using
    // the new time; retain native death/entrance handling.
    for (U32 at : {Time, Night, TimeOverride, PreviousTime})
        if (!m.Read(at)) return false;
    std::optional<U32> total, flags_a, flags_b, daily;
    if (target.dawn) {
        total = m.Read(TotalDay);
        flags_a = m.Read(DayFlagA & ~3u);
        flags_b = m.Read(DayFlagB & ~3u);
        daily = m.Read(DailyFlags & ~3u);
        if (!total || *total >= 0x7fffffff || !flags_a || !flags_b || !daily ||
            !m.Read((DailyFlags & ~3u) + 4) || !m.Read(DayEvent)) return false;
    }
    if (target.dawn) {
        // Mirror verified native AdvanceDay (0x138c78): advance both counters,
        // clear the six daily flags and the two per-day event bits.
        m.Write(Day, target.day);
        m.Write(TotalDay, *total + 1);
        m.Write(DailyFlags & ~3u, *daily & 0xffff);
        m.Write((DailyFlags & ~3u) + 4, 0);
        if ((*flags_a & (0x10u << 8)) || (*flags_b & (2u << 8))) m.Write(DayEvent, 0x27b);
        m.Write(DayFlagA & ~3u, *flags_a & ~(0x10u << 8));
        m.Write(DayFlagB & ~3u, *flags_b & ~(2u << 8));
    }
    // Exactly 06:00 is pending dawn on scene load (0x3a8f24); exactly 18:00
    // initializes the actor to await dusk (0x3a8f30). Move one clock unit past
    // either already-processed boundary to avoid a second day advance/call.
    const U32 time = DestinationTime(target);
    StoreHalf(m, Time, time);
    // This is NOT a clock copy: ordinary scene initialization turns every
    // non-sentinel value except noon into 0xfffd, a delayed evening howl.
    // Noon has its own day-advance behavior. Our clock/day are already updated.
    StoreHalf(m, TimeOverride, NoTimeOverride);
    StoreHalf(m, ScheduleTime, time);
    StoreHalf(m, PreviousTime, time);
    m.Write(Night, U32(time < 0x4000 || time >= 0xc000));
    return true;
}
template<class M, class R> void Death(M& m, R& r, bool remove_rupees) {
    // This STRH is reached only after the native seven-bottle fairy scan fails.
    // Pits/void respawns and successful fairy revivals never reach this site.
    if (remove_rupees && PlayContext(m, r[6]) && r[0] == r[6] + 0x8600 &&
        r[1] == 1 && Half(m, Health) == 0)
        ClearWallet(m);
    StoreHalf(m, r[0] + 0xac, r[1]); // displaced STRH r1,[r0,#0xac]
}
template<class M, class R> void Continue(M& m, R& r, bool end_world, bool advance_time = false,
                                        RespawnSound* sound = nullptr) {
    if (sound) *sound = {};
    const U32 ctx = r[0];
    auto scene = Half(m, ctx + 0x148);
    auto health = Half(m, Health);
    if ((end_world || advance_time) && PlayContext(m, ctx) && r[9] == ctx &&
        r[6] == ctx + GameOverOffset && Half(m, r[6]) == 3 &&
        scene && *scene <= 0x74 && !OnMoon(*scene) && health && *health == 0) {
        if (!end_world && advance_time) {
            const auto day = m.Read(Day), time = Half(m, Time);
            const auto target = day && time ? SixHoursLater(*day, *time) : std::nullopt;
            if (target && target->crash) end_world = true;
            else if (target && AdvanceClock(m, *target) && sound) sound->Arm(*target);
        }
        if (end_world) {
            // Finish the death state before changing scenes. Native ordinary death
            // also restores three hearts and advances state3->4 at this point.
            StoreHalf(m, Health, 0x30);
            StoreHalf(m, r[6], 4);
            // Call the native moon-crash entry, which sets day/time, both entrance
            // fields, cutscene ID, transition and audio. Skip the ordinary respawn
            // writes after this call; they would overwrite the crash transition.
            r[14] = ContinueReturn;
            r[15] = NativeMoonCrash;
            return;
        }
    }
    r[14] = ContinuePC + 4;
    r[15] = NativeRespawn; // displaced BL, including every disabled/Moon case
}
} // namespace MM3D::DeathPenalties
