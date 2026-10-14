// SPDX-License-Identifier: GPL-2.0-or-later
// clang++ -std=c++20 -Isrc tools/mm3d/difficulty_check.cpp -o /tmp/difficulty-check
#include "mm3d/difficulty.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace MM3D::Difficulty;
struct Memory {
    std::unordered_map<U32,U32> words;
    void Write(U32 at,U32 value){words[at]=value;}
    std::optional<U32> Read(U32 at) const {auto i=words.find(at);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
};
int main(int argc,char** argv) {
    // Native enhanced-defense condition and exact disabled behavior.
    for(U32 flags:{0u,0x80000000u,0x10000000u,0x90000000u})for(int hit=1;hit<=320;++hit) {
        const U32 delta=U32(-hit);const bool lt=((flags>>31)&1)!=((flags>>28)&1);
        assert(DefenseDelta(delta,delta,flags,false)==(lt?U32(-(hit+1)/2):delta));
        const U32 weak=DefenseDelta(delta,delta,flags,true);
        const int expected=(2*hit+1)/3;
        assert(weak==(lt?U32(-expected):delta));
        assert(weak==DefenseDelta(delta,delta,flags,true)); // no per-hit randomness
        for(int multiplier:{1,2,16})if(lt)
            assert(ApplyDamage(320,weak,multiplier)==(multiplier==1?320+weak:U32(std::max(0,320-expected*multiplier))));
    }
    assert(DefenseDelta(U32(-1),U32(-1),0x80000000,true)==U32(-1));
    assert(DefenseDelta(0x80000000u,0x80000000u,0x80000000,true)==U32(-1431655765ll));
    assert(DefenseDelta(123,16,0,true)==123); // non-LT instruction leaves r4 alone
    // One heart = 16 health units. Include one-unit hazards, quarter hearts,
    // lethal hits, zero health and amounts large enough to overflow a halfword.
    for(U32 health:{0u,1u,4u,16u,48u,320u})for(int damage:{1,4,8,16,320,32768})for(int factor=1;factor<=16;++factor) {
        const auto delta=std::bit_cast<U32>(-damage);
        auto result=ApplyDamage(health,delta,factor);
        if(factor==1)assert(result==health+delta); // bit-exact replay at default
        else assert(result==U32(std::max<std::int64_t>(0,std::int64_t(health)-std::int64_t(damage)*factor)));
    }
    for(int factor=1;factor<=16;++factor)for(U32 heal:{0u,1u,16u,320u,0x7fffu})
        assert(ApplyDamage(48,heal,factor)==48+heal);
    assert(ApplyDamage(48,U32(-4),2)==40); // quarter-heart hit becomes half a heart
    assert(ApplyDamage(48,U32(-2),2)==44); // native defense reduction happened first
    assert(ApplyDamage(16,0x80000000u,8)==0);
    assert(ApplyDamage(48,U32(-4),0)==44&&ApplyDamage(48,U32(-4),100)==0);
    for(U32 pc:{OriginalPC,RestorationPC}) {
        Memory m;for(unsigned i=0;i<HealthCode.size();++i)m.words[pc-0x38+4*i]=HealthCode[i];
        assert(CodeMatches(m,pc)&&Instruction(pc,Word)==(0xef000000|SVC));
        assert(CodeMatches(m,pc-16)&&Instruction(pc-16,DefenseWord)==(0xef000000|DefenseSVC));
        assert(Instruction(pc-16,DefenseWord^1)==(DefenseWord^1));
        assert(Instruction(pc,Word^1)==(Word^1)&&Instruction(pc+4,Word)==Word);
        for(unsigned i=0;i<HealthCode.size();++i){m.words[pc-0x38+4*i]^=1;assert(!CodeMatches(m,pc)&&!CodeMatches(m,pc-16));m.words[pc-0x38+4*i]^=1;}
    }
    for(U32 pc:{SpawnPC,RestorationSpawnPC}) {
        Memory m;for(unsigned i=0;i<SpawnCode.size();++i)m.words[pc+4*i]=SpawnCode[i];
        assert(SpawnCodeMatches(m,pc)&&SpawnInstruction(pc,SpawnWord)==(0xef000000|SpawnSVC));
        for(unsigned i=0;i<SpawnCode.size();++i){m.words[pc+4*i]^=1;assert(!SpawnCodeMatches(m,pc));m.words[pc+4*i]^=1;}
    }
    for(bool enabled:{false,true})for(bool rupees:{false,true})for(bool fairies:{false,true})
        for(bool magic:{false,true})for(bool ammo:{false,true})
        for(U32 actor:{0u,0xeu,0xfu,0x10u,0xd2u,0x1b0u})
        for(U32 type=0;type<256;++type)for(U32 flags:{0u,0x8000u,0x3f00u}) {
            Memory m;std::array<U32,16> regs{};
            for(unsigned i=0;i<16;++i)regs[i]=0x123000+i*4;
            regs[2]=actor;regs[13]=0x200000;regs[15]=RestorationSpawnPC+4;
            m.words[regs[13]+8]=type|flags;const auto before=m.words;
            const std::array<U32,9> ammo_types{4,5,8,9,10,11,12,13,23};
            const bool ammo_type=std::find(ammo_types.begin(),ammo_types.end(),type)!=ammo_types.end();
            const auto saved=regs;const bool suppressed=(actor==0xe&&((enabled&&type==3)||
                (rupees&&(type==0||type==1||type==2||type==19||type==20))||
                (magic&&(type==14||type==15))||(ammo&&ammo_type)))||
                (actor==0x10&&fairies&&(type&15)==2);
            Spawn(m,regs,enabled,rupees,fairies,magic,ammo);
            if(suppressed) {
                assert(regs[0]==0&&regs[15]==saved[14]&&m.words==before);
                for(unsigned i=1;i<15;++i)assert(regs[i]==saved[i]);
            } else {
                for(unsigned i=0;i<16;++i)assert(regs[i]==(i==13?saved[i]-16:saved[i]));
                for(unsigned i=0;i<3;++i)assert(m.words[regs[13]+4*i]==saved[4+i]);
                assert(m.words[regs[13]+12]==saved[14]);
            }
        }
    // Optionally verify signatures directly against locally supplied game code.
    for(int arg=1;arg<argc;++arg){
        std::ifstream f(argv[arg],std::ios::binary);assert(f);bool matched=false;
        for(U32 pc:{OriginalPC,RestorationPC}) {
            Memory m;f.clear();f.seekg(pc-0x38-0x100000);
            for(unsigned i=0;i<HealthCode.size();++i){U32 w=0;f.read(reinterpret_cast<char*>(&w),4);m.words[pc-0x38+4*i]=w;}
            matched|=CodeMatches(m,pc);
        }
        assert(matched);bool spawn_matched=false;
        for(U32 pc:{SpawnPC,RestorationSpawnPC}) {
            Memory m;f.clear();f.seekg(pc-0x100000);
            for(unsigned i=0;i<SpawnCode.size();++i){U32 w=0;f.read(reinterpret_cast<char*>(&w),4);m.words[pc+4*i]=w;}
            spawn_matched|=SpawnCodeMatches(m,pc);
        }
        assert(spawn_matched);
        // Verify the healing-fairy profile, low-nibble role extraction, and
        // both native drop paths' type2/actor0x10 assignments.
        for(auto [at,expected]:std::array<std::pair<U32,U32>,7>{{
            {0x675328,0x00070010},{0x675338,0x31f1e4},{0x31f438,0xe200000f},
            {0x2384d8,0xe3800002},{0x2384fc,0xe3a02010},
            {0x547858,0xe3800002},{0x547878,0xe3a02010}}}) {
            U32 word=0;f.clear();f.seekg(at-0x100000);
            f.read(reinterpret_cast<char*>(&word),4);assert(word==expected);
        }
        // Installed pickup dispatch identifies magic jars, bomb and arrow refill
        // items, plus the get-item IDs for sticks and both nut quantities.
        for(const auto [type,target,word]:std::array<std::array<U32,3>,11>{{
            {4,0x428dc4,0xe3a0108f},{11,0x428dc4,0xe3a0108f},
            {5,0x428ddc,0xe3a01093},{8,0x428df4,0xe3a01094},
            {9,0x428e0c,0xe3a01095},{10,0x428e24,0xe3a01096},
            {12,0x428d40,0xe3a05028},{13,0x428d38,0xe3a05019},
            {23,0x428d48,0xe3a0502a},{14,0x428e54,0xe3a0107a},
            {15,0x428e6c,0xe3a01079}}}) {
            U32 actual=0;f.clear();f.seekg(0x428c24+type*4-0x100000);
            f.read(reinterpret_cast<char*>(&actual),4);assert(actual==target);
            f.seekg(target-0x100000);f.read(reinterpret_cast<char*>(&actual),4);
            assert(actual==word);
        }
        // Native En_Item00 pickup dispatch: these five types grant rupee items.
        // Validate classification against the installed binary, not only the filter.
        for(const auto [type, target, item]:std::array<std::array<U32,3>,5>{{
            {0,0x428c98,0x84},{1,0x428cb8,0x85},{2,0x428cd8,0x87},
            {19,0x428d18,0x8a},{20,0x428cf8,0x88}}}) {
            U32 word=0;f.clear();f.seekg(0x428c24+type*4-0x100000);
            f.read(reinterpret_cast<char*>(&word),4);assert(word==target);
            f.seekg(target+8-0x100000);f.read(reinterpret_cast<char*>(&word),4);
            assert(word==(0xe3a01000|item));
        }
    }
    std::cout<<"Damage scaling, healing, overflow, heart/rupee/fairy/magic/ammo-drop filtering and code guards passed.\n";
}
