// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "android_graphics_policy.h"
#include "fast/extension/graphics_settings_runtime.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>
namespace ReMaskedGraphics {
using Json=nlohmann::json;
inline std::string Env(const char* name){auto p=std::getenv(name);return p?p:"";}
inline void Load(){
 static std::string loaded;auto key=Env("REMASKED_GRAPHICS_DEVICE_KEY");if(key==loaded)return;loaded=key;
 auto& runtime=Fast::Extension::GraphicsSettingsRuntime::Instance();using Cap=Fast::Extension::GraphicsCapability;
 Json caps;Verified=false;
 try{std::ifstream f(Env("REMASKED_GRAPHICS_CAPABILITIES"));Json root;f>>root;if(root.at("key")==key&&root.at("version")==1){caps=root.at("capabilities");if(caps.is_array()&&caps.size()==size_t(Fast::Extension::kLastGraphicsCapability)+1){
 bool valid=true;for(const auto& state:caps)valid=valid&&state.is_object()&&state.contains("available")&&state["available"].is_boolean()&&state.contains("reason")&&state["reason"].is_string();Verified=valid;
 }}}catch(...){}
 for(int i=0;i<=int(Fast::Extension::kLastGraphicsCapability);++i){bool value=false;std::string reason="Start game to check this driver";try{if(Verified){value=caps.at(i).at("available");reason=caps.at(i).at("reason");}}catch(...){Verified=false;}runtime.SetCapability(Cap(i),value,reason);}
 // These capabilities belong to the MM3D title, not the GPU.
 runtime.SetCapability(Cap::TitleSceneView,true);runtime.SetCapability(Cap::TitleSceneFov,true);
}
inline void Publish(){
 static auto next=std::chrono::steady_clock::time_point{};auto now=std::chrono::steady_clock::now();if(now<next)return;next=now+std::chrono::seconds(5);
 auto key=Env("REMASKED_GRAPHICS_DEVICE_KEY"),path=Env("REMASKED_GRAPHICS_CAPABILITIES");if(key.empty()||path.empty())return;
 try{auto caps=Fast::Extension::GraphicsSettingsRuntime::Instance().Capabilities();Json states=Json::array();for(int i=0;i<=int(Fast::Extension::kLastGraphicsCapability);++i){const auto& s=caps.Get(Fast::Extension::GraphicsCapability(i));states.push_back({{"available",s.Available},{"reason",s.Reason}});}
 Json root{{"version",1},{"key",key},{"capabilities",states}};static std::string previous;auto text=root.dump();if(text!=previous){auto temp=path+".partial";std::ofstream f(temp);f<<text;f.close();if(!f)return;std::filesystem::rename(temp,path);previous=text;}Verified=true;
 }catch(...){} // A cache failure never interrupts gameplay or writes preferences.
}
}
