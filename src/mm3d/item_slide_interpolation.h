// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace MM3D::ItemSlideInterpolation {
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using Values = std::array<std::array<float,4>,96>;
// Preserve the original six-frame linear transition; presentation supplies
// only the intermediate positions, without changing guest animation timing.
inline float Offset(bool opening,float time,float travel) {
    const float progress=time/6.f;
    return -travel*(opening?1.f-progress:progress);
}
struct Sample {
    U64 frame{};
    U32 generation{}, owner{};
    float x{}, time{}, travel{};
    bool active{}, opening{};
};
struct Delta {float native{}, half{};};
inline std::optional<Delta> Between(const Sample& p,const Sample& n) {
    if(!p.owner||p.owner!=n.owner||p.generation!=n.generation||p.frame+1!=n.frame||!p.active)
        return {};
    const bool finishing=p.opening&&!n.active&&n.x==0;
    if(!finishing&&(!n.active||p.opening!=n.opening||p.travel!=n.travel))return {};
    const float time=finishing?6.f:n.time;
    if(!std::isfinite(time)||!std::isfinite(p.time)||!std::isfinite(p.travel)||
       !std::isfinite(p.x)||!std::isfinite(n.x)||p.travel<=0||p.travel>2048||
       p.time<0||time>6||time<=p.time||time-p.time>2.01f)return {};
    const float dx=n.x-p.x;
    if(std::abs(dx)<.001f)return {};
    return Delta{dx,Offset(p.opening,(p.time+time)*.5f,p.travel)-n.x};
}
// Native layout preparation precedes asynchronous GPU submission. Retain stamped
// samples rather than a global "currently drawing this menu" flag.
inline std::mutex sample_mutex;
inline std::vector<Sample> samples;
inline void Publish(Sample sample) {
    std::scoped_lock lock(sample_mutex);
    if(!samples.empty()&&(sample.generation!=samples.back().generation||sample.frame<samples.back().frame))samples.clear();
    if(!samples.empty()&&sample.frame==samples.back().frame) {samples.back()=sample;return;}
    if(samples.size()==8)samples.erase(samples.begin());
    samples.push_back(sample);
}
inline bool HasRecentPopup(U64 frame,U32 generation) {
    std::scoped_lock lock(sample_mutex);
    for(const auto& sample:samples)
        if(sample.owner&&sample.generation==generation&&
           (sample.frame==frame||sample.frame+1==frame))return true;
    return false;
}
inline std::vector<Delta> Recent(U64 frame,U32 generation) {
    std::vector<Delta> result;
    std::scoped_lock lock(sample_mutex);
    for(unsigned i=1;i<samples.size();++i) {
        const auto& n=samples[i];
        // Guest layout work can belong to the upcoming submitted GPU frame.
        if(n.generation!=generation||(n.frame!=frame&&n.frame+1!=frame))continue;
        if(auto delta=Between(samples[i-1],n))result.push_back(*delta);
    }
    return result;
}
inline bool Candidate(U64 shader,U64 swizzle,unsigned vertices) {
    return shader==0x9b343fea686d98e0ULL&&swizzle==0xac22b79e29209c01ULL&&
        vertices>0&&vertices<=96&&vertices%6==0;
}
struct Pose {unsigned count{};std::array<float,16> x{};};
class History {
    using Signature=std::array<float,53>;
    struct Entry {std::array<U32,3> key;Signature signature;float x;};
    std::unordered_multimap<U64,Entry> previous,current;
    std::optional<U64> frame;
    static std::optional<Signature> Identify(const Values& u,unsigned q,unsigned count) {
        const unsigned row=10+q*4;
        const auto& desc=u[row];
        if((desc[0]!=0&&desc[0]!=1)||desc[1]!=3||!std::isfinite(desc[2])||
           desc[2]<float(10+4*count)||desc[2]>94||std::floor(desc[2])!=desc[2])return {};
        const auto uv=unsigned(desc[2]);
        Signature s{};unsigned at=0;
        for(unsigned r=0;r<8;++r)for(float v:u[r])s[at++]=v;
        for(unsigned r=row+1;r<=row+3;++r)for(unsigned c=0;c<4;++c)
            if(r!=row+1||c!=0)s[at++]=u[r][c];
        for(unsigned r=uv;r<uv+2;++r)for(float v:u[r])s[at++]=v;
        s[at++]=desc[0];s[at++]=desc[1];
        for(float v:s)if(!std::isfinite(v))return {};
        if(!std::isfinite(u[row+1][0])||std::abs(u[row+1][0])>4096)return {};
        return s;
    }
    static U64 Hash(const Signature& s,const std::array<U32,3>& key) {
        U64 h=14695981039346656037ULL;
        for(auto k:key)h=(h^k)*1099511628211ULL;
        for(float v:s)h=(h^std::bit_cast<U32>(v==0?0.f:v))*1099511628211ULL;
        return h;
    }
public:
    void Reset() {previous.clear();current.clear();frame.reset();}
    std::optional<Pose> Apply(U64 next,const std::array<U32,3>& key,const Values& u,
                              unsigned vertices,const std::vector<Delta>& deltas) {
        if(!frame||next!=*frame) {
            if(frame&&next==*frame+1)previous=std::move(current);else previous.clear();
            current.clear();frame=next;
        }
        if(!vertices||vertices>96||vertices%6)return {};
        Pose pose{};pose.count=vertices/6;bool changed=false;
        for(unsigned q=0;q<pose.count;++q) {
            const unsigned row=11+q*4;
            const float x=u[row][0];pose.x[q]=x;
            const auto signature=Identify(u,q,pose.count);if(!signature)continue;
            const auto hash=Hash(*signature,key);
            std::optional<float> correction;bool ambiguous=false;
            const auto [begin,end]=previous.equal_range(hash);
            for(auto it=begin;it!=end;++it) {
                const auto& entry=it->second;
                if(entry.key!=key||entry.signature!=*signature)continue;
                for(const auto& delta:deltas) {
                    if(!std::isfinite(delta.native)||!std::isfinite(delta.half)||
                       std::abs((x-entry.x)-delta.native)>.02f)continue;
                    if(correction&&std::abs(*correction-delta.half)>.02f)ambiguous=true;
                    correction=delta.half;
                }
            }
            if(correction&&!ambiguous) {pose.x[q]+=*correction;changed=true;}
            if(current.size()<2048)current.emplace(hash,Entry{key,*signature,x});
        }
        return changed?std::optional<Pose>(pose):std::nullopt;
    }
};
} // namespace MM3D::ItemSlideInterpolation
