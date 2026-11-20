// SPDX-License-Identifier: GPL-2.0-or-later
// ReTuned's unified settings presentation, bound to ReMasked's existing owners.
#pragma once
#include "settings_style.h"
#include "settings_navigation.h"
#include "fast/extension/remasked_interface_fonts.h"
#include "oot3d_menu_model.h"
#include "oot3d_menu_font.h"
#include "oot3d_menu_presentation.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <map>
#include <functional>

namespace ReMaskedSettings {
using namespace Oot3dNativeGame::Menu;
// Match display dp even when the game renders below physical resolution.
inline std::atomic<float> DisplayDensity{0.f}, DisplayHeight{0.f};
inline float MenuScale(float viewportHeight){
 const char* text=std::getenv("REMASKED_ANDROID_DENSITY");
 const float reported=DisplayDensity.load();
 const float density=reported>0?reported:(text?std::clamp(std::strtof(text,nullptr),1.f,4.f):1.f);
 const float height=DisplayHeight.load();
 return UserMenuScale.load()*density*(height>0?viewportHeight/height:1.f);
}
inline int LastDrawFrame=-2;
inline size_t LastPage=0;
inline bool ExternalBack=false,ExternalBackHeld=false;
inline const char* CloseLabel="Resume";
inline void ObserveBack(bool held){ExternalBack|=held&&!ExternalBackHeld;ExternalBackHeld=held;}
inline const char* Description(std::string_view title){
 if(title=="General")return "Make Majora ReMasked comfortable on your screen.";
 if(title=="Controls")return "Map your controller and game controls.";
 if(title=="Touch")return "Adjust touch controls and item icons.";
 if(title=="Graphics")return "Frame rate, camera, resolution and custom textures.";
 if(title=="Audio")return "Sound and volume.";
 if(title=="HUD")return "Adjust the HUD while keeping your chosen layout.";
 if(title=="Quality of life")return "Small changes for a more comfortable adventure.";
 if(title=="Models")return "Player appearance and imported models.";
 if(title=="Data")return "Import and export saves, or choose where ReMasked stores game data.";
 return "About Majora ReMasked and its controls.";
}
inline std::map<Row*,int> PendingEdits;
inline void Apply(Model& model,Row& row,int value){
 if(!row.Write)return;
 auto result=row.Write(value);model.SetStatus(result.Message.empty()?(result.Error?"Could not save":"Saved"):result.Message,result.Error);model.Refresh();
}
inline void FinishEdits(Model& model){auto pending=std::move(PendingEdits);PendingEdits.clear();for(auto [row,value]:pending)Apply(model,*row,value);}
inline std::function<ImTextureID(const std::string&)> LoadIcon;
inline ImTextureID IconTexture(const std::string& name){return LoadIcon?LoadIcon(name):0;}
inline void IconChoices(Model& model,Row& row){
 ImGui::TextUnformatted(row.Key.c_str());const float f=ImGui::GetFontSize(),side=f*4.7f;
 const float width=ImGui::GetContentRegionAvail().x;const int columns=std::max(1,int((width+ImGui::GetStyle().ItemSpacing.x)/(side+ImGui::GetStyle().ItemSpacing.x)));
 for(size_t i=0;i<row.Choices.size();++i){
  if(i%columns)ImGui::SameLine();ImGui::PushID(int(i));
  const auto pos=ImGui::GetCursorScreenPos();const bool selected=row.Value==int(i);
  const bool enabled=row.Enabled&&(!row.ChoiceAvailable||row.ChoiceAvailable(int(i)));ImGui::BeginDisabled(!enabled);
  if(ImGui::Selectable("##Icon",selected,0,{side,side+f*1.8f}))Apply(model,row,int(i));
  auto* draw=ImGui::GetWindowDrawList();
  if(i<row.ChoiceImages.size())if(auto texture=IconTexture(row.ChoiceImages[i]))draw->AddImage(texture,{pos.x+f*.25f,pos.y+f*.25f},{pos.x+side-f*.25f,pos.y+side-f*.25f});
  const auto text=ImGui::CalcTextSize(row.Choices[i].c_str());draw->AddText({pos.x+(side-text.x)*.5f,pos.y+side+f*.2f},ImGui::GetColorU32(ImGuiCol_Text),row.Choices[i].c_str());
  ImGui::EndDisabled();ImGui::PopID();
 }
 if(!row.Help.empty())ImGui::TextWrapped("%s",row.Help.c_str());ImGui::Spacing();
}
inline void DrawRow(Model& model,Row& row){
 ImGui::BeginDisabled(!row.Enabled);
 if(row.Kind==RowKind::Toggle){bool value=row.Value!=0;if(Toggle(row.Key.c_str(),&value))Apply(model,row,value);}
 else if(row.Kind==RowKind::Number){
  const float available=ImGui::GetContentRegionAvail().x;
  const bool stacked=ImGui::CalcTextSize(row.Key.c_str()).x+ImGui::GetFontSize()*8>available;
  if(stacked){ImGui::TextWrapped("%s",row.Key.c_str());ImGui::SetNextItemWidth(available);}
  const std::string label=stacked?"##Value":row.Key;
  int value=PendingEdits.contains(&row)?PendingEdits.at(&row):row.Value;std::string format=row.Format?row.Format(value):"%d";
  if(row.Format){std::string literal;for(char c:format){literal+=c;if(c=='%')literal+='%';}format=std::move(literal);}
  if(ImGui::SliderInt(label.c_str(),&value,row.Minimum,row.Maximum,format.c_str(),ImGuiSliderFlags_AlwaysClamp)){
   const int step=std::max(1,row.Step);value=std::clamp(row.Minimum+int(std::lround(float(value-row.Minimum)/step))*step,row.Minimum,row.Maximum);PendingEdits[&row]=value;
  }
  if(ImGui::IsItemDeactivatedAfterEdit()&&PendingEdits.contains(&row)){const int final=PendingEdits.at(&row);PendingEdits.erase(&row);Apply(model,row,final);}
 }
 else if(row.Kind==RowKind::Choice){
  const std::string preview=row.ValueText();
  const bool stacked=ImGui::CalcTextSize(row.Key.c_str()).x+ImGui::GetFontSize()*8>ImGui::GetContentRegionAvail().x;
  if(stacked){ImGui::TextWrapped("%s",row.Key.c_str());ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);}
  const std::string label=stacked?"##Value":row.Key;
  if(ImGui::BeginCombo(label.c_str(),preview.c_str())){
   for(size_t i=0;i<row.Choices.size();++i){const bool available=!row.ChoiceAvailable||row.ChoiceAvailable(int(i));ImGui::BeginDisabled(!available);if(ImGui::Selectable(row.Choices[i].c_str(),row.Value==int(i)))Apply(model,row,int(i));if(row.Value==int(i))ImGui::SetItemDefaultFocus();ImGui::EndDisabled();}
   ImGui::EndCombo();
  }
 }
 else if(row.Kind==RowKind::Action){if(ImGui::Button(row.Key.c_str())&&row.Activate){auto result=row.Activate();model.SetStatus(result.Message,result.Error);model.Refresh();}}
 else if(row.Kind==RowKind::Info){ImGui::TextUnformatted(row.Key.c_str());ImGui::TextWrapped("%s",(row.DynamicText?row.DynamicText():row.Text).c_str());}
 if(!row.Help.empty()&&ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){ImGui::BeginTooltip();ImGui::PushTextWrapPos(ImGui::GetFontSize()*28);ImGui::TextUnformatted(row.Help.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}
 ImGui::EndDisabled();
 if(row.Notice){auto text=row.Notice();if(!text.empty())ImGui::TextWrapped("%s",text.c_str());}
 if(!row.Enabled&&!row.UnavailableReason.empty())ImGui::TextDisabled("%s",row.UnavailableReason.c_str());
}
inline void Modal(Model& model){
 if(!model.CurrentModal())return;
 auto& modal=*model.CurrentModal();ImGui::OpenPopup("##ReMaskedQuestion");
 ImGui::SetNextWindowSize({std::min(ImGui::GetMainViewport()->WorkSize.x*.85f,ImGui::GetFontSize()*40),0},ImGuiCond_Always);
 if(ImGui::BeginPopupModal("##ReMaskedQuestion",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings)){
  ImGui::TextWrapped("%s",modal.Title.c_str());ImGui::Separator();for(const auto& line:modal.Lines)ImGui::TextWrapped("%s",line.c_str());ImGui::Spacing();
  bool yes=ImGui::Button(modal.ConfirmLabel.c_str());bool no=ImGui::IsKeyPressed(ImGuiKey_Escape,false)||ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false);if(no&&modal.SingleButton()){yes=true;no=false;}
  if(!modal.CancelLabel.empty()){ImGui::SameLine();no=ImGui::Button(modal.CancelLabel.c_str());}
  if(yes||no){auto action=yes?modal.OnConfirm:modal.OnCancel;const bool card=model.CardOnly();model.SetModal(std::nullopt);ImGui::CloseCurrentPopup();if(action)action();if(card)model.Close();}
  ImGui::EndPopup();
 }
}
inline void Draw(Model& model){
 if(!model.IsOpen())return;
 auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
 const bool opening=LastDrawFrame+1!=ImGui::GetFrameCount();LastDrawFrame=ImGui::GetFrameCount();
 if(opening)SettingsNav=SettingsNavigation{};
 SettingsNav.Begin(false);
 if(ExternalBack){ExternalBack=false;if(!SettingsNav.HadPopup&&!SettingsNav.HadEditor&&!model.CurrentModal()){if(SettingsNav.LastPane==SettingsNavigation::Pane::Page)SettingsNav.FocusCategories=true;else SettingsNav.Close=true;}}
 const ImGuiStyle previous=ImGui::GetStyle();const float previousFontScale=io.FontGlobalScale;
 PurpleStyle(ImGui::GetStyle());const auto* viewport=ImGui::GetMainViewport();
 const float scale=MenuScale(viewport->WorkSize.y);const auto faces=FacesForSize(15.f*scale);const float pixels=faces.Body?faces.Pixels:ImGui::GetFont()->FontSize;
 io.FontGlobalScale=15.f*scale/pixels;ImGui::GetStyle().ScaleAllSizes(scale);
 ImGui::GetStyle().FramePadding.y=std::max(ImGui::GetStyle().FramePadding.y,12.f*scale);ImGui::GetStyle().WindowPadding.x=std::max(ImGui::GetStyle().WindowPadding.x,40.f*scale);
 ImGui::PushFont(faces.Body?faces.Body:ImGui::GetFont());
 ImGui::SetNextWindowPos(viewport->WorkPos,ImGuiCond_Always);ImGui::SetNextWindowSize(viewport->WorkSize,ImGuiCond_Always);
 constexpr auto flags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDocking;
 if(ImGui::Begin("Majora ReMasked / Settings",nullptr,flags)){
  ImGui::SetWindowFontScale(1.f);ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),"Majora ReMasked");ImGui::SameLine();ImGui::TextUnformatted(" / Settings");
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-ImGui::GetFontSize()*8));if(ImGui::Button(CloseLabel,{ImGui::GetFontSize()*6,0})){FinishEdits(model);model.Close();}ImGui::Separator();
  ImGui::BeginChild("##UnifiedContents",{0,-ImGui::GetFontSize()*2.2f});
  const float font=ImGui::GetFontSize(),navWidth=std::min(ImGui::GetContentRegionAvail().x*.27f,font*12);
  ImGui::BeginChild("##Navigation",{navWidth,0},true);float selectedY=0;
  for(size_t i=0;i<model.Doc().Tabs.size();++i){const bool selected=model.TabIndex()==i;if(selected)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_Header));
   const bool activated=ImGui::Button(model.Doc().Tabs[i].Title.c_str(),{-1,std::max(font*2.5f,44.f)});
   if(selected||activated){SettingsNav.SelectedCategory=ImGui::GetItemID();selectedY=ImGui::GetItemRectMin().y-ImGui::GetWindowPos().y+font;}
   if(activated){FinishEdits(model);model.SelectTab(i);SettingsNav.ActivateCategory(ImGui::GetItemID());}
   if(selected)ImGui::PopStyleColor();
  }
  SettingsNav.CategoriesDrawn(selectedY);ImGui::EndChild();ImGui::SameLine();
  if(opening||LastPage!=model.TabIndex()){ImGui::SetNextWindowScroll({0,0});LastPage=model.TabIndex();}
  ImGui::BeginChild("##SettingsPage",{0,0},true);SettingsNav.PageBegun();
  auto& page=model.Doc().Tabs[model.TabIndex()];
  if(faces.Heading)ImGui::PushFont(faces.Heading);ImGui::SetWindowFontScale(20.f/15.f);ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark),"%s",page.Title.c_str());ImGui::SetWindowFontScale(1.f);if(faces.Heading)ImGui::PopFont();
  ImGui::TextWrapped("%s",Description(page.Title));if(page.Title=="Models")ImGui::TextWrapped("Appearance changes apply at the next game launch. Save before using the PAK Model Loader.");ImGui::Separator();ImGui::Spacing();
  ImGui::PushItemWidth(std::min(ImGui::GetContentRegionAvail().x*.58f,font*18));bool sectionVisible=true;
  for(size_t i=0;i<page.Rows.size();++i){auto& row=page.Rows[i];ImGui::PushID(int(i));
   if(row.Kind==RowKind::Heading){const bool collapse=(page.Title=="HUD"&&row.Key!="Overall HUD")||row.Key=="Individual controls"||row.Key=="Save states"||row.Key=="Lighting"||row.Key=="Grass";
    sectionVisible=!collapse||ImGui::CollapsingHeader(row.Key.c_str());if(!collapse)ImGui::SeparatorText(row.Key.c_str());
   }else if(sectionVisible){if(!row.ChoiceImages.empty())IconChoices(model,row);else DrawRow(model,row);}
   ImGui::PopID();
  }
  ImGui::PopItemWidth();ImGui::EndChild();ImGui::EndChild();ImGui::Spacing();
  if(model.StatusIsError())ImGui::TextColored({1,.45f,.45f,1},"%s",model.Status().c_str());else ImGui::TextDisabled("Settings save automatically. Tap an option or use your controller.");
  Modal(model);
 }
 ImGui::End();ImGui::PopFont();ImGui::GetStyle()=previous;io.FontGlobalScale=previousFontScale;
 SettingsNav.End();if(SettingsNav.Close){FinishEdits(model);model.Close();}
}
}
