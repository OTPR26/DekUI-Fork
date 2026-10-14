// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/notebook_inspector.h"
#include <iostream>
#include <map>
#include <vector>

int main() {
    using namespace MM3D::NotebookInspector;
    std::vector<unsigned char*> pages(1 << 20);
    struct PageView {
        std::vector<unsigned char*>& direct;
        U32 cached_index{};
        const unsigned char* cached_backing{};
        std::size_t size() const { return direct.size(); }
        const unsigned char* operator[](std::size_t i) const {
            return i == cached_index && cached_backing ? cached_backing : direct[i];
        }
    } view{pages};
    std::map<U32, std::array<unsigned char, 4096>> storage;
    auto write = [&](U32 at, auto value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (unsigned i = 0; i < sizeof(value); ++i) {
            auto& page = storage[(at+i) >> 12];
            pages[(at+i) >> 12] = page.data();
            page[(at+i) & 4095] = bytes[i];
        }
    };
    auto check = [](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
    };
    auto now = std::chrono::steady_clock::now();
    auto poll = [&] {
        now += std::chrono::milliseconds(110);
        Poll(view, &pages, now);
    };
    enabled = true;
    poll();
    check(!GetSnapshot().controller, "reject wrong executable");
    write(0x5ab48c, U32{0xe92d4ff7});
    write(0x5ab540, U32{0xe3500009});
    write(0x620e84, U32{0x01030507});
    constexpr U32 base = 0x08001000, list = 0x08003000;
    write(base+0x68, list);
    write(base+0x73, std::uint8_t{12});
    write(base+0x254, std::int8_t{0});
    write(base+0x255, std::uint8_t{2});
    constexpr std::array<unsigned,9> order{7,5,3,1,0,2,4,6,8};
    for (unsigned i=0; i<9; ++i) {
        const U32 object = 0x08010000 + i*4096;
        const U32 container = 0x08020000 + order[i]*4096;
        write(base+0x7c+4*i, object);
        write(base+0x138+4*order[i], container);
        write(object+0x178, static_cast<std::uint8_t>(i));
        write(object+0x179, std::uint8_t{1});
        write(list+2*i, static_cast<std::uint16_t>(0x1000+i));
        write(container+0x20, object);
        write(container+0x90, 212.f);
        write(container+0x94, -25.f + 38*i);
    }
    // Model a graphics-cached container page: no direct JIT pointer, but CPU
    // backing memory remains readable through the page provider.
    view.cached_index = 0x08020000 >> 12;
    view.cached_backing = pages[view.cached_index];
    pages[view.cached_index] = nullptr;
    for (unsigned i=0; i<1000 && !GetSnapshot().controller; ++i) poll();
    auto s=GetSnapshot();
    check(s.controller == base, "discover controller from structural bindings");
    check(s.rows[7].quest == 0 && s.rows[8].quest == 8, "plate permutation");
    check(s.rows[0].object_index == 4 && s.rows[0].list_index == 4, "object versus plate index");
    write(0x08020000+0x20, U32{0});
    poll();
    check(GetSnapshot().rows[0].attachment == 0, "observe detach without stale row");
    const auto before = storage;
    poll();
    check(storage == before, "inspector never changes guest memory");
    // A second structurally valid copy must produce ambiguity, not a guess.
    storage[0x8002] = storage[0x8001];
    pages[0x8002] = storage[0x8002].data();
    rescan_requested = true;
    for (unsigned i=0; i<1000; ++i) poll();
    check(!GetSnapshot().controller && GetSnapshot().candidates == 2, "reject ambiguous copies");
    write(0x5ab48c, U32{0});
    poll();
    check(!GetSnapshot().controller, "invalidate on executable change");
    std::cout << "PASS Notebook inspector: signatures, discovery, mapping, detach, read-only, ambiguity\n";
}
