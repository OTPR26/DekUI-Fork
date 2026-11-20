// SPDX-License-Identifier: GPL-3.0-or-later
#include "oot3d/renderer/azahar_texture_pack.h"
#include <jni.h>
#include "../hud_setup.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_otpr26_remasked_nativegame_HudSetup_prepareNative(
    JNIEnv* env, jclass, jstring staging, jbyteArray archive, jbyteArray atlas) {
    if (!staging || !archive || !atlas) return JNI_FALSE;
    const auto archiveSize = env->GetArrayLength(archive);
    const auto atlasSize = env->GetArrayLength(atlas);
    if (archiveSize > 128 * 1024 || atlasSize > 2 * 1024 * 1024) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(staging, nullptr);
    if (!path) return JNI_FALSE;
    jbyte* layouts = env->GetByteArrayElements(archive, nullptr);
    jbyte* artwork = layouts ? env->GetByteArrayElements(atlas, nullptr) : nullptr;
    const bool success = artwork && remasked_prepare_hud(path,
        reinterpret_cast<const uint8_t*>(layouts), archiveSize,
        reinterpret_cast<const uint8_t*>(artwork), atlasSize);
    if (artwork) env->ReleaseByteArrayElements(atlas, artwork, JNI_ABORT);
    if (layouts) env->ReleaseByteArrayElements(archive, layouts, JNI_ABORT);
    env->ReleaseStringUTFChars(staging, path);
    return success ? JNI_TRUE : JNI_FALSE;
}

#include "oot3d_menu_font.h"
#include "mm3d_single_screen.h"
extern "C" JNIEXPORT void JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_trimTextureMemory(JNIEnv*, jclass, jint mib) {
    const uint64_t budget = mib <= 16 ? 16 : mib <= 64 ? 64 : 192;
    Oot3d::Renderer::AzaharTexturePackRuntime::Instance().SetDecodedCacheBudgetBytes(budget << 20U);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_setGameplayTuning(
    JNIEnv*,jclass,jfloat distance,jfloat walk,jfloat climb,jfloat swim,jboolean resetCamera) {
    return Oot3dNativeGame::Mm3dSingleScreen::SetGameplayTuning(distance,walk,climb,swim,resetCamera==JNI_TRUE)?JNI_TRUE:JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_applyPresentationBytes(
    JNIEnv* env,jclass,jbyteArray json) {
    if(json==nullptr)return JNI_FALSE;
    const auto size=env->GetArrayLength(json);
    if(size<1 || size>1024*1024)return JNI_FALSE;
    std::string document(static_cast<size_t>(size),'\0');
    env->GetByteArrayRegion(json,0,size,reinterpret_cast<jbyte*>(document.data()));
    if(env->ExceptionCheck())return JNI_FALSE;
    std::string error;
    return Oot3dNativeGame::Mm3dSingleScreen::ApplySettingsPatchJson(document,&error)?JNI_TRUE:JNI_FALSE;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_activePresentationBytes(JNIEnv* env,jclass) {
    const auto json=Oot3dNativeGame::Mm3dSingleScreen::ActiveSettingsJson();
    auto result=env->NewByteArray(static_cast<jsize>(json.size()));
    if(result!=nullptr)env->SetByteArrayRegion(result,0,static_cast<jsize>(json.size()),reinterpret_cast<const jbyte*>(json.data()));
    return result;
}

#include "android_menu.h"
#include "fast/extension/remasked_secondary_swapchain.h"
#include "settings_view.h"
#include "oot3d_menu_prompts.h"
extern "C" JNIEXPORT void JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_reportMenuDisplayMetrics(JNIEnv*,jclass,jfloat density,jfloat height){
 if(std::isfinite(density)&&density>0&&std::isfinite(height)&&height>0){
  ReMaskedSettings::DisplayDensity=std::clamp(density,1.f,4.f);
  ReMaskedSettings::DisplayHeight=height;
 }
}
extern "C" JNIEXPORT void JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_reportTouchPreferences(JNIEnv* env,jclass,jbyteArray bytes){
 if(!bytes)return;auto size=env->GetArrayLength(bytes);if(size<2||size>65536)return;
 std::string text(size,'\0');env->GetByteArrayRegion(bytes,0,size,reinterpret_cast<jbyte*>(text.data()));
 auto json=ReMaskedMenu::Json::parse(text,nullptr,false);if(!json.is_discarded()){
  ReMaskedDual::Requested=json.value("dualScreen",0)==1;
  ReMaskedMenu::ReportTouch(json);
  Oot3dNativeGame::Menu::UserMenuScale=std::clamp(json.value("menuScale",100),50,125)/100.f;
  Oot3dNativeGame::Mm3dSingleScreen::SetTouchIconOpacity(std::clamp(json.value("itemIconOpacity",100),0,100)/100.f);
  std::array<float,6> scales{};constexpr int ids[]{700,702,703,701,773,774};
  for(unsigned i=0;i<6;++i)scales[i]=(json.value("controlScale-"+std::to_string(ids[i]),50)+50)/100.f;
  Oot3dNativeGame::Mm3dSingleScreen::SetTouchIconScales(scales);
 }
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_takeTouchPreferences(JNIEnv* env,jclass){
 auto text=ReMaskedMenu::TakeTouch().dump();auto result=env->NewByteArray(text.size());if(result)env->SetByteArrayRegion(result,0,text.size(),reinterpret_cast<const jbyte*>(text.data()));return result;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_nativeMenuVisible(JNIEnv*,jclass){return ReMaskedMenu::Visible.load(std::memory_order_relaxed)?JNI_TRUE:JNI_FALSE;}
// Exercises real menu callbacks against an isolated settings directory supplied
// by instrumentation, never the device's external GameContent directory.
extern "C" JNIEXPORT jstring JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_checkNativeMenuContract(JNIEnv* env,jclass){
 using namespace ReMaskedMenu;
 try{
  Oot3dNativeGame::Menu::Document document;Tab controls;controls.Title="Controls";document.Tabs.push_back(controls);
  Tab video;video.Title="Video";Row renderScale;renderScale.Key="Render scale";renderScale.Kind=RowKind::Number;renderScale.Minimum=50;renderScale.Maximum=200;renderScale.Step=10;renderScale.Read=[](){return 100;};video.Rows.push_back(std::move(renderScale));document.Tabs.push_back(std::move(video));
  std::string routed;Append(document,[&](std::string page){routed=page;});
  if(document.Tabs.size()!=10||!document.InlineSettings)throw std::runtime_error("Shared settings categories missing");
  const auto tab=[&](const std::string& title)->Tab&{auto it=std::find_if(document.Tabs.begin(),document.Tabs.end(),[&](const Tab& t){return t.Title==title;});if(it==document.Tabs.end())throw std::runtime_error("Missing category: "+title);return *it;};
  auto& rows=tab("Graphics").Rows;
  const auto findRow=[](std::vector<Row>& rows,const std::string& key)->Row& {
    auto it=std::find_if(rows.begin(),rows.end(),[&](const Row& r){return r.Key==key;});
    if(it==rows.end())throw std::runtime_error("Missing menu row: "+key);return *it;
  };
  auto& distance=findRow(rows,"Camera distance");
  if(distance.Step!=1||findRow(tab("Quality of life").Rows,"Walking speed").Step!=1||findRow(tab("Quality of life").Rows,"Climbing speed").Step!=1||findRow(tab("Quality of life").Rows,"Swimming speed").Step!=1)throw std::runtime_error("Camera/movement controls must support 1-percent steps");
  if(distance.Write(160).Error||Current().at("camera_distance").get<double>()<1.59)throw std::runtime_error("Camera callback not connected");
  auto path=RuntimePath();auto canonical=path.parent_path()/"presentation.json";
  if(ReadFile(canonical).at("future_model_options").at("name")!="Link — 日本語 🎮")throw std::runtime_error("Unknown canonical fields lost");
  auto before=ReadFile(canonical);
  if(!distance.Write(301).Error||ReadFile(canonical)!=before)throw std::runtime_error("Out-of-range menu edit wrote settings");
  if(Save(Json{{"hud_buttons_scale",1.73},{"hud_diamond_enabled",true}}).Error)throw std::runtime_error("HUD menu write failed");
  if(Save(PresetPatch(Current(),2,1)).Error||Save(Json{{"hud_buttons_scale",1.2}}).Error||Save(PresetPatch(Current(),2,0)).Error)throw std::runtime_error("Preset callback failed");
  if(Current().at("hud_buttons_scale").get<double>()<1.72)throw std::runtime_error("Preset did not restore layout");
  if(PresetPatch(Current(),2,2).size()!=1)throw std::runtime_error("Preset clear touched unrelated fields");
  for(auto& page:document.Tabs)for(auto& row:page.Rows)if(page.Title.find("Photo")!=std::string::npos||row.Key.find("Photo mode")!=std::string::npos||row.Key.find("F6")!=std::string::npos)throw std::runtime_error("Desktop photo mode remains reachable");
  for(const auto key:{"Disable Tatl reminders","Disable Bomber interruptions","Disable low-health beep","Disable heart drops","Disable rupee drops","Disable fairy drops","Disable magic drops","Disable ammo pickups","Lose rupees on death","End the world on death"}){
    auto& row=findRow(tab("Quality of life").Rows,key);const int original=row.Read();if(row.Write(!original).Error||row.Read()==original||row.Write(original).Error)throw std::runtime_error("QoL callback did not persist");
  }
  auto& touchRows=tab("Touch").Rows;
  for(auto [key,value]:std::vector<std::pair<std::string,int>>{{"Camera touch mode",1},{"Shoulder labels",1},{"Item icon opacity",65}}){auto& row=findRow(touchRows,key);if(row.Write(value).Error||row.Read()!=value)throw std::runtime_error("Touch choice not connected");}TakeTouch();
  findRow(tab("Data").Rows,"Import saves").Activate();if(routed!="remasked-save-import")throw std::runtime_error("Data import route missing");
  findRow(tab("Data").Rows,"Export saves").Activate();if(routed!="remasked-save-export")throw std::runtime_error("Data export route missing");
  findRow(tab("Data").Rows,"Choose custom data folder…").Activate();if(routed!="remasked-data-folder")throw std::runtime_error("Custom data route missing");
  findRow(tab("Graphics").Rows,"Use system driver").Activate();if(TakeTouch().at("platformAction")!="remasked-driver-system")throw std::runtime_error("System driver must act inline");
  findRow(tab("Graphics").Rows,"Choose driver ZIP…").Activate();if(TakeTouch().at("platformAction")!="remasked-driver-pick")throw std::runtime_error("Driver picker must act inline");
  auto& output=findRow(tab("Graphics").Rows,"Output resolution");
  if(output.Choices!=std::vector<std::string>{"Native display","720p","1080p"}||output.Read()!=0||output.Write(1).Error||TakeTouch().at("outputResolution")!=1||output.Write(0).Error)throw std::runtime_error("Native display output must be independent of game scale");TakeTouch();
  if(findRow(tab("Graphics").Rows,"Render scale").Step!=1)throw std::runtime_error("Render scale must support precise independent resolution");
  auto& menuSize=findRow(tab("General").Rows,"Menu size");if(menuSize.Write(115).Error||menuSize.Read()!=115)throw std::runtime_error("Menu sizing not connected");TakeTouch();
  if(menuSize.Minimum!=50||menuSize.Write(50).Error||menuSize.Read()!=50||TakeTouch().at("menuScale")!=50||!menuSize.Write(49).Error)throw std::runtime_error("Menu must support 0.50x and reject smaller values");
  findRow(tab("General").Rows,"Reset menu size").Activate();if(menuSize.Read()!=100||TakeTouch().at("menuScale")!=100)throw std::runtime_error("ReTuned default must be exactly 1.00x");
  for(const auto& page:document.Tabs)for(const auto& row:page.Rows)for(const auto& text:{row.Key,row.Text,row.Help})for(auto cp:Oot3dNativeGame::Menu::DecodeUtf8(text))if(cp>=0xe000&&cp<=0xf8ff)throw std::runtime_error("Old controller font markers remain in menu text");
  auto& movement=findRow(touchRows,"Movement stick position");
  if(movement.Kind!=RowKind::Choice||movement.Choices!=std::vector<std::string>{"Fixed","Floating"})throw std::runtime_error("Movement mode choices missing");
  movement.Write(1);if(movement.Read()!=1||TakeTouch().at("floating")!=true)throw std::runtime_error("Floating choice not connected");
  movement.Write(0);if(movement.Read()!=0)throw std::runtime_error("Fixed choice not connected");auto pending=TakeTouch();
  if(!pending.contains("floating")||pending.at("floating")!=false)throw std::runtime_error("Touch preference bridge lost boolean type");
  auto& playerRows=tab("Models").Rows;auto active=Current();
  if(playerRows[1].Write(1).Error||Current()!=active)throw std::runtime_error("Next-launch appearance modified the live title");
  playerRows.back().Activate();if(routed!="remasked-pak")throw std::runtime_error("PAK route missing");
  for(auto& tab:document.Tabs)for(auto& row:tab.Rows)if(row.Read)(void)row.Read();
  // The same rows must remain reachable at phone, handheld and smaller-window
  // aspect ratios; long tab strips and long HUD pages scroll inside the canvas.
  Oot3dNativeGame::Menu::Model model(document);model.Open();
  for(float width:{240.f*16/9,240.f*19.5f/9,320.f})for(float scale:{1.f,1.72f,2.45f}){
   for(size_t tab=0;tab<document.Tabs.size();++tab){model.SelectTab(tab);std::vector<oot3d::ui::UiPrimitive> primitives;
    Oot3dNativeGame::Menu::AppendMenuPresentation(model,width,240,primitives,{},Oot3dNativeGame::Menu::HintsFor(Oot3dNativeGame::Menu::PromptSet::Touch),{Oot3dNativeGame::Menu::kDp*scale,true});
    bool selectedTab=false;for(auto& hit:model.CurrentLayout().Regions){auto a=hit.Area;if(a.X<-.01f||a.Y<-.01f||a.X+a.Width>width+.01f||a.Y+a.Height>240.01f)throw std::runtime_error("Responsive menu target outside window");selectedTab|=hit.Kind==Oot3dNativeGame::Menu::HitKind::Tab&&hit.Index==tab;}
    if(!selectedTab)throw std::runtime_error("Selected settings tab hidden after resize");
   }
  }
  // Render the actual unified widgets, without a renderer or user data,
  // at handheld and resized-window sizes. Check the two navigation regions.
  auto* previousContext=ImGui::GetCurrentContext();auto* testContext=ImGui::CreateContext();
  try{
   auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={1920,1080};io.DeltaTime=1.f/60;io.Fonts->AddFontDefault();io.Fonts->Build();
   for(const ImVec2 size:{ImVec2{1920,1080},ImVec2{1280,720},ImVec2{960,540}}){
    io.DisplaySize=size;
    for(size_t i=0;i<document.Tabs.size();++i){
     model.SelectTab(i);model.Open();model.Refresh();ImGui::NewFrame();ReMaskedSettings::Draw(model);ImGui::Render();
     auto* categories=ReMaskedSettings::SettingsNav.Categories;auto* page=ReMaskedSettings::SettingsNav.Page;
     if(!categories||!page||categories->Pos.x+categories->Size.x>page->Pos.x+1||page->Size.x<=0||page->Pos.x+page->Size.x>size.x+1||ImGui::GetDrawData()->TotalVtxCount<=0)throw std::runtime_error("Unified settings panes overlap or leave the window");
    }
   }
  }catch(...){ImGui::DestroyContext(testContext);ImGui::SetCurrentContext(previousContext);throw;}
  ImGui::DestroyContext(testContext);ImGui::SetCurrentContext(previousContext);ReMaskedSettings::SettingsNav={};ReMaskedSettings::LastDrawFrame=-2;
  return nullptr;
 }catch(const std::exception& error){return env->NewStringUTF(error.what());}
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_otpr26_remasked_nativegame_ReMaskedGameActivity_reportAppIconPixels(JNIEnv* env,jclass,jint index,jint width,jint height,jbyteArray bytes){
 if(index<0||index>4||width<=0||height<=0||width>1024||height>1024||!bytes||env->GetArrayLength(bytes)!=width*height*4)return;
 oot3d::ui::UiTexturePixels pixels;pixels.width=width;pixels.height=height;pixels.rgba8.resize(size_t(width)*height*4);
 env->GetByteArrayRegion(bytes,0,pixels.rgba8.size(),reinterpret_cast<jbyte*>(pixels.rgba8.data()));
 if(!env->ExceptionCheck())Oot3dNativeGame::Menu::RegisterMenuImage("menu/image/remasked-icon/"+std::to_string(index),std::move(pixels));
}

extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedDualScreen_surface(JNIEnv* env,jclass,jobject surface){ReMaskedDual::AndroidSetLowerSurface(env,surface);}
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedDualScreen_displayWanted(JNIEnv*,jclass){return ReMaskedDual::Requested.load();}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedDualScreen_touch(JNIEnv*,jclass,jint x,jint y,jboolean down){ReMaskedDual::SetTouch(x,y,down);}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedDualScreen_size(JNIEnv*,jclass,jint w,jint h){ReMaskedDual::Width=std::max(0,w);ReMaskedDual::Height=std::max(0,h);}
