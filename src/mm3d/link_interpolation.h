// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/link_pose_capture.h"
#include <unordered_map>
#include <cstring>
#include <limits>
namespace MM3D::LinkInterpolation {
using namespace LinkPoseMath;
using Palette=std::array<Matrix,10>;
using Overrides=std::array<std::optional<Matrix>,10>;
inline constexpr std::array<unsigned,5> PipeBones{2,3,5,6,4};
// Local actor/archive correlation identifies the five-bone `chat` fairy model.
// Its two glow quads share the root position, but use a scaled view-facing basis.
inline bool FairyGlow(const Matrix& matrix,const LinkPoseCapture::LivePose& pose,
                      unsigned vertices) {
    if(vertices!=4 || pose.signature!=0x09e2df0cf6765f8eull ||
       pose.pose.bones.size()!=5 || !Near(matrix,matrix,0))return false;
    const double scale=matrix[0][0];
    if(scale<=0)return false;
    for(unsigned row=0;row<3;++row) {
        if(std::abs(matrix[row][3]-pose.pose.bones[0][row*4+3])>.03)return false;
        for(unsigned col=0;col<3;++col)
            if(std::abs(matrix[row][col]-(row==col?scale:0))>scale*.0002)return false;
    }
    return true;
}
struct Match { unsigned bone{}; bool skin{}; bool operator==(const Match&)const=default; };
inline std::optional<Match> Identify(const Matrix& matrix,const LinkPoseCapture::LivePose& pose) {
    if(!Inverse(matrix))return {};
    std::optional<Match> match;
    for(unsigned bone=0;bone<pose.pose.bones.size();++bone) {
        const auto direct=Expand(pose.pose.bones[bone]);
        for(bool skin : {false,true}) {
            const auto candidate=skin?Multiply(direct,Expand(pose.bind[bone])):direct;
            if(!PaletteNear(matrix,candidate))continue;
            if(match) {
                // Identity binds don't create two different bone identities.
                if(match->bone==bone && Near(Expand(pose.bind[bone]),Identity(),1e-5))continue;
                return {};
            }
            match=Match{bone,skin};
        }
    }
    return match;
}
// Precompute current direct/skin candidates once per pose instead of multiplying
// every bone by its bind for every material and unrelated scene draw. Snapshots
// also detect same-frame pose/bind changes; this cache carries no animation state.
class BoneCandidates {
    std::vector<LinkPoseCapture::Matrix> bones,bind;
    std::vector<std::array<Matrix,2>> candidates;
    std::vector<bool> identity_bind;
    std::array<double,3> low{},high{};
public:
    void Prepare(const LinkPoseCapture::LivePose& pose) {
        const auto same=[](const auto& a,const auto& b) {
            return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);
        };
        if(same(bones,pose.pose.bones) && same(bind,pose.bind))return;
        bones=pose.pose.bones;bind=pose.bind;
        candidates.clear();identity_bind.clear();
        low.fill(std::numeric_limits<double>::infinity());
        high.fill(-std::numeric_limits<double>::infinity());
        if(bones.size()!=bind.size())return;
        candidates.reserve(bones.size());identity_bind.reserve(bones.size());
        for(unsigned i=0;i<bones.size();++i) {
            const auto direct=Expand(bones[i]),binding=Expand(bind[i]);
            candidates.push_back({direct,Multiply(direct,binding)});
            for(const auto& candidate:candidates.back())for(unsigned axis=0;axis<3;++axis) {
                const auto value=candidate[axis][3];
                if(std::isfinite(value)) {low[axis]=std::min(low[axis],value);high[axis]=std::max(high[axis],value);}
            }
            identity_bind.push_back(Near(binding,Identity(),1e-5));
        }
    }
    std::optional<Match> Identify(const Matrix& matrix) const {
        // Conservative bounds over every direct and skin candidate. Use the
        // original subtraction/tolerance so boundary rounding cannot reject a
        // match that the reference matcher accepts.
        for(unsigned axis=0;axis<3;++axis) {
            const auto value=matrix[axis][3];
            if(!std::isfinite(value) || low[axis]-value>.03 || value-high[axis]>.03)return {};
        }
        if(!Inverse(matrix))return {};
        std::optional<Match> match;
        for(unsigned bone=0;bone<candidates.size();++bone)for(bool skin:{false,true}) {
            const auto& candidate=candidates[bone][skin];
            // Most draws belong to other actors. Reject distant translations
            // before touching the basis, using the same positional tolerance.
            if(std::abs(matrix[0][3]-candidate[0][3])>.03 ||
               std::abs(matrix[1][3]-candidate[1][3])>.03 ||
               std::abs(matrix[2][3]-candidate[2][3])>.03 ||
               !PaletteNear(matrix,candidate))continue;
            if(match) {
                if(match->bone==bone && identity_bind[bone])continue;
                return {};
            }
            match=Match{bone,skin};
        }
        return match;
    }
};
// Once the actor is uniquely known, equivalent skin bones may name the same
// palette transform. Accept them only if their interpolated results also agree.
inline std::optional<Matrix> ResolvePalette(const Matrix& matrix,
        const LinkPoseCapture::LivePose& pose,const std::vector<Matrix>& half,const Matrix& view) {
    if(!Inverse(matrix) || half.size()!=pose.pose.bones.size())return {};
    std::optional<Matrix> result;
    for(unsigned bone=0;bone<pose.pose.bones.size();++bone)for(bool skin:{false,true}) {
        auto candidate=Expand(pose.pose.bones[bone]);
        if(skin)candidate=Multiply(candidate,Expand(pose.bind[bone]));
        if(!PaletteNear(matrix,candidate))continue;
        auto value=Multiply(view,half[bone]);
        if(skin)value=Multiply(value,Expand(pose.bind[bone]));
        if(!Near(value,value,0) || (result && !PaletteNear(*result,value)))return {};
        result=value;
    }
    return result;
}
// Like Like's scale-compensated joints produce local shear. For small changes,
// affine averaging preserves that shear without treating it as a rigid rotation.
// Restrict this fallback to the captured skeleton and bounded relative motion.
inline std::optional<Matrix> LikeLikeHalf(const Matrix& a,const Matrix& b,
                                        const LinkPoseCapture::LivePose& pose) {
    if(pose.signature!=0xe5524c5eb3c8c915ull || pose.pose.bones.size()!=5 ||
       pose.words.size()!=5 || pose.bind.size()!=5)return {};
    const auto inverse=Inverse(a);
    if(!inverse || !Inverse(b))return {};
    const auto relative=Multiply(*inverse,b);
    for(unsigned i=0;i<4;++i)
        if(a[3][i]!=(i==3?1.:0.) || b[3][i]!=(i==3?1.:0.))return {};
    for(unsigned i=0;i<3;++i) {
        if(std::abs(relative[i][3])>200)return {};
        for(unsigned j=0;j<3;++j)
            if(std::abs(relative[i][j]-(i==j?1.:0.))>.2)return {};
    }
    auto result=Identity();
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j)result[i][j]=(a[i][j]+b[i][j])*.5;
    return Inverse(result)?std::optional<Matrix>(result):std::nullopt;
}
// ChuChu death wobble skews its single model matrix; column-normalized rotation
// interpolation cannot represent it. Average small affine steps only for the
// captured slime materials (this root skeleton signature is shared by props).
inline std::optional<Matrix> SlimeHalf(const Matrix& a,const Matrix& b,
                                     const LinkPoseCapture::LivePose& pose,unsigned vertices) {
    if(pose.signature!=0xf0c54784cce31d97ull || pose.pose.bones.size()!=1 ||
       pose.bind.size()!=1 || pose.words.size()!=1 || pose.words[0]!=0xffff0000 ||
       !Near(Expand(pose.bind[0]),Identity(),1e-5) ||
       (vertices!=18 && vertices!=444 && vertices!=612))return {};
    const auto ai=Inverse(a),bi=Inverse(b);
    if(!ai || !bi)return {};
    for(unsigned i=0;i<4;++i)
        if(a[3][i]!=(i==3?1.:0.) || b[3][i]!=(i==3?1.:0.))return {};
    double movement=0;
    for(unsigned i=0;i<3;++i)movement+=std::pow(a[i][3]-b[i][3],2);
    if(movement>=10000)return {};
    // Check both directions so expansion and contraction have identical guards.
    // A basis change this close to identity cannot flip or collapse through zero.
    for(const auto& relative:{Multiply(*ai,b),Multiply(*bi,a)}) {
        double change=0;
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)
            change+=std::pow(relative[i][j]-(i==j?1.:0.),2);
        if(!std::isfinite(change) || change>.25)return {};
    }
    auto result=Identity();
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j)result[i][j]=(a[i][j]+b[i][j])*.5;
    return Inverse(result)?std::optional<Matrix>(result):std::nullopt;
}
// Extended fins are attached to the forearms after skeleton evaluation. Their
// stored parent remains bone 23, so interpolating that hierarchy disagrees with
// the arm's hierarchy even when both current matrices are identical.
inline std::optional<Matrix> ExtendedZoraFin(const Matrix& matrix,
        const LinkPoseCapture::LivePose& pose,const std::vector<Matrix>& half,
        const Matrix& view,unsigned vertices) {
    if(vertices!=120 || pose.signature!=0x42c4c515d83599dbull ||
       pose.pose.bones.size()!=28 || half.size()!=28)return {};
    std::optional<Matrix> result;
    for(const auto pair:{std::array<unsigned,2>{16,24},std::array<unsigned,2>{20,25}}) {
        if(!PaletteNear(matrix,Expand(pose.pose.bones[pair[0]])) ||
           !PaletteNear(matrix,Expand(pose.pose.bones[pair[1]])))continue;
        const auto arm=Multiply(view,half[pair[0]]);
        if(!Near(arm,arm,0) || (result && !PaletteNear(*result,arm)))return {};
        result=arm;
    }
    return result;
}
class ActorHistory {
    std::uint64_t frame{};
    std::int64_t time{};
    unsigned generation{};
    std::uint32_t instance{},model{};
    bool seen{},ready{};
    std::vector<Matrix> current,half;
    std::uint64_t signature{};
    Matrix view_camera{};
public:
    void Reset() { seen=ready=false; }
    Overrides Apply(std::uint64_t next,std::int64_t now,const Matrix& camera,
                    const Palette& palette,const LinkPoseCapture::LivePose& selected,
                    bool allow,bool fairy_glow=false,bool deku_pipes=false,unsigned vertices=0,std::optional<Matrix> attached_root={}) {
        Overrides result{};
        const auto* pose=&selected;
        if(!pose || !Rigid(camera))return result;
        if(!seen || next!=frame) {
            const auto old=current;
            current.resize(pose->pose.bones.size());half.resize(current.size());
            bool continuous=seen && next==frame+1 && now>=time && now-time<=200 &&
                instance==pose->pose.instance && model==pose->model && generation==pose->generation && signature==pose->signature && old.size()==current.size();
            for(unsigned i=0;i<current.size();++i)current[i]=Multiply(camera,Expand(pose->pose.bones[i]));
            if(continuous) {
                double movement=0;for(unsigned i=0;i<3;++i)movement+=std::pow(old[0][i][3]-current[0][i][3],2);
                continuous=movement<10000;
            }
            ready=continuous;
            if(ready)for(unsigned i=0;i<current.size();++i) {
                if(i==0 && attached_root) {half[i]=*attached_root;continue;}
                const auto parent=pose->words[i]>>16;
                auto a=old[i],b=current[i];
                if(parent!=65535) {
                    const auto ai=Inverse(old[parent]),bi=Inverse(current[parent]);
                    if(!ai||!bi){half[i]=current[i];continue;}
                    a=Multiply(*ai,a);b=Multiply(*bi,b);
                }
                // Zora combat fins switch between .4 and 1 local scale.
                // Permit that captured transition only on their two bones.
                const bool zora_fin=pose->signature==0x42c4c515d83599dbull &&
                    current.size()==28 && (i==24 || i==25);
                auto local=Half(a,b,zora_fin?2.51:2.);
                if(!local)local=LikeLikeHalf(a,b,*pose);
                if(!local)local=SlimeHalf(a,b,*pose,vertices);
                half[i]=local?(parent==65535?*local:Multiply(half[parent],*local)):current[i];
            }
            frame=next;time=now;instance=pose->pose.instance;model=pose->model;
            generation=pose->generation;signature=pose->signature;view_camera=camera;seen=true;
        }
        if(!allow || !ready || instance!=pose->pose.instance || model!=pose->model ||
           generation!=pose->generation || signature!=pose->signature ||
           !Near(view_camera,camera,.001))return result;
        const auto view=InverseRigid(camera);
        if(fairy_glow) {
            // Keep the native billboard orientation, size and pulsing. Only its
            // center follows the same interpolated root used by the wings.
            auto glow=palette[0];
            const auto root=Multiply(view,half[0]);
            for(unsigned i=0;i<3;++i)glow[i][3]=root[i][3];
            result[0]=glow;return result;
        }
        for(unsigned slot=0;slot<10;++slot)
            result[slot]=ResolvePalette(palette[slot],*pose,half,view);
        if(const auto fin=ExtendedZoraFin(palette[0],*pose,half,view,vertices))result[0]=fin;
        if(deku_pipes)for(unsigned slot=1;slot<=PipeBones.size();++slot) {
            const auto bone=PipeBones[slot-1];
            const auto bind=Expand(pose->bind[bone]);
            if(PaletteNear(palette[slot],Multiply(Expand(pose->pose.bones[bone]),bind)))
                result[slot]=Multiply(Multiply(view,half[bone]),bind);
        }
        return result;
    }
};
// One bounded history per skeleton instance, never shared between actors.
class History {
    struct Entry { ActorHistory history; std::uint64_t frame{}; };
    std::unordered_map<std::uint32_t,Entry> actors;
    struct CandidateEntry { BoneCandidates candidates;std::uint64_t frame{},query{};const LinkPoseCapture::LivePose* source{}; };
    std::uint64_t query_serial{};
    std::unordered_map<std::uint32_t,CandidateEntry> candidate_cache;
    std::optional<std::uint64_t> cleanup_frame;
    std::optional<Match> IdentifyCached(std::uint64_t frame,const Matrix& matrix,
                                      const LinkPoseCapture::LivePose& pose) {
        if(!candidate_cache.contains(pose.pose.instance) && candidate_cache.size()>=128)
            return Identify(matrix,pose);
        auto& entry=candidate_cache[pose.pose.instance];entry.frame=frame;
        if(entry.query!=query_serial || entry.source!=&pose) {
            entry.candidates.Prepare(pose);entry.query=query_serial;entry.source=&pose;
        }
        return entry.candidates.Identify(matrix);
    }
    std::array<std::uint32_t,6> head_attachment{};
    std::uint64_t head_attachment_frame{};
    bool head_attachment_seen{},head_attachment_ready{};
    struct Attachment {
        std::array<std::uint32_t,6> key{};
        std::uint64_t frame{};
        bool seen{},ready{};
    };
    std::unordered_map<std::uint32_t,Attachment> actor_attachments;
    Overrides ApplyKnown(std::uint64_t frame,std::int64_t now,const Matrix& camera,
                         const Palette& palette,const LinkPoseCapture::LivePose& pose,
                         bool allow,bool glow=false,bool pipes=false,unsigned vertices=0,std::optional<Matrix> attached_root={}) {
        if(!actors.contains(pose.pose.instance) && actors.size()>=128)return {};
        auto& entry=actors[pose.pose.instance];entry.frame=frame;
        return entry.history.Apply(frame,now,camera,palette,pose,allow,glow,pipes,vertices,attached_root);
    }
public:
    void Reset() { actors.clear();candidate_cache.clear();actor_attachments.clear();cleanup_frame.reset();head_attachment_seen=head_attachment_ready=false; }
    Overrides Apply(std::uint64_t frame,std::int64_t now,const Matrix& camera,
                    const Palette& palette,const std::vector<LinkPoseCapture::LivePose>& poses,
                    bool allow,unsigned vertices=0) {
        ++query_serial;
        if(cleanup_frame!=frame) {
            const auto expired=[&](const auto& pair){return pair.second.frame>frame || frame-pair.second.frame>2;};
            std::erase_if(actors,expired);std::erase_if(candidate_cache,expired);
            std::erase_if(actor_attachments,expired);cleanup_frame=frame;
        }
        const LinkPoseCapture::LivePose* found=nullptr;
        bool fairy_glow=false;
        std::vector<const LinkPoseCapture::LivePose*> owners;
        for(const auto& pose:poses) {
            if(pose.bind.size()!=5 || pose.words.size()!=5 ||
               !FairyGlow(palette[0],pose,vertices))continue;
            owners.push_back(&pose);fairy_glow=true;
        }
        if(!fairy_glow)
        for(const auto& pose:poses) {
            if(pose.pose.bones.empty() || pose.pose.bones.size()!=pose.bind.size() ||
               pose.words.size()!=pose.bind.size())continue;
            if(IdentifyCached(frame,palette[0],pose))owners.push_back(&pose);
        }
        // The opening cutscene submits two identical Tatl skeleton instances.
        // Only accept their shared draw if both complete current poses AND their
        // independently tracked midpoint palettes agree. Never pick an arbitrary
        // fairy when two different trajectories meet.
        if(owners.size()>1 && std::all_of(owners.begin(),owners.end(),[&](const auto* p) {
            if(p->signature!=0x09e2df0cf6765f8eull || p->pose.bones.size()!=5 ||
               p->bind.size()!=5 || p->words!=owners[0]->words)return false;
            for(unsigned i=0;i<5;++i)
                if(!Near(Expand(p->pose.bones[i]),Expand(owners[0]->pose.bones[i]),1e-5) ||
                   !Near(Expand(p->bind[i]),Expand(owners[0]->bind[i]),1e-5))return false;
            return true;
        })) {
            Overrides consensus{};bool first=true,agree=true;
            for(const auto* pose:owners) {
                const auto candidate=ApplyKnown(frame,now,camera,palette,*pose,allow,fairy_glow,false,vertices);
                if(first) {consensus=candidate;first=false;continue;}
                for(unsigned i=0;i<10;++i)
                    if(bool(candidate[i])!=bool(consensus[i]) ||
                       (candidate[i] && !PaletteNear(*candidate[i],*consensus[i])))agree=false;
            }
            return agree?consensus:Overrides{};
        }
        if(fairy_glow) {
            if(owners.size()!=1)return {};
            found=owners[0];owners.clear();
        }
        // Deku pipes share their base with Link, but animate their own branches.
        // Validate the captured draw and its ordered skin palette before selecting it.
        const LinkPoseCapture::LivePose* pipes=nullptr;
        if(!fairy_glow && vertices==1284)for(const auto& pose:poses) {
            if(pose.signature!=0x79937db4f6cb5c95ull || pose.pose.bones.size()!=7 ||
               pose.bind.size()!=7 || pose.words.size()!=7)continue;
            std::vector<Matrix> current;
            for(const auto& bone:pose.pose.bones)current.push_back(Expand(bone));
            if(!ResolvePalette(palette[0],pose,current,Identity()))continue;
            bool matches=true;
            for(unsigned slot=1;slot<=PipeBones.size();++slot) {
                const auto bone=PipeBones[slot-1];
                matches &= PaletteNear(palette[slot],Multiply(current[bone],Expand(pose.bind[bone])));
            }
            if(!matches)continue;
            if(pipes)return {};
            pipes=&pose;
        }
        if(pipes) {
            if(owners.size()>1)return {};
            if(!owners.empty() && (owners[0]->signature!=0x17beb877e4d23506ull ||
                                   owners[0]->pose.bones.size()!=24))return {};
            auto result=ApplyKnown(frame,now,camera,palette,*pipes,allow,false,true);
            if(!owners.empty()) {
                const auto& link=*owners[0];
                const auto shared=ApplyKnown(frame,now,camera,palette,link,allow);
                // Only the base is shared. A resting opening may happen to
                // equal Link's transform but retains its explicit bone identity.
                if(result[0] && (!shared[0] || !PaletteNear(*result[0],*shared[0])))
                    result[0].reset();
            }
            return result;
        }
        if(owners.size()>1 && owners.size()<=10) {
            constexpr std::array<std::uint64_t,10> sign_fragments{
                0x63f1ea87bac6ed15ull,0x237eacb5846225e3ull,0xa5ab84c08cb4c10cull,
                0x45e68157044301aeull,0x3cc2d5b83770f56aull,0x957491d08c5433edull,
                0x15e80cb6d1b86f59ull,0xe0695fdbeb7af70bull,0x8c3894096e7beb07ull,
                0xe6f89274e4c3a586ull};
            bool verified=true;
            for(unsigned i=0;i<owners.size();++i) {
                const auto& p=*owners[i];
                verified &= p.pose.bones.size()==1 &&
                    std::find(sign_fragments.begin(),sign_fragments.end(),p.signature)!=sign_fragments.end();
                for(unsigned j=0;j<i;++j)verified &= p.signature!=owners[j]->signature &&
                    p.model!=owners[j]->model && p.pose.instance!=owners[j]->pose.instance;
            }
            if(verified) {
                auto result=ApplyKnown(frame,now,camera,palette,*owners[0],allow);
                for(unsigned i=1;i<owners.size();++i) {
                    const auto other=ApplyKnown(frame,now,camera,palette,*owners[i],allow);
                    for(unsigned slot=0;slot<result.size();++slot)
                        if(!result[slot] || !other[slot] || !PaletteNear(*result[slot],*other[slot]))result[slot].reset();
                }
                return result;
            }
        }
        if(owners.size()>1) {
            // Verified attachments/paired marker passes: never pick an arbitrary owner.
            // A shared slot is usable only when both independent histories agree.
            if(owners.size()!=2)return {};
            // Captured rigid accessories inherit their parent's bone verbatim.
            // Use that bone's hierarchy midpoint: independently interpolating
            // the attached model's root would detach it during joint rotation.
            struct AttachedModel {
                std::uint64_t body,item;
                unsigned bones,parent;
                std::array<unsigned,2> draws;
            };
            static constexpr std::array<AttachedModel,3> attached_models{{
                {0xbf2f38bb6502de36ull,0x10aa2e239eaea424ull,15,14,{204,96}}, // Postman cap
                {0xbf2f38bb6502de36ull,0x61a2e0e2eed80dccull,15,7,{414,414}}, // Postman bag
                {0x228b499bc4c2bef7ull,0xa81ec0d047c2eaadull,20,8,{36,36}}, // Carpenter wood
            }};
            for(unsigned index=0;index<2;++index) {
                const auto& body=*owners[index];const auto& item=*owners[1-index];
                const auto model=std::find_if(attached_models.begin(),attached_models.end(),[&](const auto& m) {
                    return body.signature==m.body && item.signature==m.item &&
                        (vertices==m.draws[0] || vertices==m.draws[1]);
                });
                if(model==attached_models.end() || body.pose.bones.size()!=model->bones ||
                   body.bind.size()!=model->bones || body.words.size()!=model->bones ||
                   item.pose.bones.size()!=1 || item.bind.size()!=1 || item.words.size()!=1 ||
                   item.words[0]!=0xffff0000 || !Near(Expand(item.bind[0]),Identity(),1e-5) ||
                   body.pose.instance==item.pose.instance || body.model==item.model)continue;
                const auto parent=Expand(body.pose.bones[model->parent]);
                if(!PaletteNear(palette[0],parent) ||
                   !PaletteNear(Expand(item.pose.bones[0]),parent))continue;
                if(!actor_attachments.contains(item.pose.instance) &&
                   actor_attachments.size()>=128)return {};
                auto& attachment=actor_attachments[item.pose.instance];
                const std::array<std::uint32_t,6> key{body.pose.instance,body.model,body.generation,
                    item.pose.instance,item.model,item.generation};
                if(!attachment.seen || attachment.frame!=frame || attachment.key!=key) {
                    attachment.ready=attachment.seen && attachment.key==key &&
                        frame==attachment.frame+1;
                    attachment.key=key;attachment.frame=frame;attachment.seen=true;
                }
                return ApplyKnown(frame,now,camera,palette,body,allow && attachment.ready);
            }
            const auto mask=[](const auto& p) {
                return p.pose.bones.size()==1 &&
                    (p.signature==0x6787d559c31d313aull || p.signature==0xbee0d7a2ff0e9481ull);
            };
            const auto human=[](const auto& p) {
                return p.words.size()==LinkPoseCapture::BoneWords.size() &&
                    std::equal(p.words.begin(),p.words.end(),LinkPoseCapture::BoneWords.begin());
            };
            // Worn masks inherit the human head transform. Blend
            // through the head's actual hierarchy, not a separate root rotation.
            // Require the same attachment on consecutive frames; newly equipped
            // masks and actor swaps must seed rather than borrow old history.
            for(unsigned index=0;index<2;++index) {
                const auto& link=*owners[index];const auto& item=*owners[1-index];
                const bool animated_mask=(item.signature==0x2090ecc88f377de1ull && item.pose.bones.size()==7) ||
                    (item.signature==0x15ad1b7410d5bd8dull && item.pose.bones.size()==3);
                const auto count=item.pose.bones.size();
                if(!human(link) || link.pose.bones.size()!=26 || (count!=1 && !animated_mask) ||
                   item.bind.size()!=count || item.words.size()!=count ||
                   link.pose.instance==item.pose.instance || link.model==item.model ||
                   !PaletteNear(palette[0],Expand(link.pose.bones[11])) ||
                   !PaletteNear(Multiply(Expand(item.pose.bones[0]),Expand(item.bind[0])),
                                Expand(link.pose.bones[11])))continue;
                const std::array<std::uint32_t,6> key{link.pose.instance,link.model,link.generation,
                    item.pose.instance,item.model,item.generation};
                if(!head_attachment_seen || head_attachment_frame!=frame || head_attachment!=key) {
                    head_attachment_ready=head_attachment_seen && head_attachment==key &&
                        frame==head_attachment_frame+1;
                    head_attachment=key;head_attachment_frame=frame;head_attachment_seen=true;
                }
                auto result=ApplyKnown(frame,now,camera,palette,link,allow && head_attachment_ready);
                if(!animated_mask)return result;
                // The mask root uses a fixed bind offset from the head. Seed
                // its hierarchy even on the first attachment frame, then use
                // the head midpoint as the parent of its animated children.
                std::optional<Matrix> root;
                const auto offset=Inverse(Expand(item.bind[0]));
                if(result[0] && offset)root=Multiply(Multiply(camera,*result[0]),*offset);
                return ApplyKnown(frame,now,camera,palette,item,
                    allow && head_attachment_ready && root.has_value(),false,false,vertices,root);
            }
            const auto marker=[](const auto& p) {
                // Paired target/mark models and their depth models, locally verified.
                return p.pose.bones.size()==1 &&
                    (p.signature==0x578e07cd3309c81aull || p.signature==0x018a783e5ef2d2bdull);
            };
            const bool marker_pair=marker(*owners[0]) && marker(*owners[1]) &&
                owners[0]->signature==owners[1]->signature &&
                owners[0]->pose.instance!=owners[1]->pose.instance && owners[0]->model!=owners[1]->model;
            if(!(marker_pair || (mask(*owners[0])&&human(*owners[1])) ||
                 (mask(*owners[1])&&human(*owners[0]))))return {};
            auto a=ApplyKnown(frame,now,camera,palette,*owners[0],allow);
            const auto b=ApplyKnown(frame,now,camera,palette,*owners[1],allow);
            for(unsigned i=0;i<a.size();++i)
                if(!a[i] || !b[i] || !PaletteNear(*a[i],*b[i]))a[i].reset();
            return a;
        }
        if(owners.size()==1)found=owners[0];
        if(!found)for(const auto& pose:poses) {
            // Verified effects, pickups, Bomber and intro trees have equivalent bones.
            // ResolvePalette still rejects them if their midpoint poses diverge.
            const unsigned count=pose.signature==0xdfe0c7f5a2531807ull ? 4 :
                pose.signature==0xdf35e5b3de06d85eull ? 7 :
                pose.signature==0xce5abb274d304de4ull ? 3 :
                pose.signature==0x69e723ba0b961e6bull ? 4 :
                pose.signature==0xc299a4754ffae267ull ? 17 :
                pose.signature==0x04e3fb346228fd20ull ? 4 :
                pose.signature==0xd56b42acae52759full ? 7 :
                pose.signature==0x7526de611fb49ce3ull ? 8 :
                pose.signature==0x3ed3e32e1ee38993ull ? 3 :
                pose.signature==0x6735aa3cacc26ca6ull ? 3 : // Zora electric barrier
                pose.signature==0xe26d2bb9ac2e87b6ull ? 3 : // Intro forest trees
                pose.signature==0x76a7e789464b75d3ull ? 2 :
                pose.signature==0xe40ebd58d8b46adaull ? 2 :
                pose.signature==0x2507dd32b95b7c14ull ? 24 : 0;
            if(!count || pose.pose.bones.size()!=count ||
               pose.bind.size()!=count || pose.words.size()!=count)continue;
            std::vector<Matrix> current;
            for(const auto& bone:pose.pose.bones)current.push_back(Expand(bone));
            if(ResolvePalette(palette[0],pose,current,Identity())) {
                if(found)return {};
                found=&pose;
            }
        }
        if(!found) {
            // Some actor materials leave slot zero unmatched, while their
            // active bones occupy later slots. Require three distinct bones
            // and a unique instance; stale or ambiguous palettes stay native.
            for(const auto& pose:poses) {
                if(pose.pose.bones.empty() || pose.pose.bones.size()!=pose.bind.size() ||
                   pose.words.size()!=pose.bind.size())continue;
                std::vector<unsigned> bones;
                for(unsigned slot=1;slot<palette.size();++slot)
                    if(const auto match=IdentifyCached(frame,palette[slot],pose))
                        if(std::find(bones.begin(),bones.end(),match->bone)==bones.end())
                            bones.push_back(match->bone);
                if(bones.size()>=3){if(found)return {};found=&pose;}
            }
        }
        if(!found)return {};
        return ApplyKnown(frame,now,camera,palette,*found,allow,fairy_glow,false,vertices);
    }
};

}
