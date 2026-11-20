#include "fast/renderer/diffuse_gi_sequence.h"

// Synthetic sequences for the M3 tests. Own code: rigid poses, the exact
// previous position of each visible surface point through its object's
// motion, and its projection through the previous (jittered or unjittered)
// camera.

#include <cmath>
#include <limits>

namespace Fast::Renderer::DiffuseGi::Synthetic {
namespace {

using Matrix = std::array<double, 9>;

Matrix RotationOf(const Pose& pose) {
    const double cy = std::cos(pose.Yaw), sy = std::sin(pose.Yaw);
    const double cp = std::cos(pose.Pitch), sp = std::sin(pose.Pitch);
    // Ry(yaw) * Rx(pitch).
    return {cy, sy * sp, sy * cp, 0.0, cp, -sp, -sy, cy * sp, cy * cp};
}

Vec3 Apply(const Matrix& m, const Vec3& v) {
    return {m[0] * v.X + m[1] * v.Y + m[2] * v.Z, m[3] * v.X + m[4] * v.Y + m[5] * v.Z,
            m[6] * v.X + m[7] * v.Y + m[8] * v.Z};
}

Vec3 ApplyTransposed(const Matrix& m, const Vec3& v) {
    return {m[0] * v.X + m[3] * v.Y + m[6] * v.Z, m[1] * v.X + m[4] * v.Y + m[7] * v.Z,
            m[2] * v.X + m[5] * v.Y + m[8] * v.Z};
}

Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
    return out;
}

Matrix Transposed(const Matrix& m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }

const Pose& ObjectPose(const SequenceFrame& frame, uint32_t object) {
    static const Pose identity{};
    if (object == 0U || object >= frame.Objects.size()) return identity;
    return frame.Objects[object];
}

// Primitive of the scene placed by the frame's poses into its view space.
Primitive Place(const Primitive& p, const Pose& object, const Pose& camera, double emission) {
    const Matrix ro = RotationOf(object);
    const Matrix rc = RotationOf(camera);
    Primitive out = p;
    const Vec3 world = Add(Apply(ro, p.Centre), object.Position);
    out.Centre = ApplyTransposed(rc, Sub(world, camera.Position));
    for (auto& axis : out.Axes) axis = ApplyTransposed(rc, Apply(ro, axis));
    out.Radiance = {p.Radiance.R * emission, p.Radiance.G * emission, p.Radiance.B * emission};
    return out;
}

} // namespace

PinholeCamera JitteredCamera(const PinholeCamera& camera, double jitterX, double jitterY) {
    // ray(uv) = ray_unjittered(uv - j / extent): the jittered projection of a
    // point is its unjittered projection plus the jitter.
    PinholeCamera out = camera;
    const double tx = camera.TanHalfX > 0.0 ? camera.TanHalfX : camera.TanHalfY * camera.Width / camera.Height;
    out.TanHalfX = tx;
    out.ShiftX = camera.ShiftX - 2.0 * tx * jitterX / camera.Width;
    out.ShiftY = camera.ShiftY + 2.0 * camera.TanHalfY * jitterY / camera.Height;
    return out;
}

std::array<double, 2> ProjectToUv(const DepthCamera& c, const Vec3& point) {
    const double tx = point.X / -point.Z - c.ViewX[2];
    const double ty = point.Y / -point.Z - c.ViewY[2];
    const double det = c.ViewX[0] * c.ViewY[1] - c.ViewX[1] * c.ViewY[0];
    return {(c.ViewY[1] * tx - c.ViewX[1] * ty) / det, (-c.ViewY[0] * tx + c.ViewX[0] * ty) / det};
}

RenderedFrame RenderSequenceFrame(const SequenceScene& scene, const SequenceFrame& current,
                                  const SequenceFrame& previous) {
    RenderedFrame out;
    out.JitterX = current.JitterX;
    out.JitterY = current.JitterY;
    out.View.Name = scene.Name;
    out.View.Camera = JitteredCamera(scene.Camera, current.JitterX, current.JitterY);
    for (size_t i = 0; i < scene.Primitives.size(); ++i) {
        const uint32_t object = i < scene.Object.size() ? scene.Object[i] : 0U;
        const double emission = i < current.Emission.size() ? current.Emission[i] : 1.0;
        out.View.Primitives.push_back(Place(scene.Primitives[i], ObjectPose(current, object), current.Camera, emission));
    }
    out.Input = Rasterize(out.View);

    const Matrix rc = RotationOf(current.Camera), rp = RotationOf(previous.Camera);
    // x_prev = Rp^T (Rc x + Cc - Cp).
    const Matrix r = Multiply(Transposed(rp), rc);
    const Vec3 t = ApplyTransposed(rp, Sub(current.Camera.Position, previous.Camera.Position));
    out.CurrentToPrevious.R = r;
    out.CurrentToPrevious.T = {t.X, t.Y, t.Z};

    const DepthCamera now = out.Input.Camera;
    const DepthCamera nowPlain = MakeDepthCamera(scene.Camera);
    const DepthCamera before = MakeDepthCamera(JitteredCamera(scene.Camera, previous.JitterX, previous.JitterY));
    const DepthCamera beforePlain = nowPlain;
    const uint32_t w = scene.Camera.Width, h = scene.Camera.Height;
    const size_t count = static_cast<size_t>(w) * h;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    out.MotionIncluded.assign(2 * count, nan);
    out.MotionExcluded.assign(2 * count, nan);
    out.PreviousPosition.assign(3 * count, nan);
    out.SurfaceId.assign(count, 0U);
    const double farLimit = std::isinf(scene.Camera.Far) ? 1.0e300 : scene.Camera.Far;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const double u = (x + 0.5) / w, v = (y + 0.5) / h;
            const Vec3 ray{now.ViewX[0] * u + now.ViewX[1] * v + now.ViewX[2],
                           now.ViewY[0] * u + now.ViewY[1] * v + now.ViewY[2], -1.0};
            const auto hit = Intersect(out.View.Primitives, {}, ray, scene.Camera.Near, farLimit);
            Vec3 previousPoint;
            Vec3 currentPoint;
            bool direction = false;
            if (hit) {
                out.SurfaceId[i] = static_cast<uint32_t>(hit->Primitive) + 1U;
                currentPoint = Scale(ray, hit->Distance);
                const uint32_t object = hit->Primitive < scene.Object.size() ? scene.Object[hit->Primitive] : 0U;
                const Pose& oc = ObjectPose(current, object);
                const Pose& op = ObjectPose(previous, object);
                const Vec3 world = Add(Apply(rc, currentPoint), current.Camera.Position);
                const Vec3 local = ApplyTransposed(RotationOf(oc), Sub(world, oc.Position));
                const Vec3 worldBefore = Add(Apply(RotationOf(op), local), op.Position);
                previousPoint = ApplyTransposed(rp, Sub(worldBefore, previous.Camera.Position));
                out.PreviousPosition[3 * i] = previousPoint.X;
                out.PreviousPosition[3 * i + 1] = previousPoint.Y;
                out.PreviousPosition[3 * i + 2] = previousPoint.Z;
            } else {
                // Sky: a direction (point at infinity) rotated by the camera only.
                direction = true;
                currentPoint = ray;
                previousPoint = Apply(r, ray);
            }
            if (!(previousPoint.Z < 0.0)) continue; // behind the previous camera: no motion
            (void)direction;
            const auto uvBefore = ProjectToUv(before, previousPoint);
            const auto uvBeforePlain = ProjectToUv(beforePlain, previousPoint);
            const auto uvNowPlain = ProjectToUv(nowPlain, currentPoint);
            out.MotionIncluded[2 * i] = uvBefore[0] - u;
            out.MotionIncluded[2 * i + 1] = uvBefore[1] - v;
            out.MotionExcluded[2 * i] = uvBeforePlain[0] - uvNowPlain[0];
            out.MotionExcluded[2 * i + 1] = uvBeforePlain[1] - uvNowPlain[1];
        }
    return out;
}

TemporalFrame MakeSequenceTemporalFrame(const RenderedFrame& rendered, const PreparedGeometry& geometry,
                                        const std::vector<PixelGather>& gather, JitterInMotion jitter) {
    TemporalFrame frame = MakeTemporalFrame(rendered.Input, geometry, gather);
    frame.JitterX = rendered.JitterX;
    frame.JitterY = rendered.JitterY;
    frame.CurrentToPrevious = rendered.CurrentToPrevious;
    frame.Motion = {MotionDirection::CurrentToPrevious, MotionUnits::NormalizedViewportUv, jitter};
    frame.MotionVectors = jitter == JitterInMotion::Excluded ? rendered.MotionExcluded : rendered.MotionIncluded;
    frame.PreviousPosition = rendered.PreviousPosition;
    frame.SurfaceId = rendered.SurfaceId;
    return frame;
}

} // namespace Fast::Renderer::DiffuseGi::Synthetic
