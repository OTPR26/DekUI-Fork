// SPDX-License-Identifier: GPL-2.0-or-later
#include "oot3d/renderer/azahar_texture_pack.h"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace Oot3d::Renderer;
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){auto root=std::filesystem::temp_directory_path()/("remasked-texture-switch-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{
 auto hud=root/"hud",hd=root/"hd",user=root/"user";
 for(auto dir:{hud,hd,user})std::filesystem::create_directories(dir);
 std::ofstream(hud/"pack.json")<<R"({"options":{"use_new_hash":true}})";
 std::ofstream(hd/"pack.json")<<R"({"options":{"use_new_hash":true}})";
 // An empty user folder with legacy hash metadata must not mask the bundled HD pack.
 std::ofstream(user/"pack.json")<<R"({"options":{"use_new_hash":false}})";
 std::array<uint8_t,256> h{},w{},rgba{};h.fill(17);w.fill(29);rgba.fill(255);
 const AzaharTextureRequest hudRequest{8,8,0,0,h,rgba},worldRequest{8,8,0,0,w,rgba};
 const auto write=[&](const std::filesystem::path& dir,const AzaharTextureRequest& request,uint8_t red){std::array<uint8_t,256> pixels;pixels.fill(255);for(size_t i=0;i<pixels.size();i+=4)pixels[i]=red;WriteAzaharTexturePng(dir/MakeAzaharTextureFilename(8,8,ComputeAzaharTextureHash(request,true),0,0),8,8,pixels);};
 write(hud,hudRequest,11);write(hd,worldRequest,77);
 AzaharTexturePackRuntime runtime;runtime.SetBaseDirectories({hud});runtime.SetCustomBaseDirectories({hd});
 AzaharTexturePackConfiguration config;config.UserDirectory=root;config.LoadDirectory=user;
 const auto red=[&](const AzaharTextureRequest& request){auto result=runtime.ResolveAndMaybeDump(request);return result?int((*result->Rgba8)[0]):-1;};
 for(bool enabled:{true,false,true}){config.LoadCustomTextures=enabled;runtime.Configure(config);runtime.WaitForPendingLoads();Check(red(hudRequest)==11,"Required HUD must remain enabled");Check(red(worldRequest)==(enabled?77:-1),"On must enable HD textures and Off must disable them");}
 // A nonempty user pack still wins over bundled HD assets.
 std::ofstream(user/"pack.json")<<R"({"options":{"use_new_hash":true}})";write(user,worldRequest,99);runtime.Reload();runtime.WaitForPendingLoads();Check(red(worldRequest)==99,"A real user pack must retain precedence");
 std::filesystem::remove_all(root);std::cout<<"Custom textures On/Off/On, required HUD preservation, empty-folder hash mode and user-pack priority passed\n";
}catch(const std::exception& e){std::filesystem::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}}
