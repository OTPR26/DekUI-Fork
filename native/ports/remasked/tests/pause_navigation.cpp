// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../titles/mm3d/dekui/menu_page_switch.h"
#include "../../../titles/mm3d/dekui/items_map_prompt.h"
#include "../../../titles/mm3d/dekui/native_notebook.h"
#include "fast/extension/remasked_dual_screen.h"
#include <map>
#include <iostream>
#include <stdexcept>
using namespace Mm3dDekui;
using U32=std::uint32_t;
void Check(bool value,const char* name){if(!value)throw std::runtime_error(name);}
struct MenuMemoryFixture {
    std::map<U32,U32> words;
    std::optional<U32> Read(U32 at){auto i=words.find(at);return i==words.end()?std::nullopt:std::optional(i->second);}
    void Write(U32 at,U32 value){words[at]=value;}
    void Layout(U32 at,const char* name){Write(at+4,at+0x200);Write(at+0x218,at+0x300);
        std::string s=name;for(unsigned i=0;i<=s.size();i+=4){U32 v=0;for(unsigned j=0;j<4&&i+j<s.size();++j)v|=U32(s[i+j])<<(8*j);Write(at+0x300+i,v);}}
};
int main(){try {
    Check(ReMaskedDual::NativePageInput(true,0),"released touch remains native input");
    Check(!ReMaskedDual::NativePageInput(false,0),"single-screen routing unchanged");
    for(unsigned shoulder:{0x100u,0x200u,0x4000u,0x8000u})Check(!ReMaskedDual::NativePageInput(true,shoulder),"all four physical shoulders retain controller routing");
    PauseNavigation::SettingsPress key;
    Check(key.Update(true),"Select rising edge");Check(!key.Update(true),"held Select does not repeat");
    Check(!key.Update(false),"release does not toggle");Check(key.Update(true),"second press toggles");
    key.Update(false);Check(key.Update(false,true),"short touch pulse");Check(!key.Update(false),"touch pulse consumed");
    Check(!PauseNavigation::NotebookUnlocked(0),"locked notebook");
    Check(PauseNavigation::NotebookUnlocked(1u<<18),"owned notebook");
    MenuMemoryFixture base;constexpr U32 ctx=0x8000000,ui=0x8010000,game=0x8020000;
    constexpr U32 items=0x8030000,masks=0x8040000,gear=0x8050000,map=0x8060000,book=0x8070000;
    base.Write(ctx+4,ui);base.Write(ctx+8,map);base.Write(ctx+12,0);base.Write(ctx+16,0x101);
    base.Write(ui+0x16c,game);base.Write(game+0x13c,0x400);base.Write(game+0x34,0x100);
    base.Write(0x656b10,items);base.Write(0x656b18,masks);base.Write(0x656b0c,gear);base.Write(0x656b14,map);base.Write(0x656b08,book);
    base.Write(items,MenuPageSwitch::ItemsVtable);base.Write(masks,MenuPageSwitch::MasksVtable);base.Write(gear,MenuPageSwitch::GearVtable);
    base.Write(map,MenuPageSwitch::MapVtable);base.Write(book,MenuPageSwitch::NotebookVtable);
    base.Write(map+0x78c,0);base.Write(map+0x18,0x8100000);base.Layout(0x8100000,"MenuMapDungeon");
    base.Write(book+0xc,0);base.Write(book+0x40,0x8110000);base.Layout(0x8110000,"MenuQuestlist");
    base.Write(0x7751d8+0x1f8,1u<<18);
    base.Write(0x5b4b40,MenuPageSwitch::NotebookVtable);base.Write(MenuPageSwitch::NotebookVtable+0x1c,0x5b45b4);
    base.Write(MenuPageSwitch::NotebookVtable+0x20,0x5b48cc);base.Write(MenuPageSwitch::NotebookVtable+0x24,0x5b4168);
    PauseNavigation::enabled=true;
    for(auto page:{items,masks})for(U32 edge:{0x100u,0x200u}) {
        auto m=base;m.Write(ctx+8,page);m.Write(game+0x30,0);
        m.Write(game+0x34,edge);
        m.Write(page+0x9c,2u<<24);m.Write(page+0xc0,0x8120000);
        m.Layout(0x8120000,page==items?"MenuItem":"MenuMask");
        auto physical=m;MenuPageSwitch::Hook h;
        h.Tick(m,ctx,false,true);
        Check(m.Read(ctx+12)==(page==items?masks:items),"native lower tab uses inventory pair, including release-generated shoulder");
        MenuPageSwitch::Hook physicalHook;physicalHook.Tick(physical,ctx,false,false);
        const bool outer=(page==items&&edge==0x100)||(page==masks&&edge==0x200);
        Check(physical.Read(ctx+12)==(outer?(page==items?map:gear):0),"physical bumper retains outer routing and native inner edge");
    }
    Check(ItemsMapPrompt::NotebookHintAvailable(base,map),"unlocked Notebook hint matches navigation availability");
    {auto m=base;m.Write(0x7751d8+0x1f8,0);Check(!ItemsMapPrompt::NotebookHintAvailable(m,map),"locked Notebook has no navigation hint");}
    {auto m=base;m.Write(map+0x78c,1u<<16);Check(!ItemsMapPrompt::NotebookHintAvailable(m,map),"modal Map has no navigation hint");}
    {auto m=base;m.Write(MenuPageSwitch::NotebookVtable+0x20,0);Check(!ItemsMapPrompt::NotebookHintAvailable(m,map),"foreign notebook code fails closed");}
    PauseNavigation::enabled=false;Check(!ItemsMapPrompt::NotebookHintAvailable(base,map),"other platforms retain existing Map hints");PauseNavigation::enabled=true;
    Check(ItemsMapPrompt::MapReturnHintAvailable(base,book,0x8110000),"Event Notes shows Map return hint");
    {auto m=base;m.Write(book+0xc,1);Check(!ItemsMapPrompt::MapReturnHintAvailable(m,book,0x8110000),"Notebook detail/schedule state hides Map hint");}
    {auto m=base;m.Layout(0x8110000,"MenuSchedule");Check(!ItemsMapPrompt::MapReturnHintAvailable(m,book,0x8110000),"Schedule layout hides Map hint");}
    {auto m=base;m.Write(map,0);Check(!ItemsMapPrompt::MapReturnHintAvailable(m,book,0x8110000),"Invalid Map target hides return hint");}
    PauseNavigation::enabled=false;Check(!ItemsMapPrompt::MapReturnHintAvailable(base,book,0x8110000),"Other platforms retain notebook presentation");PauseNavigation::enabled=true;
    {auto m=base;
     m.Write(book+0x6c,0x8200000);m.Write(book+0x70,0x00ff00ff);m.Write(book+0x254,0x1fe);m.Write(0x8200010,0xcccccccc);
     Check(!NotebookPrompt::EventDataReady(m,book),"empty Notebook rejects uninitialized event table");
     Check(NativeNotebook::MapInputFlags(m,book,0x400,0)&0x40000000,"empty Notebook blocks X map callback");
     Check(NativeNotebook::ScheduleInputFlags(m,book,0x800,0)&0x40000000,"empty Notebook blocks Y schedule callback");
     m.Write(book+0x70,1u<<24);m.Write(book+0x254,0);m.Write(0x8200000,1);
     Check(NotebookPrompt::EventDataReady(m,book),"initialized Notebook event remains available");
     Check(!(NativeNotebook::MapInputFlags(m,book,0x400,0)&0x40000000),"valid Notebook X map remains available");
     Check(!(NativeNotebook::ScheduleInputFlags(m,book,0x800,0)&0x40000000),"valid Notebook Y schedule remains available");
     m.Write(0x8200000,7);Check(!NotebookPrompt::EventDataReady(m,book),"event cannot index past six icon layouts into Widget array");
     m.Write(0x8200000,1);m.Write(book+0x254,1u<<8);Check(!NotebookPrompt::EventDataReady(m,book),"out-of-range event row rejected");
     m.Write(book+0x254,1u<<16);Check(!NotebookPrompt::EventDataReady(m,book),"out-of-range subevent rejected");
    }
    for(auto page:{items,masks,gear,map,book}) {
        auto m=base;m.Write(ctx+8,page);m.Write(game+0x34,8|0x100);m.Write(game+0x30,8);
        m.Write(items+0x9c,2u<<24);m.Write(masks+0x9c,3u<<24);m.Write(gear+0x194,2);
        MenuPageSwitch::Hook h;
        const auto opening=m.words;h.Tick(m,ctx);Check(m.words==opening,"opening Start press cannot immediately close menu");
        m.Write(game+0x34,0);h.Tick(m,ctx);Check(m.Read(game+0x34)==0,"held Start cannot arm Back");
        m.Write(game+0x30,0);h.Tick(m,ctx); // release arms the next press
        m.Write(game+0x34,8|0x100);m.Write(game+0x30,8);
        const auto original=m.words;h.Tick(m,ctx);
        Check(m.Read(game+0x34)==(2|0x100),"Start becomes native Back on pause pages");
        Check(m.Read(ctx+12)==0,"native page owns exit animation, no forced screen switch");
        Check(m.Read(game+0x30)==8,"held input unchanged");
        auto expected=original;expected[game+0x34]=2|0x100;
        Check(m.words==expected,"only Start pressed edge replaced; other input and page state preserved");
        m.Write(game+0x34,0);Check(!MenuPageSwitch::StartBack(m,ctx,h.start_back_armed),"holding Start cannot repeat");
    }
    auto back=base;back.Write(ctx+8,items);back.Write(items+0x9c,2u<<24);back.Write(game+0x34,8);
    for(int scenario=0;scenario<7;++scenario){
        auto m=back;
        if(scenario==0)m.Write(ctx+12,map);
        if(scenario==1)m.Write(ctx+16,0x100);
        if(scenario==2)m.Write(game+0x13c,0x300);
        if(scenario==3)m.Write(items+0x9c,1u<<24);
        if(scenario==4)m.Write(items,0xdeadbeef);
        if(scenario==5){m.Write(ctx+8,book);m.Write(book+0xc,1);}
        if(scenario==6)PauseNavigation::enabled=false;
        bool armed=true;const auto before=m.words;Check(!MenuPageSwitch::StartBack(m,ctx,armed),"Start not rerouted in transitions, title, modal or other platform");
        Check(m.words==before,"rejected Start routing leaves memory untouched");PauseNavigation::enabled=true;
    }
    {auto m=base;MenuPageSwitch::Hook h;h.Tick(m,ctx);Check(m.Read(ctx+12)==book,"Map next opens Notebook");Check(m.Read(game+0x34)==0,"only shoulder press consumed");Check(!h.context,"Notebook uses normal resource readiness");}
    {auto m=base;MenuPageSwitch::Hook h;m.Write(0x7751d8+0x1f8,0);h.Tick(m,ctx);Check(m.Read(ctx+12)==0,"locked Notebook not opened");}
    {auto m=base;MenuPageSwitch::Hook h;m.Write(ctx+12,items);h.Tick(m,ctx);Check(m.Read(ctx+12)==items,"in-flight page switch retained");}
    {auto m=base;MenuPageSwitch::Hook h;m.Write(MenuPageSwitch::NotebookVtable+0x20,0);h.Tick(m,ctx);Check(m.Read(ctx+12)==0,"foreign notebook code rejected");}
    {auto m=base;MenuPageSwitch::Hook h;m.Write(ctx+8,book);m.Write(game+0x34,0x200);h.Tick(m,ctx);Check(m.Read(ctx+12)==map,"Notebook previous returns Map");Check(!h.context,"return uses ordinary loader");}
    {auto m=base;MenuPageSwitch::Hook h;m.Write(ctx+8,book);m.Write(book+0xc,1);m.Write(game+0x34,0x200);h.Tick(m,ctx);Check(m.Read(ctx+12)==0,"Notebook detail navigation preserved");}
    {auto m=base;MenuPageSwitch::Hook h;PauseNavigation::enabled=false;h.Tick(m,ctx);Check(m.Read(ctx+12)==0,"other platforms retain layout");}
    std::cout<<"Pause settings edge, unlock, resource readiness, page return and platform isolation passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
