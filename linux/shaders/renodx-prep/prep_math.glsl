// The Preprocess math (its curves, gain, forward/backward and meter), shared by the RenoDX-style option's Preprocess
// pass (renodx_prep.comp) and the wilsjo2 layers' (wilsjo2_prep.comp). Until 10-26 a checked copy of the runtime's own
// Preprocess pass (runtime_prep.comp), which went with the runtime's own composition: this is the one source now.
// ---- the curves, scene-linear -> display-linear, and their inverses ------------------------------
// 1 neutral (Khronos PBR Neutral's highlight half; its toe is left out, see the probe), 2 reinhard,
// 3 filmic (Hable), 4 gt (Uchimura), 5 aces (Narkowicz), 6 agx. 0 none: OptiScaler's own knee on a
// linear frame, nothing on an SDR one.
vec3 neutral(vec3 c) {
    const float start = 0.76, d = 0.24, desat = 0.15;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    const float np = 1.0 - d * d / (peak + d - start);
    c *= np / peak;
    const float g = 1.0 - 1.0 / (desat * (peak - np) + 1.0);
    return mix(c, vec3(np), g);
}
vec3 neutral_inv(vec3 y) {
    const float start = 0.76, d = 0.24, desat = 0.15;
    const float np = max(y.r, max(y.g, y.b));
    if (np < start) return y;
    const float peak = d * d / max(1.0 - np, 1e-6) - d + start;
    const float g = 1.0 - 1.0 / (desat * (peak - np) + 1.0);
    return (y - g * np) / (1.0 - g) * (peak / np);
}
vec3 reinhard(vec3 x) { return x / (1.0 + x); }
vec3 reinhard_inv(vec3 y) { y = min(y, vec3(0.9999)); return y / (1.0 - y); }

const float HA = 0.15, HB = 0.50, HC = 0.10, HD = 0.20, HE = 0.02, HF = 0.30;
float hable1(float x) { return ((x * (HA * x + HC * HB) + HD * HE) / (x * (HA * x + HB) + HD * HF)) - HE / HF; }
vec3 hable(vec3 x) {
    const float w = hable1(11.2);
    return vec3(hable1(x.r), hable1(x.g), hable1(x.b)) / w;
}
float hable_inv1(float y) {
    const float t = min(y * hable1(11.2) + HE / HF, 0.999);
    const float qa = HA * (1.0 - t), qb = HC * HB - HB * t, qc = HD * HE - HD * HF * t;
    return max((-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa), 0.0);
}
vec3 hable_inv(vec3 y) { return vec3(hable_inv1(y.r), hable_inv1(y.g), hable_inv1(y.b)); }

float gt1(float x) {
    const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
    const float l0 = ((P - m) * l) / a, S0 = m + l0, S1 = m + a * l0;
    const float C2 = (a * P) / (P - S1), CP = -C2 / P;
    const float w0 = 1.0 - smoothstep(0.0, m, x);
    const float w2 = step(m + l0, x);
    const float w1 = 1.0 - w0 - w2;
    const float T = m * pow(max(x, 0.0) / m, c) + b;
    const float S = P - (P - S1) * exp(CP * (x - S0));
    const float L = m + a * (x - m);
    return T * w0 + L * w1 + S * w2;
}
vec3 gt(vec3 x) { return vec3(gt1(x.r), gt1(x.g), gt1(x.b)); }
// The inverse in closed form above m (the linear part, then the exponential shoulder) and by Newton's method on
// t = x / m in the toe (4 steps from t = (y / m)^(1 / c)); matched a 32-step bisection within the probe's tested range and tolerance.
float gt_inv1(float y) {
    const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33;
    const float l0 = ((P - m) * l) / a, S0 = m + l0, S1 = m + a * l0;
    const float C2 = (a * P) / (P - S1);
    if (y >= gt1(64.0)) return 64.0;
    if (y >= S0) return S0 - log(max((P - y) / (P - S1), 1e-30)) / C2;
    if (y >= m) return y;
    float t = pow(max(y, 0.0) / m, 1.0 / c);
    for (int i = 0; i < 4; ++i) {
        const float s = t * t * (3.0 - 2.0 * t), ds = 6.0 * t * (1.0 - t), tc = pow(t, c);
        const float g = tc * (1.0 - s) + t * s - y / m;
        const float dg = c * pow(max(t, 1e-20), c - 1.0) * (1.0 - s) - tc * ds + s + t * ds;
        t = clamp(t - g / max(dg, 1e-6), 0.0, 1.0);
    }
    return m * t;
}
vec3 gt_inv(vec3 y) { return vec3(gt_inv1(y.r), gt_inv1(y.g), gt_inv1(y.b)); }

vec3 aces(vec3 x) { return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14); }
vec3 aces_inv(vec3 y) {
    y = min(y, vec3(0.9999));
    const vec3 qa = 2.51 - 2.43 * y, qb = 0.03 - 0.59 * y, qc = -0.14 * y;
    return max((-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa), 0.0);
}

const mat3 AGX_IN = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                         0.0784335999999992, 0.878468636469772, 0.0784336,
                         0.0792237451477643, 0.0791661274605434, 0.879142973793104);
const mat3 AGX_OUT = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                          -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                          -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
const float AGX_MIN = -12.47393, AGX_MAX = 4.026069;
float agx_sig(float x) {
    const float x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float agx_dsig(float x) {
    const float x2 = x * x, x4 = x2 * x2;
    return 93.0 * x4 * x - 200.7 * x4 + 127.84 * x2 * x - 20.604 * x2 + 0.8596 * x + 0.1191;
}
// 8 bisection steps, then 3 Newton steps on the sigmoid interval [0,1].
// This does not undo the forward log/gamut clamps.
float agx_sig_inv(float y) {
    y = clamp(y, agx_sig(0.0), agx_sig(1.0));
    float lo = 0.0, hi = 1.0;
    for (int i = 0; i < 8; ++i) {
        const float mid = 0.5 * (lo + hi);
        if (agx_sig(mid) < y) lo = mid; else hi = mid;
    }
    float x = 0.5 * (lo + hi);
    for (int i = 0; i < 3; ++i) x = clamp(x - (agx_sig(x) - y) / agx_dsig(x), lo, hi);
    return x;
}
vec3 agx(vec3 c) {
    vec3 v = AGX_IN * c;
    v = clamp(log2(max(v, vec3(1e-10))), AGX_MIN, AGX_MAX);
    v = (v - AGX_MIN) / (AGX_MAX - AGX_MIN);
    v = vec3(agx_sig(v.r), agx_sig(v.g), agx_sig(v.b));
    return pow(max(AGX_OUT * v, vec3(0.0)), vec3(2.2));
}
vec3 agx_inv(vec3 y) {
    vec3 v = inverse(AGX_OUT) * pow(max(y, vec3(0.0)), vec3(1.0 / 2.2));
    v = vec3(agx_sig_inv(v.r), agx_sig_inv(v.g), agx_sig_inv(v.b));
    v = exp2(v * (AGX_MAX - AGX_MIN) + AGX_MIN);
    return inverse(AGX_IN) * v;
}

vec3 curve(vec3 x) {
    switch (pc.curve) {
    case 1u: return neutral(x * pc.k);
    case 2u: return reinhard(x * pc.k);
    case 3u: return hable(x * pc.k);
    case 4u: return gt(x * pc.k);
    case 5u: return aces(x * pc.k);
    case 6u: return agx(x * pc.k);
    default: return unknee() ? knee(x) : x;
    }
}
vec3 curve_inv(vec3 y) {
    switch (pc.curve) {
    case 1u: return neutral_inv(y) / pc.k;
    case 2u: return reinhard_inv(y) / pc.k;
    case 3u: return hable_inv(y) / pc.k;
    case 4u: return gt_inv(y) / pc.k;
    case 5u: return aces_inv(y) / pc.k;
    case 6u: return agx_inv(y) / pc.k;
    default: return unknee() ? knee_inv(y) : y;
    }
}

// auto: the meter's gain (bias included); otherwise the bias alone, 0 when exposure is off.
float gain() { return (pc.flags & AUTO) != 0u ? st.s[2] : exp2(pc.bias); }

vec3 forward(vec3 x) {
    x *= gain();
    if (pc.contrast != 1.0) x = 0.18 * pow(max(x, vec3(0.0)) / 0.18, vec3(pc.contrast));
    if (pc.saturation != 1.0) {
        const float l = dot(x, LUMA);
        x = max(l + pc.saturation * (x - l), vec3(0.0));
    }
    return curve(x);
}
vec3 backward(vec3 y) {
    vec3 x = curve_inv(y);
    if (pc.saturation != 1.0) {
        const float l = dot(x, LUMA);
        x = l + (x - l) / pc.saturation;
    }
    if (pc.contrast != 1.0) x = 0.18 * pow(max(x, vec3(0.0)) / 0.18, vec3(1.0 / pc.contrast));
    return x / gain();
}

bool finite(vec3 v) { return !any(isnan(v)) && !any(isinf(v)); }

// The meter is a histogram auto exposure after Unreal Engine's design, with its defaults for the
// percentile window and the adaptation speeds: a histogram of log2 luminance (about 14000 samples,
// 1/6.4 stop bins from 2^-16), the average of the part between the 10th and the 90th percentile -
// the sun, the sky and the lamps above, black corners below, left out - brought to 18% grey, clamped
// to kMinGain..kMaxGain, and adapted exponentially in real time: 3 stops a second when the scene gets
// brighter, 1 when it gets darker. The histogram range and the gain limits are this project's, wide
// enough for 007 First Light, which computes its exposure and does not hand it over.
const float kLog2Key = -2.4739;    // log2(0.18)
const float kLow = 0.10, kHigh = 0.90;
const float kMinGain = -4.0, kMaxGain = 8.0;
const float kSpeedBrighter = 3.0, kSpeedDarker = 1.0;   // per second, scene getting brighter / darker
const uint kBins = 128u;
shared uint hist[128];

void meter() {
    const uint t = gl_LocalInvocationIndex;
    if (t < kBins) hist[t] = 0u;
    barrier();
    const uint sx = max(1u, pc.width / 160u), sy = max(1u, pc.height / 90u);
    const uint nx = (pc.width + sx - 1u) / sx, ny = (pc.height + sy - 1u) / sy;
    for (uint i = t; i < nx * ny; i += 256u) {
        const ivec2 xy = ivec2((i % nx) * sx, (i / nx) * sy);
#ifdef PREP_FRAME
        const vec3 P = load_p(xy).rgb;
#else
        const vec3 P = imageLoad(a, xy).rgb;
#endif
        if (!finite(P)) continue;
        const float l = max(dot(light_at(P, xy), LUMA), 1.5e-5);
        atomicAdd(hist[uint(clamp((log2(l) + 16.0) * 6.4, 0.0, 127.0))], 1u);
    }
    barrier();
    if (t != 0u) return;
    uint n = 0u;
    for (uint b = 0u; b < kBins; ++b) n += hist[b];
    const bool valid = st.s[1] == 1.0 && (pc.flags & RESET) == 0u;
    float s = valid ? st.s[0] : 0.0;
    if (n >= 64u) {
        // The mean of log2 luminance over the samples ranked kLow..kHigh, bins cut where the window
        // cuts them.
        const float lo = kLow * float(n), hi = kHigh * float(n);
        float below = 0.0, sum = 0.0, weight = 0.0;
        for (uint b = 0u; b < kBins; ++b) {
            const float c = float(hist[b]);
            const float from = max(below, lo), to = min(below + c, hi);
            if (to > from) { sum += (to - from) * ((float(b) + 0.5) / 6.4 - 16.0); weight += to - from; }
            below += c;
        }
        const float target = clamp(kLog2Key - sum / max(weight, 1.0), kMinGain, kMaxGain);
        st.s[3] = target;
        // Gain falling = the scene got brighter.
        const float speed = target < s ? kSpeedBrighter : kSpeedDarker;
        s = valid ? s + (target - s) * (1.0 - exp(-clamp(pc.dt, 0.0, 0.25) * speed)) : target;
    }
    s = clamp(s, kMinGain, kMaxGain);
    st.s[0] = s; st.s[1] = 1.0; st.s[2] = exp2(s + pc.bias);
}

