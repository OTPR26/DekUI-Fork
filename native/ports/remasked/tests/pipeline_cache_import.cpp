// SPDX-License-Identifier: GPL-3.0-or-later
#include "../renderer/pipeline_cache_import.h"
#include <cassert>
#include <cstdio>
using ReMaskedRenderer::PipelineCacheImportIsUsable;
int main() {
    // Actual Odin capture: strict provenance/checksum passed, then Turnip
    // retained the cache while making it 40 bytes smaller. NRI rejected it.
    assert(PipelineCacheImportIsUsable(76215988,76215948,36));
    assert(!PipelineCacheImportIsUsable(76215988,36,36));
    assert(!PipelineCacheImportIsUsable(76215988,0,36));
    assert(PipelineCacheImportIsUsable(76215988,76215988,36));
    assert(PipelineCacheImportIsUsable(76215988,76216000,36));
    assert(PipelineCacheImportIsUsable(0,36,36));
    // The empty-cache header is driver-dependent; do not assume 36 bytes.
    assert(!PipelineCacheImportIsUsable(1000,64,64));
    assert(PipelineCacheImportIsUsable(1000,960,64));
    std::puts("Turnip compacted-cache regression and empty/invalid cache boundaries passed.");
}
