// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace MM3D::FastTime {
using U32 = std::uint32_t;
inline std::atomic_bool enabled{false};
constexpr U32 FirstSVC = 0xfffb40;
constexpr U32 StaticPointer = 0x66889c;
constexpr U32 Common = 0x7751d8;
constexpr U32 SceneSpeedOffset = 0x86b2;
constexpr U32 UpdatePC = 0x172a24;
constexpr U32 ScenePC = 0x40c660;
constexpr U32 InvertedPC = 0x7cfb84;
struct Hook { U32 pc, word; };
constexpr std::array Hooks{
    Hook{UpdatePC, 0xe1d023b2},   // LDRH r2,[r0,#0x32]
    Hook{ScenePC, 0xe1c063b2},    // STRH r6,[r0,#0x32]
    Hook{InvertedPC, 0xe1d432b0}, // LDRH r3,[r4,#0x20]
};
struct Signature { U32 start, size, hash; };
constexpr std::array Signatures{
    Signature{0x172a18, 0x50, 0x99b0405d}, // Native clock + schedule updates, Restoration call
    Signature{0x40c5e4, 0x80, 0x321e311e}, // Scene rate / pre-ocarina override
    Signature{0x7cfb38, 0x68, 0x35301c59}, // Restoration's fractional Inverted Song correction
    Signature{0x201060, 0x14, 0xecd988e7}, // Item lookup used by the native first-cycle check
};
inline U32 Instruction(U32 pc, U32 word) {
    for (U32 i = 0; i < Hooks.size(); ++i)
        if (pc == Hooks[i].pc && word == Hooks[i].word)
            return 0xef000000u | (FirstSVC + i);
    return word;
}
template<class M> bool CodeMatches(M& m) {
    for (const auto& signature : Signatures) {
        U32 hash = 2166136261u;
        for (U32 offset = 0; offset < signature.size; offset += 4) {
            const auto word = m.Read(signature.start + offset);
            if (!word) return false;
            for (unsigned byte = 0; byte < 4; ++byte)
                hash = (hash ^ ((*word >> (byte * 8)) & 255u)) * 16777619u;
        }
        if (hash != signature.hash) return false;
    }
    return true;
}
template<class M> std::optional<U32> Half(M& m, U32 at) {
    const auto word = m.Read(at & ~3u);
    if (!word) return std::nullopt;
    return (*word >> ((at & 2u) * 8u)) & 0xffffu;
}
template<class M> void StoreHalf(M& m, U32 at, U32 value) {
    const auto word = m.Read(at & ~3u);
    if (!word) return;
    const auto shift = (at & 2u) * 8u;
    m.Write(at & ~3u, (*word & ~(0xffffu << shift)) | ((value & 0xffffu) << shift));
}
// Native scene setup changes nonzero scene speed to 5 before obtaining the ocarina.
// It then rounds scene_speed * 2/3: the usual 3 becomes 2, first-cycle 5 becomes 3.
// Keep the scene value intact so disabling the option can recover its native rate.
inline U32 LiveRate(U32 current, U32 scene, bool fast) {
    if ((current != 2 && current != 3) || (scene != 3 && scene != 5)) return current;
    return fast || scene == 5 ? 3u : 2u;
}
template<class M, class R> void Update(M& m, R& r, U32 svc, bool fast) {
    if (svc == FirstSVC) {
        const auto speed = Half(m, r[0] + 0x32);
        if (speed) {
            auto rate = *speed;
            if (const auto scene = Half(m, r[4] + SceneSpeedOffset))
                rate = LiveRate(rate, *scene, fast);
            // Change the shared rate, not just this load: schedule/NPC consumers and
            // the second native clock update must see exactly the same increment.
            if (rate != *speed) StoreHalf(m, r[0] + 0x32, rate);
            r[2] = rate;
        }
        r[15] = UpdatePC + 4;
    } else if (svc == FirstSVC + 1) {
        const auto rate = r[6] & 0xffffu;
        StoreHalf(m, r[0] + 0x32, fast && rate == 2 ? 3 : rate);
        r[15] = ScenePC + 4;
    } else if (svc == FirstSVC + 2) {
        const auto time = Half(m, r[4] + 0x20);
        if (time) r[3] = *time;
        r[15] = InvertedPC + 4;
        if (!fast || !time || r[4] != Common) return;
        const auto pointer = m.Read(StaticPointer);
        const auto speed = pointer ? Half(m, *pointer + 0x32) : std::nullopt;
        const auto extra = m.Read(Common + 0x10);
        if (speed != 3u || extra != 0xffffffffu) return;
        // Reached only when Restoration's ShouldSlowTime() is true. Base 3 plus
        // extra -1 is 2; subtract 1 every tick to get 1/3 of the fast base rate.
        // Restoration's existing scheduled-NPC hook already returns base / 3.
        // Its event exceptions still follow the original native path.
        r[3] = (*time - 1u) & 0xffffu;
        StoreHalf(m, Common + 0x20, r[3]);
        r[15] = 0x7cfb54; // Native copy to schedule time, then return
    }
}
} // namespace MM3D::FastTime
