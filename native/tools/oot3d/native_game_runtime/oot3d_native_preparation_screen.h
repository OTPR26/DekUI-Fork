#pragma once

#include "fast/renderer3ds/pica_program_preparation.h"

#include <chrono>
#include <cstdint>

namespace Oot3dNativeGame {

// Initial preparation, shown in place of the game. First the guest waits
// (its clock does not run) until the renderer's startup preparation is ready:
// the always-available program parts and the upscaler feature of the last run.
// Then the first frames render under the screen until they run within budget,
// so the remaining first-use costs (an upscaler feature for new parameters,
// the first scene's grass placements) are paid before gameplay is shown.
class NativePreparationScreen {
  public:
    using Clock = std::chrono::steady_clock;
    enum class Phase : uint8_t { Programs, FirstFrames, Done };

    // A frame within budget, and how many in a row end the first frames.
    static constexpr double kSteadyFrameMilliseconds = 100.0;
    static constexpr uint32_t kSteadyFrames = 10U;
    // The first frames never hold the screen longer than this.
    static constexpr double kFirstFramesLimitSeconds = 30.0;

    explicit NativePreparationScreen(Clock::time_point start) noexcept
        : mStart(start) {}

    // Before a host frame: true while the guest must wait.
    bool HoldsGuest(const Fast::Renderer3ds::PicaPreparationStatus& status,
                    Clock::time_point now) noexcept {
        if (mPhase != Phase::Programs) return false;
        if (!mHoldStarted) {
            mHoldStart = now;
            mHoldStarted = true;
        }
        mParts = status.Parts;
        if (!status.Ready()) return true;
        mPhase = Phase::FirstFrames;
        mStartupSeconds = Seconds(mStart, now);
        mWaitSeconds = Seconds(mHoldStart, now);
        mFirstFramesStart = now;
        return false;
    }

    // After a completed host frame of the guest.
    void ObserveFrame(double milliseconds, Clock::time_point now) noexcept {
        if (mPhase != Phase::FirstFrames) return;
        ++mFirstFrames;
        mSteadyFrames = milliseconds < kSteadyFrameMilliseconds ? mSteadyFrames + 1U : 0U;
        mFirstFramesSeconds = Seconds(mFirstFramesStart, now);
        if (mSteadyFrames >= kSteadyFrames || mFirstFramesSeconds >= kFirstFramesLimitSeconds)
            mPhase = Phase::Done;
    }

    [[nodiscard]] Phase CurrentPhase() const noexcept { return mPhase; }
    [[nodiscard]] bool CoversFrame() const noexcept { return mPhase != Phase::Done; }
    [[nodiscard]] bool Done() const noexcept { return mPhase == Phase::Done; }
    // From the start of the preparation to the guest's first frame, and the
    // part of it the host loop held the guest.
    [[nodiscard]] double StartupSeconds() const noexcept { return mStartupSeconds; }
    [[nodiscard]] double WaitSeconds() const noexcept { return mWaitSeconds; }
    [[nodiscard]] double FirstFramesSeconds() const noexcept { return mFirstFramesSeconds; }
    [[nodiscard]] uint32_t FirstFrames() const noexcept { return mFirstFrames; }
    [[nodiscard]] size_t Parts() const noexcept { return mParts; }
    [[nodiscard]] uint32_t SteadyFrames() const noexcept { return mSteadyFrames; }

    // Draws the screen over everything else of this ImGui frame.
    void Draw(const Fast::Renderer3ds::PicaPreparationStatus& status) const;

  private:
    static double Seconds(Clock::time_point from, Clock::time_point to) noexcept {
        return std::chrono::duration<double>(to - from).count();
    }

    Clock::time_point mStart;
    Clock::time_point mHoldStart{};
    Clock::time_point mFirstFramesStart{};
    bool mHoldStarted = false;
    Phase mPhase = Phase::Programs;
    size_t mParts = 0;
    uint32_t mFirstFrames = 0;
    uint32_t mSteadyFrames = 0;
    double mStartupSeconds = 0.0;
    double mWaitSeconds = 0.0;
    double mFirstFramesSeconds = 0.0;
};

} // namespace Oot3dNativeGame
