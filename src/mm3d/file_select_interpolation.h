// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace MM3D::FileSelectInterpolation {
using U32=std::uint32_t;
using U64=std::uint64_t;
using Values=std::array<std::array<float,4>,96>;
using Key=std::array<U32,4>; // active texture, framebuffer, generation, native panel
using Rectangles=std::array<std::array<float,4>,32>;
struct Pose {
    unsigned count{}; std::array<float,16> x{},y{}; std::optional<float> alpha;
    std::optional<Rectangles> rectangles;
};
using Tev=std::array<std::array<U32,5>,6>;
inline std::optional<float> GlowAlpha(const Tev& tev) {
    for(unsigned i=0;i<6;++i) {
        const auto& t=tev[i];
        if(t[0]!=(i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu)||
           t[1]!=0||t[2]!=(i<2?0x10001u:0u)||t[4]!=0||
           (i==0?(t[3]&0xffffffu)!=0xffffffu:t[3]!=0xffffffffu))return {};
    }
    return float(tev[0][3]>>24)/255.f;
}
struct Sample {
    U64 frame{}; U32 generation{},owner{};
    std::array<float,3> slides{}; bool presentation{};
    std::array<float,3> entrance_slides{};
};
inline std::mutex sample_mutex;
inline std::vector<Sample> samples;
// Native layout preparation and GPU submission are asynchronous. Keep stamped
// ownership, including inactive samples, rather than a global menu flag.
inline void Publish(Sample sample) {
    std::scoped_lock lock(sample_mutex);
    if(!samples.empty()&&(sample.generation!=samples.back().generation||sample.frame<samples.back().frame))samples.clear();
    if(!samples.empty()&&sample.frame==samples.back().frame) {samples.back()=sample;return;}
    if(samples.size()==8)samples.erase(samples.begin());
    samples.push_back(sample);
}
inline U32 Owner(U64 frame,U32 generation) {
    std::scoped_lock lock(sample_mutex);
    for(auto it=samples.rbegin();it!=samples.rend();++it)
        if(it->generation==generation&&(it->frame==frame||it->frame+1==frame))return it->owner;
    return 0;
}
// Known native panel displacement disambiguates repeated glyphs sharing an
// atlas cell and baseline; their position alone must never select a match.
inline std::vector<float> Motion(U64 frame,U32 generation,U32 owner) {
    std::vector<float> result{0.f};
    std::scoped_lock lock(sample_mutex);
    for(unsigned i=1;i<samples.size();++i) {
        const auto& p=samples[i-1];const auto& n=samples[i];
        if(!owner||p.owner!=owner||n.owner!=owner||p.generation!=generation||n.generation!=generation||
           p.frame+1!=n.frame||(n.frame!=frame&&n.frame+1!=frame)||!p.presentation||!n.presentation)continue;
        for(unsigned q=0;q<n.slides.size();++q) {
            const float dx=n.slides[q]-p.slides[q];
            if(std::isfinite(dx)&&std::abs(dx)<=256)result.push_back(dx);
        }
        for(unsigned q=0;q<n.entrance_slides.size();++q) {
            const float dx=n.entrance_slides[q]-p.entrance_slides[q];
            if(std::isfinite(dx)&&std::abs(dx)<=256)result.push_back(dx);
        }
    }
    return result;
}
inline bool Layout(U64 shader,U64 swizzle,unsigned vertices) {
    return shader==0x9b343fea686d98e0ULL&&swizzle==0xac22b79e29209c01ULL&&
        vertices>0&&vertices<=96&&vertices%6==0;
}
// Credits batch up to thirteen 16x18 glyphs, sometimes from several lines.
// Register allocation changes when batches split: descriptor.w names a color
// register, not a fixed material ID. Geometry recognition is only a candidate;
// CreditsY below also requires the captured continuous upward scrolling rate.
inline bool CreditsLayout(const Values& u,unsigned vertices) {
    if(!vertices || vertices>78 || vertices%6)return false;
    const unsigned count=vertices/6,first_free=10+count*4;
    for(unsigned q=0;q<count;++q) {
        const unsigned row=10+q*4;
        const auto& desc=u[row];
        if(desc[0]!=1||desc[1]!=3||!std::isfinite(desc[2])||!std::isfinite(desc[3])||
           desc[2]<first_free||desc[2]>94||std::floor(desc[2])!=desc[2]||
           -desc[3]<first_free||-desc[3]>95||std::floor(desc[3])!=desc[3]||
           u[row+3]!=std::array<float,4>{0,1,0,0}||
           u[row+1][1]!=0||u[row+1][2]!=0||u[row+1][3]!=16||
           u[row+2][1]!=0||u[row+2][2]!=18||u[row+2][3]!=0)return false;
        const auto uv=unsigned(desc[2]),color=unsigned(-desc[3]);
        if(color==uv||color==uv+1||
           u[uv][0]!=0||u[uv][2]!=0||u[uv][3]!=.0625f||
           u[uv+1][0]!=0||u[uv+1][2]!=.0703125f||u[uv+1][3]!=0)return false;
        for(float v:u[color])if(!std::isfinite(v)||v<0||v>1)return false;
        if(!std::isfinite(u[row+1][0])||!std::isfinite(u[row+2][0]))return false;
    }
    return true;
}
inline bool Candidate(U64 shader,U64 swizzle,unsigned texture_mask,
                      bool depth_write,bool blend_enable,unsigned source,
                      unsigned destination,unsigned equation,unsigned vertices) {
    return Layout(shader,swizzle,vertices)&&texture_mask==1&&!depth_write&&blend_enable&&
        source==6&&destination==7&&equation==0;
}
enum class Channel { X,Y,Alpha,CreditsY };
class ChannelHistory {
    Channel channel;
    using Signature=std::array<float,66>;
    struct Entry { Key key; Signature signature; float x; };
    std::unordered_multimap<U64,Entry> previous,current;
    std::optional<U64> frame;
    std::optional<Signature> Identify(const Values& u,unsigned q,unsigned count) const {
        const unsigned row=10+q*4;
        const auto& desc=u[row];
        if((desc[0]!=0&&desc[0]!=1)||desc[1]!=3||!std::isfinite(desc[2])||
           desc[2]<float(10+4*count)||desc[2]>94||std::floor(desc[2])!=desc[2])return {};
        const auto uv=unsigned(desc[2]);
        if(channel==Channel::CreditsY && !CreditsLayout(u,count*6))return {};
        if(channel==Channel::Y) {
            // Captured SubMenuTitle strip, lower edge and the two title atlas
            // variants. Do not treat reflowed option-menu glyphs as moving text.
            const auto& a=u[uv];const auto& b=u[uv+1];
            const bool banner=(a==std::array<float,4>{0,.7890625f,0,0}&&b==std::array<float,4>{0,.1875f,.0625f,0})||
                (a==std::array<float,4>{0,0,0,.78125f}&&b==std::array<float,4>{0,0,.25f,0})||
                (a==std::array<float,4>{0,0,0,.5625f}&&b==std::array<float,4>{0,.71875f,.09375f,0})||
                (a==std::array<float,4>{0,.5625f,0,.28125f}&&b==std::array<float,4>{0,.4375f,.1875f,0});
            if(desc[0]!=0||desc[3]!=1||!banner)return {};
        }
        Signature s{};unsigned at=0;
        for(unsigned r=0;r<10;++r)for(float v:u[r])s[at++]=v;
        for(unsigned r=row+1;r<=row+3;++r)for(unsigned c=0;c<4;++c)
            if(c!=0||((channel!=Channel::X||r!=row+1)&&((channel!=Channel::Y&&channel!=Channel::CreditsY)||r!=row+2)))
                s[at++]=u[r][c];
        for(unsigned r=uv;r<uv+2;++r)for(float v:u[r])s[at++]=v;
        s[at++]=desc[0];s[at++]=desc[1];
        if(channel==Channel::CreditsY)for(float v:u[unsigned(-desc[3])])s[at++]=v;
        else s[at++]=desc[3];
        for(float v:s)if(!std::isfinite(v))return {};
        if(!std::isfinite(u[row+1][0])||std::abs(u[row+1][0])>4096||
           !std::isfinite(u[row+2][0])||std::abs(u[row+2][0])>4096)return {};
        return s;
    }
    static U64 Hash(const Signature& s,const Key& key) {
        U64 h=14695981039346656037ULL;
        for(auto k:key)h=(h^k)*1099511628211ULL;
        for(float v:s)h=(h^std::bit_cast<U32>(v==0?0.f:v))*1099511628211ULL;
        return h;
    }
public:
    explicit ChannelHistory(Channel c):channel(c) {}
    void Reset() {previous.clear();current.clear();frame.reset();}
    std::optional<Pose> Apply(U64 next,const Key& key,const Values& u,unsigned vertices,bool allow,
                              const std::vector<float>& motion={},std::optional<float> alpha={}) {
        if(!frame||next!=*frame) {
            if(frame&&next==*frame+1)previous=std::move(current);else previous.clear();
            current.clear();frame=next;
        }
        if(!key[3]||!vertices||vertices>96||vertices%6||
           (channel==Channel::Alpha&&(!alpha||!std::isfinite(*alpha)||*alpha<0||*alpha>1)))return {};
        Pose pose{};pose.count=vertices/6;bool changed=false,complete=true;
        for(unsigned q=0;q<pose.count;++q) {
            const float x=channel==Channel::Alpha?*alpha:u[(channel==Channel::X?11:12)+q*4][0];pose.x[q]=x;
            const auto signature=Identify(u,q,pose.count);if(!signature){complete=false;continue;}
            const auto hash=Hash(*signature,key);
            std::optional<float> old;bool ambiguous=false;
            const auto [begin,end]=previous.equal_range(hash);
            for(auto it=begin;it!=end;++it) {
                const auto& e=it->second;
                if(e.key!=key||e.signature!=*signature)continue;
                // Do not join repeated letters on separate lines or animate
                // text reflow. Only the measured credits scroll is admitted.
                if(channel==Channel::CreditsY&&std::abs((x-e.x)+.6171875f)>.002f)continue;
                if(old&&std::abs(*old-e.x)>.001f)ambiguous=true;
                old=e.x;
            }
            if(ambiguous&&!motion.empty()) {
                old.reset();ambiguous=false;
                for(auto it=begin;it!=end;++it) {
                    const auto& e=it->second;
                    if(e.key!=key||e.signature!=*signature)continue;
                    bool matched=false;
                    for(float dx:motion)matched|=std::isfinite(dx)&&std::abs(x-e.x-dx)<.02f;
                    if(!matched)continue;
                    if(old&&std::abs(*old-e.x)>.001f)ambiguous=true;
                    old=e.x;
                }
            }
            if(old&&!ambiguous&&std::abs(x-*old)<=(channel==Channel::Alpha?.15f:256.f)) {
                pose.x[q]=(x+*old)*.5f;changed|=x!=*old;
            } else complete=false;
            if(current.size()<2048)current.emplace(hash,Entry{key,*signature,x});
        }
        if(channel==Channel::Alpha) {
            // One combiner constant affects the entire draw. Never apply a
            // cursor fade to unrelated quads merged into its batch at full alpha.
            for(unsigned q=1;q<pose.count;++q)complete&=std::abs(pose.x[q]-pose.x[0])<.00001f;
            if(!complete)return {};
        }
        return allow&&changed?std::optional<Pose>{pose}:std::nullopt;
    }
};
// PopupWindow draws a complete nine-slice plate in one batch. Its corners,
// edges and center grow together about a fixed layout pivot while the combiner fades.
class PopupHistory {
    struct Entry {U64 frame;Values u;float alpha;std::optional<Pose> half;bool ambiguous{};};
    std::map<Key,Entry> entries;
    static bool Near(float a,float b,float eps=.0001f) {
        return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=eps;
    }
public:
    static bool Recognized(const Values& u,unsigned vertices) {
        if(vertices!=54)return false;
        for(unsigned r=0;r<10;++r)for(float x:u[r])if(!std::isfinite(x))return false;
        for(unsigned q=0;q<9;++q) {
            const unsigned r=10+4*q,uv=94-2*q,col=q%3,row=q/3;
            if(u[r]!=std::array<float,4>{0,3,float(uv),1}||u[r+3]!=std::array<float,4>{0,1,0,0})return false;
            const std::array<float,4> a{0,col==2?.578125f:.640625f,0,col==1?0.f:.0625f};
            const std::array<float,4> b{0,row==2?.875f:.9375f,row==1?0.f:.0625f,0};
            for(unsigned c=0;c<4;++c)if(!Near(u[uv][c],a[c],.000001f)||!Near(u[uv+1][c],b[c],.000001f))return false;
            for(unsigned k=r+1;k<r+3;++k)for(float x:u[k])if(!std::isfinite(x))return false;
            if(u[r+1][1]!=0||u[r+1][2]!=0||u[r+2][1]!=0||u[r+2][3]!=0||
               u[r+1][3]<=0||u[r+2][2]<=0)return false;
            // Shared rows/columns and seamless edges distinguish the plate
            // from unrelated rectangles using the same UI atlas.
            if(!Near(u[r+1][0],u[11+4*col][0],.001f)||!Near(u[r+1][3],u[11+4*col][3],.001f)||
               !Near(u[r+2][0],u[12+12*row][0],.001f)||!Near(u[r+2][2],u[12+12*row][2],.001f))return false;
        }
        const float border=u[11][3];
        return border>.1f&&border<40&&Near(border,u[19][3],.001f)&&Near(border,u[12][2],.001f)&&
            Near(border,u[36][2],.001f)&&Near(u[19][0]+u[19][3],u[15][0],.001f)&&
            Near(u[15][0]+u[15][3],u[11][0],.001f)&&Near(u[36][0]+u[36][2],u[24][0],.001f)&&
            Near(u[24][0]+u[24][2],u[12][0],.001f);
    }
    void Reset(){entries.clear();}
    std::optional<Pose> Apply(U64 frame,const Key& key,const Values& u,unsigned vertices,bool allow,std::optional<float> alpha) {
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.frame>frame||frame-it->second.frame>1)it=entries.erase(it);else ++it;
        if(!key[3]||!alpha||!std::isfinite(*alpha)||*alpha<0||*alpha>1||!Recognized(u,vertices))return {};
        auto it=entries.find(key);
        if(it==entries.end()) {if(entries.size()<16)entries.emplace(key,Entry{frame,u,*alpha,{}});return {};}
        auto& e=it->second;
        if(e.frame==frame) {
            if(e.u!=u||e.alpha!=*alpha)e.ambiguous=true;
            return allow&&!e.ambiguous?e.half:std::nullopt;
        }
        bool valid=!e.ambiguous&&e.frame+1==frame;
        for(unsigned r=0;r<10;++r)valid&=e.u[r]==u[r];
        // Keep the layout pivot and unscaled aspect fixed; allow the captured growth,
        // small overshoot and close, but reject menu repositioning or replacement.
        const auto cx=[](const Values& v){return (v[19][0]+v[11][0]+v[11][3])*.5f;};
        const auto cy=[](const Values& v){return (v[36][0]+v[12][0]+v[12][2])*.5f-.75f*v[11][3];};
        valid&=Near(cx(e.u),cx(u),.05f)&&Near(cy(e.u),cy(u),.05f)&&
            std::abs(e.u[11][3]-u[11][3])<=5&&std::abs(e.alpha-*alpha)<=.4f;
        const float ratio=u[11][3]/e.u[11][3];
        for(unsigned q=0;q<9;++q) {
            valid&=Near(u[11+4*q][3],e.u[11+4*q][3]*ratio,.005f)&&
                Near(u[12+4*q][2],e.u[12+4*q][2]*ratio,.005f);
        }
        std::optional<Pose> half;
        if(valid) {
            Pose p{};p.count=9;p.rectangles.emplace();bool changed=e.alpha!=*alpha;
            for(unsigned q=0;q<9;++q)for(unsigned row=0;row<2;++row)for(unsigned c=0;c<4;++c) {
                const unsigned r=11+4*q+row;
                (*p.rectangles)[q*2+row][c]=(e.u[r][c]+u[r][c])*.5f;
                changed|=e.u[r][c]!=u[r][c];
            }
            for(unsigned q=0;q<9;++q){p.x[q]=(*p.rectangles)[q*2][0];p.y[q]=(*p.rectangles)[q*2+1][0];}
            p.alpha=(e.alpha+*alpha)*.5f;
            if(changed)half=p;
        }
        e={frame,u,*alpha,half};return allow?half:std::nullopt;
    }
};
// Independent axes keep a shared baseline for horizontal text and a shared
// column for vertical text, without weakening the existing slide matching.
class History {
    ChannelHistory horizontal{Channel::X},vertical{Channel::Y},glow{Channel::Alpha};
    PopupHistory popup;
    ChannelHistory credits{Channel::CreditsY};
public:
    void Reset() {horizontal.Reset();vertical.Reset();glow.Reset();popup.Reset();credits.Reset();}
    std::optional<Pose> ApplyCredits(U64 next,const Key& key,const Values& u,unsigned vertices,bool allow) {
        if(!CreditsLayout(u,vertices))return {};
        const auto y=credits.Apply(next,key,u,vertices,allow);
        if(!y)return {};
        Pose result{};result.count=vertices/6;
        for(unsigned q=0;q<result.count;++q) {
            result.x[q]=u[11+q*4][0];result.y[q]=y->x[q];
        }
        return result;
    }
    std::optional<Pose> Apply(U64 next,const Key& key,const Values& u,unsigned vertices,bool allow,
                              const std::vector<float>& motion={},std::optional<float> alpha={}) {
        const auto x=horizontal.Apply(next,key,u,vertices,allow,motion);
        const auto y=vertical.Apply(next,key,u,vertices,allow);
        const auto a=glow.Apply(next,key,u,vertices,allow,{},alpha);
        if(auto p=popup.Apply(next,key,u,vertices,allow,alpha))return p;
        if(!x&&!y&&!a)return {};
        Pose result{};result.count=vertices/6;
        for(unsigned q=0;q<result.count;++q) {
            result.x[q]=x?x->x[q]:u[11+q*4][0];
            result.y[q]=y?y->x[q]:u[12+q*4][0];
        }
        if(a)result.alpha=a->x[0];
        return result;
    }
};
}
