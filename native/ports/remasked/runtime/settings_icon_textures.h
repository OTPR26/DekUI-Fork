// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "settings_view.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/window/Window.h"
namespace ReMaskedSettings {
inline ImTextureID UploadIconTexture(const std::string& name){
 if(!ImGui::GetIO().BackendRendererName)return {};
 auto* context=Ship::Context::GetRawInstance();if(!context||!context->GetWindow())return {};
 auto gui=std::dynamic_pointer_cast<Fast::Fast3dGui>(context->GetWindow()->GetGui());if(!gui)return {};
 if(!gui->HasTextureByName(name)){
  auto pixels=Oot3dNativeGame::Menu::MenuImagePixels(name);if(!pixels)return {};
  auto texture=std::make_shared<Ship::GuiTexture>();texture->Data=nullptr;
  texture->DataSize=pixels->rgba8.size();texture->Data=static_cast<uint8_t*>(std::malloc(texture->DataSize));if(!texture->Data)return {};
  std::memcpy(texture->Data,pixels->rgba8.data(),texture->DataSize);texture->Metadata.Width=pixels->width;texture->Metadata.Height=pixels->height;
  gui->LoadTextureFromResource(name,texture);
 }
 return gui->GetTextureByName(name);
}
inline void InstallIconLoader(){if(!LoadIcon)LoadIcon=UploadIconTexture;}
}
