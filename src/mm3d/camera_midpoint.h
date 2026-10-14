// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include "mm3d/widescreen_probe.h"

namespace MM3D::CameraMidpoint {
using Matrix = std::array<std::array<double,4>,4>;
using Quaternion = std::array<double,4>; // w,x,y,z
inline std::atomic<bool> enabled{false}, native_preview{false}, present_60{false};
inline std::atomic<bool> suppress_transform_feedback{false};
inline std::atomic<std::uint64_t> capture_pair_at_us{};
inline std::atomic<unsigned> generation{0}, copied_draws{0}, rejected_draws{0}, fallback_draws{0}, presented_pairs{0};
inline Matrix Identity() { Matrix m{}; for(unsigned i=0;i<4;++i)m[i][i]=1; return m; }
inline Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix r{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)r[i][j]+=a[i][k]*b[k][j];
    return r;
}
inline bool Near(const Matrix& a,const Matrix& b,double epsilon) {
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        if(!std::isfinite(a[i][j])||!std::isfinite(b[i][j])||std::abs(a[i][j]-b[i][j])>epsilon)return false;
    return true;
}
// Captured title-logo camera: a separate fixed view, not the world camera.
inline bool TitleLogoCamera(const Matrix& camera,const Matrix& projection) {
    auto expected=Identity();expected[2][3]=30.;
    Matrix lens{};lens[0][1]=2.390578508377075;lens[1][0]=-1.43434739112854;
    lens[2][2]=1.0003907680511475;lens[2][3]=5.001953601837158;lens[3][2]=-1.;
    return Near(camera,expected,.0001) && Near(projection,lens,.0001);
}
inline bool Rigid(const Matrix& m) {
    for(const auto& row:m)for(double v:row)if(!std::isfinite(v))return false;
    for(unsigned i=0;i<4;++i)if(std::abs(m[3][i]-(i==3?1.:0.))>1e-6)return false;
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j) {
        double dot=0;for(unsigned k=0;k<3;++k)dot+=m[k][i]*m[k][j];
        if(std::abs(dot-(i==j?1.:0.))>.002)return false;
    }
    const double determinant=m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-
        m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    return std::abs(determinant-1)<.003;
}
inline Matrix InverseRigid(const Matrix& m) {
    Matrix r=Identity();
    for(unsigned i=0;i<3;++i) {
        for(unsigned j=0;j<3;++j)r[i][j]=m[j][i];
        for(unsigned j=0;j<3;++j)r[i][3]-=r[i][j]*m[j][3];
    }
    return r;
}
inline Quaternion Rotation(const Matrix& m) {
    Quaternion q{};
    const double trace=m[0][0]+m[1][1]+m[2][2];
    if(trace>0) {
        const double s=2*std::sqrt(trace+1);
        q={s/4,(m[2][1]-m[1][2])/s,(m[0][2]-m[2][0])/s,(m[1][0]-m[0][1])/s};
    } else {
        unsigned i=0;for(unsigned k=1;k<3;++k)if(m[k][k]>m[i][i])i=k;
        const unsigned j=(i+1)%3,k=(i+2)%3;
        const double s=2*std::sqrt(1+m[i][i]-m[j][j]-m[k][k]);
        q[0]=(m[k][j]-m[j][k])/s; q[i+1]=s/4;
        q[j+1]=(m[j][i]+m[i][j])/s;q[k+1]=(m[k][i]+m[i][k])/s;
    }
    double length=0;for(double v:q)length+=v*v;length=std::sqrt(length);
    for(double& v:q)v/=length;
    return q;
}
inline Matrix Midpoint(const Matrix& a,const Matrix& b) {
    auto qa=Rotation(a),qb=Rotation(b);
    double dot=0;for(unsigned i=0;i<4;++i)dot+=qa[i]*qb[i];
    Quaternion q{};double length=0;
    for(unsigned i=0;i<4;++i){q[i]=qa[i]+(dot<0?-qb[i]:qb[i]);length+=q[i]*q[i];}
    length=std::sqrt(length);for(double& v:q)v/=length;
    const auto [w,x,y,z]=q;
    Matrix r=Identity();
    r[0][0]=1-2*(y*y+z*z);r[0][1]=2*(x*y-z*w);r[0][2]=2*(x*z+y*w);
    r[1][0]=2*(x*y+z*w);r[1][1]=1-2*(x*x+z*z);r[1][2]=2*(y*z-x*w);
    r[2][0]=2*(x*z-y*w);r[2][1]=2*(y*z+x*w);r[2][2]=1-2*(x*x+y*y);
    for(unsigned i=0;i<3;++i)r[i][3]=(a[i][3]+b[i][3])/2;
    return r;
}
// Sky and translucent world draws can use different clipping planes while
// sharing the same camera lens. The Z row must remain specific to each draw.
inline bool SameLens(const Matrix& a,const Matrix& b) {
    for(unsigned i : {0u,1u,3u})for(unsigned j=0;j<4;++j)
        if(!std::isfinite(a[i][j]) || !std::isfinite(b[i][j]) ||
           std::abs(a[i][j]-b[i][j])>1e-5)return false;
    return true;
}
inline bool WorldProjection(const Matrix& p) {
    return Near(p,p,0) && p[3]==std::array<double,4>{0,0,-1,0};
}
// Captured opaque night sky dome. It precedes the scene's background flash,
// despite using the same opaque blend/depth state as foreground geometry.
inline bool OpaqueSkyDome(unsigned vertices,const Matrix& projection,const Matrix& model) {
    if(vertices!=288 || !WorldProjection(projection) || !Near(model,model,0) ||
       std::abs(projection[2][2]-1.0000892877578735)>1e-6 ||
       std::abs(projection[2][3]-5.000446796417236)>1e-5)return false;
    auto rotation=model;
    for(unsigned i=0;i<3;++i) {
        if(std::abs(model[i][3])>.05)return false;
        for(unsigned j=0;j<3;++j)rotation[i][j]/=320.;
    }
    return Rigid(rotation);
}
// Accept gradual uniform lens scaling, not a changed aspect, skew or projection type.
// Rotated 3DS perspective matrices put the lens terms off the diagonal.
inline std::optional<double> GradualLensScale(const Matrix& previous,const Matrix& current) {
    if(!WorldProjection(previous)||!WorldProjection(current))return {};
    unsigned row=0,col=0;
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j)
        if(std::abs(previous[i][j])>std::abs(previous[row][col])){row=i;col=j;}
    if(std::abs(previous[row][col])<1e-6)return {};
    const double scale=current[row][col]/previous[row][col];
    if(scale<=0)return {};
    // A steady transformation zoom can exceed 5% per frame at a narrow FOV.
    // Bound its angular change too, rather than treating that smooth zoom as a
    // cut. Retain a relative cap so extremely narrow lenses cannot jump freely.
    const auto lens_angle=[](double term) {
        return 2.*std::atan(1./std::abs(term))*180./3.141592653589793;
    };
    const bool small_angle=scale>=.9 && scale<=1.1 &&
        std::abs(lens_angle(current[row][col])-lens_angle(previous[row][col]))<=2.5;
    if((scale<.95 || scale>1.05) && !small_angle)return {};
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j)
        if(std::abs(current[i][j]-previous[i][j]*scale)>1e-5)return {};
    return scale;
}
// Underwater world projection has a small independent X/Y wobble relative to
// the sky. Only share the camera delta within one frame, across distinct clip
// ranges, and for the captured rotated perspective layout. Keep this draw's
// native lens; applying the sky's zoom correction would distort the world.
inline bool AlternateWorldLens(const Matrix& a,const Matrix& b) {
    if(!WorldProjection(a)||!WorldProjection(b)||a[2]==b[2])return false;
    for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j) {
        if(j==1-i) {
            if(std::abs(a[i][j])<1e-6)return false;
            const double ratio=b[i][j]/a[i][j];
            if(ratio<.92 || ratio>1.08)return false;
        } else if(std::abs(a[i][j])>1e-6 || std::abs(b[i][j])>1e-6)return false;
    }
    return true;
}
// The underwater wobble also changes independently on each axis between
// frames, including scenes without a sky draw to seed a stable lens. Accept
// only small changes of the rotated perspective terms with unchanged clipping.
inline std::optional<std::array<double,2>> GradualWorldWobble(const Matrix& a,const Matrix& b) {
    if(!WorldProjection(a)||!WorldProjection(b)||a[2]!=b[2])return {};
    // Both underwater captures use this near/far range. Swimming produces
    // much stronger distortion than the nearly stationary first capture.
    // Keep the larger envelope specific to that verified projection.
    const bool underwater=std::abs(a[2][0])<1e-6 && std::abs(a[2][1])<1e-6 &&
        std::abs(a[2][2]-1.0004167556762695)<1e-6 &&
        std::abs(a[2][3]-5.002084255218506)<1e-5;
    const double limit=underwater?.05:.005;
    std::array<double,2> scale{};
    for(unsigned r=0;r<2;++r)for(unsigned c=0;c<4;++c) {
        if(c==1-r) {
            if(std::abs(a[r][c])<1e-6)return {};
            scale[r]=b[r][c]/a[r][c];
            if(scale[r]<1.-limit || scale[r]>1.+limit)return {};
        } else if(std::abs(a[r][c])>1e-6 || std::abs(b[r][c])>1e-6)return {};
    }
    // Both axes animate in the captured distortion. A one-axis resize/aspect
    // transition must not use this exception to the normal lens guard.
    if(scale[0]==1. || scale[1]==1.)return {};
    return scale;
}
// Provenance for the native end-of-day scene-copy quad. Only bypass copies
// derived from the scene captured in this guest frame, never arbitrary textures.
struct SceneCopies {
    std::uint64_t frame{};
    std::array<std::uint32_t,8> addresses{};
    std::array<std::uint32_t,8> previous_addresses{};
    void Begin(std::uint64_t current,std::uint32_t address) {
        if(frame!=current){
            previous_addresses=current==frame+1?addresses:std::array<std::uint32_t,8>{};
            frame=current;addresses={};
        }
        Add(address);
    }
    bool PreviousContains(std::uint64_t current,std::uint32_t address) const {
        return current==frame && address &&
            std::find(previous_addresses.begin(),previous_addresses.end(),address)!=previous_addresses.end();
    }
    void Add(std::uint32_t address) {
        if(!address)return;
        for(auto& entry:addresses){if(entry==address)return;if(!entry){entry=address;return;}}
    }
    bool Contains(std::uint64_t current,std::uint32_t address) const {
        return current==frame && address &&
            std::find(addresses.begin(),addresses.end(),address)!=addresses.end();
    }
    void Transfer(std::uint64_t current,std::uint32_t source,std::uint32_t destination) {
        const bool known=Contains(current,source);
        for(auto& entry:addresses)if(entry==destination)entry=0;
        if(known)Add(destination);
    }
};
inline bool TransformationFeedback(const Matrix& projection,const Matrix& model,
                                   bool previous_scene,unsigned vertices,unsigned textures) {
    if(!previous_scene || vertices!=4 || textures!=1)return false;
    Matrix p{};p[0]={0,-1./120,0,1};p[1]={-1./200,0,0,1};
    p[2]={0,0,.5,-.5};p[3]={0,0,0,1};
    auto m=Identity();m[0][0]=m[1][1]=m[2][2]=1.0008;
    m[0][3]=-.16;m[1][3]=-.096;
    return Near(projection,p,.00001) && Near(model,m,.00001);
}
inline bool NormalizedSceneCopy(bool normalized,bool source_is_scene,bool ortho,
                               unsigned vertices,unsigned textures,bool opaque,bool stencil) {
    return normalized && source_is_scene && ortho && vertices==4 && textures==1 && opaque && !stencil;
}
inline bool NormalizedSceneClear(bool normalized,bool target_is_scene,bool ortho,
                                unsigned vertices,unsigned textures,bool opaque,bool stencil) {
    return normalized && target_is_scene && ortho && vertices==4 && textures==0 && opaque && !stencil;
}
// A native clear followed by the normalized scene copy is redundant in the
// private midpoint target. Defer the clear only until the next top-screen draw
// proves that pair; an unmatched clear must still make presentation fall back.
struct SceneClearPair {
    std::optional<std::uint64_t> frame;
    std::uint32_t target{};
    bool Observe(std::uint64_t current,std::uint32_t destination,bool clear,bool scene_copy) {
        const bool rejected=Pending(current) && !(scene_copy && destination==target);
        frame=clear?std::optional(current):std::nullopt;
        target=clear?destination:0;
        return rejected;
    }
    bool Pending(std::uint64_t current) const { return frame && *frame==current; }
    void Reset() { frame.reset();target=0; }
};
inline bool TransitionFade(const WidescreenProbe::Draw& draw,const Matrix& p) {
    // Captured screen-cover signature, shared by transitions and letterbox
    // bars. It does not by itself establish their order relative to the HUD;
    // paired presentation must preserve guest submission order.
    return draw == WidescreenProbe::TransitionOverlay && Near(p,p,0);
}
// Host-only sequencing: a pair is midpoint then current, never current then
// midpoint. Duplicate refreshes after a pair emit nothing; native 60 Hz scenes
// and discontinuities fall back to their original cadence.
enum class Presentation { Native, Pair, Skip };
enum class PresentationStatus : unsigned { Off, Settings, WaitingScene, FrameMismatch,
    NoMidpoint, NoHud, UnsupportedHud, Comparison, Cadence, Active };
inline std::atomic<PresentationStatus> presentation_status{PresentationStatus::Off};
inline const char* StatusText(PresentationStatus status) {
    switch(status) {
    case PresentationStatus::Off:return "Off";
    case PresentationStatus::Settings:return "Waiting: enable VSync and preview; use one landscape screen view";
    case PresentationStatus::WaitingScene:return "Native: waiting for supported scene";
    case PresentationStatus::FrameMismatch:return "Native: captured scene is not the completed frame";
    case PresentationStatus::NoMidpoint:return "Native: camera history not ready";
    case PresentationStatus::NoHud:return "Native: waiting for HUD capture";
    case PresentationStatus::UnsupportedHud:return "Native: unsupported HUD/scene draw";
    case PresentationStatus::Comparison:return "Native: disable current-camera comparison";
    case PresentationStatus::Cadence:return "Native: waiting for stable 30 Hz scene cadence";
    case PresentationStatus::Active:return "Active: presenting midpoint/native pairs";
    }
    return "Unknown";
}
inline bool IsCompletedCapture(std::optional<std::uint64_t> capture,std::uint64_t completed) {
    // Draws use the count before SetBufferSwap/EndGameFrame increments it.
    return capture && completed>0 && *capture==completed-1;
}
// Pace frame starts on a fixed timeline. Submission/GPU work consumes this budget;
// it must not be followed by a fresh full-frame sleep.
class PresentationPacer {
    std::optional<std::int64_t> next;
public:
    static constexpr std::int64_t Period=16666667;
    std::int64_t Deadline(std::int64_t now,bool paired) {
        if(!paired){next.reset();return now;}
        // Late frames resume immediately; adding a new buffer delay here creates
        // visible hitches and grows queue latency. Rebase to avoid rapid catch-up.
        if(!next || now-*next>=Period/4)next=now;
        const auto deadline=*next;
        *next+=Period;
        return deadline;
    }
};
class PresentationClock {
    std::optional<std::uint64_t> frame;
    std::int64_t time{};
    unsigned stable{};
    bool paired{};
public:
    void Reset() { frame.reset();stable=0;paired=false; }
    Presentation Next(std::uint64_t next,std::int64_t now,bool eligible) {
        if(frame && *frame==next)return paired?Presentation::Skip:Presentation::Native;
        const auto elapsed=now-time;
        const bool continuous=frame && next==*frame+1 && elapsed>=25 && elapsed<=50;
        stable=continuous && eligible?stable+1:0;
        frame=next;time=now;paired=eligible && stable>=2;
        return paired?Presentation::Pair:Presentation::Native;
    }
};
// Diagnostic counts are draw observations, not completed game-frame counts.
inline std::atomic<unsigned> camera_cpu_mismatch{},camera_lens_change{},camera_cut{},camera_gap{};
// Render-thread history only. It never writes guest camera state or advances time.
class History {
    Matrix current{}, projection{}, camera_delta{};
    std::array<double,2> midpoint_lens_scale{1,1};
    Matrix Project(const Matrix& p) const {
        auto midpoint=p;
        for(unsigned i=0;i<2;++i)for(unsigned j=0;j<4;++j)midpoint[i][j]*=midpoint_lens_scale[i];
        return Multiply(midpoint,camera_delta);
    }
    std::uint64_t frame{};
    std::int64_t time{};
    bool seen{}, ready{};
public:
    void Reset() { seen=ready=false; }
    // Reacquisition must not blank a valid native scene. Never interpolate when
    // the CPU camera is unavailable/mismatched, or across cuts/projection changes.
    Matrix Preview(std::uint64_t next_frame,std::int64_t now,const Matrix& camera,
                   const Matrix& p,bool cpu_matches,bool comparison,bool& fallback) {
        fallback=true;
        // A rejected draw must not erase the validated world camera from this
        // frame (e.g. auxiliary title geometry). It still renders natively and
        // never advances history. If a whole frame lacks a matching camera,
        // Observe's consecutive-frame guard forces fresh acquisition afterward.
        if(!cpu_matches) { ++camera_cpu_mismatch;return p; }
        const auto result=Observe(next_frame,now,camera,p);
        if(comparison || !result)return p;
        fallback=false;
        return *result;
    }

    std::optional<Matrix> Observe(std::uint64_t next_frame,std::int64_t now,
                                  const Matrix& camera,const Matrix& p) {
        if(!Rigid(camera)||!Near(p,p,0)) { Reset();return {}; }
        if(seen && next_frame==frame) {
            if(!Near(current,camera,.001))return {};
            if(SameLens(projection,p))
                return ready?std::optional<Matrix>(Project(p)):std::nullopt;
            if(ready && AlternateWorldLens(projection,p))return Multiply(p,camera_delta);
            return {};
        }
        const bool frame_ok=seen && next_frame==frame+1 && now>=time && now-time<=200;
        const auto lens_scale=GradualLensScale(projection,p);
        const auto wobble=GradualWorldWobble(projection,p);
        const bool lens_ok=SameLens(projection,p) || lens_scale.has_value() || wobble.has_value();
        if(!frame_ok)++camera_gap;
        else if(!lens_ok)++camera_lens_change;
        bool continuous=frame_ok && lens_ok;
        if(continuous) {
            double distance=0,trace=0;
            for(unsigned i=0;i<3;++i) {
                distance+=(current[i][3]-camera[i][3])*(current[i][3]-camera[i][3]);
                for(unsigned j=0;j<3;++j)trace+=current[i][j]*camera[i][j];
            }
            continuous=distance<=250*250 && (trace-1)/2>=std::cos(45.*3.141592653589793/180.);
            if(!continuous)++camera_cut;
        }
        ready=continuous;
        if(ready) {
            // Original vertices are in current view space. Change only the host
            // projection: P * V_mid * C_current * view_vertex.
            camera_delta=Multiply(InverseRigid(Midpoint(current,camera)),camera);
            for(unsigned r=0;r<2;++r)
                midpoint_lens_scale[r]=lens_scale?(1.+1. / *lens_scale)*.5:
                    wobble?(1.+1. / (*wobble)[r])*.5:1.;
        }
        current=camera;projection=p;frame=next_frame;time=now;seen=true;
        return ready?std::optional<Matrix>(Project(p)):std::nullopt;
    }
};
}
