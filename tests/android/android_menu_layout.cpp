// SPDX-License-Identifier: GPL-2.0-or-later
// Build with the bundled ImGui sources and -D__ANDROID__; no emulator or game required.
#include <cassert>
#include <iostream>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"
#include "mm3d/android_menu_style.h"
using namespace MM3D::AndroidMenuStyle;
static void CheckWidth() {
    const auto* w=ImGui::GetCurrentWindow();
    const auto& r=ImGui::GetCurrentContext()->LastItemData.Rect;
    assert(r.Min.x>=w->WorkRect.Min.x-1);
    assert(r.Max.x<=w->WorkRect.Max.x+1);
}
static void Layout(int width,int height,int step,int chosen) {
    auto& io=ImGui::GetIO();io.DisplaySize={float(width),float(height)};
    const float scale=std::clamp(float(height)/1080.f,.5f,4.f)*(1.8f+.06f*step);
    io.FontGlobalScale=scale*.25f;ImGui::GetStyle()=ImGuiStyle{};ImGui::StyleColorsDark();Theme(ImGui::GetStyle());ImGui::GetStyle().ScaleAllSizes(scale);
    for(int frame=0;frame<3;++frame) {
        ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Test",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar);
        bool resume=false,quit=false;int page=chosen;
        Begin(page,width,height,resume,quit);assert(page==chosen);assert(Page(page,Pages[chosen]));
        assert(ImGui::GetWindowWidth()>width*.5f);
        bool enabled=true;Checkbox("Single-screen game instructions",&enabled);CheckWidth();assert(enabled);
        int size=step;SliderInt("Menu size",&size,0,40,"%+d");CheckWidth();
        const auto slider=ImGui::GetCurrentContext()->LastItemData.Rect;
        AfterControl();ImGui::Button("Reset size");CheckWidth();
        assert(ImGui::GetCurrentContext()->LastItemData.Rect.Min.y>=slider.Max.y);
        for(int i=0;i<25;i++){ImGui::PushID(i);Checkbox("A longer setting label that must wrap rather than overflow",&enabled);CheckWidth();ImGui::PopID();}
        End("",width,height);
        auto* root=ImGui::GetCurrentWindow();assert(root->Pos.x==0&&root->Pos.y==0);
        if(root->Size.x!=width||root->Size.y!=height)std::cerr<<width<<"x"<<height<<" step="<<step<<" actual="<<root->Size.x<<","<<root->Size.y<<" frame="<<frame<<"\n";assert(root->Size.x==width&&root->Size.y==height);
        if(frame==2) { if(root->ScrollMax.x!=0||root->ScrollMax.y!=0)std::cerr<<width<<"x"<<height<<" step="<<step<<" root scroll="<<root->ScrollMax.x<<","<<root->ScrollMax.y<<"\n";assert(root->ScrollMax.x==0&&root->ScrollMax.y==0); }
        ImGui::End();ImGui::Render();
    }
}
int main(int argc,char** argv) {
    assert(argc==2);ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DeltaTime=1.f/60;
    assert(io.Fonts->AddFontFromFileTTF(argv[1],52));unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(1);
    for(const auto size:{ImVec2{1920,1080},ImVec2{1280,720},ImVec2{960,540}})
        for(int step:{0,20,40})for(int page=0;page<int(std::size(Pages));page++)Layout(int(size.x),int(size.y),step,page);
    ImGui::DestroyContext();std::cout<<"PASS: "<<9*std::size(Pages)<<" size/scale/page combinations; full-screen bounds, wrapped toggles, slider/reset reachability and no parent scrolling.\n";
}
