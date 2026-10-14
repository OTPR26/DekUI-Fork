// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include "mm3d/splash_interpolation.h"
#include "mm3d/ripple_interpolation.h"
#include "mm3d/forest_particle_interpolation.h"
#include "mm3d/song_effect_interpolation.h"
#include "mm3d/glare_interpolation.h"
#include "mm3d/bloom_sampling.h"
#include <limits>
#include <unordered_map>
#include "mm3d/link_pose_capture.h"
#include "mm3d/link_interpolation.h"
#include <stdexcept>
#include "mm3d/camera_midpoint.h"
using namespace MM3D::CameraMidpoint;
void Check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
Matrix Camera(double degrees,double x=0) {
    Matrix m=Identity();const double angle=degrees*3.141592653589793/180.;
    m[0][0]=m[2][2]=std::cos(angle);m[0][2]=std::sin(angle);m[2][0]=-std::sin(angle);m[0][3]=x;
    return m;
}
int main() {
 try {
    {
        using namespace MM3D::ForestParticleInterpolation;
        const auto sprite=[](float x) {
            std::vector<Vertex> inputs(5);
            for(unsigned i=0;i<4;++i) {
                inputs[i][0]={x+float(i&1)*8,float(i>>1)*8,0,1};
                inputs[i][1]={5.f/255,75.f/255,155.f/255,.5f};
                inputs[i][2]={float(i&1),float(i>>1),0,1};
            }
            return inputs;
        };
        const std::vector<std::uint32_t> order{0,1,2,3,3,4};
        const auto a=Decode(sprite(0),order),b=Decode(sprite(2),order);
        Check(a && b,"captured forest strip layout recognized");
        MM3D::ForestParticleInterpolation::History h;
        Check(!h.Apply(1,1,1,*a),"forest particles seed history");
        const auto mid=h.Apply(2,1,1,*b);
        Check(mid && std::abs((*mid)[0].vertices[0][0][0]-1)<1e-6,
              "forest particle moves halfway in world space");
        Check(!h.Apply(4,1,1,*b),"particle gaps reset history");
        Check(!h.Apply(5,2,1,*b) && !h.Apply(6,2,2,*b),"texture and generation changes stay native");
        auto invalid=sprite(0);invalid[0][1][0]=.8f;
        Check(!Decode(invalid,order),"other particle colors excluded");
        invalid=sprite(0);invalid[2][0][2]=2;
        Check(!Decode(invalid,order),"non-square meshes excluded");
        invalid=sprite(0);invalid[0][0][0]=std::numeric_limits<float>::quiet_NaN();
        Check(!Decode(invalid,order),"nonfinite geometry excluded");
        Check(!Decode(sprite(0),std::vector<std::uint32_t>{0,1,2,3,2,4}),"wrong strip connectors rejected");
        Check(!Decode(sprite(0),std::vector<std::uint32_t>{0,1,2,99,99,4}),"invalid cache index rejected");
        auto reordered=sprite(0);std::swap(reordered[0],reordered[2]);
        const auto decoded=Decode(reordered,std::vector<std::uint32_t>{2,1,0,3,3,4});
        Check(decoded && (*decoded)[0].vertices==(*a)[0].vertices,
              "particle decode follows submission order rather than cache order");
        h.Reset();h.Apply(1,1,1,*a);
        auto duplicate=*b;duplicate.push_back(duplicate[0]);
        Check(!h.Apply(2,1,1,duplicate),"two particles cannot inherit one previous particle");
        h.Reset();h.Apply(1,1,1,duplicate);
        Check(!h.Apply(2,1,1,*b),"ambiguous previous particles rejected");
        h.Reset();h.Apply(1,1,1,*a);
        const auto distant=Decode(sprite(100),order);
        Check(!h.Apply(2,1,1,*distant),"new distant particle does not inherit old motion");
        auto both=*a;both.push_back((*distant)[0]);
        h.Reset();h.Apply(1,1,1,both);
        auto swapped=both;std::swap(swapped[0],swapped[1]);
        for(auto& q:swapped)for(auto& v:q.vertices)v[0][0]+=2;
        const auto matched=h.Apply(2,1,1,swapped);
        Check(matched && (*matched)[0].vertices[0][0][0]==101 && (*matched)[1].vertices[0][0][0]==1,
              "particle reordering preserves separate identities");
    }
    {
        using namespace MM3D::LinkPoseMath;
        MM3D::LinkPoseCapture::LivePose pose;
        pose.pose.bones.resize(5);pose.bind.resize(5);
        MM3D::LinkInterpolation::BoneCandidates cached;
        for(unsigned pass=0;pass<6;++pass) {
            for(unsigned i=0;i<5;++i) {
                pose.pose.bones[i]=Flatten(Camera(i*12+pass,i*5+pass));
                pose.bind[i]=Flatten(pass%2?Camera(-int(i)*3,i):Identity());
            }
            if(pass==4)pose.pose.bones[1]=pose.pose.bones[0];
            if(pass==5)pose.bind[2][0]=std::numeric_limits<float>::quiet_NaN();
            cached.Prepare(pose);
            for(unsigned i=0;i<5;++i)for(bool skin:{false,true}) {
                auto value=Expand(pose.pose.bones[i]);
                if(skin)value=Multiply(value,Expand(pose.bind[i]));
                for(double offset:{-.031,-.03,0.,.03,.031}) {
                    auto query=value;query[0][3]+=offset;
                    Check(cached.Identify(query)==MM3D::LinkInterpolation::Identify(query,pose),
                          "position bounds preserve matching tolerance edges");
                }
                for(double perturbation:{0.,.000001,.1}) {
                    auto query=value;query[0][0]+=perturbation;
                    Check(cached.Identify(query)==MM3D::LinkInterpolation::Identify(query,pose),
                          "cached matches preserve direct/skin tolerance and ambiguity semantics");
                }
            }
            Check(cached.Identify(Matrix{})==MM3D::LinkInterpolation::Identify(Matrix{},pose),
                  "cached matching rejects singular matrix");
        }
        pose.pose.bones.clear();pose.bind.clear();cached.Prepare(pose);
        Check(!cached.Identify(Identity()),"empty pose invalidates cached candidates");
    }
    {
        using namespace MM3D::SplashInterpolation;
        const auto sprite=[](int cell) {
            Sprite s{};
            for(unsigned i=0;i<4;++i){s.points[i]={float(i%2)*40,float(i<2)*30,0};s.colors[i]={0,0,0,.8f};
                s.uv[i]={float(cell%8)*.125f+float(i%2)*.125f,1.f-float(cell/8)*.5f-float(i/2)*.5f};}
            return s;
        };
        MM3D::SplashInterpolation::History h;
        Check(!h.Apply(1,1,0,sprite(0)),"splash first frame native");
        auto m=h.Apply(2,1,0,sprite(1));
        Check(m && m->blend && std::abs(m->sprite.colors[0][3]-.4f)<1e-6,"splash frame blend halves additive weights");
        Check(m->old_uv==sprite(0).uv,"splash keeps previous cell coordinates");
        Check(!h.Apply(4,1,0,sprite(2)),"splash gap resets");
        auto moved=sprite(3);for(auto& p:moved.points)p[0]+=50;
        Check(!h.Apply(5,1,0,moved),"new landing does not inherit old splash");
        Check(!h.Apply(6,1,0,sprite(0)),"sprite restart stays native");
    }
    {
        using namespace MM3D::RippleInterpolation;
        const auto ring=[](float radius,float alpha,float x=0) {
            Mesh m;m.points.resize(9);m.colors.resize(9);
            constexpr int xs[]{-1,1,-1,1,1,-1,1,-1},zs[]{-1,-1,0,0,1,1,0,0};
            for(unsigned i=0;i<8;++i){m.points[i]={x+xs[i]*radius,.5f,zs[i]*radius};m.colors[i]={0,0,0,alpha};}
            return m;
        };
        MM3D::RippleInterpolation::History h;
        Check(!h.Apply(1,1,0,ring(10,1)),"ripple first frame native");
        auto mid=h.Apply(2,1,0,ring(12,.8f));
        Check(mid && std::abs(mid->points[0][0]+11)<1e-6 && std::abs(mid->colors[0][3]-.9f)<1e-6,"ripple expansion and fade midpoint");
        Check(mid->points[8]==Point{},"ripple sentinel unchanged");
        Check(!h.Apply(3,1,0,ring(14,.6f,100)),"new ripple does not inherit other center");
        Check(!h.Apply(5,1,0,ring(16,.4f,100)),"ripple gap reset");
        Check(!h.Apply(6,2,0,ring(18,.2f,100)),"ripple texture reset");
        auto malformed=ring(18,.2f);malformed.points[3][1]=2;
        Check(!h.Apply(7,2,0,malformed),"nonplanar splash excluded");
    }
    {
        using namespace MM3D::SongEffectInterpolation;
        MM3D::SongEffectInterpolation::History h;Values v{};Key a{1,2,3,4,5},b{1,6,7,4,5};
        Check(!h.Apply(1,a,v,true),"song first sample native");
        v[0][3]=.04f;auto m=h.Apply(2,a,v,true);
        Check(m && std::abs((*m)[0][3]-.02f)<1e-6,"song texture offset midpoint");
        Check(!h.Apply(2,b,v,true),"song layers have independent history");
        Check(!h.Apply(2,a,v,false),"song comparison stays native");
        Check(!h.Apply(4,a,v,true),"song gap resets");
        v[0][3]=1;Check(!h.Apply(5,a,v,true),"song offset jump remains native");
        v[0][0]=2;Check(!h.Apply(6,a,v,true),"song texture basis changes reset");
        h.Reset();Check(!h.Apply(7,a,v,true),"song generation reset");
        v={};v[0][0]=8;v[1][1]=1;v[2][0]=6;v[3][1]=-3;
        Check(Recognized(90,v),"captured temple song material recognized");
        Check(!Recognized(4,v),"screen quad with inherited song uniforms excluded");
        v[0][1]=.1f;Check(!Recognized(90,v),"unverified UV basis excluded");
        v={{{8,0,0,0},{0,4,0,0},{4.6982188225f,1.7100105286f,0,0},{-1.7100105286f,4.6982188225f,0,0}}};
        Check(Recognized(90,v),"Epona rotated texture basis recognized");
        Check(!Recognized(4,v),"Epona screen overlay excluded");
        v[2][0]+=.01f;Check(!Recognized(90,v),"different texture rotation excluded");
        v={{{1,0,0,0},{0,1,0,0},{2.9883618355f,.2614479661f,0,0},{-.2614479661f,2.9883618355f,0,0}}};
        Check(Recognized(276,v),"Zora Hall waterfall UV basis recognized");
        Check(Recognized(237,v),"second Zora Hall waterfall sheet recognized");
        Check(!Recognized(174,v),"waterfall uniforms inherited by particles excluded");
        h.Reset();Check(!h.Apply(1,a,v,true),"waterfall seeds history");
        v[0][3]=-.0016667f;v[1][3]=.0208333f;v[2][3]=.0075f;v[3][3]=.05f;
        m=h.Apply(2,a,v,true);
        Check(m && std::abs((*m)[3][3]-.025f)<1e-6 &&
              std::abs((*m)[1][3]-.01041665f)<1e-6 && (*m)[2][0]==v[2][0],
              "both waterfall texture offsets interpolate without changing rotation");
        Check(!h.Apply(4,a,v,true),"waterfall capture gap stays native");
        v[2][0]+=.01f;Check(!Recognized(276,v),"different waterfall UV basis excluded");
        v={{{1,0,0,0},{0,1.5f,0,0},{.9961205721f,-.0871493220f,0,0},{.3485972881f,3.9844822884f,0,0}}};
        Check(Recognized(282,v) && Recognized(270,v),"waterfall additive overlays recognized");
        v={{{1.5f,0,0,0},{0,1,0,0},{2.8975834846f,-.7764056921f,0,0},{.5176038146f,1.9317222834f,0,0}}};
        Check(Recognized(423,v),"waterfall lower overlay recognized");
        v={{{2,0,0,0},{0,2,0,0},{1,0,0,0},{0,1,0,0}}};
        Check(Recognized(549,v),"waterfall water surface layer recognized");


    }
    for(const auto signature:{0x578e07cd3309c81aull,0x018a783e5ef2d2bdull}) {
        using namespace MM3D::LinkPoseMath;
        MM3D::LinkPoseCapture::LivePose marker;
        marker.pose.instance=100;marker.model=200;marker.signature=signature;
        marker.pose.bones={Flatten(Camera(0))};marker.bind={Flatten(Identity())};
        marker.words={0xffff0000};
        auto depth=marker;depth.pose.instance=101;depth.model=201;
        MM3D::LinkInterpolation::Palette palette;palette.fill(Camera(0));
        MM3D::LinkInterpolation::History history;
        Check(!history.Apply(1,0,Identity(),palette,{marker,depth},true)[0],"marker pair seeds histories");
        marker.pose.bones={Flatten(Camera(60,4))};depth.pose.bones=marker.pose.bones;
        palette.fill(Expand(marker.pose.bones[0]));
        auto mid=history.Apply(2,33,Identity(),palette,{marker,depth},true);
        Check(mid[0] && Near(*mid[0],Camera(30,2),1e-6),"paired target rotation and position interpolate");
        Check(!history.Apply(4,99,Identity(),palette,{marker,depth},true)[0],"marker disappearance resets history");
        auto duplicate=depth;duplicate.pose.instance=102;duplicate.model=202;
        Check(!history.Apply(5,132,Identity(),palette,{marker,depth,duplicate},true)[0],"extra target owner is ambiguous");
        MM3D::LinkInterpolation::History disagree;
        marker.pose.bones={Flatten(Camera(0))};depth.pose.bones={Flatten(Camera(-60))};
        palette.fill(Camera(0));disagree.Apply(1,0,Identity(),palette,{marker,depth},true);
        palette.fill(Expand(depth.pose.bones[0]));disagree.Apply(1,0,Identity(),palette,{marker,depth},true);
        marker.pose.bones={Flatten(Camera(60))};depth.pose.bones=marker.pose.bones;
        palette.fill(Expand(marker.pose.bones[0]));
        Check(!disagree.Apply(2,33,Identity(),palette,{marker,depth},true)[0],"disagreeing target histories stay native");
        depth.signature=signature==0x578e07cd3309c81aull ? 0x018a783e5ef2d2bdull : 0x578e07cd3309c81aull;
        Check(!history.Apply(5,132,Identity(),palette,{marker,depth},true)[0],"mixed arrow and lock-on pair rejected");
        depth.signature=signature+1;
        Check(!history.Apply(5,132,Identity(),palette,{marker,depth},true)[0],"other paired models are not targets");
    }
    {
        using MM3D::BloomSampling::MidpointComposite;
        constexpr auto shader=0x89b1ab98da3d9241ULL,swizzle=0x6f0b398b1fc186a9ULL;
        Check(MidpointComposite(shader,swizzle,4,3,true),"captured bloom composite accepted");
        Check(!MidpointComposite(shader,swizzle,72,3,true),"scenery/glare not mistaken for bloom");
        Check(!MidpointComposite(shader,swizzle,4,1,true),"single texture effect not broadened");
        Check(!MidpointComposite(shader,swizzle,4,3,false),"perspective geometry excluded");
        Check(!MidpointComposite(shader+1,swizzle,4,3,true),"unverified shader excluded");
    }
    {
        SceneCopies copies;
        copies.Begin(10,100);copies.Transfer(10,100,200);
        Check(!copies.PreviousContains(10,200),"feedback cannot use current-frame-only provenance");
        copies.Begin(11,100);
        Check(copies.PreviousContains(11,200),"feedback recognizes previous scene transfer");
        Check(!copies.PreviousContains(12,200),"feedback requires the active frame");
        copies.Begin(13,100);
        Check(!copies.PreviousContains(13,200),"feedback provenance expires over gaps");
        Matrix p{};p[0]={0,-1./120,0,1};p[1]={-1./200,0,0,1};
        p[2]={0,0,.5,-.5};p[3]={0,0,0,1};
        auto m=Identity();m[0][0]=m[1][1]=m[2][2]=1.0008;
        m[0][3]=-.16;m[1][3]=-.096;
        Check(TransformationFeedback(p,m,true,4,1),"captured feedback quad recognized");
        Check(!TransformationFeedback(p,m,false,4,1),"arbitrary texture never matches feedback");
        Check(!TransformationFeedback(p,m,true,4,0),"untextured bars and fades excluded");
        Check(!TransformationFeedback(p,m,true,72,1),"sun glare excluded");
        m[0][3]=200;
        Check(!TransformationFeedback(p,m,true,4,1),"different quad placement excluded");
    }
    {
        using namespace MM3D::LinkPoseMath;
        using MM3D::LinkPoseCapture::LivePose;
        const auto pose=[](unsigned count,unsigned instance,std::uint64_t signature) {
            LivePose p;p.pose.instance=instance;p.model=instance;p.signature=signature;
            p.pose.bones.assign(count,Flatten(Identity()));p.bind.assign(count,Flatten(Identity()));
            p.words.resize(count);p.words[0]=0xffff0000;
            for(unsigned i=1;i<count;++i)p.words[i]=i;
            return p;
        };
        {
            auto rupee=pose(7,321,0xdf35e5b3de06d85eull);
            const auto transform=[](double angle,double x) {
                auto m=Camera(angle,x);
                for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)m[r][c]*=.015;
                return m;
            };
            MM3D::LinkInterpolation::Palette p;p.fill(transform(0,0));
            rupee.pose.bones.assign(7,Flatten(p[0]));
            MM3D::LinkInterpolation::History h;
            Check(!h.Apply(1,0,Identity(),p,{rupee},true)[0],"rupee needs history");
            p.fill(transform(60,4));rupee.pose.bones.assign(7,Flatten(p[0]));
            auto mid=h.Apply(2,33,Identity(),p,{rupee},true);
            Check(mid[0] && Near(*mid[0],transform(30,2),1e-6),"small scaled rupee rotates and moves at midpoint");
            Check(!h.Apply(2,33,Identity(),p,{rupee},false)[0],"disabled interpolation leaves rupee native");
            auto duplicate=rupee;duplicate.pose.instance++;
            Check(!h.Apply(3,66,Identity(),p,{rupee,duplicate},true)[0],"overlapping rupee instances rejected");
            Check(!h.Apply(4,99,Identity(),p,{rupee},true)[0],"rupee gap resets history");
            MM3D::LinkInterpolation::History divergent;
            rupee.pose.bones[1]=Flatten(transform(-60,4));
            divergent.Apply(1,0,Identity(),p,{rupee},true);
            rupee.pose.bones.assign(7,Flatten(p[0]));
            Check(!divergent.Apply(2,33,Identity(),p,{rupee},true)[0],"divergent rupee bones stay native");
            rupee.signature++;
            Check(!h.Apply(5,132,Identity(),p,{rupee},true)[0],"unverified equivalent-bone model rejected");
        }
        {
            auto like=pose(5,432,0xe5524c5eb3c8c915ull);
            auto a=Identity();a[0][1]=.15;
            auto b=a;b[0][1]=.25;b[1][3]=10;
            auto expected=a;expected[0][1]=.20;expected[1][3]=5;
            Check(!Half(a,b),"Like Like shear is not a rigid scaled rotation");
            const auto mid=MM3D::LinkInterpolation::LikeLikeHalf(a,b,like);
            Check(mid && Near(*mid,expected,1e-10),"Like Like shear and translation blend");
            auto jump=b;jump[0][0]=2;
            Check(!MM3D::LinkInterpolation::LikeLikeHalf(a,jump,like),"Like Like scale jump rejected");
            jump=b;jump[1][3]=250;
            Check(!MM3D::LinkInterpolation::LikeLikeHalf(a,jump,like),"Like Like joint jump rejected");
            jump=b;jump[2][2]=0;
            Check(!MM3D::LinkInterpolation::LikeLikeHalf(a,jump,like),"Like Like singular joint rejected");
            like.signature++;
            Check(!MM3D::LinkInterpolation::LikeLikeHalf(a,b,like),"other actors retain shear rejection");
        }
        {
            auto slime=pose(1,433,0xf0c54784cce31d97ull);
            auto a=Identity();a[0][0]=a[1][1]=a[2][2]=.01;a[0][1]=.003;
            auto b=a;b[0][1]=.0045;b[1][3]=4;
            auto expected=a;expected[0][1]=.00375;expected[1][3]=2;
            Check(!Half(a,b),"blob death shear cannot use rigid scale interpolation");
            for(unsigned vertices:{18u,444u,612u}) {
                const auto mid=MM3D::LinkInterpolation::SlimeHalf(a,b,slime,vertices);
                const auto reverse=MM3D::LinkInterpolation::SlimeHalf(b,a,slime,vertices);
                Check(mid && reverse && Near(*mid,expected,1e-10) && Near(*reverse,expected,1e-10),
                      "blob death deformation interpolates symmetrically");
                MM3D::LinkInterpolation::History h;
                MM3D::LinkInterpolation::Palette p;p.fill(Camera(0,9999));p[0]=a;
                slime.pose.bones[0]=Flatten(a);
                Check(!h.Apply(1,0,Identity(),p,{slime},true,vertices)[0],"blob seeds before interpolation");
                p[0]=b;slime.pose.bones[0]=Flatten(b);
                const auto result=h.Apply(2,33,Identity(),p,{slime},true,vertices);
                Check(result[0] && Near(*result[0],expected,1e-6),
                      "actor replay replaces death wobble with affine midpoint");
                Check(!h.Apply(2,33,Identity(),p,{slime},false,vertices)[0],
                      "disabled blob interpolation remains native");
                Check(!h.Apply(4,99,Identity(),p,{slime},true,vertices)[0],"blob gap resets death history");
            }
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,b,slime,36),
                  "shared one-bone prop signature alone cannot enable death fallback");
            auto invalid=b;invalid[0][0]=0;
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,invalid,slime,444),"collapsed blob rejected");
            invalid=b;invalid[0][0]=-.01;
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,invalid,slime,444),"flipped blob rejected");
            invalid=b;invalid[0][1]=.02;
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,invalid,slime,444),"large deformation rejected");
            invalid=b;invalid[0][3]=101;
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,invalid,slime,444),"blob teleport rejected");
            invalid=b;invalid[0][0]=std::numeric_limits<double>::quiet_NaN();
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,invalid,slime,444),"nonfinite blob rejected");
            slime.signature++;
            Check(!MM3D::LinkInterpolation::SlimeHalf(a,b,slime,444),"other skeletons retain shear guard");
        }
        {
            auto small=Identity(),large=Identity();
            small[0][0]=small[1][1]=small[2][2]=.4;
            Check(!Half(small,large),"default scale guard rejects fin-sized jump");
            const auto extended=Half(small,large,2.51);
            const auto retracted=Half(large,small,2.51);
            auto expected=Identity();expected[0][0]=expected[1][1]=expected[2][2]=.7;
            Check(extended && Near(*extended,expected,1e-10),"fin extension has intermediate size");
            Check(retracted && Near(*retracted,expected,1e-10),"fin retraction has intermediate size");
            for(const auto signature:{0x42c4c515d83599dbull,0x42c4c515d83599dcull}) {
                auto zora=pose(28,765,signature);
                for(unsigned bone:{24u,25u}) {
                    auto from=small,to=large;from[0][3]=to[0][3]=24;
                    zora.pose.bones[bone]=Flatten(from);
                    MM3D::LinkInterpolation::Palette palette;palette.fill(from);
                    MM3D::LinkInterpolation::ActorHistory history;
                    history.Apply(1,0,Identity(),palette,zora,true);
                    zora.pose.bones[bone]=Flatten(to);palette.fill(to);
                    const auto output=history.Apply(2,33,Identity(),palette,zora,true);
                    const double size=signature==0x42c4c515d83599dbull?.7:1.;
                    Check(output[0] && std::abs((*output[0])[0][0]-size)<1e-6,
                          "fin exception applies only to verified Zora bones");
                    zora.pose.bones[bone]=Flatten(Identity());
                }
            }
            small[0][0]=.1;
            Check(!Half(small,large,2.51),"larger fin scale jump remains rejected");
            small[0][0]=0;
            Check(!Half(small,large,2.51),"collapsed fin stays rejected");
        }
        {
            auto zora=pose(28,876,0x42c4c515d83599dbull);
            auto arm=Camera(20,15);auto other=Camera(-20,-15);
            zora.pose.bones[16]=zora.pose.bones[24]=Flatten(arm);
            zora.pose.bones[20]=zora.pose.bones[25]=Flatten(other);
            std::vector<Matrix> half(28,Identity());
            half[16]=Camera(10,7);half[24]=Camera(9,6);
            half[20]=Camera(-10,-7);half[25]=Camera(-9,-6);
            for(const auto pair:{std::array<unsigned,2>{16,24},std::array<unsigned,2>{20,25}}) {
                const auto current=Expand(zora.pose.bones[pair[0]]);
                const auto mid=MM3D::LinkInterpolation::ExtendedZoraFin(current,zora,half,Identity(),120);
                Check(mid && Near(*mid,half[pair[0]],1e-10),"extended blade follows forearm hierarchy");
                Check(!MM3D::LinkInterpolation::ExtendedZoraFin(current,zora,half,Identity(),144),
                      "other Zora geometry is excluded");
            }
            zora.pose.bones[24][0]*=.4;
            Check(!MM3D::LinkInterpolation::ExtendedZoraFin(arm,zora,half,Identity(),120),
                  "retracted fin cannot use extended arm attachment");
            zora.signature++;
            Check(!MM3D::LinkInterpolation::ExtendedZoraFin(other,zora,half,Identity(),120),
                  "non-Zora actor cannot use arm attachment");
        }
        for(const auto signature:{0xce5abb274d304de4ull,0x69e723ba0b961e6bull,0x04e3fb346228fd20ull,0xd56b42acae52759full,0x7526de611fb49ce3ull,0x3ed3e32e1ee38993ull,0x2507dd32b95b7c14ull}) {
            const unsigned count=(signature==0xce5abb274d304de4ull ||
                                  signature==0x3ed3e32e1ee38993ull) ? 3 :
                signature==0xd56b42acae52759full ? 7 :
                signature==0x7526de611fb49ce3ull ? 8 :
                signature==0x2507dd32b95b7c14ull ? 24 : 4;
            auto bubble=pose(count,654,signature);
            const auto transform=[](double scale,double x) {
                auto m=Identity();m[0][0]=m[2][2]=scale;m[1][1]=scale*.95;
                m[0][3]=x;return m;
            };
            MM3D::LinkInterpolation::Palette p;p.fill(transform(.02,0));
            bubble.pose.bones.assign(count,Flatten(p[0]));
            MM3D::LinkInterpolation::History h;
            Check(!h.Apply(1,0,Identity(),p,{bubble},true)[0],"bubble seeds history");
            p.fill(transform(.03,12));bubble.pose.bones.assign(count,Flatten(p[0]));
            auto mid=h.Apply(2,33,Identity(),p,{bubble},true);
            Check(mid[0] && Near(*mid[0],transform(.025,6),1e-6),"bubble growth and flight interpolate together");
            Check(!h.Apply(2,33,Identity(),p,{bubble},false)[0],"disabled bubble remains native");
            auto duplicate=bubble;duplicate.pose.instance++;
            Check(!h.Apply(3,66,Identity(),p,{bubble,duplicate},true)[0],"ambiguous bubbles rejected");
            Check(!h.Apply(4,99,Identity(),p,{bubble},true)[0],"bubble disappearance resets");
            MM3D::LinkInterpolation::History divergent;
            bubble.pose.bones[1]=Flatten(transform(.02,0));
            divergent.Apply(1,0,Identity(),p,{bubble},true);
            bubble.pose.bones.assign(count,Flatten(p[0]));
            Check(!divergent.Apply(2,33,Identity(),p,{bubble},true)[0],"disagreeing bubble bones rejected");
        }
        {
            auto barrier=pose(3,985,0x6735aa3cacc26ca6ull);
            const auto child=Camera(90);
            barrier.bind[1]=barrier.bind[2]=Flatten(InverseRigid(child));
            const auto update=[&](double angle,double x) {
                barrier.pose.bones[0]=Flatten(Camera(angle,x));
                barrier.pose.bones[1]=barrier.pose.bones[2]=Flatten(Multiply(Camera(angle,x),child));
            };
            MM3D::LinkInterpolation::Palette p;p.fill(Camera(0,9999));
            MM3D::LinkInterpolation::History h;
            update(0,0);p[0]=Expand(barrier.pose.bones[1]);
            Check(!MM3D::LinkInterpolation::Identify(p[0],barrier),
                  "electric barrier's identical sibling bones are individually ambiguous");
            Check(!h.Apply(1,0,Identity(),p,{barrier},true,216)[0],"electric barrier seeds history");
            update(40,4);p[0]=Expand(barrier.pose.bones[1]);
            for(unsigned vertices:{216u,108u}) {
                const auto mid=h.Apply(2,33,Identity(),p,{barrier},true,vertices);
                Check(mid[0] && Near(*mid[0],Multiply(Camera(20,2),child),1e-5),
                      "both electric barrier materials interpolate matching sibling bones");
            }
            Check(!h.Apply(2,33,Identity(),p,{barrier},false,216)[0],"disabled barrier remains native");
            auto duplicate=barrier;duplicate.pose.instance++;
            Check(!h.Apply(3,66,Identity(),p,{barrier,duplicate},true,216)[0],
                  "overlapping electric barriers remain ambiguous");
            Check(!h.Apply(4,99,Identity(),p,{barrier},true,216)[0],"barrier disappearance resets history");
            MM3D::LinkInterpolation::History divergent;
            barrier.pose.bones[2][3]+=6;
            divergent.Apply(1,0,Identity(),p,{barrier},true,216);
            barrier.pose.bones[2][3]-=6;
            Check(!divergent.Apply(2,33,Identity(),p,{barrier},true,216)[0],
                  "barrier siblings with different midpoint trajectories stay native");
            barrier.signature++;
            Check(!h.Apply(5,132,Identity(),p,{barrier},true,216)[0],
                  "unverified three-bone effect retains ambiguity guard");
        }
        for(const auto signature:{0xe26d2bb9ac2e87b6ull,0x76a7e789464b75d3ull,0xe40ebd58d8b46adaull}) {
            const unsigned count=signature==0xe26d2bb9ac2e87b6ull?3:2;
            auto tree=pose(count,1100,signature),other=pose(count,1101,signature);
            const auto update=[&](double angle,double x) {
                tree.pose.bones.assign(count,Flatten(Camera(angle,x)));
                other.pose.bones.assign(count,Flatten(Camera(angle,x+200)));
            };
            MM3D::LinkInterpolation::Palette p;
            MM3D::LinkInterpolation::History h;
            update(0,0);p.fill(Expand(tree.pose.bones[0]));
            Check(!h.Apply(1,0,Identity(),p,{tree,other},true)[0],"intro tree seeds history");
            update(20,4);p.fill(Expand(tree.pose.bones[0]));
            const auto mid=h.Apply(2,33,Identity(),p,{other,tree},true);
            Check(mid[0] && Near(*mid[0],Camera(10,2),1e-5),"intro tree keeps its own motion among identical skeletons");
            Check(!h.Apply(2,33,Identity(),p,{tree,other},false)[0],"disabled intro trees remain native");
            other.pose.bones=tree.pose.bones;
            Check(!h.Apply(3,66,Identity(),p,{tree,other},true)[0],"overlapping tree instances rejected");
            Check(!h.Apply(4,99,Identity(),p,{tree},true)[0],"tree capture gaps reset history");
            MM3D::LinkInterpolation::History divergent;
            tree.pose.bones[1][3]+=6;
            divergent.Apply(1,0,Identity(),p,{tree},true);
            tree.pose.bones[1][3]-=6;
            Check(!divergent.Apply(2,33,Identity(),p,{tree},true)[0],"tree bones with different past motion stay native");
            tree.signature++;
            Check(!h.Apply(5,132,Identity(),p,{tree},true)[0],"unverified scenery excluded");
        }
        {
            auto bomber=pose(17,987,0xc299a4754ffae267ull);
            // Two bones resolve to the same skin transform, with distinct binds.
            for(unsigned i=1;i<17;++i)bomber.pose.bones[i][3]=float(i*10);
            bomber.bind[1][3]=-10;
            MM3D::LinkInterpolation::Palette p;p.fill(Identity());
            MM3D::LinkInterpolation::History h;
            Check(!h.Apply(1,0,Identity(),p,{bomber},true)[0],"Bomber shared skin seeds history");
            for(auto& b:bomber.pose.bones)b[3]+=4;
            p.fill(Camera(0,4));
            auto mid=h.Apply(2,33,Identity(),p,{bomber},true);
            Check(mid[0] && Near(*mid[0],Camera(0,2),1e-6),"Bomber equivalent skin bones interpolate");
            auto duplicate=bomber;duplicate.pose.instance++;
            Check(!h.Apply(3,66,Identity(),p,{bomber,duplicate},true)[0],"overlapping Bombers remain ambiguous");
            Check(!h.Apply(4,99,Identity(),p,{bomber},true)[0],"Bomber disappearance resets");
            MM3D::LinkInterpolation::History disagree;
            bomber.pose.bones[1][3]+=6;
            disagree.Apply(1,0,Identity(),p,{bomber},true);
            bomber.pose.bones[1][3]-=6;
            Check(!disagree.Apply(2,33,Identity(),p,{bomber},true)[0],"divergent Bomber skin histories rejected");
            bomber.signature++;
            Check(!h.Apply(5,132,Identity(),p,{bomber},true)[0],"unverified 17-bone actor excluded");
        }
        {
            auto postman=pose(15,1001,0xbf2f38bb6502de36ull);
            auto cap=pose(1,1002,0x10aa2e239eaea424ull);
            auto bag=pose(1,1003,0x61a2e0e2eed80dccull);
            const auto update=[&](double angle,double x) {
                const auto root=Camera(angle,x);
                postman.pose.bones[0]=Flatten(root);
                for(unsigned i=1;i<15;++i)
                    postman.pose.bones[i]=Flatten(Multiply(root,Camera(i*3,i*5)));
                cap.pose.bones[0]=postman.pose.bones[14];
                bag.pose.bones[0]=postman.pose.bones[7];
            };
            const auto palette=[&](unsigned bone) {
                MM3D::LinkInterpolation::Palette p;p.fill(Camera(0,9999));
                p[0]=Expand(postman.pose.bones[bone]);return p;
            };
            MM3D::LinkInterpolation::History h;
            update(0,0);
            for(unsigned vertices:{204u,414u,96u})
                Check(!h.Apply(1,0,Identity(),palette(vertices==414?7:14),
                               {postman,cap,bag},true,vertices)[0],"Postman attachments seed independently");
            update(60,4);
            for(unsigned vertices:{204u,414u,96u}) {
                const unsigned bone=vertices==414?7:14;
                const auto p=palette(bone);
                const auto mid=h.Apply(2,33,Identity(),p,{postman,cap,bag},true,vertices);
                const auto expected=Multiply(Camera(30,2),Camera(bone*3,bone*5));
                Check(mid[0] && Near(*mid[0],expected,1e-4),
                      "Postman cap and bag stay on the interpolated parent bone");
                const auto separate=Half(Camera(bone*3,bone*5),p[0]);
                Check(separate && !PaletteNear(*separate,expected),
                      "attachment root interpolation would differ from parent hierarchy");
                Check(!h.Apply(2,33,Identity(),p,{postman,cap,bag},false,vertices)[0],
                      "disabled Postman interpolation stays native");
            }
            auto duplicate=postman;duplicate.pose.instance++;
            Check(!h.Apply(2,33,Identity(),palette(14),{postman,cap,bag,duplicate},true,204)[0],
                  "overlapping Postman owners remain ambiguous");
            Check(!h.Apply(2,33,Identity(),palette(14),{postman,cap,bag},true,205)[0],
                  "unverified attachment draw stays native");
            auto unrelated=cap;unrelated.signature++;
            Check(!h.Apply(2,33,Identity(),palette(14),{postman,unrelated,bag},true,204)[0],
                  "unverified accessory cannot borrow Postman history");
            cap.model++;
            Check(!h.Apply(3,66,Identity(),palette(14),{postman,cap,bag},true,204)[0],
                  "reused accessory address seeds new attachment");
            Check(h.Apply(4,99,Identity(),palette(14),{postman,cap,bag},true,204)[0].has_value(),
                  "stable attachment resumes after replacement");
            Check(!h.Apply(6,165,Identity(),palette(14),{postman,cap,bag},true,204)[0],
                  "Postman disappearance resets attachment");
            ++postman.generation;++cap.generation;
            Check(!h.Apply(7,198,Identity(),palette(14),{postman,cap,bag},true,204)[0],
                  "generation change resets Postman attachment");
            h.Reset();
            Check(!h.Apply(8,231,Identity(),palette(14),{postman,cap,bag},true,204)[0],
                  "explicit reset clears attachment history");
        }
        {
            auto carpenter=pose(20,1101,0x228b499bc4c2bef7ull);
            auto wood=pose(1,1102,0xa81ec0d047c2eaadull);
            const auto update=[&](double angle,double x) {
                const auto root=Camera(angle,x);
                carpenter.pose.bones[0]=Flatten(root);
                for(unsigned i=1;i<20;++i)
                    carpenter.pose.bones[i]=Flatten(Multiply(root,Camera(i*2,i*4)));
                wood.pose.bones[0]=carpenter.pose.bones[8];
            };
            MM3D::LinkInterpolation::Palette p;p.fill(Camera(0,9999));
            MM3D::LinkInterpolation::History h;
            update(0,0);p[0]=Expand(wood.pose.bones[0]);
            Check(!h.Apply(1,0,Identity(),p,{carpenter,wood},true,36)[0],
                  "carpenter wood seeds attachment history");
            update(60,4);p[0]=Expand(wood.pose.bones[0]);
            auto mid=h.Apply(2,33,Identity(),p,{wood,carpenter},true,36);
            const auto expected=Multiply(Camera(30,2),Camera(16,32));
            Check(mid[0] && Near(*mid[0],expected,1e-4),
                  "wood follows carpenter bone eight through hierarchy rotation");
            Check(!h.Apply(2,33,Identity(),p,{carpenter,wood},false,36)[0],
                  "disabled interpolation leaves wood native");
            Check(!h.Apply(2,33,Identity(),p,{carpenter,wood},true,204)[0],
                  "wood cannot use Postman attachment geometry");
            auto duplicate=carpenter;duplicate.pose.instance+=10;
            Check(!h.Apply(2,33,Identity(),p,{carpenter,wood,duplicate},true,36)[0],
                  "wood cannot choose between overlapping carpenters");
            auto unrelated=wood;unrelated.signature++;
            Check(!h.Apply(2,33,Identity(),p,{carpenter,unrelated},true,36)[0],
                  "unverified tool does not use wood attachment");
            wood.model++;
            Check(!h.Apply(3,66,Identity(),p,{carpenter,wood},true,36)[0],
                  "replaced wood model seeds history");
            Check(h.Apply(4,99,Identity(),p,{carpenter,wood},true,36)[0].has_value(),
                  "wood resumes after stable attachment");
            Check(!h.Apply(6,165,Identity(),p,{carpenter,wood},true,36)[0],
                  "carpenter disappearance resets wood attachment");
            h.Reset();
            Check(!h.Apply(7,198,Identity(),p,{carpenter,wood},true,36)[0],
                  "reset clears wood history");
        }
        {
            auto link=pose(24,800,0x17beb877e4d23506ull);
            auto pipes=pose(7,801,0x79937db4f6cb5c95ull);
            for(unsigned i=1;i<24;++i)link.pose.bones[i][3]=float(100+i*10);
            for(unsigned i=2;i<7;++i)pipes.pose.bones[i][3]=float(i*10);
            MM3D::LinkInterpolation::Palette p;p.fill(Camera(0,999));
            p[0]=Identity();
            for(unsigned i=0;i<5;++i)p[i+1]=Expand(pipes.pose.bones[MM3D::LinkInterpolation::PipeBones[i]]);
            MM3D::LinkInterpolation::History h;
            h.Apply(1,0,Identity(),p,{link,pipes},true,1284);
            for(auto& b:link.pose.bones)b[3]+=4;
            for(auto& b:pipes.pose.bones)b[3]+=4;
            pipes.pose.bones[2][7]=6;
            p[0]=Camera(0,4);
            for(unsigned i=0;i<5;++i)p[i+1]=Expand(pipes.pose.bones[MM3D::LinkInterpolation::PipeBones[i]]);
            auto mid=h.Apply(2,33,Identity(),p,{link,pipes},true,1284);
            Check(mid[0] && Near(*mid[0],Camera(0,2),1e-6),"pipes and Link share interpolated base");
            Check(mid[1] && std::abs((*mid[1])[1][3]-3)<1e-6,"pipe branch animates independently of Link");
            for(unsigned i=1;i<=5;++i)Check(bool(mid[i]),"all unique pipe branches interpolate");
            auto duplicate=pipes;duplicate.pose.instance++;
            Check(!h.Apply(3,66,Identity(),p,{link,pipes,duplicate},true,1284)[0],"duplicate pipes rejected");
            auto unrelated=link;unrelated.signature++;
            Check(!h.Apply(3,66,Identity(),p,{unrelated,pipes},true,1284)[0],"pipes cannot override unrelated owner");
            Check(!h.Apply(2,33,Identity(),p,{link,pipes},false,1284)[1],"disabled pipes remain native");
            MM3D::LinkInterpolation::History closing;
            pipes.pose.bones.assign(7,Flatten(Camera(0,4)));
            pipes.pose.bones[2][0]=pipes.pose.bones[2][5]=pipes.pose.bones[2][10]=1.2f;
            p.fill(Camera(0,4));p[1]=Expand(pipes.pose.bones[2]);
            closing.Apply(1,0,Identity(),p,{link,pipes},true,1284);
            pipes.pose.bones[2]=Flatten(Camera(0,4));p[1]=Camera(0,4);
            mid=closing.Apply(2,33,Identity(),p,{link,pipes},true,1284);
            Check(mid[1] && std::abs((*mid[1])[0][0]-1.1)<1e-6,
                  "closing pipe retains its identity when all openings coincide");
            Check(mid[2] && std::abs((*mid[2])[0][0]-1)<1e-6,
                  "other openings do not inherit the played note");

        }
        {
            auto a=pose(1,1001,0x63f1ea87bac6ed15ull),b=pose(1,1002,0x237eacb5846225e3ull),c=pose(1,1003,0xa5ab84c08cb4c10cull);
            MM3D::LinkInterpolation::Palette p;p.fill(Identity());
            MM3D::LinkInterpolation::History h;
            Check(!h.Apply(1,0,Identity(),p,{a,b,c},true)[0],"sign group seeds independent histories");
            a.pose.bones=b.pose.bones=c.pose.bones={Flatten(Camera(60,4))};p.fill(Camera(60,4));
            auto mid=h.Apply(2,33,Identity(),p,{a,b,c},true);
            Check(mid[0] && Near(*mid[0],Camera(30,2),1e-6),"verified sign fragments agree on fall midpoint");
            auto duplicate=a;duplicate.pose.instance++;duplicate.model++;
            Check(!h.Apply(3,66,Identity(),p,{a,b,c,duplicate},true)[0],"two signs sharing fragment signature rejected");
            c.signature++;
            Check(!h.Apply(3,66,Identity(),p,{a,b,c},true)[0],"unverified sign owner rejected");
            c.signature--;
            MM3D::LinkInterpolation::History divergent;
            c.pose.bones={Flatten(Camera(-60))};
            divergent.Apply(1,0,Identity(),p,{a,b,c},true);
            p.fill(Camera(-60));divergent.Apply(1,0,Identity(),p,{a,b,c},true);
            c.pose.bones=a.pose.bones;p.fill(Camera(60,4));
            Check(!divergent.Apply(2,33,Identity(),p,{a,b,c},true)[0],"sign histories must agree");
        }
        auto effect=pose(4,123,0xdfe0c7f5a2531807ull);
        MM3D::LinkInterpolation::Palette palette;palette.fill(Identity());
        MM3D::LinkInterpolation::History effect_history;
        Check(!effect_history.Apply(1,0,Identity(),palette,{effect},true)[0],"effect seeds history");
        for(auto& b:effect.pose.bones)b[3]=2;
        for(auto& b:palette)b[0][3]=2;
        auto result=effect_history.Apply(2,33,Identity(),palette,{effect},true);
        Check(result[0] && std::abs((*result[0])[0][3]-1)<1e-6,"equivalent transformation bones interpolate");
        MM3D::LinkInterpolation::History divergent;
        effect.pose.bones[1][3]=8;
        divergent.Apply(1,0,Identity(),palette,{effect},true);
        effect.pose.bones[1][3]=2;
        Check(!divergent.Apply(2,33,Identity(),palette,{effect},true)[0],"different effect histories remain native");
        for(const auto signature:{0x2090ecc88f377de1ull,0x15ad1b7410d5bd8dull}) {
            const unsigned count=signature==0x2090ecc88f377de1ull?7:3;
            auto fairy_mask=pose(count,654,signature);
            const auto old_root=Camera(0,0),new_root=Camera(30,10),head_mid=Camera(12,4);
            std::vector<Matrix> locals;
            for(unsigned i=0;i<count;++i) {
                locals.push_back(Camera(0,double(i*3)));
                fairy_mask.pose.bones[i]=Flatten(Multiply(old_root,locals[i]));
            }
            MM3D::LinkInterpolation::Palette p;p.fill(old_root);
            MM3D::LinkInterpolation::ActorHistory attached;
            attached.Apply(1,0,Identity(),p,fairy_mask,true);
            const auto old_hair=locals[1];locals[1]=Camera(20,3);
            for(unsigned i=0;i<count;++i)fairy_mask.pose.bones[i]=Flatten(Multiply(new_root,locals[i]));
            p[0]=new_root;p[1]=Expand(fairy_mask.pose.bones[1]);
            const auto mid=attached.Apply(2,33,Identity(),p,fairy_mask,true,false,false,0,head_mid);
            Check(mid[0] && Near(*mid[0],head_mid,1e-6),"animated mask root follows head midpoint");
            Check(mid[1] && Near(*mid[1],Multiply(head_mid,*Half(old_hair,locals[1])),1e-5),
                  "mask hair retains local animation beneath interpolated head");
        }
        auto human=pose(26,456,1234);
        human.words.assign(MM3D::LinkPoseCapture::BoneWords.begin(),MM3D::LinkPoseCapture::BoneWords.end());
        auto mask=pose(1,789,0x6787d559c31d313aull);
        // Give each human bone a unique current matrix; the mask matches head 11.
        for(unsigned i=0;i<26;++i)human.pose.bones[i][7]=float(i);
        mask.pose.bones[0][7]=11;
        palette.fill(Expand(mask.pose.bones[0]));
        MM3D::LinkInterpolation::History attachment;
        attachment.Apply(1,0,Identity(),palette,{human,mask},true);
        for(auto& b:human.pose.bones)b[3]=2;
        mask.pose.bones[0][3]=2;palette.fill(Expand(mask.pose.bones[0]));
        result=attachment.Apply(2,33,Identity(),palette,{human,mask},true);
        Check(result[0] && std::abs((*result[0])[0][3]-1)<1e-6,"mask and head agree on shared midpoint");
        for(const auto signature:{0x9c5589d73f0deea4ull,0x533d208b50f73607ull,0x12345678ull}) {
            mask.signature=signature;mask.pose.bones[0]=human.pose.bones[11];
            palette.fill(Expand(mask.pose.bones[0]));
            MM3D::LinkInterpolation::History worn;
            Check(!worn.Apply(10,330,Identity(),palette,{human,mask},true)[0],"new head attachment seeds");
            for(auto& bone:human.pose.bones)bone[3]+=2;
            mask.pose.bones[0]=human.pose.bones[11];palette.fill(Expand(mask.pose.bones[0]));
            auto mid=worn.Apply(11,363,Identity(),palette,{human,mask},true);
            Check(mid[0] && std::abs((*mid[0])[0][3]-(mask.pose.bones[0][3]-1))<1e-6,
                  "single-bone worn masks share human head interpolation");
            Check(!worn.Apply(13,429,Identity(),palette,{human,mask},true)[0],"head attachment gap rejected");
            mask.pose.instance++;
            Check(!worn.Apply(14,462,Identity(),palette,{human,mask},true)[0],"new mask instance seeds");
        }
        for(auto& bone:human.pose.bones)bone[3]=2;
        mask.signature=0x6787d559c31d313aull;
        mask.pose.bones[0]=human.pose.bones[11];palette.fill(Expand(mask.pose.bones[0]));
        MM3D::LinkInterpolation::History different_attachment;
        mask.pose.bones[0][3]=8;
        different_attachment.Apply(1,0,Identity(),palette,{human,mask},true);
        palette.fill(Expand(mask.pose.bones[0]));
        different_attachment.Apply(1,0,Identity(),palette,{human,mask},true);
        mask.pose.bones[0][3]=2;palette.fill(Expand(mask.pose.bones[0]));
        Check(!different_attachment.Apply(2,33,Identity(),palette,{human,mask},true)[0],"disagreeing mask history rejected");
    }
    {
        using namespace MM3D::LinkPoseMath;
        MM3D::LinkPoseCapture::LivePose fairy;
        fairy.signature=0x09e2df0cf6765f8eull;fairy.pose.instance=123;fairy.model=456;
        fairy.pose.bones.assign(5,Flatten(Identity()));
        fairy.bind.assign(5,Flatten(Identity()));fairy.words={0xffff0000,1,2,3,4};
        MM3D::LinkInterpolation::Palette palette;
        palette.fill(Identity());
        for(unsigned i=0;i<3;++i)palette[0][i][i]=14;
        MM3D::LinkInterpolation::History history;
        Check(!history.Apply(1,0,Identity(),palette,{fairy},true,4)[0],
              "fairy glow requires previous pose");
        const auto camera=Camera(20,4);
        auto world=Identity();world[0][3]=10;
        const auto current=Multiply(InverseRigid(camera),world);
        fairy.pose.bones.assign(5,Flatten(current));
        for(unsigned i=0;i<3;++i)palette[0][i][3]=current[i][3];
        const auto result=history.Apply(2,33,camera,palette,{fairy},true,4);
        auto midpoint=Identity();midpoint[0][3]=5;
        const auto expected=Multiply(InverseRigid(camera),midpoint);
        Check(result[0].has_value(),"fairy glow interpolates with moving camera");
        for(unsigned i=0;i<3;++i) {
            Check(std::abs((*result[0])[i][3]-expected[i][3])<.0001,
                  "fairy glow follows midpoint world position");
            for(unsigned j=0;j<3;++j)Check((*result[0])[i][j]==palette[0][i][j],
                  "fairy glow preserves native orientation and size");
        }
        for(unsigned i=1;i<10;++i)Check(!result[i],"glow never modifies unused bones");
        Check(!history.Apply(2,33,camera,palette,{fairy},false,4)[0],"comparison keeps native glow");
        auto frog=fairy;frog.signature=0xc0835328e6782411ull;
        Check(!MM3D::LinkInterpolation::FairyGlow(palette[0],frog,4),"frog cannot match fairy glow");
        Check(!MM3D::LinkInterpolation::FairyGlow(palette[0],fairy,24),"fairy wings excluded from glow path");
        auto other=fairy;other.pose.instance++;
        Check(!history.Apply(2,33,camera,palette,{fairy,other},true,4)[0],"ambiguous fairy instance rejected");
        auto bad=palette[0];bad[0][3]+=1;
        Check(!MM3D::LinkInterpolation::FairyGlow(bad,fairy,4),"nearby unrelated particle excluded");
        bad=palette[0];bad[0][1]=1;
        Check(!MM3D::LinkInterpolation::FairyGlow(bad,fairy,4),"nonbillboard geometry excluded");
        Check(!history.Apply(4,99,camera,palette,{fairy},true,4)[0],"glow frame gap resets history");
        fairy.generation++;
        Check(!history.Apply(5,132,camera,palette,{fairy},true,4)[0],"glow generation change resets history");
    }
    {
        using namespace MM3D::LinkPoseMath;
        MM3D::LinkPoseCapture::LivePose fairy;
        fairy.signature=0x09e2df0cf6765f8eull;fairy.pose.instance=123;fairy.model=456;
        fairy.bind.assign(5,Flatten(Identity()));fairy.words={0xffff0000,1,2,3,4};
        fairy.pose.bones.clear();
        for(unsigned i=0;i<5;++i)fairy.pose.bones.push_back(Flatten(Camera(0,i*3)));
        auto other=fairy;other.pose.instance++;
        MM3D::LinkInterpolation::Palette palette;palette.fill(Camera(0,3));
        MM3D::LinkInterpolation::History twins;
        Check(!twins.Apply(1,0,Identity(),palette,{fairy,other},true,24)[0],"duplicate fairy histories seed");
        for(unsigned i=0;i<5;++i)fairy.pose.bones[i]=Flatten(Camera(0,i*3+4));
        other.pose.bones=fairy.pose.bones;palette.fill(Camera(0,7));
        auto mid=twins.Apply(2,33,Identity(),palette,{fairy,other},true,24);
        Check(mid[0] && Near(*mid[0],Camera(0,5),1e-6),"identical fairy wings share agreed midpoint");
        Check(!twins.Apply(2,33,Identity(),palette,{fairy,other},false,24)[0],"disabled fairy twins stay native");
        other.pose.bones[4][3]+=1;
        Check(!twins.Apply(2,33,Identity(),palette,{fairy,other},true,24)[0],"different complete fairy poses rejected");
        MM3D::LinkInterpolation::History crossing;
        other.pose.bones=fairy.pose.bones;
        crossing.Apply(1,0,Identity(),palette,{fairy},true,24);
        for(auto& bone:other.pose.bones)bone[3]+=4;
        palette.fill(Camera(0,11));crossing.Apply(1,0,Identity(),palette,{other},true,24);
        other.pose.bones=fairy.pose.bones;palette.fill(Camera(0,7));
        Check(!crossing.Apply(2,33,Identity(),palette,{fairy,other},true,24)[0],
              "meeting fairies with different previous trajectories rejected");
    }
    {
        MM3D::GlareInterpolation h;
        MM3D::GlareInterpolation::Points a{},b{};
        b[0]={10,20,0};
        Check(!h.Apply(1,123,0,a),"glare first frame stays native");
        auto mid=h.Apply(2,123,0,b);
        Check(mid && (*mid)[0][0]==5 && (*mid)[0][1]==10,"glare positions interpolate");
        Check((*mid)[48]==b[48],"extra glare vertex stays native");
        Check(!h.Apply(4,123,0,a),"glare disappearance resets history");
        Check(!h.Apply(5,456,0,b),"glare texture changes reset history");
        Check(!h.Apply(6,456,1,a),"glare generation reset");
        b[0][2]=1;
        Check(!h.Apply(7,456,1,b),"changed glare geometry rejected");
        b[0][0]=500;
        Check(!h.Apply(8,456,1,b),"large glare jump rejected");
    }

    {
        SceneCopies copies;
        copies.Begin(10,0x1000);
        copies.Transfer(10,0x1000,0x2000);
        Check(copies.Contains(10,0x2000),"scene transfer preserves provenance");
        Check(!copies.Contains(11,0x2000),"previous frame copy rejected");
        copies.Transfer(10,0x3000,0x2000);
        Check(!copies.Contains(10,0x2000),"overwritten scene copy rejected");
        copies.Begin(11,0x4000);
        Check(!copies.Contains(11,0x1000),"new frame clears prior scene");
        Check(NormalizedSceneCopy(true,true,true,4,1,true,false),"normalized opaque scene copy accepted");
        Check(!NormalizedSceneCopy(false,true,true,4,1,true,false),"native shrink remains native");
        Check(!NormalizedSceneCopy(true,false,true,4,1,true,false),"unrelated texture rejected");
        Check(!NormalizedSceneCopy(true,true,false,4,1,true,false),"perspective geometry rejected");
        Check(!NormalizedSceneCopy(true,true,true,4,3,true,false),"multitexture effects rejected");
        Check(!NormalizedSceneCopy(true,true,true,4,1,false,false),"blended effects rejected");
        Check(!NormalizedSceneCopy(true,true,true,4,1,true,true),"stencilled effects rejected");
        Check(NormalizedSceneClear(true,true,true,4,0,true,false),"normalized solid scene clear candidate");
        Check(!NormalizedSceneClear(false,true,true,4,0,true,false),"ordinary clears cannot be omitted");
        Check(!NormalizedSceneClear(true,false,true,4,0,true,false),"unrelated framebuffer clear rejected");
        Check(!NormalizedSceneClear(true,true,false,4,0,true,false),"solid perspective geometry is not a clear");
        Check(!NormalizedSceneClear(true,true,true,6,0,true,false),"unverified clear geometry rejected");
        Check(!NormalizedSceneClear(true,true,true,4,1,true,false),"textured geometry is not a clear");
        Check(!NormalizedSceneClear(true,true,true,4,0,false,false),"alpha fades are not omitted");
        Check(!NormalizedSceneClear(true,true,true,4,0,true,true),"stencil masks are not omitted");

        SceneClearPair clear;
        Check(!clear.Observe(11,0x4000,true,false) && clear.Pending(11),
              "clear alone blocks paired presentation until its copy arrives");
        copies.Transfer(11,0x4000,0x5000);
        Check(!clear.Observe(11,0x4000,false,NormalizedSceneCopy(
                  true,copies.Contains(11,0x5000),true,4,1,true,false)) && !clear.Pending(11),
              "verified clear and scene copy preserve private midpoint");
        clear.Observe(12,0x4000,true,false);
        Check(clear.Observe(12,0x4000,false,false),"intervening HUD forces fallback");
        clear.Observe(13,0x4000,true,false);
        Check(clear.Observe(13,0x6000,false,true),"copy to different target cannot validate clear");
        clear.Observe(14,0x4000,true,false);
        Check(clear.Observe(14,0x4000,true,false) && clear.Pending(14),
              "second clear does not hide first unmatched clear");
        Check(!clear.Pending(15) && !clear.Observe(15,0x4000,false,true),
              "unmatched clear does not poison following frame");
        clear.Observe(16,0x4000,true,false);
        clear.Reset();
        Check(!clear.Pending(16),"mode or session reset discards pending clear");
    }

    {
        namespace L=MM3D::LinkPoseCapture;
        struct Memory {
            std::unordered_map<L::U32,L::U32> words;
            std::optional<L::U32> Read(L::U32 at) {
                const auto it=words.find(at);return it==words.end()?std::nullopt:std::optional(it->second);
            }
        } memory; // Intentionally no Write method: this hook must remain read-only.
        constexpr L::U32 instance=0x08000000,wrapper=instance+0x1000,data=instance+0x2000,out=instance+0x3000;
        memory.words[instance+0xc]=wrapper;memory.words[instance+0x24]=out;memory.words[wrapper]=data;
        memory.words[data]=0x206c6b73;memory.words[data+4]=1160;memory.words[data+8]=26;
        for(unsigned i=0;i<26;++i) {
            memory.words[data+16+44*i]=L::BoneWords[i];memory.words[data+56+44*i]=L::Hashes[i];
            for(unsigned j=0;j<12;++j)memory.words[out+i*48+j*4]=std::bit_cast<L::U32>(float(i*12+j));
        }
        const auto before=memory.words;
        L::Start();L::begins=L::Now();
        L::Hook hook;hook.Enter(memory,instance);
        Check(hook.current.instance==instance,"identify full human Link skeleton signature");
        for(unsigned i=0;i<25;++i)hook.MatrixBuilt(memory,instance,i,7);
        Check(L::pose_count==0,"incomplete pose is not published");
        hook.MatrixBuilt(memory,instance,25,7);
        Check(L::pose_count==1 && L::poses.back().bones[25][11]==311.f,"publish all 26 matched bones");
        Check(memory.words==before,"pose capture leaves guest memory unchanged");
        ++memory.words[data+56];hook.Enter(memory,instance);
        Check(!hook.current.instance,"different bone signature rejected");
        memory.words=before;hook.Enter(memory,instance);
        hook.MatrixBuilt(memory,instance,0,8);hook.MatrixBuilt(memory,instance,1,9);
        Check(!hook.current.instance && L::pose_count==1,"cross-frame partial pose rejected");
        hook.Enter(memory,instance);memory.words[out]=0x7fc00000;
        hook.MatrixBuilt(memory,instance,0,10);
        Check(!hook.current.instance,"nonfinite bone matrix rejected");
        L::active=false;hook.Enter(memory,instance);
        Check(!hook.current.instance,"capture disabled has no guest observation");
        // A generic three-bone actor exercises runtime topology and bind parsing.
        memory.words=before;
        memory.words[data+4]=16+44*3;memory.words[data+8]=3;
        for(unsigned i=0;i<3;++i) {
            memory.words[data+16+44*i]=i==0?0xffff0000:((i-1)<<16)|i;
            memory.words[data+56+44*i]=100+i;
            for(unsigned j=0;j<9;++j)
                memory.words[data+20+44*i+j*4]=std::bit_cast<L::U32>(j<3?1.f:0.f);
        }
        L::interpolate_actors=true;++L::interpolation_generation;
        hook.Enter(memory,instance);
        Check(hook.current.instance==instance && !hook.human,"generic skeleton accepted separately from human Link");
        hook.MatrixBuilt(memory,instance,0,20);hook.MatrixBuilt(memory,instance,0,20);
        hook.MatrixBuilt(memory,instance,1,20);
        Check(L::ForFrame(21).empty(),"duplicate bones cannot complete a partial actor pose");
        hook.MatrixBuilt(memory,instance,2,20);
        const auto generic=L::ForFrame(21);
        Check(generic.size()==1 && generic[0].words.size()==3,"complete runtime skeleton published on renderer frame plus one");
        Check(L::ForFrame(20).empty(),"actor pose uses correct renderer frame");
        ++L::interpolation_generation;
        Check(L::ForFrame(21).empty(),"toggle discards stale generic poses");
        memory.words[data+16+44]=0x10001;hook.Enter(memory,instance);
        Check(!hook.current.instance,"cyclic skeleton parent rejected");
        memory.words[data+8]=129;hook.Enter(memory,instance);
        Check(!hook.current.instance,"oversized skeleton rejected");
        L::interpolate_actors=false;L::live.clear();

    }
    {
        namespace L=MM3D::LinkPoseCapture;
        namespace I=MM3D::LinkInterpolation;
        const auto pose=[](double world_x,double camera_x,unsigned frame) {
            L::LivePose p{};p.model=99;p.pose.instance=42;p.pose.frame=frame;
            for(unsigned i=0;i<26;++i) {
                auto bone=Identity();for(unsigned j=0;j<3;++j)bone[j][j]=.01;
                bone[0][3]=world_x+10*i-camera_x;
                p.pose.bones[i]=MM3D::LinkPoseMath::Flatten(bone);
                p.bind[i]=MM3D::LinkPoseMath::Flatten(Identity());
            }
            return p;
        };
        const auto palette=[](const L::LivePose& p) {
            I::Palette values;for(unsigned i=0;i<10;++i)values[i]=MM3D::LinkPoseMath::Expand(p.pose.bones[i]);
            return values;
        };
        auto first=pose(0,0,0),second=pose(2,10,1);
        I::History h;
        const auto initial=h.Apply(1,0,Identity(),palette(first),{first},true);
        Check(!initial[0],"Link first pose seeds without inventing history");
        auto camera=Identity();camera[0][3]=10;
        const auto result=h.Apply(2,33,camera,palette(second),{second},true);
        Check(result[0].has_value(),"matched human Link pose interpolates");
        Check(std::abs((*result[0])[0][3]+9)<1e-8,"world-space Link midpoint avoids double camera motion");
        Check(std::abs((*result[0])[0][0]-.01)<1e-8,"Link model scale retained");
        Check(result[9] && std::abs((*result[9])[0][3]-81)<1e-8,"hierarchical child stays attached");
        Check(!h.Apply(2,33,camera,palette(second),{second},false)[0],"native comparison never interpolates Link");
        {
            auto equivalent=second;
            equivalent.pose.bones.resize(2);equivalent.bind.resize(2);
            auto base=Identity(),child=Identity(),bind=Identity();
            base[0][3]=10;child[0][3]=20;bind[0][3]=-10;
            equivalent.pose.bones[0]=MM3D::LinkPoseMath::Flatten(base);
            equivalent.pose.bones[1]=MM3D::LinkPoseMath::Flatten(child);
            equivalent.bind[0]=MM3D::LinkPoseMath::Flatten(Identity());
            equivalent.bind[1]=MM3D::LinkPoseMath::Flatten(bind);
            std::vector<Matrix> halves{base,child};
            halves[0][0][3]=5;halves[1][0][3]=15;
            Check(!I::Identify(base,equivalent),"equivalent bones remain ambiguous for actor identification");
            auto resolved=I::ResolvePalette(base,equivalent,halves,Identity());
            Check(resolved && (*resolved)[0][3]==5,"equivalent interpolated skin transforms accepted");
            halves[1][0][3]=16;
            Check(!I::ResolvePalette(base,equivalent,halves,Identity()),
                  "coincident current bones with different histories rejected");
        }
        I::History later_slots;
        auto later0=palette(first),later1=palette(second);
        later0[0][0][3]=later1[0][0][3]=999;
        later_slots.Apply(1,0,Identity(),later0,{first},true);
        const auto later=later_slots.Apply(2,33,camera,later1,{second},true);
        Check(!later[0] && later[1] && std::abs((*later[1])[0][3]-1)<1e-8,
              "later-slot actor bones interpolate without replacing unmatched slot zero");
        auto duplicate_later=second;duplicate_later.pose.instance=123;
        Check(!later_slots.Apply(2,33,camera,later1,{second,duplicate_later},true)[1],
              "later-slot ambiguity stays native");
        auto repeated=later1;
        for(unsigned i=1;i<10;++i)repeated[i]=later1[1];
        Check(!later_slots.Apply(2,33,camera,repeated,{second},true)[1],
              "repeated copies of one bone cannot identify an actor");
        auto other=second;other.pose.instance=43;
        Check(!h.Apply(2,33,camera,palette(second),{second,other},true)[0],"ambiguous model instances rejected");
        auto npc=palette(second);npc[0][0][3]=999;
        Check(!h.Apply(2,33,camera,npc,{second},true)[0],"unmatched NPC draw untouched");
        Check(!h.Apply(4,99,camera,palette(second),{second},true)[0],"missing Link frame breaks history");
        auto teleported=pose(1000,10,4);
        Check(!h.Apply(5,132,camera,palette(teleported),{teleported},true)[0],"Link teleport uses native pose");

        I::History crowd;
        auto a0=pose(0,0,0),b0=pose(1000,0,0);b0.pose.instance=77;
        auto a1=pose(2,0,1),b1=pose(1006,0,1);b1.pose.instance=77;
        // Use a different bone count and topology for the second actor.
        for(auto* p:{&b0,&b1}) {
            p->pose.bones.resize(3);p->bind.resize(3);p->words={0xffff0000,1,0x10002};
        }
        const auto short_palette=[](const L::LivePose& p) {
            I::Palette values;for(auto& m:values)m=Identity();
            for(unsigned i=0;i<p.pose.bones.size();++i)values[i]=MM3D::LinkPoseMath::Expand(p.pose.bones[i]);
            return values;
        };
        crowd.Apply(1,0,Identity(),palette(a0),{a0,b0},true);
        crowd.Apply(1,0,Identity(),short_palette(b0),{a0,b0},true);
        const auto ar=crowd.Apply(2,33,Identity(),palette(a1),{a1,b1},true);
        const auto br=crowd.Apply(2,33,Identity(),short_palette(b1),{a1,b1},true);
        Check(ar[0] && std::abs((*ar[0])[0][3]-1)<1e-8,"first actor retains its own history");
        Check(br[0] && std::abs((*br[0])[0][3]-1003)<1e-8,"second actor uses its own history and runtime skeleton");
        b1.signature=1;
        Check(!crowd.Apply(3,66,Identity(),short_palette(b1),{b1},true)[0],"reused model address with changed skeleton resets history");
        auto duplicate=a1;duplicate.pose.instance=99;
        Check(!crowd.Apply(3,66,Identity(),palette(a1),{a1,duplicate},true)[0],"overlapping identical actors are not guessed between");
        auto rotated=Identity();rotated[0][0]=rotated[1][1]=0;rotated[0][1]=-1;rotated[1][0]=1;
        const auto half=MM3D::LinkPoseMath::Half(Identity(),rotated);
        Check(half && std::abs((*half)[0][0]-std::sqrt(.5))<1e-8,"bone rotation uses quaternion midpoint");
        auto collapsed=Identity();collapsed[0][0]=0;
        Check(!MM3D::LinkPoseMath::Half(Identity(),collapsed),"collapsed bone does not produce invalid rotation");
    }
    Check(IsCompletedCapture(41,42),"draws precede completed-frame counter increment");
    Check(!IsCompletedCapture(42,42),"in-progress draw is not the completed frame");
    Check(!IsCompletedCapture(40,42),"stale captured scene is rejected");
    Check(!IsCompletedCapture({},42),"missing captured frame is rejected");
    Check(!IsCompletedCapture(0,0),"no completed frame at session start");
    PresentationPacer pacer;
    constexpr auto period=PresentationPacer::Period;
    Check(pacer.Deadline(0,true)==0,"first pair starts without extra buffering");
    Check(pacer.Deadline(5000000,true)==period,"submission work counts toward presentation interval");
    Check(pacer.Deadline(period+5000000,true)==2*period,"no added per-frame delay");
    Check(pacer.Deadline(10*period,true)==10*period,"stall resumes immediately without buffering again");
    Check(pacer.Deadline(10*period,false)==10*period,"native frame clears pacing");
    Check(pacer.Deadline(10*period+1,true)==10*period+1,"reenabling starts immediately");
    PresentationPacer late;
    late.Deadline(0,true);
    const auto arrival=period+5000000;
    Check(late.Deadline(arrival,true)==arrival,"late arrival receives no additional wait");
    Check(late.Deadline(arrival+1000000,true)==arrival+period,"late recovery avoids a short catch-up interval");
    PresentationClock clock;
    Check(clock.Next(1,0,true)==Presentation::Native,"first frame stays native");
    Check(clock.Next(2,33,true)==Presentation::Native,"wait for stable cadence");
    Check(clock.Next(3,66,true)==Presentation::Pair,"30 Hz scene emits midpoint then native");
    Check(clock.Next(3,82,true)==Presentation::Skip,"duplicate refresh cannot emit another pair");
    Check(clock.Next(4,99,true)==Presentation::Pair,"next game frame emits one pair");
    Check(clock.Next(5,115,true)==Presentation::Native,"native 60 Hz scene is not doubled");
    Check(clock.Next(6,131,true)==Presentation::Native,"60 Hz stays native");
    Check(clock.Next(7,300,true)==Presentation::Native,"stall resets cadence");
    Check(clock.Next(8,333,false)==Presentation::Native,"incomplete HUD or scene falls back");
    Check(clock.Next(9,366,true)==Presentation::Native,"recovery waits for stability");
    Check(clock.Next(10,399,true)==Presentation::Pair,"stable recovery resumes");
    Check(clock.Next(1,432,true)==Presentation::Native,"session rewind resets cadence");
    clock.Reset();
    Check(clock.Next(1,432,true)==Presentation::Native,"toggle reset restores native frame");
    const auto a=Camera(10,30),b=Camera(30,50);
    Check(Rigid(a)&&Near(Multiply(a,InverseRigid(a)),Identity(),1e-10),"rigid inverse");
    Check(Near(Midpoint(a,b),Camera(20,40),1e-10),"midpoint position and rotation");
    Check(Near(Midpoint(Camera(170),Camera(-170)),Camera(180),1e-10),"short path across rotation wrap");
    auto p=Identity();p[0]={0,1.4281474,0,0};p[1]={-.8568886,0,0,0};p[2]={0,0,1.00025,5.00125};p[3]={0,0,-1,0};
    History h;
    Check(!h.Observe(1,0,a,p),"first frame seeds history");
    const auto adjusted=h.Observe(2,33,b,p);
    Check(adjusted.has_value(),"consecutive frames produce midpoint projection");
    // Independent expected path: transform world point with the known halfway
    // camera, versus current view plus corrected projection. All clip components agree.
    const std::array<double,4> world{7,11,-200,1};
    const auto expected=Multiply(p,InverseRigid(Camera(20,40)));
    const auto actual=Multiply(*adjusted,InverseRigid(b));
    for(unsigned i=0;i<4;++i) {
        double x=0,y=0;for(unsigned j=0;j<4;++j){x+=expected[i][j]*world[j];y+=actual[i][j]*world[j];}
        Check(std::abs(x-y)<1e-9,"intermediate projection equals direct midpoint rendering");
    }
    Check(h.Observe(2,34,b,p).has_value(),"same frame reuses camera without advancing history");
    Check(!h.Observe(2,34,Camera(31,50),p),"another camera in same frame is excluded");
    Check(!h.Observe(4,66,b,p),"frame gap resets interpolation");
    Check(!h.Observe(5,300,b,p),"long stall resets interpolation");
    Check(!h.Observe(6,333,Camera(120,50),p),"large camera cut excluded");
    Check(!h.Observe(7,366,Camera(120,1000),p),"teleport excluded");
    auto different=p;different[0][1]*=2;
    Check(!h.Observe(8,399,Camera(120,1000),different),"projection change resets interpolation");
    Check(!h.Observe(0,420,b,p),"session rewind resets interpolation");
    auto bad=a;bad[0][0]=std::numeric_limits<double>::quiet_NaN();
    Check(!h.Observe(1,430,bad,p),"nonfinite camera rejected");
    bad=a;bad[0][0]*=2;Check(!Rigid(bad),"scaled transform is not a camera pose");
    bad=Identity();bad[0][0]=-1;Check(!Rigid(bad),"reflection rejected");
    Check(Near(Midpoint(a,a),a,1e-10),"stationary camera remains stationary");
    auto fade=MM3D::WidescreenProbe::TransitionOverlay;
    Check(TransitionFade(fade,Identity()),"verified orthographic fade accepted");
    Check(TransitionFade(fade,p),"exact perspective Z-target bars are a late overlay");
    fade.textures=1;
    Check(!TransitionFade(fade,p),"textured perspective particle is not a fade");
    fade.textures=0;
    ++fade.vertices;
    Check(!TransitionFade(fade,Identity()),"different geometry is not a fade");
    fade=MM3D::WidescreenProbe::TransitionOverlay;fade.textures=1;
    Check(!TransitionFade(fade,Identity()),"textured HUD is not a fade");
    fade=MM3D::WidescreenProbe::TransitionOverlay;fade.height=320;
    Check(!TransitionFade(fade,Identity()),"bottom-screen cover excluded");
    Check(WorldProjection(p),"world perspective accepted");
    Check(!WorldProjection(Identity()),"orthographic HUD excluded");
    auto sky=p;sky[2]={0,0,1.0000893,5.0004468};
    {
        auto dome=Identity();for(unsigned i=0;i<3;++i)dome[i][i]=320;
        Check(OpaqueSkyDome(288,sky,dome),"opaque sky dome is background geometry");
        Check(!OpaqueSkyDome(336,sky,dome),"unverified dome geometry excluded");
        auto foreground=dome;foreground[0][3]=1;
        Check(!OpaqueSkyDome(288,sky,foreground),"translated foreground is not sky");
        foreground=dome;foreground[0][0]=.15;
        Check(!OpaqueSkyDome(288,sky,foreground),"small foreground geometry is not sky");
        Check(!OpaqueSkyDome(288,Identity(),dome),"orthographic geometry is not sky");
    }
    Check(SameLens(p,sky),"sky clipping range shares world lens");
    History mixed;
    mixed.Observe(1,0,a,sky);
    const auto sky_mid=mixed.Observe(2,33,b,sky);
    const auto world_mid=mixed.Observe(2,33,b,p);
    Check(sky_mid && world_mid,"sky and world can share camera history");
    const auto delta=Multiply(InverseRigid(Midpoint(a,b)),b);
    Check(Near(*sky_mid,Multiply(sky,delta),1e-10),"sky retains own clipping planes");
    Check(Near(*world_mid,Multiply(p,delta),1e-10),"world retains own clipping planes");
    auto underwater=p;underwater[0][1]*=1.0151;underwater[1][0]*=1.04193;
    const auto underwater_mid=mixed.Observe(2,33,b,underwater);
    Check(underwater_mid && Near(*underwater_mid,Multiply(underwater,delta),1e-10),
          "underwater anisotropic lens retains world camera interpolation");
    auto bad_water=underwater;bad_water[0][1]*=1.2;
    Check(!mixed.Observe(2,33,b,bad_water),"large alternate lens rejected");
    bad_water=underwater;bad_water[0][2]=.01;
    Check(!mixed.Observe(2,33,b,bad_water),"skewed alternate lens rejected");
    bad_water=underwater;bad_water[2]=sky[2];
    Check(!mixed.Observe(2,33,b,bad_water),"same clip range cannot bypass lens guard");
    Check(!mixed.Observe(2,33,a,underwater),"alternate lens requires identical camera");
    Check(!mixed.Observe(2,33,b,Identity()),"alternate lens rejects orthographic UI");
    Check(mixed.Observe(3,66,a,p).has_value(),"different first draw range preserves history");
    {
        History water;
        auto before=p;before[0][1]=1.44587;before[1][0]=-.834802;
        auto after=before;after[0][1]=1.44838;after[1][0]=-.83559;
        Check(!GradualLensScale(before,after),"captured underwater wobble is not uniform zoom");
        Check(!water.Observe(1,0,a,before),"underwater history seeds natively");
        const auto result=water.Observe(2,33,b,after);
        auto half=after;half[0][1]=(before[0][1]+after[0][1])*.5;
        half[1][0]=(before[1][0]+after[1][0])*.5;
        Check(result && Near(*result,Multiply(half,delta),1e-9),
              "underwater camera and independent lens axes interpolate together");
        const auto repeat=water.Observe(2,33,b,after);
        Check(repeat && Near(*repeat,*result,1e-10),"same-frame underwater draws reuse midpoint");
        auto clipped=after;clipped[2]=sky[2];
        const auto alternate=water.Observe(2,33,b,clipped);
        half[2]=sky[2];
        Check(alternate && Near(*alternate,Multiply(half,delta),1e-9),
              "underwater clipping remains specific to each draw");
        Check(!water.Observe(4,99,b,after),"underwater gaps still require reacquisition");
        auto invalid=after;invalid[0][1]*=1.02;
        Check(!GradualWorldWobble(before,invalid),"abrupt aspect change excluded from wobble");
        invalid=after;invalid[1][0]=before[1][0];
        Check(!GradualWorldWobble(before,invalid),"single-axis resize excluded from wobble");
        invalid=after;invalid[0][2]=.01;
        Check(!GradualWorldWobble(before,invalid),"projection skew excluded from wobble");
        Check(!GradualWorldWobble(before,clipped),"cross-frame clipping changes excluded from wobble");
        invalid=after;invalid[1][0]=-invalid[1][0];
        Check(!GradualWorldWobble(before,invalid),"flipped projection excluded from wobble");
        invalid=after;invalid[0][1]=std::numeric_limits<double>::quiet_NaN();
        Check(!GradualWorldWobble(before,invalid),"nonfinite wobble excluded");
        Check(!GradualWorldWobble(Identity(),Identity()),"orthographic UI excluded from wobble");
        History cut;cut.Observe(1,0,a,before);
        Check(!cut.Observe(2,33,Camera(0,1000),after),"underwater camera cut still rejected");
        auto swimming=before;
        swimming[2]={0,0,1.0004167556762695,5.002084255218506};
        swimming[0][1]=1.360515;swimming[1][0]=-.819312;
        auto next=swimming;next[0][1]=1.426104;next[1][0]=-.820839;
        History strong;strong.Observe(1,0,a,swimming);
        const auto strong_mid=strong.Observe(2,33,b,next);
        auto strong_half=next;
        strong_half[0][1]=(swimming[0][1]+next[0][1])*.5;
        strong_half[1][0]=(swimming[1][0]+next[1][0])*.5;
        Check(strong_mid && Near(*strong_mid,Multiply(strong_half,delta),1e-9),
              "captured strong swimming distortion keeps camera and lens interpolation");
        auto other=swimming,other_next=next;
        other[2]=other_next[2]={0,0,1.0003907680511475,5.001953601837158};
        Check(!GradualWorldWobble(other,other_next),"strong wobble exception excludes normal scene projection");
        other_next=next;other_next[0][1]=swimming[0][1]*1.06;
        Check(!GradualWorldWobble(swimming,other_next),"strong underwater distortion remains bounded");
        other_next=next;other_next[1][0]=swimming[1][0];
        Check(!GradualWorldWobble(swimming,other_next),"strong underwater exception excludes single-axis resize");
    }
    auto ortho=Identity();
    Check(!SameLens(p,ortho),"orthographic lens is unrelated to world");
    History zoom;
    zoom.Observe(1,0,a,p);
    auto zoomed=p;zoomed[0][1]*=1.01;zoomed[1][0]*=1.01;
    const auto zoom_mid=zoom.Observe(2,33,b,zoomed);
    auto halfway_lens=p;halfway_lens[0][1]*=1.005;halfway_lens[1][0]*=1.005;
    Check(zoom_mid && Near(*zoom_mid,Multiply(halfway_lens,delta),1e-9),
          "gradual FOV zoom interpolates lens and camera together");
    auto zoom_sky=zoomed;zoom_sky[2]=sky[2];
    auto half_sky=halfway_lens;half_sky[2]=sky[2];
    const auto sky_zoom_mid=zoom.Observe(2,33,b,zoom_sky);
    Check(sky_zoom_mid && Near(*sky_zoom_mid,Multiply(half_sky,delta),1e-9),
          "zoom preserves each draw's near and far clipping planes");
    for(unsigned i=3;i<100;++i) {
        zoomed[0][1]*=.999;zoomed[1][0]*=.999;
        Check(zoom.Observe(i,i*33,b,zoomed).has_value(),"entry zoom does not restart history every frame");
    }
    auto sudden=zoomed;sudden[0][1]*=1.5;sudden[1][0]*=1.5;
    Check(!zoom.Observe(100,3300,b,sudden),"abrupt FOV cut still falls back");
    auto aspect=p;aspect[0][1]*=1.01;
    Check(!GradualLensScale(p,aspect),"aspect change is not mistaken for zoom");
    Check(!GradualLensScale(p,Identity()),"orthographic switch is not zoom");
    auto narrow=p;
    narrow[0][1]=3.;narrow[1][0]=-1.8;
    auto closer=narrow;closer[0][1]*=1.056;closer[1][0]*=1.056;
    Check(GradualLensScale(narrow,closer).has_value(),
          "steady narrow transformation zoom exceeding five percent is accepted");
    Check(GradualLensScale(closer,narrow).has_value(),"reverse narrow zoom is accepted");
    auto jump=narrow;jump[0][1]*=1.11;jump[1][0]*=1.11;
    Check(!GradualLensScale(narrow,jump),"relative cap still rejects narrow lens jumps");
    auto wide=p;wide[0][1]=1.;wide[1][0]=-.6;
    jump=wide;jump[0][1]*=1.056;jump[1][0]*=1.056;
    Check(!GradualLensScale(wide,jump),"larger angular lens change remains a cut");
    closer[1][0]*=1.01;
    Check(!GradualLensScale(narrow,closer),"narrow zoom cannot change aspect");
    // Room transitions can change projection or temporarily invalidate the CPU
    // snapshot for many frames. Keep displaying native geometry, without blending
    // cameras from different rooms, and resume midpoint rendering when stable.
    History transition;
    bool fallback=false;
    Check(Near(transition.Preview(1,0,a,p,true,false,fallback),p,0)&&fallback,
          "first visible scene renders immediately");
    Check(Near(transition.Preview(2,33,b,p,true,true,fallback),p,0)&&fallback,
          "comparison always uses native projection");
    Check(!Near(transition.Preview(3,66,a,p,true,false,fallback),p,1e-10)&&!fallback,
          "validated continuous camera interpolates");
    for(unsigned i=4;i<154;++i) {
        auto animated=p;animated[0][1]+=i*.001;
        Check(Near(transition.Preview(i,i*33,b,animated,true,false,fallback),animated,0)&&fallback,
              "changing transition projection never hides scene");
    }
    Check(Near(transition.Preview(154,5082,b,p,false,false,fallback),p,0)&&fallback,
          "unmatched CPU camera uses native projection");
    Check(Near(transition.Preview(155,5115,a,p,true,false,fallback),p,0)&&fallback,
          "reacquisition seeds fresh history");
    transition.Preview(156,5148,b,p,true,false,fallback);
    Check(!fallback,"midpoint resumes on next compatible frame");
    transition.Reset();
    Check(Near(transition.Preview(157,5181,a,p,true,false,fallback),p,0)&&fallback,
          "reset never reuses previous room camera");
    auto logo_camera=Identity();logo_camera[2][3]=30;
    Matrix logo_lens{};logo_lens[0][1]=2.390578508377075;logo_lens[1][0]=-1.43434739112854;
    logo_lens[2][2]=1.0003907680511475;logo_lens[2][3]=5.001953601837158;logo_lens[3][2]=-1;
    Check(TitleLogoCamera(logo_camera,logo_lens),"captured title camera recognized");
    Check(!TitleLogoCamera(a,logo_lens),"world camera excluded from title overlay");
    Check(!TitleLogoCamera(logo_camera,Identity()),"orthographic draw excluded from title camera");
    auto wrong_logo=logo_camera;wrong_logo[0][3]=1;
    Check(!TitleLogoCamera(wrong_logo,logo_lens),"translated world view excluded");
    History mixed_cameras;
    mixed_cameras.Preview(1,0,a,p,true,false,fallback);
    auto auxiliary=a;auxiliary[0][3]+=1000;
    Check(Near(mixed_cameras.Preview(1,1,auxiliary,p,false,false,fallback),p,0)&&fallback,
          "auxiliary camera remains native");
    mixed_cameras.Preview(2,33,b,p,true,false,fallback);
    Check(!fallback,"auxiliary draw does not erase previous validated world camera");
    mixed_cameras.Preview(2,34,auxiliary,p,false,false,fallback);
    Check(fallback,"rejected draw cannot inherit world interpolation");
    mixed_cameras.Preview(2,35,b,p,true,false,fallback);
    Check(!fallback,"world draw after auxiliary draw retains same-frame midpoint");
    mixed_cameras.Preview(3,66,auxiliary,p,false,false,fallback);
    mixed_cameras.Preview(4,99,a,p,true,false,fallback);
    Check(fallback,"whole rejected frame forces fresh acquisition");
    mixed_cameras.Preview(5,132,b,p,true,false,fallback);
    Check(!fallback,"interpolation resumes after two consecutive validated frames");
    mixed_cameras.Preview(5,133,auxiliary,p,false,false,fallback);
    mixed_cameras.Preview(6,165,auxiliary,p,true,false,fallback);
    Check(fallback,"validated camera cut still falls back after auxiliary draw");
    std::cout<<"PASS camera midpoint math, wrapped rotation, history and cut rejection\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
