#pragma once

#include "fast/renderer3ds/pica_light_binding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Fast::Renderer3ds {

// Continuous scene light from a title's stepped native values, advanced once
// per guest frame (docs/TRIAEVUM_ANIMATED_LIGHTING_STRATEGY.md, 5.2-5.4).
// The state belongs to the title's capture; a discontinuity (scene,
// environment, state load) restarts it.

// One stepped native value made continuous. After each step it moves to the
// new value over as many guest frames as the previous step took (at most
// kMaximumFrames), so a value that changes by single units at a steady
// cadence moves in every frame and never strays more than a step or two
// from the native one. A step larger than kEventUnits (a flash, a cut) is
// taken at once.
class PicaSteppedSignal {
  public:
    static constexpr uint32_t kMaximumFrames = 30U;
    static constexpr float kEventUnits = 3.0F;

    float Update(float native, float unit, bool continuous) noexcept {
        if (!mValid || !continuous || !std::isfinite(native)) {
            Snap(native);
            return mOutput;
        }
        ++mSinceStep;
        if (native != mNative) {
            if (!(unit > 0.0F) || std::abs(native - mNative) > kEventUnits * unit) {
                Snap(native);
                return mOutput;
            }
            mFrom = mOutput;
            mDuration = std::clamp(mSinceStep, 1U, kMaximumFrames);
            mElapsed = 0U;
            mSinceStep = 0U;
            mNative = native;
        }
        if (mElapsed < mDuration) {
            ++mElapsed;
            mOutput = mFrom + (mNative - mFrom) * (float(mElapsed) / float(mDuration));
        }
        return mOutput;
    }

    [[nodiscard]] float Output() const noexcept { return mOutput; }

  private:
    void Snap(float native) noexcept {
        mNative = mFrom = mOutput = native;
        mDuration = 1U;
        mElapsed = 1U;
        mSinceStep = 0U;
        mValid = std::isfinite(native);
    }

    float mNative = 0.0F;
    float mFrom = 0.0F;
    float mOutput = 0.0F;
    uint32_t mDuration = 1U;
    uint32_t mElapsed = 1U;
    uint32_t mSinceStep = 0U;
    bool mValid = false;
};

// Decides, once per guest frame, which scene directional casts the shadow
// and how strongly, and publishes it in the snapshot
// (ShadowCasterIdentity, ShadowStrength) so every draw, the shadow map and
// every presented sample agree.
//
// Candidates are the lights above the horizon, ranked by their energy after
// the direct horizon. A challenger stronger than the caster by
// kHandoverMargin takes over: the shadow fades out, the caster changes at
// zero, and the shadow fades in. A caster that sets hands over at once (its
// horizon has already removed its shadow) and the new one fades in. The
// published strength is that fade times the caster's own horizon, so a
// setting sun's shadow thins to nothing and the rising moon's grows from
// nothing.
class PicaSceneShadowCasterTracker {
  public:
    static constexpr float kHandoverMargin = 0.1F;
    static constexpr float kHandoverSeconds = 0.4F;

    void Update(PicaSceneDirectionalLights& lights, float seconds, bool continuous) noexcept {
        if (lights.Count > lights.Lights.size()) {
            Reset();
            return;
        }
        const auto find = [&](uint64_t identity) -> const PicaSceneDirectionalLight* {
            if (identity == 0) return nullptr;
            for (uint32_t i = 0; i < lights.Count; ++i)
                if (lights.Lights[i].SourceIdentity == identity && PicaSceneDirectionalDirection(lights.Lights[i]))
                    return &lights.Lights[i];
            return nullptr;
        };
        const auto energy = [&](const PicaSceneDirectionalLight* light) {
            return light != nullptr ? PicaSceneShadowCandidateEnergy(*light, lights.WorldUp) : 0.0F;
        };
        // The stateless rule, without a published decision.
        lights.ShadowCasterIdentity = 0;
        const auto* best = FindPicaSceneMainDirectional(lights);
        const float step = std::clamp(seconds, 0.0F, 1.0F) / (0.5F * kHandoverSeconds);
        const auto* incumbent = find(mCaster);
        // The frame of a change shows the new caster at the strength it starts
        // from; fading in begins with the next frame.
        bool changed = false;
        if (!continuous || incumbent == nullptr) {
            // A discontinuity starts with the strongest light, fully shown;
            // a caster that vanished hands over and the new one fades in.
            mFade = !continuous || mCaster == 0 ? 1.0F : 0.0F;
            mCaster = best != nullptr ? best->SourceIdentity : 0;
            mPending = 0;
            changed = true;
        } else if (energy(incumbent) <= 0.0F) {
            if (best != nullptr && best != incumbent && energy(best) > 0.0F) {
                mCaster = best->SourceIdentity;
                mFade = 0.0F;
                changed = true;
            }
            mPending = 0;
        } else if (mPending != 0) {
            const auto* pending = find(mPending);
            if (pending != nullptr && energy(pending) > energy(incumbent)) {
                mFade -= step;
                if (mFade <= 0.0F) {
                    mCaster = mPending;
                    mPending = 0;
                    mFade = 0.0F;
                    changed = true;
                }
            } else {
                mPending = 0;
            }
        } else if (best != nullptr && best != incumbent &&
                   energy(best) > energy(incumbent) * (1.0F + kHandoverMargin)) {
            mPending = best->SourceIdentity;
        }
        if (!changed && mPending == 0 && mFade < 1.0F) mFade = std::min(1.0F, mFade + step);
        const auto* caster = find(mCaster);
        if (caster == nullptr) {
            lights.ShadowCasterIdentity = 0;
            lights.ShadowStrength = 0.0F;
            return;
        }
        const auto direction = PicaSceneDirectionalDirection(*caster);
        lights.ShadowCasterIdentity = mCaster;
        lights.ShadowStrength = std::clamp(mFade, 0.0F, 1.0F) *
            PicaSceneShadowHorizon(PicaSceneElevationSine(*direction, lights.WorldUp));
    }

    void Reset() noexcept { *this = {}; }

  private:
    uint64_t mCaster = 0;
    uint64_t mPending = 0;
    float mFade = 1.0F;
};

} // namespace Fast::Renderer3ds
