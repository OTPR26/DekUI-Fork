#pragma once
// Developer generators: the entries of one SPIR-V pack with the strings and
// modules they view, written as the pack file or compared with it.
#include "fast/renderer/spirv_pack.h"
#include <deque>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

struct SpirvPackFile {
    std::deque<std::string> Strings;
    std::deque<std::vector<uint32_t>> Modules;
    std::vector<Fast::Renderer::SpirvPackEntry> Entries;

    std::string_view Keep(std::string value) { return Strings.emplace_back(std::move(value)); }
    Fast::Renderer::SpirvPackEntry& Add(Fast::Renderer::SpirvPackKind kind, std::vector<uint32_t> words) {
        auto& entry = Entries.emplace_back();
        entry.Kind = kind;
        entry.Spirv = Modules.emplace_back(std::move(words));
        return entry;
    }
    void Write(const std::string& path) const {
        const auto bytes = Fast::Renderer::WriteSpirvPack(Entries);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())))
            throw std::runtime_error("cannot write SPIR-V pack " + path);
    }
    // The existing pack holds exactly these entries, byte for byte.
    bool Matches(const std::string& path) const {
        std::ifstream file(path, std::ios::binary);
        const std::vector<unsigned char> existing((std::istreambuf_iterator<char>(file)), {});
        return bool(file) && existing == Fast::Renderer::WriteSpirvPack(Entries);
    }
};
