// SPDX-FileCopyrightText: 2026 LittleSixNine
// SPDX-License-Identifier: GPL-3.0-or-later
// Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

// overscan.metal -- RFC-032 §26-§27: the film outside the frame, as exposure.
//
// All three kernels read one parameter block `P` (floats; layout in
// `overscan.cpp`, `enum OsP`, which is the single source of the indices).
// Geometry is in film millimetres: `s` along the film, `t` across it, `t = 0`
// at the film's first long edge. A canvas pixel maps to film coordinates
// through the scan registration (a rotation and an offset), so the film --
// gate, perforations, edge print -- can sit a hair off square in the output,
// as it does in a real scan.
#include <metal_stdlib>
using namespace metal;

enum : uint {
    P_CW = 0, P_CH, P_PX, P_FW, P_FH,
    P_A00, P_A01, P_A10, P_A11, P_CS, P_CT, P_CU, P_CV,
    P_VERTICAL,
    P_GS0, P_GT0, P_GW, P_GH, P_CORNER, P_PENUMBRA,
    P_WOB_A = 20,                 // 4 sides x 3 harmonics
    P_WOB_PH = 32,                // 4 sides x 3 harmonics
    P_FILM_W = 44, P_FOG_AMP, P_FOG_WIDTH, P_FOG_SEED, P_FOG_PERIOD,
    P_FOG_R, P_FOG_G, P_FOG_B,
    P_N_LEAKS = 52,
    P_PERFORATED = 83, P_PERF_PITCH, P_PERF_W, P_PERF_H, P_PERF_EDGE, P_PERF_R, P_PERF_PHASE,
    P_HOLE_C = 90, P_HOLE_M, P_HOLE_Y,
    P_FLARE_AMP = 93, P_FLARE_WIDTH,
    P_GATE_R = 95,                // 4 corner radii: (s-,t-), (s+,t-), (s+,t+), (s-,t+)
    P_N_Q = 99, P_Q = 100,        // up to 8 x (4 vertices s,t; round; sign: +1/-1 quad adds/cuts, +2/-2 ellipse (cs, ct, rs, rt))
    P_ROUGH_AMP = 180, P_ROUGH_PERIOD, P_ROUGH_SEED,
    P_HOLES_LIGHT = 183, P_LIGHT_R, P_LIGHT_G, P_LIGHT_B,
    P_PERF_SEED = 187, P_LIGHT_FALL, P_LIGHT_DIR,
    P_WALL_T = 190, P_WALL_VS, P_WALL_VT, P_WALL_PAR, P_BASE_R, P_BASE_G, P_BASE_B, P_WALL_GLOW, P_WALL_SCATTER,
    P_CARRIER = 199,
    P_LEAKS = 200,          // up to 12 x (s, side, amp, sigma_s, sigma_t)
    P_PAIR_ADV = 260,       // > 0: the same gate exposed again this far along (a half-frame pair)
    P_SCRATCH_STRENGTH = 261, P_SCRATCH_LONG, P_SCRATCH_SEED, P_SCRATCH_EDGE, P_PERF_LEAK, P_PERF_LEAK_SPREAD, P_FULL_WIDTH, P_DUST_AMOUNT, P_DUST_SIZE, P_DUST_STRENGTH, P_DUST_FULL_FRAME, P_DUST_FIBER_RATIO,
    P_PERF_LEAK_STYLE = 273,
    P_PERF_LEAK_COVERAGE = 274,
    P_PERF_LEAK_SOFTNESS = 275,
    P_PERF_LEAK_SEED = 276,
    P_COUNT = 277
};
constant uint kQStride = 10u;

static float2 canvas_to_film(float x, float y, device const float* P) {
    const float u = (x + 0.5f) * P[P_PX] - P[P_CU];
    const float v = (y + 0.5f) * P[P_PX] - P[P_CV];
    return float2(P[P_A00] * u + P[P_A01] * v + P[P_CS], P[P_A10] * u + P[P_A11] * v + P[P_CT]);
}

static uint hash_u(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float hash_f(int i, uint seed) { return float(hash_u(uint(i) * 0x9E3779B9u ^ seed)) * (1.0f / 4294967295.0f); }
static float value_noise(float x, uint seed) {
    const float i0 = floor(x), f = x - i0;
    const float w = f * f * (3.0f - 2.0f * f);
    return mix(hash_f(int(i0), seed), hash_f(int(i0) + 1, seed), w);
}

// Signed distance to a convex quad (either winding; a repeated vertex makes a
// triangle), negative inside.
static float sd_quad(float2 p, device const float* q) {
    float2 v[4] = {float2(q[0], q[1]), float2(q[2], q[3]), float2(q[4], q[5]), float2(q[6], q[7])};
    float d = dot(p - v[0], p - v[0]);
    float sgn = 1.0f;
    for (uint i = 0u, j = 3u; i < 4u; j = i, ++i) {
        const float2 e = v[j] - v[i], w = p - v[i];
        const float ee = dot(e, e);
        if (ee < 1e-12f) continue;
        const float2 b = w - e * clamp(dot(w, e) / ee, 0.0f, 1.0f);
        d = min(d, dot(b, b));
        const bool c0 = p.y >= v[i].y, c1 = p.y < v[j].y, c2 = e.x * w.y > e.y * w.x;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) sgn = -sgn;
    }
    return sgn * sqrt(d);
}

// The gate (RFC-032 §29). Real gates are not symmetric rounded rectangles:
// each corner has its own radius, and a camera's gate carries its own
// features -- the eared sides of a 645 whose film rails stand proud, the
// shoulders of a 6x8 mask, the kicked-out corners of a 6x6 insert, a burr.
// Those come in as convex quads that add to or cut from the opening, and a
// fine roughness models the machined edge. On top, each side keeps the
// camera's low-frequency wobble (three harmonics, vanishing at the corners).
// Signed distance, negative inside, in mm.
static float gate_sdf(float2 st, device const float* P) {
    const float hw = 0.5f * P[P_GW], hh = 0.5f * P[P_GH];
    const float ds = st.x - (P[P_GS0] + hw), dt = st.y - (P[P_GT0] + hh);
    const float r = ds > 0.0f ? (dt > 0.0f ? P[P_GATE_R + 2u] : P[P_GATE_R + 1u])
                              : (dt > 0.0f ? P[P_GATE_R + 3u] : P[P_GATE_R + 0u]);
    const float qs = fabs(ds) - (hw - r), qt = fabs(dt) - (hh - r);
    float d = length(max(float2(qs, qt), 0.0f)) + min(max(qs, qt), 0.0f) - r;
    uint side; float along, len;
    if (qs > qt) { side = ds > 0.0f ? 1u : 3u; along = dt + hh; len = 2.0f * hh; }
    else         { side = dt > 0.0f ? 2u : 0u; along = ds + hw; len = 2.0f * hw; }
    const float u = clamp(along / len, 0.0f, 1.0f);
    float w = 0.0f;
    for (uint k = 0; k < 3u; ++k)
        w += P[P_WOB_A + side * 3u + k] * sin(float(k + 1u) * M_PI_F * u + P[P_WOB_PH + side * 3u + k]);
    d -= w * sin(M_PI_F * u);
    const uint nq = uint(P[P_N_Q]);
    for (uint k = 0; k < nq; ++k) {
        device const float* q = P + P_Q + kQStride * k;
        float dq;
        if (fabs(q[9]) > 1.5f) {
            // an ellipse: centre q[0..1], radii q[2..3]; distance scaled by the
            // smaller radius, exact on the boundary, which is all a penumbra reads
            const float2 r = max(float2(q[2], q[3]), float2(1e-4f));
            dq = (length((st - float2(q[0], q[1])) / r) - 1.0f) * min(r.x, r.y);
        } else {
            dq = sd_quad(st, q) - q[8];
        }
        d = q[9] > 0.0f ? min(d, dq) : max(d, -dq);
    }
    if (P[P_ROUGH_AMP] > 0.0f) {
        const uint seed = uint(P[P_ROUGH_SEED]);
        const float per = max(P[P_ROUGH_PERIOD], 1e-3f);
        d += P[P_ROUGH_AMP] * (value_noise(st.x / per, seed) + value_noise(st.y / per, seed ^ 0x5bd1e995u) - 1.0f);
    }
    return d;
}

// Full-width 135 maps the source to the whole strip; there is no top/bottom camera mask.
static float exposure_gate_sdf(float2 st, device const float* P) {
    if (P[P_FULL_WIDTH] > 0.5f) return -1e6f; // the output is an exposed crop, without frame-gap padding
    return gate_sdf(st, P);
}

// The gate's shadow on the emulsion from a circular aperture: the lit fraction
// of a disc of radius r cut by a straight edge at signed distance d. Not a
// Gaussian -- a Gaussian has tails a real penumbra does not have.
static float penumbra(float d, float r) {
    if (r <= 1e-6f) return d < 0.0f ? 1.0f : 0.0f;
    const float u = clamp(-d / r, -1.0f, 1.0f);
    return 0.5f + (u * sqrt(max(1.0f - u * u, 0.0f)) + asin(u)) * (1.0f / M_PI_F);
}

static float3 sample_frame(device const float* f, float fx, float fy, uint fw, uint fh) {
    fx = clamp(fx, 0.0f, float(fw) - 1.0f);
    fy = clamp(fy, 0.0f, float(fh) - 1.0f);
    const uint x0 = uint(floor(fx)), y0 = uint(floor(fy));
    const uint x1 = min(x0 + 1u, fw - 1u), y1 = min(y0 + 1u, fh - 1u);
    const float ax = fx - float(x0), ay = fy - float(y0);
    const uint i00 = 3u * (y0 * fw + x0), i01 = 3u * (y0 * fw + x1);
    const uint i10 = 3u * (y1 * fw + x0), i11 = 3u * (y1 * fw + x1);
    const float3 a = float3(f[i00], f[i00 + 1u], f[i00 + 2u]);
    const float3 b = float3(f[i01], f[i01 + 1u], f[i01 + 2u]);
    const float3 c = float3(f[i10], f[i10 + 1u], f[i10 + 2u]);
    const float3 d = float3(f[i11], f[i11 + 1u], f[i11 + 2u]);
    return mix(mix(a, b, ax), mix(c, d, ax), ay);
}

static float perf_sdf(float2 st, device const float* P, thread uint& hole);

// One perforation's seeded light events: rim weight, soft halo and local thickness.
// shortcut: visual light placement, not calibrated propagation through a specific film base.
static float3 perforation_light(float2 st, device const float* P, int nearest, uint seed) {
    const float pitch = P[P_PERF_PITCH], phase = P[P_PERF_PHASE];
    const bool row_top = st.y < 0.5f * P[P_FILM_W];
    const uint row_seed = seed ^ (row_top ? 0xA101u : 0xB202u);
    const uint hole = hash_u(uint(nearest * 2 + (row_top ? 0 : 1)) * 0x9E3779B9u ^ uint(P[P_PERF_SEED]));
    const uint rim_seed = row_seed ^ hole;
    const bool regular = int(P[P_PERF_LEAK_STYLE]) <= 1;
    const float coverage = int(P[P_PERF_LEAK_STYLE]) == 0 ? P[P_PERF_LEAK_COVERAGE] : 0.0f;
    const float shared_angle = 2.0f * M_PI_F * hash_f(0, seed ^ 0x14u);
    const float size = regular ? 0.8f + 0.4f * hash_f(0, rim_seed ^ 0x18u) : 1.0f;
    const bool top = st.y < 0.5f * P[P_FILM_W];
    const float2 half_size = 0.5f * float2(P[P_PERF_W], P[P_PERF_H]);
    const float2 center = float2(phase + float(nearest) * pitch + half_size.x,
                                top ? P[P_PERF_EDGE] + half_size.y : P[P_FILM_W] - P[P_PERF_EDGE] - half_size.y);
    const float2 local = st - center;
    const float u = atan2(local.y / half_size.y, local.x / half_size.x) * (0.5f / M_PI_F) + 0.5f;
    const float spread = P[P_PERF_LEAK_SPREAD];
    float rim_field = 0.0f, halo_field = 0.0f, thickness_sum = 0.0f;
    // Seeded light events, like spool leaks; wrapped distances have no seam around the outline.
    const int count = regular ? 3 : 1 + min(2, int(3.0f * hash_f(0, rim_seed ^ 0x10u)));
    for (int k = 0; k < count; ++k) {
        // Every hole has a main arc in the same direction; small shoulders vary its shape.
        const float position = regular ? shared_angle * (0.5f / M_PI_F) + 0.5f +
            (k == 0 ? 0.0f : (k == 1 ? -0.10f : 0.10f)) : hash_f(k, rim_seed ^ 0x11u);
        const float width = regular ? (k == 0 ? 0.12f : 0.055f) * size * (1.0f + 3.0f * coverage) :
            0.035f + 0.09f * hash_f(k, rim_seed ^ 0x12u);
        const float amplitude = regular ? (k == 0 ? 1.0f : 0.1f + 0.1f * hash_f(k, rim_seed ^ 0x13u)) :
            exp2(-0.7f + 1.4f * hash_f(k, rim_seed ^ 0x13u));
        const float turn = u - position - floor(u - position + 0.5f);
        const float du = regular ? abs(turn) : min(abs(u - position), 1.0f - abs(u - position));
        const float x = du / width;
        const float soft_x = du / (width * (1.3f + 0.7f * spread));
        const float event = amplitude * exp(-0.5f * x * x);
        // Each light event has its own thickness, tapering smoothly along the arc.
        const float shift = 1.2f * (hash_f(k, rim_seed ^ 0x19u) - 0.5f);
        const float taper_x = turn / width + shift;
        const float sigma = 0.007f + 0.058f * pow(hash_f(k, rim_seed ^ 0x18u), 2.0f);
        thickness_sum += event * sigma * (0.55f + 0.45f * exp(-0.5f * taper_x * taper_x));
        rim_field += event;
        halo_field += amplitude * exp(-0.5f * soft_x * soft_x);
    }
    // The scratch envelope's smooth threshold makes lit arcs fade to genuinely unlit stretches.
    const float arc = smoothstep(0.15f, 0.85f, rim_field);
    const float envelope = mix(arc, 0.35f + 0.65f * arc, coverage);
    const float q_radius = P[P_PERF_R];
    const float2 q = abs(local) - (half_size - q_radius);
    const float2 outside = max(q, 0.0f);
    const float2 normal = length(outside) > 1e-5f ? normalize(outside) * sign(local)
        : (q.x > q.y ? float2(sign(local.x), 0.0f) : float2(0.0f, sign(local.y)));
    const float angle = shared_angle + (regular ? 0.0f :
                        0.12f * (hash_f(0, rim_seed ^ 0x15u) - 0.5f));
    // As in the scan-wall model, a side facing the light receives more than the opposite side.
    const float facing = 0.15f + 0.85f * max(0.0f, dot(normal, float2(cos(angle), sin(angle))));
    // Broader coverage lifts the back-facing arc without erasing the shared light direction.
    const float direction = mix(facing, 0.65f + 0.35f * facing, coverage);
    const float presence = hash_f(0, rim_seed ^ 0x16u) < 0.75f ? 1.0f : 0.05f;
    const float trend = 0.4f + 0.6f * value_noise(st.x / (3.0f * pitch), seed);
    const float hole_weight = regular ? 0.3f * (0.8f + 0.4f * hash_f(0, rim_seed ^ 0x17u)) :
        presence * (0.35f + 0.65f * hash_f(0, rim_seed ^ 0x17u)) * trend;
    const float rim_weight = hole_weight * direction * envelope * (1.0f + 0.75f * envelope);
    // A three-neighbour sum merges adjacent halos without nearest-cell amplitude jumps.
    const float radius = length(outside) + min(max(q.x, q.y), 0.0f) - q_radius;
    const float sigma = (0.06f + 0.54f * spread * spread) * (int(P[P_PERF_LEAK_STYLE]) == 2 ? 2.2f : size);
    const float x = max(radius, 0.0f) / sigma;
    const float halo = 0.8f * exp(-0.5f * x * x) + 0.2f * exp(-0.5f * x * x / 9.0f);
    const float halo_weight = hole_weight * (0.25f + 0.75f * direction) *
                              (0.04f + 0.96f * (1.0f - exp(-halo_field)));
    const float support = 1.0f - smoothstep(pitch, 1.4f * pitch, abs(local.x));
    return float3(rim_weight, halo_weight * halo * support,
                  (regular ? 0.035f * size : thickness_sum / max(rim_field, 1e-8f)) *
                  (0.45f + 0.55f * direction));
}

// Keep the real hole SDF for the thin rim; soft light merges from neighbouring holes.
static float perforation_leak(float2 st, device const float* P) {
    if (P[P_PERF_LEAK] <= 0.0f || P[P_PERFORATED] < 0.5f) return 0.0f;
    uint hole;
    const float d = perf_sdf(st, P, hole);
    if (d < 0.0f) return 0.0f;
    const float pitch = P[P_PERF_PITCH], phase = P[P_PERF_PHASE];
    const int nearest = int(floor((st.x - phase - 0.5f * P[P_PERF_W]) / pitch + 0.5f));
    const uint seed = uint(P[P_PERF_LEAK_SEED]) ^ 0x1EAFu;
    const float spread = P[P_PERF_LEAK_SPREAD];
    const int style = int(P[P_PERF_LEAK_STYLE]);
    const float strength = P[P_PERF_LEAK];
    // Other appearances reuse the edge fog's smooth noise and spool leaks' exposure profiles.
    if (style == 3 || style == 5) {
        const bool top = st.y < 0.5f * P[P_FILM_W];
        const float edge = max(0.0f, min(st.y, P[P_FILM_W] - st.y));
        const uint side_seed = seed ^ (top ? 0x301u : 0x502u);
        const float slow = value_noise(st.x / (3.0f * pitch), side_seed);
        if (style == 3) {
            const float center = P[P_PERF_EDGE] + 0.5f * P[P_PERF_H] + 0.7f * (slow - 0.5f);
            const float sigma = (0.18f + 1.5f * spread * spread) * (0.65f + 0.7f * slow);
            const float x = (edge - center) / sigma;
            return 18.0f * strength * (0.45f + 0.55f * slow) *
                   (0.8f * exp(-0.5f * x*x) + 0.2f * exp(-0.5f * x*x / 9.0f));
        }
        // A wandering exposure front merges the hole lobes into an irregular edge burn.
        const float tongues = value_noise((st.x - 0.65f * edge) / (0.28f * pitch), side_seed ^ 0x71u);
        const float front = 0.25f + (1.0f + 7.0f * spread) * slow * slow +
                            (0.5f + 6.0f * spread) * pow(tongues, 3.0f);
        const float x = max(0.0f, edge - front) / (0.2f + 0.75f * spread);
        const bool primary = hash_f(0, seed ^ 0x59u) < 0.5f;
        const float side_weight = top == primary ? 1.0f : 0.3f;
        return 48.0f * strength * (0.35f + 0.65f * strength) * side_weight *
               (0.15f + 0.85f * slow * slow) *
               (exp(-0.5f*x*x) + 0.03f * exp(-edge / (1.0f + 3.0f*spread)));
    }
    if (style == 4) {
        float field = 0.0f;
        const int count = 1 + int(hash_f(0, seed ^ 0x40u) > 0.65f);
        for (int k = 0; k < count; ++k) {
            const float center = P[P_GS0] + (0.05f + 0.9f * hash_f(k, seed ^ 0x41u)) * P[P_GW];
            const float drift = 0.35f * (value_noise(st.y / 6.0f, seed ^ uint(k)) - 0.5f);
            const float sigma = 0.025f + 0.45f * spread * spread;
            const float x = (st.x - center - drift) / sqrt(sigma*sigma + 0.0961f*P[P_PX]*P[P_PX]);
            const float envelope = 0.4f + 0.6f * value_noise(st.y / 9.0f, seed ^ 0x42u ^ uint(k));
            field += envelope * (exp(-0.5f*x*x) + 0.12f*exp(-0.5f*x*x/16.0f));
        }
        return 24.0f * strength * field;
    }
    if (style == 1 || style == 2) {
        const float reach = 18.0f * (0.06f + 0.54f * spread * spread) * (style == 2 ? 2.2f : 1.2f);
        if (d > reach) return 0.0f;
        const float halo = perforation_light(st, P, nearest, seed).y +
                           perforation_light(st, P, nearest - 1, seed).y +
                           perforation_light(st, P, nearest + 1, seed).y;
        return (style == 2 ? 18.0f : 20.0f) * strength * halo;
    }
    const float softness = P[P_PERF_LEAK_SOFTNESS];
    const float max_soft_sigma = 0.384f * softness * softness;
    const float max_sigma = sqrt(0.065f * 0.065f + max_soft_sigma * max_soft_sigma + 0.0961f * P[P_PX] * P[P_PX]);
    const float rim_reach = 0.0325f + 6.0f * max_sigma;
    const float reach = spread > 0.0f ? max(rim_reach, 21.6f * (0.06f + 0.54f * spread * spread)) : rim_reach;
    if (d > reach) return 0.0f; // Skip negligible Gaussian tails before generating light events.
    const float3 light = perforation_light(st, P, nearest, seed);
    const float sigma = max(light.z, 0.003f);
    const float rim_sigma = sqrt(sigma * sigma + 0.0961f * P[P_PX] * P[P_PX]);
    const float rim_x = (d - 0.5f * sigma) / rim_sigma;
    // Keep the exposure peak: normalising before nonlinear development dims small previews.
    float amount = 6.00f * P[P_PERF_LEAK] * light.x * exp(-0.5f * rim_x * rim_x);
    if (softness > 0.0f) {
        const float soft_sigma = 0.32f * softness * softness * (sigma / 0.035f);
        const float width = sqrt(rim_sigma * rim_sigma + soft_sigma * soft_sigma);
        const float x = (d - 0.5f * sigma) / width;
        // Retain a bright core; peak-preserving soft exposure avoids a dim blur after development.
        amount = (1.0f - 0.55f * softness) * amount +
                 6.00f * P[P_PERF_LEAK] * light.x * 0.85f * softness * exp(-0.5f * x * x);
    }
    if (spread <= 0.0f) return amount;
    const float halo = light.y + perforation_light(st, P, nearest - 1, seed).y +
                                perforation_light(st, P, nearest + 1, seed).y;
    return amount + 1.40f * P[P_PERF_LEAK] * smoothstep(0.0f, 1.0f, spread) * halo;
}

// ① + ② + the fog of ③: raw exposure on the film canvas.
kernel void spk_overscan_canvas(device const float* frame [[buffer(0)]],
                                device const float* P [[buffer(1)]],
                                device const uint* n [[buffer(2)]],
                                device float* out [[buffer(3)]],
                                uint3 tid [[thread_position_in_grid]]) {
    const uint i = tid.x;
    if (i >= n[0]) return;
    const uint cw = uint(P[P_CW]);
    const float2 st = canvas_to_film(float(i % cw), float(i / cw), P);
    const float W = P[P_FILM_W];
    float3 e = float3(0.0f);
    if (P[P_CARRIER] > 1.5f || (st.y >= 0.0f && st.y <= W)) {
        // A pair: past the middle of the gap, the second exposure of the same gate.
        float2 sg = st;
        float shift = 0.0f;
        if (P[P_PAIR_ADV] > 0.0f && st.x > P[P_GS0] + 0.5f * (P[P_GW] + P[P_PAIR_ADV])) { shift = P[P_PAIR_ADV]; sg.x -= shift; }
        const float cov = penumbra(exposure_gate_sdf(sg, P), P[P_PENUMBRA]);
        if (cov > 0.0f) {
            const float px = P[P_PX];
            float fx, fy;
            if (P[P_VERTICAL] < 0.5f) { fx = (st.x - P[P_GS0]) / px - 0.5f; fy = (st.y - P[P_GT0]) / px - 0.5f; }
            else                      { fx = (P[P_GT0] + P[P_GH] - st.y) / px - 0.5f; fy = (st.x - P[P_GS0]) / px - 0.5f; }
            e += cov * sample_frame(frame, fx, fy, uint(P[P_FW]), uint(P[P_FH]));
        }
        // Gate flare: the gate's bevelled edge reflects scene light onto the
        // film just outside the frame -- a thin line that follows the scene's
        // brightness along the edge (bright sky, bright line).
        const float d_gate = exposure_gate_sdf(sg, P);
        if (P[P_FLARE_AMP] > 0.0f && d_gate > -0.02f && d_gate < 6.0f * P[P_FLARE_WIDTH]) {
            const float px = P[P_PX];
            const float in_s = clamp(sg.x, P[P_GS0] + 0.12f, P[P_GS0] + P[P_GW] - 0.12f) + shift;
            const float in_t = clamp(st.y, P[P_GT0] + 0.12f, P[P_GT0] + P[P_GH] - 0.12f);
            float fx, fy;
            if (P[P_VERTICAL] < 0.5f) { fx = (in_s - P[P_GS0]) / px - 0.5f; fy = (in_t - P[P_GT0]) / px - 0.5f; }
            else                      { fx = (P[P_GT0] + P[P_GH] - in_t) / px - 0.5f; fy = (in_s - P[P_GS0]) / px - 0.5f; }
            const float3 edge_light = sample_frame(frame, fx, fy, uint(P[P_FW]), uint(P[P_FH]));
            const float prof = exp(-max(d_gate, 0.0f) / P[P_FLARE_WIDTH]);
            e += P[P_FLARE_AMP] * prof * edge_light;
        }
        // Edge fog: warm exposure decaying inward from each long edge, slowly
        // varying along the length; and the spool leaks, seeded blobs on an edge.
        // (clamped: the canvas reaches past the film, where there is none to fog)
        const float d_top = max(st.y, 0.0f), d_bot = max(W - st.y, 0.0f);
        const uint seed = uint(P[P_FOG_SEED]);
        const float period = max(P[P_FOG_PERIOD], 0.1f);
        const float nz = 0.6f * value_noise(st.x / period, seed) +
                         0.4f * value_noise(st.x / (0.37f * period), seed ^ 0xA5A5A5A5u);
        float fog = P[P_FOG_AMP] * exp(-min(d_top, d_bot) / max(P[P_FOG_WIDTH], 1e-3f)) * (0.3f + 0.7f * nz);
        const uint nl = uint(P[P_N_LEAKS]);
        for (uint k = 0; k < nl; ++k) {
            const uint b = P_LEAKS + 5u * k;
            const float dd = P[b + 1u] < 0.5f ? d_top : d_bot;
            const float ds = st.x - P[b];
            fog += P[b + 2u] * exp(-ds * ds / (2.0f * P[b + 3u] * P[b + 3u])) * exp(-dd / P[b + 4u]);
        }
        const float appearance = perforation_leak(st, P);
        // Shadow is attenuation of scene exposure, never negative light or a changed film profile.
        if (int(P[P_PERF_LEAK_STYLE]) == 2) e *= exp2(-appearance);
        else fog += appearance;
        e += fog * float3(P[P_FOG_R], P[P_FOG_G], P[P_FOG_B]);
    }
    out[3u * i] = e.x;
    out[3u * i + 1u] = e.y;
    out[3u * i + 2u] = e.z;
}

// ③: a rasterised imprint (coverage, canvas-aligned) added as exposure.
// M = {x0, y0, mw, mh, cw, ch, e_r, e_g, e_b}.
kernel void spk_overscan_add_mask(device const float* mask [[buffer(0)]],
                                  device const float* M [[buffer(1)]],
                                  device const uint* n [[buffer(2)]],
                                  device float* canvas [[buffer(3)]],
                                  uint3 tid [[thread_position_in_grid]]) {
    const uint i = tid.x;
    if (i >= n[0]) return;
    const uint mw = uint(M[2]);
    const int x = int(M[0]) + int(i % mw), y = int(M[1]) + int(i / mw);
    if (x < 0 || y < 0 || x >= int(M[4]) || y >= int(M[5])) return;
    const float c = mask[i];
    if (c <= 0.0f) return;
    const uint o = 3u * (uint(y) * uint(M[4]) + uint(x));
    canvas[o] += c * M[6];
    canvas[o + 1u] += c * M[7];
    canvas[o + 2u] += c * M[8];
}

// The nearest perforation's signed distance (mm, negative inside). KS holes
// are punched to tight tolerances, not drawn: each hole sits a few microns
// off the ideal pitch and row, is a few microns off size, its corners are not
// all the same, and the punched edge is not a perfect line. `hole` returns the
// hole's own hash (row and index) for the scan's dust.
static float perf_sdf(float2 st, device const float* P, thread uint& hole) {
    const float W = P[P_FILM_W];
    const float pitch = P[P_PERF_PITCH], ph = P[P_PERF_PHASE];
    // The nearest hole: the cell is centred on it. (It began at the hole's
    // leading edge, so just outside that edge the distance was the previous
    // hole's -- 2.8 mm -- and that side had no shoulder, no flare, and a hard
    // cut where the other three sides were soft.)
    const float k = floor((st.x - ph - 0.5f * P[P_PERF_W]) / pitch + 0.5f);
    const bool top = st.y < 0.5f * W;
    const uint seed = uint(P[P_PERF_SEED]);
    hole = hash_u(uint(int(k) * 2 + (top ? 0 : 1)) * 0x9E3779B9u ^ seed);
    const float j0 = hash_f(int(hole & 0xFFFFu), seed ^ 0x1u) - 0.5f, j1 = hash_f(int(hole & 0xFFFFu), seed ^ 0x2u) - 0.5f;
    const float j2 = hash_f(int(hole & 0xFFFFu), seed ^ 0x3u) - 0.5f, j3 = hash_f(int(hole & 0xFFFFu), seed ^ 0x4u) - 0.5f;
    const float j4 = hash_f(int(hole & 0xFFFFu), seed ^ 0x5u) - 0.5f;
    const float wa = P[P_PERF_W] + 0.016f * j2, hc = P[P_PERF_H] + 0.016f * j3;
    const float r = clamp(P[P_PERF_R] + 0.08f * j4, 0.3f, 0.6f);
    const float sc = ph + k * pitch + 0.5f * P[P_PERF_W] + 0.020f * j0;
    const float tc = (top ? P[P_PERF_EDGE] + 0.5f * P[P_PERF_H] : W - P[P_PERF_EDGE] - 0.5f * P[P_PERF_H]) + 0.024f * j1;
    const float2 q = fabs(float2(st.x - sc, st.y - tc)) - float2(0.5f * wa - r, 0.5f * hc - r);
    float d = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
    // the punched edge: each hole's own roughness, 1-4 microns
    const float rough = 0.001f + 0.003f * hash_f(int(hole & 0xFFFFu), seed ^ 0x6u);
    d += rough * (value_noise(st.x / 0.05f, seed ^ 0x77u) + value_noise(st.y / 0.05f, seed ^ 0x99u) - 1.0f) * 2.0f;
    // a burr or a chip on about one hole in four: a 20-50 micron bump on the
    // edge, extending the hole outward at its left short-edge centre
    if (hash_f(int(hole & 0xFFFFu), seed ^ 0x7u) < 0.25f) {
        // The owner fixes the film leader at s-: pull contact is the left short-edge centre.
        const float2 pb = float2(sc - 0.5f * wa, tc);
        const float sig = 0.020f + 0.030f * hash_f(int(hole & 0xFFFFu), seed ^ 0x9u);
        // Negative signed-distance displacement expands the hole into the film, towards s-.
        const float amp = -(0.006f + 0.010f * hash_f(int(hole & 0xFFFFu), seed ^ 0xBu));
        d += amp * exp(-dot(st - pb, st - pb) / (sig * sig));
    }
    // Extend the existing Gaussian burr/chip at the sprocket's pull-contact point.
    // shortcut: dimensions are visual estimates; calibrate against measured negatives for physical claims.
    const float wear = P[P_SCRATCH_EDGE];
    const float2 contact = float2(sc - 0.5f * wa, tc);
    const float2 qwear = st - contact;
    if (wear > 0.0f && abs(qwear.x) < 0.20f && abs(qwear.y) < 0.18f) {
        const uint ws = uint(P[P_SCRATCH_SEED]) ^ hole ^ 0xD271u;
        if (hash_f(0, ws ^ 0x7u) < 0.75f) {
            const float2 sig = float2(0.025f + 0.020f * hash_f(0, ws ^ 0x8u),
                                      0.035f + 0.040f * hash_f(0, ws ^ 0x9u));
            const float amp = -wear * (0.015f + 0.035f * hash_f(0, ws ^ 0xBu));
            const float2 z = qwear / sig;
            const float end = 1.0f - smoothstep(0.10f, 0.18f, abs(qwear.y));
            d += amp * exp(-dot(z,z)) * end *
                 (0.75f + 0.25f * value_noise(qwear.y / 0.012f, ws ^ 0x77u));
        }
    }
    return d;
}

// How soft a perforation's edge is in a scan, as the sigma of a Gaussian edge
// in film millimetres. Measured on the owner's strips in reference_film/135
// (2026-10-04): across 61-80 hole edges on each of eight scans the edge takes
// 44-71 microns to go from 10 % to 90 % of the hole's light (median 59), at
// 21-32 px/mm -- where this engine's one-pixel edge took 34, and at the full
// tier of a 45 MP frame 5: a knife. Less the scans' own pixels that is a
// sigma of 17 microns: the scanner's lens, and a cut wall 0.13 mm deep that
// is not all in focus at once.
constant float kHoleEdgeSigma = 0.017f;

// erf, Abramowitz and Stegun 7.1.26 (|error| < 1.5e-7).
static float erf_as(float x) {
    const float t = 1.0f / (1.0f + 0.3275911f * fabs(x));
    const float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f) * t * exp(-x * x);
    return x < 0.0f ? -y : y;
}

// Where there is film: 1 on the strip, 0 in the perforations and past its edges.
static float film_alpha(float2 st, device const float* P) {
    const float W = P[P_FILM_W], px = P[P_PX];
    // (with no carrier the canvas is the film's width, and the film runs on
    // under the corners a tilted scan carries past its edge)
    float a = P[P_CARRIER] > 1.5f ? 1.0f : clamp(min(st.y, W - st.y) / px + 0.5f, 0.0f, 1.0f);
    if (P[P_PERFORATED] > 0.5f) {
        uint hole;
        // The hole's edge as the scan resolves it: the measured softness, and
        // the pixel's own width (a one-pixel ramp is a sigma of 0.31 px), so a
        // small render is no softer than it was and a large one no harder
        // than film.
        const float sigma = sqrt(kHoleEdgeSigma * kHoleEdgeSigma + 0.0961f * px * px);
        a *= 0.5f + 0.5f * erf_as(perf_sdf(st, P, hole) / (1.41421356f * sigma));
    }
    return a;
}

// Line mask/envelope adapted from FilmDamageSimulator's line_scratch;
// see third_party/film_damage_simulator. Geometry uses this engine's film mm.
static float scratch_line(float2 st, float2 center, float len, float width, float angle, uint seed) {
    const float2 axis = float2(cos(angle), sin(angle));
    const float2 q = st - center;
    const float along = dot(q, axis);
    if (abs(along) >= 0.5f * len) return 0.0f;
    const float across = dot(q, float2(-axis.y, axis.x));
    const float fade = smoothstep(0.0f, min(0.15f, len * 0.15f), 0.5f * len - abs(along));
    const float envelope = smoothstep(0.25f, 0.65f, value_noise((along + len) / max(0.12f, len * 0.09f), seed));
    return exp(-0.5f * across * across / (width * width)) * fade * envelope;
}

static float scratch_mask(float2 st, device const float* P) {
    const uint seed = uint(P[P_SCRATCH_SEED]);
    float2 other = st; other.x -= P[P_PAIR_ADV];
    const float gate = min(gate_sdf(st, P), P[P_PAIR_ADV] > 0.0f ? gate_sdf(other, P) : 1e6f);
    const bool full_width = P[P_FULL_WIDTH] > 0.5f;
    const float rebate_depth = P[P_PERF_EDGE] + P[P_PERF_H] + 0.35f;
    if ((!full_width && gate < 0.10f) || (full_width && min(st.y, P[P_FILM_W] - st.y) > rebate_depth) ||
        st.y <= 0.0f || st.y >= P[P_FILM_W]) return 0.0f;
    uint hole;
    if (perf_sdf(st, P, hole) < 0.07f) return 0.0f;
    const float along_span = P[P_GW] + P[P_PAIR_ADV];
    float mask = 0.0f;
    // Three neighboring tracks share a band, while their starts/ends differ.
    for (int i = 0; i < 12; ++i) {
        if (hash_f(i, seed ^ 0x101u) >= P[P_SCRATCH_LONG]) continue;
        const int group = i / 3;
        const bool top = hash_f(group, seed ^ 0x202u) < 0.5f;
        const float offset = 0.28f + 0.65f * hash_f(group, seed ^ 0x303u) + float(i % 3) * 0.07f;
        const float band = P[P_PERF_EDGE] + P[P_PERF_H] + 0.25f - offset;
        const float t = full_width ? (top ? band : P[P_FILM_W] - band)
                                  : (top ? P[P_GT0] - offset : P[P_GT0] + P[P_GH] + offset);
        const float ss = P[P_GS0] + along_span * hash_f(i, seed ^ 0x404u);
        const float len = along_span * (0.3f + 0.8f * hash_f(i, seed ^ 0x505u));
        const float w = sqrt(pow(0.009f + 0.0105f * hash_f(i, seed ^ 0x606u), 2.0f) + 0.0961f * P[P_PX] * P[P_PX]);
        mask = max(mask, scratch_line(st, float2(ss,t), len, w, 0.0f, seed ^ uint(i + 1)));
    }
    return mask;
}

// Approved real grayscale masks; tile order is 64 dust, 32 short, 16 long fibers.
static float dust_sample(float2 q, uint shape, device const uchar* atlas) {
    if (any(abs(q) > 1.5f)) return 0.0f;
    const float2 p = (q + 1.5f) * (64.0f / 3.0f) - 0.5f;
    const int2 i = int2(floor(p)); const float2 f = fract(p);
    const int2 a = clamp(i, int2(0), int2(63));
    const int2 b = clamp(i + 1, int2(0), int2(63));
    const uint offset = shape * 4096u;
    return mix(mix(float(atlas[offset + a.y*64+a.x]), float(atlas[offset + a.y*64+b.x]), f.x),
               mix(float(atlas[offset + b.y*64+a.x]), float(atlas[offset + b.y*64+b.x]), f.x), f.y) / 255.0f;
}

static float dust_mask(float2 st, device const float* P, device const uchar* atlas) {
    if (P[P_DUST_FULL_FRAME] < 0.5f) {
        if (P[P_FULL_WIDTH] > 0.5f) {
            if (min(st.y, P[P_FILM_W] - st.y) > P[P_PERF_EDGE] + P[P_PERF_H] + 0.35f) return 0.0f;
        } else {
            float2 other = st; other.x -= P[P_PAIR_ADV];
            if (min(gate_sdf(st, P), P[P_PAIR_ADV] > 0.0f ? gate_sdf(other, P) : 1e6f) < 0.10f) return 0.0f;
        }
    }
    const uint seed = uint(P[P_SCRATCH_SEED]) ^ 0xD057u;
    const int2 cell = int2(floor(st));
    float mask = 0.0f;
    // Physical 1 mm cells keep positions stable across preview/export sizes.
    for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
        const int2 id = cell + int2(x, y);
        const uint h = hash_u(uint(id.x) * 0x9E3779B9u ^ uint(id.y) * 0x85EBCA6Bu ^ seed);
        const float cluster = 0.25f + 0.75f * value_noise(float(id.x) / 5.0f, seed ^ uint(id.y / 5));
        if (hash_f(0, h) >= 0.22f * P[P_DUST_AMOUNT] * cluster) continue;
        const float2 center = float2(id) + float2(hash_f(1, h), hash_f(2, h));
        const bool fiber = hash_f(7, h) < P[P_DUST_FIBER_RATIO];
        const bool long_fiber = fiber && hash_f(9, h) < 0.2f;
        const uint shape = !fiber ? hash_u(h ^ 0x51u) % 64u
                         : long_fiber ? 96u + hash_u(h ^ 0x52u) % 16u
                                      : 64u + hash_u(h ^ 0x53u) % 32u;
        const float u = hash_f(3, h);
        const float base_radius = !fiber ? 0.014f + 0.10f * pow(u, 3.0f)
                                : long_fiber ? 0.20f + 0.35f * u : 0.05f + 0.13f * u;
        const float radius = base_radius * (0.4f + 1.6f * P[P_DUST_SIZE]);
        const float2 v = st - center;
        if (length(v) > 2.122f * radius + P[P_PX]) continue;
        const float angle = 6.2831853f * hash_f(4, h);
        const float2 q = float2(cos(angle) * v.x + sin(angle) * v.y, -sin(angle) * v.x + cos(angle) * v.y) / radius;
        // Pixel-footprint integration keeps thin fibers from flickering at preview scale.
        const float aa = 0.25f * P[P_PX] / radius;
        const float cover = 0.25f * (dust_sample(q + float2(aa, aa), shape, atlas)
                                  + dust_sample(q + float2(-aa, aa), shape, atlas)
                                  + dust_sample(q + float2(aa, -aa), shape, atlas)
                                  + dust_sample(q - float2(aa, aa), shape, atlas));
        mask = max(mask, cover * (0.5f + 0.45f * hash_f(5, h)));
    }
    return mask;
}

// ④: where there is no film -- the perforations, and outside the strip --
// the negative's density goes to the cmy that cancels this film's base, so
// the enlarger or the scanner sees clear light.
kernel void spk_overscan_film_present(device const float* cmy [[buffer(0)]],
                                      device const float* P [[buffer(1)]],
                                      device const uint* n [[buffer(2)]],
                                      device float* out [[buffer(3)]],
                                      device const uchar* dust_atlas [[buffer(4)]],
                                      uint3 tid [[thread_position_in_grid]]) {
    const uint i = tid.x;
    if (i >= n[0]) return;
    const uint cw = uint(P[P_CW]);
    const float a = film_alpha(canvas_to_film(float(i % cw), float(i / cw), P), P);
    float3 c = float3(cmy[3u * i], cmy[3u * i + 1u], cmy[3u * i + 2u]);
    if (P[P_SCRATCH_STRENGTH] > 0.0f && P[P_PERFORATED] > 0.5f && a > 0.999f) {
        // shortcut: neutral density approximates scratch obstruction; calibrate against measured wear before claiming a physical model.
        const float wear = scratch_mask(canvas_to_film(float(i % cw), float(i / cw), P), P);
        c += 0.90f * P[P_SCRATCH_STRENGTH] * wear;
    }
    if (P[P_DUST_AMOUNT] > 0.0f && P[P_DUST_STRENGTH] > 0.0f && a > 0.999f) {
        const float cover = dust_mask(canvas_to_film(float(i % cw), float(i / cw), P), P, dust_atlas);
        // shortcut: neutral CMY attenuation reuses the negative/print path; not calibrated scanner spectral dust.
        c += -log10(max(1.0f - 0.98f * P[P_DUST_STRENGTH] * cover, 0.02f));
    }
    const float3 hole = float3(P[P_HOLE_C], P[P_HOLE_M], P[P_HOLE_Y]);
    const float3 r3 = a * c + (1.0f - a) * hole;
    out[3u * i] = r3.x;
    out[3u * i + 1u] = r3.y;
    out[3u * i + 2u] = r3.z;
}

// ⑤ (RFC-032 §30.5, §31): the scan's view of the holes. Inside a hole: the
// picture's own white (holes = white) or a black backing (holes = black).
// Around it, the cut wall: the punch shears the top of the base clean and
// fractures the rest at a slant, and a scan seen a little off-axis sees the
// wall on the side away from its axis. Seen from above, that is a band where
// the base thins from full to nothing -- brown through the orange mask on a
// white hole, a dim warm rim around a black one. Each hole's slant is its
// own; the viewing tilt and parallax are the scan's. Linear output RGB,
// after the scanner's blur and sharpening (the edge's softness is `film_alpha`'s).
kernel void spk_overscan_light(device const float* rgb [[buffer(0)]],
                               device const float* P [[buffer(1)]],
                               device const uint* n [[buffer(2)]],
                               device float* out [[buffer(3)]],
                               uint3 tid [[thread_position_in_grid]]) {
    const uint i = tid.x;
    if (i >= n[0]) return;
    const uint cw = uint(P[P_CW]);
    const float2 st = canvas_to_film(float(i % cw), float(i / cw), P);
    const float a = film_alpha(st, P);
    const bool white = P[P_HOLES_LIGHT] > 0.5f;
    float3 light = float3(P[P_LIGHT_R], P[P_LIGHT_G], P[P_LIGHT_B]);
    float3 c = float3(rgb[3u * i], rgb[3u * i + 1u], rgb[3u * i + 2u]);
    if (P[P_PERFORATED] > 0.5f) {
        uint hole;
        const float d = perf_sdf(st, P, hole);
        // The cut is not a perfectly vertical knife: the base is a little rounded
        // where the punch went through, and a scan sees that as a soft shoulder
        // just outside the hole -- measured on the owner's reference at 1-2 px
        // of 0.03 mm, lifting the black by ~1-5 % (linear), a faint olive-warm,
        // and not the same on every side or every hole. Inside, a few edges
        // carry a faint cream line. Subtle on purpose (RFC-032 §31.5).
        const uint seed = uint(P[P_PERF_SEED]);
        const int h = int(hole & 0xFFFFu);
        if (d > -0.03f && d < 0.15f) {
            const float e = 0.003f;
            uint hh;
            const float2 g = float2(perf_sdf(st + float2(e, 0), P, hh) - perf_sdf(st - float2(e, 0), P, hh),
                                    perf_sdf(st + float2(0, e), P, hh) - perf_sdf(st - float2(0, e), P, hh));
            const float2 nrm = g / max(length(g), 1e-6f);
            const float2 vdir = float2(P[P_WALL_VS], P[P_WALL_VT]);
            // how much this stretch of this hole's outline shows: per hole, patchy
            // along the outline, a little more on the side the scan leans to
            const float per_hole = 0.4f + 0.6f * hash_f(h, seed ^ 0xE1u);
            const float patch = smoothstep(0.25f, 0.75f, 0.5f * (value_noise(st.x / 0.35f, seed ^ uint(h)) +
                                                                 value_noise(st.y / 0.35f, seed ^ uint(h) ^ 0x3Cu)));
            const float side = 0.6f + 0.8f * max(0.0f, dot(nrm, vdir));
            const float k = per_hole * patch * side;
            // the base's tint, mostly washed out
            const float3 tint = mix(float3(1.0f), float3(P[P_BASE_R], P[P_BASE_G], P[P_BASE_B]), 0.3f);
            const float lam = P[P_WALL_SCATTER];
            if (P[P_SCRATCH_EDGE] > 0.0f && abs(d) < 0.06f) {
                const uint ws = uint(P[P_SCRATCH_SEED]) ^ hole ^ 0xC010u;
                const float patch = smoothstep(0.25f, 0.75f, 0.5f * (value_noise(st.x / 0.20f, ws) +
                                                                             value_noise(st.y / 0.20f, ws ^ 0x33u)));
                // Existing spectral base transmittance: a narrow absorptive colour mark, not added light.
                const float amount = 0.65f * P[P_SCRATCH_EDGE] * patch * exp(-abs(d) / 0.018f);
                const float3 filter = mix(float3(1.0f), clamp(float3(P[P_BASE_R], P[P_BASE_G], P[P_BASE_B]), 0.01f, 1.0f), amount);
                // Output-space conversion can leave negative channels; absorption must not lift them.
                if (d >= 0.0f) c -= max(c, 0.0f) * (1.0f - filter);
                else light *= filter;
            }
            if (d >= 0.0f) {
                const float lift = P[P_WALL_GLOW] * k * exp(-d / lam);
                c += lift * tint * (white ? light : float3(1.0f));
            } else if (white && hash_f(h, seed ^ 0xE4u) < 0.5f) {
                light *= 1.0f - 0.12f * k * (1.0f - tint) * exp(d / 0.012f);
            }
        }
        if (white && d >= 0.0f) {
            // a faint flare from the hole's light into the film, on some holes
            const float fl = 0.012f * hash_f(h, seed ^ 0xC4u) * step(0.5f, hash_f(h, seed ^ 0xC5u));
            c += fl * exp(-d / 0.025f) * light;
        }
    }
    // Past the film's long edges is the scan's carrier, not a hole: black, or
    // the scan's open light.
    const float W = P[P_FILM_W], px = P[P_PX];
    const float on_strip = P[P_CARRIER] > 1.5f ? 1.0f : clamp(min(st.y, W - st.y) / px + 0.5f, 0.0f, 1.0f);
    const float in_film = on_strip > 0.0f ? a / on_strip : 0.0f;
    const float3 r3 = mix(float3(P[P_CARRIER]), mix(light, c, in_film), on_strip);
    out[3u * i] = r3.x;
    out[3u * i + 1u] = r3.y;
    out[3u * i + 2u] = r3.z;
}

// Local user textures contain processed, assumed-sRGB RGB16 values, not spectral radiance.
static float3 wide_sample(device const ushort* texture, uint w, uint h, float2 uv) {
    const float2 p = clamp(uv, 0.0f, 1.0f) * float2(w-1u, h-1u);
    const uint2 a = uint2(floor(p)), b = min(a+1u, uint2(w-1u,h-1u));
    const float2 f = fract(p);
    const uint i00=3u*(a.y*w+a.x), i10=3u*(a.y*w+b.x), i01=3u*(b.y*w+a.x), i11=3u*(b.y*w+b.x);
    const float3 c00=float3(texture[i00],texture[i00+1],texture[i00+2]) / 65535.0f;
    const float3 c10=float3(texture[i10],texture[i10+1],texture[i10+2]) / 65535.0f;
    const float3 c01=float3(texture[i01],texture[i01+1],texture[i01+2]) / 65535.0f;
    const float3 c11=float3(texture[i11],texture[i11+1],texture[i11+2]) / 65535.0f;
    return mix(mix(c00,c10,f.x),mix(c01,c11,f.x),f.y);
}
static float3 wide_matrix(float3 c, device const float* m) {
    return float3(dot(c,float3(m[0],m[1],m[2])),dot(c,float3(m[3],m[4],m[5])),dot(c,float3(m[6],m[7],m[8])));
}
static float3 wide_encode(float3 c) {
    return select(12.92f*c,1.055f*pow(max(c,0.0f),float3(1.0f/2.4f))-0.055f,c>0.0031308f);
}
static float3 wide_decode(float3 c) {
    return select(c/12.92f,pow(max((c+0.055f)/1.055f,0.0f),float3(2.4f)),c>0.04045f);
}
kernel void spk_wide_light_leak(device const float* rgb [[buffer(0)]],
                                device const float* P [[buffer(1)]],
                                device const ushort* texture [[buffer(2)]],
                                device const float* geometry [[buffer(3)]],
                                device float* out [[buffer(4)]],
                                uint3 tid [[thread_position_in_grid]]) {
    const uint i=tid.x, w=uint(P[0]), h=uint(P[1]);
    if (i>=w*h) return;
    const float3 original=float3(rgb[3*i],rgb[3*i+1],rgb[3*i+2]);
    const float2 pixel=float2(float(i%w)+0.5f,float(i/w)+0.5f);
    float film=1.0f;
    if (P[12]>0.5f) film=film_alpha(canvas_to_film(pixel.x-0.5f,pixel.y-0.5f,geometry),geometry);
    const bool aging=P[35]>0.5f;
    float2 q=pixel, span=float2(w,h);
    if (aging) {
        const uint mirror=uint(P[34]);
        if (mirror&1u) q.x=span.x-q.x;
        if (mirror&2u) q.y=span.y-q.y;
        // Match portrait orientation first, then apply saved clockwise quarter-turns.
        const int turns=(h>w ? 1 : 0)+int(P[36]);
        for (int turn=0;turn<turns;++turn) { q=float2(q.y,span.x-q.x); span=span.yx; }
    }
    if (P[31]<0.5f) {
        const int direction=int(P[7]);
        if (direction==1) q.x=span.x-q.x;
        else if (direction==2) { q=float2(pixel.y,float(w)-pixel.x); span=float2(h,w); }
        else if (direction==3) { q=float2(float(h)-pixel.y,pixel.x); span=float2(h,w); }
    }
    const float2 ts=float2(P[2],P[3]);
    const uint seed=uint(P[9]);
    const float jitter=0.03f*(hash_f(0,seed^0xA31u)-0.5f);
    const float vertical=0.03f*(hash_f(0,seed^0xA32u)-0.5f);
    float2 uv;
    if (P[31]>0.5f) {
        const float aspect=span.x/span.y, textureAspect=ts.x/ts.y;
        const float2 window=float2(min(1.0f,aspect/textureAspect),min(1.0f,textureAspect/aspect));
        float2 unit=q/span;
        const uint flip=uint(P[34]);
        if (!aging && (flip&1u)) unit.x=1.0f-unit.x;
        if (!aging && (flip&2u)) unit.y=1.0f-unit.y;
        // Flip inside the selected window so a mirror cannot crop away the retained edge.
        uv=(1.0f-window)*float2(P[32],P[33])+unit*window;
    } else if (P[10]>0.5f) {
        const float scale=max(span.x/ts.x,span.y/ts.y)*P[6]/1.2f;
        uv=((q-(span-ts*scale)*0.5f)/scale)/ts+float2(jitter,vertical);
    } else {
        const float scale=span.y/ts.y*P[6];
        uv=float2((q.x-(P[5]+jitter)*span.x)/(scale*ts.x)+P[11],
                  (q.y-(span.y-ts.y*scale)*0.5f)/(scale*ts.y)+vertical);
    }
    float3 light=wide_sample(texture,uint(ts.x),uint(ts.y),uv);
    if (aging) {
        const float aspect=span.x/span.y, textureAspect=ts.x/ts.y;
        const float2 window=float2(min(1.0f,aspect/textureAspect),min(1.0f,textureAspect/aspect));
        const float2 footprint=0.25f*window/span;
        light=0.25f*(wide_sample(texture,uint(ts.x),uint(ts.y),uv+footprint)+
                    wide_sample(texture,uint(ts.x),uint(ts.y),uv-footprint)+
                    wide_sample(texture,uint(ts.x),uint(ts.y),uv+float2(footprint.x,-footprint.y))+
                    wide_sample(texture,uint(ts.x),uint(ts.y),uv+float2(-footprint.x,footprint.y)));
    }
    if (P[8]>0.0f && !aging) {
        const float radius=0.025f*min(ts.x,ts.y)*P[8]*P[8];
        const float2 dx=float2(radius/ts.x,0),dy=float2(0,radius/ts.y);
        light=0.4f*light+0.15f*(wide_sample(texture,uint(ts.x),uint(ts.y),uv-dx)+
              wide_sample(texture,uint(ts.x),uint(ts.y),uv+dx)+wide_sample(texture,uint(ts.x),uint(ts.y),uv-dy)+
              wide_sample(texture,uint(ts.x),uint(ts.y),uv+dy));
    }
    float3 result=original;
    const float peak=max(light.x,max(light.y,light.z));
    if (film>1e-6f && peak>1e-8f) {
        const float strength=P[4];
        const float tail=aging ? 0.0f : clamp((strength-0.7f)/0.3f,0.0f,1.0f);
        const float gain=aging ? strength : min(strength/0.7f,1.0f)+3.0f*tail*tail;
        const float3 encoded=wide_encode(wide_matrix(original,P+13));
        float3 screen=encoded+max(1.0f-encoded,0.0f)*clamp(light*gain,0.0f,1.0f);
        // The natural Screen range ends at 70%; only the remaining range lifts toward white.
        const float white=smoothstep(0.0f,1.0f,tail)*smoothstep(0.12f,0.65f,peak);
        screen=mix(screen,max(screen,1.0f),white);
        result=mix(original,wide_matrix(wide_decode(screen),P+22),film);
    }
    out[3*i]=result.x;out[3*i+1]=result.y;out[3*i+2]=result.z;
}
