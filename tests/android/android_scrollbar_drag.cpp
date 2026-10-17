// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual ImGui scrollbar behavior for both panes with held-pointer input.
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include "imgui.h"
#include "imgui_internal.h"
#include "mm3d/android_menu_style.h"
using namespace MM3D::AndroidMenuStyle;
static void Frame() {
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({1920,1080});
    ImGui::Begin("Drag test",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings);
    int page=0;bool resume=false,quit=false;Begin(page,1920,1080,resume,quit);
    bool value=false;for(int i=0;i<35;i++){ImGui::PushID(i);Checkbox("Test setting",&value);ImGui::PopID();}
    End("",1920,1080);ImGui::End();ImGui::Render();
}
static void Drag(const char* name) {
    ImGuiWindow* window=nullptr;
    for(auto* w:ImGui::GetCurrentContext()->Windows)if(w->Active&&std::strstr(w->Name,name))window=w;
    assert(window&&window->ScrollbarY&&window->ScrollMax.y>0);
    const auto track=ImGui::GetWindowScrollbarRect(window,ImGuiAxis_Y);
    const float x=(track.Min.x+track.Max.x)*.5f,y=track.Min.y+30.f;
    auto& io=ImGui::GetIO();io.AddMousePosEvent(x,y);Frame();
    io.AddMouseButtonEvent(0,true);Frame();Frame();
    const float before=window->Scroll.y;
    io.AddMousePosEvent(x,y+120.f);Frame();Frame();
    const float dragged=window->Scroll.y;
    assert(dragged>before);
    assert(ImGui::GetCurrentContext()->ActiveId!=0);
    io.AddMouseButtonEvent(0,false);Frame();Frame();
    assert(std::abs(window->Scroll.y-dragged)<1.f);
    std::cout<<name<<": dragging down increases scroll; release keeps position\n";
}
int main(int argc,char** argv) {
    assert(argc==2);ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DeltaTime=1.f/60;io.DisplaySize={1920,1080};
    assert(io.Fonts->AddFontFromFileTTF(argv[1],52));unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(1);io.FontGlobalScale=.75f;
    Theme(ImGui::GetStyle());ImGui::GetStyle().ScaleAllSizes(3);
    Frame();Frame();Frame();Drag("Categories");Drag("Page/General");
    ImGui::DestroyContext();std::cout<<"PASS: sidebar and content scrollbar drag/release\n";
}
