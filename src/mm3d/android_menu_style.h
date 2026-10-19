// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <string>
#include <imgui.h>

// Shared presentation. Setting storage and game actions stay in overlay.cpp.
namespace MM3D::AndroidMenuStyle {
inline constexpr ImVec4 Accent{.725f,.49f,.95f,1};
inline void Theme(ImGuiStyle& s) {
    s.WindowPadding={12,10}; s.FramePadding={8,6}; s.ItemSpacing={8,9};
    s.ItemInnerSpacing={8,6}; s.ScrollbarSize=16; s.GrabMinSize=15;
    s.WindowRounding=0; s.ChildRounding=5; s.FrameRounding=3;
    s.PopupRounding=4; s.ScrollbarRounding=6; s.GrabRounding=8;
    s.WindowBorderSize=0; s.ChildBorderSize=0; s.FrameBorderSize=1;
    auto& c=s.Colors;
    c[ImGuiCol_Text]={.95f,.95f,.95f,1};
    c[ImGuiCol_TextDisabled]={.55f,.52f,.61f,1};
    c[ImGuiCol_WindowBg]={.032f,.028f,.051f,1};
    c[ImGuiCol_ChildBg]={.064f,.05f,.09f,1};
    c[ImGuiCol_PopupBg]={.095f,.07f,.14f,1};
    c[ImGuiCol_Border]={.28f,.22f,.35f,.6f};
    c[ImGuiCol_FrameBg]={.14f,.11f,.19f,1};
    c[ImGuiCol_FrameBgHovered]={.24f,.13f,.35f,1};
    c[ImGuiCol_FrameBgActive]={.28f,.12f,.45f,1};
    c[ImGuiCol_Button]={.16f,.11f,.22f,1};
    c[ImGuiCol_ButtonHovered]={.30f,.15f,.44f,1};
    c[ImGuiCol_ButtonActive]={.36f,.19f,.52f,1};
    c[ImGuiCol_Header]={.24f,.08f,.41f,1};
    c[ImGuiCol_HeaderHovered]={.29f,.12f,.46f,1};
    c[ImGuiCol_HeaderActive]={.32f,.14f,.49f,1};
    c[ImGuiCol_CheckMark]=c[ImGuiCol_SliderGrab]=Accent;
    c[ImGuiCol_SliderGrabActive]={.9f,.73f,1,1};
    c[ImGuiCol_Separator]={.25f,.18f,.32f,1};
    c[ImGuiCol_ScrollbarBg]={0,0,0,0};
    c[ImGuiCol_ScrollbarGrab]={.35f,.25f,.43f,1};
    c[ImGuiCol_NavCursor]=Accent;
}

inline float ControlWidth() {
#if defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
    return std::min(ImGui::GetContentRegionAvail().x,ImGui::GetFontSize()*28.f);
#else
    return ImGui::GetContentRegionAvail().x;
#endif
}
inline bool Checkbox(const char* label,bool* value,bool radio=false) {
#if defined(__ANDROID__) || defined(ENABLE_MM3D_GAME_FOLDER)
    const float f=ImGui::GetFontSize(), w=ControlWidth();
#if defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
    const float pad=f*.55f, toggle=f*3.f;
#else
    const float pad=f*.45f, toggle=f*1.65f;
#endif
    const float text_w=std::max(f*2,w-toggle-pad*3);
    const auto text=ImGui::CalcTextSize(label,nullptr,true,text_w);
#if defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
    const float h=std::max(f*2.1f,text.y+pad*2);
#else
    const float h=std::max({f*1.85f,text.y+pad*2,ImGui::GetIO().DisplaySize.y*.065f});
#endif
    const auto p=ImGui::GetCursorScreenPos();
    const bool clicked=ImGui::InvisibleButton(label,{w,h},ImGuiButtonFlags_EnableNav);
    if(clicked)*value=!*value;
    auto* d=ImGui::GetWindowDrawList();
    const auto bg=ImGui::IsItemHovered()?ImGuiCol_FrameBgHovered:ImGuiCol_FrameBg;
#if defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
    d->AddRectFilled(p,{p.x+w,p.y+h},ImGui::GetColorU32(ImGui::IsItemHovered()?bg:ImGuiCol_ChildBg),f*.3f);
    d->AddRect(p,{p.x+w,p.y+h},ImGui::GetColorU32(ImGuiCol_Border),f*.3f);
#else
    d->AddRectFilled(p,{p.x+w,p.y+h},ImGui::GetColorU32(bg),f*.2f);
#endif
    if(ImGui::IsItemFocused())d->AddRect(p,{p.x+w,p.y+h},ImGui::GetColorU32(Accent),f*.2f,0,2);
    d->AddText(ImGui::GetFont(),f,{p.x+pad,p.y+(h-text.y)*.5f},
        ImGui::GetColorU32(ImGuiCol_Text),label,nullptr,text_w);
    const ImVec2 a{p.x+w-toggle-pad,p.y+(h-f*.85f)*.5f};
    if(radio) {
        const ImVec2 center{a.x+toggle-f*.425f,a.y+f*.425f};
        d->AddCircle(center,f*.40f,ImGui::GetColorU32(Accent),0,2);
        if(*value)d->AddCircleFilled(center,f*.24f,ImGui::GetColorU32(Accent));
    } else {
#if defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
        const char* state=*value?"On":"Off";
        const auto state_size=ImGui::CalcTextSize(state);
        d->AddText({p.x+w-pad-state_size.x,p.y+(h-state_size.y)*.5f},
                   ImGui::GetColorU32(*value?Accent:ImVec4{.69f,.65f,.74f,1}),state);
#else
        d->AddRectFilled(a,{a.x+toggle,a.y+f*.85f},
            ImGui::GetColorU32(*value?Accent:ImVec4{.36f,.31f,.42f,1}),f);
        d->AddCircleFilled({a.x+(*value?toggle-f*.425f:f*.425f),a.y+f*.425f},
            f*.30f,ImGui::GetColorU32(ImGuiCol_Text));
#endif
    }
    return clicked;
#else
    return ImGui::Checkbox(label,value);
#endif
}

inline void Label(const char* label) {
    const float width=ControlWidth();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+width);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(width);
}
inline void AfterControl() {
#if !defined(__ANDROID__) && !defined(ENABLE_MM3D_GAME_FOLDER)
    ImGui::SameLine();
#endif
}
inline bool RadioButton(const char* label,int* v,int choice) {
#if defined(__ANDROID__) || defined(ENABLE_MM3D_GAME_FOLDER)
    bool selected=*v==choice;
    if(Checkbox(label,&selected,true)){*v=choice;return true;}
    return false;
#else
    return ImGui::RadioButton(label,v,choice);
#endif
}
inline bool SliderInt(const char* label,int* v,int min,int max,const char* format="%d",ImGuiSliderFlags flags=0) {
#if defined(__ANDROID__) || defined(ENABLE_MM3D_GAME_FOLDER)
    ImGui::PushID(label); Label(label);
    const bool changed=ImGui::SliderInt("##value",v,min,max,format,flags);
    ImGui::PopID();return changed;
#else
    return ImGui::SliderInt(label,v,min,max,format,flags);
#endif
}
inline bool SliderFloat(const char* label,float* v,float min,float max,const char* format="%.3f",ImGuiSliderFlags flags=0) {
#if defined(__ANDROID__) || defined(ENABLE_MM3D_GAME_FOLDER)
    ImGui::PushID(label); Label(label);
    const bool changed=ImGui::SliderFloat("##value",v,min,max,format,flags);
    ImGui::PopID();return changed;
#else
    return ImGui::SliderFloat(label,v,min,max,format,flags);
#endif
}
inline bool Combo(const char* label,int* v,const char* items,int height=-1) {
#if defined(__ANDROID__) || defined(ENABLE_MM3D_GAME_FOLDER)
    ImGui::PushID(label);Label(label);
    const bool changed=ImGui::Combo("##value",v,items,height);
    ImGui::PopID();return changed;
#else
    return ImGui::Combo(label,v,items,height);
#endif
}

inline constexpr const char* Pages[]={"General","Controls","Touch","Graphics","Audio","HUD",
    "Difficulty","Restoration","Quality of life","Mods","Cheats","About"};
inline constexpr const char* Descriptions[]={
    "Make Majora ReMasked comfortable on your screen.","Map your controller and game instructions.",
    "Touch controls and the game's touchscreen.","Frame rate, resolution and custom textures.",
    "Sound, music and volume.","Adjust the HUD while keeping your chosen layout.",
    "Damage, healing and world rules.","Choose your Project Restoration features.",
    "Small changes for a more comfortable adventure.","Optional character and equipment mods.","Game modifications and availability.",
    "About Majora ReMasked and its contributors."};

// Fixed header/footer, independently scrolling category rail and content. Text
// enlargement never scales the panel or consumes the content's scroll height.
inline void Begin(int& page,int width,int height,bool& resume,bool& quit) {
    if(ImGui::IsKeyPressed(ImGuiKey_PageUp))page=(page+int(std::size(Pages))-1)%int(std::size(Pages));
    if(ImGui::IsKeyPressed(ImGuiKey_PageDown))page=(page+1)%int(std::size(Pages));
    const float f=ImGui::GetFontSize();
    const float gap=std::max({12.f,float(height)*.018f,ImGui::GetStyle().WindowPadding.x,ImGui::GetStyle().WindowPadding.y});
    const float header=std::clamp(f*2.7f,float(height)*.095f,float(height)*.18f);
    const float footer=std::clamp(f*2.4f,float(height)*.09f,float(height)*.15f);
    const float rail=std::clamp(f*8.8f,float(width)*.22f,float(width)*.32f);
    const float body=float(height)-header-footer-gap*3;
    ImGui::SetCursorPos({gap,gap});
    ImGui::TextColored(Accent,"Majora ReMasked");ImGui::SameLine();ImGui::TextUnformatted("/ Settings");
    ImGui::SetCursorPos({float(width)-gap-f*5.5f,gap});
    resume=ImGui::Button("Resume",{f*5.5f,header-gap});
    ImGui::SetCursorPos({gap,header+gap});
    ImGui::BeginChild("Categories",{rail,body},ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    for(int i=0;i<int(std::size(Pages));++i) {
        ImGui::PushID(i);
        if(i==page)ImGui::PushStyleColor(ImGuiCol_Button,{.24f,.08f,.41f,1});
        else ImGui::PushStyleColor(ImGuiCol_Button,{0,0,0,0});
        if(ImGui::Button(Pages[i],{ImGui::GetContentRegionAvail().x,std::max(f*1.9f,float(height)*.065f)}))page=i;
        if(i==page)ImGui::SetItemDefaultFocus();
        ImGui::PopStyleColor();ImGui::PopID();
    }
    ImGui::PopStyleVar();ImGui::EndChild();
    ImGui::SetCursorPos({gap,header+gap+body+gap});
#ifdef ENABLE_MM3D_GAME_FOLDER
    quit=ImGui::Button("Reset",{rail,footer-gap});
#else
    quit=ImGui::Button("Quit game",{rail,footer-gap});
#endif
    ImGui::SetCursorPos({rail+gap*2,header+gap});
    ImGui::BeginChild("Settings panel",{float(width)-rail-gap*3,body},ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(Accent,"%s",Pages[page]);
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{.69f,.65f,.74f,1});
    ImGui::TextWrapped("%s",Descriptions[page]);
    ImGui::PopStyleColor();ImGui::Separator();
    const std::string id=std::string("Page/")+Pages[page];
    ImGui::BeginChild(id.c_str(),{0,0},ImGuiChildFlags_None);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x*.62f);
}
inline bool Page(int current,const char* name) {
    const std::string n=name;
    return n==Pages[current] || (n=="QOL"&&current==8);
}
inline void End(const char* help,int width,int height) {
    ImGui::PopItemWidth();ImGui::EndChild();ImGui::EndChild();
    const float f=ImGui::GetFontSize(),gap=std::max({12.f,float(height)*.018f,ImGui::GetStyle().WindowPadding.x,ImGui::GetStyle().WindowPadding.y});
    const float rail=std::clamp(f*8.8f,float(width)*.22f,float(width)*.32f);
    const float footer=std::clamp(f*2.4f,float(height)*.09f,float(height)*.15f);
    ImGui::SetCursorPos({rail+gap*2,float(height)-footer-gap});
    ImGui::BeginChild("Context help",{float(width)-rail-gap*3,footer},ImGuiChildFlags_None);
    ImGui::SetWindowFontScale(.72f);
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{.69f,.65f,.74f,1});
#ifdef __ANDROID__
    ImGui::TextWrapped("%s",*help?help:"Swipe from the left for settings. D-pad to navigate, A to select, B to resume.");
#else
    ImGui::TextWrapped("%s",*help?help:"Use the Settings button or settings shortcut. D-pad/arrow keys to navigate; confirm to select, back to resume.");
#endif
    ImGui::PopStyleColor();ImGui::EndChild();
}
}
