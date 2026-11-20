// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>

namespace ReMaskedIOS {
inline constexpr auto SupportedGameCodeSha256 =
    "6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d";

// Extract a supported decrypted CTR image into the caller's staging directory.
void PrepareRom(const std::filesystem::path& rom, const std::filesystem::path& staging);
}
