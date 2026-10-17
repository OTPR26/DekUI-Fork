// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/file_select_interpolation.h"

namespace MM3D::InventoryInterpolation {
using FileSelectInterpolation::U32;
using FileSelectInterpolation::U64;
using FileSelectInterpolation::Key;
using FileSelectInterpolation::Values;
using FileSelectInterpolation::Tev;
// Separate lifetime from file select and Give Item. An inactive stamp prevents
// the last inventory page from lending ownership to ordinary gameplay HUD.
inline std::mutex mutex;
struct OwnerSample { U64 frame; U32 generation,owner,world_map; };
inline std::vector<OwnerSample> owners;
inline void Publish(U64 frame,U32 generation,U32 owner,U32 world_map=0) {
    std::scoped_lock lock(mutex);
    if(!owners.empty()&&(frame<owners.back().frame||generation!=owners.back().generation))owners.clear();
    if(!owners.empty()&&owners.back().frame==frame)owners.pop_back();
    if(owners.size()==8)owners.erase(owners.begin());
    owners.push_back({frame,generation,owner,world_map});
}
inline U32 Owner(U64 frame,U32 generation,bool world_map_only=false) {
    std::scoped_lock lock(mutex);
    for(auto it=owners.rbegin();it!=owners.rend();++it)
        if(it->generation==generation&&(it->frame==frame||it->frame+1==frame))
            return world_map_only?it->world_map:it->owner;
    return 0;
}
struct Pose {Values values{};std::array<bool,96> rows{};float alpha{};};
class History {
    using Signature=std::vector<U32>;
    struct Entry {Pose pose;bool ambiguous=false;};
    std::map<Signature,Entry> previous,current;
    std::map<Signature,Entry> previous_shadows,current_shadows;
    std::optional<U64> frame;
    static std::optional<Signature> Identify(const Key& key,const Values& u,unsigned vertices,
                                            const Tev& tev,Pose& pose) {
        if(!key[3]||!vertices||vertices>96||vertices%6)return {};
        Signature s(key.begin(),key.end());s.push_back(vertices);
        const auto add=[&](float v){s.push_back(std::bit_cast<U32>(v==0?0.f:v));};
        // Captured layout combiner: retain tint and shadow/font modifiers as
        // identity; only stage-zero opacity is animated. Never blend textures.
        for(unsigned i=0;i<6;++i) {
            auto t=tev[i];
            if(t[0]!=(i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu)||
               t[2]!=(i<2?0x10001u:0u)||t[4]||
               (i==1?(t[1]!=0&&t[1]!=32):t[1]!=0)||
               (i&&t[3]!=0xffffffffu))return {};
            if(!i)t[3]&=0xffffffu;
            s.insert(s.end(),t.begin(),t.end());
        }
        pose.values=u;pose.alpha=float(tev[0][3]>>24)/255.f;
        for(unsigned r=0;r<10;++r)for(float v:u[r]) {
            if(!std::isfinite(v))return {};
            // f8 is inherited scratch state, not an input to this layout
            // shader's accepted descriptor paths. Its change at menu close
            // must not discard otherwise consecutive widget transforms.
            if(r!=8)add(v);
        }
        const unsigned count=vertices/6,free=10+4*count;
        std::array<bool,96> uv_rows{};
        for(unsigned q=0;q<count;++q) {
            const unsigned r=10+q*4;const auto& desc=u[r];
            if((desc[0]!=0&&desc[0]!=1)||desc[1]!=3||!std::isfinite(desc[2])||
               desc[2]<free||desc[2]>94||std::floor(desc[2])!=desc[2]||
               !std::isfinite(desc[3]))return {};
            add(desc[0]);add(desc[1]);
            const unsigned uv=unsigned(desc[2]);uv_rows[uv]=uv_rows[uv+1]=true;
            for(unsigned k=uv;k<uv+2;++k)for(float v:u[k]) {
                if(!std::isfinite(v))return {};
                add(v);
            }
            // Font color registers move when the native batch splits. Match
            // their contents, not their register number, and interpolate alpha.
            if(desc[3]<0) {
                if(-desc[3]<free||-desc[3]>95||std::floor(desc[3])!=desc[3])return {};
                const unsigned color=unsigned(-desc[3]);
                for(float v:u[color])if(!std::isfinite(v)||v<0||v>1)return {};
                add(-1);for(unsigned c=0;c<3;++c)add(u[color][c]);
                pose.rows[color]=true;
            } else {if(desc[3]!=1)return {};add(1);}
            for(unsigned k=r+1;k<r+4;++k) {
                for(float v:u[k])if(!std::isfinite(v)||std::abs(v)>4096)return {};
                pose.rows[k]=true;
            }
        }
        for(unsigned r=free;r<96;++r)if(uv_rows[r]&&pose.rows[r])return {};
        return s;
    }
    static bool Same(const Pose& a,const Pose& b) {
        if(a.alpha!=b.alpha||a.rows!=b.rows)return false;
        for(unsigned r=0;r<96;++r)if(a.rows[r]&&a.values[r]!=b.values[r])return false;
        return true;
    }
    static bool HorizontalShadow(const Pose& a,const Pose& b,unsigned vertices) {
        // Repeated black strips use different fixed opacities. Only resolve
        // those ties when every quad makes the same horizontal translation;
        // preserve vertical placement, scale, rotation and font color.
        if(a.alpha!=b.alpha||a.rows!=b.rows)return false;
        const float dx=b.values[11][0]-a.values[11][0];
        for(unsigned r=0;r<96;++r)if(a.rows[r])for(unsigned c=0;c<4;++c) {
            const bool x=c==0&&r>=11&&r<10+4*(vertices/6)&&(r-11)%4==0;
            if(std::abs(b.values[r][c]-a.values[r][c]-(x?dx:0.f))>.0001f)return false;
        }
        return true;
    }
public:
    void Reset(){previous.clear();current.clear();previous_shadows.clear();current_shadows.clear();frame.reset();}
    std::optional<Pose> Apply(U64 next,const Key& key,const Values& u,unsigned vertices,const Tev& tev) {
        if(!frame||next!=*frame) {
            if(frame&&next==*frame+1) {
                previous=std::move(current);previous_shadows=std::move(current_shadows);
            } else {previous.clear();previous_shadows.clear();}
            current.clear();current_shadows.clear();frame=next;
        }
        Pose native;auto signature=Identify(key,u,vertices,tev,native);
        if(!signature)return {};
        if(current.size()>=512)return {};
        auto [stored,inserted]=current.emplace(*signature,Entry{native});
        if(!inserted&&!Same(stored->second.pose,native))stored->second.ambiguous=true;
        // Retain an opacity-specific identity for black shadow batches. Do not
        // use it for ordinary unique batches, whose opacity can interpolate.
        std::optional<Signature> shadow_signature;
        if((tev[0][3]&0xffffffu)==0) {
            if(current_shadows.size()>=512)return {};
            shadow_signature=*signature;shadow_signature->push_back(tev[0][3]>>24);
            auto [shadow,added]=current_shadows.emplace(*shadow_signature,Entry{native});
            if(!added&&!Same(shadow->second.pose,native))shadow->second.ambiguous=true;
        }
        const auto old=previous.find(*signature);
        if(old==previous.end())return {};
        const Pose* prior=&old->second.pose;
        if(old->second.ambiguous||stored->second.ambiguous) {
            if(!shadow_signature)return {};
            const auto shadow=previous_shadows.find(*shadow_signature);
            if(shadow==previous_shadows.end()||shadow->second.ambiguous||
               current_shadows.at(*shadow_signature).ambiguous||
               !HorizontalShadow(shadow->second.pose,native,vertices))return {};
            prior=&shadow->second.pose;
        }
        const auto& p=*prior;
        if(p.rows!=native.rows||std::abs(p.alpha-native.alpha)>.5f)return {};
        Pose half=native;bool changed=p.alpha!=native.alpha;
        half.alpha=(p.alpha+native.alpha)*.5f;
        for(unsigned r=0;r<96;++r)if(native.rows[r])for(unsigned c=0;c<4;++c) {
            const float a=p.values[r][c],b=native.values[r][c];
            if(std::abs(a-b)>192)return {}; // teleports/reflow are not motion
            half.values[r][c]=(a+b)*.5f;changed|=a!=b;
        }
        return changed?std::optional<Pose>{half}:std::nullopt;
    }
};
} // namespace MM3D::InventoryInterpolation
