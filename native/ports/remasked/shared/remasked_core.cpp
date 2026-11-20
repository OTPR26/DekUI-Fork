// SPDX-License-Identifier: GPL-3.0-or-later
#include "remasked_core.h"
#include "settings.h"
#include "triaevum/input_service.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <string>
static_assert(NLOHMANN_JSON_VERSION_MAJOR==3 && NLOHMANN_JSON_VERSION_MINOR==12 && NLOHMANN_JSON_VERSION_PATCH==0,
              "The prototype pins nlohmann/json 3.12.0");

struct RMCore {
    std::mutex mutex;
    nlohmann::json document;
    Mm3dDekui::Settings settings;
    triaevum::module::InputStateV1 input;
    uint32_t input_sources[8]{};
};
namespace {
constexpr size_t Limit=1024*1024;
bool Range(const char* key, double& lo, double& hi, bool& integral) {
    if (!key) return false;
    const std::string name(key); integral=false;
    if(name=="player_hud_opacity") {lo=0;hi=1;return true;}
    if(name=="hud_buttons_scale") {lo=.1;hi=4;return true;}
    if(name=="player_hud_scale") {lo=-10;hi=10;integral=true;return true;}
    return false;
}
}
extern "C" RMCore* rm_create(const char* data,size_t length) {
    if(!data || !length || length>Limit) return nullptr;
    try {
        auto document=nlohmann::json::parse(data,data+length);
        if(!document.is_object() || document.value("version",0)!=1) return nullptr;
        auto core=std::make_unique<RMCore>();
        if(!core->settings.LoadFromString(std::string_view(data,length))) return nullptr;
        // The original document owns persistence. The native serializer migrates
        // legacy audio and drops unknown fields, so it must never overwrite it.
        core->document=std::move(document);
        return core.release();
    } catch(...) {return nullptr;}
}
extern "C" void rm_destroy(RMCore* core) {delete core;}
extern "C" size_t rm_copy_config(RMCore* core,char* output,size_t capacity) {
    if(!core) return 0;
    try {
        std::lock_guard lock(core->mutex);const auto text=core->document.dump(2)+"\n";
        if(output && capacity>=text.size()+1) std::memcpy(output,text.c_str(),text.size()+1);
        return text.size()+1;
    } catch(...) {return 0;}
}
extern "C" int rm_set_hud_number(RMCore* core,const char* key,double value) {
    if(!core || !std::isfinite(value)) return 0;
    try {
        double lo,hi;bool integral;
        if(!Range(key,lo,hi,integral) || value<lo || value>hi || (integral && std::trunc(value)!=value)) return 0;
        std::lock_guard lock(core->mutex);auto candidate=core->document;
        if(integral) candidate[key]=int(value);else candidate[key]=value;
        Mm3dDekui::Settings parsed;
        if(!parsed.LoadFromString(candidate.dump())) return 0;
        // Reload rather than assigning the native Settings object (it owns a save guard).
        if(!core->settings.LoadFromString(candidate.dump())) return 0;
        core->document=std::move(candidate);return 1;
    } catch(...) {return 0;}
}
extern "C" double rm_hud_number(RMCore* core,const char* key) {
    if(!core) return 0;
    try {
        double lo,hi;bool integral;if(!Range(key,lo,hi,integral)) return 0;
        std::lock_guard lock(core->mutex);
        const auto it=core->document.find(key);
        return it!=core->document.end() && it->is_number()?it->get<double>():0;
    } catch(...) {return 0;}
}
extern "C" int rm_hud_element(RMCore* core,unsigned index,RMHudElement* output) {
    if(!core || !output || index>=16) return 0;
    std::lock_guard lock(core->mutex);
    // A layout inspection surface, not replacement game graphics. Actual game
    // anchors/visibility must come from the native title composition interface.
    constexpr float anchors[10][4]={{24,148,42,42},{20,38,88,9},{20,18,100,14},{300,165,76,46},
        {332,68,54,70},{165,216,70,16},{20,218,55,14},{150,190,90,14},{120,175,80,14},{190,16,48,14}};
    const auto& hud=core->settings.player_hud;
    if(index<10) {
        const float scale=Mm3dDekui::HudAdjustments::Factor(hud.overall)*Mm3dDekui::HudAdjustments::Factor(hud.elements[index]);
        *output={anchors[index][0]+hud.x[index],anchors[index][1]+hud.y[index],anchors[index][2]*scale,anchors[index][3]*scale,hud.opacity,1};
        if(index==4 && hud.hide_minimap) output->visible=0;
    } else {
        const unsigned n=index-10;const auto& buttons=core->settings.hud_buttons;
        const auto& pose=buttons.buttons[n];
        float x=300+float(n%3)*28+pose.x,y=158+float(n/3)*28+pose.y;
        if(buttons.diamond.enabled) {
            const float angle=buttons.diamond.rotation*3.14159265f/180.f;
            constexpr float offsets[6][2]={{0,-1},{-1,0},{1,0},{0,1},{-1,1},{1,1}};
            const float dx=offsets[n][0]*buttons.diamond.spacing,dy=offsets[n][1]*buttons.diamond.spacing;
            x=buttons.diamond.x+dx*std::cos(angle)-dy*std::sin(angle)+pose.x;
            y=buttons.diamond.y+dx*std::sin(angle)+dy*std::cos(angle)+pose.y;
        }
        const float size=22*buttons.scale*pose.scale;
        *output={x,y,size,size,hud.opacity,1};
    }
    return 1;
}
extern "C" int rm_hud_bounds(RMCore* core,RMHudElement* output) {
    if(!core||!output)return 0;
    float left=0,top=0,right=400,bottom=240;
    for(unsigned i=0;i<16;++i) {
        RMHudElement e{};if(!rm_hud_element(core,i,&e)||!e.visible)continue;
        left=std::min(left,e.x);top=std::min(top,e.y);
        right=std::max(right,e.x+e.width);bottom=std::max(bottom,e.y+e.height);
    }
    *output={left-8,top-8,right-left+16,bottom-top+16,1,1};return 1;
}
extern "C" int rm_input_button(RMCore* core,unsigned button,int down) {
    return rm_input_source_button(core,0,button,down);
}
extern "C" int rm_input_source_button(RMCore* core,unsigned source,unsigned button,int down) {
    if(!core || source>=8 || button>=32 || !(TRIAEVUM_INPUT_BUTTON_MASK_V1&(uint32_t(1)<<button))) return 0;
    std::lock_guard lock(core->mutex);auto candidate=core->input;
    auto mask=core->input_sources[source];
    if(down) mask|=uint32_t(1)<<button;else mask&=~(uint32_t(1)<<button);
    candidate.buttons=mask;
    for(unsigned i=0;i<8;++i) if(i!=source) candidate.buttons|=core->input_sources[i];
    ++candidate.sampleSequence;
    if(!triaevum::module::ValidateInputStateV1(candidate)) return 0;
    core->input_sources[source]=mask;core->input=candidate;return 1;
}
extern "C" int rm_input_sticks(RMCore* core,float lx,float ly,float rx,float ry) {
    if(!core) return 0;std::lock_guard lock(core->mutex);auto next=core->input;
    next.leftStickX=lx;next.leftStickY=ly;next.rightStickX=rx;next.rightStickY=ry;++next.sampleSequence;
    if(!triaevum::module::ValidateInputStateV1(next)) return 0;
    core->input=next;return 1;
}
extern "C" uint32_t rm_input_mask(RMCore* core) {
    if(!core) return 0;std::lock_guard lock(core->mutex);return core->input.buttons;
}
extern "C" void rm_release_inputs(RMCore* core) {
    if(core) {std::lock_guard lock(core->mutex);core->input={};std::fill(std::begin(core->input_sources),std::end(core->input_sources),0);}
}
extern "C" void rm_release_source(RMCore* core,unsigned source) {
    if(!core||source>=8)return;std::lock_guard lock(core->mutex);
    core->input_sources[source]=0;core->input.buttons=0;
    for(auto mask:core->input_sources)core->input.buttons|=mask;
    ++core->input.sampleSequence;
}
extern "C" unsigned rm_settings_count(RMCore* core) {
    if(!core) return 0;std::lock_guard lock(core->mutex);return unsigned(core->document.size());
}
