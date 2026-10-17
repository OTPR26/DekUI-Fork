// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <limits>
#include <stdexcept>
#include "mm3d/cpu_interpolation_capture.h"
#include "mm3d/lens_interpolation.h"
#include "mm3d/letterbox_interpolation.h"

namespace I=MM3D::InventoryInterpolation;
void Check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
int main() {
    {
        namespace B=MM3D::LetterboxInterpolation;
        B::Values u{};B::Material material;
        u[0]={0,-1.f/120,0,1};u[1]={-1.f/200,0,0,1};
        u[2]={0,0,1.f/1800,1.f/3};u[3]={0,0,0,1};
        u[8]={1.f/255,0,1.f/255,1};u[9]={1,1,1,1};
        u[20]={400,0,0,200};u[22]={0,0,1,-1200};
        u[70]={1,0,0,0};u[71]={0,1,0,0};u[72]={0,0,1,1200};
        material.state={1,6,7,0,15,0,0};
        for(auto& t:material.tev)t={0x0fff0fff,0,0,0xffffffff,0};
        material.tev[0]={238030384,0,0,0,0};
        B::History h;B::Key key{123,1};
        for(unsigned side=0;side<2;++side) {
            const auto at=[&](float height){auto v=u;v[21]={0,height,0,side?240-height*.5f:height*.5f};return v;};
            auto a=at(5),b=at(10);
            Check(B::Identify(a,material)==side,"top and bottom lock-on bars recognized separately");
            Check(!h.Apply(1,key,side,a),"bar acquisition remains native");
            const auto half=h.Apply(2,key,side,b);
            Check(half&&half->values[21][1]==7.5f&&half->values[21][3]==(side?236.25f:3.75f),
                  "bar entrance midpoint keeps outer edge fixed");
            Check(half&&half->alpha==0&&half->rows[21]&&!half->rows[20]&&!half->rows[22]&&half->values[8]==b[8],
                  "bars preserve width depth and color");
            const auto closing=h.Apply(3,key,side,a);
            Check(closing&&closing->values[21][1]==7.5f,"bar exit also interpolates");
            Check(!h.Apply(3,key,side,a)&&!h.Apply(4,key,side,b),"duplicate bar rejects ambiguous next frame");
            Check(!h.Apply(6,key,side,a),"missing bar frame does not bridge transitions");
            Check(!h.Apply(7,key,side,at(20)),"large bar height jump stays native");
            Check(!h.Apply(8,{123,2},side,at(15)),"bar generation change resets continuity");
            auto wrong=a;wrong[21][3]+=1;
            Check(!B::Identify(wrong,material),"unanchored rectangles are not letterbox bars");
            wrong=at(240);
            Check(!B::Identify(wrong,material),"full-screen transition fades excluded");
            wrong=a;wrong[20][0]=200;
            Check(!B::Identify(wrong,material),"partial-width rectangles excluded");
            auto bad=material;bad.textures=1;
            Check(!B::Identify(a,bad),"textured quads excluded");
            bad=material;bad.tev[0][3]=0xffffffff;
            Check(!B::Identify(a,bad),"nonblack screen effects excluded");
            bad=material;bad.state[6]=1;
            Check(!B::Identify(a,bad),"stencil effects excluded");
        }
        h.Reset();u[21]={0,5,0,2.5f};
        Check(!h.Apply(9,{123,2},0,u),"explicit reset clears both bar histories");
    }
    {
        namespace H=MM3D::HeartInterpolation;
        H::Values a{};H::Tev tev{};
        for(unsigned i=0;i<6;++i)tev[i]={i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu,
            0,i<2?0x10001u:0u,0xffffffffu,0};
        for(unsigned q=0;q<14;++q) {
            const unsigned r=10+4*q,uv=94-2*q;
            a[r]={0,3,float(uv),1};a[r+1]={float(q*8),0,0,6};a[r+2]={15,0,6,0};a[r+3]={0,.5f,0,0};
            a[uv]={0,.97265625f,0,.0234375f};a[uv+1]={0,.0078125f,.046875f,0};
        }
        auto b=a;b[11][0]-=.2f;b[12][0]-=.2f;b[11][3]=b[12][2]=6.4f;
        H::History h;H::Key key{1,2,3};
        Check(H::Recognized(a,84,tev),"captured full-heart batch recognized");
        Check(!h.Apply(1,key,a,84,tev),"heart initial frame native");
        auto half=h.Apply(2,key,b,84,tev);
        Check(half&&H::Near(half->values[11][3],6.2f)&&H::Near(half->values[11][0],-.1f),
              "heart pulse interpolates around its center");
        Check(half&&!half->rows[15]&&half->values[94]==b[94],"other hearts and atlas unchanged");
        Check(!h.Apply(2,key,b,84,tev)&&!h.Apply(3,key,a,84,tev),"duplicate heart batch invalidates history");
        h.Reset();h.Apply(1,key,a,84,tev);auto moved=b;moved[11][0]+=1;
        Check(!h.Apply(2,key,moved,84,tev),"HUD repositioning stays native");
        h.Reset();h.Apply(1,key,a,84,tev);
        Check(!h.Apply(3,key,b,84,tev),"heart frame gaps stay native");
        h.Reset();h.Apply(1,key,a,84,tev);
        Check(!h.Apply(2,{1,2,4},b,84,tev),"heart scene change stays native");
        auto wrong=a;wrong[94][1]=.5f;
        Check(!H::Recognized(wrong,84,tev),"nonheart atlas rejected");
        wrong=a;wrong[11][3]=12;
        Check(!H::Recognized(wrong,84,tev),"nonsquare heart geometry rejected");
        Check(MM3D::CpuInterpolation::CaptureShader(0x9b343fea686d98e0ULL,0xac22b79e29209c01ULL,
            84,1,1,3,a[94],a[95]),"CPU retains heart batch beyond ordinary glyph limit");
    }
    {
        namespace L=MM3D::LensInterpolation;
        L::Values a{};
        a[0]={0,-1.f/120,0,1};a[1]={-1.f/200,0,0,1};a[2]={0,0,1.f/1800,1.f/3};a[3]={0,0,0,1};
        a[20]={-.6f,0,0,200};a[21]={0,-.6f,0,120};a[22]={0,0,.6f,-1200};
        auto b=a;b[20][0]=b[21][1]=-.67f;b[22][2]=.67f;
        L::Material m;m.textures=1;m.state={1,1,1,0,15,0,1};
        for(auto& t:m.tev)t={0x0fff0fff,0,0,0xffffffff,0};
        m.tev[0]={238226995,0,0,0xff000000,0};
        L::History h;const L::Key key{1,2,3};
        for(unsigned kind=0;kind<3;++kind) {
            if(kind<2){m.stencil={1,0,kind,255,255,2,2,2};m.alpha={1,kind?7u:4u,204};}
            else {
                m.textures=3;
                m.state={1,6,7,0,15,1,0};m.alpha={0,6,158};m.stencil={};
                m.tev[0]={238945091,131072,65545,0xff000000,1};
                m.tev[1]={250547951,0,65537,1721303193,0};
            }
            Check(L::Identify(a,m)==kind,"lens masks and purple cover recognized individually");
            auto wrong_texture=m;wrong_texture.textures=kind<2?3:1;
            Check(!L::Identify(a,wrong_texture),"lens texture stages must match the pass");
            Check(!h.Apply(1,key,kind,a),"lens first frame stays native");
            const auto half=h.Apply(2,key,kind,b);
            Check(half&&L::Near(half->values[22][2],.635f)&&half->values[20][3]==200&&half->values[21][3]==120,
                  "all lens passes share halfway scale and fixed center");
            Check(!h.Apply(2,key,kind,b)&&!h.Apply(3,key,kind,a),"duplicate lens draw rejects ambiguous history");
            Check(!h.Apply(5,key,kind,a),"lens inactive gap stays native");
            auto jump=a;jump[22][2]=1.4f;
            Check(!h.Apply(6,key,kind,jump),"lens discontinuity stays native");
            Check(!h.Apply(7,{1,2,4},kind,a),"lens generation change seeds history");
        }
        auto bad=a;bad[20][3]+=1;
        Check(!L::Identify(bad,m),"unrelated orthographic model excluded");
        bad=a;bad[0][1]=.1f;
        Check(!L::Identify(bad,m),"unrelated projection excluded");
        m.tev[1][3]=0;
        Check(!L::Identify(a,m),"unrelated transparent mesh material excluded");
        m.textures=1;m.state={1,1,1,0,15,0,1};m.tev[0]={238226995,0,0,0xff000000,0};
        m.tev[1]={0x0fff0fff,0,0,0xffffffff,0};m.stencil={1,0,0,255,255,2,2,2};m.alpha={1,4,204};
        Check(L::Identify(a,m)==0,"verified stencil restored");
        m.stencil[4]=0;
        Check(!L::Identify(a,m),"stencil reader is not a lens mask writer");
    }
    {
        namespace M=MM3D::MagicMeterInterpolation;
        M::Values a{};
        a[10]={0,3,94,1};a[11]={1.91667175f,0,0,46.093746f};
        a[12]={23.8f,0,6.72f,0};a[13]={0,.5f,0,0};
        a[94]={0,.921875f,0,.015625f};a[95]={0,0,.03125f,0};
        M::Tev material{};
        for(unsigned i=0;i<6;++i)material[i]={i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu,
            0,i<2?0x10001u:0u,i==0?0xff06ff00u:0xffffffffu,0};
        auto b=a;b[11][3]=49.21875f;
        const M::Key key{1,2,3};M::History meter;
        Check(M::Recognized(a,material),"captured magic fill recognized");
        Check(!meter.Apply(1,key,a,material),"magic first frame stays native");
        auto half=meter.Apply(2,key,b,material);
        Check(half&&std::abs(half->values[11][3]-47.656248f)<.00001f,
              "refill generates intermediate width");
        Check(half->values[11][0]==a[11][0]&&half->values[12]==a[12]&&half->values[94]==a[94],
              "magic interpolation preserves anchor height and atlas");
        Check(meter.Apply(3,key,a,material).has_value(),"gradual magic drain interpolates");
        Check(!meter.Apply(3,key,b,material)&&!meter.Apply(4,key,a,material),"duplicate magic draws reject ambiguous history");
        Check(!meter.Apply(6,key,b,material),"magic gap seeds fresh history");
        Check(!meter.Apply(7,{1,2,4},a,material),"magic scene change seeds history");
        meter.Reset();meter.Apply(1,key,a,material);
        auto moved=b;moved[11][0]+=1;
        Check(!meter.Apply(2,key,moved,material),"HUD placement changes stay native");
        meter.Reset();meter.Apply(1,key,a,material);moved=b;moved[12][2]*=1.1f;
        Check(!meter.Apply(2,key,moved,material),"HUD height changes stay native");
        meter.Reset();meter.Apply(1,key,a,material);moved=b;moved[11][3]=75;
        Check(!meter.Apply(2,key,moved,material),"instant refill jump stays native");
        auto wrong_material=material;wrong_material[0][3]=0xff000000;
        Check(!M::Recognized(a,wrong_material),"magic background does not match green fill");
        moved=a;moved[94][1]=.916015625f;
        Check(!M::Recognized(moved,material),"magic border atlas excluded");
        moved=a;moved[11][3]=std::numeric_limits<float>::quiet_NaN();
        Check(!M::Recognized(moved,material),"invalid magic width excluded");
        // User-scaled HUD keeps its own height, origin and rate bound.
        a[13][1]=1;a[11][3]*=2;a[12][2]*=1.2f;b=a;b[11][3]+=6.25f;
        meter.Reset();meter.Apply(1,key,a,material);half=meter.Apply(2,key,b,material);
        Check(half&&half->values[12]==a[12],"scaled magic meter retains custom height");
        Check(MM3D::CpuInterpolation::CaptureShader(0x9b343fea686d98e0ULL,
            0xac22b79e29209c01ULL,6,1,200,3,a[94],a[95]),"CPU retains magic fill without menu owner");
        Check(!MM3D::CpuInterpolation::CaptureShader(0x9b343fea686d98e0ULL,
            0xac22b79e29209c01ULL,6,1,200,3),"CPU does not retain arbitrary HUD quads");
    }
    const I::Key key{100,200,7,42};
    I::Values a{};
    a[0]={1,0,-1.f/120,0};a[1]={1,0,0,-1.f/200};
    a[10]={0,3,94,1};a[11]={10,0,0,32};a[12]={20,0,16,0};a[13]={0,1,0,0};
    a[94]={0,.25f,0,.125f};a[95]={0,.5f,.0625f,0};
    I::Tev tev{};
    for(unsigned i=0;i<6;++i)
        tev[i]={i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu,0,
                i<2?0x10001u:0u,0xffffffffu,0};
    auto b=a;b[11][0]=110;b[12][0]=70;b[11][3]=40;b[12][2]=20;
    auto faded=tev;faded[0][3]=0x80ffffff;
    I::History h;
    Check(!h.Apply(10,key,a,6,tev),"first sample is native");
    const auto half=h.Apply(11,key,b,6,faded);
    Check(half&&half->values[11][0]==60&&half->values[12][0]==45&&
        half->values[11][3]==36&&half->values[12][2]==18,"slide and scale interpolate together");
    Check(half->alpha>.75f&&half->alpha<.76f,"combiner opacity midpoint");
    Check(half->values[94]==b[94]&&!half->rows[94]&&half->values[10]==b[10],"UVs and descriptors unchanged");
    Check(b[11][0]==110&&faded[0][3]==0x80ffffff,"native inputs untouched");
    for(unsigned failure=0;failure<9;++failure) {
        I::History guard;guard.Apply(10,key,a,6,tev);
        auto k=key;auto u=b;auto t=tev;I::U64 frame=11;
        switch(failure) {
        case 0:k[3]=0;break;
        case 1:k[3]++;break;
        case 2:k[2]++;break;
        case 3:k[0]++;break;
        case 4:frame=12;break;
        case 5:u[94][1]+=.125f;break;
        case 6:u[11][0]=1000;break;
        case 7:u[11][0]=std::numeric_limits<float>::quiet_NaN();break;
        case 8:t[1][0]=0;break;
        }
        Check(!guard.Apply(frame,k,u,6,t),"ownership, discontinuity or material guard failed");
    }
    I::History repeated;
    repeated.Apply(10,key,a,6,tev);auto duplicate=a;duplicate[11][0]+=40;
    repeated.Apply(10,key,duplicate,6,tev);
    Check(!repeated.Apply(11,key,b,6,tev),"ambiguous repeated atlas cell must not be joined");
    // Closing Gear changes inherited f8 while the same widgets keep sliding.
    // It is unused by the verified layout descriptor paths, unlike projection.
    I::History closing;
    auto closing_a=a,closing_b=b;
    closing_a[8]={.49803925f,.49803925f,.49803925f,1};closing_b[8]={0,0,0,0};
    closing.Apply(1,key,closing_a,6,tev);
    const auto closing_half=closing.Apply(2,key,closing_b,6,tev);
    Check(closing_half&&closing_half->values[11][0]==60,
          "unused inherited shader value cannot interrupt closing motion");
    Check(!closing_half->rows[8]&&closing_half->values[8]==closing_b[8],
          "scratch uniform stays native rather than being interpolated");
    closing_b[0][0]+=1;
    Check(!closing.Apply(3,key,closing_b,6,tev),"projection changes still break matching");

    // Gear emits three black shadow strips with the same atlas cells but
    // distinct opacity and vertical positions. Reorder them between frames.
    auto strip=a;
    strip[14]=strip[10];strip[14][2]=92;
    strip[15]=strip[11];strip[15][0]+=100;
    strip[16]=strip[12];strip[17]=strip[13];
    strip[92]=strip[94];strip[93]=strip[95];
    const auto strip_pose=[&](unsigned i,float dx) {
        auto u=strip;
        for(unsigned q=0;q<2;++q){u[11+4*q][0]+=dx;u[12+4*q][0]+=1.3f*i;}
        return u;
    };
    const auto strip_tev=[&](unsigned i) {
        auto t=tev;t[0][3]=(36+23*i)<<24;return t;
    };
    I::History shadows;
    for(unsigned i=0;i<3;++i)shadows.Apply(1,key,strip_pose(i,0),12,strip_tev(i));
    for(unsigned i:{2u,0u,1u}) {
        auto u=strip_pose(i,-63.5432f);u[12][0]-=.000015f;u[16][0]-=.000015f;
        const auto p=shadows.Apply(2,key,u,12,strip_tev(i));
        Check(p&&std::abs(p->values[11][0]-(strip[11][0]-31.7716f))<.0001f,
              "distinct-opacity shadow strips interpolate horizontal slide despite order and float rounding");
        Check(p->alpha==float(36+23*i)/255.f,"shadow opacity is not borrowed from another strip");
    }
    for(unsigned failure=0;failure<5;++failure) {
        I::History guard;
        for(unsigned i=0;i<3;++i)guard.Apply(1,key,strip_pose(i,0),12,strip_tev(i));
        auto u=strip_pose(0,20);auto t=strip_tev(0);
        switch(failure) {
        case 0:u[12][0]+=1.3f;u[16][0]+=1.3f;t=strip_tev(1);u[11][3]+=1;break;
        case 1:t=strip_tev(1);break; // cannot join another opacity at a different Y
        case 2:u[15][0]+=2;break; // two quads must share displacement
        case 3:u[12][0]+=.1f;break;
        case 4:{auto duplicate_strip=strip_pose(0,40);
            guard.Apply(1,key,duplicate_strip,12,strip_tev(0));break;}
        }
        Check(!guard.Apply(2,key,u,12,t),"shadow fallback rejects changed shape, fade, shear, vertical motion or same-opacity ambiguity");
    }
    I::History fonts;
    a[10][0]=1;a[10][3]=-93;a[93]={1,1,1,.5f};b=a;b[93][3]=1;b[11][0]+=8;
    fonts.Apply(1,key,a,6,tev);
    const auto font=fonts.Apply(2,key,b,6,tev);
    Check(font&&font->values[93][3]==.75f&&font->values[11][0]==14,"font fade and position interpolate");
    I::Publish(100,7,42);
    Check(I::Owner(100,7)==42&&I::Owner(101,7)==42&&!I::Owner(102,7),"owner stamp expiration");
    Check(MM3D::CpuInterpolation::CaptureShader(0x9b343fea686d98e0ULL,
        0xac22b79e29209c01ULL,96,1,100,7),"CPU retains full inventory batches");
    I::Publish(101,7,0);
    Check(!I::Owner(101,7)&&!I::Owner(102,7),"inactive stamp closes ownership immediately");
    Check(!MM3D::CpuInterpolation::CaptureShader(0x9b343fea686d98e0ULL,
        0xac22b79e29209c01ULL,96,1,101,7),"ordinary gameplay batches not retained");
    std::cout<<"PASS inventory interpolation: motion, scale, fades, closing scratch state, shadow strips, UV isolation, ambiguity, ownership, CPU capture\n";
}
