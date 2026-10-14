// SPDX-License-Identifier: GPL-2.0-or-later
#define main ExistingTextChecks
#include "single_screen_text_check.cpp"
#undef main
#include "mm3d/classic_save_hooks.h"
struct Guest : Memory {
    using Memory::Memory;
    void Write(U32 at,U32 value) {
        if(at>=Base&&at-Base+4<=data.size())Memory::Write(at,value);
        else {assert(words.contains(at));words[at]=value;}
    }
};
struct NoService {
    bool Begin(unsigned,MM3D::ClassicSaving::Kind){return false;}
    bool Finish(unsigned,MM3D::ClassicSaving::Kind){return false;}
    void Load(unsigned,std::span<const std::uint8_t>){}
};
int main(int argc,char** argv) {
    assert(argc==2);const auto original=Load(argv[1]);
    namespace C=MM3D::ClassicSaving;
    constexpr U32 player=0x8000000,play=0x8100000,owl=0x8200000,window=0x8300000,stack=0x8400000;
    for(unsigned scenario=0;scenario<9;++scenario) {
        Guest m(original);C::Hook hook;NoService service;
        C::enabled=true;C::resume_notice=true;C::resume_text_active=false;
        // Realistic save -> title -> load: this resource was already replaced
        // by the five-page classic save prompt in the same process.
        assert(Apply(m,Memory::Self,0xc01));
        for(unsigned n=4;n<=16;n+=4)m.words[0x6b3738+n]=*m.Read(Memory::Self+n);
        m.words[play+0x13c]=0x402;m.words[play+0x20dc]=player;m.words[play+0x2118]=owl;
        m.words[player+0xe14]=0x1fcb34;m.words[player+0x118]=0x0104abcd;
        m.words[player+0x11dd0]=0;m.words[player+0x24]=0;m.words[player+0x2c]=0;
        m.words[owl]=0x1b2;m.words[owl+0x1c]=0;m.words[owl+0x268]=0x58d8d4;
        m.words[owl+0x24]=std::bit_cast<U32>(100.f);m.words[owl+0x2c]=0;m.words[owl+0x134]=0;
        m.words[owl+4]=0x19;m.words[owl+0x264]=0x00010000;
        m.words[0x6b3724]=window;m.words[window+0x3dc]=0;
        for(unsigned i=0;i<9;++i)m.words[stack+4*i]=0x12340000+i;
        const U32 offset=*m.Read(m.Entry(0xc01)+12);
        switch(scenario) {
        case 1:C::enabled=false;break;
        case 2:C::resume_notice=false;break;
        case 3:m.words[window+0x3dc]=9;break;
        case 4:m.words[player+0xe14]=0x20d96c;break; // entrance animation still running
        case 5:m.words[play+0x2118]=0;break;
        case 6:m.data[offset]^=1;break; // unsupported text must never open save choices
        case 7:m.words.erase(stack+32);break;
        case 8:m.words[play+0x13c]=0x401;break;
        }
        std::array<U32,16> r{};r[0]=player;r[1]=play;r[13]=stack;r[15]=0x1fcb3c;
        const auto before=m.words;const auto payload_before=m.data;auto expected=r;expected[4]=player;
        hook.Handle(m,r,17,service);
        if(scenario) {
            assert(r==expected&&m.words==before&&m.data==payload_before);
            assert(!C::resume_text_active&&!hook.returning.player);continue;
        }
        assert(r[0]==play&&r[1]==player&&r[15]==0x224f14&&r[13]==stack+36);
        for(unsigned i=0;i<8;++i)assert(r[4+i]==before.at(stack+4*i));
        assert(r[14]==before.at(stack+32)&&!C::resume_notice&&C::resume_text_active);
        assert(m.Read(player+0x118)==0x0c01abcd&&m.Read(player+0x11dd0)==owl);
        assert(m.Read(owl+4)==before.at(owl+4)&&m.Read(owl+0x264)==before.at(owl+0x264));
        auto returned=ClassicOwlReturnText;
        for(const auto& d:MM3D::ClassicLocalizedText::Dialogues)if(m.Read(Memory::Base+12)==d.language)returned=d.returned;
        for(unsigned i=0;i<*m.Read(m.Entry(0xc01)+16);++i)
            assert(m.data[offset+i]==(i<returned.size()?static_cast<unsigned char>(returned[i]):0));
        assert(m.Read(m.Entry(0xc01)+8)==0x00ff0001);
        // Native initializer installs conversation state before attention updates.
        m.words[player+0xe14]=0x22d06c;
        constexpr U32 scene=0x8600000,bars=0x8700000;
        m.words[0x72f5c0]=scene;m.words[scene+0x16c]=play;
        m.words[bars]=0x12340556; // Actual escaped draw was five pixels high.
        // Native transient state must not end our owned notice's draw scope.
        for(unsigned other=0;other<6;++other) {
            if(other==1)m.words[scene+0x16c]=play+4;
            if(other==2)m.words[player+0x11dd0]=owl+4;
            if(other==3)m.words[play+0x20dc]=player+4;
            if(other==4)C::resume_text_active=false;
            if(other==5)m.words[play+0x13c]=0x403;
            r={};r[0]=bars;r[15]=0x173af8;
            auto draw_expected=r;draw_expected[4]=other==4?5:0;
            const auto unchanged=m.words;
            hook.Handle(m,r,19,service);
            assert(r==draw_expected&&m.words==unchanged);
            m.words[scene+0x16c]=play;m.words[player+0x11dd0]=owl;
            m.words[play+0x20dc]=player;
            m.words[play+0x13c]=0x402;
            m.words[player+0xe14]=0x22d06c;C::resume_text_active=true;
        }
        // No one-frame gap while the native initializer installs the handler.
        m.words[player+0xe14]=0x1fcb34;
        assert(hook.returning.HideFocusBars(m));
        m.words[player+0xe14]=0x22d06c;
        // The verified pending resume must suppress the arrival animation,
        // before Enter has captured any player/Play identity at all.
        C::OwlReturn pending;C::resume_notice=true;C::resume_text_active=false;
        assert(pending.HideFocusBars(m));
        C::enabled=false;assert(!pending.HideFocusBars(m));C::enabled=true;
        m.words[play+0x13c]=0x302;assert(!pending.HideFocusBars(m));
        m.words[play+0x13c]=0x402;
        C::resume_notice=false;assert(!pending.HideFocusBars(m));
        C::resume_text_active=true;
        constexpr U32 camera=0x8500000;
        m.words[camera+0xa0]=play;
        for(unsigned other=0;other<4;++other) {
            r={};r[0]=other==0?0x0100076c:0x010007a3;r[6]=camera;r[15]=0x18b5e8;
            if(other==1)m.words[camera+0xa0]=play+4;
            if(other==2)m.words[player+0x11dd0]=owl+4;
            if(other==3)m.words[player+0xe14]=0x1fcb34;
            const auto unchanged=m.words;
            hook.Handle(m,r,18,service);
            assert(r[15]==0x2006e4&&r[14]==0x18b5e8&&hook.returning.focus_sound_pending);
            assert(m.words==unchanged);
            m.words[camera+0xa0]=play;m.words[player+0x11dd0]=owl;m.words[player+0xe14]=0x22d06c;
        }
        r={};r[0]=0x010007a3;r[6]=camera;r[15]=0x18b5e8;
        auto sound_expected=r;sound_expected[14]=0x18b5e8;const auto sound_memory=m.words;
        hook.Handle(m,r,18,service);
        assert(r==sound_expected&&!hook.returning.focus_sound_pending&&m.words==sound_memory);
        hook.Handle(m,r,18,service);
        assert(r[15]==0x2006e4); // future focus cues remain native
        const auto notice=m.data;
        assert(Apply(m,Memory::Self,0xc01)&&m.data==notice); // native parser lookups are idempotent
        for(U32 msg_state:{0u,5u,4u}) {
            r={};r[4]=player;r[6]=play;r[0]=msg_state;hook.Handle(m,r,16,service);
            assert(r[0]==msg_state&&C::resume_text_active&&hook.returning.player);
        }
        r={};r[4]=player;r[6]=play;r[0]=2;hook.Handle(m,r,16,service);
        assert(r[0]==2&&!C::resume_text_active&&!hook.returning.player);
        assert(m.Read(player+0x118)==0x0104abcd);
        C::resume_text_active=true;
        assert(!hook.returning.HideFocusBars(m)); // flag alone cannot claim ownership
        C::resume_text_active=false;
        // After dismissal every native signed height is replayed, without
        // touching the animation state or the separate side-bar height.
        for(U32 height:{0u,24u,127u,128u,255u}) {
            m.words[bars]=0x12340056|(height<<8);
            r={};r[0]=bars;const auto unchanged=m.words;
            hook.Handle(m,r,19,service);
            assert(r[4]==(height&128?height|0xffffff00u:height)&&m.words==unchanged);
        }
        r={};r[0]=0x010007a3;r[6]=camera;r[15]=0x18b5e8;
        hook.Handle(m,r,18,service);assert(r[15]==0x2006e4);
        assert(Apply(m,Memory::Self,0xc01)&&m.data==payload_before); // later A restores save prompt
        C::enabled=false;assert(Apply(m,Memory::Self,0xc01)&&m.data==original);
    }
    std::cout<<"Classic owl return: idle interception, balanced stack, text transitions, native finish and disabled/invalid cases passed.\n";
}
