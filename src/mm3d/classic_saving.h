// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
namespace MM3D::ClassicSaving {
inline std::atomic<bool> enabled{false};
// Renderer acknowledges full black before the guest changes state.
enum class ExitFade { Idle, Out, Black, In };
inline std::atomic<ExitFade> exit_fade{ExitFade::Idle};
inline std::atomic_bool exit_black_rendered{false};
// Guest title-logo draw acknowledges the destination; host time alone is insufficient.
inline std::atomic_bool exit_title_ready{false};
inline float TitleHandoffAlpha(float& elapsed,float delta,bool ready) {
    if(!ready){elapsed=0;return 1.f;}
    elapsed+=delta;
    return elapsed<=0.5f?1.f:(elapsed>=1.1f?0.f:1.f-(elapsed-0.5f)/0.6f);
}
inline std::atomic_bool resume_notice{false};
inline std::atomic_bool resume_text_active{false};
inline std::atomic<float> exit_world_alpha{0.f};
inline std::atomic_bool exit_world_black_drawn{false};
inline std::atomic_uint owl_draw_status{0};
enum class Kind { None, Cycle, Owl };
void RegisterPath(const std::string& path);
void ObserveMessage(std::uint32_t id);
bool BeginWrite(unsigned slot, Kind kind);
bool FinishWrite(unsigned slot, Kind kind);
bool Resume(unsigned slot, std::span<const std::uint8_t> loaded);
// Armed by verified checkpoint consumption, consumed before the first idle update.
inline bool ResumeNoticeArmed() { return enabled.load() && resume_notice.load(); }
// A consumed owl save arms this before gameplay loads. Neutralize controls
// throughout arrival, including non-idle player states. Enter transfers control
// to the native conversation and clears the notice so A can dismiss both pages.
template<class Axis> inline void FilterArrivalPad(std::uint32_t& buttons,Axis& x,Axis& y) {
    if(ResumeNoticeArmed()){buttons=0;x=0;y=0;}
}

std::string Status();
unsigned PendingMask(); // Read-only, verified native/sidecar identity for file markers.
} // namespace MM3D::ClassicSaving
