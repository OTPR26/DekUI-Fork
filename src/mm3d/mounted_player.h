// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/link_pose_capture.h"
#include "mm3d/player_model_scale.h"

namespace MM3D::MountedPlayer {
using U32 = std::uint32_t;
using Matrix = std::array<float, 12>;
// Scale the character about the saddle attachment, rather than the feet.
// The native matrix builder still owns every rotation, scale and animation.
inline std::optional<std::array<float, 3>> Offset(const Matrix& root, const Matrix& pelvis,
                                                 const Matrix& view, float ratio) {
    if (!std::isfinite(ratio) || ratio < 1.f || ratio > 2.f) return {};
    for (const auto& matrix : {root, pelvis, view})
        for (float value : matrix) if (!std::isfinite(value)) return {};
    std::array<float, 3> local{}, result{};
    for (unsigned axis = 0; axis < 3; ++axis) {
        local[axis] = (pelvis[axis * 4 + 3] - root[axis * 4 + 3]) * (1.f / ratio - 1.f);
        if (std::abs(local[axis]) > 10000.f) return {};
    }
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col)
            result[row] += view[row * 4 + col] * local[col];
    return result;
}

// Native shared human-animation IDs for the supported USA Rev 1 archive.
// Intro riding uses scripted animation, without the ordinary riding flag.
enum class Pose { Ground, Seated, Mount, Dismount, Fall };
inline Pose AnimationPose(U32 animation) {
    if ((animation >= 360 && animation <= 374) ||
        (animation >= 379 && animation <= 382) ||
        (animation >= 417 && animation <= 420) || animation == 452 ||
        animation == 487 || animation == 488 || animation == 530 ||
        animation == 531 || animation == 533 || animation == 541 || animation == 542)
        return Pose::Seated;
    if (animation == 376 || animation == 378 || animation == 425 ||
        animation == 426 || animation == 532) return Pose::Mount;
    if (animation == 375 || animation == 377) return Pose::Dismount;
    if (animation == 411) return Pose::Fall;
    return Pose::Ground;
}
inline float Weight(U32 animation, bool riding, float frame, float start, float end) {
    const auto pose = AnimationPose(animation);
    if (pose == Pose::Ground) return riding ? 1.f : 0.f;
    if (pose == Pose::Seated) return 1.f;
    if (!std::isfinite(frame) || !std::isfinite(start) || !std::isfinite(end) ||
        std::abs(end - start) < .001f) return 0.f;
    float t = std::clamp((frame - start) / (end - start), 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    return pose == Pose::Mount ? t : 1.f - t;
}

struct Hook {
    float weight = 0.f;
    U32 player_instance = 0, animation_id = 0;
    U32 instance = 0, output = 0;
    std::array<float, 3> offset{};
    template<class M> void BeginActor(M& m, U32 actor, U32 context, bool enabled) {
        weight = 0.f; player_instance = instance = output = animation_id = 0; offset = {};
        if (!enabled || !PlayerModelScale::HumanPlayer(m, actor, context) ||
            !PlayerModelScale::Pointer(actor, 0x11dd0)) return;
        const auto flags = m.Read(actor + 0x11db0), animation = m.Read(actor + 0x370),
                   controller = m.Read(actor + 0x368), scale = m.Read(actor + 0x5c),
                   frame = m.Read(actor + 0x37c), start = m.Read(actor + 0x384),
                   end = m.Read(actor + 0x388);
        if (!flags || !animation || !controller || !scale || !frame || !start || !end ||
            !PlayerModelScale::Pointer(*controller, 0xfc) ||
            PlayerModelScale::Scale(*scale, true) == *scale) return;
        animation_id = *animation;
        weight = Weight(*animation, (*flags & 0x800000) != 0, std::bit_cast<float>(*frame),
                        std::bit_cast<float>(*start), std::bit_cast<float>(*end));
        player_instance = *controller + 0xd4;
    }
    template<class M> static std::optional<Matrix> ReadMatrix(M& m, U32 address) {
        if (!PlayerModelScale::Pointer(address, 48)) return {};
        Matrix matrix{};
        for (unsigned i = 0; i < matrix.size(); ++i) {
            const auto value = m.Read(address + i * 4);
            if (!value || !std::isfinite(std::bit_cast<float>(*value))) return {};
            matrix[i] = std::bit_cast<float>(*value);
        }
        return matrix;
    }
    // Resolve the active player at skeleton submission. Actor transforms and
    // skeleton submission can run in separate passes, so actor-draw state must
    // not be carried between them.
    template<class M> void Submit(M& m, U32 candidate, U32 view, bool enabled) {
        weight = 0.f; player_instance = instance = output = animation_id = 0; offset = {};
        if (!enabled) return;
        const auto root = m.Read(0x72f5c0);
        if (!root || !PlayerModelScale::Pointer(*root, 0x170)) return;
        const auto context = m.Read(*root + 0x16c);
        if (!context || !PlayerModelScale::Pointer(*context, 0x20e0)) return;
        const auto actor = m.Read(*context + 0x20dc);
        if (!actor) return;
        BeginActor(m, *actor, *context, true);
        Enter(m, candidate, view);
    }
    template<class M> void Enter(M& m, U32 candidate, U32 view) {
        instance = output = 0;
        if (weight <= 0.f || candidate != player_instance ||
            !PlayerModelScale::Pointer(candidate, 0x28)) return;
        const auto wrapper = m.Read(candidate + 0xc), source = m.Read(candidate + 0x1c),
                   destination = m.Read(candidate + 0x24);
        if (!wrapper || !source || !destination || !PlayerModelScale::Pointer(*wrapper, 4) ||
            !PlayerModelScale::Pointer(*source, 26 * 48) ||
            !PlayerModelScale::Pointer(*destination, 26 * 48)) return;
        const auto skeleton = m.Read(*wrapper);
        if (!skeleton || !PlayerModelScale::Pointer(*skeleton, 16 + 26 * 44) ||
            m.Read(*skeleton) != std::optional<U32>(0x206c6b73) ||
            m.Read(*skeleton + 8) != std::optional<U32>(26)) return;
        for (unsigned i = 0; i < 26; ++i) {
            if (m.Read(*skeleton + 16 + i * 44) != std::optional<U32>(LinkPoseCapture::BoneWords[i]) ||
                m.Read(*skeleton + 56 + i * 44) != std::optional<U32>(LinkPoseCapture::Hashes[i])) return;
        }
        const auto root = ReadMatrix(m, *source), pelvis = ReadMatrix(m, *source + 48),
                   camera = ReadMatrix(m, view);
        if (!root || !pelvis || !camera) return;
        const auto shift = Offset(*root, *pelvis, *camera, PlayerModelScale::AdultScale);
        if (!shift) return;
        offset = *shift;
        for (auto& value : offset) value *= weight;
        instance = candidate; output = *destination;
    }
    template<class M> void MatrixBuilt(M& m, U32 candidate, unsigned index) {
        if (!instance || candidate != instance || index >= 26) return;
        const U32 address = output + index * 48;
        const auto matrix = ReadMatrix(m, address);
        if (!matrix) { instance = output = 0; return; }
        for (unsigned axis = 0; axis < 3; ++axis)
            m.Write(address + (axis * 4 + 3) * 4,
                    std::bit_cast<U32>((*matrix)[axis * 4 + 3] + offset[axis]));
    }
};
} // namespace MM3D::MountedPlayer
