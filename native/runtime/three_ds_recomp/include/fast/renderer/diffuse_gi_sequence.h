#pragma once

// Synthetic frame sequences for the diffuse GI temporal tests (milestone M3).
//
// A sequence scene is a set of analytic primitives (diffuse_gi_scene.h) whose
// objects and camera move rigidly from frame to frame, with optional per-frame
// emission scales (lights switching on and off) and projection jitter. Each
// rendered frame carries the adapter buffers of the current (jittered) camera
// and the exact temporal data the adapter contract asks for: motion vectors
// from current to previous viewport UV (with and without jitter), the
// previous view-space position of every visible surface point, surface ids
// and the camera transform between the two view spaces. Everything is
// deterministic: no random numbers, no files.

#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_temporal.h"

#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi::Synthetic {

// World from local: rotation Ry(Yaw) Rx(Pitch), then translation. The world
// is the view space of the identity camera pose (camera at the origin looking
// down -Z, +Y up).
struct Pose {
    Vec3 Position;
    double Yaw = 0.0;
    double Pitch = 0.0;
};

struct SequenceScene {
    std::string Name;
    PinholeCamera Camera;              // intrinsics; jitter is added per frame
    std::vector<Primitive> Primitives; // in object space (world space for object 0)
    std::vector<uint32_t> Object;      // object index per primitive; 0 = static world
};

struct SequenceFrame {
    Pose Camera;
    std::vector<Pose> Objects;    // world from object; index 0 is ignored
    std::vector<double> Emission; // radiance scale per primitive (empty: 1)
    double JitterX = 0.0, JitterY = 0.0; // pixels, positive along +u / +v
};

struct RenderedFrame {
    Scene View;        // primitives in the current view space (3D ground truth)
    GatherInput Input; // adapter buffers (jittered camera)
    // Current -> previous normalized viewport UV per viewport pixel (2 each):
    // jitter included (both UVs jittered) and excluded (both unjittered).
    std::vector<double> MotionIncluded, MotionExcluded;
    std::vector<double> PreviousPosition; // previous view space (3 each), NaN for sky
    std::vector<uint32_t> SurfaceId;      // primitive index + 1, 0 for sky
    RigidTransform CurrentToPrevious;     // camera motion between view spaces
    double JitterX = 0.0, JitterY = 0.0;
};

[[nodiscard]] RenderedFrame RenderSequenceFrame(const SequenceScene& scene, const SequenceFrame& current,
                                                const SequenceFrame& previous);

// Unjittered camera of a frame (for exact projections in tests).
[[nodiscard]] PinholeCamera JitteredCamera(const PinholeCamera& camera, double jitterX, double jitterY);
// Viewport UV of a view-space point through a declared UV -> ray map.
[[nodiscard]] std::array<double, 2> ProjectToUv(const DepthCamera& camera, const Vec3& point);

// TemporalFrame of a rendered frame: receivers and raw signal from the gather,
// camera, motion (canonical convention: current to previous, normalized UV,
// jitter as declared), previous positions and surface ids.
[[nodiscard]] TemporalFrame MakeSequenceTemporalFrame(const RenderedFrame& rendered, const PreparedGeometry& geometry,
                                                      const std::vector<PixelGather>& gather,
                                                      JitterInMotion jitter = JitterInMotion::Included);

} // namespace Fast::Renderer::DiffuseGi::Synthetic
