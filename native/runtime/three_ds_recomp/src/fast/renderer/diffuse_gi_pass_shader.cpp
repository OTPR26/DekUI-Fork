#include "fast/renderer/diffuse_gi_pass_shader.h"

// Kernel catalogue of the M5 runtime pass, its own output and upsample
// kernels and the module's normal code. The M2-M4 kernels are referenced
// unchanged; the output and upsample kernels, the normal code and their FP32
// twins are own code (no third-party shader or upsampler was consulted).

#include "fast/renderer/diffuse_gi_gather_shader.h"
#include "fast/renderer/diffuse_gi_m4_shader.h"
#include "fast/renderer/diffuse_gi_temporal_shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr const char* kOutputShader = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std140, set = 0, binding = 0) uniform OutputParams {
    uvec4 extent;   // buffer width, buffer height, viewport x, viewport y
    uvec4 viewport; // viewport width, viewport height, mode (0 final, 1 neutral), write A (1 / 0)
    vec4 limits;    // largest stored |J|
} p;
layout(std430, set = 0, binding = 1) readonly buffer Colour { vec4 colour[]; };
layout(std430, set = 0, binding = 2) readonly buffer Geometry { vec4 geo[]; };
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D illumination;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D accessibility;

bool finite1(float v) { return !isnan(v) && !isinf(v); }

float stored(float v, float limit) { return finite1(v) ? clamp(v, -limit, limit) : 0.0; }

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= p.extent.x || id.y >= p.extent.y) return; // no barriers in this kernel
    vec4 j = vec4(0.0);
    float a = 1.0;
    bool inside = id.x >= p.extent.z && id.y >= p.extent.w && id.x - p.extent.z < p.viewport.x &&
                  id.y - p.extent.w < p.viewport.y;
    if (p.viewport.z == 0u && inside) {
        uint i = (id.y - p.extent.w) * p.viewport.x + (id.x - p.extent.z);
        vec4 g = geo[i];
        bool receiver = g.w > 0.0 && finite1(g.w) && finite1(g.x) && finite1(g.y) && finite1(g.z) &&
                        g.x * g.x + g.y * g.y + g.z * g.z > 0.0;
        if (receiver) {
            vec4 c = colour[i];
            float limit = p.limits.x;
            j = vec4(stored(c.x, limit), stored(c.y, limit), stored(c.z, limit), 1.0);
            a = finite1(c.w) ? clamp(c.w, 0.0, 1.0) : 1.0;
        }
    }
    imageStore(illumination, ivec2(id), j);
    if (p.viewport.w != 0u) imageStore(accessibility, ivec2(id), vec4(a, 0.0, 0.0, 0.0));
}
)glsl";

// Octahedral normal code of the module (diffuse_gi_normal_code.h).
constexpr const char* kNormalCodec = kDiffuseGiNormalCodecGlsl;

// Geometry-guided upsampling of the work-scale result to the output extent
// (own design from specification section 8: "risalire con un filtro guidato
// dalla geometria"). See diffuse_gi_pass_shader.h.
constexpr const char* kUpsampleShader = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std140, set = 0, binding = 0) uniform UpsampleParams {
    uvec4 output_extent; // output width, output height, write A (1 / 0), flags (1 shading normals, M8)
    uvec4 work;          // work width, work height, factor, phase
    vec4 output_x;       // output UV -> ray map (ax, bx, cx), hard-edge ratio (M8)
    vec4 output_y;
    vec4 work_x;         // work UV -> ray map
    vec4 work_y;
    vec4 surface;        // plane sine, plane depth (relative), normal cos, largest stored |J|
} p;
layout(std430, set = 0, binding = 1) readonly buffer Colour { vec4 colour[]; };
layout(std430, set = 0, binding = 2) readonly buffer Geometry { vec4 geo[]; };
layout(std430, set = 0, binding = 3) readonly buffer OutputGuide { uvec2 guide[]; };
// M8: the work pixels' shading normals and the output texels' shading codes.
layout(std430, set = 0, binding = 4) readonly buffer ShadingGeometry { vec4 shading_geo[]; };
layout(std430, set = 0, binding = 5) readonly buffer OutputShading { uint shading_guide[]; };
layout(set = 0, binding = 6, rgba16f) uniform writeonly image2D illumination;
layout(set = 0, binding = 7, r32f) uniform writeonly image2D accessibility;
)glsl";

constexpr const char* kUpsampleMain = R"glsl(
#define INF uintBitsToFloat(0x7F800000u)
bool finite1(float v) { return !isnan(v) && !isinf(v); }
bool finite4(vec4 v) { return finite1(v.x) && finite1(v.y) && finite1(v.z) && finite1(v.w); }
float stored(float v, float limit) { return finite1(v) ? clamp(v, -limit, limit) : 0.0; }
float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float vlength(vec3 a) { return sqrt(vdot(a, a)); }
vec3 map_ray(vec4 rx, vec4 ry, float px, float py, uint w, uint h) {
    float u = px / float(w);
    float v = py / float(h);
    return vec3(rx.x * u + rx.y * v + rx.z, ry.x * u + ry.y * v + ry.z, -1.0);
}
bool receiver(vec4 g) {
    return g.w > 0.0 && finite1(g.w) && finite1(g.x) && finite1(g.y) && finite1(g.z) &&
           g.x * g.x + g.y * g.y + g.z * g.z > 0.0;
}
// The work sample q lies on the output texel's tangent plane and faces the same way.
bool compatible(vec3 pa, vec3 na, float zp, vec3 qb, vec3 nb) {
    vec3 d = qb - pa;
    return abs(vdot(na, d)) <= p.surface.x * vlength(d) + p.surface.y * zp && vdot(na, nb) >= p.surface.z;
}
// M8: a smooth crease between the output texel and a work sample (the
// temporal module's rule: shading normals agree and turn less than the faces,
// the face planes meet between the two).
bool smooth_crease(vec3 pa, vec3 na, vec3 sa, float zp, vec3 qb, vec3 nb, vec3 sb) {
    if (!(vdot(sa, sa) > 0.0) || !(vdot(sb, sb) > 0.0)) return false;
    float cs = vdot(sa, sb);
    float cf = vdot(na, nb);
    if (!(cs >= p.surface.z) || !(1.0 - cs < p.output_x.w * (1.0 - cf))) return false;
    vec3 d = qb - pa;
    float sf = sqrt(max(1.0 - cf * cf, 0.0));
    float tolerance = (p.surface.x + sf) * vlength(d) + p.surface.y * zp;
    return abs(vdot(na, d)) <= tolerance && abs(vdot(nb, d)) <= tolerance;
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= p.output_extent.x || id.y >= p.output_extent.y) return; // no barriers in this kernel
    vec4 j = vec4(0.0);
    float a = 1.0;
    uvec2 g0 = guide[id.y * p.output_extent.x + id.x];
    float z = uintBitsToFloat(g0.x);
    vec3 n = dgi_decode_normal(g0.y);
    // M8: the output texel's shading normal, decoded at the first tap whose
    // surface test fails.
    bool m8 = (p.output_extent.w & 1u) != 0u;
    bool so_ready = false;
    vec3 so = vec3(0.0);
    if (z > 0.0 && finite1(z) && vdot(n, n) > 0.0) {
        uint W = p.work.x;
        uint H = p.work.y;
        vec3 pos = map_ray(p.output_x, p.output_y, float(id.x) + 0.5, float(id.y) + 0.5, p.output_extent.x,
                           p.output_extent.y) * z;
        // Continuous work-pixel coordinates (centres at i + 1/2) of this texel.
        float wx = (float(id.x) - float(p.work.w)) / float(p.work.z) + 0.5;
        float wy = (float(id.y) - float(p.work.w)) / float(p.work.z) + 0.5;
        int x0 = int(floor(wx - 0.5));
        int y0 = int(floor(wy - 0.5));
        float fx = wx - 0.5 - float(x0);
        float fy = wy - 0.5 - float(y0);
        vec4 sum = vec4(0.0);
        float weight = 0.0;
        for (int b = 0; b < 2; ++b) {
            for (int c = 0; c < 2; ++c) {
                float w = (c == 0 ? 1.0 - fx : fx) * (b == 0 ? 1.0 - fy : fy);
                int qx = x0 + c;
                int qy = y0 + b;
                if (!(w > 0.0) || qx < 0 || qy < 0 || qx >= int(W) || qy >= int(H)) continue; // before any read
                uint q = uint(qy) * W + uint(qx);
                vec4 gq = geo[q];
                if (!receiver(gq)) continue;
                vec3 pq = map_ray(p.work_x, p.work_y, float(qx) + 0.5, float(qy) + 0.5, W, H) * gq.w;
                if (!compatible(pos, n, z, pq, gq.xyz)) {
                    if (!m8) continue;
                    if (!so_ready) {
                        so = dgi_decode_normal(shading_guide[id.y * p.output_extent.x + id.x]);
                        so_ready = true;
                    }
                    if (!smooth_crease(pos, n, so, z, pq, gq.xyz, shading_geo[q].xyz)) continue;
                }
                vec4 cq = colour[q];
                if (!finite4(cq)) continue;
                sum += w * cq;
                weight += w;
            }
        }
        if (!(weight > 0.0)) {
            // No bilinear tap on this surface: the nearest compatible work
            // sample of the surrounding 4 x 4, or no estimate.
            float best = INF;
            for (int b = -1; b < 3; ++b) {
                for (int c = -1; c < 3; ++c) {
                    int qx = x0 + c;
                    int qy = y0 + b;
                    if (qx < 0 || qy < 0 || qx >= int(W) || qy >= int(H)) continue; // before any read
                    uint q = uint(qy) * W + uint(qx);
                    vec4 gq = geo[q];
                    if (!receiver(gq)) continue;
                    vec3 pq = map_ray(p.work_x, p.work_y, float(qx) + 0.5, float(qy) + 0.5, W, H) * gq.w;
                    if (!compatible(pos, n, z, pq, gq.xyz)) {
                        if (!m8) continue;
                        if (!so_ready) {
                            so = dgi_decode_normal(shading_guide[id.y * p.output_extent.x + id.x]);
                            so_ready = true;
                        }
                        if (!smooth_crease(pos, n, so, z, pq, gq.xyz, shading_geo[q].xyz)) continue;
                    }
                    vec4 cq = colour[q];
                    if (!finite4(cq)) continue;
                    float d = vlength(pq - pos);
                    if (d < best) {
                        best = d;
                        sum = cq;
                        weight = 1.0;
                    }
                }
            }
        }
        if (weight > 0.0) {
            vec4 cj = sum / weight;
            float limit = p.surface.w;
            j = vec4(stored(cj.x, limit), stored(cj.y, limit), stored(cj.z, limit), 1.0);
            a = finite1(cj.w) ? clamp(cj.w, 0.0, 1.0) : 1.0;
        }
    }
    imageStore(illumination, ivec2(id), j);
    if (p.output_extent.z != 0u) imageStore(accessibility, ivec2(id), vec4(a, 0.0, 0.0, 0.0));
}
)glsl";

float Stored(float v, float limit) noexcept { return std::isfinite(v) ? std::clamp(v, -limit, limit) : 0.0F; }

struct F3 {
    float X = 0.0F, Y = 0.0F, Z = 0.0F;
};
float Dot(const F3& a, const F3& b) noexcept { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
F3 Ray(const std::array<float, 4>& rx, const std::array<float, 4>& ry, float px, float py, uint32_t w,
       uint32_t h) noexcept {
    const float u = px / static_cast<float>(w);
    const float v = py / static_cast<float>(h);
    return {rx[0] * u + rx[1] * v + rx[2], ry[0] * u + ry[1] * v + ry[2], -1.0F};
}
bool Receiver(const float* g) noexcept {
    return g[3] > 0.0F && std::isfinite(g[3]) && std::isfinite(g[0]) && std::isfinite(g[1]) && std::isfinite(g[2]) &&
           g[0] * g[0] + g[1] * g[1] + g[2] * g[2] > 0.0F;
}
bool Finite4(const float* c) noexcept {
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]) && std::isfinite(c[3]);
}

} // namespace

std::string BuildDiffuseGiOutputShader() { return kOutputShader; }
std::string DiffuseGiNormalCodecGlsl() { return kNormalCodec; }
std::string BuildDiffuseGiUpsampleShader() { return std::string(kUpsampleShader) + kNormalCodec + kUpsampleMain; }

std::vector<PassKernelSource> BuildDiffuseGiPassKernels() {
    std::vector<PassKernelSource> kernels;
    kernels.reserve(kPassKernelCount);
    // The M2-M4 kernels with the M8 smooth-shaded paths compiled in (each
    // behind its flag: the M7 arithmetic when off).
    kernels.push_back({PassKernel::Normal, "diffuse_gi_normal.comp", BuildDiffuseGiNormalShader(true)});
    kernels.push_back(
        {PassKernel::HierarchyLevel0, "diffuse_gi_hierarchy_level0.comp", BuildDiffuseGiHierarchyLevel0Shader(true)});
    kernels.push_back(
        {PassKernel::HierarchyReduce, "diffuse_gi_hierarchy_reduce.comp", BuildDiffuseGiHierarchyReduceShader(true)});
    kernels.push_back({PassKernel::Gather, "diffuse_gi_gather.comp", BuildDiffuseGiGatherM4Shader(true)});
    kernels.push_back({PassKernel::Pack, "diffuse_gi_pack.comp", BuildDiffuseGiPackShader(true)});
    kernels.push_back({PassKernel::Temporal, "diffuse_gi_temporal.comp", BuildDiffuseGiTemporalShader(true)});
    kernels.push_back({PassKernel::Filter, "diffuse_gi_filter.comp", BuildDiffuseGiFilterShader(true)});
    kernels.push_back({PassKernel::Output, "diffuse_gi_output.comp", BuildDiffuseGiOutputShader()});
    kernels.push_back({PassKernel::Upsample, "diffuse_gi_upsample.comp", BuildDiffuseGiUpsampleShader()});
    return kernels;
}

OutputTexel OutputTexelF32(const OutputShaderParams& p, uint32_t x, uint32_t y, const float* colour,
                           const float* geometry) noexcept {
    OutputTexel out;
    const bool inside = x >= p.Extent[2] && y >= p.Extent[3] && x - p.Extent[2] < p.Viewport[0] &&
                        y - p.Extent[3] < p.Viewport[1];
    if (p.Viewport[2] != kOutputModeFinal || !inside) return out;
    const size_t i = static_cast<size_t>(y - p.Extent[3]) * p.Viewport[0] + (x - p.Extent[2]);
    const float* g = geometry + 4 * i;
    if (!Receiver(g)) return out;
    const float* c = colour + 4 * i;
    const float limit = p.Limits[0];
    out.Illumination = {Stored(c[0], limit), Stored(c[1], limit), Stored(c[2], limit), 1.0F};
    out.Accessibility = std::isfinite(c[3]) ? std::clamp(c[3], 0.0F, 1.0F) : 1.0F;
    return out;
}

OutputTexel UpsampleTexelF32(const UpsampleShaderParams& p, uint32_t x, uint32_t y, const float* colour,
                             const float* geometry, const uint32_t* guide, const float* shading,
                             const uint32_t* shadingGuide) noexcept {
    OutputTexel out;
    const size_t t = static_cast<size_t>(y) * p.Output[0] + x;
    float z = 0.0F;
    std::memcpy(&z, &guide[2 * t], 4);
    const auto nn = DecodeDiffuseGiNormal(guide[2 * t + 1]);
    const F3 n{nn[0], nn[1], nn[2]};
    const bool m8 = (p.Output[3] & 1U) != 0U && shading != nullptr && shadingGuide != nullptr;
    const auto so3 = m8 ? DecodeDiffuseGiNormal(shadingGuide[t]) : std::array<float, 3>{};
    const F3 so{so3[0], so3[1], so3[2]};
    if (!(z > 0.0F && std::isfinite(z) && Dot(n, n) > 0.0F)) return out;
    const uint32_t W = p.Work[0], H = p.Work[1];
    const F3 r = Ray(p.OutputX, p.OutputY, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, p.Output[0],
                     p.Output[1]);
    const F3 pos{r.X * z, r.Y * z, r.Z * z};
    const float wx = (static_cast<float>(x) - static_cast<float>(p.Work[3])) / static_cast<float>(p.Work[2]) + 0.5F;
    const float wy = (static_cast<float>(y) - static_cast<float>(p.Work[3])) / static_cast<float>(p.Work[2]) + 0.5F;
    const int x0 = static_cast<int>(std::floor(wx - 0.5F));
    const int y0 = static_cast<int>(std::floor(wy - 0.5F));
    const float fx = wx - 0.5F - static_cast<float>(x0);
    const float fy = wy - 0.5F - static_cast<float>(y0);
    const auto sample = [&](int qx, int qy, F3& q, const float*& c) {
        if (qx < 0 || qy < 0 || qx >= static_cast<int>(W) || qy >= static_cast<int>(H)) return false;
        const size_t i = static_cast<size_t>(qy) * W + static_cast<size_t>(qx);
        const float* g = geometry + 4 * i;
        if (!Receiver(g)) return false;
        const F3 rq = Ray(p.WorkX, p.WorkY, static_cast<float>(qx) + 0.5F, static_cast<float>(qy) + 0.5F, W, H);
        q = {rq.X * g[3], rq.Y * g[3], rq.Z * g[3]};
        const F3 d{q.X - pos.X, q.Y - pos.Y, q.Z - pos.Z};
        const F3 nq{g[0], g[1], g[2]};
        if (!(std::abs(Dot(n, d)) <= p.Surface[0] * std::sqrt(Dot(d, d)) + p.Surface[1] * z &&
              Dot(n, nq) >= p.Surface[2])) {
            // M8: a smooth crease (the kernel's smooth_crease).
            if (!m8) return false;
            const float* sq3 = shading + 4 * i;
            const F3 sq{sq3[0], sq3[1], sq3[2]};
            if (!(Dot(so, so) > 0.0F) || !(Dot(sq, sq) > 0.0F)) return false;
            const float cs = Dot(so, sq);
            const float cf = Dot(n, nq);
            if (!(cs >= p.Surface[2]) || !(1.0F - cs < p.OutputX[3] * (1.0F - cf))) return false;
            const float sf = std::sqrt(std::max(1.0F - cf * cf, 0.0F));
            const float tolerance = (p.Surface[0] + sf) * std::sqrt(Dot(d, d)) + p.Surface[1] * z;
            if (!(std::abs(Dot(n, d)) <= tolerance && std::abs(Dot(nq, d)) <= tolerance)) return false;
        }
        c = colour + 4 * i;
        return Finite4(c);
    };
    float sum[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    float weight = 0.0F;
    for (int b = 0; b < 2; ++b)
        for (int c = 0; c < 2; ++c) {
            const float w = (c == 0 ? 1.0F - fx : fx) * (b == 0 ? 1.0F - fy : fy);
            if (!(w > 0.0F)) continue;
            F3 q;
            const float* cq = nullptr;
            if (!sample(x0 + c, y0 + b, q, cq)) continue;
            for (int k = 0; k < 4; ++k) sum[k] += w * cq[k];
            weight += w;
        }
    if (!(weight > 0.0F)) {
        float best = std::numeric_limits<float>::infinity();
        for (int b = -1; b < 3; ++b)
            for (int c = -1; c < 3; ++c) {
                F3 q;
                const float* cq = nullptr;
                if (!sample(x0 + c, y0 + b, q, cq)) continue;
                const F3 d{q.X - pos.X, q.Y - pos.Y, q.Z - pos.Z};
                const float dist = std::sqrt(Dot(d, d));
                if (dist < best) {
                    best = dist;
                    for (int k = 0; k < 4; ++k) sum[k] = cq[k];
                    weight = 1.0F;
                }
            }
    }
    if (!(weight > 0.0F)) return out;
    float cj[4];
    for (int k = 0; k < 4; ++k) cj[k] = sum[k] / weight;
    const float limit = p.Surface[3];
    out.Illumination = {Stored(cj[0], limit), Stored(cj[1], limit), Stored(cj[2], limit), 1.0F};
    out.Accessibility = std::isfinite(cj[3]) ? std::clamp(cj[3], 0.0F, 1.0F) : 1.0F;
    return out;
}

} // namespace Fast::Renderer::DiffuseGi
