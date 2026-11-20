// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <imgui.h>
#include <array>
#include <cmath>
namespace ReMaskedSettings {
inline ImFont* BodyFont=nullptr;
inline ImFont* BoldFont=nullptr;
inline ImFont* HeadingFont=nullptr;
inline float FontPixels=24.f;
struct InterfaceFaces{ImFont* Body=nullptr;ImFont* Heading=nullptr;float Pixels=0;};
inline constexpr std::array<float,9> InterfacePixelSizes{8,10,12,16,20,24,32,40,48};
inline std::array<InterfaceFaces,InterfacePixelSizes.size()> SizedFaces{};
inline InterfaceFaces FacesForSize(float target){
 InterfaceFaces best{BodyFont,HeadingFont,FontPixels};float error=1e9f;
 for(const auto& face:SizedFaces)if(face.Body){const float distance=std::abs(std::log(target/face.Pixels));if(distance<error){best=face;error=distance;}}
 return best;
}
// Include typographic apostrophes, quotation marks, dashes and ellipses.
// ImGui's default Latin range ends before these characters.
inline constexpr ImWchar GlyphRanges[]{0x0020,0x00ff,0x2000,0x206f,0x2190,0x21ff,0};
}
