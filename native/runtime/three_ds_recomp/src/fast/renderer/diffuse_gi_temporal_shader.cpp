#include "fast/renderer/diffuse_gi_temporal_shader.h"

#include "fast/renderer/diffuse_gi_normal_code.h"

// GLSL twin of diffuse_gi_temporal.cpp (Precision::Float32). Every function
// mirrors the C++ template instantiated with float, in the same operation
// order. Own code; no third-party shader was consulted.

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr const char* kCommon = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#define INF uintBitsToFloat(0x7F800000u)

bool finite1(float v) { return !isnan(v) && !isinf(v); }
bool finite4(vec4 v) { return finite1(v.x) && finite1(v.y) && finite1(v.z) && finite1(v.w); }
float luma(vec3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
float vdot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float vlength(vec3 a) { return sqrt(vdot(a, a)); }

bool is_receiver(vec4 g) {
    return g.w > 0.0 && finite1(g.w) && finite1(g.x) && finite1(g.y) && finite1(g.z) &&
           g.x * g.x + g.y * g.y + g.z * g.z > 0.0;
}

vec3 map_ray(vec4 rx, vec4 ry, float px, float py, uint w, uint h) {
    float u = px / float(w);
    float v = py / float(h);
    return vec3(rx.x * u + rx.y * v + rx.z, ry.x * u + ry.y * v + ry.z, -1.0);
}

bool compatible(vec3 pa, vec3 na, float zp, vec3 qb, vec3 nb, float sine, float depth, float cosn) {
    vec3 d = qb - pa;
    float plane = abs(vdot(na, d));
    return plane <= sine * vlength(d) + depth * zp && vdot(na, nb) >= cosn;
}
)glsl";

// M8 (smooth-shaded receivers): the continuity test of two receiver samples
// whose face normals differ (diffuse_gi_temporal.h, SmoothCreaseT).
constexpr const char* kSmoothSurface = R"glsl(
bool smooth_normals(vec3 na, vec3 sa, vec3 nb, vec3 sb, float cosn, float ratio) {
    if (!(vdot(sa, sa) > 0.0) || !(vdot(sb, sb) > 0.0)) return false;
    float cs = vdot(sa, sb);
    float cf = vdot(na, nb);
    return cs >= cosn && 1.0 - cs < ratio * (1.0 - cf);
}
bool smooth_crease(vec3 pa, vec3 na, vec3 sa, float zp, vec3 qb, vec3 nb, vec3 sb, float sine, float depth,
                   float cosn, float ratio) {
    if (!smooth_normals(na, sa, nb, sb, cosn, ratio)) return false;
    float cf = vdot(na, nb);
    vec3 d = qb - pa;
    float sf = sqrt(max(1.0 - cf * cf, 0.0));
    float tolerance = (sine + sf) * vlength(d) + depth * zp;
    return abs(vdot(na, d)) <= tolerance && abs(vdot(nb, d)) <= tolerance;
}
)glsl";

constexpr const char* kTemporalMain = R"glsl(
layout(std140, set = 0, binding = 0) uniform TemporalParams {
    uvec4 extent;   // width, height, flags (1 reset, 2 shading normals), window radius
    uvec4 modes;    // tile, reconstruct, flow and depth, surface ids
    vec4 view_x;
    vec4 view_y;
    vec4 prev_x;
    vec4 prev_y;
    vec4 rot0;
    vec4 rot1;
    vec4 rot2;
    vec4 motion;     // to pixels x, y; jitter compensation x, y
    vec4 accumulate; // alpha min, variance frames, exposure ratio, 1 / alpha min
    vec4 change;     // z low, z high, floor J, floor A
    vec4 geometry;   // plane sine, plane depth, normal cos, hard-edge ratio (M8)
    vec4 moving;     // flow, depth, confidence
} p;
struct History { vec4 a; vec4 b; vec4 c; vec4 d; };
layout(std430, set = 0, binding = 1) readonly buffer Raw { vec4 raw[]; };
layout(std430, set = 0, binding = 2) readonly buffer Geometry { vec4 geo[]; };
layout(std430, set = 0, binding = 3) readonly buffer Motion { vec2 motion_values[]; };
layout(std430, set = 0, binding = 4) readonly buffer PreviousPosition { vec4 previous_position[]; };
layout(std430, set = 0, binding = 5) readonly buffer Ids { uint ids[]; };
layout(std430, set = 0, binding = 6) readonly buffer HistoryIn { History history_in[]; };
layout(std430, set = 0, binding = 7) writeonly buffer HistoryOut { History history_out[]; };
layout(std430, set = 0, binding = 8) writeonly buffer ColourOut { vec4 colour_out[]; };
layout(std430, set = 0, binding = 9) writeonly buffer VarianceOut { vec2 variance_out[]; };
layout(std430, set = 0, binding = 10) writeonly buffer Diag { vec4 diag[]; };
layout(std430, set = 0, binding = 11) writeonly buffer Diag2 { vec4 diag2[]; };
#ifdef DGI_SHADING
// M8: oriented shading normals per pixel (xyz, w = 1 when valid).
layout(std430, set = 0, binding = 12) readonly buffer ShadingGeometry { vec4 shading_geo[]; };
#endif

float window_weight(int d, uint tile) {
    int a = d < 0 ? -d : d;
    if (tile >= 2u) {
        int half_tile = int(tile / 2u);
        return a < half_tile ? 1.0 : (a == half_tile ? 0.5 : 0.0);
    }
    return a <= 1 ? 1.0 : 0.0;
}

void write_neutral(uint i) {
    History h;
    h.a = vec4(0.0);
    h.b = vec4(1.0, 1.0, 0.0, 0.0);
    h.c = vec4(1.0, 1.0, 0.0, 0.0);
    h.d = vec4(0.0);
    history_out[i] = h;
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    uint W = p.extent.x;
    uint H = p.extent.y;
    if (id.x >= W || id.y >= H) return; // no barriers in this kernel
    uint i = id.y * W + id.x;
    colour_out[i] = vec4(0.0, 0.0, 0.0, 1.0);
    variance_out[i] = vec2(0.0);
    diag[i] = vec4(1.0, 1.0, 0.0, 0.0);
    diag2[i] = vec4(0.0);
    vec4 g = geo[i];
    float z = g.w;
    vec3 n = g.xyz;
    if (!is_receiver(g)) {
        write_neutral(i);
        return;
    }
    uint flags = 1u;
    float sine = p.geometry.x;
    float depth = p.geometry.y;
    float cosn = p.geometry.z;
    vec3 pos = map_ray(p.view_x, p.view_y, float(id.x) + 0.5, float(id.y) + 0.5, W, H) * z;
#ifdef DGI_SHADING
    bool m8 = (p.extent.z & 2u) != 0u;
    vec3 sn = m8 ? shading_geo[i].xyz : vec3(0.0);
    float ratio = p.geometry.w;
#endif

    // Current sample: stratum reconstruction and spatial variance.
    float sw = 0.0, sw2 = 0.0, sr = 0.0, sg = 0.0, sb = 0.0, sa = 0.0, sl = 0.0, sl2 = 0.0, sa2 = 0.0;
    int radius = int(p.extent.w);
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            float w = window_weight(dx, p.modes.x) * window_weight(dy, p.modes.x);
            if (!(w > 0.0)) continue;
            int qx = int(id.x) + dx;
            int qy = int(id.y) + dy;
            if (qx < 0 || qy < 0 || qx >= int(W) || qy >= int(H)) continue;
            uint q = uint(qy) * W + uint(qx);
            vec4 gq = geo[q];
            if (!is_receiver(gq)) continue;
            if (dx != 0 || dy != 0) {
                vec3 pq = map_ray(p.view_x, p.view_y, float(qx) + 0.5, float(qy) + 0.5, W, H) * gq.w;
                if (!compatible(pos, n, z, pq, gq.xyz, sine, depth, cosn)
#ifdef DGI_SHADING
                    && !(m8 && smooth_crease(pos, n, sn, z, pq, gq.xyz, shading_geo[q].xyz, sine, depth, cosn, ratio))
#endif
                )
                    continue;
            }
            vec4 rq = raw[q];
            if (!finite4(rq)) continue;
            float l = luma(rq.xyz);
            sw += w;
            sw2 += w * w;
            sr += w * rq.x;
            sg += w * rq.y;
            sb += w * rq.z;
            sa += w * rq.w;
            sl += w * l;
            sl2 += w * l * l;
            sa2 += w * rq.w * rq.w;
        }
    }
    vec4 rc = raw[i];
    bool current_valid = finite4(rc) && sw > 0.0;
    vec3 cur = vec3(0.0);
    float cur_a = 1.0, var_l = 0.0, var_a = 0.0;
    if (current_valid) {
        float inv = 1.0 / sw;
        float mean_l = sl * inv;
        float mean_a = sa * inv;
        float vl = max(sl2 * inv - mean_l * mean_l, 0.0);
        float va = max(sa2 * inv - mean_a * mean_a, 0.0);
        if (p.modes.y != 0u) {
            cur = vec3(sr * inv, sg * inv, sb * inv);
            cur_a = mean_a;
            float shrink = sw2 * inv * inv;
            var_l = vl * shrink;
            var_a = va * shrink;
        } else {
            cur = rc.xyz;
            cur_a = rc.w;
            var_l = vl;
            var_a = va;
        }
    }

    // History: reprojection, surface compatibility, bilinear taps.
    bool accepted = false;
    bool moving = false;
    float confidence = 1.0;
    vec3 hj = vec3(0.0);
    float hm2 = 0.0, ha = 0.0, ha2 = 0.0, hn = 0.0, hna = 0.0, hv = 0.0, hva = 0.0;
    bool flow = p.modes.z != 0u;
    if ((p.extent.z & 1u) == 0u) {
        vec2 m = motion_values[i];
        vec3 pp;
        bool expected = finite1(m.x) && finite1(m.y);
        if (!flow) {
            pp = previous_position[i].xyz;
            expected = expected && finite1(pp.x) && finite1(pp.y) && finite1(pp.z);
        } else {
            pp = vec3(p.rot0.x * pos.x + p.rot0.y * pos.y + p.rot0.z * pos.z + p.rot0.w,
                      p.rot1.x * pos.x + p.rot1.y * pos.y + p.rot1.z * pos.z + p.rot1.w,
                      p.rot2.x * pos.x + p.rot2.y * pos.y + p.rot2.z * pos.z + p.rot2.w);
        }
        vec3 np = vec3(p.rot0.x * n.x + p.rot0.y * n.y + p.rot0.z * n.z,
                       p.rot1.x * n.x + p.rot1.y * n.y + p.rot1.z * n.z,
                       p.rot2.x * n.x + p.rot2.y * n.y + p.rot2.z * n.z);
#ifdef DGI_SHADING
        // M8: the shading normal in the previous view; the history keeps its
        // own shading normal in c.w (without surface ids).
        bool history_shading = m8 && p.modes.w == 0u;
        vec3 sp = vec3(p.rot0.x * sn.x + p.rot0.y * sn.y + p.rot0.z * sn.z,
                       p.rot1.x * sn.x + p.rot1.y * sn.y + p.rot1.z * sn.z,
                       p.rot2.x * sn.x + p.rot2.y * sn.y + p.rot2.z * sn.z);
#endif
        float qx = (float(id.x) + 0.5) + m.x * p.motion.x + p.motion.z;
        float qy = (float(id.y) + 0.5) + m.y * p.motion.y + p.motion.w;
        if (expected && pp.z < 0.0 && finite1(qx) && finite1(qy)) {
            float zexp = -pp.z;
            if (flow) {
                float tx = pp.x / zexp - p.prev_x.z;
                float ty = pp.y / zexp - p.prev_y.z;
                float det = p.prev_x.x * p.prev_y.y - p.prev_x.y * p.prev_y.x;
                float su = (p.prev_y.y * tx - p.prev_x.y * ty) / det * float(W);
                float sv = (-p.prev_y.x * tx + p.prev_x.x * ty) / det * float(H);
                float ex = su - qx;
                float ey = sv - qy;
                moving = !(ex * ex + ey * ey <= p.moving.x * p.moving.x);
            }
            confidence = moving ? p.moving.z : 1.0;
            float fx0 = qx - 0.5;
            float fy0 = qy - 0.5;
            float ix = floor(fx0);
            float iy = floor(fy0);
            float fx = fx0 - ix;
            float fy = fy0 - iy;
            float sum_w = 0.0;
            for (int b = 0; b < 2; ++b) {
                for (int a = 0; a < 2; ++a) {
                    float w = (a == 0 ? 1.0 - fx : fx) * (b == 0 ? 1.0 - fy : fy);
                    if (!(w > 0.0)) continue;
                    float tx = ix + float(a);
                    float ty = iy + float(b);
                    if (!(tx >= 0.0 && ty >= 0.0 && tx < float(W) && ty < float(H))) continue;
                    uint t = uint(ty) * W + uint(tx);
                    History hp = history_in[t];
                    float zh = hp.d.w;
                    if (!(zh > 0.0) || !finite1(zh)) continue;
                    if (p.modes.w != 0u) {
                        uint cid = ids[i];
                        uint hid = uint(hp.c.w);
                        if (cid != 0u && hid != 0u && cid != hid) continue;
                    }
                    vec3 nh = hp.d.xyz;
                    bool ok;
                    if (moving) {
                        ok = abs(zh - zexp) <= p.moving.y * zexp && vdot(nh, np) >= cosn;
                    } else {
                        vec3 xh = map_ray(p.prev_x, p.prev_y, tx + 0.5, ty + 0.5, W, H) * zh;
                        ok = compatible(pp, nh, zexp, xh, np, sine, depth, cosn);
                    }
#ifdef DGI_SHADING
                    if (!ok && history_shading) {
                        vec3 sh = dgi_decode_normal(floatBitsToUint(hp.c.w));
                        if (moving) {
                            ok = abs(zh - zexp) <= p.moving.y * zexp && smooth_normals(np, sp, nh, sh, cosn, ratio);
                        } else {
                            vec3 xh = map_ray(p.prev_x, p.prev_y, tx + 0.5, ty + 0.5, W, H) * zh;
                            ok = smooth_crease(pp, np, sp, zexp, xh, nh, sh, sine, depth, cosn, ratio);
                        }
                    }
#endif
                    if (!ok) continue;
                    if (!finite4(hp.a) || !finite4(hp.b) || !finite1(hp.c.x) || !finite1(hp.c.y)) continue;
                    sum_w += w;
                    hj.x += w * hp.a.x;
                    hj.y += w * hp.a.y;
                    hj.z += w * hp.a.z;
                    hm2 += w * hp.a.w;
                    ha += w * hp.b.x;
                    ha2 += w * hp.b.y;
                    hn += w * hp.b.z;
                    hna += w * hp.b.w;
                    hv += w * hp.c.x;
                    hva += w * hp.c.y;
                }
            }
            if (sum_w > 0.0) {
                accepted = true;
                float inv = 1.0 / sum_w;
                hj.x *= inv;
                hj.y *= inv;
                hj.z *= inv;
                hm2 *= inv;
                ha *= inv;
                ha2 *= inv;
                hn *= inv;
                hna *= inv;
                hv *= inv;
                hva *= inv;
                hj.x *= p.accumulate.z;
                hj.y *= p.accumulate.z;
                hj.z *= p.accumulate.z;
                hm2 *= p.accumulate.z * p.accumulate.z;
                float cap = confidence * p.accumulate.w;
                hn = min(hn, cap);
                hna = min(hna, cap);
            }
        }
    }
    if (!accepted) {
        hj = vec3(0.0);
        hm2 = 0.0;
        ha = 0.0;
        ha2 = 0.0;
        hn = 0.0;
        hna = 0.0;
        hv = 1.0;
        hva = 1.0;
        confidence = 1.0;
    }

    // Accumulation weights and light response.
    float alpha = 1.0, alpha_a = 1.0, zj = 0.0, za = 0.0;
    if (accepted && current_valid) {
        float base = max(p.accumulate.x, 1.0 / (hn + 1.0));
        float hl = luma(hj);
        float cl = luma(cur);
        float sigma2 = hn >= p.accumulate.y ? max(hm2 - hl * hl, 0.0) : var_l;
        float floor_j = p.change.z * max(cl, hl);
        float den2 = sigma2 * (1.0 + hv) + floor_j * floor_j;
        float diff = abs(cl - hl);
        zj = den2 > 0.0 ? diff / sqrt(den2) : (diff > 0.0 ? INF : 0.0);
        float rise = clamp((zj - p.change.x) / (p.change.y - p.change.x), 0.0, 1.0);
        alpha = max(base, rise);
        if (rise > base) flags |= 32u;
        float base_a = max(p.accumulate.x, 1.0 / (hna + 1.0));
        float sigma_a2 = hna >= p.accumulate.y ? max(ha2 - ha * ha, 0.0) : var_a;
        float den_a2 = sigma_a2 * (1.0 + hva) + p.change.w * p.change.w;
        float diff_a = abs(cur_a - ha);
        za = den_a2 > 0.0 ? diff_a / sqrt(den_a2) : (diff_a > 0.0 ? INF : 0.0);
        float rise_a = clamp((za - p.change.x) / (p.change.y - p.change.x), 0.0, 1.0);
        alpha_a = max(base_a, rise_a);
    }

    vec3 J;
    float m2, A, a2, N, NA, v, va;
    if (current_valid) {
        float cl = luma(cur);
        J.x = hj.x + alpha * (cur.x - hj.x);
        J.y = hj.y + alpha * (cur.y - hj.y);
        J.z = hj.z + alpha * (cur.z - hj.z);
        m2 = hm2 + alpha * (cl * cl - hm2);
        v = (1.0 - alpha) * (1.0 - alpha) * hv + alpha * alpha;
        N = 1.0 / alpha;
        A = ha + alpha_a * (cur_a - ha);
        a2 = ha2 + alpha_a * (cur_a * cur_a - ha2);
        va = (1.0 - alpha_a) * (1.0 - alpha_a) * hva + alpha_a * alpha_a;
        NA = 1.0 / alpha_a;
    } else if (accepted) {
        J = hj;
        m2 = hm2;
        A = ha;
        a2 = ha2;
        N = hn;
        NA = hna;
        v = hv;
        va = hva;
        alpha = 0.0;
        alpha_a = 0.0;
    } else {
        write_neutral(i);
        diag2[i] = vec4(0.0, 0.0, 0.0, float(flags));
        return;
    }
    float l = luma(J);
    float sigma2 = N >= p.accumulate.y ? max(m2 - l * l, 0.0) : var_l;
    float sigma_a2 = NA >= p.accumulate.y ? max(a2 - A * A, 0.0) : var_a;
    History h;
    h.a = vec4(J, m2);
    h.b = vec4(A, a2, N, NA);
    float history_tag = p.modes.w != 0u ? float(ids[i]) : 0.0;
#ifdef DGI_SHADING
    if (m8 && p.modes.w == 0u) history_tag = uintBitsToFloat(dgi_encode_normal(sn));
#endif
    h.c = vec4(v, va, confidence, history_tag);
    h.d = vec4(n, z);
    history_out[i] = h;
    flags |= 2u;
    if (accepted) flags |= 4u;
    if (moving) flags |= 8u;
    if (current_valid) flags |= 16u;
    colour_out[i] = vec4(J, A);
    variance_out[i] = vec2(sigma2 * v, sigma_a2 * va);
    diag[i] = vec4(alpha, alpha_a, N, NA);
    diag2[i] = vec4(zj, za, confidence, float(flags));
}
)glsl";

constexpr const char* kFilterMain = R"glsl(
layout(std140, set = 0, binding = 0) uniform FilterParams {
    uvec4 extent;  // width, height, step
    vec4 view_x;
    vec4 view_y;
    vec4 weights;  // luminance sigma, relative epsilon, plane sine, plane depth
    vec4 normals;  // normal cos, hard-edge ratio (M8), shading normals (1 / 0, M8)
} p;
layout(std430, set = 0, binding = 1) readonly buffer Geometry { vec4 geo[]; };
layout(std430, set = 0, binding = 2) readonly buffer ColourIn { vec4 colour_in[]; };
layout(std430, set = 0, binding = 3) readonly buffer VarianceIn { vec2 variance_in[]; };
layout(std430, set = 0, binding = 4) writeonly buffer ColourOut { vec4 colour_out[]; };
layout(std430, set = 0, binding = 5) writeonly buffer VarianceOut { vec2 variance_out[]; };
#ifdef DGI_SHADING
layout(std430, set = 0, binding = 6) readonly buffer ShadingGeometry { vec4 shading_geo[]; };
#endif

float kernel_weight(int d) {
    if (d == -2 || d == 2) return 0.0625;
    if (d == -1 || d == 1) return 0.25;
    return 0.375;
}

float colour_weight(float dist_c, float den) {
    if (!(dist_c > 0.0)) return 1.0;
    return den > 0.0 ? exp(-dist_c / den) : 0.0;
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    uint W = p.extent.x;
    uint H = p.extent.y;
    if (id.x >= W || id.y >= H) return; // no barriers in this kernel
    uint i = id.y * W + id.x;
    vec4 g = geo[i];
    vec4 c = colour_in[i];
    vec2 v = variance_in[i];
    bool centre = is_receiver(g) && finite4(c) && finite1(v.x) && finite1(v.y);
    if (!centre) {
        colour_out[i] = vec4(0.0, 0.0, 0.0, 1.0);
        variance_out[i] = vec2(0.0);
        return;
    }
    float z = g.w;
    vec3 n = g.xyz;
    vec3 pos = map_ray(p.view_x, p.view_y, float(id.x) + 0.5, float(id.y) + 0.5, W, H) * z;
#ifdef DGI_SHADING
    bool m8 = p.normals.z != 0.0;
    vec3 sn = m8 ? shading_geo[i].xyz : vec3(0.0);
#endif
    float lp = luma(c.xyz);
    float sum_j = 0.0, sum_a = 0.0, r = 0.0, gg = 0.0, b = 0.0, a = 0.0, var_j = 0.0, var_a = 0.0;
    int stride = int(p.extent.z);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int qx = int(id.x) + dx * stride;
            int qy = int(id.y) + dy * stride;
            if (qx < 0 || qy < 0 || qx >= int(W) || qy >= int(H)) continue; // before any read
            uint q = uint(qy) * W + uint(qx);
            vec4 gq = geo[q];
            if (!is_receiver(gq)) continue;
            if (dx != 0 || dy != 0) {
                vec3 pq = map_ray(p.view_x, p.view_y, float(qx) + 0.5, float(qy) + 0.5, W, H) * gq.w;
                if (!compatible(pos, n, z, pq, gq.xyz, p.weights.z, p.weights.w, p.normals.x)
#ifdef DGI_SHADING
                    && !(m8 && smooth_crease(pos, n, sn, z, pq, gq.xyz, shading_geo[q].xyz, p.weights.z, p.weights.w,
                                             p.normals.x, p.normals.y))
#endif
                )
                    continue;
            }
            vec4 cq = colour_in[q];
            vec2 vq = variance_in[q];
            if (!finite4(cq) || !finite1(vq.x) || !finite1(vq.y)) continue;
            float hk = kernel_weight(dx) * kernel_weight(dy);
            float dist_c = 0.2126 * abs(cq.x - c.x) + 0.7152 * abs(cq.y - c.y) + 0.0722 * abs(cq.z - c.z);
            float lq = luma(cq.xyz);
            float den = p.weights.x * sqrt(max(v.x + vq.x, 0.0)) + p.weights.y * (0.5 * (lp + lq));
            float wj = hk * colour_weight(dist_c, den);
            float den_a = p.weights.x * sqrt(max(v.y + vq.y, 0.0)) + p.weights.y * (0.5 * (c.w + cq.w));
            float wa = hk * colour_weight(abs(cq.w - c.w), den_a);
            sum_j += wj;
            r += wj * cq.x;
            gg += wj * cq.y;
            b += wj * cq.z;
            var_j += wj * wj * vq.x;
            sum_a += wa;
            a += wa * cq.w;
            var_a += wa * wa * vq.y;
        }
    }
    float inv = 1.0 / sum_j;
    float inv_a = 1.0 / sum_a;
    colour_out[i] = vec4(r * inv, gg * inv, b * inv, a * inv_a);
    variance_out[i] = vec2(var_j * inv * inv, var_a * inv_a * inv_a);
}
)glsl";

} // namespace

namespace {
// The M8 variant: the shading-normal paths compiled in (DGI_SHADING), each
// behind its uniform flag.
std::string WithShading(const std::string& text) {
    std::string out = text;
    out.insert(out.find('\n') + 1, "#define DGI_SHADING 1\n");
    return out;
}
} // namespace

std::string BuildDiffuseGiTemporalShader(bool shading) {
    if (!shading) return std::string(kCommon) + kTemporalMain;
    return WithShading(std::string(kCommon) + kSmoothSurface + kDiffuseGiNormalCodecGlsl + kTemporalMain);
}

std::string BuildDiffuseGiFilterShader(bool shading) {
    if (!shading) return std::string(kCommon) + kFilterMain;
    return WithShading(std::string(kCommon) + kSmoothSurface + kFilterMain);
}

} // namespace Fast::Renderer::DiffuseGi
