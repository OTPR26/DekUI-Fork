// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace MM3D::Pictograph {
using U32 = std::uint32_t;
constexpr U32 PC = 0x7cfc58, Word = 0xe5846008, SVC = 0xfffb90;
inline std::atomic<std::int64_t> requested{0};
inline std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void Request() { requested.store(Now()); }
inline void Cancel() { requested.store(0); }
inline U32 Instruction(U32 pc, U32 word) {
    return pc == PC && word == Word ? 0xef000000u | SVC : word;
}
template<class M> bool CodeMatches(M& m) {
    // Supported fork input routine, including the controller-to-touch routing.
    U32 hash = 2166136261u;
    for (U32 at = 0x7cfba8; at < 0x7cfcbc; at += 4) {
        const auto word = m.Read(at);
        if (!word) return false;
        for (unsigned byte = 0; byte < 4; ++byte)
            hash = (hash ^ ((*word >> (byte * 8)) & 255u)) * 16777619u;
    }
    return hash == 0x5fea8612u;
}
template<class M, class R> void Update(M& m, R& regs, std::int64_t now = Now()) {
    // Reproduce STR r6,[r4,#8]. The verified routine uses the fixed pad state.
    m.Write(regs[4] + 8, regs[6]);
    if (regs[4] == 0x72a000) {
        const auto when = requested.exchange(0);
        if (when > 0 && now >= when && now - when <= 250)
            regs[5] |= 0x404u; // Pictograph touch down + pressed, no item input.
        // The remaining original routine applies the native usability checks.
    } else {
        Cancel();
    }
    regs[15] = PC + 4;
}
} // namespace MM3D::Pictograph
