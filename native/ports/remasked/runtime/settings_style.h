// SPDX-License-Identifier: GPL-2.0-or-later
// ReTuned interface geometry and controls; ReMasked purple palette.
#pragma once
#include <imgui.h>
#include <algorithm>
namespace ReMaskedSettings {
inline void PurpleStyle(ImGuiStyle& style) {
    style.WindowPadding={18,16};style.FramePadding={12,8};style.ItemSpacing={10,10};
    style.ItemInnerSpacing={8,6};style.ScrollbarSize=16;style.GrabMinSize=18;
    style.WindowRounding=0;style.ChildRounding=10;style.FrameRounding=7;
    style.PopupRounding=5;style.ScrollbarRounding=8;style.GrabRounding=8;
    style.WindowBorderSize=0;style.ChildBorderSize=0;style.FrameBorderSize=0;
    auto& c=style.Colors;
    c[ImGuiCol_Text]={.949f,.949f,.949f,1};c[ImGuiCol_TextDisabled]={.60f,.63f,.69f,1};
    c[ImGuiCol_WindowBg]={.031f,.027f,.051f,1};c[ImGuiCol_ChildBg]={.051f,.035f,.075f,1};
    c[ImGuiCol_PopupBg]={.075f,.047f,.110f,1};c[ImGuiCol_Border]={.29f,.20f,.38f,.7f};
    c[ImGuiCol_FrameBg]={.16f,.10f,.23f,1};c[ImGuiCol_FrameBgHovered]={.25f,.14f,.36f,1};
    c[ImGuiCol_FrameBgActive]={.31f,.17f,.44f,1};c[ImGuiCol_Button]={.16f,.10f,.23f,1};
    c[ImGuiCol_ButtonHovered]={.31f,.17f,.44f,1};c[ImGuiCol_ButtonActive]={.38f,.20f,.53f,1};
    c[ImGuiCol_Header]={.31f,.17f,.44f,1};c[ImGuiCol_HeaderHovered]={.38f,.20f,.53f,1};
    c[ImGuiCol_HeaderActive]={.43f,.23f,.60f,1};
    c[ImGuiCol_CheckMark]=c[ImGuiCol_SliderGrab]=c[ImGuiCol_NavCursor]={.72549f,.490196f,.94902f,1};
    c[ImGuiCol_SliderGrabActive]={.85f,.70f,1,1};c[ImGuiCol_Separator]={.25f,.17f,.33f,1};
    c[ImGuiCol_ScrollbarBg]={0,0,0,0};c[ImGuiCol_ScrollbarGrab]={.37f,.26f,.47f,1};
    c[ImGuiCol_ScrollbarGrabHovered]={.47f,.33f,.59f,1};c[ImGuiCol_ScrollbarGrabActive]={.57f,.40f,.72f,1};
}
}
namespace ReMaskedSettings {
inline bool Toggle(const char* label,bool* value) {
    const float f=ImGui::GetFontSize(),width=std::min(ImGui::GetContentRegionAvail().x,f*32),height=f*2.4f;
    const auto p=ImGui::GetCursorScreenPos();
    const bool pressed=ImGui::InvisibleButton(label,{width,height},ImGuiButtonFlags_EnableNav);if(pressed)*value=!*value;
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(p,{p.x+width,p.y+height},ImGui::GetColorU32(ImGui::IsItemHovered()?ImGuiCol_FrameBgHovered:ImGuiCol_FrameBg),7);
    draw->AddText({p.x+f*.65f,p.y+(height-f)*.5f},ImGui::GetColorU32(ImGuiCol_Text),label);
    const ImVec2 a{p.x+width-f*3.1f,p.y+(height-f)*.5f};
    draw->AddRectFilled(a,{a.x+f*2.2f,a.y+f},ImGui::GetColorU32(*value?ImGuiCol_CheckMark:ImGuiCol_Border),f*.5f);
    draw->AddCircleFilled({a.x+f*(*value?1.7f:.5f),a.y+f*.5f},f*.34f,ImGui::GetColorU32(ImGuiCol_Text));
    if(ImGui::IsItemFocused())draw->AddRect(p,{p.x+width,p.y+height},ImGui::GetColorU32(ImGuiCol_CheckMark),7,0,2);
    return pressed;
}
}
