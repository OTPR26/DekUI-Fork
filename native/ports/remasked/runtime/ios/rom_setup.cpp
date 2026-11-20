// SPDX-License-Identifier: GPL-2.0-or-later
// CTR container, backwards LZSS and process layout follow TriAevum's
// ctr_rom.py and build_process_manifest.py. No keys or decryption are used.
#include "rom_setup.h"

#include <CommonCrypto/CommonDigest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ReMaskedIOS {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
constexpr uint64_t MediaUnit = 0x200;
constexpr uint64_t MaxRomSize = 4ULL << 30;
constexpr uint64_t MaxCodeSize = 32ULL << 20;
constexpr uint64_t ProgramId = 0x0004000000125500;
constexpr uint64_t Page = 0x1000;

uint64_t Little(std::span<const uint8_t> bytes, size_t offset, size_t count = 4) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}

bool Magic(std::span<const uint8_t> bytes, size_t offset, std::string_view magic) {
    return std::equal(magic.begin(), magic.end(), bytes.begin() + offset);
}

bool Fits(uint64_t offset, uint64_t size, uint64_t end) {
    return size && offset <= end && size <= end - offset;
}

Bytes Read(std::ifstream& file, uint64_t offset, size_t size) {
    Bytes bytes(size);
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) throw std::runtime_error("The ROM ended unexpectedly.");
    return bytes;
}

std::string Hash(const Bytes& bytes) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest;
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    std::string hex;
    for (auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

Bytes DecodeCode(const Bytes& code) {
    if (code.size() < 8) throw std::runtime_error("The ROM has an invalid compressed code section.");
    const auto footer = Little(code, code.size() - 8);
    const auto footerSize = footer >> 24;
    const auto encodedSize = footer & 0xffffff;
    const auto outputSize = code.size() + Little(code, code.size() - 4);
    if (footerSize < 8 || footerSize > encodedSize || encodedSize > code.size() || outputSize > MaxCodeSize)
        throw std::runtime_error("The ROM has an invalid compressed code section.");

    Bytes output(outputSize);
    std::copy(code.begin(), code.end(), output.begin());
    auto source = code.size() - footerSize;
    const auto end = code.size() - encodedSize;
    auto dest = output.size();
    while (source > end && dest) {
        auto control = code[--source];
        for (int bit = 0; bit < 8 && source > end && dest; ++bit, control <<= 1) {
            if (control & 0x80) {
                if (source - end < 2) throw std::runtime_error("The compressed code section is truncated.");
                source -= 2;
                const auto entry = Little(code, source, 2);
                const auto count = (entry >> 12) + 3;
                const auto distance = (entry & 0xfff) + 2;
                if (dest < count || distance >= output.size() - dest)
                    throw std::runtime_error("The compressed code section has an invalid reference.");
                for (size_t i = 0; i < count; ++i) {
                    output[dest - 1] = output[dest + distance];
                    --dest;
                }
            } else {
                output[--dest] = code[--source];
            }
        }
    }
    return output;
}

struct Title {
    uint64_t base;
    uint64_t end;
    Bytes header;
};

Title FindTitle(std::ifstream& file, uint64_t size) {
    const auto first = Read(file, 0, MediaUnit);
    if (Magic(first, 0x100, "NCCH") && Little(first, 0x118, 8) == ProgramId) return {0, size, first};
    if (Magic(first, 0x100, "NCSD")) {
        for (size_t entry = 0x120; entry < 0x160; entry += 8) {
            const auto base = Little(first, entry) * MediaUnit;
            const auto length = Little(first, entry + 4) * MediaUnit;
            if (!base || length < MediaUnit || !Fits(base, length, size)) continue;
            auto header = Read(file, base, MediaUnit);
            if (Magic(header, 0x100, "NCCH") && Little(header, 0x118, 8) == ProgramId)
                return {base, base + length, std::move(header)};
        }
    }
    throw std::runtime_error("Choose a decrypted ROM of the supported Majora's Mask 3D title.");
}

Bytes ReadCode(std::ifstream& file, const Title& title, bool compressed) {
    const auto exefs = title.base + Little(title.header, 0x1a0) * MediaUnit;
    const auto size = Little(title.header, 0x1a4) * MediaUnit;
    if (exefs == title.base || size < MediaUnit || !Fits(exefs, size, title.end))
        throw std::runtime_error("The ROM has an invalid executable partition.");

    const auto header = Read(file, exefs, MediaUnit);
    for (size_t entry = 0; entry < 0x80; entry += 0x10) {
        if (!Magic(header, entry, std::string_view(".code\0\0\0", 8))) continue;
        const auto offset = MediaUnit + Little(header, entry + 8);
        const auto length = Little(header, entry + 12);
        if (length > MaxCodeSize || !Fits(offset, length, size))
            throw std::runtime_error("The ROM has an invalid code section.");
        auto code = Read(file, exefs + offset, length);
        return compressed ? DecodeCode(code) : std::move(code);
    }
    throw std::runtime_error("The ROM does not have a readable code section. Check that it is decrypted.");
}

Json Value(uint64_t offset, uint64_t size, uint64_t value) {
    return {{"offset", offset}, {"size", size}, {"value", value}};
}

Json Region(const char* name, uint64_t address, uint64_t size, bool writable, Json values) {
    return {{"name", name}, {"address", address}, {"mapped_size", size},
            {"writable", writable}, {"executable", false}, {"initial_values", std::move(values)}};
}

Json Manifest(const Bytes& header, uint64_t codeSize, uint64_t romfsSize) {
    const uint64_t bss = Little(header, 0x3c);
    const uint64_t alignedBss = (bss + Page - 1) & ~(Page - 1);
    const uint64_t stack = Little(header, 0x1c);
    const size_t mode = header[0x20d] & 0xf;
    constexpr uint64_t memory[][3] = {
        {0x4000000, 0x2c00000, 0x1400000}, {}, {0x6000000, 0xc00000, 0x1400000},
        {0x5000000, 0x1c00000, 0x1400000}, {0x4800000, 0x2400000, 0x1400000},
        {0x2000000, 0x4c00000, 0x1400000}, {0x7c00000, 0x6400000, 0x2000000},
        {0xb200000, 0x2e00000, 0x2000000}
    };
    if (mode >= std::size(memory) || !memory[mode][0] || !stack || stack % Page || stack >= 0x10000000)
        throw std::runtime_error("The ROM has an invalid process layout.");
    const auto& [app, system, base] = memory[mode];
    Json segments = Json::array();
    uint64_t fileOffset = 0;
    uint64_t previousEnd = 0;
    constexpr const char* names[] = {"text", "rodata", "data_bss"};
    for (size_t i = 0; i < 3; ++i) {
        const auto address = Little(header, 0x10 + 0x10 * i);
        const uint64_t loaded = Little(header, 0x14 + 0x10 * i) * Page;
        const auto declared = Little(header, 0x18 + 0x10 * i);
        const uint64_t mapped = loaded + (i == 2 ? alignedBss : 0);
        if (!loaded || address % Page || address < previousEnd || declared > loaded ||
            !Fits(address, mapped, 0x10000000 - stack))
            throw std::runtime_error("The ROM has an invalid memory segment.");
        segments.push_back({{"name", names[i]}, {"address", address}, {"mapped_size", mapped},
                            {"declared_code_size", declared}, {"file_offset", fileOffset},
                            {"file_size", loaded}, {"writable", i == 2}, {"executable", i == 0}});
        fileOffset += loaded;
        previousEnd = address + mapped;
    }
    if (fileOffset != codeSize) throw std::runtime_error("The ROM's code size does not match its ExHeader.");

    const std::string name(header.begin(), std::find(header.begin(), header.begin() + 8, 0));
    Json config = Json::array({
        Value(0x01,1,0x3a), Value(0x03,1,2), Value(0x08,8,0x0004013000008002),
        Value(0x10,4,2), Value(0x14,1,1), Value(0x16,1,1), Value(0x18,4,0xf450),
        Value(0x30,4,mode), Value(0x40,4,app), Value(0x44,4,system), Value(0x48,4,base),
        Value(0x62,1,0x3a), Value(0x63,1,2), Value(0x64,4,2), Value(0x68,4,0xf450)
    });
    Json shared = Json::array({
        Value(0x04,1,1), Value(0x66,1,3), Value(0x67,1,2), Value(0x85,1,0x17), Value(0x86,1,1)
    });
    return {
        {"format", "oot3d_native_process_manifest_v1"},
        {"source", {{"exheader_path", "exheader.bin"}, {"exheader_sha256", Hash(header)},
            {"code_bin_path", "code.bin"}, {"code_bin_sha256", SupportedGameCodeSha256},
            {"code_bin_size", codeSize}, {"romfs_image_path", "romfs.bin"},
            {"romfs_image_file_size", romfsSize}, {"romfs_service_offset", Page},
            {"romfs_service_size", romfsSize - Page}, {"layout_authority", "CTR ExHeader"}}},
        {"process", {{"name", name}, {"entrypoint", Little(header, 0x10)}, {"page_size", Page},
            {"bss_declared_size", bss}, {"bss_mapped_size", alignedBss}, {"segments", segments},
            {"system_regions", Json::array({
                Region("ctr_vram", 0x1f000000, 0x600000, true, Json::array()),
                Region("ctr_dsp_ram", 0x1ff00000, 0x80000, true, Json::array()),
                Region("ctr_config_memory", 0x1ff80000, Page, false, config),
                Region("ctr_shared_page", 0x1ff81000, Page, false, shared)})},
            {"resource_limit", {{"limit_values", {0x18, app, 0x20, 0x20, 0x20, 8, 8, 0x10, 2, 0}},
                {"initial_values", {0, codeSize + alignedBss + stack, 1, 0, 0, 0, 0, 0, 0, 0}}}},
            {"linear_heap", {{"base_address", 0x14000000}, {"size", app}}},
            {"heap", {{"base_address", 0x08000000}, {"size", 0x08000000}}}}},
        {"primary_thread", {{"stack_base_address", 0x10000000 - stack}, {"stack_size", stack},
            {"tls_base_address", 0x1ff82000}, {"tls_size", Page}, {"thread_pointer", 0x1ff82000},
            {"argument0", 0}, {"initial_cpsr", 0x10}, {"initial_fpscr", 0x03c00010},
            {"priority", header[0x20f]}}}
    };
}

void Write(const fs::path& path, const char* data, size_t length) {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(data, length);
    stream.close();
}

void CopyRomfs(std::ifstream& source, const fs::path& path, uint64_t offset, uint64_t length) {
    std::ofstream dest(path, std::ios::binary);
    dest.exceptions(std::ios::badbit | std::ios::failbit);
    source.seekg(offset);
    Bytes buffer(256 * 1024);
    while (length) {
        const auto count = std::min<uint64_t>(buffer.size(), length);
        source.read(reinterpret_cast<char*>(buffer.data()), count);
        if (!source) throw std::runtime_error("The ROM ended while preparing game files.");
        dest.write(reinterpret_cast<const char*>(buffer.data()), count);
        length -= count;
    }
    dest.close();
}
}

void PrepareRom(const fs::path& rom, const fs::path& staging) {
    const auto size = fs::file_size(rom);
    if (size < MediaUnit || size > MaxRomSize) throw std::runtime_error("The ROM has an invalid size.");
    std::ifstream file(rom, std::ios::binary);
    const auto title = FindTitle(file, size);
    if (!(title.header[0x18d] & 2) || !Little(title.header, 0x180) ||
        !Fits(title.base + MediaUnit, 0x800, title.end))
        throw std::runtime_error("The ROM has an invalid ExHeader.");
    const auto header = Read(file, title.base + MediaUnit, 0x800);
    const auto code = ReadCode(file, title, header[0xd] & 1);
    if (Hash(code) != SupportedGameCodeSha256)
        throw std::runtime_error("The selected ROM is a different game revision than this build supports.");
    const auto romfs = title.base + Little(title.header, 0x1b0) * MediaUnit;
    const auto romfsSize = Little(title.header, 0x1b4) * MediaUnit;
    if (romfs == title.base || romfsSize < Page + 4 || !Fits(romfs, romfsSize, title.end) ||
        !Magic(Read(file, romfs, 4), 0, "IVFC") || Little(Read(file, romfs + Page, 4), 0) != 0x28)
        throw std::runtime_error("The ROM has an invalid RomFS. Check that it is decrypted.");

    const auto manifest = Manifest(header, code.size(), romfsSize).dump(2) + "\n";
    Write(staging / "exheader.bin", reinterpret_cast<const char*>(header.data()), header.size());
    Write(staging / "code.bin", reinterpret_cast<const char*>(code.data()), code.size());
    CopyRomfs(file, staging / "romfs.bin", romfs, romfsSize);
    Write(staging / "process-manifest.json", manifest.data(), manifest.size());
}
}
