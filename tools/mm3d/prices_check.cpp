// SPDX-License-Identifier: GPL-2.0-or-later
#define main text_checks_main
#include "single_screen_text_check.cpp"
#undef main
#include "mm3d/prices.h"
int main(int argc,char** argv) {
    assert(argc==3||argc==5);const auto original=Load(argv[1]);
    if(argc==5) for(unsigned language:{4u,8u}) {
        const auto bytes=Load(argv[language==4?3:4]);
        const auto& doubles=language==4?MM3D::Prices::FrenchMessages:MM3D::Prices::SpanishMessages;
        const auto& triples=language==4?MM3D::Prices::FrenchTripleMessages:MM3D::Prices::SpanishTripleMessages;
        for(const auto& e:doubles) {
            Memory m(bytes);const auto entry=m.Entry(e.id);assert(entry);
            const auto offset=*m.Read(entry+12),length=*m.Read(entry+16);
            for(unsigned factor:{1u,2u,3u,2u,1u}) {
                const auto& edits=factor==3?triples:doubles;
                const MM3D::Prices::MessageEdit* want=nullptr;
                for(const auto& v:edits)if(v.id==e.id)want=&v;
                assert(want);auto expected=bytes;
                if(factor!=1) {
                    for(U32 i=0;i<length;++i)expected[offset+i]=i<want->text.size()?want->text[i]:0;
                    for(unsigned i=0;i<4;++i)expected[entry-Memory::Base+4+i]=want->updated>>(8*i);
                }
                MM3D::Prices::Message(m,Memory::Self,e.id,factor);assert(m.data==expected);
                m.writes=0;MM3D::Prices::Message(m,Memory::Self,e.id,factor);assert(m.writes==0);
            }
            Memory bad(bytes);bad.data[offset]^=1;const auto before=bad.data;
            MM3D::Prices::Message(bad,Memory::Self,e.id,3);assert(bad.data==before&&bad.writes==0);
        }
        for(const auto& e:triples)for(const auto& english:MM3D::Prices::TripleMessages)
            if(e.id==english.id)assert(e.metadata==english.metadata&&e.updated==english.updated);
        for(U32 id:{0x29d9,0x29da,0x0452,0x0471,0x1413,0x09d5,0x04b7,0x2b66}) {
            Memory m(bytes);MM3D::Prices::Message(m,Memory::Self,id,3);assert(m.data==bytes&&m.writes==0);
        }
    }

    for(unsigned factor:{2u,3u})for(const auto& e:factor==3?MM3D::Prices::TripleMessages:MM3D::Prices::Messages) {
        Memory m(original);const auto entry=m.Entry(e.id);assert(entry);
        const auto offset=*m.Read(entry+12),length=*m.Read(entry+16);
        MM3D::Prices::Message(m,Memory::Self,e.id,1);assert(m.data==original&&m.writes==0);
        MM3D::Prices::Message(m,Memory::Self,e.id,factor);assert(m.writes>0);
        auto expected=original;
        for(U32 i=0;i<length;++i)expected[offset+i]=i<e.text.size()?e.text[i]:0;
        for(unsigned i=0;i<4;++i)expected[entry-Memory::Base+4+i]=e.updated>>(8*i);
        assert(m.data==expected);
        m.writes=0;MM3D::Prices::Message(m,Memory::Self,e.id,factor);assert(m.writes==0&&m.data==expected);
        MM3D::Prices::Message(m,Memory::Self,e.id,1);assert(m.data==original);
        for(unsigned scenario=0;scenario<5;++scenario) {
            Memory bad(original);
            if(scenario==0)bad.data[offset]^=1;
            if(scenario==1)bad.data[entry-Memory::Base+4]^=1;
            if(scenario==2)bad.data[12]=99;
            if(scenario==3)bad.words[Memory::Self+12]=16;
            if(scenario==4)bad.denied=(Memory::Base+offset+length-1)&~3u;
            const auto before=bad.data;MM3D::Prices::Message(bad,Memory::Self,e.id,factor);
            assert(bad.data==before&&bad.writes==0);
        }
    }
    for(U32 id:{0x29d9,0x29da,0x0452,0x0471,0x1413,0x09d5,0x04b7,0x2b66,0x0008}) {
        Memory m(original);MM3D::Prices::Message(m,Memory::Self,id,3);assert(m.data==original&&m.writes==0);
    }
    using namespace MM3D::Prices;
    for(unsigned i=0;i<Sites.size();++i) {
        const auto& site=Sites[i];
        assert(MM3D::Prices::Instruction(site.pc,site.word)!=site.word);
        assert(MM3D::Prices::Instruction(site.pc,site.word^1)==(site.word^1));
        for(unsigned factor:{1u,2u,3u}) {
            Memory m(original);std::array<U32,16> r{};
            if(site.kind==Kind::Funds) {
                const auto base=(site.word>>16)&15,dest=(site.word>>12)&15;
                assert((site.word&0x0ff00fff)==0x01d004f0); // LDRSH +0x40, immediate, no writeback
                for(U32 wallet=0;wallet<=999;++wallet) {
                    r[base]=Wallet-0x40;m.words[Wallet]=wallet;Execute(m,r,i,factor);
                    const bool capped=site.pc==0x2d7d68||site.pc==0x2d8038;
                    const auto want=capped&&factor==3&&wallet>=500?std::max(wallet/3,200u):wallet/factor;
                    assert(r[dest]==want);assert(m.words[Wallet]==wallet);
                    for(U32 price:{1,2,5,10,20,30,40,50,100,200,500}) {
                        if(capped&&price!=200)continue;
                        const auto cost=capped?std::min(price*factor,500u):price*factor;
                        assert((r[dest]>=price)==(wallet>=cost));
                    }
                }
            } else if(site.kind==Kind::Fallback) {Execute(m,r,i,factor);assert(r[7]==(10*factor));}
            else {
                for(int amount:{-1000,-200,-100,-50,-40,-30,-20,-10,-2,-1,0,1,20,90,100,200}) {
                    r[0]=U32(amount);Execute(m,r,i,factor);
                    const int want=site.kind==Kind::Charge?(amount<0?-(site.pc==0x4775cc&&amount==-200?std::min(-amount*int(factor),500):-amount*int(factor)):amount):(amount==90?100-10*factor:amount);
                    assert(r[0]==U32(want));assert(r[15]==ChangeRupees&&r[14]==site.pc+4);
                }
            }
        }
    }
    // Payment flow fixtures: displayed metadata feeds native checks/deductions;
    // hard-coded fees use the separate check and charge paths exactly once.
    const auto cost=[&](U32 id,unsigned slot,unsigned factor) {
        Memory m(original);Message(m,Memory::Self,id,factor);
        return (*m.Read(m.Entry(id)+4)>>(slot*10))&1023;
    };
    assert(cost(0x0655,0,2)==100); // Bomb Bag
    assert(cost(0x29da,0,2)==500); // All-Night Mask exception
    assert(cost(0x1d11,0,2)==10&&cost(0x1d11,1,2)==80); // both Tingle choices
    assert(cost(0x0477,0,2)==8); // bank service fee, not transferred principal
    assert(cost(0x0c3b,0,2)==200&&cost(0x0c55,0,2)==40); // smithy price vs Gold Dust sale
    assert(cost(0x2b0b,0,2)==40&&cost(0x2b0b,1,2)==400); // milk choices
    assert(cost(0x2b0b,0,3)==60&&cost(0x2b0b,1,3)==500);
    assert(cost(0x29da,0,3)==500);
    assert(cost(0x1600,0,3)==500&&cost(0x1606,0,3)==1023); // repeat uses hard-coded charge, no metadata
    for(const auto& e:TripleMessages)if(e.id==0x1606||e.id==0x1600||e.id==0x2afa||e.id==0x2b0b)
        assert(e.text.find("500 Rupees")!=std::string_view::npos);
    assert(cost(0x06bb,0,3)==300&&cost(0x06d8,0,3)==300);
    assert(cost(0x0477,0,3)==12);
    // Already edited resources can transition between both modes and native.
    for(const auto& e:Messages) {
        Memory m(original);
        for(unsigned factor:{2u,3u,2u,1u})Message(m,Memory::Self,e.id,factor);
        assert(m.data==original);
    }
    for(U32 pc:{0x426510,0x426858,0x2cbee0,0x49b160,0x49b190,0x49b1ac,0x3da0d8,0x2752c4})
        for(const auto& site:Sites)assert(site.pc!=pc); // transfers, payouts, confiscation and theft
        Memory code(original);code.data.clear();auto binary=Load(argv[2]);
    for(size_t i=0;i+4<=binary.size();i+=4)code.words[0x100000+U32(i)]=U32(binary[i])|U32(binary[i+1])<<8|U32(binary[i+2])<<16|U32(binary[i+3])<<24;
    assert(MM3D::Prices::CodeMatches(code));
    for(const auto& s:Sites) {
        code.words[s.pc]^=1;assert(!MM3D::Prices::CodeMatches(code));code.words[s.pc]^=1;
    }
    Initialize(false,3);Initialize(true,3);assert(MM3D::Prices::multiplier.load()==1); // restart-only latch
    std::cout<<"Price messages, exclusions, reversible data edits, all wallet boundaries and code signatures passed\n";
}
