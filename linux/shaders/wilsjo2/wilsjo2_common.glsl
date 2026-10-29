// The wilsjo2 style: our re-implementation, from our written specification of it only (not published), of
// the NR processing around the network that the OptiScaler-NR fork (wilsjo2, v0.8.91) does - the frame encoded into
// a display-like proxy for the network, and the network's answer composed back onto the frame.
//
// Numeric rules: FP32 arithmetic with no extra half casts. Arithmetic and sampling have finite
// precision too; storing the proxy, original or answer adds the destination format's rounding.
// Constants that the specification writes as expressions are written here as the decimal of their value, so they
// are the float nearest to it and not the float division of two rounded floats. lerp is written out as
// a + t * (b - a), the way the specification defines it.
//
// The layout every program shares: t0..t3 (combined image + the linear clamp sampler) at
// bindings 0..3, u0 / u1 (storage images in their own format) at 4 / 5, and the main constant buffer (256
// bytes) as push constants.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D t0;
layout(set = 0, binding = 1) uniform sampler2D t1;
layout(set = 0, binding = 2) uniform sampler2D t2;
layout(set = 0, binding = 3) uniform sampler2D t3;
layout(set = 0, binding = 4) uniform writeonly image2D u0;
layout(set = 0, binding = 5) uniform writeonly image2D u1;

layout(push_constant, std430) uniform Constants {
    uint Mode;                    //   0
    float WhitePoint;             //   4
    uint Width;                   //   8
    uint Height;                  //  12
    float TransferStrength;       //  16
    float ColourStrength;         //  20
    uint DebugView;               //  24
    float MaxRatio;               //  28
    uint Passthrough;             //  32
    float MvScaleX;               //  36
    float MvScaleY;               //  40
    uint GuideWidth;              //  44
    uint GuideHeight;             //  48
    uint CompareMode;             //  52
    float CompareSplit;           //  56
    float CompareZoom;            //  60
    uint CompareSwap;             //  64
    uint Transfer;                //  68
    float DebugScale;             //  72
    uint ReversibleMode;          //  76
    uint ApplyModel;              //  80
    uint ColourLimit;             //  84: ours, 1 = the colour of the network's edit bounded (RenoDX's +/-1 stop)
    float ResidualScale;          //  88
    uint SkinProtection;          //  92
    uint ShowSkinMask;            //  96
    float SkinDetail;             // 100
    float SkinColour;             // 104
    float EnvironmentDetail;      // 108
    float EnvironmentColour;      // 112
    float ResidualBlend;          // 116
    uint ResidualHistoryValid;    // 120
    uint ResidualMotionBaseX;     // 124
    uint ResidualMotionBaseY;     // 128
    float ReplaceDetailStrength;  // 132
    float ModelWorkScale;         // 136
    float ResidualConfidenceSensitivity;  // 140
    uint ExposureMode;            // 144
    float PreExposure;            // 148
    float ExposureTrim;           // 152
    float ExposureProtection;     // 156
    uint ExposureAnchorCount;     // 160
    uint ExposureSourceWidth;     // 164
    uint ExposureSourceHeight;    // 168
    uint ExposurePadding;         // 172
    float ExposureAnchors[16];    // 176: 8 pairs (point, trim)
    uint Padding[4];              // 240
} cb;

// ---- numeric rules ---------------------------------------------------------------------------------------------
// Three-term sums of products (luma, the 3x3 matrices) in the order the forked OptiScaler's resolve is evaluated in on
// this hardware and driver: measured against its output byte for byte (the compiler
// fuses multiplies into adds in an order of its choosing, which our source cannot leave to chance). The resolve takes
// fma(z, fma(x, y*)); its encode the order dot() gives (fma(x, fma(y, z*))).
float sum3(vec3 a, vec3 b) { return fma(a.z, b.z, fma(a.x, b.x, a.y * b.y)); }
// ... and with the first two products added unfused, as the resolve's luminance of its composed picture (8.13 R12).
float sum3_split(vec3 a, vec3 b) {
    precise float xy = a.x * b.x + a.y * b.y;
    return fma(a.z, b.z, xy);
}
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
#ifdef WILSJO2_RESOLVE
float luma(vec3 c) { return sum3(c, kLuma); }
// The luminances of the gamut compression (7.3) take the split order (measured as sum3's above).
float luma_split(vec3 c) { return sum3_split(c, kLuma); }
vec3 mat_vec(vec3 r0, vec3 r1, vec3 r2, vec3 v) { return vec3(sum3(r0, v), sum3(r1, v), sum3(r2, v)); }
#else
float luma(vec3 c) { return dot(c, kLuma); }
float luma_split(vec3 c) { return dot(c, kLuma); }
vec3 mat_vec(vec3 r0, vec3 r1, vec3 r2, vec3 v) { return vec3(dot(r0, v), dot(r1, v), dot(r2, v)); }
#endif
bool finite1(float v) { return !(isnan(v) || isinf(v)); }
float sanitize1(float v, float f) { return finite1(v) ? v : f; }
vec3 sanitize3(vec3 v, vec3 f) {
    return vec3(sanitize1(v.x, f.x), sanitize1(v.y, f.y), sanitize1(v.z, f.z));
}
float lerp1(float a, float b, float t) { return a + t * (b - a); }
vec3 lerp3(vec3 a, vec3 b, float t) { return a + t * (b - a); }
vec3 lerp3v(vec3 a, vec3 b, vec3 t) { return a + t * (b - a); }
float saturate1(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate3(vec3 x) { return clamp(x, vec3(0.0), vec3(1.0)); }
float smoothstep1(float e0, float e1, float x) {
    const float t = saturate1((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}
float maxch(vec3 c) { return max(c.r, max(c.g, c.b)); }
float minch(vec3 c) { return min(c.r, min(c.g, c.b)); }

// ---- 7.1 sRGB transfer --------------------------------------------------------------------------------------------
vec3 srgb_encode(vec3 v) {
    v = saturate3(v);
    const vec3 a = 12.92 * v;
    const vec3 b = 1.055 * pow(max(v, vec3(1e-8)), vec3(0.41666666666666667)) - 0.055;
    return lerp3v(a, b, step(vec3(0.0031308), v));
}
vec3 srgb_decode(vec3 v) {
    v = saturate3(v);
    const vec3 a = v / 12.92;
    const vec3 b = pow((v + 0.055) / 1.055, vec3(2.4));
    return lerp3v(a, b, step(vec3(0.04045), v));
}

// ---- 7.2 OkLab ----------------------------------------------------------------------------------------------------
vec3 cbrt_signed(vec3 x) { return sign(x) * pow(abs(x), vec3(0.33333333333333333)); }
vec3 to_oklab(vec3 c) {
    const vec3 lms = mat_vec(vec3(0.4122214708, 0.5363325363, 0.0514459929), vec3(0.2119034982, 0.6806995451, 0.1073969566),
                             vec3(0.0883024619, 0.2817188376, 0.6299787005), c);
    const vec3 l = cbrt_signed(lms);
    return mat_vec(vec3(0.2104542553, 0.7936177850, -0.0040720468), vec3(1.9779984951, -2.4285922050, 0.4505937099),
                   vec3(0.0259040371, 0.7827717662, -0.8086757660), l);
}
vec3 from_oklab(vec3 L) {
    const vec3 l = mat_vec(vec3(1.0, 0.3963377774, 0.2158037573), vec3(1.0, -0.1055613458, -0.0638541728),
                           vec3(1.0, -0.0894841775, -1.2914855480), L);
    const vec3 lms = l * l * l;
    return mat_vec(vec3(4.0767416621, -3.3077115913, 0.2309699292), vec3(-1.2684380046, 2.6097574011, -0.3413193965),
                   vec3(-0.0041960863, -0.7034186147, 1.7076147010), lms);
}

// ---- 7.3 gamut compression toward the neutral axis ----------------------------------------------------------------
vec3 lms_to_bt709(vec3 c) {
    return mat_vec(vec3(5.62059812, -4.57145756, 0.15577924), vec3(-1.15555585, 2.25800438, -0.15415806),
                   vec3(0.03059913, -0.19018011, 1.06820532), c);
}
vec3 bt709_to_lms(vec3 c) {
    return mat_vec(vec3(0.30569589, 0.62271286, 0.04528636), vec3(0.15776262, 0.76968599, 0.08807030),
                   vec3(0.01933082, 0.11919478, 0.95053215), c);
}
// neutral(A, Y) = d * (Y / dY) with d = (LMS->BT.709) max(A, 1e-8), dY = max(luma(d), 1e-8): d and dY are constants,
// and d / dY = (0.99999989, 1.00000003, 1.0) in exact arithmetic - the two matrices are each other's inverse to 1e-7.
// The forked OptiScaler's resolve behaves as Y * (1, 1, 1) on this hardware and driver (measured byte for byte against
// its output: every rounding of the folded constant one ulp away matches less).
vec3 neutral_of(vec3 A, float Y) { return vec3(Y); }
float safe_div(float a, float b, float f) { return abs(b) > 1e-8 ? a / b : f; }
float compression_scale(vec3 c, vec3 A) {
    const vec3 cs = sanitize3(c, vec3(0.0));
    const float y = luma_split(cs);
    if (!(y > 1e-8)) return 1.0;
    const vec3 n = neutral_of(A, y);
    float k = 1.0;
    for (int i = 0; i < 3; ++i)
        if (cs[i] < 0.0 && n[i] > cs[i]) k = min(k, safe_div(n[i], n[i] - cs[i], 1.0));
    return saturate1(sanitize1(k, 1.0));
}
vec3 clamp_ap1(vec3 c) {
    const vec3 A = bt709_to_lms(vec3(0.18, 0.18, 0.18));
    const float k = compression_scale(c, A);
    if (k >= 1.0) return c;
    const float y = luma_split(c);
    const vec3 n = neutral_of(A, y);
    return sanitize3(n + (c - n) * k, max(n, vec3(0.0)));
}

// ---- 7.4 hue from OkLab -------------------------------------------------------------------------------------------
vec3 hue_oklab(vec3 inc, vec3 cor) {
    vec3 Li = to_oklab(inc);
    const vec3 Lc = to_oklab(cor);
    const float ci = length(Li.yz), cc = length(Lc.yz);
    const vec2 direction = cc > 1e-5 ? Lc.yz / cc : vec2(0.0);
    Li.yz = direction * ci;
    return clamp_ap1(from_oklab(Li));
}

// ---- 7.5 skin colour weight ---------------------------------------------------------------------------------------
float skin_weight(vec3 rgb) {
    rgb = saturate3(rgb);
    const float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    const float cb = (rgb.b - y) * 0.564 + 0.5;
    const float cr = (rgb.r - y) * 0.713 + 0.5;
    const vec2 dist = vec2((cb - 0.405) / 0.090, (cr - 0.600) / 0.110);
    const float chroma = maxch(rgb) - minch(rgb);
    return (1.0 - smoothstep1(0.55, 1.35, length(dist))) * smoothstep1(0.02, 0.10, chroma);
}

// ---- 7.6 - 7.8 highlight curves -----------------------------------------------------------------------------------
vec3 knee(vec3 d) {
    if (cb.Passthrough != 0u) return d;
    const float L = luma(d);
    if (L > 0.75) {
        const float rolled = 0.75 + 0.25 * (1.0 - exp(-(L - 0.75) / 0.25));
        d = d * (rolled / L);
    }
    const float peak = maxch(d);
    if (peak > 1.0) d = d / peak;
    return d;
}
// v * (neutwo(m) / m) and y * (x / m) as the fork's compiled shader evaluates them: its compiler (fast maths) cancels
// the m, leaving the reciprocal square root alone (measured: only this form gives the fork's encode bit for bit).
vec3 neutwo_encode(vec3 v) {
    v = max(v, vec3(0.0));
    const float m = maxch(v);
    if (m <= 1e-6) return v;
    return v * inversesqrt(m * m + 1.0);
}
vec3 neutwo_decode(vec3 y) {
    y = max(y, vec3(0.0));
    const float m = min(maxch(y), 0.999999);
    if (m <= 1e-6) return y;
    return y * inversesqrt(max(1.0 - m * m, 1e-8));
}
float hybrid(float m) {
    if (m <= 0.75) return m;
    const float e = (m - 0.75) / 0.25;
    return 0.75 + 0.25 * (e * inversesqrt(e * e + 1.0));
}
vec3 hybrid_encode(vec3 v) {
    v = max(v, vec3(0.0));
    const float m = maxch(v);
    if (m <= 1e-6) return v;
    return v * (hybrid(m) / m);
}
float hybrid_inv(float y) {
    if (y <= 0.75) return y;
    const float u = min((y - 0.75) / 0.25, 0.999999);
    const float e = u * inversesqrt(max(1.0 - u * u, 1e-8));
    return 0.75 + 0.25 * e;
}
vec3 hybrid_decode(vec3 y) {
    y = max(y, vec3(0.0));
    const float m = maxch(y);
    if (m <= 1e-6) return y;
    return y * (hybrid_inv(m) / m);
}
// The encode's curve by ReversibleMode (7.8): 0 the soft knee, 3 and above hybrid, 1 and 2 Neutwo.
vec3 encode_curve(vec3 n) {
    const uint rm = cb.ReversibleMode;
    return rm == 0u ? knee(n) : rm >= 3u ? hybrid_encode(n) : neutwo_encode(n);
}

// ---- 7.9 the proxy rebuilt from the original -------------------------------------------------------------------
vec3 full_proxy(vec3 O) {
    if (cb.Passthrough != 0u) return saturate3(O);
    const uint rm = cb.ReversibleMode;
    if (rm == 0u) return saturate3(knee(O));
    return rm >= 3u ? hybrid_encode(O) : neutwo_encode(O);
}

// ---- 7.10 white point ---------------------------------------------------------------------------------------------
// The 1x1 exposure is t3; read only for ExposureMode 1 or 3.
float white_point() {
    const float wp = cb.WhitePoint;
    float base = max(wp, 1e-4);
    const uint em = cb.ExposureMode;
    if (em == 1u || em == 3u) {
        const float measured = texelFetch(t3, ivec2(0, 0), 0).r;
        if (finite1(measured) && measured > 1e-8) {
            base = em == 1u ? cb.PreExposure / measured : measured;
            float trim = cb.ExposureTrim;
            const uint n = min(cb.ExposureAnchorCount, 8u);
            if (n > 0u) {
                trim = cb.ExposureAnchors[1];
                for (uint i = 1u; i < 8u; ++i) {
                    if (i < n && base > cb.ExposureAnchors[2u * (i - 1u)]) {
                        const float p0 = cb.ExposureAnchors[2u * (i - 1u)], p1 = cb.ExposureAnchors[2u * i];
                        const float r0 = cb.ExposureAnchors[2u * (i - 1u) + 1u], r1 = cb.ExposureAnchors[2u * i + 1u];
                        const float t = saturate1(log2(max(base, 1e-8) / p0) / log2(p1 / p0));
                        trim = exp2(lerp1(log2(r0), log2(r1), t));
                    }
                }
            }
            base = base * trim;
        }
    }
    return max(sanitize1(base, wp), 1e-4);
}

// ---- 7.11 cube scaling of a residual ------------------------------------------------------------------------------
vec3 cube_scale(vec3 P, vec3 T) {
    if (cb.Passthrough != 0u) return T;
    const vec3 d = T - P;
    float alpha = 1.0;
    for (int c = 0; c < 3; ++c) {
        if (d[c] > 1e-6) alpha = min(alpha, (1.0 - P[c]) / d[c]);
        else if (d[c] < -1e-6) alpha = min(alpha, (0.0 - P[c]) / d[c]);
    }
    return P + saturate1(alpha) * d;
}

bool outside(uvec2 p) { return p.x >= cb.Width || p.y >= cb.Height; }
// The dispatch pixel's uv.
vec2 pixel_uv(uvec2 p) { return (vec2(p) + 0.5) / vec2(float(cb.Width), float(cb.Height)); }

#ifdef WILSJO2_RESOLVE
// ---- 7.12 the resize field (Transfer fields 3 / 4): t0 = the small proxy (field 4: the encoded field), t1 = the small
// answer; read by Load except where said ---------------------------------------------------------------------------
vec3 field_encode(vec3 f) { return 0.5 + 0.5 * f / (1.0 + abs(f)); }
vec3 field_decode(vec3 c) {
    const vec3 d = clamp(2.0 * sanitize3(c, vec3(0.5)) - 1.0, vec3(-0.999), vec3(0.999));
    return d / (1.0 - abs(d));
}
vec2 chroma(vec3 c) {
    const float y = luma(c);
    return y > 1e-6 ? vec2(c.r / y, c.b / y) : vec2(1.0);
}
vec3 lin(vec3 c) { return cb.Passthrough != 0u ? c : srgb_decode(c); }
vec3 paired_field(ivec2 p, ivec2 size) {
    const vec3 s = lin(texelFetch(t0, p, 0).rgb), m = lin(texelFetch(t1, p, 0).rgb);
    float sY = luma(s), mY = luma(m);
    if (sY < 1.0 / 1024.0) {
        sY = 0.0;
        mY = 0.0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), size - 1);
                sY += luma(lin(texelFetch(t0, q, 0).rgb));
                mY += luma(lin(texelFetch(t1, q, 0).rgb));
            }
    }
    const vec2 cm = chroma(m), cs = chroma(s);
    return sanitize3(vec3((mY - sY) / max(sY, 1.0 / 1024.0), cm.x - cs.x, cm.y - cs.y), vec3(0.0));
}
vec3 sample_field(vec2 uv, bool encoded) {
    const ivec2 wh = textureSize(t0, 0);
    const vec2 q = uv * vec2(wh) - 0.5;
    const vec2 b = floor(q), f = q - floor(q);
    vec3 field = vec3(0.0);
    for (int ty = 0; ty <= 1; ++ty)
        for (int tx = 0; tx <= 1; ++tx) {
            const ivec2 p = clamp(ivec2(b) + ivec2(tx, ty), ivec2(0), wh - 1);
            const vec3 tap = encoded ? field_decode(texelFetch(t0, p, 0).rgb) : paired_field(p, wh);
            field += tap * (tx == 1 ? f.x : 1.0 - f.x) * (ty == 1 ? f.y : 1.0 - f.y);
        }
    return field;
}
vec3 restore_field(vec3 base, vec3 field) {
    const float y = max(0.0, luma(base) * (1.0 + field.x));
    const vec2 c = field.yz + chroma(base);
    const float r = c.x * y, b = c.y * y;
    return vec3(r, (y - 0.2126 * r - 0.0722 * b) / 0.7152, b);
}
vec3 bound_carrier(vec2 uv, vec3 cand) {
    const ivec2 wh = textureSize(t0, 0);
    const ivec2 b = ivec2(floor(uv * vec2(wh) - 0.5));
    vec3 lo = vec3(1.0), hi = vec3(0.0);
    for (int ty = 0; ty <= 1; ++ty)
        for (int tx = 0; tx <= 1; ++tx) {
            const vec3 tap = sanitize3(texelFetch(t0, clamp(b + ivec2(tx, ty), ivec2(0), wh - 1), 0).rgb, vec3(0.5));
            lo = min(lo, tap);
            hi = max(hi, tap);
        }
    const vec3 ref = clamp(sanitize3(textureLod(t0, uv, 0.0).rgb, vec3(0.5)), lo, hi);
    const vec3 d = sanitize3(cand, ref) - ref;
    float a = 1.0;
    for (int c = 0; c < 3; ++c) {
        if (d[c] > 0.0) a = min(a, (hi[c] - ref[c]) / d[c]);
        else if (d[c] < 0.0) a = min(a, (lo[c] - ref[c]) / d[c]);
    }
    return clamp(ref + saturate1(a) * d, lo, hi);
}
// Enlargement "Edge-aware" (Transfer 3, the shader's 5; from the ReShade route's former runtime_transfer.comp, the
// idea of carrying the change as lighting and colour being OptiScaler-NR's): the network's change measured at the model's
// extent as a lighting gain (log2 luminance ratio) and a chromaticity change (luminance-normalised RGB), enlarged, and put
// on the full-resolution proxy P. Each of the 4x4 model texels weighted by how close its input luminance is to the
// pixel's (joint bilateral upsampling guided by P, range 0.25 stops) on a Gaussian of 0.6 model texels, plain spatial
// weights the fallback. Near black the additive residual instead. t0 / t1: the proxy and the answer at the model extent.
const float kLightingEps = 1.0 / 256.0;
vec3 model_value(vec3 v) { return cb.Passthrough != 0u ? v : srgb_decode(v); }
vec3 lighting(uvec2 xy, vec3 P) {
    const ivec2 wh = textureSize(t0, 0);
    const vec2 p = (vec2(xy) + 0.5) * (vec2(wh) / vec2(float(cb.Width), float(cb.Height))) - 0.5;
    const ivec2 base = ivec2(floor(p));
    const vec2 f = p - vec2(base);
    const ivec2 hi = wh - 1;
    const float yp = dot(P, kLuma);
    const float lp = log2(yp + kLightingEps);
    const float inv2s2 = 1.0 / (2.0 * 0.25 * 0.25);
    float wg = 0.0, ws_sum = 0.0, g_g = 0.0, g_s = 0.0;
    vec3 c_g = vec3(0.0), c_s = vec3(0.0), r_g = vec3(0.0), r_s = vec3(0.0);
    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            const ivec2 q = clamp(base + ivec2(i, j), ivec2(0), hi);
            const vec3 m = model_value(texelFetch(t1, q, 0).rgb), s = model_value(texelFetch(t0, q, 0).rgb);
            const float ym = dot(m, kLuma), ys = dot(s, kLuma);
            const float g = log2((ym + kLightingEps) / (ys + kLightingEps));
            const vec3 dc = m / (ym + kLightingEps) - s / (ys + kLightingEps);
            const vec3 res = m - s;
            const vec2 d = abs(vec2(i, j) - f);
            const float ws = exp(-dot(d, d) * (1.0 / (2.0 * 0.36)));
            const float dl = lp - log2(ys + kLightingEps);
            const float w = ws * exp(-dl * dl * inv2s2);
            g_g += w * g; c_g += w * dc; r_g += w * res; wg += w;
            g_s += ws * g; c_s += ws * dc; r_s += ws * res; ws_sum += ws;
        }
    }
    float G; vec3 DC, RES;
    if (wg > 1e-4 * ws_sum) { G = g_g / wg; DC = c_g / wg; RES = r_g / wg; }
    else { G = g_s / ws_sum; DC = c_s / ws_sum; RES = r_s / ws_sum; }
    const vec3 rebuilt = max(P + DC * (yp + kLightingEps), vec3(0.0)) * exp2(G);
    const vec3 added = P + RES;
    return mix(added, rebuilt, smoothstep(kLightingEps, 8.0 * kLightingEps, yp));
}
vec3 reconstruct(vec3 base, vec2 uv, vec3 carrier) {
    const bool dlss = cb.Transfer == 4u;
    const vec3 field = dlss ? field_decode(bound_carrier(uv, carrier)) : sample_field(uv, false);
    vec3 r = restore_field(base, field);
    const uint rm = cb.ReversibleMode;
    if (dlss && cb.Passthrough == 0u && (rm == 2u || rm == 4u)) {
        const vec3 ref = restore_field(base, sample_field(uv, true));
        if (any(greaterThanEqual(r, vec3(1.0))) && all(lessThan(ref, vec3(1.0)))) r = ref;
    }
    const float y = saturate1(luma(r));
    const vec3 d = r - y;
    float a = 1.0;
    for (int c = 0; c < 3; ++c) {
        if (d[c] > 0.0) a = min(a, (1.0 - y) / d[c]);
        else if (d[c] < 0.0) a = min(a, -y / d[c]);
    }
    return saturate3(y + saturate1(a) * d);
}
#endif
