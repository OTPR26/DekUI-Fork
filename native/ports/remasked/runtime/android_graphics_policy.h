// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oot3d_menu_model.h"
#include <algorithm>
#include <atomic>
namespace ReMaskedGraphics {
inline std::atomic<bool> Verified{false};
// Preserve engine enum values when hiding build-excluded choices.
inline void Choices(Oot3dNativeGame::Menu::Row& row,std::vector<int> ids){
 auto labels=row.Choices;auto read=row.Read;auto write=row.Write;auto available=row.ChoiceAvailable;
 row.Choices.clear();for(int id:ids)row.Choices.push_back(labels.at(id));
 row.Read=[read,ids]{int value=read?read():0;auto it=std::find(ids.begin(),ids.end(),value);return it==ids.end()?-1:int(it-ids.begin());};
 row.Write=[write,ids](int i)->Oot3dNativeGame::Menu::WriteResult{if(i<0||i>=int(ids.size()))return {"Unavailable on Android",true};return write(ids[i]);};
 row.ChoiceAvailable=[available,ids](int i){return i>=0&&i<int(ids.size())&&(!available||available(ids[i]));};
}
inline bool Filter(Oot3dNativeGame::Menu::Row& row){
 const auto& k=row.Key;
 if(k=="Upscaler"||k=="Upscaler quality"||k=="Global illumination"||k=="Advanced settings"||k=="Advanced graphics")return false;
 if(k=="Anti-aliasing") {Choices(row,{0,1,2,3,4});row.Help="Smooths edges. SMAA, MSAA and Temporal AA depend on this driver and the renderer's available guides.";}
 if(k=="Ambient occlusion"){Choices(row,{0,1});row.Help="Darkens creases and contact areas using Multiscale GTAO.";}
 if(k=="Reflections"){Choices(row,{0,1});row.Help="Hi-Z screen-space reflections need scene color, depth and material guides. Original water reflections remain when Off.";}
 if(k=="Lighting model")row.Help="Native preserves original game lighting. Modern adds per-pixel lighting on supported materials; it can cost performance.";
 if(k=="Shadows")row.Help="Virtual shadow maps require Modern lighting and driver support.";
 if(k=="Toon")row.Help="Optional cel shading. PICA material toon requires the modern material pipeline.";
 if(k=="Grass")row.Help="Interactive grass quality; only scenes with supported grass placement show extra blades.";
 if(k=="Distant grass cards")row.Help="Distant grass detail; requires grass to be enabled and a supported scene.";
 bool conditional=k=="Anti-aliasing"||k=="MSAA samples"||k=="Anisotropic filtering"||k=="Lighting model"||k=="Ambient occlusion"||k=="Shadows"||k=="Reflections"||k=="Toon"||k=="Grass"||k=="Distant grass cards";
 if(conditional){auto read=row.Read;row.Notice=[k,read]{if(read&&read()<0)return std::string("Saved mode is not available in this Android build. Choose an available mode to replace it.");if(!Verified.load())return std::string("Start the game once to check support for this driver.");if(k=="Shadows")return std::string("Requires Modern lighting; unavailable choices are disabled.");if(k=="Grass"||k=="Distant grass cards")return std::string("Scene dependent: extra grass appears only where placement is supported.");return std::string("Driver dependent; unavailable choices are disabled.");};}
 return true;
}
}
