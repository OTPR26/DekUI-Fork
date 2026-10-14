// SPDX-License-Identifier: GPL-2.0-or-later
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "mm3d/visibility_depth.h"
using namespace MM3D::VisibilityDepth;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        struct LiveMemory {
            std::unordered_map<U32, U32> words;
            std::optional<U32> Read(U32 at) {
                const auto it = words.find(at);
                return it == words.end() ? std::nullopt : std::optional<U32>{it->second};
            }
        } live{{{0x72f5c0, 0x200000}, {0x20016c, 0x300000},
                {0x30013c, 0x0402}, {0x300148, 5}}};
        Check(LiveContext(live, 11, 7) == Context{11, 0, 7, 0x300000, 5}, "live context identity");
        live.words[0x300148] = 6;
        Check(LiveContext(live, 12, 7)->scene == 6, "refresh rechecks scene even without queries");
        live.words[0x30013c] = 0;
        Check(!LiveContext(live, 12, 7), "non-play context disables background refresh");
        live.words[0x30013c] = 0x0402;
        live.words[0x300148] = 0x75;
        Check(!LiveContext(live, 12, 7), "invalid scene disables refresh");
        live.words[0x72f5c0] = 0;
        Check(!LiveContext(live, 12, 7), "missing owner disables refresh");
        Context a{10, 1, 7, 0x123000, 5}, b = a;
        Check(Fresh(a, b, 100, 100), "same frame");
        ++b.frame;
        Check(Fresh(a, b, 100, 100'000'100), "one frame at time bound");
        Check(!Fresh(a, b, 100, 100'000'101), "pause/age rejection");
        for (const U64 elapsed : {57'000'000ULL, 58'000'000ULL, 60'000'000ULL})
            Check(Fresh(a, b, 100, 100 + elapsed), "slow previous frame must not force permanent fallback");
        Check(CheckFreshness(a, b, 100, 100'000'101) == Freshness::WallAge,
              "wall-age rejection classified");
        ++b.frame;
        Check(!Fresh(a, b, 100, 101), "two frames rejected");
        Check(CheckFreshness(a, b, 100, 101) == Freshness::FrameAge,
              "frame-age rejection classified independently of wall age");
        b.frame = 9; Check(!Fresh(a, b, 100, 101), "frame rollback rejected");
        b = a; ++b.scene; Check(!Fresh(a, b, 100, 101), "scene change rejected");
        Check(CheckFreshness(a, b, 100, 101) == Freshness::Scene, "scene rejection classified");
        b = a; ++b.owner; Check(!Fresh(a, b, 100, 101), "new game context rejected");
        b = a; ++b.generation; Check(!Fresh(a, b, 100, 101), "mode switch rejected");
        Check(!Fresh(a, a, 100, 99), "time rollback rejected");
        Check(Physical(0x1f000000, 0) == 0x18000000, "VRAM start");
        Check(Physical(0x1f000000, 0x17ffff) == 0x185ffffc, "VRAM end");
        for (const auto& [base, index] : std::array<std::array<U32, 2>, 5>{{
            {0x1efffffc, 0}, {0x1f600000, 0}, {0x1f000001, 0},
            {0x1f000000, 0x40000000}, {0xfffffff0, 0x7fffffffu}}})
            Check(!Physical(base, index), "reject unaligned, non-VRAM and overflowing address");
        // Independent forward Morton addressing must invert to every pixel,
        // including a non-tile-aligned surface base (the game uses ...7fc0).
        constexpr U32 width = 512, height = 400;
        for (U32 y = 0; y < height; ++y) for (U32 x = 0; x < width; ++x) {
            U32 morton = 0;
            for (U32 bit = 0; bit < 3; ++bit)
                morton |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1);
            const U32 offset = 4 * ((y / 8) * width * 8 + (x / 8) * 64 + morton);
            Check(LinearIndex(offset, width, height) == (height - 1 - y) * width + x,
                  "all tiled offsets invert exactly");
        }
        Check(!LinearIndex(width * height * 4, width, height), "out of range pixel rejected");
        Check(!LinearIndex(1, width, height), "unaligned pixel rejected");
        Check(Instruction(LoadPC, LoadWord) == (0xef000000U | (FirstSVC + 1)), "exact load patched");
        Check(Instruction(LoadPC + 4, LoadWord) == LoadWord, "other address untouched");
        Check(Instruction(LoadPC, LoadWord ^ 1) == (LoadWord ^ 1), "other instruction untouched");
        struct Memory {
            std::vector<U32> code;
            std::optional<U32> Read(U32 at) {
                if (at < 0x183d88 || at >= 0x183f3c) return {};
                return code.at((at - 0x183d88) / 4);
            }
        } memory{std::vector<U32>(109)};
        Check(!CodeMatches(memory), "unsupported code rejected");
        // Optional local verification against the user's binary; not a CI asset.
        if (argc > 1) {
            std::ifstream file(argv[1], std::ios::binary);
            file.seekg(0x183d88 - 0x100000);
            file.read(reinterpret_cast<char*>(memory.code.data()), 109 * 4);
            Check(bool(file) && CodeMatches(memory), "supported local instruction signature");
            for (auto& word : memory.code) {
                word ^= 1; Check(!CodeMatches(memory), "any modified instruction disables hook"); word ^= 1;
            }
        }
        std::cout << "PASS visibility depth: bounded age, identity, address overflow, all tiled pixels, code guards\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
