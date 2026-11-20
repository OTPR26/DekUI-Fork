// Compares two native savestates of the same game moment, for example a state
// and its conversion to the current format. Prints a JSON report: the guest
// memory ranges that differ (virtual addresses) and every other differing
// document path. "graphics_only" is true when the only differences are
// renderer-owned runtime sections, the RomFS host binding, schema upgrades
// (fields only the second state has, section format names) and guest memory
// inside the render targets
// the second state declares (native_pica_guest_framebuffers).
#include "oot3d_native_a32_savestate.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

struct Span {
    uint64_t Begin = 0;
    uint64_t End = 0;
};

uint32_t ColorBytesPerPixel(uint32_t format) {
    return format == 0U ? 4U : format == 1U ? 3U : 2U;
}

uint32_t DepthBytesPerPixel(uint32_t format) {
    return format == 0U ? 2U : format == 2U ? 3U : format == 3U ? 4U : 0U;
}

// Render targets of a state, as virtual address spans.
std::vector<Span> FramebufferSpans(const Json& document) {
    std::vector<Span> spans;
    const auto runtime = document.find("runtime");
    if (runtime == document.end()) return spans;
    const auto framebuffers = runtime->find("native_pica_guest_framebuffers");
    if (framebuffers == runtime->end()) return spans;
    const auto& regions = document.at("dsp_hle").at("physical_regions");
    const auto toVirtual = [&](uint64_t physical, uint64_t size) -> std::optional<uint64_t> {
        for (const auto& region : regions) {
            const uint64_t base = region.at("physical_address").get<uint64_t>();
            if (physical >= base && physical - base + size <= region.at("size").get<uint64_t>())
                return region.at("virtual_address").get<uint64_t>() + (physical - base);
        }
        return std::nullopt;
    };
    for (const auto& target : framebuffers->at("targets")) {
        const uint64_t pixels = target.at("framebuffer_width").get<uint64_t>() *
                                target.at("framebuffer_height").get<uint64_t>();
        const uint64_t color = pixels * ColorBytesPerPixel(target.at("framebuffer_color_format").get<uint32_t>());
        if (const auto address = toVirtual(target.at("color_physical_address").get<uint64_t>(), color))
            spans.push_back({*address, *address + color});
        const uint64_t depthAddress = target.at("depth_physical_address").get<uint64_t>();
        const uint64_t depth = pixels * DepthBytesPerPixel(target.at("framebuffer_depth_format").get<uint32_t>());
        if (depthAddress != 0U && depth != 0U)
            if (const auto address = toVirtual(depthAddress, depth))
                spans.push_back({*address, *address + depth});
    }
    return spans;
}

void CompareValues(const Json& a, const Json& b, const std::string& path, Json& differences) {
    if (a.type() != b.type()) {
        differences.push_back({{"path", path}, {"kind", "type"}});
        return;
    }
    if (a.is_object()) {
        for (auto it = a.begin(); it != a.end(); ++it) {
            const auto other = b.find(it.key());
            if (other == b.end()) {
                differences.push_back({{"path", path + "/" + it.key()}, {"kind", "only_first"}});
            } else {
                CompareValues(it.value(), *other, path + "/" + it.key(), differences);
            }
        }
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!a.contains(it.key()))
                differences.push_back({{"path", path + "/" + it.key()}, {"kind", "only_second"}});
        return;
    }
    // Small values are shown; large ones (binary blobs, long arrays) are not.
    const auto shown = [&](const char* kind) {
        Json difference{{"path", path}, {"kind", kind}};
        const auto firstText = a.dump();
        const auto secondText = b.dump();
        if (firstText.size() <= 256U && secondText.size() <= 256U) {
            difference["first"] = a;
            difference["second"] = b;
        }
        differences.push_back(std::move(difference));
    };
    if (a.is_array()) {
        if (a.size() != b.size()) {
            shown("array_size");
            return;
        }
        for (size_t i = 0; i < a.size(); ++i) CompareValues(a[i], b[i], path + "/" + std::to_string(i), differences);
        return;
    }
    if (a != b) shown("value");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: oot3d_native_a32_savestate_diff <first.oot3dsav> <second.oot3dsav>\n"
                     "       oot3d_native_a32_savestate_diff --describe <state.oot3dsav>\n";
        return 2;
    }
    Json first;
    Json second;
    std::string error;
    if (std::string_view(argv[1]) == "--describe") {
        Oot3dNativeGame::NativeA32SavestateIoResult io;
        if (!Oot3dNativeGame::ReadNativeA32StateDocument(argv[2], first, &io, &error)) {
            std::cerr << error << '\n';
            return 2;
        }
        const auto& runtime = first.at("runtime");
        const auto clock = runtime.find("gameplay_clock");
        const auto topScreen = runtime.find("topscreen_temporal_state");
        // States older than the TopScreen temporal state ran the original UI.
        const bool topScreenProfile = topScreen != runtime.end() && topScreen->value("profile_active", false);
        std::cout << Json{
            {"file_bytes", io.FileBytes},
            {"compressed", io.FileBytes < io.PayloadBytes + 40U},
            {"frame_count", runtime.at("frame_count")},
            {"timing_mode", clock != runtime.end() ? clock->value("timing_mode", Json()) : Json()},
            {"graphics_free", runtime.contains("native_pica_guest_framebuffers")},
            {"ui_profile", topScreenProfile ? "topscreen" : "oot3d"},
            {"host_settings", first.contains("host_settings")},
            {"compatibility", first.at("compatibility")},
        }.dump() << '\n';
        return 0;
    }
    if (!Oot3dNativeGame::ReadNativeA32StateDocument(argv[1], first, nullptr, &error) ||
        !Oot3dNativeGame::ReadNativeA32StateDocument(argv[2], second, nullptr, &error)) {
        std::cerr << error << '\n';
        return 2;
    }
    try {
        // Guest memory byte by byte; the rest of the documents structurally.
        Json memory = Json::array();
        auto& firstRegions = first.at("process").at("memory").at("regions");
        auto& secondRegions = second.at("process").at("memory").at("regions");
        const auto framebuffers = FramebufferSpans(second);
        bool memoryInFramebuffers = true;
        if (firstRegions.size() == secondRegions.size()) {
            for (size_t r = 0; r < firstRegions.size(); ++r) {
                auto& a = firstRegions[r];
                auto& b = secondRegions[r];
                if (a.at("name") != b.at("name") || a.at("base_address") != b.at("base_address")) continue;
                const auto& bytesA = a.at("bytes").get_binary();
                const auto& bytesB = b.at("bytes").get_binary();
                const uint64_t base = a.at("base_address").get<uint64_t>();
                if (bytesA.size() == bytesB.size()) {
                    std::optional<Span> open;
                    uint64_t outside = 0;
                    const auto close = [&] {
                        memory.push_back({{"region", a.at("name")}, {"begin", open->Begin}, {"end", open->End},
                                          {"bytes_outside_framebuffers", outside}});
                        memoryInFramebuffers = memoryInFramebuffers && outside == 0U;
                        outside = 0;
                        open.reset();
                    };
                    for (size_t i = 0; i < bytesA.size(); ++i) {
                        if (bytesA[i] == bytesB[i]) continue;
                        const uint64_t address = base + i;
                        // Ranges closer than 64 bytes are reported as one range.
                        if (open && address > open->End + 64U) close();
                        if (!open) open = Span{address, address + 1U};
                        open->End = address + 1U;
                        if (std::none_of(framebuffers.begin(), framebuffers.end(), [&](const Span& s) {
                                return address >= s.Begin && address < s.End;
                            }))
                            ++outside;
                    }
                    if (open) close();
                    a.erase("bytes");
                    b.erase("bytes");
                }
            }
        }
        Json differences = Json::array();
        CompareValues(first, second, "", differences);
        // Each difference is renderer-owned (sections a graphics-free state
        // drops or adds, the host write counter), a host binding (the RomFS
        // file path, which a restore rebinds to the configured image), a schema
        // upgrade (a field only the second state has, or a section's format
        // name) or game state.
        const auto romFsFilePath = [&](const std::string& path) {
            static constexpr std::string_view prefix = "/ctr_host/kernel_objects/";
            static constexpr std::string_view suffix = "/file_path";
            if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size() + suffix.size() ||
                path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
                return false;
            const auto index = std::stoul(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
            const auto& objects = first.at("ctr_host").at("kernel_objects");
            return index < objects.size() &&
                   objects[index].value("type", std::string{}) == "client_session:file:romfs";
        };
        bool gameStateEqual = true;
        for (auto& difference : differences) {
            const auto path = difference.at("path").get<std::string>();
            const auto kind = difference.at("kind").get<std::string>();
            const bool formatName = path.size() >= 7 && path.compare(path.size() - 7, 7, "/format") == 0 &&
                                    path != "/format";
            const char* category =
                path.rfind("/runtime/native_pica_", 0) == 0 || path == "/process/memory/write_generation"
                    ? "renderer_owned"
                : romFsFilePath(path)                 ? "host_binding"
                : kind == "only_second" || formatName ? "schema_upgrade"
                                                      : "game_state";
            difference["category"] = category;
            gameStateEqual = gameStateEqual && std::string_view(category) != "game_state";
        }
        uint64_t memoryBytes = 0;
        for (const auto& range : memory)
            memoryBytes += range.at("end").get<uint64_t>() - range.at("begin").get<uint64_t>();
        const Json report{
            {"graphics_only", memoryInFramebuffers && gameStateEqual},
            {"memory_ranges", memory},
            {"memory_range_bytes", memoryBytes},
            {"framebuffers", [&] {
                 Json spans = Json::array();
                 for (const auto& span : framebuffers) spans.push_back({{"begin", span.Begin}, {"end", span.End}});
                 return spans;
             }()},
            {"differences", differences},
        };
        std::cout << report.dump(1) << '\n';
        return report.at("graphics_only").get<bool>() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "savestate comparison failed: " << exception.what() << '\n';
        return 2;
    }
}
