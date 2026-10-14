// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/classic_save_hooks.h"
#include <fstream>
#include <iostream>
#include <map>
#include <cstdlib>
using namespace MM3D::ClassicSaving;
void Check(bool ok,const char* what){if(!ok){std::cerr<<what<<'\n';std::exit(1);}}
struct Memory {
    std::map<U32,U32> words;
    std::optional<U32> Read(U32 p){auto i=words.find(p);return i==words.end()?std::nullopt:std::optional(i->second);}
    void Write(U32 p,U32 v){Check(words.contains(p),"invalid write");words[p]=v;}
};
struct Service {
    int begin=0,finish=0,load=0;bool allow=true,commit=true;
    bool Begin(unsigned,Kind){++begin;return allow;}
    bool Finish(unsigned,Kind){++finish;return commit;}
    void Load(unsigned,std::span<const std::uint8_t>){++load;}
};
int main(int argc,char** argv) {
    Check(argc==2,"supply supported Restoration binary");
    std::ifstream f(argv[1],std::ios::binary);std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),{});
    Memory code;
    for(auto s:Signatures)for(U32 n=0;n<s[1];n+=4){const U32 off=s[0]+n-0x100000;Check(off+4<=b.size(),"binary size");code.words[s[0]+n]=U32(b[off])|U32(b[off+1])<<8|U32(b[off+2])<<16|U32(b[off+3])<<24;}
    Check(CodeMatches(code),"supported binary guard");
    for(unsigned i=0;i<Sites.size();++i){Check(code.Read(Sites[i][0])==Sites[i][1],"site word");Check(Instruction(Sites[i][0],Sites[i][1])==(i==5?Sites[i][1]:(0xef000000|FirstSVC+i)),"translation");}
    // Once one site has been translated, the full guard must still recognize
    // the supported binary so the remaining save sites can be installed.
    code.words[Sites[0][0]]=Instruction(Sites[0][0],Sites[0][1]);
    Check(CodeMatches(code),"guard accepts an already translated site");
    code.words[Sites[0][0]]=Sites[0][1];
    for(auto& [at,w]:code.words){w^=1;Check(!CodeMatches(code),"guard rejects mutated code");w^=1;}
    constexpr U32 ctx=0x20000000,play=0x21000000,job=0x22000000;
    Memory m;m.words={{ctx+0x13c,0xa02},{ctx+0x180,4},{ctx+0x188,123},{0x7887f0,1},
      {play+0x13c,0x402},{play+0x10c,123},{ctx+0x148,job},{ctx+0x158,0},{job+8,0}};
    std::array<U32,16> r{};Hook hook;Service service;
    m.words[0x577b74]=0x62e;
    for(bool classic:{false,true,false}) {
        enabled=classic;
        for(unsigned i=0;i<r.size();++i)r[i]=0x100000+i;
        auto expected=r;expected[1]=classic?0xc01:0x62e;
        const auto unchanged=m.words;
        hook.Handle(m,r,15,service);
        Check(r==expected&&m.words==unchanged,"activation prompt remap only when classic saving is enabled");
    }
    // Completion skips only the automatic conversation, retaining native
    // activation-bit/cutscene work before the hook and model cleanup after it.
    constexpr U32 owl=0x23000000;
    m.words[owl]=0x1b2;m.words[owl+0x1c]=0;
    for(bool classic:{false,true})for(bool sword:{false,true})for(bool feather:{false,true}) {
        enabled=classic;MM3D::OwlSword::enabled=sword;m.words[owl+0x1c]=feather?0x200:0;
        r={};r[4]=owl;r[5]=1;r[6]=play;r[15]=0x577acc;
        auto expected=r;
        if(sword&&!feather){expected[0]=MM3D::OwlSword::Idle;expected[5]=0;expected[15]=0x577adc;}
        else expected[1]=classic?0xc01:0x62e;
        const auto unchanged=m.words;hook.Handle(m,r,15,service);
        Check(r==expected&&m.words==unchanged,"sword completion calls native player release then resumes cleanup; feather/disabled preserved");
    }
    // Live debugger found the normal conversation handler waiting forever
    // for state 2 with no textbox. Exercise completion then its real next query.
    constexpr U32 player=0x24000000;
    m.words[play+0x20dc]=player;m.words[player+0xe14]=0x22d06c;
    m.words[player+0x11dd0]=owl;m.words[owl+0x268]=MM3D::OwlSword::Idle;
    m.words[player+0x11db0]=0x20010040;m.words[player+0x92c]=7;
    for(bool classic:{false,true})for(bool sword:{false,true})for(bool feather:{false,true})
    for(unsigned variant=0;variant<4;++variant) {
        enabled=classic;MM3D::OwlSword::enabled=sword;m.words[owl+0x1c]=feather?0x200:0;
        m.words[player+0x11dd0]=owl;
        r={};r[4]=owl;r[5]=1;r[6]=play;hook.Handle(m,r,15,service);
        if(variant==1)m.words[player+0x11dd0]=owl+0x1000;
        r={};r[4]=player;r[6]=variant==2?play+0x1000:play;r[0]=variant==3?4:0;
        auto expected=r;expected[7]=player+0x10000;
        if(sword&&!feather&&variant==0)expected[0]=2;
        const auto unchanged=m.words;hook.Handle(m,r,16,service);
        Check(r==expected&&m.words==unchanged,"only omitted owl dialogue follows native finished path without memory writes");
        r[0]=0;hook.Handle(m,r,16,service);
        Check(r[0]==0&&!hook.quiet_player,"subsequent A conversation is not overridden");
    }
    MM3D::OwlSword::enabled=false;
    r={};
    enabled=false;r[0]=ctx;auto before=m.words;hook.Handle(m,r,0,service);
    Check(m.words==before&&r[3]==4,"off leaves native state");
    enabled=true;hook.Handle(m,r,0,service);
    Check(m.words[ctx+0x180]==7&&m.words[ctx+0x188]==0,"skip cycle choice");
    r[0]=ctx+0x148;r[14]=0x16f6b4;hook.Handle(m,r,2,service);
    Check(service.begin==1&&hook.kind==Kind::Cycle,"classify cycle writer");
    r[0]=job;hook.Handle(m,r,3,service);hook.Handle(m,r,4,service);
    Check(service.finish==1&&hook.cycle_committed,"successful cycle commit");
    m.words[ctx+0x180]=9;r[4]=ctx;hook.Handle(m,r,1,service);
    Check(r[3]==9&&m.words[ctx+0x180]==9,"preserve native success message transition");
    constexpr U32 cycle_window=0x29000000;
    m.words[ctx+0x174]=cycle_window;m.words[cycle_window+8+0xd4]=15;
    for(unsigned lifecycle:{0x0a01u,0x0a02u}) for(unsigned status=7;status<=10;++status) {
        m.words[ctx+0x13c]=lifecycle;
        m.words[ctx+0x180]=status;
        auto draw_before=m.words;
        hook.BeginCycleDraw(m,cycle_window);
        Check(m.words[cycle_window+8+0xd4]==(status<10?14:15),"hide only saving window, show success");
        hook.EndCycleDraw(m);Check(m.words==draw_before,"restore draw flags exactly");
    }
    m.words[ctx+0x180]=8;
    for(unsigned lifecycle:{0x0a00u,0x0901u,0x0402u}) {
        m.words[ctx+0x13c]=lifecycle;auto before_other_state=m.words;
        hook.BeginCycleDraw(m,cycle_window);hook.EndCycleDraw(m);
        Check(m.words==before_other_state,"changing and unrelated states never hidden");
    }
    m.words[ctx+0x13c]=0x0a01;
    enabled=false;auto disabled_draw=m.words;
    hook.BeginCycleDraw(m,cycle_window);hook.EndCycleDraw(m);
    Check(m.words==disabled_draw,"disabled mode retains Restoration window");enabled=true;
    hook.BeginCycleDraw(m,play);hook.EndCycleDraw(m);
    Check(m.words==disabled_draw,"other layouts untouched");
    // Native error cleanup also returns done: never commit or quit for it.
    r[0]=play+0x3bec;r[14]=0x4bfc30;m.words[r[0]]=job;m.words[r[0]+0x10]=0;
    hook.Handle(m,r,2,service);m.words[job+8]=0x80000001;
    r[0]=job;hook.Handle(m,r,3,service);hook.Handle(m,r,4,service);
    Check(service.finish==1&&hook.quit==0,"native save error");
    // Native message fixture: final page, copied Yes/No text objects.
    constexpr U32 win=0x23000000,labels=0x24000000,objects=0x25000000,list=0x26000000;
    m.words[0x6b3724]=win;m.words[win+0x3f4]=0xc01;
    m.words[win+0x404]=4;m.words[win+0x408]=5;m.words[win+0x444]=play;
    for(unsigned page=0;page<4;++page) {
        m.words[win+0x404]=page;
        Check(!OwlPresentation::FinalWindow(m),"informational/warning pages are not confirmation");
    }
    m.words[win+0x404]=4;
    Check(OwlPresentation::FinalWindow(m)==win,"fifth page recognized as final confirmation");
    m.words[win+0x430]=0;m.words[win+0x3dc]=9;m.words[win+0x3b8]=0x01010001;
    m.words[win+0x318]=0;m.words[win+0x3c0]=0;
    m.words[0x6b3758]=list;m.words[0x6b375c]=2;
    m.words[list]=objects;m.words[list+4]=objects+0x20;
    m.words[objects]=m.words[objects+0x20]=0x63d4fc;
    m.words[objects+8]=labels;m.words[objects+12]=3;
    m.words[objects+0x28]=labels+4;m.words[objects+0x2c]=2;
    m.words[labels]=0x736559;m.words[labels+4]=0x6f4e;
    for(auto pair: {std::array<U32,3>{0x69754f,0x6e6f4e,3},std::array<U32,3>{0xadc353,0x6f4e,2}}) {
        m.words[labels]=pair[0];m.words[labels+4]=pair[1];m.words[objects+0x2c]=pair[2];
        OwlPresentation localized;Check(localized.Confirm(m,play),"localized final choices accepted");
        localized.Reset(m);
    }
    m.words[labels]=0x736559;m.words[labels+4]=0x6f4e;m.words[objects+0x2c]=2;
    const U32 engine=win+0x368;
    m.words[engine+0x10]=0x3f800000;m.words[engine+0x1c]=1;m.words[win+0x3d8]=10;
    r[0]=engine;r[2]=1;hook.Handle(m,r,11,service);
    Check(OwlPresentation::Byte(m,engine+0x53)==0&&OwlPresentation::Byte(m,engine+0x58)==0,"choices no longer force instant reveal");
    hook.Handle(m,r,10,service);
    Check(m.words[engine+0x10]==std::bit_cast<U32>(.90f),"90 percent reveal");
    // No second update between the last glyph and the native close decision.
    r[4]=engine;hook.Handle(m,r,14,service);
    Check(r[0]==1&&OwlPresentation::Byte(m,engine+0x53)==1,
          "same-update end decision enters choices instead of closing");
    r[0]=play+0x3bec;hook.Handle(m,r,2,service);m.words[job+8]=0;
    Check(hook.presentation.window==win,"capture verified final prompt");
    r[0]=play;r[15]=Sites[12][0]+4;hook.Handle(m,r,12,service);
    Check(r[15]==Sites[12][0]+4&&m.words[labels+4]==0x6f4e,"retain prompt without corrupting parsed source");
    Check(m.words[win+0x3dc]==9 && OwlPresentation::Byte(m,win+0x3b8)==5,
          "confirmation keeps completed page drawable, never inactive");
    constexpr U32 stack=0x28000000;
    m.words[stack]=0x12345678;m.words[stack+4]=0x1cd888;
    r[13]=stack;r[0]=engine;const auto page_before=m.words;
    hook.Handle(m,r,10,service);
    Check(r[13]==stack+8&&r[4]==0x12345678&&r[15]==0x1cd888&&m.words==page_before,
          "freeze updates with balanced native stack and no page changes");
    Check(exit_fade==ExitFade::Idle,"never fade before commit");
    r[4]=play+0x3bec;r[0]=job;hook.Handle(m,r,3,service);hook.Handle(m,r,4,service);
    r[5]=play;r[4]=0x27000000;
    while(hook.presentation.frames<14)hook.Handle(m,r,9,service);
    Check(exit_fade==ExitFade::Idle,"confirmation holds for first fourteen updates");
    hook.Handle(m,r,9,service);
    Check(m.words[play+0x13c]==0x402&&exit_fade==ExitFade::Out,"verified commit starts fade with prompt retained");
    Check(m.words[labels]==0x736559,"Yes source remains intact for rendering");
    hook.Handle(m,r,9,service);
    Check(m.words[play+0x13c]==0x402,"wait for renderer black acknowledgement");
    exit_fade=ExitFade::Black;hook.Handle(m,r,9,service);
    Check(m.words[play+0x13c]==0x300&&m.words[play+0x10c]==0&&exit_fade==ExitFade::In,"black returns to actual title");
    Check(m.words[labels]==0x736559&&m.words[labels+4]==0x6f4e,"restore label bytes before leaving");
    exit_fade=ExitFade::Idle;
    // Host commit failure must not quit, and no checkpoint means ordinary owl save.
    m.words[play+0x13c]=0x402;service.commit=false;r[0]=play+0x3bec;
    hook.Handle(m,r,2,service);
    r[5]=play;
    while(hook.presentation.frames<15)hook.Handle(m,r,9,service);
    Check(exit_fade==ExitFade::Out&&m.words[play+0x13c]==0x402,
          "slow write fades at half second without exiting before verification");
    r[4]=play+0x3bec;r[0]=job;hook.Handle(m,r,3,service);hook.Handle(m,r,4,service);
    Check(exit_fade==ExitFade::Idle&&!hook.presentation.window,
          "failed commit cancels fade and returns normal presentation");
    r[0]=1;r[5]=play;hook.Handle(m,r,5,service);Check(m.words[play+0x13c]==0x402,"commit failure stays in game");
    service.allow=false;r[0]=play+0x3bec;hook.Handle(m,r,2,service);Check(hook.kind==Kind::None,"unarmed remains native");
    r[0]=play;r[15]=Sites[12][0]+4;hook.Handle(m,r,12,service);
    Check(r[15]==0x1cee24,"unarmed save retains ordinary message call");
    m.words[win+0x430]=1;
    Check(!hook.presentation.Confirm(m,play),"No never starts confirmation");
    enabled=false;r[0]=engine;r[2]=1;hook.Handle(m,r,11,service);
    Check(OwlPresentation::Byte(m,engine+0x58)==1,"disabled choice instruction replay");
    std::cout<<"Classic native hook guards, disabled replay, cycle/owls and write failures passed.\n";
}
