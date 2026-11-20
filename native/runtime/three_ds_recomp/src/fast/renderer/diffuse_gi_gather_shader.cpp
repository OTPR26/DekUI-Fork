#include "fast/renderer/diffuse_gi_gather_shader.h"

// GLSL twin of diffuse_gi_gather.cpp (FP32 path). Every function mirrors the
// C++ template instantiated with float, in the same operation order, so the
// CPU/GPU comparison isolates GPU arithmetic (transcendentals, contraction)
// from FP32 rounding. Own code; no third-party shader was consulted.

#include <algorithm>
#include <cmath>

namespace Fast::Renderer::DiffuseGi {
namespace {

// Constants, M1 sector weights (closed form) and word-safe sector masks.
constexpr const char* kMath = R"glsl(#version 450
const float PI = 3.14159265358979323846;
#define INF uintBitsToFloat(0x7F800000u)

float delta_minus_sin(float d) {
    if (d < 0.5) {
        float d2 = d * d;
        return d * d2 * (1.0 / 6.0 - d2 * (1.0 / 120.0 - d2 * (1.0 / 5040.0 - d2 * (1.0 / 362880.0 - d2 / 39916800.0))));
    }
    return d - sin(d);
}

float slice_integral(float a, float b, float p, float q) {
    float delta = q - p;
    float sum = q + p;
    float sin_delta = sin(delta);
    float half_sin = sin(sum * 0.5);
    return 0.5 * (b * (delta_minus_sin(delta) + 2.0 * sin_delta * half_sin * half_sin) + a * sin_delta * sin(sum));
}

// (1/pi) integral_lo^hi max(0, a cos t + b sin t) |sin t| dt on [-pi, pi].
float analytic_weight(float a, float b, float lo, float hi) {
    lo = max(lo, -PI);
    hi = min(hi, PI);
    if (!(hi > lo) || !(a * a + b * b > 0.0)) return 0.0;
    float gamma = atan(b, a);
    float half_pi = PI * 0.5;
    float l0 = gamma - half_pi;
    float l1 = gamma + half_pi;
    vec2 lobes[2];
    int lobe_count = 1;
    if (l0 < -PI) {
        lobes[0] = vec2(-PI, l1);
        lobes[1] = vec2(l0 + 2.0 * PI, PI);
        lobe_count = 2;
    } else if (l1 > PI) {
        lobes[0] = vec2(l0, PI);
        lobes[1] = vec2(-PI, l1 - 2.0 * PI);
        lobe_count = 2;
    } else {
        lobes[0] = vec2(l0, l1);
        lobes[1] = vec2(0.0);
    }
    float total = 0.0;
    for (int lobe = 0; lobe < lobe_count; ++lobe) {
        for (int side = 0; side < 2; ++side) {
            float side_lo = side == 0 ? -PI : 0.0;
            float side_hi = side == 0 ? 0.0 : PI;
            float p = max(max(lo, lobes[lobe].x), side_lo);
            float q = min(min(hi, lobes[lobe].y), side_hi);
            if (q > p) {
                float piece = slice_integral(a, b, p, q);
                total += side == 0 ? -piece : piece;
            }
        }
    }
    return total / PI;
}

// count in [1, 32], start + count <= 32: never a shift by the word width.
uint range_bits(uint start, uint count) {
    return (count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u)) << start;
}

uint valid_bits(uint word, uint sectors) {
    uint lo = word * 32u;
    uint hi = min(sectors, lo + 32u);
    return hi > lo ? range_bits(0u, hi - lo) : 0u;
}

uint clamp_index(float value, uint sectors) {
    if (!(value > 0.0)) return 0u;
    if (value >= float(sectors)) return sectors;
    return uint(value);
}

// Centre rule: sector j is covered by [lo, hi) iff lo <= centre_j < hi.
uvec2 centre_range(float lo, float hi, uint sectors) {
    if (!(hi > lo) || sectors == 0u) return uvec2(0u);
    float step_angle = 2.0 * PI / float(sectors);
    float x0 = (lo + PI) / step_angle - 0.5;
    float x1 = (hi + PI) / step_angle - 0.5;
    return uvec2(clamp_index(ceil(x0), sectors), clamp_index(ceil(x1), sectors));
}

void set_range(inout uint words[8], uint first, uint end) {
    for (uint w = first / 32u; w * 32u < end; ++w) {
        uint lo = max(first, w * 32u);
        uint hi = min(end, w * 32u + 32u);
        if (hi > lo) words[w] |= range_bits(lo - w * 32u, hi - lo);
    }
}

float boundary(uint j, uint sectors) {
    if (j >= sectors) return PI;
    return -PI + float(j) * (2.0 * PI / float(sectors));
}

float vlength(vec3 a) { return sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
vec3 vnormalize(vec3 a) {
    float l = vlength(a);
    return l > 0.0 ? a * (1.0 / l) : vec3(0.0);
}
vec3 vcross(vec3 a, vec3 b) {
    return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
)glsl";

// Parameters, buffers, depth decoding, effective class and rays.
constexpr const char* kGatherCommon = R"glsl(
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std140, set = 0, binding = 0) uniform GatherParams {
    uvec4 buffer_viewport;
    uvec4 extent_counts;
    uvec4 modes;
    vec4 view_x;
    vec4 view_y;
    vec4 depth;
    vec4 gather;
    vec4 radial;
    uvec4 sequence;
    uvec4 strata;
    vec4 strata_scale;
} p;
layout(std430, set = 0, binding = 1) readonly buffer DepthValues { float depth_values[]; };
layout(std430, set = 0, binding = 2) readonly buffer ClassValues { uint class_values[]; };
layout(std430, set = 0, binding = 3) readonly buffer RadianceValues { vec4 radiance_values[]; };
layout(std430, set = 0, binding = 4) buffer NormalValues { vec4 normal_values[]; };
#ifdef DGI_SHADING
// M8: shading normals (xyz, w = 1 when valid): renderer normals in, unit and
// on the geometric normal's side after the normal pass.
layout(std430, set = 0, binding = 13) buffer ShadingValues { vec4 shading_values[]; };
#endif

uint viewport_width() { return p.extent_counts.x; }
uint viewport_height() { return p.extent_counts.y; }

bool inside(int x, int y) {
    return x >= 0 && y >= 0 && x < int(viewport_width()) && y < int(viewport_height());
}

uint buffer_index(int x, int y) {
    return (p.buffer_viewport.w + uint(y)) * p.buffer_viewport.x + p.buffer_viewport.z + uint(x);
}

float decode_depth(float d) {
    float n = p.depth.x;
    float f = p.depth.y;
    uint encoding = p.modes.x;
    if (encoding == 1u) return n * f / (f - d * (f - n));
    if (encoding == 2u) return p.depth.z != 0.0 ? n / d : n * f / (n + d * (f - n));
    if (encoding == 3u) return n + d * (f - n);
    if (encoding == 4u) return f - d * (f - n);
    return uintBitsToFloat(0x7FC00000u);
}

// Effective class of a viewport texel: 0 unavailable, 1/2 geometry, 3 sky.
uint texel_class(int x, int y, out float z) {
    z = 0.0;
    uint index = buffer_index(x, y);
    uint c = class_values[index];
    if (c == 1u || c == 2u) {
        float zz = decode_depth(depth_values[index]);
        float low = p.depth.x * 0.99999;
        float high = p.depth.z != 0.0 ? INF : p.depth.y * 1.00001;
        if (!isnan(zz) && !isinf(zz) && zz >= low && zz <= high) {
            z = zz;
            return c;
        }
        return 0u;
    }
    return c == 3u ? 3u : 0u;
}

bool geometry_at(int x, int y, out float z) {
    z = 0.0;
    if (!inside(x, y)) return false;
    uint c = texel_class(x, y, z);
    return c == 1u || c == 2u;
}

vec3 ray(float px, float py) {
    float u = px / float(viewport_width());
    float v = py / float(viewport_height());
    return vec3(p.view_x.x * u + p.view_x.y * v + p.view_x.z, p.view_y.x * u + p.view_y.y * v + p.view_y.z, -1.0);
}

// M3 rotation sequence (twin of StratumIndex / PixelRotationT).
uint tile_order(uint x, uint y, uint log2) {
    uint index = 0u;
    for (uint k = 0u; k < log2; ++k) {
        uint bx = (x >> k) & 1u;
        uint by = (y >> k) & 1u;
        uint code = bx == 0u ? (by == 0u ? 0u : 3u) : (by == 0u ? 2u : 1u);
        index |= code << (2u * (log2 - 1u - k));
    }
    return index;
}

uint stratum_index(uint x, uint y) {
    uint flip = (p.sequence.x >> 2u) & 1u;
    uint coarse = ((tile_order(x, y, p.sequence.y) ^ flip) + p.strata.w) % p.strata.z;
    return p.sequence.z * coarse + p.sequence.w;
}

float pixel_rotation(uint x, uint y) {
    if ((p.sequence.x & 1u) == 0u) return p.gather.z;
    return (float(stratum_index(x, y)) + 0.5) * p.strata_scale.x;
}

float pixel_radial_jitter(uint x, uint y) {
    if ((p.sequence.x & 3u) != 3u) return p.radial.y;
    uint j = (stratum_index(x, y) * p.strata.y) % p.strata.x;
    return (float(j) + 0.5) * p.strata_scale.x;
}
)glsl";

constexpr const char* kNormalMain = R"glsl(
// Geometric normal: the quadrant with the smallest 1/z planarity errors (2x2
// term plus second differences along its two sides).
vec4 reconstruct_normal(int x, int y, float z0) {
    float w0 = 1.0 / z0;
    float tie = 4.0e-6 * w0;
    ivec2 order[4] = ivec2[4](ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1));
    int best = -1;
    int best_rank = 2;
    int best_verified = 0;
    float best_error = 0.0;
    for (int k = 0; k < 4; ++k) {
        int sx = order[k].x;
        int sy = order[k].y;
        float zx, zy, zd, z2;
        if (!geometry_at(x + sx, y, zx) || !geometry_at(x, y + sy, zy)) continue;
        float wx = 1.0 / zx;
        float wy = 1.0 / zy;
        int rank = 1;
        int verified = 0;
        float error = abs(wx - w0) + abs(wy - w0);
        if (geometry_at(x + sx, y + sy, zd)) {
            rank = 0;
            error = abs(1.0 / zd - wx - wy + w0);
        }
        if (geometry_at(x + 2 * sx, y, z2)) {
            error += abs(w0 - 2.0 * wx + 1.0 / z2);
            verified++;
        }
        if (geometry_at(x, y + 2 * sy, z2)) {
            error += abs(w0 - 2.0 * wy + 1.0 / z2);
            verified++;
        }
        bool better = best < 0 || rank < best_rank ||
                      (rank == best_rank && (error < best_error - tie ||
                                             (!(error > best_error + tie) && verified > best_verified)));
        if (better) {
            best = k;
            best_rank = rank;
            best_error = error;
            best_verified = verified;
        }
    }
    if (best < 0) return vec4(0.0);
    int sx = order[best].x;
    int sy = order[best].y;
    float zx, zy;
    geometry_at(x + sx, y, zx);
    geometry_at(x, y + sy, zy);
    vec3 p0 = ray(float(x) + 0.5, float(y) + 0.5) * z0;
    vec3 px = ray(float(x + sx) + 0.5, float(y) + 0.5) * zx;
    vec3 py = ray(float(x) + 0.5, float(y + sy) + 0.5) * zy;
    vec3 tx = (px - p0) * float(sx);
    vec3 ty = (py - p0) * float(sy);
    vec3 n = vnormalize(vcross(tx, ty));
    if (vdot(n, p0) > 0.0) n = n * -1.0;
    return vec4(n, vdot(n, n) > 0.0 ? 1.0 : 0.0);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(id.x, id.y)) return; // no barriers in this kernel
    uint index = buffer_index(id.x, id.y);
    float z0;
    uint c = texel_class(id.x, id.y, z0);
    vec4 result = vec4(0.0);
    if (c == 1u || c == 2u) {
        if ((p.modes.w & 4u) != 0u) {
            vec3 face = vnormalize(normal_values[index].xyz);
            vec3 position = ray(float(id.x) + 0.5, float(id.y) + 0.5) * z0;
            if (vdot(face, position) > 0.0) face = face * -1.0;
            result = vec4(face, vdot(face, face) > 0.0 ? 1.0 : 0.0);
        } else {
            result = reconstruct_normal(id.x, id.y, z0);
        }
    }
#ifdef DGI_SHADING
    // M8: the shading normal, unit and on the geometric normal's side (none
    // without a geometric normal or for a non-finite vector).
    if ((p.modes.w & 16u) != 0u) {
        vec4 shading = vec4(0.0);
        if (result.w != 0.0) {
            vec3 sn = vnormalize(shading_values[index].xyz);
            if (isnan(sn.x) || isinf(sn.x) || isnan(sn.y) || isinf(sn.y) || isnan(sn.z) || isinf(sn.z)) sn = vec3(0.0);
            if (vdot(sn, result.xyz) < 0.0) sn = sn * -1.0;
            shading = vec4(sn, vdot(sn, sn) > 0.0 ? 1.0 : 0.0);
        }
        shading_values[index] = shading;
    }
#endif
    normal_values[index] = result;
}
)glsl";

constexpr const char* kGatherMain = R"glsl(
layout(std430, set = 0, binding = 5) writeonly buffer Results { vec4 results[]; };
layout(std430, set = 0, binding = 6) writeonly buffer Aux { vec4 aux[]; };

// Section state (per invocation).
vec3 g_p, g_v, g_t, g_n;
#ifdef DGI_SHADING
vec3 g_ns; // M8: the lobe (shading) normal
#endif
float g_a, g_b, g_zp, g_dir_x, g_dir_y;
float g_radial_jitter;
int g_x, g_y;
uint g_mask[8];
vec3 g_sum;
uint g_scheduled, g_valid;

struct Side {
    float dx, dy, inv_x, inv_y, extent, enter, lambda0, lambda;
    int sx, sy, cx, cy, previous_x, previous_y, out_x, out_y;
    uint kx, ky, iteration, k;
    bool done, has;
};

Side make_side(int side, float extent) {
    Side s;
    s.dx = g_dir_x * float(side);
    s.dy = g_dir_y * float(side);
    float adx = abs(s.dx);
    float ady = abs(s.dy);
    s.inv_x = adx > 0.0 ? 1.0 / adx : INF;
    s.inv_y = ady > 0.0 ? 1.0 / ady : INF;
    s.sx = s.dx > 0.0 ? 1 : (s.dx < 0.0 ? -1 : 0);
    s.sy = s.dy > 0.0 ? 1 : (s.dy < 0.0 ? -1 : 0);
    s.extent = extent;
    s.cx = 0; s.cy = 0; s.kx = 0u; s.ky = 0u; s.enter = 0.0; s.iteration = 0u;
    s.k = 0u; s.previous_x = 0; s.previous_y = 0;
    s.lambda0 = min(0.5 * s.inv_x, 0.5 * s.inv_y);
    s.done = false;
    s.has = false;
    s.lambda = 0.0; s.out_x = 0; s.out_y = 0;
    if (p.modes.y == 1u && !(s.extent > s.lambda0)) s.done = true;
    return s;
}

// Next position of one side, in increasing screen distance.
void advance(inout Side s) {
    s.has = false;
    if (p.modes.y == 0u) {
        uint limit = 2u * (viewport_width() + viewport_height()) + 16u;
        float epsilon = p.gather.w;
        while (!s.done) {
            if (s.iteration >= limit) { s.done = true; break; }
            s.iteration++;
            float tx = s.inv_x < INF ? (float(s.kx) + 0.5) * s.inv_x : INF;
            float ty = s.inv_y < INF ? (float(s.ky) + 0.5) * s.inv_y : INF;
            float exit_t = min(tx, ty);
            bool emit = false;
            float foot = 0.0;
            int cell_x = s.cx;
            int cell_y = s.cy;
            if (s.cx != 0 || s.cy != 0) {
                if (!(s.enter < s.extent)) { s.done = true; break; }
                if (exit_t - s.enter > epsilon) {
                    emit = true;
                    foot = clamp(float(s.cx) * s.dx + float(s.cy) * s.dy, s.enter, exit_t);
                }
            }
            if (tx < ty) {
                s.cx += s.sx; s.enter = tx; s.kx++;
            } else if (ty < tx) {
                s.cy += s.sy; s.enter = ty; s.ky++;
            } else {
                s.cx += s.sx; s.cy += s.sy; s.enter = tx; s.kx++; s.ky++;
            }
            if (emit) {
                s.has = true; s.lambda = foot; s.out_x = cell_x; s.out_y = cell_y;
                return;
            }
        }
        return;
    }
    uint count = max(p.modes.z, 1u);
    while (!s.done && s.k < count) {
        uint k = s.k++;
        float u = (float(k) + g_radial_jitter) / float(count);
        float shaped = u > 0.0 ? pow(u, p.radial.x) : 0.0;
        float lambda = s.lambda0 + (s.extent - s.lambda0) * shaped;
        int cx = int(floor(lambda * s.dx + 0.5));
        int cy = int(floor(lambda * s.dy + 0.5));
        if ((cx == 0 && cy == 0) || (cx == s.previous_x && cy == s.previous_y)) continue;
        s.previous_x = cx; s.previous_y = cy;
        s.has = true; s.lambda = lambda; s.out_x = cx; s.out_y = cy;
        return;
    }
    s.done = true;
}

void accumulate(vec2 pieces[2], uint piece_count, vec3 radiance) {
    uint sectors = p.extent_counts.w;
    uint cover[8] = uint[8](0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    for (uint piece = 0u; piece < piece_count; ++piece) {
        uvec2 range = centre_range(pieces[piece].x, pieces[piece].y, sectors);
        set_range(cover, range.x, range.y);
    }
    uint fresh[8];
    for (uint w = 0u; w < 8u; ++w) {
        fresh[w] = cover[w] & ~g_mask[w];
        g_mask[w] |= cover[w];
    }
    for (uint w = 0u; w * 32u < sectors; ++w) {
        uint bits = fresh[w];
        while (bits != 0u) {
            uint start = uint(findLSB(bits));
            uint shifted = bits >> start;
            uint run = shifted == 0xFFFFFFFFu ? 32u - start : uint(findLSB(~shifted));
            uint j0 = w * 32u + start;
            float weight = analytic_weight(g_a, g_b, boundary(j0, sectors), boundary(j0 + run, sectors));
            g_sum.x += weight * radiance.x;
            g_sum.y += weight * radiance.y;
            g_sum.z += weight * radiance.z;
            bits &= ~range_bits(start, run);
        }
    }
}

#ifdef DGI_SHADING
// M8 (GatherSettings::ShadingNormals): a candidate on the receiver's own
// smooth surface, and the visibility of a candidate point from the receiver
// (twins: OwnSmoothSurfaceT, HiddenM8T).
bool own_smooth_m8(vec3 d, float len, vec3 nq, uint qi) {
    vec3 sq = shading_values[qi].xyz; // read only for a candidate above both planes
    if (!(vdot(sq, sq) > 0.0) || !(vdot(nq, nq) > 0.0)) return false;
    float cf = vdot(g_n, nq);
    float cs = vdot(g_ns, sq);
    if (!(cs >= p.strata_scale.y)) return false;
    if (!(1.0 - cs < p.strata_scale.z * (1.0 - cf))) return false;
    float sf = sqrt(max(1.0 - cf * cf, 0.0));
    float tolerance = (p.radial.z + sf) * len + p.radial.w * g_zp;
    return abs(vdot(g_n, d)) <= tolerance && abs(vdot(nq, d)) <= tolerance;
}
// `qi`: the buffer index of the candidate's texel (its shading normal).
bool hidden_m8(vec3 e, vec3 nq, uint qi) {
    vec3 d = e - g_p;
    float len = vlength(d);
    float tolerance = p.radial.z * len + p.radial.w * g_zp;
    if (abs(vdot(d, g_n)) <= tolerance) return true;
    if (vdot(d, g_ns) <= tolerance) return true;
    return own_smooth_m8(d, len, nq, qi);
}
#endif

void process(float lambda, int side, int cell_x, int cell_y) {
    g_scheduled++;
    int xx = g_x + cell_x;
    int yy = g_y + cell_y;
    if (!inside(xx, yy)) return;
    float zq;
    uint cls = texel_class(xx, yy, zq);
    if (cls == 0u) return;
    g_valid++;
    if (cls == 3u) return;
    float radius = p.gather.x;
    if ((p.modes.w & 2u) != 0u) {
        vec3 centre = ray(float(xx) + 0.5, float(yy) + 0.5) * zq;
#ifdef DGI_SHADING
        if ((p.modes.w & 8u) != 0u) {
            uint qi = buffer_index(xx, yy);
            if (hidden_m8(centre, normal_values[qi].xyz, qi)) return;
        } else
#endif
        {
            vec3 d = centre - g_p;
            float elevation = vdot(d, g_n);
            if (elevation <= p.radial.z * vlength(d) + p.radial.w * g_zp) return;
        }
    }
    float sign_side = float(side);
    float px = float(g_x) + 0.5 + lambda * g_dir_x * sign_side;
    float py = float(g_y) + 0.5 + lambda * g_dir_y * sign_side;
    vec3 q = ray(px, py) * zq;
    vec3 rel = q - g_p;
    float dist = vlength(rel);
    if (dist > radius) return;
    vec3 dir = vnormalize(q);
    float along = vdot(rel, dir);
    float clip_length = -along + sqrt(max(along * along - (dist * dist - radius * radius), 0.0));
    float seg = max(min(p.gather.y, clip_length), 0.0);
    vec3 back = q + dir * seg;
    float closest_at = clamp(-along, 0.0, seg);
    if (vlength(rel + dir * closest_at) <= 1.0e-6 * g_zp) return;
    vec3 rel_back = back - g_p;
    float theta_f = atan(vdot(rel, g_t), vdot(rel, g_v));
    float theta_b = atan(vdot(rel_back, g_t), vdot(rel_back, g_v));
    float two_pi = 2.0 * PI;
    float delta = theta_b - theta_f;
    if (delta > PI) delta -= two_pi;
    else if (delta <= -PI) delta += two_pi;
    float lo = delta >= 0.0 ? theta_f : theta_f + delta;
    float hi = delta >= 0.0 ? theta_f + delta : theta_f;
    vec2 pieces[2];
    pieces[0] = vec2(0.0);
    pieces[1] = vec2(0.0);
    uint piece_count = 0u;
    if (hi > lo) {
        if (lo < -PI) {
            pieces[0] = vec2(lo + two_pi, PI); pieces[1] = vec2(-PI, hi); piece_count = 2u;
        } else if (hi > PI) {
            pieces[0] = vec2(lo, PI); pieces[1] = vec2(-PI, hi - two_pi); piece_count = 2u;
        } else {
            pieces[0] = vec2(lo, hi); piece_count = 1u;
        }
    }
    uint index = buffer_index(xx, yy);
    vec3 radiance = radiance_values[index].xyz;
    if (cls == 1u) {
        vec3 nq = normal_values[index].xyz;
        if (vdot(nq, nq) > 0.0) {
            if (!(vdot(nq, g_p - q) > 0.0)) radiance = vec3(0.0);
        }
    }
    accumulate(pieces, piece_count, radiance);
}

)glsl";

// Entry point of the gather; kGatherMain + kGatherEntry is the M2/M3 kernel
// text unchanged (M4 reuses kGatherMain's functions with its own entry).
constexpr const char* kGatherEntry = R"glsl(void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(id.x, id.y)) return; // no barriers in this kernel
    uint out_index = uint(id.y) * viewport_width() + uint(id.x);
    results[out_index] = vec4(0.0, 0.0, 0.0, 1.0);
    aux[out_index] = vec4(0.0);
    g_x = id.x;
    g_y = id.y;
    float z0;
    uint c = texel_class(id.x, id.y, z0);
    uint receiver = buffer_index(id.x, id.y);
    vec3 n = normal_values[receiver].xyz;
    if (!(c == 1u || c == 2u) || !(vdot(n, n) > 0.0)) return;
    uint sections = p.extent_counts.z;
    uint sectors = p.extent_counts.w;
    float ax = p.view_x.x, bx = p.view_x.y, ay = p.view_y.x, by = p.view_y.y;
    vec3 total = vec3(0.0);
    float total_a = 0.0;
    float occupied = 0.0;
    g_scheduled = 0u;
    g_valid = 0u;
    float rotation = pixel_rotation(uint(id.x), uint(id.y));
    g_radial_jitter = pixel_radial_jitter(uint(id.x), uint(id.y));
    for (uint section = 0u; section < sections; ++section) {
        float phi = PI * (float(section) + rotation) / float(sections);
        g_zp = z0;
        g_p = ray(float(id.x) + 0.5, float(id.y) + 0.5) * z0;
        g_v = vnormalize(g_p * -1.0);
        vec3 helper = abs(g_v.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
        vec3 e1 = vnormalize(vcross(helper, g_v));
        vec3 e2 = vcross(g_v, e1);
        g_t = e1 * cos(phi) + e2 * sin(phi);
        g_n = n;
        g_a = vdot(n, g_v);
        g_b = vdot(n, g_t);
        float zp = -g_p.z;
        float gx = g_t.x * zp + g_p.x * g_t.z;
        float gy = g_t.y * zp + g_p.y * g_t.z;
        float det = ax * by - bx * ay;
        float du = (by * gx - bx * gy) / det;
        float dv = (-ay * gx + ax * gy) / det;
        float pxd = du * float(viewport_width());
        float pyd = dv * float(viewport_height());
        float len = sqrt(pxd * pxd + pyd * pyd);
        if (!(len > 0.0) || isinf(len) || isnan(len)) {
            results[out_index] = vec4(0.0, 0.0, 0.0, 1.0);
            aux[out_index] = vec4(0.0);
            return;
        }
        g_dir_x = pxd / len;
        g_dir_y = pyd / len;
        float cap = sqrt(float(viewport_width()) * float(viewport_width()) +
                         float(viewport_height()) * float(viewport_height()));
        float distance_p = vlength(g_p);
        float radius = p.gather.x;
        float extents[2];
        for (int side = 0; side < 2; ++side) {
            float extent = cap;
            if (radius < distance_p) {
                float sa = radius / distance_p;
                float ca = sqrt(max(1.0 - sa * sa, 0.0));
                float sgn = side == 0 ? 1.0 : -1.0;
                vec3 r = g_v * (-ca) + g_t * (sgn * sa);
                if (r.z < 0.0) {
                    float tx = r.x / -r.z - g_p.x / zp;
                    float ty = r.y / -r.z - g_p.y / zp;
                    float eu = (by * tx - bx * ty) / det * float(viewport_width());
                    float ev = (-ay * tx + ax * ty) / det * float(viewport_height());
                    extent = min(sqrt(eu * eu + ev * ev), cap);
                }
            }
            extents[side] = extent;
        }
        for (uint w = 0u; w < 8u; ++w) g_mask[w] = 0u;
        g_sum = vec3(0.0);
        Side plus = make_side(1, extents[0]);
        Side minus = make_side(-1, extents[1]);
        advance(plus);
        advance(minus);
        bool interleave = (p.modes.w & 1u) != 0u;
        while (plus.has || minus.has) {
            bool take_plus = !minus.has || (plus.has && (!interleave || plus.lambda <= minus.lambda));
            if (take_plus) {
                process(plus.lambda, 1, plus.out_x, plus.out_y);
                advance(plus);
            } else {
                process(minus.lambda, -1, minus.out_x, minus.out_y);
                advance(minus);
            }
        }
        float open_weight = 0.0;
        for (uint w = 0u; w * 32u < sectors; ++w) {
            uint bits = ~g_mask[w] & valid_bits(w, sectors);
            while (bits != 0u) {
                uint start = uint(findLSB(bits));
                uint shifted = bits >> start;
                uint run = shifted == 0xFFFFFFFFu ? 32u - start : uint(findLSB(~shifted));
                uint j0 = w * 32u + start;
                open_weight += analytic_weight(g_a, g_b, boundary(j0, sectors), boundary(j0 + run, sectors));
                bits &= ~range_bits(start, run);
            }
        }
        for (uint w = 0u; w < 8u; ++w) occupied += float(bitCount(g_mask[w]));
        total.x += g_sum.x * PI;
        total.y += g_sum.y * PI;
        total.z += g_sum.z * PI;
        total_a += open_weight * PI;
    }
    float inverse = 1.0 / float(sections);
    results[out_index] = vec4(total.x * inverse, total.y * inverse, total.z * inverse, total_a * inverse);
    float confidence = g_scheduled == 0u ? 0.0 : float(g_valid) / float(g_scheduled);
    aux[out_index] = vec4(confidence, 1.0, float(g_scheduled), occupied);
}
)glsl";

// M3: geometry of the receivers for the temporal kernels (viewport sized).
constexpr const char* kPackMain = R"glsl(
layout(std430, set = 0, binding = 6) readonly buffer Aux { vec4 aux[]; };
layout(std430, set = 0, binding = 7) writeonly buffer Geometry { vec4 geometry[]; };
#ifdef DGI_SHADING
// M8: the receivers' shading normals per viewport pixel (temporal, filter, upsample).
layout(std430, set = 0, binding = 14) writeonly buffer ShadingGeometry { vec4 shading_geometry[]; };
#endif

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(id.x, id.y)) return; // no barriers in this kernel
    uint out_index = uint(id.y) * viewport_width() + uint(id.x);
    float z;
    uint c = texel_class(id.x, id.y, z);
    vec4 n = normal_values[buffer_index(id.x, id.y)];
    bool receiver = (c == 1u || c == 2u) && n.w != 0.0 && aux[out_index].y != 0.0;
    geometry[out_index] = receiver ? vec4(n.xyz, z) : vec4(0.0);
#ifdef DGI_SHADING
    if ((p.modes.w & 16u) != 0u)
        shading_geometry[out_index] = receiver ? shading_values[buffer_index(id.x, id.y)] : vec4(0.0);
#endif
}
)glsl";

constexpr const char* kUnitMain = R"glsl(
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(std140, set = 0, binding = 0) uniform UnitParams { uvec4 info; } u;
layout(std430, set = 0, binding = 1) readonly buffer UnitInputs { vec4 inputs[]; };
layout(std430, set = 0, binding = 2) writeonly buffer UnitOutputs { uvec4 outputs[]; };

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= u.info.x) return; // no barriers in this kernel
    vec4 value = inputs[i];
    if (u.info.z == 0u) {
        uint words[8] = uint[8](0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
        uvec2 range = centre_range(value.x, value.y, u.info.y);
        set_range(words, range.x, range.y);
        outputs[2u * i] = uvec4(words[0], words[1], words[2], words[3]);
        outputs[2u * i + 1u] = uvec4(words[4], words[5], words[6], words[7]);
    } else {
        float weight = analytic_weight(value.x, value.y, value.z, value.w);
        outputs[2u * i] = uvec4(floatBitsToUint(weight), 0u, 0u, 0u);
        outputs[2u * i + 1u] = uvec4(0u);
    }
}
)glsl";

} // namespace

GatherShaderParams MakeGatherShaderParams(const GatherInput& input, const GatherSettings& settings) {
    GatherShaderParams p;
    p.BufferViewport = {input.BufferWidth, input.BufferHeight, input.Viewport.X, input.Viewport.Y};
    p.ExtentCounts = {input.Viewport.Width, input.Viewport.Height, settings.Sections, settings.Sectors};
    uint32_t flags = 0U;
    if (settings.InterleaveSides) flags |= kShaderFlagInterleave;
    if (settings.RejectSelfIntersection) flags |= kShaderFlagRejectSelf;
    if (input.NormalSource == GeometricNormalSource::RendererFaceNormal) flags |= kShaderFlagFaceNormals;
    // M8: the smooth-shaded receiver needs the shading normals (the pass
    // allocates and orients them; the CPU twin reads GatherInput::ShadingNormals).
    if (settings.ShadingNormals) flags |= kShaderFlagShading | kShaderFlagShadingInputs;
    // Schedule: 0 EveryTexel, 1 Radial, 2 MetricRadial (M4 kernel only).
    const uint32_t schedule = settings.Schedule == CandidateSchedule::Radial
                                  ? 1U
                                  : (settings.Schedule == CandidateSchedule::MetricRadial ? 2U : 0U);
    p.Modes = {static_cast<uint32_t>(input.Camera.Encoding), schedule, settings.CandidatesPerSide, flags};
    const auto& c = input.Camera;
    p.ViewX = {static_cast<float>(c.ViewX[0]), static_cast<float>(c.ViewX[1]), static_cast<float>(c.ViewX[2]), 0.0F};
    p.ViewY = {static_cast<float>(c.ViewY[0]), static_cast<float>(c.ViewY[1]), static_cast<float>(c.ViewY[2]), 0.0F};
    const bool infiniteFar = std::isinf(c.Far);
    p.Depth = {static_cast<float>(c.Near), infiniteFar ? 0.0F : static_cast<float>(c.Far), infiniteFar ? 1.0F : 0.0F,
               0.0F};
    p.Gather = {static_cast<float>(settings.Radius), static_cast<float>(settings.Thickness),
                static_cast<float>(settings.Rotation), static_cast<float>(settings.ChordEpsilon)};
    p.Radial = {static_cast<float>(settings.RadialExponent), static_cast<float>(settings.RadialJitter),
                static_cast<float>(settings.SelfIntersectionSine), static_cast<float>(settings.SelfIntersectionDepth)};
    const RotationSequence& sequence = settings.Rotations;
    if (sequence.Mode == RotationMode::Stratified) {
        uint32_t sequenceFlags = kShaderSequenceStratified;
        if (settings.SequenceRadialJitter) sequenceFlags |= kShaderSequenceRadialJitter;
        if (FrameFlip(sequence) != 0U) sequenceFlags |= kShaderSequenceFlip;
        const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
        const uint32_t cycle = std::clamp(sequence.Cycle, 1U, kMaxRotationCycle);
        const uint32_t strata = StratumCount(sequence);
        p.Sequence = {sequenceFlags, log2, cycle, static_cast<uint32_t>(sequence.Frame % cycle)};
        p.Strata = {strata, LatticeMultiplier(strata), 1U << (2U * log2), FrameShift(sequence)};
        p.StrataScale = {StratumScaleF32(sequence), 0.0F, 0.0F, 0.0F};
    } else {
        p.Strata = {1U, 1U, 1U, 0U}; // never used: flags are zero
    }
    if (settings.ShadingNormals) {
        p.StrataScale[1] = static_cast<float>(settings.OwnSurfaceCos);
        p.StrataScale[2] = static_cast<float>(settings.HardEdgeRatio);
    }
    return p;
}

namespace {
// The M8 variant of a kernel: the shading-normal paths compiled in, each
// behind its flag (GatherShaderParams::Modes[3] & 8 and & 16).
std::string WithShadingDefine(const std::string& text) {
    std::string out = text;
    out.insert(out.find('\n') + 1, "#define DGI_SHADING 1\n");
    return out;
}
} // namespace

std::string BuildDiffuseGiNormalShader(bool shading) {
    const std::string text = std::string(kMath) + kGatherCommon + kNormalMain;
    return shading ? WithShadingDefine(text) : text;
}

std::string BuildDiffuseGiGatherShader() {
    return std::string(kMath) + kGatherCommon + kGatherMain + kGatherEntry;
}

std::string DiffuseGiGatherFunctionsGlsl(bool shading) {
    const std::string text = std::string(kMath) + kGatherCommon + kGatherMain;
    return shading ? WithShadingDefine(text) : text;
}

std::string BuildDiffuseGiPackShader(bool shading) {
    const std::string text = std::string(kMath) + kGatherCommon + kPackMain;
    return shading ? WithShadingDefine(text) : text;
}

std::string BuildDiffuseGiUnitShader() {
    return std::string(kMath) + kUnitMain;
}

} // namespace Fast::Renderer::DiffuseGi
