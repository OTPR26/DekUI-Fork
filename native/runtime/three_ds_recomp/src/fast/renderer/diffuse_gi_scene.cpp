#include "fast/renderer/diffuse_gi_scene.h"

// Synthetic scene generator for the diffuse GI tests (M2). Own code: ray/box
// slabs, ray/rectangle planes, perspective depth encodings of the declared
// adapter contract, and the section-plane cross-sections that give M1's
// continuous integrator its breakpoints.

#include <algorithm>
#include <cmath>
#include <limits>

namespace Fast::Renderer::DiffuseGi::Synthetic {
namespace {

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }

struct Face {
    Vec3 Centre, Normal, U, V;
    double HalfU = 0.0, HalfV = 0.0;
    size_t Primitive = 0;
};

void AppendFaces(const Primitive& p, size_t index, std::vector<Face>& faces) {
    // M8 triangles add no section breakpoints: their scenes are measured
    // against analytic references, never M1-integrated.
    if (p.Shape == Primitive::Kind::Triangle) return;
    if (p.Shape == Primitive::Kind::Rectangle) {
        faces.push_back({p.Centre, p.Axes[2], p.Axes[0], p.Axes[1], p.Half.X, p.Half.Y, index});
        return;
    }
    const double half[3] = {p.Half.X, p.Half.Y, p.Half.Z};
    for (int axis = 0; axis < 3; ++axis)
        for (const double sign : {1.0, -1.0}) {
            const Vec3 normal = Scale(p.Axes[axis], sign);
            const Vec3 centre = Add(p.Centre, Scale(normal, half[axis]));
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            faces.push_back({centre, normal, p.Axes[u], p.Axes[v], half[u], half[v], index});
        }
}

double Cross2(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

double WrapAngle(double angle) {
    angle = std::remainder(angle, 2.0 * kPi);
    return angle <= -kPi ? angle + 2.0 * kPi : angle;
}

} // namespace

Rgb Primitive::RadianceAt(const Vec3& point) const {
    if (!(CheckerPeriod > 0.0)) return Radiance;
    const Vec3 local = Sub(point, Centre);
    // A small inward offset keeps the coordinate along the face normal off the
    // period boundary at the face itself.
    int64_t parity = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const double a = Dot(local, Axes[axis]);
        const double half = axis == 0 ? Half.X : (axis == 1 ? Half.Y : Half.Z);
        const double inside = std::clamp(a, -half * (1.0 - 1e-9), half * (1.0 - 1e-9));
        parity += static_cast<int64_t>(std::floor(inside / CheckerPeriod));
    }
    return (parity & 1) != 0 ? CheckerRadiance : Radiance;
}

Primitive Primitive::Box(const Vec3& centre, const Vec3& half, const Rgb& radiance) {
    Primitive p;
    p.Shape = Kind::Box;
    p.Centre = centre;
    p.Half = half;
    p.Radiance = radiance;
    return p;
}

Primitive Primitive::TiltedBox(const Vec3& centre, const Vec3& half, double pitch, const Rgb& radiance) {
    Primitive p = Box(centre, half, radiance);
    const double c = std::cos(pitch), s = std::sin(pitch);
    p.Axes = {Vec3{1.0, 0.0, 0.0}, Vec3{0.0, c, -s}, Vec3{0.0, s, c}};
    return p;
}

Primitive Primitive::Rectangle(const Vec3& centre, const Vec3& axisU, const Vec3& axisV, double halfU, double halfV,
                               const Rgb& radiance, bool twoSided) {
    Primitive p;
    p.Shape = Kind::Rectangle;
    p.Centre = centre;
    const Vec3 u = Normalize(axisU);
    const Vec3 w = Normalize(Cross(u, axisV));
    p.Axes = {u, Cross(w, u), w};
    p.Half = {halfU, halfV, 0.0};
    p.Radiance = radiance;
    p.TwoSided = twoSided;
    return p;
}

Primitive Primitive::SmoothStrip(const Vec3& centre, const Vec3& axisU, const Vec3& axisV, double halfU, double halfV,
                                 const Vec3& normalAtMinusU, const Vec3& normalAtPlusU, const Rgb& radiance) {
    Primitive p = Rectangle(centre, axisU, axisV, halfU, halfV, radiance, false);
    p.SmoothShading = true;
    p.ShadingNormals = {Normalize(normalAtMinusU), Normalize(normalAtPlusU), Vec3{}};
    return p;
}

Primitive Primitive::Triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Rgb& radiance) {
    Primitive p;
    p.Shape = Kind::Triangle;
    p.Vertices = {a, b, c};
    p.Centre = Scale(Add(Add(a, b), c), 1.0 / 3.0);
    const Vec3 u = Normalize(Sub(b, a));
    const Vec3 w = Normalize(Cross(Sub(b, a), Sub(c, a)));
    p.Axes = {u, Cross(w, u), w};
    p.Radiance = radiance;
    return p;
}

Primitive Primitive::SmoothTriangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& na, const Vec3& nb,
                                    const Vec3& nc, const Rgb& radiance) {
    Primitive p = Triangle(a, b, c, radiance);
    p.SmoothShading = true;
    p.ShadingNormals = {Normalize(na), Normalize(nb), Normalize(nc)};
    return p;
}

std::optional<RayHit> Intersect(const std::vector<Primitive>& primitives, const Vec3& origin, const Vec3& direction,
                                double tMin, double tMax) {
    std::optional<RayHit> best;
    for (size_t index = 0; index < primitives.size(); ++index) {
        const Primitive& p = primitives[index];
        const Vec3 local = Sub(origin, p.Centre);
        if (p.Shape == Primitive::Kind::Triangle) {
            // Ray/triangle by barycentric coordinates (u at b, v at c), with
            // a tiny margin so that shared edges of a mesh leave no cracks.
            constexpr double kEdge = 1.0e-12;
            const Vec3 e1 = Sub(p.Vertices[1], p.Vertices[0]), e2 = Sub(p.Vertices[2], p.Vertices[0]);
            const Vec3 pv = Cross(direction, e2);
            const double det = Dot(e1, pv);
            if (!(std::abs(det) > 0.0)) continue;
            const Vec3 tv = Sub(origin, p.Vertices[0]);
            const double u = Dot(tv, pv) / det;
            if (u < -kEdge || u > 1.0 + kEdge) continue;
            const Vec3 qv = Cross(tv, e1);
            const double v = Dot(direction, qv) / det;
            if (v < -kEdge || u + v > 1.0 + kEdge) continue;
            const double t = Dot(e2, qv) / det;
            if (!(t >= tMin && t <= tMax) || (best && t >= best->Distance)) continue;
            const bool emitting = p.TwoSided || Dot(direction, p.Axes[2]) < 0.0;
            const Vec3 shading =
                p.SmoothShading ? Normalize(Add(Add(Scale(p.ShadingNormals[0], 1.0 - u - v), Scale(p.ShadingNormals[1], u)),
                                                Scale(p.ShadingNormals[2], v)))
                                : p.Axes[2];
            best = RayHit{t, p.Axes[2], index, emitting, shading};
            continue;
        }
        if (p.Shape == Primitive::Kind::Rectangle) {
            const double denominator = Dot(direction, p.Axes[2]);
            if (!(std::abs(denominator) > 0.0)) continue;
            const double t = -Dot(local, p.Axes[2]) / denominator;
            if (!(t >= tMin && t <= tMax) || (best && t >= best->Distance)) continue;
            const Vec3 hit = Add(local, Scale(direction, t));
            if (std::abs(Dot(hit, p.Axes[0])) > p.Half.X || std::abs(Dot(hit, p.Axes[1])) > p.Half.Y) continue;
            const bool emitting = p.TwoSided || -denominator > 0.0;
            Vec3 shading = p.Axes[2];
            if (p.SmoothShading) {
                const double w = std::clamp((Dot(hit, p.Axes[0]) + p.Half.X) / (2.0 * p.Half.X), 0.0, 1.0);
                shading = Normalize(Add(Scale(p.ShadingNormals[0], 1.0 - w), Scale(p.ShadingNormals[1], w)));
            }
            best = RayHit{t, p.Axes[2], index, emitting, shading};
            continue;
        }
        const double half[3] = {p.Half.X, p.Half.Y, p.Half.Z};
        double tNear = -std::numeric_limits<double>::infinity();
        double tFar = std::numeric_limits<double>::infinity();
        Vec3 normal;
        bool miss = false;
        for (int axis = 0; axis < 3 && !miss; ++axis) {
            const double o = Dot(local, p.Axes[axis]);
            const double d = Dot(direction, p.Axes[axis]);
            if (!(std::abs(d) > 0.0)) {
                if (std::abs(o) > half[axis]) miss = true;
                continue;
            }
            double t1 = (-half[axis] - o) / d;
            double t2 = (half[axis] - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tNear) {
                tNear = t1;
                normal = Scale(p.Axes[axis], d > 0.0 ? -1.0 : 1.0);
            }
            tFar = std::min(tFar, t2);
        }
        if (miss || tNear > tFar || !(tNear >= tMin && tNear <= tMax)) continue;
        if (best && tNear >= best->Distance) continue;
        best = RayHit{tNear, normal, index, true, normal};
    }
    return best;
}

DepthCamera MakeDepthCamera(const PinholeCamera& c) {
    DepthCamera depth;
    const double tx = c.TanHalfX > 0.0 ? c.TanHalfX : c.TanHalfY * c.Width / c.Height;
    depth.Encoding = c.Encoding;
    depth.Near = c.Near;
    depth.Far = c.Far;
    depth.ViewX = {2.0 * tx, 0.0, -tx + c.ShiftX};
    depth.ViewY = {0.0, -2.0 * c.TanHalfY, c.TanHalfY + c.ShiftY};
    return depth;
}

float EncodeDepth(double z, const PinholeCamera& c) {
    const double n = c.Near, f = c.Far;
    double d = 0.0;
    switch (c.Encoding) {
    case DepthEncoding::PerspectiveZeroToOne:
        d = f * (z - n) / (z * (f - n));
        break;
    case DepthEncoding::ReversedPerspectiveZeroToOne:
        d = std::isinf(f) ? n / z : n * (f - z) / (z * (f - n));
        break;
    case DepthEncoding::LinearWBuffer:
        d = (z - n) / (f - n);
        break;
    case DepthEncoding::ReversedLinearWBuffer:
        d = (f - z) / (f - n);
        break;
    default:
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (c.Storage == DepthStorage::Unorm24) {
        constexpr double levels = 16777215.0;
        d = std::round(std::clamp(d, 0.0, 1.0) * levels) / levels;
    }
    return static_cast<float>(d);
}

GatherInput Rasterize(const Scene& scene) {
    const PinholeCamera& c = scene.Camera;
    GatherInput in;
    in.BufferWidth = c.Width + scene.PadLeft + scene.PadRight;
    in.BufferHeight = c.Height + scene.PadTop + scene.PadBottom;
    in.Viewport = {scene.PadLeft, scene.PadTop, c.Width, c.Height};
    in.Camera = MakeDepthCamera(c);
    const size_t texels = static_cast<size_t>(in.BufferWidth) * in.BufferHeight;
    // Poison outside the viewport: a near bright surface. Any read of it would
    // dominate the result.
    in.Depth.assign(texels, EncodeDepth(std::max(2.0 * c.Near, 0.5), c));
    in.Class.assign(texels, static_cast<uint8_t>(TexelClass::OneSided));
    in.Radiance.assign(3 * texels, 1.0e30F);
    in.FaceNormals.assign(3 * texels, 0.0F);
    for (size_t i = 0; i < texels; ++i) in.FaceNormals[3 * i + 2] = 1.0F;
    if (scene.ShadingGuide) in.ShadingNormals = in.FaceNormals;
    in.NormalSource = GeometricNormalSource::DepthReconstruction;
    for (uint32_t j = 0; j < c.Height; ++j)
        for (uint32_t i = 0; i < c.Width; ++i) {
            const size_t bi = static_cast<size_t>(j + scene.PadTop) * in.BufferWidth + i + scene.PadLeft;
            const double u = (i + 0.5) / c.Width, v = (j + 0.5) / c.Height;
            const Vec3 ray{in.Camera.ViewX[0] * u + in.Camera.ViewX[1] * v + in.Camera.ViewX[2],
                           in.Camera.ViewY[0] * u + in.Camera.ViewY[1] * v + in.Camera.ViewY[2], -1.0};
            const auto hit = Intersect(scene.Primitives, {}, ray, c.Near, std::isinf(c.Far) ? 1.0e300 : c.Far);
            if (!hit) {
                in.Depth[bi] = EncodeDepth(c.Far, c);
                in.Class[bi] = static_cast<uint8_t>(TexelClass::Sky);
                in.Radiance[3 * bi] = in.Radiance[3 * bi + 1] = in.Radiance[3 * bi + 2] = 0.0F;
                continue;
            }
            const Primitive& p = scene.Primitives[hit->Primitive];
            in.Depth[bi] = EncodeDepth(hit->Distance, c);
            in.Class[bi] = static_cast<uint8_t>(p.TwoSided ? TexelClass::TwoSided : TexelClass::OneSided);
            const Rgb radiance = hit->FrontFace ? (p.CheckerPeriod > 0.0 ? p.RadianceAt(Scale(ray, hit->Distance))
                                                                         : p.Radiance)
                                                : Rgb{};
            in.Radiance[3 * bi] = static_cast<float>(radiance.R);
            in.Radiance[3 * bi + 1] = static_cast<float>(radiance.G);
            in.Radiance[3 * bi + 2] = static_cast<float>(radiance.B);
            const Vec3 visible = Dot(hit->Normal, ray) < 0.0 ? hit->Normal : Scale(hit->Normal, -1.0);
            in.FaceNormals[3 * bi] = static_cast<float>(visible.X);
            in.FaceNormals[3 * bi + 1] = static_cast<float>(visible.Y);
            in.FaceNormals[3 * bi + 2] = static_cast<float>(visible.Z);
            if (scene.ShadingGuide) {
                // The shading normal on the visible side (same flip as the face).
                const Vec3 shade = Dot(hit->Normal, ray) < 0.0 ? hit->Shading : Scale(hit->Shading, -1.0);
                in.ShadingNormals[3 * bi] = static_cast<float>(shade.X);
                in.ShadingNormals[3 * bi + 1] = static_cast<float>(shade.Y);
                in.ShadingNormals[3 * bi + 2] = static_cast<float>(shade.Z);
            }
        }
    for (const auto& rect : scene.Unavailable)
        for (uint32_t j = rect[1]; j < std::min(rect[3], c.Height); ++j)
            for (uint32_t i = rect[0]; i < std::min(rect[2], c.Width); ++i)
                in.Class[static_cast<size_t>(j + scene.PadTop) * in.BufferWidth + i + scene.PadLeft] =
                    static_cast<uint8_t>(TexelClass::Unavailable);
    return in;
}

void QuantizeFaceNormals8(GatherInput& input) {
    for (float& component : input.FaceNormals) {
        const double code = std::round((0.5 * std::clamp(static_cast<double>(component), -1.0, 1.0) + 0.5) * 255.0);
        component = static_cast<float>(code / 255.0 * 2.0 - 1.0);
    }
}

void QuantizeShadingNormals8(GatherInput& input) {
    for (float& component : input.ShadingNormals) {
        const double code = std::round((0.5 * std::clamp(static_cast<double>(component), -1.0, 1.0) + 0.5) * 255.0);
        component = static_cast<float>(code / 255.0 * 2.0 - 1.0);
    }
}

Scene Scaled(const Scene& scene, double factor) {
    Scene out = scene;
    for (auto& p : out.Primitives) {
        p.Centre = Scale(p.Centre, factor);
        p.Half = Scale(p.Half, factor);
        for (Vec3& vertex : p.Vertices) vertex = Scale(vertex, factor);
    }
    out.Camera.Near *= factor;
    out.Camera.Far *= factor;
    return out;
}

SceneField::SceneField(const std::vector<Primitive>& primitives, const Vec3& receiver, double radius)
    : mPrimitives(primitives), mReceiver(receiver), mRadius(radius),
      mTMin(1.0e-7 * std::sqrt(Dot(receiver, receiver))) {}

FieldSample SceneField::Sample(const Vec3& omega) const {
    const auto hit = Intersect(mPrimitives, mReceiver, omega, mTMin, mRadius);
    if (!hit) return {};
    FieldSample sample;
    sample.State = DirectionState::Hit;
    if (hit->FrontFace) {
        const Primitive& p = mPrimitives[hit->Primitive];
        sample.Radiance = p.CheckerPeriod > 0.0 ? p.RadianceAt(Add(mReceiver, Scale(omega, hit->Distance))) : p.Radiance;
    }
    return sample;
}

void SceneField::SectionBreakpoints(const Vec3& v, const Vec3& t, std::vector<double>& thetas) const {
    // Cross-section of every face with the section plane through the
    // receiver: a 2D segment in (v, t) coordinates. The first hit (and its
    // emitting side) changes only at segment endpoints, at crossings of two
    // segments and where a segment crosses the R circle.
    std::vector<Face> faces;
    for (size_t i = 0; i < mPrimitives.size(); ++i) AppendFaces(mPrimitives[i], i, faces);
    struct Segment {
        double X0, Y0, X1, Y1;
        size_t Primitive;
    };
    std::vector<Segment> segments;
    const auto angle = [&](double x, double y) { thetas.push_back(WrapAngle(std::atan2(y, x))); };
    for (const Face& f : faces) {
        const Vec3 offset = Sub(mReceiver, f.Centre);
        const double a = Dot(f.Normal, v), b = Dot(f.Normal, t), c = Dot(f.Normal, offset);
        const double norm2 = a * a + b * b;
        if (!(norm2 > 1.0e-24)) continue;
        const double x0 = -c * a / norm2, y0 = -c * b / norm2;
        const double norm = std::sqrt(norm2);
        const double ex = -b / norm, ey = a / norm;
        double sMin = -std::numeric_limits<double>::infinity(), sMax = std::numeric_limits<double>::infinity();
        bool empty = false;
        for (int k = 0; k < 2 && !empty; ++k) {
            const Vec3& axis = k == 0 ? f.U : f.V;
            const double half = k == 0 ? f.HalfU : f.HalfV;
            const double base = Dot(offset, axis) + x0 * Dot(v, axis) + y0 * Dot(t, axis);
            const double slope = ex * Dot(v, axis) + ey * Dot(t, axis);
            if (!(std::abs(slope) > 1.0e-15)) {
                if (std::abs(base) > half) empty = true;
                continue;
            }
            double s1 = (-half - base) / slope, s2 = (half - base) / slope;
            if (s1 > s2) std::swap(s1, s2);
            sMin = std::max(sMin, s1);
            sMax = std::min(sMax, s2);
        }
        if (empty || !(sMax > sMin) || !std::isfinite(sMin) || !std::isfinite(sMax)) continue;
        const Segment segment{x0 + sMin * ex, y0 + sMin * ey, x0 + sMax * ex, y0 + sMax * ey, f.Primitive};
        segments.push_back(segment);
        angle(segment.X0, segment.Y0);
        angle(segment.X1, segment.Y1);
        // R circle: |x0 + s e| = R.
        const double proj = x0 * ex + y0 * ey;
        const double disc = proj * proj - (x0 * x0 + y0 * y0 - mRadius * mRadius);
        if (disc >= 0.0)
            for (const double s : {-proj - std::sqrt(disc), -proj + std::sqrt(disc)})
                if (s > sMin && s < sMax) angle(x0 + s * ex, y0 + s * ey);
    }
    for (size_t i = 0; i < segments.size(); ++i)
        for (size_t j = i + 1; j < segments.size(); ++j) {
            const Segment& p = segments[i];
            const Segment& q = segments[j];
            if (p.Primitive == q.Primitive) continue;
            const double rx = p.X1 - p.X0, ry = p.Y1 - p.Y0, sx = q.X1 - q.X0, sy = q.Y1 - q.Y0;
            const double denominator = Cross2(rx, ry, sx, sy);
            if (!(std::abs(denominator) > 0.0)) continue;
            const double qpx = q.X0 - p.X0, qpy = q.Y0 - p.Y0;
            const double tp = Cross2(qpx, qpy, sx, sy) / denominator;
            const double tq = Cross2(qpx, qpy, rx, ry) / denominator;
            if (tp > 0.0 && tp < 1.0 && tq > 0.0 && tq < 1.0) angle(p.X0 + tp * rx, p.Y0 + tp * ry);
        }
}

GroundTruth IntegrateGroundTruth(const Scene& scene, const GatherInput& input, const PreparedGeometry& geometry,
                                 uint32_t x, uint32_t y, double radius, double tolerance) {
    GroundTruth out;
    const size_t index = static_cast<size_t>(y) * geometry.Width + x;
    const Vec3 p = geometry.Position(input.Camera, x, y);
    const Vec3 n = geometry.Normal[index];
    const SceneField field(scene.Primitives, p, radius);
    const SectionFrame frame = SectionFrame::FromView(Scale(p, -1.0));
    ContinuousOptions options;
    options.Tolerance = tolerance;
    options.MaxPanels = 1024;
    options.Inner = {1.0e-10, 1.0e-14, 64, true};
    const ContinuousResult result = IntegrateContinuous(field, frame, n, {}, options);
    out.J = result.J;
    out.A = result.A;
    out.Converged = result.Converged;
    out.Panels = result.Panels;
    out.LastCorrection = result.LastCorrection;
    return out;
}

// ---------------------------------------------------------------------------
// Catalogue. View space: camera at the origin looking down -Z, +Y up; the
// floor's top face is y = -1. Units are scene units (metre-like).

PinholeCamera StandardCamera(uint32_t width, uint32_t height) {
    PinholeCamera camera;
    camera.Width = width;
    camera.Height = height;
    return camera;
}

Primitive BlackFloor(double radiance) {
    return Primitive::Box({0.0, -1.5, -50.0}, {60.0, 0.5, 50.0}, {radiance, radiance, radiance});
}

Scene BlackOccluderScene(uint32_t width, uint32_t height, bool withOccluder) {
    Scene scene;
    scene.Name = withOccluder ? "t06_black_occluder" : "t06_wall_only";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -9.5}, {8.0, 3.0, 0.5}, {4.0, 3.0, 2.0}));
    if (withOccluder) scene.Primitives.push_back(Primitive::Box({0.3, -0.55, -6.8}, {0.6, 0.45, 0.5}, {}));
    return scene;
}

Scene ThinBarScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t07_thin_bar";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -8.5}, {8.0, 3.0, 0.5}, {2.0, 2.5, 3.0}));
    scene.Primitives.push_back(Primitive::Box({0.0, -0.45, -6.0}, {4.0, 0.05, 0.05}, {}));
    return scene;
}

Scene ThickBlockScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t07_thick_block";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -11.5}, {10.0, 3.5, 0.5}, {3.0, 3.0, 3.0}));
    scene.Primitives.push_back(Primitive::Box({-0.5, 0.0, -7.0}, {0.8, 1.0, 1.75}, {}));
    return scene;
}

Scene NearPlaneScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t08_near_plane";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    // Back face at z = -0.13 and a bright box protruding from it up to 0.1%
    // beyond the near plane (front face at z = -0.1001).
    scene.Primitives.push_back(Primitive::Box({0.0, 0.0, -0.18}, {0.3, 0.3, 0.05}, {0.2, 0.2, 0.2}));
    scene.Primitives.push_back(Primitive::Box({0.015, 0.0, -0.11505}, {0.006, 0.02, 0.01495}, {3.0, 2.0, 1.0}));
    return scene;
}

Scene SlopedPlaneScene(uint32_t width, uint32_t height, double pitch) {
    Scene scene;
    scene.Name = "sloped_plane";
    scene.Camera = StandardCamera(width, height);
    const double yaw = 17.0 * kPi / 180.0;
    // Plane axes: X and the pitched Y axis, both rotated about Y by the yaw
    // ((x, z) -> (x cos + z sin, -x sin + z cos)) so no axis is screen-aligned.
    const Vec3 u{std::cos(yaw), 0.0, -std::sin(yaw)};
    const Vec3 v{-std::sin(pitch) * std::sin(yaw), std::cos(pitch), -std::sin(pitch) * std::cos(yaw)};
    scene.Primitives.push_back(Primitive::Rectangle({0.0, -0.5, -6.0}, u, v, 200.0, 200.0, {1.0, 1.0, 1.0}, false));
    return scene;
}

Scene CornerRoomScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "corner_room";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor(0.2));
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -8.5}, {6.0, 3.0, 0.5}, {0.3, 0.3, 0.3}));
    scene.Primitives.push_back(Primitive::Box({-3.5, 1.5, -5.0}, {0.5, 3.0, 4.0}, {2.5, 0.5, 0.3}));
    scene.Primitives.push_back(Primitive::Box({3.5, 1.5, -5.0}, {0.5, 3.0, 4.0}, {0.05, 0.05, 0.05}));
    scene.Primitives.push_back(Primitive::Box({-2.4, -0.6, -7.4}, {0.4, 0.4, 0.4}, {0.2, 0.8, 0.3}));
    return scene;
}

Scene PillarsScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "pillars";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor(0.1));
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -10.5}, {8.0, 3.0, 0.5}, {1.5, 1.5, 1.2}));
    scene.Primitives.push_back(Primitive::Box({-1.2, 0.0, -5.5}, {0.2, 1.0, 0.2}, {0.2, 0.4, 2.0}));
    scene.Primitives.push_back(Primitive::Box({0.4, 0.0, -6.5}, {0.2, 1.0, 0.2}, {2.0, 0.8, 0.1}));
    scene.Primitives.push_back(Primitive::Box({1.6, 0.0, -5.0}, {0.2, 1.0, 0.2}, {}));
    scene.Primitives.push_back(Primitive::Box({-0.3, -0.75, -4.2}, {0.25, 0.25, 0.25}, {0.9, 0.9, 0.9}));
    return scene;
}

Scene WordBoundaryScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t05_word_boundaries";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor(0.3));
    scene.Primitives.push_back(Primitive::Box({-0.8, -0.4, -4.6}, {0.3, 0.6, 0.3}, {3.0, 0.2, 0.2}));
    scene.Primitives.push_back(Primitive::Box({0.7, -0.5, -5.2}, {0.4, 0.5, 0.2}, {0.2, 3.0, 0.2}));
    scene.Primitives.push_back(Primitive::TiltedBox({0.0, -0.8, -6.0}, {1.5, 0.2, 0.4}, 0.6, {0.2, 0.2, 3.0}));
    return scene;
}

Scene PanelScene(uint32_t width, uint32_t height, bool twoSided) {
    Scene scene;
    scene.Name = twoSided ? "panel_two_sided" : "panel_one_sided";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Rectangle({0.9, -0.4, -6.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.5, 0.6,
                                                    {3.0, 2.0, 1.0}, twoSided));
    return scene;
}

Scene OverhangScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t09_overhang";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -10.5}, {8.0, 3.0, 0.5}, {2.0, 2.0, 2.0}));
    // Only its front face (z = -2) is visible to the camera; its underside
    // (y = -0.2) spans z in [-5.8, -2].
    scene.Primitives.push_back(Primitive::Box({0.2, 0.3, -3.9}, {0.6, 0.5, 1.9}, {0.5, 0.5, 0.5}));
    return scene;
}

Scene CheckerRoomScene(uint32_t width, uint32_t height) {
    Scene scene = CornerRoomScene(width, height);
    scene.Name = "checker_room";
    // Floor, back wall, red wall: checkers of 0.25 / 0.4 / 0.3 scene units.
    scene.Primitives[0].CheckerPeriod = 0.25;
    scene.Primitives[0].CheckerRadiance = {0.6, 0.6, 0.6};
    scene.Primitives[1].CheckerPeriod = 0.4;
    scene.Primitives[1].CheckerRadiance = {1.5, 1.2, 0.2};
    scene.Primitives[2].CheckerPeriod = 0.3;
    scene.Primitives[2].CheckerRadiance = {0.3, 0.1, 0.05};
    return scene;
}

// --- M8 scenes: low-poly meshes with interpolated vertex normals.
namespace {

// One-sided bright wall in the plane x = -1.4 facing +X, z in [-6.2, -2.2],
// y in [-1.2, 1.2]: on screen and within 4.5 units of every M8 receiver.
Primitive M8SourceWall() {
    return Primitive::Rectangle({-1.4, 0.0, -4.2}, {0.0, 0.0, -1.0}, {0.0, 1.0, 0.0}, 2.0, 1.2, {3.0, 2.6, 2.2}, false);
}

Vec3 Radial(double theta) { return {std::cos(theta), 0.0, std::sin(theta)}; }

// Facet strips of a vertical prism around `axis` (radius, half height) over
// the angles [begin, end] (direction (cos, 0, sin)); outward or inward. Each
// strip spans the chord between two vertices; smooth strips interpolate the
// radial vertex normals.
void AppendPrism(std::vector<Primitive>& out, const Vec3& axis, double radius, double halfHeight, uint32_t facets,
                 double begin, double end, bool inward, bool smooth, const Rgb& radiance) {
    const double step = (end - begin) / facets;
    for (uint32_t k = 0; k < facets; ++k) {
        const double a = begin + step * k, b = a + step, m = a + 0.5 * step;
        const Vec3 centre = Add(axis, Scale(Radial(m), radius * std::cos(0.5 * step)));
        const Vec3 tangent{-std::sin(m), 0.0, std::cos(m)}; // increasing angle
        const double halfU = radius * std::sin(0.5 * step);
        const double sign = inward ? -1.0 : 1.0;
        // Outward strips run their U axis toward decreasing angle (facing +radial).
        const Vec3 axisU = Scale(tangent, inward ? 1.0 : -1.0);
        const Vec3 atMinusU = Scale(Radial(inward ? a : b), sign), atPlusU = Scale(Radial(inward ? b : a), sign);
        out.push_back(smooth ? Primitive::SmoothStrip(centre, axisU, {0.0, 1.0, 0.0}, halfU, halfHeight, atMinusU,
                                                      atPlusU, radiance)
                             : Primitive::Rectangle(centre, axisU, {0.0, 1.0, 0.0}, halfU, halfHeight, radiance, false));
    }
}

// Icosahedron subdivided `levels` times on the unit sphere, outward winding.
std::vector<std::array<Vec3, 3>> UnitIcosphere(uint32_t levels) {
    const double phi = 0.5 * (1.0 + std::sqrt(5.0));
    const std::array<Vec3, 12> v{{{-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0}, {0, -1, phi}, {0, 1, phi},
                                  {0, -1, -phi}, {0, 1, -phi}, {phi, 0, -1}, {phi, 0, 1}, {-phi, 0, -1}, {-phi, 0, 1}}};
    constexpr int kFaces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9},  {5, 11, 4},
                                   {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6},  {3, 6, 8},
                                   {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    std::vector<std::array<Vec3, 3>> faces;
    for (const auto& f : kFaces) faces.push_back({Normalize(v[f[0]]), Normalize(v[f[1]]), Normalize(v[f[2]])});
    for (uint32_t level = 0; level < levels; ++level) {
        std::vector<std::array<Vec3, 3>> finer;
        for (const auto& f : faces) {
            const Vec3 ab = Normalize(Add(f[0], f[1])), bc = Normalize(Add(f[1], f[2])), ca = Normalize(Add(f[2], f[0]));
            finer.push_back({f[0], ab, ca});
            finer.push_back({ab, f[1], bc});
            finer.push_back({ca, bc, f[2]});
            finer.push_back({ab, bc, ca});
        }
        faces.swap(finer);
    }
    for (auto& f : faces)
        if (Dot(Cross(Sub(f[1], f[0]), Sub(f[2], f[0])), Add(Add(f[0], f[1]), f[2])) < 0.0) std::swap(f[1], f[2]);
    return faces;
}

} // namespace

Scene LowPolyCylinderScene(uint32_t width, uint32_t height, uint32_t sides, bool smooth) {
    Scene scene;
    scene.Name = "m8_cylinder_" + std::to_string(sides) + (smooth ? "_smooth" : "_flat");
    scene.Camera = StandardCamera(width, height);
    scene.ShadingGuide = true;
    scene.Primitives.push_back(M8SourceWall());
    // Vertices start 0.3 rad from +X: no facet edge on the view axis.
    AppendPrism(scene.Primitives, {0.3, 0.0, -3.2}, 0.8, 0.8, sides, 0.3, 0.3 + 2.0 * kPi, false, smooth,
                {0.3, 0.3, 0.3});
    return scene;
}

Scene LowPolySphereScene(uint32_t width, uint32_t height, uint32_t levels, bool smooth, bool onFloor) {
    Scene scene;
    scene.Name = "m8_sphere_" + std::to_string(levels) + (smooth ? "_smooth" : "_flat") + (onFloor ? "_on_floor" : "");
    scene.Camera = StandardCamera(width, height);
    scene.ShadingGuide = true;
    scene.Primitives.push_back(M8SourceWall());
    if (onFloor)
        scene.Primitives.push_back(Primitive::Rectangle({0.3, -0.8, -3.7}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, 3.0, 2.5,
                                                        {0.25, 0.25, 0.25}, false));
    const Vec3 centre{0.3, 0.0, -3.2};
    const double radius = 0.8;
    for (const auto& f : UnitIcosphere(levels)) {
        const Vec3 a = Add(centre, Scale(f[0], radius)), b = Add(centre, Scale(f[1], radius)),
                   c = Add(centre, Scale(f[2], radius));
        scene.Primitives.push_back(smooth ? Primitive::SmoothTriangle(a, b, c, f[0], f[1], f[2], {0.3, 0.3, 0.3})
                                          : Primitive::Triangle(a, b, c, {0.3, 0.3, 0.3}));
    }
    return scene;
}

Scene TroughScene(uint32_t width, uint32_t height, uint32_t sides, bool smooth) {
    Scene scene;
    scene.Name = "m8_trough_" + std::to_string(sides) + (smooth ? "_smooth" : "_flat");
    scene.Camera = StandardCamera(width, height);
    scene.ShadingGuide = true;
    // The known source: a bright floor in front of and inside the trough.
    scene.Primitives.push_back(Primitive::Rectangle({0.2, -0.9, -3.1}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, 2.0, 1.5,
                                                    {2.0, 2.0, 2.0}, false));
    AppendPrism(scene.Primitives, {0.2, 0.0, -3.6}, 1.0, 0.9, sides, kPi, 2.0 * kPi, true, smooth, {0.1, 0.1, 0.1});
    return scene;
}

Scene WallFloorScene(uint32_t width, uint32_t height, double bendDegrees) {
    Scene scene;
    scene.Name = "m8_wall_floor_bend" + std::to_string(static_cast<int>(bendDegrees));
    scene.Camera = StandardCamera(width, height);
    scene.ShadingGuide = true;
    const double s = std::sin(0.5 * bendDegrees * kPi / 180.0), c = std::cos(0.5 * bendDegrees * kPi / 180.0);
    // The known source: the bright back wall (z = -7, facing +Z); grey floor (y = -1).
    scene.Primitives.push_back(Primitive::SmoothStrip({0.0, 0.5, -7.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 3.0, 1.5,
                                                      {-s, 0.0, c}, {s, 0.0, c}, {1.0, 1.0, 1.0}));
    scene.Primitives.push_back(Primitive::SmoothStrip({0.0, -1.0, -4.5}, {1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, 3.0, 2.5,
                                                      {-s, c, 0.0}, {s, c, 0.0}, {0.3, 0.3, 0.3}));
    return scene;
}

void FacePlane(const Primitive& p, int32_t face, Vec3& normal, double& offset) {
    if (p.Shape != Primitive::Kind::Box) {
        normal = p.Axes[2];
        offset = Dot(normal, p.Centre);
        return;
    }
    const int axis = face / 2;
    const double sign = (face % 2) == 0 ? 1.0 : -1.0;
    const double half = axis == 0 ? p.Half.X : (axis == 1 ? p.Half.Y : p.Half.Z);
    normal = Scale(p.Axes[axis], sign);
    offset = Dot(normal, p.Centre) + half;
}

SurfaceIds RasterizeSurfaceIds(const Scene& scene) {
    const PinholeCamera& c = scene.Camera;
    const DepthCamera camera = MakeDepthCamera(c);
    SurfaceIds out;
    out.Primitive.assign(static_cast<size_t>(c.Width) * c.Height, -1);
    out.Face.assign(out.Primitive.size(), -1);
    out.Distance.assign(out.Primitive.size(), 0.0);
    for (uint32_t j = 0; j < c.Height; ++j)
        for (uint32_t i = 0; i < c.Width; ++i) {
            const double u = (i + 0.5) / c.Width, v = (j + 0.5) / c.Height;
            const Vec3 ray{camera.ViewX[0] * u + camera.ViewX[1] * v + camera.ViewX[2],
                           camera.ViewY[0] * u + camera.ViewY[1] * v + camera.ViewY[2], -1.0};
            const auto hit = Intersect(scene.Primitives, {}, ray, c.Near, std::isinf(c.Far) ? 1.0e300 : c.Far);
            if (!hit) continue;
            const size_t index = static_cast<size_t>(j) * c.Width + i;
            out.Primitive[index] = static_cast<int32_t>(hit->Primitive);
            out.Distance[index] = hit->Distance;
            const Primitive& p = scene.Primitives[hit->Primitive];
            int32_t face = 0;
            if (p.Shape == Primitive::Kind::Box)
                for (int axis = 0; axis < 3; ++axis) {
                    const double d = Dot(hit->Normal, p.Axes[axis]);
                    if (std::abs(d) > 0.5) face = 2 * axis + (d < 0.0 ? 1 : 0);
                }
            out.Face[index] = face;
        }
    for (const auto& rect : scene.Unavailable)
        for (uint32_t j = rect[1]; j < std::min(rect[3], c.Height); ++j)
            for (uint32_t i = rect[0]; i < std::min(rect[2], c.Width); ++i)
                out.Primitive[static_cast<size_t>(j) * c.Width + i] = -2;
    return out;
}

} // namespace Fast::Renderer::DiffuseGi::Synthetic
