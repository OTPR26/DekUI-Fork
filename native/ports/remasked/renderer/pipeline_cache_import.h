// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
namespace ReMaskedRenderer {
// A driver may compact imported entries. Compare with an actual empty cache,
// not the input byte count, before treating the import as wholly discarded.
constexpr bool PipelineCacheImportIsUsable(std::size_t initialBytes,
                                          std::size_t serializedBytes,
                                          std::size_t emptyBytes) {
    return serializedBytes >= initialBytes || serializedBytes > emptyBytes;
}
}
