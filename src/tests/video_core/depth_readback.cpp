// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "video_core/rasterizer_cache/surface_base.h"

using namespace VideoCore;

TEST_CASE("Depth readback coalesces owned tiles", "[video_core][depth_readback]") {
    SurfaceParams surface;
    // Capture address is not aligned to a 256-byte host boundary.
    surface.addr = 0x18537fc0;
    surface.width = surface.stride = 512;
    surface.height = 400;
    surface.is_tiled = true;
    surface.pixel_format = PixelFormat::D24S8;
    surface.UpdateParams();
    const auto full = surface.GetInterval();
    const SurfaceInterval request{0x185a3b74, 0x185a3b78};
    const auto tile = surface.GetFlushInterval(request, full);
    REQUIRE(tile.lower() <= request.lower());
    REQUIRE(tile.upper() >= request.upper());
    REQUIRE(tile.upper() - tile.lower() == 256);
    REQUIRE((tile.lower() - surface.addr) % 256 == 0);
    const auto rect = surface.GetSubRect(surface.FromInterval(tile));
    REQUIRE(rect.GetWidth() == 8);
    REQUIRE(rect.GetHeight() == 8);

    // Model the dirty-region bookkeeping used by FlushRegion. Nearby reads
    // reuse the downloaded bytes; drawing again dirties the tile afresh.
    SurfaceRegions dirty;
    dirty += full;
    dirty -= tile;
    REQUIRE(boost::icl::is_empty(dirty & request));
    REQUIRE(boost::icl::is_empty(dirty & SurfaceInterval{tile.lower()+4, tile.lower()+8}));
    REQUIRE_FALSE(boost::icl::is_empty(dirty & SurfaceInterval{tile.upper(), tile.upper()+4}));
    dirty += full;
    REQUIRE_FALSE(boost::icl::is_empty(dirty & request));

    // Another owner or CPU-written bytes in the same tile must not be touched.
    const SurfaceInterval owned{request.lower()-2, request.upper()+2};
    REQUIRE(surface.GetFlushInterval(request, owned) == owned);
    REQUIRE(boost::icl::is_empty(surface.GetFlushInterval(request, {surface.addr, surface.addr+4})));

    for (const auto offset : {0u, 252u, 254u, surface.size-4}) {
        const SurfaceInterval read{surface.addr+offset, surface.addr+offset+4};
        const auto selected = surface.GetFlushInterval(read, full);
        REQUIRE((selected & read) == read);
        REQUIRE((selected & full) == selected);
        REQUIRE(selected.upper() - selected.lower() <= 512);
        const auto bounds = surface.GetSubRect(surface.FromInterval(selected));
        REQUIRE(bounds.GetWidth() % 8 == 0);
        REQUIRE(bounds.GetHeight() % 8 == 0);
    }
    // Crossing a tile row needs a wider staging rectangle, but only the two
    // owned tiles should be written back and marked clean.
    const PAddr row_end = surface.addr + surface.width*8*4;
    const auto crossing = surface.GetFlushInterval({row_end-2, row_end+2}, full);
    REQUIRE(crossing.upper()-crossing.lower() == 512);
    REQUIRE(surface.FromInterval(crossing).height == 16);

    SurfaceBase tracked{surface, {}};
    for (u32 tile_index = 0; tile_index < 4; ++tile_index) {
        const PAddr at = surface.addr + tile_index * 256;
        const auto selected = tracked.GetFlushInterval({at, at+4}, full);
        REQUIRE(selected.upper() - selected.lower() == 256);
    }
    REQUIRE(tracked.GetFlushInterval(request, full) == full);
    tracked.MarkInvalid(full);
    tracked.MarkValid(full);
    REQUIRE(tracked.GetFlushInterval(request, full) == tile);

    const SurfaceInterval bulk{surface.addr+16, surface.addr+4096};
    REQUIRE(surface.GetFlushInterval(bulk, full) == bulk);
    surface.pixel_format = PixelFormat::RGBA8;
    surface.UpdateParams();
    REQUIRE(surface.GetFlushInterval(request, full) == full);
    surface.pixel_format = PixelFormat::D24S8;
    surface.is_tiled = false;
    surface.UpdateParams();
    REQUIRE(surface.GetFlushInterval(request, full) == full);
    surface.is_tiled = true;
    surface.levels = 2;
    surface.UpdateParams();
    REQUIRE(surface.GetFlushInterval(request, full) == full);
}
