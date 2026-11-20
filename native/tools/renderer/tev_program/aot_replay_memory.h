#pragma once

#include "oot3d_native_a32_memory.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

// Developer replay only. Guest mappings need not start or end on a page.
// Own the initial bytes once; reset only changed chunks without touching the
// live game's pointers, observer state, or write-generation bookkeeping.
class AotReplayMemoryReset final {
public:
    AotReplayMemoryReset(const Oot3dNativeGame::NativeA32Memory& initial,
                         Oot3dNativeGame::NativeA32Memory& output,
                         std::array<uint8_t*, 1U << 20>& pageTable)
        : snapshot(initial.CaptureState()) {
        for (const auto& region : snapshot.at("regions")) {
            const auto base = region.at("base_address").get<uint32_t>();
            const auto& bytes = region.at("bytes").get_binary();
            auto* destination = const_cast<uint8_t*>(output.GetReadPointer(base, bytes.size()));
            if (!destination) throw std::runtime_error("Replay mapping changed during invocation");
            const bool writable = region.at("writable").get<bool>();
            const bool executable = region.at("executable").get<bool>();
            for (size_t offset = 0; offset < bytes.size();) {
                const auto address = base + static_cast<uint32_t>(offset);
                const size_t count = std::min(bytes.size() - offset, size_t{4096} - (address & 4095U));
                if (writable && !executable && count == 4096)
                    pageTable[address >> 12U] = destination + offset;
                if (std::memcmp(destination + offset, bytes.data() + offset, count) != 0) {
                    if (!writable || executable)
                        throw std::runtime_error("Replay reference modified protected or executable memory");
                    spans.push_back({destination + offset, bytes.data() + offset, count,
                                     {destination + offset, destination + offset + count}});
                    byteCount += count;
                }
                offset += count;
            }
        }
    }
    void Restore() const noexcept {
        for (const auto& span : spans) std::memcpy(span.Destination, span.Initial, span.Size);
    }
    size_t ByteCount() const noexcept { return byteCount; }

    bool MatchesOutput() const noexcept {
        return std::all_of(spans.begin(), spans.end(), [](const auto& span) {
            return std::memcmp(span.Destination, span.Expected.data(), span.Size) == 0;
        });
    }
private:
    nlohmann::json snapshot;
    struct Span {
        uint8_t* Destination;
        const uint8_t* Initial;
        size_t Size;
        std::vector<uint8_t> Expected;
    };
    std::vector<Span> spans;
    size_t byteCount = 0;
};
