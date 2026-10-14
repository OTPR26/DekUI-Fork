// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace MM3D::VisibilityDepth {
using U32 = std::uint32_t;
using U64 = std::uint64_t;
constexpr U32 EntryPC = 0x183d90, LoadPC = 0x183e98;
constexpr U32 EntryWord = 0xe3a00000, LoadWord = 0xe7920100;
constexpr U32 FirstSVC = 0xfffa90;
constexpr U64 Title = 0x0004000000125500ULL;

inline U32 Instruction(U32 pc, U32 word) {
    if (pc == EntryPC && word == EntryWord) return 0xef000000U | FirstSVC;
    if (pc == LoadPC && word == LoadWord) return 0xef000000U | (FirstSVC + 1);
    return word;
}
// Restoration USA: validate the entire 109-word visibility routine, including
// the load, tiled-coordinate calculation, comparison, loops and literals.
// The JIT's reported PC 0x183e20 is NOT the load instruction.
template<class M> bool CodeMatches(M& memory) {
    U64 hash = 14695981039346656037ULL;
    for (U32 pc = 0x183d88; pc < 0x183f3c; pc += 4) {
        const auto word = memory.Read(pc);
        if (!word) return false;
        hash = (hash ^ *word) * 1099511628211ULL;
    }
    return hash == 0x982a6670cca5373fULL;
}

struct Context {
    U64 frame{}, batch{};
    U32 generation{}, owner{}, scene{};
    bool operator==(const Context&) const = default;
};
inline bool SameScene(const Context& a, const Context& b) {
    return a.generation == b.generation && a.owner == b.owner && a.scene == b.scene;
}
// Shared by the verified query hook and background refresh. Re-read the live
// game identity so an idle query cannot keep history alive across a scene change.
template<class M> std::optional<Context> LiveContext(M& memory, U64 frame, U32 generation) {
    const auto pointer = [](U32 p) { return p >= 0x100000 && p < 0x3ffff000 && !(p & 3); };
    const auto root = memory.Read(0x72f5c0);
    const auto game = root && pointer(*root) ? memory.Read(*root + 0x16c) : std::nullopt;
    if (game && pointer(*game)) {
        const auto state = memory.Read(*game + 0x13c), scene = memory.Read(*game + 0x148);
        if (state && (*state & 0xffff) == 0x0402 && scene && (*scene & 0xffff) <= 0x74)
            return Context{frame, 0, generation, *game, *scene};
    }
    return {};
}
// A previous game frame can take 57-60ms on Deck. The old 50ms bound rejected
// every completed snapshot at that cadence, forcing another synchronous read.
// Keep the one-frame bound, with a finite wall-clock cap for long stalls/pauses.
constexpr U64 MaxAgeNs = 100'000'000;
enum class Freshness { Fresh, Scene, FrameAge, WallAge };
inline Freshness CheckFreshness(const Context& sample, const Context& request,
                                U64 captured_ns, U64 now_ns) {
    if (!SameScene(sample, request)) return Freshness::Scene;
    if (request.frame < sample.frame || request.frame - sample.frame > 1)
        return Freshness::FrameAge;
    if (now_ns < captured_ns || now_ns - captured_ns > MaxAgeNs)
        return Freshness::WallAge;
    return Freshness::Fresh;
}
inline bool Fresh(const Context& sample, const Context& request, U64 captured_ns, U64 now_ns) {
    return CheckFreshness(sample, request, captured_ns, now_ns) == Freshness::Fresh;
}
inline std::optional<U32> Physical(U32 base, U32 index) {
    const U64 address = U64(base) + U64(index) * 4;
    if (address < 0x1f000000 || address > 0x1f5ffffc || (address & 3)) return {};
    return U32(address) - 0x1f000000 + 0x18000000;
}
// Convert a guest tiled D24S8 word offset into the linear, vertically inverted
// output of DepthToBuffer. This never writes older data into guest RAM.
inline std::optional<U32> LinearIndex(U32 offset, U32 width, U32 height) {
    if ((offset & 3) || !width || !height || width % 8 || height % 8 ||
        U64(offset) >= U64(width) * height * 4) return {};
    const U32 pixel = offset / 4, tile = pixel / 64, morton = pixel % 64;
    U32 x = (tile % (width / 8)) * 8, y = (tile / (width / 8)) * 8;
    for (unsigned bit = 0; bit < 3; ++bit) {
        x |= ((morton >> (2 * bit)) & 1) << bit;
        y |= ((morton >> (2 * bit + 1)) & 1) << bit;
    }
    return (height - 1 - y) * width + x;
}
struct Backend {
    virtual ~Backend() = default;
    virtual std::optional<U32> ReadVisibilityDepth(U32 address, const Context& context) = 0;
    virtual void ResetVisibilityDepth() = 0;
};
inline std::atomic<Backend*> backend{};
struct Hook {
    Context context{};
    bool active{};
    template<class M> void Begin(M& memory, bool enabled, U64 frame, U32 generation) {
        active = false;
        ++context.batch;
        if (enabled && CodeMatches(memory)) {
            if (const auto live = LiveContext(memory, frame, generation)) {
                const auto batch = context.batch;
                context = *live;
                context.batch = batch;
                active = true;
            }
        }
        if (!active) if (auto* b = backend.load()) b->ResetVisibilityDepth();
    }
    std::optional<U32> Read(U32 base, U32 index, bool enabled, U32 generation) const {
        const auto address = Physical(base, index);
        auto* b = backend.load();
        if (!active || !enabled || context.generation != generation || !address || !b) return {};
        return b->ReadVisibilityDepth(*address, context);
    }
};
} // namespace MM3D::VisibilityDepth
