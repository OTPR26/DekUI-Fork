// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace MM3D::OwlIntro {
using U32 = std::uint32_t;
inline std::atomic<bool> disabled{false};
constexpr U32 Intro = 0x43fa20, IntroPC = 0x43fa24, IntroWord = 0xe1a04000;
constexpr U32 IntroReturn = 0x43faf8, Idle = 0x58d8d4;
constexpr U32 TatlPC = 0x47b348, TatlWord = 0xe5d002d3, FirstSVC = 0xfffe25;
// USA Restoration: first-statue proximity state and the specific Tatl 0x0282
// reminder gate. Do not mark the tutorial seen in the persistent event flags.
inline constexpr std::array<std::pair<U32, U32>, 21> Guards{{
    {Intro, 0xe92d4070}, {IntroPC, IntroWord},
    {0x43faf4, 0xe3a00001}, {IntroReturn, 0xe8bd8070},
    {0x43fb08, 0x007761d8}, {0x489590, Intro},
    {0x47b014, 0xe92d4ff0}, {0x47b340, 0xe59f0268},
    {TatlPC, TatlWord}, {0x47b34c, 0xe3100010}, {0x47b354, 0x0a000001},
    {0x47b384, 0x159f0228}, {0x47b390, 0xe1c60cbc},
    {0x47b5b0, 0x007761d8}, {0x47b5b4, 0x00000282},
    {Idle, 0xe1d021fc}, {0x58d8dc, 0xe3a00000}, {0x58d918, 0xe12fff1e},
    {0x58d920, 0x007752d8}, {0x58d924, 0x00626c3c}, {0x58d928, 0x00000c01}
}};
template<class M> bool CodeMatches(M& m) {
    for (auto [at, word] : Guards) if (m.Read(at) != word) return false;
    return true;
}
inline U32 Instruction(U32 pc, U32 word) {
    if (pc == IntroPC && word == IntroWord) return 0xef000000 | FirstSVC;
    if (pc == TatlPC && word == TatlWord) return 0xef000000 | (FirstSVC + 1);
    return word;
}
template<class R> void Enter(R& r) {
    r[4] = r[0]; // Replay MOV after the native push; use its matching pop.
    if (disabled) {
        // Run the normal idle handler to select the correct inactive/active/
        // feather dialogue and return zero (allow A). The native pop then
        // restores the original caller PC saved by the intro prologue.
        r[14] = IntroReturn;
        r[15] = Idle;
    }
}
inline U32 TatlFlags(U32 flags) {
    // Register-only override of this reminder's gate, not all Tatl prompts.
    return disabled ? flags | 0x10u : flags;
}
inline bool AllowsSword(U32 state) { return disabled && state == Intro; }
}
