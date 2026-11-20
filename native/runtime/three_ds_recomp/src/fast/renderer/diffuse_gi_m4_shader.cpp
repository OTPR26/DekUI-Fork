#include "fast/renderer/diffuse_gi_m4_shader.h"

// GLSL twins of the M4 C++ paths (diffuse_gi_hierarchy.cpp, the M4 parts of
// diffuse_gi_gather.cpp, diffuse_gi_guiding.cpp), in the same operation order
// as the float instantiations, so the CPU/GPU comparison isolates GPU
// arithmetic. Own code; no third-party shader was consulted.

#include "fast/renderer/diffuse_gi_gather_shader.h"

#include <algorithm>
#include <cmath>

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr const char* kM4Common = R"glsl(
const uint CELL_GEOMETRY = 1u;
const uint CELL_PLANAR = 2u;
const uint CELL_TWO_SIDED = 4u;
const uint CELL_SKY = 8u;
const uint CELL_UNAVAILABLE = 16u;
const uint M4_FOOTPRINT = 1u;
const uint M4_CACHE_MEAN = 2u;
const uint M4_CACHE_SH = 4u;
const uint M4_GUIDING = 8u;
const uint M4_NOISE = 16u;
const uint M4_INDEPENDENT_A = 32u;
const uint M4_RECORDS = 64u;
const uint NO_BIN = 0xFFFFFFFFu;

layout(std140, set = 0, binding = 7) uniform M4Params {
    uvec4 features;    // flags, gather max level, hierarchy levels, SH order
    uvec4 level[8];    // width, height, first cell, 0
    vec4 hierarchy;    // plane tolerance, normal cos, spacing scale, 0
    uvec4 guide;       // regions x, regions y, region log2, bins
    uvec4 noise;       // width, height, depth, slice
    vec4 noise_scale;  // 1 / count
    uvec4 build;       // level being built, sections (update)
    vec4 guide_rates;  // mix, rate
} m;
layout(std430, set = 0, binding = 8) buffer Cells { vec4 cells[]; };
layout(std430, set = 0, binding = 9) buffer ShValues { vec4 sh_values[]; };

uint cell_index(uint level, uint x, uint y) { return m.level[level].z + y * m.level[level].x + x; }
uint cell_flags(uint index) { return floatBitsToUint(cells[3u * index].w) & 0xFFu; }
uint sh_count() { return m.features.w == 0u ? 0u : (m.features.w + 1u) * (m.features.w + 1u); }

// Real orthonormal SH up to order 2 ([B5] eq. 3 convention), index l^2 + l + m.
void sh_basis(float x, float y, float z, out float b[9]) {
    b[0] = 0.28209479177387814;
    b[1] = 0.4886025119029199 * y;
    b[2] = 0.4886025119029199 * z;
    b[3] = 0.4886025119029199 * x;
    b[4] = 1.0925484305920792 * (x * y);
    b[5] = 1.0925484305920792 * (y * z);
    b[6] = 0.31539156525252005 * (3.0 * z * z - 1.0);
    b[7] = 1.0925484305920792 * (x * z);
    b[8] = 0.5462742152960396 * (x * x - y * y);
}
// Clamped-cosine coefficients A_hat_l ([B5] eqs. 6, 8) of band l(k).
float clamped_cosine(uint k) {
    return k == 0u ? 3.14159265358979323846 : (k < 4u ? 2.0943951023931957 : 0.78539816339744831);
}
)glsl";

constexpr const char* kLevel0Main = R"glsl(
void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(id.x, id.y)) return; // no barriers in this kernel
    uint i = cell_index(0u, uint(id.x), uint(id.y));
    float z;
    uint c = texel_class(id.x, id.y, z);
    uint flags = 0u;
    float zmin = INF;
    float zmax = -INF;
    float w = 0.0, gx = 0.0, gy = 0.0, nx = 0.0, ny = 0.0, nz = 0.0, r = 0.0, g = 0.0, b = 0.0;
    vec4 n = vec4(0.0);
    bool geometry = false;
    if (c == 3u) {
        flags = CELL_SKY;
    } else if (!(c == 1u || c == 2u)) {
        flags = CELL_UNAVAILABLE;
    } else {
        n = normal_values[buffer_index(id.x, id.y)];
        zmin = z;
        zmax = z;
        if (!(n.x * n.x + n.y * n.y + n.z * n.z > 0.0)) {
            flags = CELL_UNAVAILABLE;
        } else {
            geometry = true;
            flags = CELL_GEOMETRY;
            vec4 rad = radiance_values[buffer_index(id.x, id.y)];
            r = rad.x;
            g = rad.y;
            b = rad.z;
            float width = float(viewport_width());
            float height = float(viewport_height());
            float u = (float(id.x) + 0.5) / width;
            float v = (float(id.y) + 0.5) / height;
            float rx = p.view_x.x * u + p.view_x.y * v + p.view_x.z;
            float ry = p.view_y.x * u + p.view_y.y * v + p.view_y.z;
            float d = (n.x * rx + n.y * ry - n.z) * z;
            float gxx = (n.x * p.view_x.x + n.y * p.view_y.x) / (width * d);
            float gyy = (n.x * p.view_x.y + n.y * p.view_y.y) / (height * d);
            bool finite_radiance = !isinf(r) && !isnan(r) && !isinf(g) && !isnan(g) && !isinf(b) && !isnan(b);
            if (d < 0.0 && !isinf(gxx) && !isnan(gxx) && !isinf(gyy) && !isnan(gyy) && finite_radiance) {
                flags |= CELL_PLANAR;
                if (c == 2u) flags |= CELL_TWO_SIDED;
                w = 1.0 / z;
                gx = gxx;
                gy = gyy;
                nx = n.x;
                ny = n.y;
                nz = n.z;
            }
        }
    }
    cells[3u * i] = vec4(w, gx, gy, uintBitsToFloat(flags | (1u << 8u)));
    cells[3u * i + 1u] = vec4(nx, ny, nz, zmin);
    cells[3u * i + 2u] = vec4(r, g, b, zmax);
    uint count = sh_count();
    float basis[9];
    sh_basis(n.x, n.y, n.z, basis);
    for (uint k = 0u; k < count; ++k) {
        float projected = clamped_cosine(k) * basis[k];
        sh_values[i * count + k] = geometry ? vec4(r * projected, g * projected, b * projected, projected) : vec4(0.0);
    }
}
)glsl";

constexpr const char* kReduceMain = R"glsl(
void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    uint l = m.build.x;
    if (id.x >= m.level[l].x || id.y >= m.level[l].y) return; // no barriers in this kernel
    uint child_w = m.level[l - 1u].x;
    uint child_h = m.level[l - 1u].y;
    float size = float(1u << l);
    float child_size = float(1u << (l - 1u));
    float xc = float(id.x) * size + 0.5 * size;
    float yc = float(id.y) * size + 0.5 * size;
    uint flags = 0u, count = 0u, children = 0u, two_sided = 0u;
    bool all_geometry = true, all_planar = true;
    float zmin = INF, zmax = -INF;
    float sum_w = 0.0, sum_gx = 0.0, sum_gy = 0.0, sum_nx = 0.0, sum_ny = 0.0, sum_nz = 0.0;
    float sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
    uint kid_index[4] = uint[4](0u, 0u, 0u, 0u);
    uvec2 kid_pos[4] = uvec2[4](uvec2(0u), uvec2(0u), uvec2(0u), uvec2(0u));
    uint shc = sh_count();
    vec4 sh_acc[9];
    for (uint k = 0u; k < 9u; ++k) sh_acc[k] = vec4(0.0);
    for (uint b = 0u; b < 2u; ++b) {
        for (uint a = 0u; a < 2u; ++a) {
            uint kx = 2u * id.x + a;
            uint ky = 2u * id.y + b;
            if (kx >= child_w || ky >= child_h) continue;
            uint ki = cell_index(l - 1u, kx, ky);
            vec4 c0 = cells[3u * ki];
            vec4 c1 = cells[3u * ki + 1u];
            vec4 c2 = cells[3u * ki + 2u];
            uint kf = floatBitsToUint(c0.w) & 0xFFu;
            uint kc = floatBitsToUint(c0.w) >> 8u;
            kid_index[children] = ki;
            kid_pos[children] = uvec2(kx, ky);
            children++;
            count += kc;
            flags |= kf & (CELL_SKY | CELL_UNAVAILABLE);
            all_geometry = all_geometry && (kf & CELL_GEOMETRY) != 0u;
            all_planar = all_planar && (kf & CELL_PLANAR) != 0u;
            if ((kf & CELL_TWO_SIDED) != 0u) two_sided++;
            zmin = min(zmin, c1.w);
            zmax = max(zmax, c2.w);
            if ((kf & CELL_PLANAR) != 0u) {
                float xk = float(kx) * child_size + 0.5 * child_size;
                float yk = float(ky) * child_size + 0.5 * child_size;
                sum_w += c0.x + c0.y * (xc - xk) + c0.z * (yc - yk);
                sum_gx += c0.y;
                sum_gy += c0.z;
                sum_nx += c1.x;
                sum_ny += c1.y;
                sum_nz += c1.z;
            }
            float weight = float(kc);
            sum_r += c2.x * weight;
            sum_g += c2.y * weight;
            sum_b += c2.z * weight;
            for (uint k = 0u; k < shc; ++k) sh_acc[k] += sh_values[ki * shc + k];
        }
    }
    if (all_geometry) flags |= CELL_GEOMETRY;
    float w = 0.0, gx = 0.0, gy = 0.0, nx = 0.0, ny = 0.0, nz = 0.0, r = 0.0, g = 0.0, bb = 0.0;
    bool planar = all_geometry && all_planar && !(two_sided != 0u && two_sided != children);
    if (planar) {
        float inverse = 1.0 / float(children);
        float cw = sum_w * inverse;
        float cgx = sum_gx * inverse;
        float cgy = sum_gy * inverse;
        float len = sqrt(sum_nx * sum_nx + sum_ny * sum_ny + sum_nz * sum_nz);
        if (!(len > 0.0)) {
            planar = false;
        } else {
            float cnx = sum_nx / len, cny = sum_ny / len, cnz = sum_nz / len;
            for (uint k = 0u; k < children && planar; ++k) {
                vec4 k0 = cells[3u * kid_index[k]];
                vec4 k1 = cells[3u * kid_index[k] + 1u];
                if (!(k1.x * cnx + k1.y * cny + k1.z * cnz >= m.hierarchy.y)) planar = false;
                float x0 = float(kid_pos[k].x) * child_size;
                float y0 = float(kid_pos[k].y) * child_size;
                float xk = x0 + 0.5 * child_size;
                float yk = y0 + 0.5 * child_size;
                for (uint corner = 0u; corner < 4u && planar; ++corner) {
                    float px = x0 + ((corner & 1u) != 0u ? child_size : 0.0);
                    float py = y0 + ((corner & 2u) != 0u ? child_size : 0.0);
                    float wc = cw + cgx * (px - xc) + cgy * (py - yc);
                    float wk = k0.x + k0.y * (px - xk) + k0.z * (py - yk);
                    if (!(wc > 0.0) || !(abs(wk - wc) <= m.hierarchy.x * wc)) planar = false;
                }
            }
            if (planar) {
                flags |= CELL_PLANAR;
                if (two_sided != 0u) flags |= CELL_TWO_SIDED;
                w = cw;
                gx = cgx;
                gy = cgy;
                nx = cnx;
                ny = cny;
                nz = cnz;
                float c = float(count);
                r = sum_r / c;
                g = sum_g / c;
                bb = sum_b / c;
            }
        }
    }
    uint out_index = cell_index(l, id.x, id.y);
    cells[3u * out_index] = vec4(w, gx, gy, uintBitsToFloat(flags | (count << 8u)));
    cells[3u * out_index + 1u] = vec4(nx, ny, nz, zmin);
    cells[3u * out_index + 2u] = vec4(r, g, bb, zmax);
    for (uint k = 0u; k < shc; ++k) sh_values[out_index * shc + k] = sh_acc[k];
}
)glsl";

// The gather with the M4 paths (after DiffuseGiGatherFunctionsGlsl: reuses
// accumulate(), process() and the M2 sides for the M2 schedules).
constexpr const char* kGatherM4Main = R"glsl(
layout(std430, set = 0, binding = 10) readonly buffer GuideTableValues { float guide_table[]; };
layout(std430, set = 0, binding = 11) buffer GuideRecords { vec4 guide_records[]; };
layout(std430, set = 0, binding = 12) readonly buffer NoiseRanks { uint noise_ranks[]; };

float g_distance;

bool uses_levels() {
    return (m.features.x & (M4_FOOTPRINT | M4_CACHE_MEAN | M4_CACHE_SH)) != 0u && p.modes.y != 0u;
}

float metric_screen_distance(float rho, float sign_side) {
    float cap = sqrt(float(viewport_width()) * float(viewport_width()) +
                     float(viewport_height()) * float(viewport_height()));
    if (!(rho < g_distance)) return cap;
    float sa = rho / g_distance;
    float ca = sqrt(max(1.0 - sa * sa, 0.0));
    vec3 r = g_v * (-ca) + g_t * (sign_side * sa);
    if (!(r.z < 0.0)) return cap;
    float zp = -g_p.z;
    float ax = p.view_x.x, bx = p.view_x.y, ay = p.view_y.x, by = p.view_y.y;
    float det = ax * by - bx * ay;
    float tx = r.x / -r.z - g_p.x / zp;
    float ty = r.y / -r.z - g_p.y / zp;
    float eu = (by * tx - bx * ty) / det * float(viewport_width());
    float ev = (-ay * tx + ax * ty) / det * float(viewport_height());
    return min(sqrt(eu * eu + ev * ev), cap);
}

struct SideM4 {
    float dx, dy, extent, lambda0, contact, sign_side, lambda, spacing;
    // natural_lambda of the next candidate (computed once for the spacing,
    // reused as that candidate's lambda) and of the last one.
    float lambda_next, lambda_last;
    int previous_x, previous_y, out_x, out_y, hx, hy;
    uint k, previous_level, level, lambda_k;
    bool done, has;
};

SideM4 make_side_m4(int side, float extent) {
    SideM4 s;
    s.dx = g_dir_x * float(side);
    s.dy = g_dir_y * float(side);
    float adx = abs(s.dx);
    float ady = abs(s.dy);
    float inv_x = adx > 0.0 ? 1.0 / adx : INF;
    float inv_y = ady > 0.0 ? 1.0 / ady : INF;
    s.extent = extent;
    s.sign_side = side > 0 ? 1.0 : -1.0;
    s.lambda0 = min(0.5 * inv_x, 0.5 * inv_y);
    float tx0 = 0.5 * inv_x;
    float ty0 = 0.5 * inv_y;
    float enter = min(tx0, ty0);
    float exit_t = tx0 < ty0 ? min(3.0 * tx0, ty0) : (ty0 < tx0 ? min(tx0, 3.0 * ty0) : 3.0 * tx0);
    s.contact = 0.5 * (enter + exit_t);
    s.k = 0u;
    s.lambda_k = 0xFFFFFFFFu;
    s.lambda_next = 0.0;
    s.lambda_last = 0.0;
    s.previous_x = 0;
    s.previous_y = 0;
    s.previous_level = 0u;
    s.done = false;
    s.has = false;
    s.lambda = 0.0;
    s.spacing = 0.0;
    s.out_x = 0;
    s.out_y = 0;
    s.hx = 0;
    s.hy = 0;
    s.level = 0u;
    if (p.modes.y == 1u && !(s.extent > s.lambda0)) s.done = true;
    if (p.modes.y == 2u && !(s.extent > s.contact)) s.done = true;
    return s;
}

float natural_lambda(SideM4 s, uint k) {
    uint count = max(p.modes.z, 1u);
    float u = (float(k) + g_radial_jitter) / float(count);
    float shaped = u > 0.0 ? pow(u, p.radial.x) : 0.0;
    if (p.modes.y == 1u) return s.lambda0 + (s.extent - s.lambda0) * shaped;
    float lambda = metric_screen_distance(p.gather.x * shaped, s.sign_side);
    return lambda < s.contact ? s.contact : lambda;
}

uint footprint_level(float spacing, uint max_level) {
    uint level = 0u;
    while (level < max_level && float(2u << level) <= spacing) level++;
    return level;
}

void advance_m4(inout SideM4 s) {
    s.has = false;
    uint count = max(p.modes.z, 1u);
    bool levels = uses_levels();
    bool footprint = (m.features.x & M4_FOOTPRINT) != 0u;
    while (!s.done && s.k < count) {
        uint k = s.k++;
        float lambda = s.lambda_k == k ? s.lambda_next : natural_lambda(s, k);
        if (lambda > s.extent) { s.done = true; return; }
        int cx = int(floor(lambda * s.dx + 0.5));
        int cy = int(floor(lambda * s.dy + 0.5));
        uint level = 0u;
        uint requested = 0u;
        int hx = cx;
        int hy = cy;
        int tx = g_x + cx;
        int ty = g_y + cy;
        float spacing;
        if (k + 1u < count) {
            s.lambda_next = natural_lambda(s, k + 1u);
            s.lambda_k = k + 1u;
            spacing = s.lambda_next - lambda;
        } else {
            spacing = k > 0u ? lambda - s.lambda_last : s.extent - lambda;
        }
        s.lambda_last = lambda;
        if (levels) requested = footprint_level(spacing * m.hierarchy.z, m.features.y);
        if (levels && inside(tx, ty) && !(cx == 0 && cy == 0)) {
            level = requested;
            while (level > 0u) {
                uint ix = uint(tx) >> level;
                uint iy = uint(ty) >> level;
                bool own = (uint(g_x) >> level) == ix && (uint(g_y) >> level) == iy;
                if (!own && (cell_flags(cell_index(level, ix, iy)) & CELL_PLANAR) != 0u) {
                    hx = int(ix);
                    hy = int(iy);
                    break;
                }
                level--;
            }
            if (level == 0u) { hx = cx; hy = cy; }
        }
        uint key_level = footprint ? level : 0u;
        int key_x = footprint ? hx : cx;
        int key_y = footprint ? hy : cy;
        if ((cx == 0 && cy == 0) || (key_level == s.previous_level && key_x == s.previous_x && key_y == s.previous_y))
            continue;
        s.previous_level = key_level;
        s.previous_x = key_x;
        s.previous_y = key_y;
        s.has = true;
        s.lambda = lambda;
        s.out_x = cx;
        s.out_y = cy;
        s.level = level;
        s.hx = hx;
        s.hy = hy;
        return;
    }
    s.done = true;
}

void slab(float o, float d, float b0, float b1, inout float la, inout float lb) {
    if (abs(d) > 0.0) {
        float t0 = (b0 - o) / d;
        float t1 = (b1 - o) / d;
        if (t0 > t1) { float t = t0; t0 = t1; t1 = t; }
        la = max(la, t0);
        lb = min(lb, t1);
    } else if (o < b0 || o > b1) {
        lb = -INF;
    }
}

vec3 lift(float lambda, float ox, float oy, float dx, float dy, vec4 c0, float xc, float yc) {
    float px = ox + lambda * dx;
    float py = oy + lambda * dy;
    float w = c0.x + c0.y * (px - xc) + c0.z * (py - yc);
    return ray(px, py) * (1.0 / w);
}

bool below_plane(vec3 e) {
    vec3 rel = e - g_p;
    return vdot(rel, g_n) <= p.radial.z * vlength(rel) + p.radial.w * g_zp;
}

vec3 back_of(vec3 e, float radius) {
    vec3 rel = e - g_p;
    float dist = vlength(rel);
    vec3 dir = vnormalize(e);
    float along = vdot(rel, dir);
    float clip_length = -along + sqrt(max(along * along - (dist * dist - radius * radius), 0.0));
    float seg = max(min(p.gather.y, clip_length), 0.0);
    return e + dir * seg;
}

bool near_receiver(vec3 u, vec3 v) {
    vec3 e = v - u;
    vec3 r = g_p - u;
    float ee = vdot(e, e);
    float t = ee > 0.0 ? clamp(vdot(r, e) / ee, 0.0, 1.0) : 0.0;
    return vlength(r - e * t) <= 1.0e-6 * g_zp;
}

// Twin of EvaluateCoarseCandidate (+ AccumulateCentre of its interval).
void process_coarse(float lambda, int side, int cell_x, int cell_y, uint level, int hx, int hy) {
    g_scheduled++;
    int xx = g_x + cell_x;
    int yy = g_y + cell_y;
    float zq_texel;
    uint cls = texel_class(xx, yy, zq_texel);
    g_valid++;
    uint ci = cell_index(level, uint(hx), uint(hy));
    vec4 c0 = cells[3u * ci];
    vec4 c1 = cells[3u * ci + 1u];
    vec4 c2 = cells[3u * ci + 2u];
    float radius = p.gather.x;
    float sign_side = float(side);
    vec3 corners[4];
    uint corner_count = 0u;
    vec3 q;
    vec3 nq;
    if ((m.features.x & M4_FOOTPRINT) != 0u) {
        float size = float(1u << level);
        float x0 = float(hx) * size;
        float y0 = float(hy) * size;
        float x1 = min(x0 + size, float(viewport_width()));
        float y1 = min(y0 + size, float(viewport_height()));
        float xc = x0 + 0.5 * size;
        float yc = y0 + 0.5 * size;
        nq = c1.xyz;
        if ((p.modes.w & 2u) != 0u) {
            float wp = c0.x + c0.y * (float(g_x) + 0.5 - xc) + c0.z * (float(g_y) + 0.5 - yc);
            float w0 = 1.0 / g_zp;
            if (vdot(nq, g_n) >= m.hierarchy.y && abs(wp - w0) <= m.hierarchy.x * w0) return;
        }
        float ox = float(g_x) + 0.5;
        float oy = float(g_y) + 0.5;
        float dx = g_dir_x * sign_side;
        float dy = g_dir_y * sign_side;
        float la = 0.0;
        float lb = INF;
        slab(ox, dx, x0, x1, la, lb);
        slab(oy, dy, y0, y1, la, lb);
        if (!(lb >= la)) { la = lambda; lb = lambda; }
        vec3 qa = lift(la, ox, oy, dx, dy, c0, xc, yc);
        vec3 qb = lift(lb, ox, oy, dx, dy, c0, xc, yc);
        vec3 d = qb - qa;
        vec3 f = qa - g_p;
        float a2 = vdot(d, d);
        float b2 = 2.0 * vdot(f, d);
        float c2v = vdot(f, f) - radius * radius;
        float t0 = 0.0;
        float t1 = 1.0;
        if (a2 > 0.0) {
            float disc = b2 * b2 - 4.0 * a2 * c2v;
            if (disc < 0.0) return;
            float root = sqrt(disc);
            t0 = max(0.0, (-b2 - root) / (2.0 * a2));
            t1 = min(1.0, (-b2 + root) / (2.0 * a2));
        } else if (c2v > 0.0) {
            t1 = -1.0;
        }
        if (!(t1 >= t0)) return;
        vec3 fa = qa + d * t0;
        vec3 fb = qa + d * t1;
#ifdef DGI_SHADING
        if ((p.modes.w & 2u) != 0u && (p.modes.w & 8u) != 0u) {
            uint qi = buffer_index(xx, yy);
            if (hidden_m8(fa, nq, qi) && hidden_m8(fb, nq, qi)) return;
        } else
#endif
        if ((p.modes.w & 2u) != 0u && below_plane(fa) && below_plane(fb)) return;
        vec3 ba = back_of(fa, radius);
        vec3 bb = back_of(fb, radius);
        if (near_receiver(fa, ba) || near_receiver(fb, bb) || near_receiver(fa, fb)) return;
        corners[0] = fa;
        corners[1] = fb;
        corners[2] = ba;
        corners[3] = bb;
        corner_count = 4u;
        q = lift(lambda, ox, oy, dx, dy, c0, xc, yc);
    } else {
        float zq = zq_texel;
        nq = normal_values[buffer_index(xx, yy)].xyz;
        if ((p.modes.w & 2u) != 0u) {
            vec3 centre = ray(float(xx) + 0.5, float(yy) + 0.5) * zq;
#ifdef DGI_SHADING
            if ((p.modes.w & 8u) != 0u) {
                if (hidden_m8(centre, nq, buffer_index(xx, yy))) return;
            } else
#endif
            {
                vec3 dd = centre - g_p;
                float elevation = vdot(dd, g_n);
                if (elevation <= p.radial.z * vlength(dd) + p.radial.w * g_zp) return;
            }
        }
        float px = float(g_x) + 0.5 + lambda * g_dir_x * sign_side;
        float py = float(g_y) + 0.5 + lambda * g_dir_y * sign_side;
        q = ray(px, py) * zq;
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
        corners[0] = q;
        corners[1] = back;
        corner_count = 2u;
    }
    vec3 rel0 = corners[0] - g_p;
    float theta0 = atan(vdot(rel0, g_t), vdot(rel0, g_v));
    float low_delta = 0.0;
    float high_delta = 0.0;
    for (uint c = 1u; c < corner_count; ++c) {
        vec3 rel = corners[c] - g_p;
        float delta = atan(vdot(rel, g_t), vdot(rel, g_v)) - theta0;
        if (delta > PI) delta -= 2.0 * PI;
        else if (delta <= -PI) delta += 2.0 * PI;
        low_delta = min(low_delta, delta);
        high_delta = max(high_delta, delta);
    }
    if (high_delta - low_delta >= PI) return;
    float lo = theta0 + low_delta;
    float hi = theta0 + high_delta;
    float two_pi = 2.0 * PI;
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
    vec3 radiance;
    if ((m.features.x & M4_CACHE_MEAN) != 0u) {
        radiance = c2.xyz;
    } else if ((m.features.x & M4_CACHE_SH) != 0u) {
        vec3 omega = vnormalize(g_p - q);
        float basis[9];
        sh_basis(omega.x, omega.y, omega.z, basis);
        uint shc = sh_count();
        float n0 = 0.0, n1 = 0.0, n2 = 0.0, dsum = 0.0;
        for (uint k = 0u; k < shc; ++k) {
            vec4 cf = sh_values[ci * shc + k];
            n0 += cf.x * basis[k];
            n1 += cf.y * basis[k];
            n2 += cf.z * basis[k];
            dsum += cf.w * basis[k];
        }
        if (!(dsum > 0.0)) {
            radiance = vec3(0.0);
        } else {
            float inv = 1.0 / dsum;
            radiance = vec3(n0 * inv, n1 * inv, n2 * inv);
        }
    } else {
        radiance = radiance_values[buffer_index(xx, yy)].xyz;
    }
    if (cls == 1u) {
        if (vdot(nq, nq) > 0.0) {
            if (!(vdot(nq, g_p - q) > 0.0)) radiance = vec3(0.0);
        }
    }
    accumulate(pieces, piece_count, radiance);
}

float noise_value(uint x, uint y, bool offset) {
    uint ox = offset ? m.noise.x / 2u : 0u;
    uint oy = offset ? m.noise.y / 2u : 0u;
    uint xx = (x + ox) % m.noise.x;
    uint yy = (y + oy) % m.noise.y;
    uint rank = noise_ranks[(m.noise.w * m.noise.y + yy) * m.noise.x + xx];
    return (float(rank) + 0.5) * m.noise_scale.x;
}

float guide_draw(uint region, float u, out float pdf, out uint bin) {
    uint bins = m.guide.w;
    uint base = region * (2u * bins + 1u);
    uint b = 0u;
    while (b + 1u < bins && !(u < guide_table[base + b + 1u])) b++;
    float lo = guide_table[base + b];
    float hi = guide_table[base + b + 1u];
    float fraction = (u - lo) / (hi - lo);
    fraction = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    pdf = guide_table[base + bins + 1u + b];
    bin = b;
    return (float(b) + fraction) * (PI / float(bins));
}

// One section: setup, both sides merged by screen distance, open weight.
bool run_section(float phi, out vec3 f_sum, out float fa, out float occupied_sectors) {
    f_sum = vec3(0.0);
    fa = 0.0;
    occupied_sectors = 0.0;
    float z0;
    texel_class(g_x, g_y, z0);
    uint sectors = p.extent_counts.w;
    float ax = p.view_x.x, bx = p.view_x.y, ay = p.view_y.x, by = p.view_y.y;
    g_zp = z0;
    g_p = ray(float(g_x) + 0.5, float(g_y) + 0.5) * z0;
    g_v = vnormalize(g_p * -1.0);
    vec3 helper = abs(g_v.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 e1 = vnormalize(vcross(helper, g_v));
    vec3 e2 = vcross(g_v, e1);
    g_t = e1 * cos(phi) + e2 * sin(phi);
#ifdef DGI_SHADING
    g_a = vdot(g_ns, g_v);
    g_b = vdot(g_ns, g_t);
#else
    g_a = vdot(g_n, g_v);
    g_b = vdot(g_n, g_t);
#endif
    float zp = -g_p.z;
    float gx = g_t.x * zp + g_p.x * g_t.z;
    float gy = g_t.y * zp + g_p.y * g_t.z;
    float det = ax * by - bx * ay;
    float du = (by * gx - bx * gy) / det;
    float dv = (-ay * gx + ax * gy) / det;
    float pxd = du * float(viewport_width());
    float pyd = dv * float(viewport_height());
    float len = sqrt(pxd * pxd + pyd * pyd);
    if (!(len > 0.0) || isinf(len) || isnan(len)) return false;
    g_dir_x = pxd / len;
    g_dir_y = pyd / len;
    float cap = sqrt(float(viewport_width()) * float(viewport_width()) +
                     float(viewport_height()) * float(viewport_height()));
    float distance_p = vlength(g_p);
    g_distance = distance_p;
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
    bool interleave = (p.modes.w & 1u) != 0u;
    if (p.modes.y == 2u || uses_levels()) {
        SideM4 plus = make_side_m4(1, extents[0]);
        SideM4 minus = make_side_m4(-1, extents[1]);
        advance_m4(plus);
        advance_m4(minus);
        while (plus.has || minus.has) {
            bool take_plus = !minus.has || (plus.has && (!interleave || plus.lambda <= minus.lambda));
            if (take_plus) {
                if (plus.level > 0u) process_coarse(plus.lambda, 1, plus.out_x, plus.out_y, plus.level, plus.hx, plus.hy);
                else process(plus.lambda, 1, plus.out_x, plus.out_y);
                advance_m4(plus);
            } else {
                if (minus.level > 0u) process_coarse(minus.lambda, -1, minus.out_x, minus.out_y, minus.level, minus.hx, minus.hy);
                else process(minus.lambda, -1, minus.out_x, minus.out_y);
                advance_m4(minus);
            }
        }
    } else {
        Side plus = make_side(1, extents[0]);
        Side minus = make_side(-1, extents[1]);
        advance(plus);
        advance(minus);
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
    float occ = 0.0;
    for (uint w = 0u; w < 8u; ++w) occ += float(bitCount(g_mask[w]));
    f_sum = g_sum;
    fa = open_weight;
    occupied_sectors = occ;
    return true;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(id.x, id.y)) return; // no barriers in this kernel
    uint out_index = uint(id.y) * viewport_width() + uint(id.x);
    uint sections = p.extent_counts.z;
    bool records = (m.features.x & M4_RECORDS) != 0u;
    results[out_index] = vec4(0.0, 0.0, 0.0, 1.0);
    aux[out_index] = vec4(0.0);
    if (records)
        for (uint s = 0u; s < sections; ++s) guide_records[out_index * sections + s] = vec4(0.0, 0.0, 0.0, uintBitsToFloat(NO_BIN));
    g_x = id.x;
    g_y = id.y;
    float z0;
    uint c = texel_class(id.x, id.y, z0);
    uint receiver = buffer_index(id.x, id.y);
    vec3 n = normal_values[receiver].xyz;
    if (!(c == 1u || c == 2u) || !(vdot(n, n) > 0.0)) return;
    g_n = n;
#ifdef DGI_SHADING
    // M8: the lobe of the shading normal (the geometric normal without one).
    g_ns = n;
    if ((p.modes.w & 8u) != 0u) {
        vec3 sn = shading_values[receiver].xyz;
        if (vdot(sn, sn) > 0.0) g_ns = sn;
    }
#endif
    vec3 total = vec3(0.0);
    float total_a = 0.0;
    float occupied = 0.0;
    g_scheduled = 0u;
    g_valid = 0u;
    float rotation = pixel_rotation(uint(id.x), uint(id.y));
    g_radial_jitter = pixel_radial_jitter(uint(id.x), uint(id.y));
    if ((m.features.x & M4_NOISE) != 0u) {
        rotation = noise_value(uint(id.x), uint(id.y), false);
        g_radial_jitter = noise_value(uint(id.x), uint(id.y), true);
    }
    bool guided = (m.features.x & M4_GUIDING) != 0u;
    bool independent_a = guided && (m.features.x & M4_INDEPENDENT_A) != 0u;
    uint region = (uint(id.y) >> m.guide.z) * m.guide.x + (uint(id.x) >> m.guide.z);
    for (uint section = 0u; section < sections; ++section) {
        float phi;
        float inverse_pdf = PI;
        float density = 1.0 / PI;
        uint bin = NO_BIN;
        if (guided) {
            float u = (float(section) + rotation) / float(sections);
            phi = guide_draw(region, u, density, bin);
            inverse_pdf = 1.0 / density;
        } else {
            phi = PI * (float(section) + rotation) / float(sections);
            if (records) bin = min(uint(phi * float(m.guide.w) / PI), m.guide.w - 1u);
        }
        vec3 f_sum;
        float fa;
        float occ;
        if (!run_section(phi, f_sum, fa, occ)) {
            results[out_index] = vec4(0.0, 0.0, 0.0, 1.0);
            aux[out_index] = vec4(0.0);
            if (records)
                for (uint s = 0u; s < sections; ++s)
                    guide_records[out_index * sections + s] = vec4(0.0, 0.0, 0.0, uintBitsToFloat(NO_BIN));
            return;
        }
        total.x += f_sum.x * inverse_pdf;
        total.y += f_sum.y * inverse_pdf;
        total.z += f_sum.z * inverse_pdf;
        if (independent_a) {
            vec3 f_unused;
            float fa_uniform;
            float occ_unused;
            float phi_a = PI * (float(section) + rotation) / float(sections);
            if (!run_section(phi_a, f_unused, fa_uniform, occ_unused)) {
                results[out_index] = vec4(0.0, 0.0, 0.0, 1.0);
                aux[out_index] = vec4(0.0);
                if (records)
                    for (uint s = 0u; s < sections; ++s)
                        guide_records[out_index * sections + s] = vec4(0.0, 0.0, 0.0, uintBitsToFloat(NO_BIN));
                return;
            }
            total_a += fa_uniform * PI;
        } else {
            total_a += fa * inverse_pdf;
        }
        occupied += occ;
        if (records)
            guide_records[out_index * sections + section] =
                vec4(0.2126 * f_sum.x + 0.7152 * f_sum.y + 0.0722 * f_sum.z, density, phi, uintBitsToFloat(bin));
    }
    float inverse = 1.0 / float(sections);
    results[out_index] = vec4(total.x * inverse, total.y * inverse, total.z * inverse, total_a * inverse);
    float confidence = g_scheduled == 0u ? 0.0 : float(g_valid) / float(g_scheduled);
    aux[out_index] = vec4(confidence, 1.0, float(g_scheduled), occupied);
}
)glsl";

// Guide update: one invocation per region (8 x 8 workgroups over the region
// grid). Twin of GuideField::Update with float arithmetic and of RegionTable<float>.
constexpr const char* kGuideUpdateMain = R"glsl(
layout(std430, set = 0, binding = 11) readonly buffer GuideRecordsIn { vec4 guide_records[]; };
layout(std430, set = 0, binding = 13) readonly buffer GuideStateIn { float state_in[]; };
layout(std430, set = 0, binding = 14) writeonly buffer GuideStateOut { float state_out[]; };
layout(std430, set = 0, binding = 15) writeonly buffer GuideTableOut { float table_out[]; };

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= m.guide.x || id.y >= m.guide.y) return; // no barriers in this kernel
    uint r = id.y * m.guide.x + id.x;
    uint bins = m.guide.w;
    uint size = 1u << m.guide.z;
    uint sections = m.build.y;
    uint width = viewport_width();
    uint height = viewport_height();
    float sums[32];
    for (uint b = 0u; b < 32u; ++b) sums[b] = 0.0;
    uint n = 0u;
    bool broken = false;
    for (uint y = id.y * size; y < min((id.y + 1u) * size, height); ++y)
        for (uint x = id.x * size; x < min((id.x + 1u) * size, width); ++x)
            for (uint s = 0u; s < sections; ++s) {
                vec4 record = guide_records[(y * width + x) * sections + s];
                uint bin = floatBitsToUint(record.w);
                if (bin == NO_BIN) continue;
                n++;
                float value = record.x / record.y;
                if (isinf(value) || isnan(value) || value < 0.0 || bin >= bins) broken = true;
                else sums[bin] += value;
            }
    uint sbase = r * (bins + 1u);
    float memory[32];
    for (uint b = 0u; b < bins; ++b) memory[b] = state_in[sbase + b];
    bool valid = state_in[sbase + bins] != 0.0;
    if (broken) {
        for (uint b = 0u; b < bins; ++b) memory[b] = 0.0;
        valid = false;
    } else if (n > 0u) {
        float beta = m.guide_rates.y;
        float total = 0.0;
        for (uint b = 0u; b < bins; ++b) {
            float e = sums[b] / float(n);
            float estimate = valid ? (1.0 - beta) * memory[b] + beta * e : e;
            memory[b] = estimate;
            total += estimate;
        }
        if (!(total > 0.0) || isinf(total) || isnan(total)) {
            for (uint b = 0u; b < bins; ++b) memory[b] = 0.0;
            valid = false;
        } else {
            valid = true;
        }
    }
    for (uint b = 0u; b < bins; ++b) state_out[sbase + b] = memory[b];
    state_out[sbase + bins] = valid ? 1.0 : 0.0;
    // Table of the region (RegionTable<float>).
    float delta = PI / float(bins);
    float total = 0.0;
    if (valid)
        for (uint b = 0u; b < bins; ++b) total += memory[b];
    bool uniform_table = !valid || !(total > 0.0) || isinf(total) || isnan(total);
    float eta = m.guide_rates.x;
    float base_probability = (1.0 - eta) / float(bins);
    uint tbase = r * (2u * bins + 1u);
    float running = 0.0;
    float cdf[33];
    cdf[0] = 0.0;
    for (uint b = 0u; b < bins; ++b) {
        float share = uniform_table ? 1.0 / float(bins) : memory[b] / total;
        running += base_probability + eta * share;
        cdf[b + 1u] = b + 1u == bins ? 1.0 : min(running, 1.0);
    }
    for (uint b = 0u; b <= bins; ++b) table_out[tbase + b] = cdf[b];
    for (uint b = 0u; b < bins; ++b) table_out[tbase + bins + 1u + b] = (cdf[b + 1u] - cdf[b]) / delta;
}
)glsl";

} // namespace

uint32_t HierarchyCellCount(uint32_t width, uint32_t height, uint32_t levels) {
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; ++l)
        total += ((width + (1U << l) - 1U) >> l) * ((height + (1U << l) - 1U) >> l);
    return total;
}

M4ShaderParams MakeM4ShaderParams(const GatherInput& input, const GatherSettings& settings,
                                  const HierarchySettings& hierarchy, const GuideTable* guide,
                                  const GuidingSettings* guiding, const NoiseMask* noise, bool records) {
    M4ShaderParams p;
    uint32_t flags = 0U;
    if (settings.HierarchyFootprint) flags |= kM4FlagFootprint;
    if (settings.Cache == CacheMode::Mean) flags |= kM4FlagCacheMean;
    if (settings.Cache == CacheMode::SphericalHarmonics) flags |= kM4FlagCacheSh;
    if (settings.Guiding) flags |= kM4FlagGuiding;
    if (settings.Rotations.Mode == RotationMode::BlueNoise) flags |= kM4FlagNoise;
    if (settings.GuidingAccessibility == GuidedAccessibility::IndependentUniform) flags |= kM4FlagIndependentA;
    if (records) flags |= kM4FlagRecords;
    const uint32_t levels = std::clamp(hierarchy.Levels, 1U, kMaxHierarchyLevels);
    p.Features = {flags, std::min(settings.HierarchyMaxLevel, levels - 1U), levels,
                  hierarchy.ShOrder == 0U ? 0U : std::min(hierarchy.ShOrder, kMaxShOrder)};
    uint32_t offset = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        const uint32_t w = (input.Viewport.Width + (1U << l) - 1U) >> l;
        const uint32_t h = (input.Viewport.Height + (1U << l) - 1U) >> l;
        p.Levels[l] = {w, h, offset, 0U};
        offset += w * h;
    }
    p.Hierarchy = {static_cast<float>(hierarchy.PlaneTolerance), static_cast<float>(hierarchy.NormalCos),
                   static_cast<float>(settings.HierarchySpacingScale), 0.0F};
    if (guide != nullptr) p.Guide = {guide->RegionsX, guide->RegionsY, guide->RegionLog2, guide->Bins};
    if (guiding != nullptr) p.GuideRates = {static_cast<float>(guiding->Mix), static_cast<float>(guiding->Rate), 0.0F, 0.0F};
    if (noise != nullptr) {
        p.Noise = {noise->Width, noise->Height, noise->Depth, static_cast<uint32_t>(settings.Rotations.Frame % noise->Depth)};
        p.NoiseScale = {1.0F / static_cast<float>(noise->Count()), 0.0F, 0.0F, 0.0F};
    }
    p.Build = {0U, settings.Sections, 0U, 0U};
    return p;
}

std::vector<float> PackGuideTable(const GuideTable& table) {
    const size_t regions = static_cast<size_t>(table.RegionsX) * table.RegionsY;
    std::vector<float> out;
    out.reserve(regions * (2U * table.Bins + 1U));
    for (size_t r = 0; r < regions; ++r) {
        for (uint32_t b = 0; b <= table.Bins; ++b) out.push_back(static_cast<float>(table.Cdf[r * (table.Bins + 1U) + b]));
        for (uint32_t b = 0; b < table.Bins; ++b) out.push_back(static_cast<float>(table.Pdf[r * table.Bins + b]));
    }
    return out;
}

std::vector<float> PackGuideState(const GuideField& field) {
    const uint32_t bins = field.Settings().Bins;
    const size_t regions = field.Valid().size();
    std::vector<float> out;
    out.reserve(regions * (bins + 1U));
    for (size_t r = 0; r < regions; ++r) {
        for (uint32_t b = 0; b < bins; ++b) out.push_back(static_cast<float>(field.Memory()[r * bins + b]));
        out.push_back(field.Valid()[r] != 0U ? 1.0F : 0.0F);
    }
    return out;
}

std::string BuildDiffuseGiHierarchyLevel0Shader(bool shading) {
    return DiffuseGiGatherFunctionsGlsl(shading) + kM4Common + kLevel0Main;
}
std::string BuildDiffuseGiHierarchyReduceShader(bool shading) {
    return DiffuseGiGatherFunctionsGlsl(shading) + kM4Common + kReduceMain;
}
std::string BuildDiffuseGiGatherM4Shader(bool shading) {
    return DiffuseGiGatherFunctionsGlsl(shading) + kM4Common + kGatherM4Main;
}
std::string BuildDiffuseGiGuideUpdateShader() { return DiffuseGiGatherFunctionsGlsl() + kM4Common + kGuideUpdateMain; }

} // namespace Fast::Renderer::DiffuseGi
