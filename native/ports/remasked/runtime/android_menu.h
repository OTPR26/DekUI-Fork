// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oot3d_in_game_menu.h"
#include "android_graphics_policy.h"
#include "mm3d_single_screen.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <array>
#include <cmath>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <tuple>

namespace ReMaskedMenu {
using Json=nlohmann::json;
using Oot3dNativeGame::Menu::Row;
using Oot3dNativeGame::Menu::RowKind;
using Oot3dNativeGame::Menu::Tab;
using Oot3dNativeGame::Menu::WriteResult;
struct TouchState {std::mutex Mutex;Json Values=Json::object(),Pending=Json::object();};
inline std::atomic<bool> Visible{false};
inline TouchState& Touch(){static TouchState state;return state;}
inline void ReportTouch(const Json& values){if(!values.is_object())return;auto& s=Touch();std::lock_guard lock(s.Mutex);s.Values=values;for(auto& [k,v]:s.Pending.items())s.Values[k]=v;}
inline Json TakeTouch(){auto& s=Touch();std::lock_guard lock(s.Mutex);Json pending=std::move(s.Pending);s.Pending=Json::object();return pending;}
inline Json ReadFile(const std::filesystem::path& path){std::ifstream stream(path);if(!stream)throw std::runtime_error("Settings unavailable");Json j;stream>>j;if(!j.is_object())throw std::runtime_error("Invalid settings");return j;}
inline std::filesystem::path RuntimePath(){const char* p=std::getenv("REMASKED_GAME_PRESENTATION");if(!p||!*p)throw std::runtime_error("Settings unavailable");return p;}
inline std::filesystem::path ModelsPath(){if(const char* p=std::getenv("REMASKED_MODELS_ROOT"))return p;return RuntimePath().parent_path().parent_path()/"load/mm3d-models";}
inline bool ModelAvailable(const std::string& folder){auto root=ModelsPath()/folder;for(auto path:{"romfs/actors/zelda2_link_child_new.gar.lzs","variants/master/romfs/actors/zelda2_link_child_new.gar.lzs","variants/kokiri/romfs/actors/zelda2_link_child_new.gar.lzs"})if(std::filesystem::is_regular_file(root/path))return true;return false;}
inline Json Current(bool canonical=false){return canonical?ReadFile(RuntimePath().parent_path()/"presentation.json"):Json::parse(Oot3dNativeGame::Mm3dSingleScreen::ActiveSettingsJson());}
inline void AtomicWrite(const std::filesystem::path& path,const Json& value){auto temp=path;temp+=".menu-partial";try{std::ofstream f(temp,std::ios::trunc);f<<value.dump(2)<<'\n';f.flush();if(!f)throw std::runtime_error("Cannot write settings");f.close();std::filesystem::rename(temp,path);}catch(...){std::error_code ec;std::filesystem::remove(temp,ec);throw;}}
inline WriteResult Save(const Json& patch,bool nextLaunch=false){
  Json active;bool applied=false;std::filesystem::path canonical;Json before;bool firstWritten=false;
  try{
    if(!patch.is_object())return {"Invalid settings",true};
    auto runtime=RuntimePath();canonical=runtime.parent_path()/"presentation.json";
    before=ReadFile(canonical);auto originalRuntime=ReadFile(runtime);auto c=before,r=originalRuntime;
    for(const auto& [k,v]:patch.items()){c[k]=v;r[k]=v;}
    if(!nextLaunch){active=Current();std::string error;if(!Oot3dNativeGame::Mm3dSingleScreen::ApplySettingsPatchJson(patch.dump(),&error))return {"Settings rejected",true};applied=true;}
    AtomicWrite(canonical,c);firstWritten=true;AtomicWrite(runtime,r);
    return {nextLaunch?"Saved for the next game launch":"Saved",false};
  }catch(...){try{if(firstWritten)AtomicWrite(canonical,before);if(applied){std::string error;Oot3dNativeGame::Mm3dSingleScreen::ApplySettingsJson(active.dump(),&error);}}catch(...){}return {"Settings could not be saved",true};}
}
inline Row Heading(std::string label){Row r;r.Key=std::move(label);r.Kind=RowKind::Heading;return r;}
inline Row Number(std::string key,std::string label,int min,int max,int step=1,int denominator=1,double fallback=0,bool nextLaunch=false,bool touch=false){
 Row r;r.Key=std::move(label);r.Kind=RowKind::Number;r.Minimum=min;r.Maximum=max;r.Step=step;
 r.Format=[denominator](int v){if(denominator==100)return std::to_string(v)+"%";if(denominator==10){char text[32];std::snprintf(text,sizeof(text),"%.1f",v/10.0);return std::string(text);}return std::to_string(v);};
 r.Read=[key,denominator,fallback,nextLaunch,touch]{try{Json j;if(touch){auto& s=Touch();std::lock_guard lock(s.Mutex);j=s.Values;}else j=Current(nextLaunch);return static_cast<int>(std::lround(j.value(key,fallback)*denominator));}catch(...){return static_cast<int>(std::lround(fallback*denominator));}};
 r.Write=[key,denominator,min,max,nextLaunch,touch](int v)->WriteResult{if(v<min||v>max)return {"Value out of range",true};Json value=denominator==1?Json(v):Json(static_cast<double>(v)/denominator);if(touch){auto& s=Touch();std::lock_guard lock(s.Mutex);s.Values[key]=value;s.Pending[key]=value;return {};}return Save(Json{{key,value}},nextLaunch);};
 return r;
}
inline Row Toggle(std::string key,std::string label,bool fallback=false,bool nextLaunch=false,bool touch=false){
 Row r=Number(key,std::move(label),0,1,1,1,0,nextLaunch,touch);r.Kind=RowKind::Toggle;
 r.Read=[key,fallback,nextLaunch,touch]{try{if(touch){auto& s=Touch();std::lock_guard lock(s.Mutex);return s.Values.value(key,fallback)?1:0;}return Current(nextLaunch).value(key,fallback)?1:0;}catch(...){return fallback?1:0;}};
 r.Write=[key,nextLaunch,touch](int v)->WriteResult{if(touch){auto& s=Touch();std::lock_guard lock(s.Mutex);s.Values[key]=v!=0;s.Pending[key]=v!=0;return {};}return Save(Json{{key,v!=0}},nextLaunch);};return r;
}
inline Row SizeSteps(std::string key,std::string label){auto r=Number(std::move(key),std::move(label),-10,10);r.Format=[](int v){return std::to_string(100+5*v)+"%";};return r;}
inline Json PresetPatch(const Json& source,int slot,int action){
 if(slot<1||slot>3||action<0||action>2)throw std::runtime_error("Unknown preset");
 std::string prefix="hud_button_layout_"+std::to_string(slot)+"_";Json patch=Json::object();
 if(action==2){patch[prefix+"saved"]=false;return patch;}
 if(action==0&&!source.value(prefix+"saved",false))throw std::runtime_error("This preset is empty");
 std::vector<std::pair<std::string,std::string>> pairs={{"hud_buttons_scale",prefix+"scale"},{"hud_buttons_flip_peak",prefix+"flip_peak"}};
 for(auto key:{"enabled","x","y","spacing","rotation"})pairs.emplace_back("hud_diamond_"+std::string(key),prefix+"diamond_"+key);
 for(int i=0;i<6;++i)for(auto key:{"x","y","scale"})pairs.emplace_back("hud_button_"+std::to_string(i)+"_"+key,prefix+std::to_string(i)+"_"+key);
 for(auto& [current,saved]:pairs){const auto& from=action==0?saved:current;const auto& to=action==0?current:saved;if(source.contains(from))patch[to]=source[from];}
 if(action==1)patch[prefix+"saved"]=true;return patch;
}
inline void Organize(Oot3dNativeGame::Menu::Document& document,const std::function<void(std::string)>& route){
 std::array<Tab,10> pages;const char* titles[]={"General","Controls","Touch","Graphics","Audio","HUD","Quality of life","Models","Data","About"};
 for(unsigned i=0;i<pages.size();++i)pages[i].Title=titles[i];
 auto add=[&](unsigned page,Row row){
  if((page==3||row.Key=="Advanced settings")&&!ReMaskedGraphics::Filter(row))return;
  if(row.Key=="Photo camera"||row.Key=="Photo mode"||row.Key=="Free camera"||row.Key=="All settings (F1)"||row.Key=="Share diagnostics"||row.Key=="Show the help card")return;
  if(row.Key=="Export saves"){row.Key="Manage saves";row.Help="Close the game, then export or import native save slots using Android’s file picker.";}
  if(row.Key=="Show Camera stick")row.Key="Camera touch control";
  if(row.Key=="Advanced settings"){row.Key="Advanced graphics";row.Help="Additional renderer settings.";}
  for(auto* text:{&row.Help,&row.Text}){
   for(const auto& [old,replacement]:std::vector<std::pair<std::string,std::string>>{{"F1 > Materials > Texture packs","Graphics > Textures"},{"F1 > Lighting > Toon","Graphics > Advanced graphics"},{"F1 > Grass > Performance","Graphics > Advanced graphics"},{"F1 > Grass","Graphics > Advanced graphics"},{"Forge > Enhancements imports a pack; the folders are in Graphics > Textures.","Texture packs are managed from Graphics."},{"Game tab > Advanced settings","Graphics > Advanced graphics"},{"F1 > Lighting","Graphics > Advanced graphics"},{"F1 > Controls","Controls"},{"in F1","in Advanced graphics"},{"Game tab","Data"}}){
    size_t pos=0;while((pos=text->find(old,pos))!=std::string::npos){text->replace(pos,old.size(),replacement);pos+=replacement.size();}
   }
  }
  pages[page].Rows.push_back(std::move(row));
 };
 for(auto& tab:document.Tabs){std::string section;
  for(auto& row:tab.Rows){if(row.Kind==RowKind::Heading)section=row.Key;
   if(tab.Title=="Video"){
    if(row.Key=="Window mode"||row.Key=="Resolution"||row.Key=="Reset display settings")continue;
    if(row.Key=="Render scale"){row.Step=1;row.Help="Game resolution relative to the output. At 1080p output, 67% is about 720p; 100% is 1080p. Menu text stays at the output resolution.";}
    add(3,std::move(row));
   }else if(tab.Title=="Graphics")add(3,std::move(row));
   else if(tab.Title=="Controls"){
    bool touch=section=="Touch controls"||section=="ReMasked touch controls";
    if(row.Key=="ReMasked touch controls")continue;
    add(touch?2:1,std::move(row));
   }else if(tab.Title=="ReMasked"){
    if(row.Key=="Camera and movement")continue;
    if(row.Key=="App icon")add(0,std::move(row));
    else if(row.Key=="Camera distance"){add(3,Heading("Camera"));row.Help="Exploration camera distance. Walls can bring the camera closer to avoid clipping.";add(3,std::move(row));}
    else add(6,std::move(row));
   }else if(tab.Title=="HUD")add(5,std::move(row));
   else if(tab.Title=="Models")add(7,std::move(row));
   else if(tab.Title=="Game"){
    if(section=="Audio")add(4,std::move(row));
    else if(section=="Language")add(0,std::move(row));
    else if(section=="Save states"){if(row.Key.find("(F5)")!=std::string::npos)row.Key="Save state";if(row.Key.find("(F8)")!=std::string::npos)row.Key="Load last state";add(8,std::move(row));}
    else if(row.Key=="Advanced settings")add(3,std::move(row));
    else if(row.Key=="Quit game")add(0,std::move(row));
    else if(section=="Gameplay")add(6,std::move(row));
   }else if(tab.Title!="Photo mode"){
    if(row.Key=="Photo camera")continue;
    const bool saves=row.Key.find("save")!=std::string::npos||row.Key.find("Save")!=std::string::npos;
    add(saves?8:9,std::move(row));
   }
  }
 }
 auto screen=Number("dualScreen","Screen mode",0,1,1,1,0,false,true);screen.Kind=RowKind::Choice;screen.Choices={"Single Screen","Dual Screen"};
 screen.Help="Dual Screen restores the native bottom screen on a second Android display, like the AYN Thor. Single Screen is the default. If the display disconnects, gameplay returns to Single Screen without changing your selection.";
 screen.Notice=[] {auto& s=Touch();std::lock_guard lock(s.Mutex);return s.Values.value("dualScreen",0)==1&&!s.Values.value("lowerDisplayAvailable",false)?std::string("Waiting for a second display; using Single Screen."):std::string{};};
 pages[0].Rows.insert(pages[0].Rows.begin(),std::move(screen));
 auto output=Number("outputResolution","Output resolution",0,2,1,1,0,false,true);
 output.Kind=RowKind::Choice;output.Choices={"Native display","720p","1080p"};
 output.Help="Interface and output resolution. Applies next game launch. The game render scale is independent; output cannot exceed the display resolution.";
 auto saveOutput=output.Write;output.Write=[saveOutput](int v){auto result=saveOutput(v);if(!result.Error)result.Message="Saved; applies next game launch";return result;};
 pages[3].Rows.insert(pages[3].Rows.begin(),std::move(output));
 const auto status=[](std::string key,std::string label,std::string fallback){Row r;r.Kind=RowKind::Info;r.Key=std::move(label);r.DynamicText=[key,fallback]{auto& s=Touch();std::lock_guard lock(s.Mutex);return s.Values.value(key,fallback);};return r;};
 const auto action=[&](std::string label,std::string page){Row r;r.Kind=RowKind::Action;r.Key=std::move(label);r.Activate=[route,page]{if(page.starts_with("remasked-driver-")){auto& s=Touch();std::lock_guard lock(s.Mutex);s.Pending["platformAction"]=page;return WriteResult{"Applies next game launch",false};}route(page);return WriteResult{};};return r;};
 pages[3].Rows.insert(pages[3].Rows.begin(),action("Choose driver ZIP…","remasked-driver-pick"));
 pages[3].Rows.insert(pages[3].Rows.begin(),action("Use system driver","remasked-driver-system"));
 pages[3].Rows.insert(pages[3].Rows.begin(),status("graphicsDriver","Current graphics driver","System driver"));
 // The Data page owns ordinary save transfers and the native data root.
 std::erase_if(pages[8].Rows,[](const Row& row){return row.Key=="Manage saves"||row.Key=="Export saves"||(row.Kind==RowKind::Info&&row.Key=="Save states");});
 pages[8].Rows.insert(pages[8].Rows.begin(),action("Use app data folder","remasked-data-default"));
 pages[8].Rows.insert(pages[8].Rows.begin(),action("Choose custom data folder…","remasked-data-folder"));
 pages[8].Rows.insert(pages[8].Rows.begin(),status("dataDirectory","Current data folder","App data folder"));
 pages[8].Rows.insert(pages[8].Rows.begin(),action("Export saves","remasked-save-export"));
 pages[8].Rows.insert(pages[8].Rows.begin(),action("Import saves","remasked-save-import"));
 pages[0].Rows.insert(pages[0].Rows.begin(),Heading("Appearance"));
 auto menuSize=Number("menuScale","Menu size",50,125,1,1,100,false,true);menuSize.Format=[](int v){char text[24];std::snprintf(text,sizeof(text),"%.2fx",v/100.f);return std::string(text);};menuSize.Help="Scales the settings menu. The layout adapts to the current window size.";pages[0].Rows.insert(pages[0].Rows.begin()+1,std::move(menuSize));
 Row resetMenu;resetMenu.Key="Reset menu size";resetMenu.Kind=RowKind::Action;resetMenu.Activate=[](){auto& s=Touch();std::lock_guard lock(s.Mutex);s.Values["menuScale"]=100;s.Pending["menuScale"]=100;return WriteResult{};};pages[0].Rows.insert(pages[0].Rows.begin()+2,std::move(resetMenu));
 auto icon=std::find_if(pages[0].Rows.begin(),pages[0].Rows.end(),[](const Row& row){return row.Key=="App icon";});
 if(icon!=pages[0].Rows.end())std::rotate(pages[0].Rows.begin()+3,icon,icon+1);
 pages[6].Rows.insert(pages[6].Rows.begin(),Heading("Movement"));
 for(const auto key:{"Swimming speed","Climbing speed","Walking speed"}){auto& rows=pages[6].Rows;auto it=std::find_if(rows.begin(),rows.end(),[&](const Row& row){return row.Key==key;});if(it!=rows.end())std::rotate(rows.begin()+1,it,it+1);}
 pages[6].Rows.push_back(Heading("Majora’s Mask"));
 for(auto [key,label]:std::vector<std::pair<std::string,std::string>>{{"disable_tatl_reminders","Disable Tatl reminders"},{"disable_bomber_approach","Disable Bomber interruptions"},{"disable_low_health_beep","Disable low-health beep"}})pages[6].Rows.push_back(Toggle(key,label));
 pages[6].Rows.push_back(Heading("Difficulty"));
 pages[6].Rows.push_back(Number("damage_multiplier","Damage multiplier",1,16));
 pages[6].Rows.back().Read=[]{return Current().value("damage_multiplier",1);};
 pages[6].Rows.push_back(Toggle("weaker_enhanced_defense","Weaker enhanced defense"));
 for(auto [key,label]:std::vector<std::pair<std::string,std::string>>{{"disable_heart_drops","Disable heart drops"},{"disable_rupee_drops","Disable rupee drops"},{"disable_fairy_drops","Disable fairy drops"},{"disable_magic_drops","Disable magic drops"},{"disable_ammo_pickups","Disable ammo pickups"},{"lose_rupees_on_death","Lose rupees on death"},{"world_ends_on_death","End the world on death"}})pages[6].Rows.push_back(Toggle(key,label));
 auto shops=Toggle("double_prices","Double shop prices",false,true);shops.Help="Doubles supported shop charges and their text at the next game launch. Existing purchases keep their current price until restarting.";pages[6].Rows.push_back(std::move(shops));
 pages[6].Rows.push_back(Heading("Bottle healing"));
 for(auto [key,label,fallback]:std::vector<std::tuple<std::string,std::string,int>>{{"red_potion_hearts","Red potion hearts",20},{"blue_potion_hearts","Blue potion hearts",20},{"bottled_fairy_hearts","Bottled fairy hearts",10}})pages[6].Rows.push_back(Number(key,label,1,20,1,1,fallback));
 pages[4].Rows.push_back(Toggle("mute_music","Mute music"));
 
 Row look=Toggle("dragLook","Camera touch mode",false,false,true);look.Kind=RowKind::Choice;look.Choices={"Stick","Drag to look"};look.Help="Stick turns the camera relative to where you press. Drag to look turns it as you swipe; a stationary finger stops the camera.";pages[2].Rows.push_back(std::move(look));
 pages[2].Rows.push_back(Toggle("shoulderLabels","Shoulder labels",false,false,true));
 auto opacity=Number("itemIconOpacity","Item icon opacity",0,100,1,1,100,false,true);opacity.Format=[](int v){return std::to_string(v)+"%";};pages[2].Rows.push_back(std::move(opacity));
 auto& touchRows=pages[2].Rows;
 auto perButton=std::find_if(touchRows.begin(),touchRows.end(),[](const Row& row){return row.Key=="Show Action";});
 if(perButton!=touchRows.end()){
  const auto position=perButton-touchRows.begin();
  std::rotate(touchRows.begin()+position,touchRows.end()-3,touchRows.end());
  touchRows.insert(touchRows.begin()+position+3,Heading("Individual controls"));
 }
 std::swap(pages[8],pages[9]);
 const auto textOnly=[](std::string text){for(size_t i=0;i+2<text.size();){const auto a=static_cast<unsigned char>(text[i]),b=static_cast<unsigned char>(text[i+1]),c=static_cast<unsigned char>(text[i+2]);if((a==0xee||a==0xef)&&(b&0xc0)==0x80&&(c&0xc0)==0x80){const unsigned cp=((a&15)<<12)|((b&63)<<6)|(c&63);if(cp>=0xe000&&cp<=0xf8ff){text.erase(i,3);continue;}}++i;}return text;};
 for(auto& page:pages)for(auto& row:page.Rows){row.Key=textOnly(row.Key);row.Text=textOnly(row.Text);row.Help=textOnly(row.Help);for(auto& choice:row.Choices)choice=textOnly(choice);}
 document.Tabs.assign(std::make_move_iterator(pages.begin()),std::make_move_iterator(pages.end()));document.CategorySidebar=true;document.InlineSettings=true;
}
inline void Append(Oot3dNativeGame::Menu::Document& document,std::function<void(std::string)> route){
 Tab gameplay;gameplay.Title="ReMasked";
 Row icon=Number("appIconSelection","App icon",0,4,1,1,0,false,true);
 icon.Kind=RowKind::Choice;icon.Choices={"Combined","Icon 1","Icon 2","Icon 3","Icon 4"};
 for(int i=0;i<5;++i)icon.ChoiceImages.push_back("menu/image/remasked-icon/"+std::to_string(i));
 icon.Help="Changes the Android launcher and recent-app icon. The launcher refreshes after leaving the app; gameplay stays running.";
 gameplay.Rows.push_back(std::move(icon));
 gameplay.Rows.push_back(Heading("Camera and movement"));
 for(auto [key,label]:std::vector<std::pair<std::string,std::string>>{{"camera_distance","Camera distance"},{"walk_speed","Walking speed"},{"climb_speed","Climbing speed"},{"swim_speed","Swimming speed"}})gameplay.Rows.push_back(Number(key,label,50,300,1,100,1));
 document.Tabs.push_back(std::move(gameplay));
 Tab hud;hud.Title="HUD";hud.Rows.push_back(Heading("Overall HUD"));
 hud.Rows.push_back(SizeSteps("player_hud_scale","HUD size"));hud.Rows.push_back(Number("player_hud_opacity","Opacity",0,100,5,100,1));
 for(auto side:{"left","right","top","bottom"})hud.Rows.push_back(Number("player_hud_"+std::string(side)+"_padding",std::string(side)+" margin",(side==std::string("left")||side==std::string("right"))?-100:-40,(side==std::string("left")||side==std::string("right"))?100:40));
 hud.Rows.push_back(Toggle("player_hud_hide_minimap","Hide minimap"));hud.Rows.push_back(Toggle("player_hud_hide_menu_prompts","Hide menu prompts"));hud.Rows.push_back(Toggle("player_hud_mirror_item_buttons","Mirror item backgrounds"));
 hud.Rows.push_back(Number("player_hud_heart_spacing","Heart spacing",-40,40));hud.Rows.push_back(Number("player_hud_magic_width","Magic meter width",50,300,5,100,1));hud.Rows.push_back(SizeSteps("player_hud_magic_height","Magic meter height"));
 for(auto [key,label]:std::vector<std::pair<std::string,std::string>>{{"dpad","D-pad"},{"magic","Magic meter"},{"hearts","Hearts"},{"buttons","Buttons group"},{"minimap","Minimap"},{"clock","Clock"},{"rupees","Rupees"},{"minigame","Minigame counter"},{"carrots","Epona carrots"},{"timer","Timer"}}){
  hud.Rows.push_back(Heading(label));std::string p="player_hud_"+key+"_";hud.Rows.push_back(SizeSteps(p+"scale","Size"));hud.Rows.push_back(Number(p+"x","Horizontal offset",-400,400));hud.Rows.push_back(Number(p+"y","Vertical offset",-240,240));
 }
 hud.Rows.push_back(Heading("Action button layout"));hud.Rows.push_back(Number("hud_buttons_scale","Button size",10,300,5,100,1));hud.Rows.push_back(Number("hud_buttons_flip_peak","Flip animation",100,120,1,100,1.2));hud.Rows.push_back(Toggle("hud_diamond_enabled","Diamond layout"));
 hud.Rows.push_back(Number("hud_diamond_spacing","Diamond spacing",0,1600,1,10,24));hud.Rows.push_back(Number("hud_diamond_rotation","Diamond rotation",-1800,1800,1,10));hud.Rows.push_back(Number("hud_diamond_x","Diamond horizontal position",-5120,5120,1,10,350));hud.Rows.push_back(Number("hud_diamond_y","Diamond vertical position",-2400,2400,1,10,55));
 std::array<const char*,6> names={"Action","Item X","Item Y","Sword","Item I","Item II"};
 for(int i=0;i<6;++i){hud.Rows.push_back(Heading(names[i]));std::string p="hud_button_"+std::to_string(i)+"_";hud.Rows.push_back(Number(p+"x","Horizontal offset",-5120,5120,1,10));hud.Rows.push_back(Number(p+"y","Vertical offset",-2400,2400,1,10,i>=4?48:0));hud.Rows.push_back(Number(p+"scale","Size",10,300,5,100,1));}
 hud.Rows.push_back(Heading("Layout presets"));
 for(int slot=1;slot<=3;++slot)for(int action=0;action<3;++action){Row r;r.Key=std::string(action==0?"Load":action==1?"Save":"Clear")+" preset "+std::to_string(slot);r.Kind=RowKind::Action;r.Activate=[slot,action]()->WriteResult{try{return Save(PresetPatch(Current(),slot,action));}catch(...){return {"This preset is empty",true};}};hud.Rows.push_back(std::move(r));}
 document.Tabs.push_back(std::move(hud));
 Tab player;player.Title="Models";Row model;model.Key="Player model";model.Kind=RowKind::Choice;model.Choices={"Original Link","Adult Link","Dark Link","Twilight Princess Link","Imported PAK"};model.Help="Changes apply at the next game launch. Imported files are retained.";
 model.Read=[]{auto j=Current(true);for(int i=4;i>0;--i){const char* key=i==4?"pak_link_model":i==3?"twilight_link_model":i==2?"dark_link_model":"adult_link_model";if(j.value(key,false))return i;}return 0;};
 const auto available=[](int i){if(i==0)return true;const char* folder=i==4?"pak-import":i==3?"twilight-link":i==2?"dark-link":"adult-link";return ModelAvailable(folder);};
 model.ChoiceAvailable=available;model.Write=[available](int i)->WriteResult{if(i<0||i>4||!available(i))return {"Import this model first",true};return Save(Json{{"adult_link_model",i==1},{"dark_link_model",i==2},{"twilight_link_model",i==3},{"pak_link_model",i==4}},true);};player.Rows.push_back(std::move(model));
 auto adult=[](){auto j=Current(true);return j.value("adult_link_model",false)&&!j.value("pak_link_model",false)&&!j.value("twilight_link_model",false)&&!j.value("dark_link_model",false);};
 auto twilight=[](){auto j=Current(true);return j.value("twilight_link_model",false)&&!j.value("pak_link_model",false);};
 auto adultSword=Toggle("adult_link_master_sword","Adult Link: Master Sword",false,true);adultSword.Available=adult;adultSword.UnavailableReason="Select Adult Link first.";player.Rows.push_back(std::move(adultSword));
 auto twilightSword=Toggle("twilight_link_master_sword","Twilight Link: Master Sword",true,true);twilightSword.Available=twilight;twilightSword.UnavailableReason="Select Twilight Princess Link first.";player.Rows.push_back(std::move(twilightSword));
 Row tunic=Number("adult_link_tunic_color","Adult Link tunic",0,6,1,1,0,true);tunic.Kind=RowKind::Choice;tunic.Choices={"Green","Blue","Red","Purple","Black","White","Gold"};tunic.Available=adult;tunic.UnavailableReason="Select Adult Link first.";player.Rows.push_back(std::move(tunic));
 for(auto [page,label]:std::vector<std::pair<std::string,std::string>>{{"remasked-pak","PAK Model Loader"}}){Row r;r.Key=label;r.Kind=RowKind::Action;r.Help="Save your game first. This closes the game before opening the file picker or driver selection.";r.Activate=[route,page](){route(page);return WriteResult{};};if(page=="remasked-driver"){for(auto& tab:document.Tabs)if(tab.Title=="Graphics")tab.Rows.push_back(r);}else player.Rows.push_back(std::move(r));}
 document.Tabs.push_back(std::move(player));
 auto photo=std::find_if(document.Tabs.begin(),document.Tabs.end(),[](const Tab& tab){return tab.Title=="Photo mode";});
 if(photo!=document.Tabs.end())std::rotate(photo,document.Tabs.end()-3,document.Tabs.end());
 for(auto& tab:document.Tabs)if(tab.Title=="Controls"){
  const auto insertion=std::find_if(tab.Rows.begin(),tab.Rows.end(),[](const Row& r){return r.Key=="ReMasked controller layout"||r.Key=="Aiming";});
  const size_t start=tab.Rows.size(),position=size_t(insertion-tab.Rows.begin());
  tab.Rows.push_back(Heading("ReMasked touch controls"));
  Row movement=Toggle("floating","Movement stick position",true,false,true);movement.Kind=RowKind::Choice;movement.Choices={"Fixed","Floating"};movement.Help="Fixed keeps the movement stick anchored. Floating places it where you press. The camera stick follows your touch and is hidden at rest.";tab.Rows.push_back(std::move(movement));tab.Rows.push_back(Toggle("itemIcons","Equipped item artwork",true,false,true));
  Row face=Number("faceLayout","Face layout",0,2,1,1,0,false,true);face.Kind=RowKind::Choice;face.Choices={"Nintendo diamond","Alternate diamond","Compact"};tab.Rows.push_back(std::move(face));
  std::array<const char*,13> labels={"Action","Sword","Item X","Item Y","Item I","Item II","Target","Shield","Start / Items menu","Select / App settings","D-pad","Movement stick","Camera stick"};
  std::array<int,13> ids={700,701,702,703,773,774,707,708,704,705,780,713,718};
  for(int i=0;i<13;++i){tab.Rows.push_back(Toggle("buttonToggle"+std::to_string(i),std::string("Show ")+labels[i],true,false,true));auto size=Number("controlScale-"+std::to_string(ids[i]),std::string(labels[i])+" size",0,150,5,1,50,false,true);size.Format=[](int v){return std::to_string(v+50)+"%";};tab.Rows.push_back(std::move(size));}
  // Keep original ReMasked touch settings together with the overlay's common
  // controls instead of burying them below the controller/aim sections.
  std::rotate(tab.Rows.begin()+position,tab.Rows.begin()+start,tab.Rows.end());
 }
 Organize(document,route);
}
}
