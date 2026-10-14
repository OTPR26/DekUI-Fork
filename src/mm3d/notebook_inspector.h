// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"

namespace MM3D::NotebookInspector {
using U32 = std::uint32_t;
struct Row {
    U32 container{}, attachment{}, object{};
    int object_index{-1}, quest{-1}, retained{-1}, list_index{};
    U32 entry{};
    bool entry_valid{};
    float x{}, y{};
};
struct Snapshot {
    const char* status{"Enable inspector while the game is running."};
    U32 controller{}, pages{}, candidates{};
    U32 metadata_matches{}, object_matches{};
    int start{}, selected{}, count{};
    unsigned samples{};
    std::array<Row, 9> rows{};
    std::array<std::uint8_t, 0x260> fields{};
};
inline std::atomic<bool> enabled{false}, log_requested{false}, rescan_requested{false};
inline std::atomic<bool> log_discovery{false};
inline std::atomic<bool> state_probe{false};
inline std::mutex snapshot_mutex;
inline Snapshot published;
inline Snapshot GetSnapshot() {
    std::lock_guard lock(snapshot_mutex);
    return published;
}
inline void Publish(const Snapshot& s) {
    std::lock_guard lock(snapshot_mutex);
    published = s;
}
inline bool Heap(U32 p) { return p >= 0x08000000 && p < 0x40000000 && !(p & 3); }

// Read CPU backing RAM through the page provider, including cached pages.
// Never invoke guest MMIO or write guest state.
// Called from the emulation thread after JIT execution, not the rendering thread.
template <typename Pages>
void Poll(Pages& pages, const void* address_space,
          std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    using Clock = std::chrono::steady_clock;
    static auto last = Clock::time_point{};
    if (now - last < std::chrono::milliseconds(100)) return;
    last = now;
    static const void* previous_space{};
    static U32 cursor = 0x8000, controller{}, candidate{}, candidate_count{}, scanned{};
    static unsigned samples{};
    static U32 metadata_matches{}, object_matches{};
    auto reset = [&] {
        cursor = 0x8000; controller = candidate = candidate_count = scanned = 0;
        metadata_matches = object_matches = 0;
    };
    if (previous_space != address_space || rescan_requested.exchange(false)) {
        previous_space = address_space;
        reset();
    }
    auto read = [&](U32 addr, auto& value) {
        auto* dest = reinterpret_cast<unsigned char*>(&value);
        for (unsigned i = 0; i < sizeof(value); ++i) {
            const U32 at = addr + i;
            if (at < addr || (at >> 12) >= pages.size() || !pages[at >> 12]) return false;
            dest[i] = pages[at >> 12][at & 4095];
        }
        return true;
    };
    Snapshot s;
    U32 a{}, b{}, c{};
    if (!read(0x5ab48c, a) || !read(0x5ab540, b) || !read(0x620e84, c) ||
        a != 0xe92d4ff7 || b != 0xe3500009 || c != 0x01030507) {
        reset();
        s.status = "Running-code signature mismatch (or game not loaded). Inspector inactive.";
        Publish(s);
        return;
    }
    auto inspect = [&](U32 base, Snapshot& result) {
        std::uint8_t count{}, selected{};
        std::int8_t start{};
        U32 list{};
        if (!read(base + 0x73, count) || count == 0 || count > 127 ||
            !read(base + 0x254, start) || start < -2 || start >= count ||
            !read(base + 0x255, selected) || selected >= count ||
            !read(base + 0x68, list) || !Heap(list)) return false;
        ++metadata_matches;
        std::array<U32, 9> objects{}, containers{};
        std::array<std::uint8_t, 9> ids{}, flags{};
        for (unsigned i = 0; i < 9; ++i) {
            if (!read(base + 0x7c + 4*i, objects[i]) || !Heap(objects[i]) ||
                !read(objects[i] + 0x178, ids[i]) || (ids[i] > 127 && ids[i] != 255) ||
                !read(objects[i] + 0x179, flags[i]) || flags[i] > 1 ||
                !read(base + 0x138 + 4*i, containers[i]) || !Heap(containers[i])) return false;
            for (unsigned j = 0; j < i; ++j)
                if (objects[i] == objects[j] || containers[i] == containers[j]) return false;
        }
        constexpr std::array<unsigned, 9> order{7,5,3,1,0,2,4,6,8};
        ++object_matches;
        if (state_probe.load()) {
            if (!read(base, result.fields)) return false;
            result.controller = base; result.start = start;
            result.selected = selected; result.count = count;
            return true;
        }
        // Bounded evidence capture before applying the still-unverified
        // attachment/coordinate assumptions. Never promote these to matches.
        const bool capture = !controller && object_matches <= 128 && log_discovery.load();
        if (capture) {
            LOG_INFO(Core_ARM11, "MM3D Notebook candidate base={:08x} start={} selected={} count={} list={:08x}",
                     base, static_cast<int>(start), selected, count, list);
            for (unsigned i = 0; i < 9; ++i) {
                U32 attachment{}, xbits{}, ybits{};
                unsigned ordinal{};
                while (order[ordinal] != i) ++ordinal;
                const int index = start + static_cast<int>(ordinal);
                std::uint16_t entry{};
                const bool entry_ok = index >= 0 && index < count && read(list + 2*index, entry);
                const bool fields_ok = read(containers[i]+0x20, attachment) &&
                    read(containers[i]+0x90, xbits) && read(containers[i]+0x94, ybits);
                LOG_INFO(Core_ARM11, "MM3D Notebook candidate slot={} object={:08x} id={} in_use={} container={:08x} attached={:08x} xbits={:08x} ybits={:08x} readable={} index={} entry={:04x} entry_ok={}",
                         i, objects[i], ids[i], flags[i], containers[i], attachment,
                         xbits, ybits, fields_ok, index, entry, entry_ok);
            }
        }
        auto reject = [&](const char* reason, unsigned slot) {
            if (capture) LOG_INFO(Core_ARM11, "MM3D Notebook reject base={:08x} slot={} reason={}", base, slot, reason);
            return false;
        };
        unsigned matched{};
        for (unsigned i = 0; i < 9; ++i) {
            Row row;
            row.container = containers[i];
            if (!read(containers[i] + 0x20, row.attachment) ||
                !read(containers[i] + 0x90, row.x) || !read(containers[i] + 0x94, row.y) ||
                !std::isfinite(row.x) || !std::isfinite(row.y) ||
                std::abs(row.x) > 16384 || std::abs(row.y) > 16384) return reject("coordinate/read check", i);
            unsigned ordinal{};
            while (order[ordinal] != i) ++ordinal;
            row.list_index = start + static_cast<int>(ordinal);
            if (row.list_index >= 0 && row.list_index < count) {
                std::uint16_t entry{};
                if (!read(list + 2*row.list_index, entry)) return reject("list read", i);
                row.entry = entry;
                row.entry_valid = true;
            }
            if (row.attachment) {
                if (!Heap(row.attachment)) return reject("attachment address", i);
                for (unsigned j = 0; j < 9; ++j) {
                    if (row.attachment != objects[j]) continue;
                    row.object_index = j; row.object = objects[j];
                    row.quest = ids[j]; row.retained = flags[j];
                    if (row.entry_valid && (row.entry & 0x1000) &&
                        (row.entry & 255) == ids[j]) ++matched;
                }
                if (row.object_index < 0) {
                    if (!row.entry_valid || (row.entry & 0x1000) ||
                        (row.entry & 255) < 0x40 || (row.entry & 255) > 0x42) return reject("non-object attachment / header entry", i);
                    U32 header{};
                    if (!read(base + 0x1c8 + 4*((row.entry & 255)-0x40), header) ||
                        header != row.attachment) return reject("header pointer mismatch", i);
                }
            }
            result.rows[i] = row;
        }
        if (!matched) return reject("no attachment matches an entry ID", 9);
        result.controller = base; result.start = start;
        result.selected = selected; result.count = count;
        return true;
    };
    if (controller && !inspect(controller, s)) reset();
    if (!controller) {
        // Complete a bounded, incremental pass before choosing: stale copies
        // and ambiguous matches must never silently become the live controller.
        const auto scan_deadline = Clock::now() + std::chrono::milliseconds(8);
        for (unsigned budget = 0; budget < 2048 && cursor < 0x40000 &&
             Clock::now() < scan_deadline; ++budget, ++cursor) {
            const auto* page = pages[cursor];
            ++scanned;
            if (!page) continue;
            for (U32 offset = 0; offset < 4096; offset += 4) {
                U32 first{};
                std::memcpy(&first, page + offset, 4);
                if (!Heap(first)) continue;
                const U32 base = (cursor << 12) + offset - 0x7c;
                U32 second{}, last_object{};
                if (!read(base + 0x80, second) || !Heap(second) || first == second ||
                    !read(base + 0x9c, last_object) || !Heap(last_object)) continue;
                Snapshot trial;
                if (inspect(base, trial)) { candidate = base; ++candidate_count; }
            }
        }
        s.pages = scanned; s.candidates = candidate_count;
        s.metadata_matches = metadata_matches; s.object_matches = object_matches;
        s.status = "Scanning mapped RAM for validated nine-slot bindings...";
        if (cursor == 0x40000) {
            if (candidate_count == 1 && inspect(candidate, s)) controller = candidate;
            else s.status = candidate_count > 1 ?
                "Ambiguous controller matches. Open Events, then Rescan." :
                "No validated controller found. Open Events, then Rescan.";
        }
    }
    if (controller) {
        s.status = state_probe.load() ? "Controller candidate found; Events visibility UNVERIFIED." :
            "Validated bindings (sampled; attachment is not proof of visibility).";
        s.samples = ++samples;
        if (log_requested.exchange(false)) {
            LOG_INFO(Core_ARM11, "MM3D Notebook snapshot controller={:08x} start={} selected={} count={}",
                     controller, s.start, s.selected, s.count);
            for (unsigned i = 0; i < 9; ++i) {
                const auto& r = s.rows[i];
                LOG_INFO(Core_ARM11, "MM3D Notebook slot={} list={} entry={:04x} entry_valid={} container={:08x} attachment={:08x} object_index={} quest={} retained={} raw_x={} raw_y={}",
                         i, r.list_index, r.entry, r.entry_valid, r.container, r.attachment,
                         r.object_index, r.quest, r.retained, r.x, r.y);
            }
        }
    }
    Publish(s);
}
} // namespace MM3D::NotebookInspector
