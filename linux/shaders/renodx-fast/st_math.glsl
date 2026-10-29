
vec3 st_dec(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045))); }
vec3 st_enc(vec3 l) { l = clamp(l, 0.0, 1.0); return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, greaterThan(l, vec3(0.0031308))); }
vec3 st_lab(vec3 s) {
    const vec3 l = st_dec(max(s, 0.0));
    const vec3 lms = vec3(dot(l, vec3(0.4122214708, 0.5363325363, 0.0514459929)),
                          dot(l, vec3(0.2119034982, 0.6806995451, 0.1073969566)),
                          dot(l, vec3(0.0883024619, 0.2817188376, 0.6299787005)));
    const vec3 q = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return vec3(dot(q, vec3(0.2104542553, 0.7936177850, -0.0040720468)),
                dot(q, vec3(1.9779984951, -2.4285922050, 0.4505937099)),
                dot(q, vec3(0.0259040371, 0.7827717662, -0.8086757660)));
}
vec3 st_lin(vec3 lab) {
    const vec3 q = vec3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                        lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                        lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    const vec3 lms = q * q * q;
    return vec3(dot(lms, vec3(4.0767416621, -3.3077115913, 0.2309699292)),
                dot(lms, vec3(-1.2684380046, 2.6097574011, -0.3413193965)),
                dot(lms, vec3(-0.0041960863, -0.7034186147, 1.7076147010)));
}
bool st_in(vec3 l) { return all(greaterThanEqual(l, vec3(-1e-6))) && all(lessThanEqual(l, vec3(1.0 + 1e-6))); }
vec3 st_at(vec3 X, vec3 O, vec2 h, float t) {
    const float lx = clamp(X.x, 1e-4, 1.0 - 1e-4), lo = clamp(O.x, 1e-4, 1.0 - 1e-4);
    const float L = lo < lx ? lx * pow(lo / lx, t) : 1.0 - (1.0 - lx) * pow((1.0 - lo) / (1.0 - lx), t);
    const float c = max(length(X.yz) + t * (length(O.yz) - length(X.yz)), 0.0);
    return vec3(L, c * h);
}
vec3 st_enhance(vec3 p, vec3 n, float k) {
    const vec3 X = st_lab(p), O = st_lab(n);
    const float co = length(O.yz);
    const vec2 h = co > 1e-7 ? O.yz / co : vec2(0.0);
    vec3 l = st_lin(st_at(X, O, h, k));
    if (st_in(l)) return st_enc(l);
    float lo = 1.0, hi = k;
    for (int i = 0; i < 12; ++i) { const float m = 0.5 * (lo + hi); if (st_in(st_lin(st_at(X, O, h, m)))) lo = m; else hi = m; }
    l = st_lin(st_at(X, O, h, lo));
    return st_in(l) ? st_enc(l) : n;
}
const vec3 st_lw = vec3(0.2126, 0.7152, 0.0722);
vec3 st_up(texture2D lowtex, vec2 pos, float guide, uvec2 low, out float conf) {
    const vec2 q = clamp(pos / 16.0 - 0.5, vec2(0.0), vec2(low) - 1.0);
    const ivec2 a = ivec2(floor(q)), b = min(a + 1, ivec2(low) - 1);
    const vec2 f = q - vec2(a);
    const ivec2 t[4] = ivec2[4](a, ivec2(b.x, a.y), ivec2(a.x, b.y), b);
    const float bw[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    vec3 acc = vec3(0.0); float ws = 0.0;
    for (int i = 0; i < 4; ++i) {
        const vec4 v = texelFetch(lowtex, t[i], 0);
        const float d = (guide - v.a) / 0.06;
        const float w = bw[i] * exp(-d * d);
        acc += w * v.rgb; ws += w;
    }
    conf = clamp(ws, 0.0, 1.0);
    return ws > 1e-5 ? acc / ws : vec3(0.0);
}
vec3 st_gfq(texture2D CA, texture2D CB, vec2 pos, float I, uvec2 low, out float mI, out float vI) {
    const vec2 q = clamp(pos / 4.0 - 0.5, vec2(0.0), vec2(low) - 1.0);
    const ivec2 a = ivec2(floor(q)), b = min(a + 1, ivec2(low) - 1);
    const vec2 f = q - vec2(a);
    const vec4 A = mix(mix(texelFetch(CA, a, 0), texelFetch(CA, ivec2(b.x, a.y), 0), f.x),
                       mix(texelFetch(CA, ivec2(a.x, b.y), 0), texelFetch(CA, b, 0), f.x), f.y);
    const vec4 B = mix(mix(texelFetch(CB, a, 0), texelFetch(CB, ivec2(b.x, a.y), 0), f.x),
                       mix(texelFetch(CB, ivec2(a.x, b.y), 0), texelFetch(CB, b, 0), f.x), f.y);
    mI = A.w; vI = B.w;
    return A.rgb * I + B.rgb;
}
float st_soft(float anchor, float extra) {
    const float a = clamp(anchor, 1e-4, 1.0 - 1e-4);
    return extra < 0.0 ? a * exp(extra / a) : 1.0 - (1.0 - a) * exp(-extra / (1.0 - a));
}
vec3 st_relight(vec3 lab, float Lb, float extra) {
    vec3 l = st_lin(vec3(st_soft(Lb, extra), lab.yz));
    if (st_in(l)) return st_enc(l);
    float lo = 0.0, hi = 1.0;
    for (int i = 0; i < 12; ++i) { const float m = 0.5 * (lo + hi); if (st_in(st_lin(vec3(st_soft(Lb, m * extra), lab.yz)))) lo = m; else hi = m; }
    return st_enc(st_lin(vec3(st_soft(Lb, lo * extra), lab.yz)));
}
vec3 st_relight2(vec3 lab, float Lb, float extra, float lg) {
    vec3 l = st_lin(vec3(st_soft(Lb, extra), lab.yz * exp(lg)));
    if (st_in(l)) return st_enc(l);
    float lo = 0.0, hi = 1.0;
    for (int i = 0; i < 12; ++i) { const float m = 0.5 * (lo + hi); if (st_in(st_lin(vec3(st_soft(Lb, m * extra), lab.yz * exp(m * lg))))) lo = m; else hi = m; }
    return st_enc(st_lin(vec3(st_soft(Lb, lo * extra), lab.yz * exp(lo * lg))));
}
