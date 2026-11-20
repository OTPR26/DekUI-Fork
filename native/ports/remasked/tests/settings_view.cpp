// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings_view.h"
#include "android_graphics_policy.h"
#include <stdexcept>
#include <iostream>
#include <filesystem>
using namespace ReMaskedSettings;
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){try{

 // Android policy must preserve engine values and never rewrite preferences on view.
 for(auto key:{"Upscaler","Upscaler quality","Global illumination","Advanced settings","Advanced graphics"}){Row r;r.Key=key;Check(!ReMaskedGraphics::Filter(r),"Build-excluded and desktop rows must be hidden");}
 for(auto key:{"Anti-aliasing","Ambient occlusion","Reflections"}){
  Row r;r.Key=key;r.Kind=RowKind::Choice;r.Choices={"Off","One","Two","Three","Four","Five"};int stored=5,writes=0,queried=-1;
  r.Read=[&]{return stored;};r.Write=[&](int v){stored=v;++writes;return WriteResult{};};r.ChoiceAvailable=[&](int v){queried=v;return v!=1;};
  Check(ReMaskedGraphics::Filter(r),"Supported row retained");Check(writes==0&&stored==5,"Viewing policy must not change stored preferences");
  Check(r.Read()==-1&&r.Notice().find("Saved mode")!=std::string::npos,"Hidden saved mode must be identified without overwriting it");
  r.Write(1);Check(stored==1&&writes==1,"Filtered choice must write the corresponding engine enum");
  Check(!r.ChoiceAvailable(1)&&queried==1,"Filtered choice must keep renderer validation");
  Check(r.Write(99).Error&&writes==1,"Out-of-range filtered choices must not write");
  ReMaskedGraphics::Verified=false;Check(r.Notice().find("Start the game")!=std::string::npos,"Unverified driver must request a game launch, not claim unsupported");
  ReMaskedGraphics::Verified=true;Check(r.Notice().find("Driver dependent")!=std::string::npos,"Both surfaces label conditional support");
 }
 ReMaskedGraphics::Verified=false;
 {Row r;r.Choices={"zero","one","two","three"};int saved=3;r.Read=[&]{return saved;};r.Write=[&](int v){saved=v;return WriteResult{};};ReMaskedGraphics::Choices(r,{0,3});Check(r.Read()==1,"Sparse enum must map to displayed choice");r.Write(1);Check(saved==3,"Sparse displayed choice must retain engine enum");}
 auto* ctx=ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1920,1080};io.DeltaTime=1.f/60;
 const auto fontRoot=std::filesystem::path(__FILE__).parent_path()/"../resources/fonts";
 FontPixels=64;
 BodyFont=io.Fonts->AddFontFromFileTTF((fontRoot/"FiraSans-Regular.ttf").c_str(),FontPixels,nullptr,GlyphRanges);
 BoldFont=io.Fonts->AddFontFromFileTTF((fontRoot/"FiraSans-Bold.ttf").c_str(),FontPixels,nullptr,GlyphRanges);
 HeadingFont=io.Fonts->AddFontFromFileTTF((fontRoot/"FiraSansCondensed-Regular.ttf").c_str(),FontPixels,nullptr,GlyphRanges);
 for(size_t i=0;i<SizedFaces.size();++i){const float pixels=InterfacePixelSizes[i];
  SizedFaces[i]={io.Fonts->AddFontFromFileTTF((fontRoot/"FiraSans-Regular.ttf").c_str(),pixels,nullptr,GlyphRanges),io.Fonts->AddFontFromFileTTF((fontRoot/"FiraSansCondensed-Regular.ttf").c_str(),pixels,nullptr,GlyphRanges),pixels};
 }
 io.FontDefault=BodyFont;io.Fonts->Build();
 for(const auto& faces:SizedFaces)for(auto* font:{faces.Body,faces.Heading})Check(font->FindGlyphNoFallback(0x2019),"Every baked menu size must contain typographic apostrophes");
 for(float target:{8.65f,17.3f,27.675f,34.6f,43.2f}){
  auto faces=FacesForSize(target);Check(target/faces.Pixels>.8f&&target/faces.Pixels<1.26f,"Use a nearby raster size, never aggressively shrink a giant atlas");
 }

 for(auto* font:{BodyFont,BoldFont,HeadingFont})for(ImWchar glyph:{ImWchar(0x27),ImWchar(0x2019),ImWchar(0x2018),ImWchar(0x201c),ImWchar(0x201d),ImWchar(0x2026)})Check(font&&font->FindGlyphNoFallback(glyph),"Actual interface faces must contain apostrophes and punctuation without fallback");
 Document doc;doc.InlineSettings=true;doc.CategorySidebar=true;int number=100;bool on=false;
 for(auto title:{"General","Controls","Touch","Graphics","Audio","HUD","Quality of life","Models","About","Data"}){
  Tab tab;tab.Title=title;Row h;h.Kind=RowKind::Heading;h.Key="Options";tab.Rows.push_back(h);
  Row toggle;toggle.Kind=RowKind::Toggle;toggle.Key="A neutral toggle";toggle.Read=[&]{return on;};toggle.Write=[&](int v){on=v;return WriteResult{};};tab.Rows.push_back(toggle);
  Row slider;slider.Kind=RowKind::Number;slider.Key="Size";slider.Minimum=50;slider.Maximum=200;slider.Read=[&]{return number;};slider.Write=[&](int v){number=v;return WriteResult{};};tab.Rows.push_back(slider);
  Row combo;combo.Kind=RowKind::Choice;combo.Key="Choice";combo.Choices={"First","Second"};combo.Read=[](){return 0;};combo.Write=[](int){return WriteResult{};};tab.Rows.push_back(combo);doc.Tabs.push_back(tab);
 }
 Model model(doc);model.Open();
 for(auto size:{ImVec2{1920,1080},ImVec2{1280,720},ImVec2{960,540}})for(float scale:{.5f,.8f,1.f,1.25f}){
  UserMenuScale=scale;io.DisplaySize=size;
  for(size_t i=0;i<doc.Tabs.size();i++){
   model.SelectTab(i);model.Refresh();ImGui::NewFrame();Draw(model);ImGui::Render();
   auto* nav=SettingsNav.Categories;auto* page=SettingsNav.Page;
   Check(nav&&page,"Both ReTuned navigation regions must exist");if(size.x==1920)Check(std::abs(nav->Size.x-15.f*scale*12.f)<1.1f,"Menu size must scale the actual ReTuned geometry");Check(nav->Pos.x+nav->Size.x<=page->Pos.x,"Category list and page cannot overlap");
   Check(page->Size.x>0&&page->Size.y>0&&page->Pos.x+page->Size.x<=size.x+1&&page->Pos.y+page->Size.y<=size.y+1,"Settings page must fit resized window");
   Check(ImGui::GetDrawData()->TotalVtxCount>0,"Actual inline widgets must draw");
  }
 }
 // Reduced game surfaces must retain the same physical menu size.
 DisplayDensity=369.f/160.f;DisplayHeight=1080.f;UserMenuScale=.8f;
 float physicalNav=0;
 for(auto size:{ImVec2{1920,1080},ImVec2{1280,720},ImVec2{960,540}}){
  io.DisplaySize=size;ImGui::NewFrame();Draw(model);ImGui::Render();
  const float physical=SettingsNav.Categories->Size.x*1080.f/size.y;
  if(physicalNav==0)physicalNav=physical;
  Check(std::abs(physical-physicalNav)<2.f,"Changing game render resolution must not enlarge the menu");
  Check(std::abs(40.f*MenuScale(size.y)*1080.f/size.y-73.8f)<.1f,"Odin 0.80x padding must match ReTuned's physical pixels");
 }
 UserMenuScale=.5f;Check(std::abs(MenuScale(720.f)-.76875f)<.001f,"0.50x must reach renderer without old minimum clamp");
 DisplayDensity=0;DisplayHeight=0;
 auto& row=model.Doc().Tabs[0].Rows[2];PendingEdits[&row]=156;FinishEdits(model);Check(number==156&&PendingEdits.empty(),"Closing or switching commits slider edits once");
 ImGuiStyle style;PurpleStyle(style);Check(style.ChildRounding==10&&style.FrameRounding==7&&style.FramePadding.x==12&&style.ItemSpacing.y==10,"ReTuned geometry must remain exact");
 Check(style.Colors[ImGuiCol_CheckMark].x>.7f&&style.Colors[ImGuiCol_CheckMark].z>.9f,"Accent must stay ReMasked purple");
 ImGui::DestroyContext(ctx);std::cout<<"ReTuned two-pane widgets: ten pages, three window sizes, four menu scales and physical display scaling, slider commit and purple style passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
