// SPDX-License-Identifier: GPL-3.0-or-later
#include "remasked_core.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
void check(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
std::string save(RMCore* c) {
    std::vector<char> b(rm_copy_config(c,nullptr,0));check(!b.empty(),"size");
    check(rm_copy_config(c,b.data(),b.size())==b.size(),"copy");return b.data();
}
int main(int argc,char** argv) {
 try {
    check(argc==2,"fixture");std::ifstream f(argv[1]);std::string text{std::istreambuf_iterator<char>(f),{}};
    auto before=nlohmann::json::parse(text);check(before.size()==586,"pinned baseline has 586 settings");
    before["future_model_settings"]={{"twilight",true},{"voice","adult"},{"label","Link — 日本語 🎮"}};
    text=before.dump();auto* c=rm_create(text.data(),text.size());check(c,"parse");
    check(nlohmann::json::parse(save(c))==before,"every original value and unknown feature must survive");
    const auto original=save(c);char sentinel='!';check(rm_copy_config(c,&sentinel,1)>1 && sentinel=='!',"short buffers unchanged");
    check(!rm_set_hud_number(c,"player_hud_scale",11),"reject out of range");
    check(!rm_set_hud_number(c,"player_hud_scale",1.5),"reject fractional steps");
    check(!rm_set_hud_number(c,"player_hud_opacity",std::numeric_limits<double>::quiet_NaN()),"reject NaN");
    check(!rm_set_hud_number(c,"adult_link_model",0),"HUD editor cannot alter models");
    check(save(c)==original,"rejections are atomic");
    check(rm_set_hud_number(c,"player_hud_scale",2),"scale");
    auto after=nlohmann::json::parse(save(c));before["player_hud_scale"]=2;check(after==before,"edit only requested key");
    RMHudElement element{};check(rm_hud_element(c,0,&element)&&element.width>0,"native HUD projection");
    check(!rm_hud_element(c,16,&element),"bounded elements");
    RMHudElement bounds{};check(rm_hud_bounds(c,&bounds),"HUD preview bounds");
    for(unsigned i=0;i<16;++i){check(rm_hud_element(c,i,&element),"element exists");
        check(element.x>=bounds.x&&element.y>=bounds.y&&element.x+element.width<=bounds.x+bounds.width&&element.y+element.height<=bounds.y+bounds.height,"all saved HUD poses fit preview");}
    check(rm_input_button(c,RM_SELECT,1)&&rm_input_mask(c)==4,"SELECT routes independently");
    check(rm_input_button(c,RM_START,1)&&rm_input_mask(c)==12,"simultaneous START/SELECT");
    check(rm_input_button(c,RM_SELECT,0)&&rm_input_mask(c)==8,"independent release");
    check(rm_input_source_button(c,1,RM_START,1),"controller hold");
    check(rm_input_button(c,RM_START,0)&&rm_input_mask(c)==8,"touch release preserves controller hold");
    check(!rm_input_source_button(c,8,RM_A,1),"source bound");
    check(!rm_input_button(c,31,1)&&rm_input_mask(c)==8,"unknown input rejected");
    check(rm_input_button(c,RM_SELECT,1),"touch hold");rm_release_source(c,1);
    check(rm_input_mask(c)==4,"controller disconnect preserves touch hold");
    check(rm_input_sticks(c,-1,1,0,0),"valid sticks");check(!rm_input_sticks(c,2,0,0,0),"invalid sticks");
    rm_release_inputs(c);check(rm_input_mask(c)==0,"lifecycle releases input");
    auto serialized=save(c);auto* restored=rm_create(serialized.data(),serialized.size());
    check(restored&&save(restored)==serialized,"restore identical");rm_destroy(restored);rm_destroy(c);
    check(!rm_create("[]",2)&&!rm_create("{",1),"invalid documents rejected");
    std::cout<<"PASS: 586 settings, unknown model fields, atomic HUD edits, input validation and lifecycle\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
