#include "oot3d_native_preparation_screen.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace Oot3dNativeGame {

void NativePreparationScreen::Draw(const Fast::Renderer3ds::PicaPreparationStatus& status) const {
    if (!CoversFrame()) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(viewport));
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0, 0, 0, 255));

    char detail[96];
    float fraction = 1.0F;
    if (mPhase == Phase::Programs && !status.ProgramsReady) {
        std::snprintf(detail, sizeof(detail), "Programs %zu / %zu", status.PreparedParts, status.Parts);
        fraction = status.Parts == 0U ? 1.0F
            : static_cast<float>(status.PreparedParts) / static_cast<float>(status.Parts);
    } else if (mPhase == Phase::Programs) {
        std::snprintf(detail, sizeof(detail), "Upscaler");
    } else {
        std::snprintf(detail, sizeof(detail), "First frames");
        fraction = static_cast<float>(std::min(mSteadyFrames, kSteadyFrames)) / static_cast<float>(kSteadyFrames);
    }

    ImFont* font = ImGui::GetFont();
    const float scale = std::max(1.0F, size.y / 720.0F);
    const float titleSize = ImGui::GetFontSize() * 1.6F * scale;
    const float detailSize = ImGui::GetFontSize() * 1.1F * scale;
    const char* title = "Preparing the renderer";
    const ImVec2 titleExtent = font->CalcTextSizeA(titleSize, FLT_MAX, 0.0F, title);
    const ImVec2 detailExtent = font->CalcTextSizeA(detailSize, FLT_MAX, 0.0F, detail);
    const float centerX = origin.x + size.x * 0.5F;
    const float centerY = origin.y + size.y * 0.5F;
    draw->AddText(font, titleSize, ImVec2(centerX - titleExtent.x * 0.5F, centerY - titleExtent.y * 2.0F),
                  IM_COL32(235, 235, 235, 255), title);
    draw->AddText(font, detailSize, ImVec2(centerX - detailExtent.x * 0.5F, centerY - detailExtent.y * 0.5F),
                  IM_COL32(170, 170, 170, 255), detail);

    const float barWidth = std::min(size.x * 0.4F, 640.0F * scale);
    const float barHeight = 6.0F * scale;
    const ImVec2 barMin(centerX - barWidth * 0.5F, centerY + detailExtent.y * 1.5F);
    const ImVec2 barMax(barMin.x + barWidth, barMin.y + barHeight);
    draw->AddRectFilled(barMin, barMax, IM_COL32(50, 50, 50, 255));
    draw->AddRectFilled(barMin, ImVec2(barMin.x + barWidth * std::clamp(fraction, 0.0F, 1.0F), barMax.y),
                        IM_COL32(200, 200, 200, 255));
}

} // namespace Oot3dNativeGame
