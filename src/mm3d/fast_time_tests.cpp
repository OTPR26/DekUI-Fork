// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/fast_time.h"
#include "mm3d/movement_speed.h"
static_assert(MM3D::FastTime::FirstSVC >= MM3D::MovementSpeed::FirstSVC +
              MM3D::MovementSpeed::PCs.size(), "Fast Time must not intercept movement speed hooks");
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using namespace MM3D::FastTime;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Memory {
    std::unordered_map<U32, U32> words;
    std::optional<U32> Read(U32 at) const {
        const auto it = words.find(at);
        return it == words.end() ? std::nullopt : std::optional<U32>(it->second);
    }
    void Write(U32 at, U32 value) { words.at(at) = value; }
};
constexpr U32 Static = 0x20000, Context = 0x30000;
using Registers = std::array<U32, 16>;
Memory MakeMemory(U32 rate = 2, U32 scene = 3) {
    Memory m;
    m.words[StaticPointer] = Static;
    m.words[Static + 0x30] = (rate << 16) | 0xabcd;
    m.words[Context + (SceneSpeedOffset & ~3u)] = (scene << 16) | 0x1234;
    m.words[Common + 0x20] = 0x56780000;
    m.words[Common + 0x10] = 0;
    m.words[Common + 0x36a0] = 0x9876;
    return m;
}
// Execute the verified native additions around the hooks and Restoration's
// existing schedule copy. Deliberately keep these separate from the rate policy.
U32 Tick(Memory& m, bool fast, unsigned frame, bool slow) {
    Registers r{}; r[0] = Static; r[4] = Context;
    Update(m, r, FirstSVC, fast);
    const U32 speed = r[2];
    const U32 extra = speed ? *m.Read(Common + 0x10) : 0;
    const U32 time = (*Half(m, Common + 0x20) + speed + extra) & 0xffffu;
    StoreHalf(m, Common + 0x20, time);
    StoreHalf(m, Common + 0x36a2, *Half(m, Common + 0x36a2) + *Half(m, Static + 0x32) + extra);
    if (speed) {
        if (slow) {
            r[4] = Common;
            Update(m, r, FirstSVC + 2, fast);
            if (r[15] == InvertedPC + 4 && frame % 3 == 0)
                StoreHalf(m, Common + 0x20, r[3] - 1u);
        }
        StoreHalf(m, Common + 0x36a2, *Half(m, Common + 0x20));
    }
    Require(Half(m, Common + 0x20) == Half(m, Common + 0x36a2), "clock/schedule mismatch");
    Require((*m.Read(Static + 0x30) & 0xffffu) == 0xabcd, "adjacent static field changed");
    Require((*m.Read(Common + 0x20) >> 16) == 0x5678, "adjacent save field changed");
    Require((*m.Read(Common + 0x36a0) & 0xffffu) == 0x9876, "environment time overwritten");
    return *Half(m, Common + 0x20);
}
void CheckBinary(const char* path) {
    std::ifstream file(path, std::ios::binary);
    Require(bool(file), "cannot open guest code");
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
    Memory m;
    for (const auto& sig : Signatures) {
        Require(sig.start >= 0x100000 && sig.start - 0x100000 + sig.size <= bytes.size(), "signature outside binary");
        for (U32 p = sig.start; p < sig.start + sig.size; p += 4) {
            const auto i = p - 0x100000;
            m.words[p] = U32(bytes[i]) | (U32(bytes[i+1]) << 8) |
                         (U32(bytes[i+2]) << 16) | (U32(bytes[i+3]) << 24);
        }
    }
    Require(CodeMatches(m), "verified binary signatures differ");
    for (U32 i = 0; i < Hooks.size(); ++i)
        Require(Instruction(Hooks[i].pc, *m.Read(Hooks[i].pc)) == (0xef000000u | (FirstSVC+i)), "binary hook mismatch");
    for (const auto& sig : Signatures) {
        m.words[sig.start] ^= 1;
        Require(!CodeMatches(m), "changed binary accepted");
        m.words[sig.start] ^= 1;
    }
}
int main(int argc, char** argv) try {
    Require(!enabled.load(), "default must be off");
    for (U32 i = 0; i < Hooks.size(); ++i) {
        const auto h = Hooks[i];
        Require(Instruction(h.pc, h.word) == (0xef000000u | (FirstSVC+i)), "instruction replacement");
        Require(Instruction(h.pc, h.word ^ 1) == (h.word ^ 1), "wrong instruction patched");
        Require(Instruction(h.pc + 4, h.word) == h.word, "wrong PC patched");
    }
    Memory missing;
    Require(!CodeMatches(missing), "unreadable binary accepted");
    // All clock values, including midnight, dawn, dusk and wraparound.
    for (U32 start = 0; start < 65536; ++start) {
        for (const bool fast : {false, true}) {
            for (const bool slow : {false, true}) {
                auto m = MakeMemory();
                StoreHalf(m, Common + 0x20, start);
                StoreHalf(m, Common + 0x36a2, start);
                m.words[Common + 0x10] = slow ? 0xffffffffu : 0;
                U32 end = start;
                for (unsigned frame = 0; frame < 3; ++frame) end = Tick(m, fast, frame, slow);
                const U32 delta = slow ? (fast ? 3 : 2) : (fast ? 9 : 6);
                Require(end == ((start + delta) & 0xffffu), "normal/inverted rate or wrap incorrect");
            }
        }
    }
    auto m = MakeMemory();
    Tick(m, true, 1, false);
    Tick(m, false, 2, false);
    Require(Half(m, Static + 0x32) == 2u, "toggle off did not restore rate");
    m = MakeMemory(3, 5);
    Tick(m, false, 1, false);
    Require(Half(m, Static + 0x32) == 3u, "native first cycle altered");
    Tick(m, true, 2, false);
    Require(Half(m, Static + 0x32) == 3u, "first-cycle speed compounded");
    for (U32 speed : {0u, 1u, 4u, 267u, 0xffffu}) {
        Require(LiveRate(speed, 3, true) == speed, "special/frozen rate changed");
        Require(LiveRate(speed, 3, false) == speed, "special/frozen rate changed on disable");
    }
    for (U32 scene : {0u, 1u, 2u, 4u, 6u, 255u})
        Require(LiveRate(2, scene, true) == 2, "unrecognized scene rate changed");
    // Scene loads, frozen indoor scenes and scripted acceleration preserve native stores.
    for (U32 speed : {0u, 1u, 2u, 3u, 267u}) {
        for (bool fast : {false, true}) {
            m = MakeMemory(); Registers r{}; r[0] = Static; r[6] = speed;
            const auto before = r;
            Update(m, r, FirstSVC + 1, fast);
            Require(Half(m, Static + 0x32) == (fast && speed == 2 ? 3u : speed), "scene rate store");
            Require(r[15] == ScenePC + 4, "scene continuation");
            r[15] = before[15]; Require(r == before, "scene registers clobbered");
        }
    }
    // Restoration exceptions retain the native base+extra behavior.
    m = MakeMemory(); m.words[Common + 0x10] = 0xffffffffu;
    Require(Tick(m, true, 0, false) == 2, "Restoration event exception changed");
    // Frozen time remains frozen even while inverted.
    m = MakeMemory(0, 0); m.words[Common + 0x10] = 0xffffffffu;
    Require(Tick(m, true, 0, true) == 0, "frozen time advanced");
    if (argc > 1) CheckBinary(argv[1]);
    std::cout << "PASS fast time: rates, schedules, wraparound, toggles, scene setup, frozen/scripted time and hook guards\n";
} catch (const std::exception& e) {
    std::cerr << "FAIL fast time: " << e.what() << '\n';
    return 1;
}
