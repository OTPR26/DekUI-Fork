// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/item_slide_interpolation.h"
#include "mm3d/cpu_interpolation_capture.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace MM3D::ItemSlideInterpolation;
void Require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
Values Quad(float x,float y=20) {
    Values u{};u[0]={1,0,-1.f/120,0};u[1]={1,0,0,-.005f};
    u[10]={0,3,94,1};u[11]={x,0,0,64};u[12]={y,0,32,0};u[13]={0,1,0,0};
    u[94]={0,0,0,1};u[95]={0,0,1,0};return u;
}
int main(int argc,char** argv) try {
    Require(Candidate(0x9b343fea686d98e0ULL,0xac22b79e29209c01ULL,96),"layout shader rejected");
    Require(!Candidate(0,0xac22b79e29209c01ULL,6),"world shader accepted");
    Require(!Candidate(0x9b343fea686d98e0ULL,0xac22b79e29209c01ULL,97),"invalid quad count accepted");
    for(bool opening:{true,false}) {
        const float travel=400;
        Require(Offset(opening,0,travel)==(opening?-travel:0),"start endpoint");
        Require(Offset(opening,6,travel)==(opening?0:-travel),"end endpoint");

        History history;
        Sample p{0,1,7,Offset(opening,0,travel),0,travel,true,opening};
        history.Apply(0,{1,2,1},Quad(p.x+100),6,{});
        for(unsigned tick=1;tick<=6;++tick) {
            Sample n{tick,1,7,Offset(opening,float(tick),travel),float(tick),travel,true,opening};
            auto delta=Between(p,n);Require(bool(delta),"valid animation tick rejected");
            const auto native=Quad(n.x+100);
            auto pose=history.Apply(tick,{1,2,1},native,6,{*delta});
            Require(pose&&std::abs(pose->x[0]-(100+Offset(opening,float(tick)-.5f,travel)))<.001f,"midpoint not on original linear slide");
            const float step=std::abs(n.x-p.x);
            Require(std::abs(step-travel/6.f)<.001f,"native linear slide timing changed");p=n;
        }
        p.frame=10;p.time=5;p.x=Offset(opening,5,travel);
        Sample n{11,1,7,0,0,0,false,false};
        Require(bool(Between(p,n))==opening,"end-of-opening handling");
        n=p;n.frame+=2;Require(!Between(p,n),"frame gap interpolated");
        n=p;n.frame++;n.owner++;Require(!Between(p,n),"different menu owner accepted");
        n=p;n.frame++;n.generation++;Require(!Between(p,n),"reset generation accepted");
    }
    const std::array<U32,3> key{1,2,3};
    History h;auto a=Quad(100),b=Quad(80);
    Require(!h.Apply(1,key,a,6,{}),"first frame changed");
    Require(!h.Apply(2,key,a,6,{{-20,10}}),"stationary HUD moved with menu");
    Require(!h.Apply(3,key,b,6,{}),"missing native slide evidence accepted");
    h.Reset();h.Apply(1,key,a,6,{});
    b[95][0]=.5f;Require(!h.Apply(2,key,b,6,{{-20,10}}),"different icon atlas accepted");
    h.Reset();h.Apply(1,key,a,6,{});b=Quad(80);
    Require(!h.Apply(3,key,b,6,{{-20,10}}),"skipped frame matched");
    h.Reset();h.Apply(1,key,a,6,{});
    Require(!h.Apply(2,{1,2,4},b,6,{{-20,10}}),"scene generation matched");
    // Duplicate glyphs and changing batch boundaries: match by quad contents and
    // the native slide displacement, not draw index or position in the batch.
    h.Reset();h.Apply(1,key,Quad(100),6,{});h.Apply(1,key,Quad(150),6,{});
    Values batch=Quad(80);batch[14]=batch[10];batch[15]=batch[11];batch[15][0]=130;
    batch[16]=batch[12];batch[17]=batch[13];
    auto pose=h.Apply(2,key,batch,12,{{-20,10}});
    Require(pose&&pose->x[0]==90&&pose->x[1]==140,"regrouped icons/glyphs mismatch");
    h.Reset();h.Apply(1,key,Quad(100),6,{});h.Apply(1,key,Quad(120),6,{});
    Require(!h.Apply(2,key,Quad(80),6,{{-20,10},{-40,20}}),"ambiguous motion accepted");
    // Validate the stamped bridge for immediate and next-frame GPU submission.
    Publish({10,99,7,Offset(true,3,400),3,400,true,true});
    Publish({11,99,7,Offset(true,4,400),4,400,true,true});
    Require(Recent(11,99).size()==1&&Recent(12,99).size()==1,"submission delay lost motion");
    Require(Recent(13,99).empty()&&Recent(12,98).empty(),"stale metadata reused");
    // Exercise the same eligibility decision used by PicaCore's CPU recorder.
    // Ordinary glyphs must remain cheap, but all popup batch sizes need inputs.
    const auto captures=[&](unsigned vertices,U64 frame,U32 generation,float color=1.f) {
        return MM3D::CpuInterpolation::CaptureShader(0x9b343fea686d98e0ULL,
            0xac22b79e29209c01ULL,vertices,color,frame,generation);
    };
    for(unsigned vertices=6;vertices<=96;vertices+=6) {
        Require(captures(vertices,11,99)&&captures(vertices,12,99),"CPU popup inputs not recorded");
        Require(!captures(vertices,13,99)&&!captures(vertices,12,98),"stale popup captures ordinary HUD");
    }
    Require(!captures(97,11,99),"invalid popup batch captured");
    Require(captures(6,13,99,-93),"dialogue CPU recording regressed");
    Require(MM3D::CpuInterpolation::CaptureShader(0x89b1ab98da3d9241ULL,
        0x6f0b398b1fc186a9ULL,300,0,13,99),"world CPU recording regressed");
    Publish({20,99,7,Offset(true,0,400),0,400,true,true});
    Require(Recent(20,99).empty()&&captures(96,20,99),"opening baseline lacks CPU inputs");
    Publish({21,99,0,0,0,0,false,false});
    Require(!captures(96,22,99),"closed popup retains CPU capture");
    // Optional local capture replay. The binary fixture contains observed native
    // uniforms plus the independently measured background displacement per frame.
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);Require(bool(f),"capture fixture missing");
        unsigned changed=0,draws=0;h.Reset();
        while(true) {
            U64 frame;U32 vertices;std::array<U32,3> k;float dx;Values u;
            f.read(reinterpret_cast<char*>(&frame),sizeof(frame));if(f.eof())break;
            f.read(reinterpret_cast<char*>(&vertices),sizeof(vertices));f.read(reinterpret_cast<char*>(k.data()),sizeof(k));
            f.read(reinterpret_cast<char*>(&dx),sizeof(dx));f.read(reinterpret_cast<char*>(u.data()),sizeof(u));
            Require(bool(f),"truncated fixture");
            const auto result=h.Apply(frame,k,u,vertices,dx!=0?std::vector<Delta>{{dx,-dx*.5f}}:std::vector<Delta>{});
            if(result)for(unsigned q=0;q<result->count;++q)
                if(result->x[q]!=u[11+q*4][0])++changed;
            ++draws;
        }
        Require(changed>100,"capture matched too few moving popup quads");
        std::cout<<"PASS capture: "<<draws<<" layout draws, "<<changed<<" moving quads matched\n";
    }
    std::cout<<"PASS item slide: native linear timing, endpoints, frame/owner guards, glyph batching, ambiguity and static HUD\n";
} catch(const std::exception& e) {std::cerr<<"FAIL item slide: "<<e.what()<<'\n';return 1;}
