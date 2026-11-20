#pragma once
// Precompiled SPIR-V packs: one family of shader modules, with the identity
// each module is looked up by, as one binary file embedded in the program
// instead of C++ literals. Developer generators write packs; the program reads
// each embedded pack once into its artifact table.
//
// Layout (little-endian, offsets in bytes from the start of the pack):
//   header   u32 magic "SPVK", u32 version, u32 entry count, u32 pack size
//   entries  count x 64 bytes: u32 kind, stage, Vulkan minor, flags;
//            u64 identity[3]; u32 source offset, source size,
//            defines offset, define count, SPIR-V offset, SPIR-V words
//   payload  defines (u32 key offset, key size, value offset, value size),
//            strings (UTF-8, unterminated), SPIR-V words; every block starts
//            4-byte aligned. Identical strings and modules are stored once.
//
// Embedding, in the .cpp next to the pack (cmake/spirv_pack.cmake makes the
// object depend on the pack and, without #embed, generates the byte list):
//   TRIAEVUM_SPIRV_PACK_EMBED_BEGIN
//   alignas(16) const unsigned char kPack[] = {
//   #if TRIAEVUM_SPIRV_PACK_EMBED
//   #embed "family.spvpack"
//   #else
//   #include "family.spvpack.inc"
//   #endif
//   };
//   TRIAEVUM_SPIRV_PACK_EMBED_END
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TRIAEVUM_SPIRV_PACK_EMBED
#if defined(__has_embed)
#define TRIAEVUM_SPIRV_PACK_EMBED 1
#else
#define TRIAEVUM_SPIRV_PACK_EMBED 0
#endif
#endif
#if defined(__clang__)
#define TRIAEVUM_SPIRV_PACK_EMBED_BEGIN \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wc23-extensions\"")
#define TRIAEVUM_SPIRV_PACK_EMBED_END _Pragma("clang diagnostic pop")
#else
#define TRIAEVUM_SPIRV_PACK_EMBED_BEGIN
#define TRIAEVUM_SPIRV_PACK_EMBED_END
#endif

namespace Fast::Renderer {

static_assert(std::endian::native == std::endian::little, "SPIR-V packs are little-endian");

enum class SpirvPackKind : uint32_t { PicaFragment = 1, Pass = 2, PicaVertex = 3 };
inline constexpr uint32_t kSpirvPackMagic = 0x4b565053U; // "SPVK"
inline constexpr uint32_t kSpirvPackVersion = 1U;
inline constexpr uint32_t kSpirvPackSeparateSamplers = 1U; // PicaFragment flag

struct SpirvPackEntry {
    SpirvPackKind Kind = SpirvPackKind::Pass;
    uint32_t Stage = 0; // SpirvStage for passes
    uint32_t VulkanMinor = 0;
    uint32_t Flags = 0;
    std::array<uint64_t, 3> Identity{}; // PicaShaderSourceIdentity for PICA programs
    std::string_view Source;
    std::vector<std::pair<std::string_view, std::string_view>> Defines;
    std::span<const uint32_t> Spirv;
};

namespace SpirvPackDetail {
inline constexpr size_t kHeaderBytes = 16;
inline constexpr size_t kEntryBytes = 64;
inline uint32_t U32(const unsigned char* at) { uint32_t value; std::memcpy(&value, at, 4); return value; }
inline uint64_t U64(const unsigned char* at) { uint64_t value; std::memcpy(&value, at, 8); return value; }
} // namespace SpirvPackDetail

// Reads a pack. The bytes must stay alive and be 4-byte aligned; the entries
// point into them. A malformed pack yields no entries and the reason.
inline std::vector<SpirvPackEntry> ReadSpirvPack(std::span<const unsigned char> pack,
                                                 std::string* error = nullptr) {
    using namespace SpirvPackDetail;
    const auto fail = [&](const char* reason) {
        if (error) *error = reason;
        return std::vector<SpirvPackEntry>{};
    };
    if (pack.size() < kHeaderBytes || reinterpret_cast<uintptr_t>(pack.data()) % 4 != 0)
        return fail("SPIR-V pack is truncated or unaligned");
    if (U32(pack.data()) != kSpirvPackMagic || U32(pack.data() + 4) != kSpirvPackVersion)
        return fail("not a SPIR-V pack of this version");
    const size_t count = U32(pack.data() + 8);
    if (U32(pack.data() + 12) != pack.size() || count > (pack.size() - kHeaderBytes) / kEntryBytes)
        return fail("SPIR-V pack size does not match its header");
    const auto inside = [&](size_t offset, size_t bytes) {
        return offset <= pack.size() && bytes <= pack.size() - offset;
    };
    const auto text = [&](size_t offset, size_t bytes) {
        return std::string_view(reinterpret_cast<const char*>(pack.data() + offset), bytes);
    };
    std::vector<SpirvPackEntry> entries(count);
    for (size_t index = 0; index < count; ++index) {
        const unsigned char* at = pack.data() + kHeaderBytes + index * kEntryBytes;
        auto& entry = entries[index];
        const uint32_t kind = U32(at);
        if (kind < uint32_t(SpirvPackKind::PicaFragment) || kind > uint32_t(SpirvPackKind::PicaVertex))
            return fail("unknown SPIR-V pack entry kind");
        entry.Kind = SpirvPackKind(kind);
        entry.Stage = U32(at + 4);
        entry.VulkanMinor = U32(at + 8);
        entry.Flags = U32(at + 12);
        entry.Identity = {U64(at + 16), U64(at + 24), U64(at + 32)};
        const size_t sourceOffset = U32(at + 40), sourceSize = U32(at + 44);
        const size_t definesOffset = U32(at + 48), defineCount = U32(at + 52);
        const size_t spirvOffset = U32(at + 56), spirvWords = U32(at + 60);
        if (!inside(sourceOffset, sourceSize) || !inside(definesOffset, defineCount * 16) ||
            spirvOffset % 4 != 0 || !inside(spirvOffset, spirvWords * 4))
            return fail("SPIR-V pack entry exceeds the pack");
        entry.Source = text(sourceOffset, sourceSize);
        for (size_t define = 0; define < defineCount; ++define) {
            const unsigned char* pair = pack.data() + definesOffset + define * 16;
            const size_t keyOffset = U32(pair), keySize = U32(pair + 4);
            const size_t valueOffset = U32(pair + 8), valueSize = U32(pair + 12);
            if (!inside(keyOffset, keySize) || !inside(valueOffset, valueSize))
                return fail("SPIR-V pack define exceeds the pack");
            entry.Defines.emplace_back(text(keyOffset, keySize), text(valueOffset, valueSize));
        }
        entry.Spirv = {reinterpret_cast<const uint32_t*>(pack.data() + spirvOffset), spirvWords};
    }
    return entries;
}

// Developer generators: the pack of these entries, in order, byte-for-byte
// deterministic.
inline std::vector<unsigned char> WriteSpirvPack(std::span<const SpirvPackEntry> entries) {
    using namespace SpirvPackDetail;
    std::vector<unsigned char> payload;
    const size_t base = kHeaderBytes + entries.size() * kEntryBytes;
    const auto align = [&] { payload.resize((payload.size() + 3) & ~size_t(3)); };
    const auto append = [&](const void* data, size_t bytes) {
        align();
        const size_t offset = base + payload.size();
        payload.insert(payload.end(), static_cast<const unsigned char*>(data),
                       static_cast<const unsigned char*>(data) + bytes);
        return uint32_t(offset);
    };
    std::map<std::string_view, uint32_t> strings;
    std::map<std::vector<uint32_t>, uint32_t> modules;
    const auto string = [&](std::string_view value) {
        const auto [it, added] = strings.try_emplace(value, 0U);
        if (added) it->second = append(value.data(), value.size());
        return it->second;
    };
    std::vector<unsigned char> table(base, 0);
    const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(table.data() + at, &value, 4); };
    const auto put64 = [&](size_t at, uint64_t value) { std::memcpy(table.data() + at, &value, 8); };
    for (size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const size_t at = kHeaderBytes + index * kEntryBytes;
        std::vector<uint32_t> defines;
        for (const auto& [key, value] : entry.Defines) {
            defines.push_back(string(key));
            defines.push_back(uint32_t(key.size()));
            defines.push_back(string(value));
            defines.push_back(uint32_t(value.size()));
        }
        const uint32_t source = string(entry.Source);
        const uint32_t definesOffset = defines.empty() ? 0U : append(defines.data(), defines.size() * 4);
        std::vector<uint32_t> words(entry.Spirv.begin(), entry.Spirv.end());
        const auto [module, added] = modules.try_emplace(std::move(words), 0U);
        if (added) module->second = append(module->first.data(), module->first.size() * 4);
        put32(at, uint32_t(entry.Kind));
        put32(at + 4, entry.Stage);
        put32(at + 8, entry.VulkanMinor);
        put32(at + 12, entry.Flags);
        put64(at + 16, entry.Identity[0]);
        put64(at + 24, entry.Identity[1]);
        put64(at + 32, entry.Identity[2]);
        put32(at + 40, source);
        put32(at + 44, uint32_t(entry.Source.size()));
        put32(at + 48, definesOffset);
        put32(at + 52, uint32_t(entry.Defines.size()));
        put32(at + 56, module->second);
        put32(at + 60, uint32_t(entry.Spirv.size()));
    }
    align();
    put32(0, kSpirvPackMagic);
    put32(4, kSpirvPackVersion);
    put32(8, uint32_t(entries.size()));
    put32(12, uint32_t(base + payload.size()));
    table.insert(table.end(), payload.begin(), payload.end());
    return table;
}

} // namespace Fast::Renderer
