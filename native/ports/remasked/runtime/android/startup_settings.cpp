// SPDX-License-Identifier: GPL-3.0-or-later
// File-backed settings host. No title session, Vulkan device or shader preparation.
#include "android_menu.h"
#include "android_graphics_support.h"
#include "settings_view.h"
#include "oot3d_menu_textures.h"
#include "ship/audio/AudioOutputPolicy.h"
#include "fast/extension/graphics_settings_runtime.h"
#include "fast/extension/graphics_settings_persistence.h"
#include "remasked_interface_font_data.h"
#include <imgui_impl_opengl3.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace {
using namespace Oot3dNativeGame;
using Json=nlohmann::json;
struct Store final:Fast::Extension::GraphicsSettingsPersistencePort,Ship::AudioOutputPersistencePort{
 std::filesystem::path Path;
 explicit Store(std::filesystem::path p):Path(std::move(p)){}
 bool LoadRoot(Json& root)override{try{root=ReMaskedMenu::ReadFile(Path);return true;}catch(...){return false;}}
 bool Put(const char* key,const Json& value){try{auto root=ReMaskedMenu::ReadFile(Path);root[key]=value;ReMaskedMenu::AtomicWrite(Path,root);return true;}catch(...){return false;}}
 bool StoreGraphics(const Json& j)override{return Put("Graphics",j);}
 bool StoreGraphicsPresets(const Json& j)override{return Put("GraphicsPresets",j);}
 bool LoadAudio(Json& j)override{Json root;if(!LoadRoot(root))return false;j=root.value("Audio",Json{});return true;}
 bool StoreAudio(const Json& j,std::string* error)override{if(Put("Audio",j))return true;if(error)*error="Audio could not be saved";return false;}
};
std::unique_ptr<InGameMenu> SettingsMenu;
std::shared_ptr<NativeControlConfigRuntime> Controls;
std::string Route;
std::unordered_map<std::string,GLuint> Icons;
ImGuiContext* Context=nullptr;
std::chrono::steady_clock::time_point LastFrame;
ImTextureID Icon(const std::string& name){
 if(auto it=Icons.find(name);it!=Icons.end())return (ImTextureID)(intptr_t)it->second;
 auto pixels=Oot3dNativeGame::Menu::MenuImagePixels(name);if(!pixels)return 0;
 GLuint id;glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,pixels->width,pixels->height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels->rgba8.data());Icons[name]=id;return (ImTextureID)(intptr_t)id;
}
void Fonts(){
 using namespace ReMaskedSettings;
 auto& io=ImGui::GetIO();ImFontConfig config;config.FontDataOwnedByAtlas=false;config.OversampleH=2;config.OversampleV=2;
 for(size_t i=0;i<SizedFaces.size();++i){float p=InterfacePixelSizes[i];auto b=io.Fonts->AddFontFromMemoryTTF(FontData::Body,sizeof(FontData::Body),p,&config,GlyphRanges);auto h=io.Fonts->AddFontFromMemoryTTF(FontData::Heading,sizeof(FontData::Heading),p,&config,GlyphRanges);SizedFaces[i]={b,h,p};}
 auto defaults=FacesForSize(24);BodyFont=defaults.Body;HeadingFont=defaults.Heading;FontPixels=defaults.Pixels;io.FontDefault=BodyFont;
}
jstring String(JNIEnv* e,const Json& j){auto s=j.dump();return e->NewStringUTF(s.c_str());}
}
extern "C" JNIEXPORT jstring JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedSettingsActivity_create(JNIEnv* e,jclass,jstring directory){
 try{
 const char* text=e->GetStringUTFChars(directory,nullptr);std::filesystem::path root(text);e->ReleaseStringUTFChars(directory,text);
 if(Context){ImGui::SetCurrentContext(Context);ImGui_ImplOpenGL3_Shutdown();ImGui::DestroyContext(Context);Icons.clear();}
 Context=ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;Fonts();
 if(!ImGui_ImplOpenGL3_Init("#version 300 es"))throw std::runtime_error("Settings interface could not initialize");
 ReMaskedSettings::LoadIcon=Icon;ReMaskedSettings::CloseLabel="Back";
 if(!SettingsMenu){
  const auto game=root/"GameContent";setenv("REMASKED_GAME_PRESENTATION",(game/"runtime-presentation.json").c_str(),1);
  if(!Mm3dSingleScreen::LoadSettings(game/"presentation.json"))throw std::runtime_error("ReMasked settings could not be loaded");
  auto store=std::make_shared<Store>(game/"native-game.json");Fast::Extension::InstallGraphicsSettingsPersistencePort(store);
  ReMaskedGraphics::Load();
  auto controls=NativeControlDefaults();std::string error;if(!LoadNativeControlConfig(game/"oot3d_controls.json",&controls,&error))throw std::runtime_error("Controller settings could not be loaded");
  Controls=std::make_shared<NativeControlConfigRuntime>(game/"oot3d_controls.json",controls);
  std::vector<GameLanguage> available;auto languages=ReMaskedMenu::ReadFile(game/"game_language.json");for(auto& l:languages.at("available"))available.push_back({l.at("code"),l.at("label"),l.at("system_id")});
  auto language=std::make_shared<GameLanguageSettings>(game/"game_language.json",available);
  InGameMenuHost host;host.Controls=Oot3dNativeGame::Menu::HelpControls::Touch;host.SaveDataDirectory=game/"saves";host.LogsDirectory=game/"logs";
  host.OpenInstallerPage=[](std::string_view page,std::string*){Route=page;return true;};host.QuitGame=[](){Route="back";};
  host.AppendSettingsTabs=[](Oot3dNativeGame::Menu::Document& document){ReMaskedMenu::Append(document,[](std::string page){Route=std::move(page);});};
  SettingsMenu=std::make_unique<InGameMenu>(InGameMenuOwners{Controls,nullptr,language,nullptr,nullptr,nullptr,std::move(host),std::make_shared<Ship::AudioOutputPolicy>(store),{.QuickPutAway=true,.RollDash=true},true});
  for(auto& tab:SettingsMenu->Model().Doc().Tabs){
   std::erase_if(tab.Rows,[](const auto& row){return row.Key=="Quit game";});
   for(auto& row:tab.Rows){
    // Live preview/capture actions have no game behind this startup surface.
    if(row.Key=="Advanced graphics"||row.Key=="Edit layout"||row.Key=="Calibrate controller gyro"){row.Available=[](){return false;};row.UnavailableReason="Available during gameplay";}
    if(tab.Title=="Graphics"&&row.Write&&row.Key!="Output resolution"){
     auto write=row.Write;row.Write=[write,store](int value){auto result=write(value);if(result.Error)return result;
      // Display confirmation belongs to a running renderer. Here a graphics
      // edit is a next-launch preference, persisted through the same store.
      if(!store->StoreGraphics(Fast::Extension::SerializeGraphicsSettings(Fast::Extension::GraphicsSettingsRuntime::Instance().Snapshot())))return Oot3dNativeGame::Menu::WriteResult{"Settings could not be saved",true};
      return Oot3dNativeGame::Menu::WriteResult{"Saved for the next game launch",false};};
    }
   }
  }
 }
 SettingsMenu->Model().Open();LastFrame=std::chrono::steady_clock::now();return nullptr;
 }catch(const std::exception& error){return e->NewStringUTF(error.what());}
}
extern "C" JNIEXPORT jstring JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedSettingsActivity_frame(JNIEnv* e,jclass,jint width,jint height,jfloat density){
 if(!SettingsMenu||!Context)return nullptr;ImGui::SetCurrentContext(Context);auto& io=ImGui::GetIO();io.DisplaySize={float(width),float(height)};
 auto now=std::chrono::steady_clock::now();io.DeltaTime=std::max(.001f,std::chrono::duration<float>(now-LastFrame).count());LastFrame=now;
 ReMaskedSettings::DisplayHeight=float(height);ReMaskedSettings::DisplayDensity=density;
 ReMaskedGraphics::Load();
 ImGui_ImplOpenGL3_NewFrame();ImGui::NewFrame();SettingsMenu->Refresh();ReMaskedSettings::Draw(SettingsMenu->Model());ImGui::Render();
 glViewport(0,0,width,height);glClearColor(.035f,.025f,.05f,1);glClear(GL_COLOR_BUFFER_BIT);ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
 auto pending=ReMaskedMenu::TakeTouch();bool close=!SettingsMenu->Model().IsOpen()||(!Route.empty()&&!Route.starts_with("remasked-save-"));if(pending.empty()&&!close&&Route.empty())return nullptr;
 Json result{{"preferences",pending},{"close",close},{"route",Route}};Route.clear();return String(e,result);
}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedSettingsActivity_pointer(JNIEnv*,jclass,jfloat x,jfloat y,jboolean down){if(Context){ImGui::SetCurrentContext(Context);auto& io=ImGui::GetIO();io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);io.AddMousePosEvent(x,y);io.AddMouseButtonEvent(0,down);}}
extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedSettingsActivity_key(JNIEnv*,jclass,jint key,jboolean down){
 if(!Context)return;ImGui::SetCurrentContext(Context);ImGuiKey mapped=ImGuiKey_None;
 switch(key){case 19:mapped=ImGuiKey_UpArrow;break;case 20:mapped=ImGuiKey_DownArrow;break;case 21:mapped=ImGuiKey_LeftArrow;break;case 22:mapped=ImGuiKey_RightArrow;break;case 23:case 66:case 96:mapped=ImGuiKey_Enter;break;case 4:case 97:mapped=ImGuiKey_Escape;break;}
 if(mapped!=ImGuiKey_None)ImGui::GetIO().AddKeyEvent(mapped,down);
}

extern "C" JNIEXPORT void JNICALL Java_io_github_otpr26_remasked_nativegame_ReMaskedSettingsActivity_category(JNIEnv* e,jclass,jstring value){if(!SettingsMenu)return;const char* text=e->GetStringUTFChars(value,nullptr);for(size_t i=0;i<SettingsMenu->Model().Doc().Tabs.size();++i)if(SettingsMenu->Model().Doc().Tabs[i].Title==text)SettingsMenu->Model().SelectTab(i);e->ReleaseStringUTFChars(value,text);}
