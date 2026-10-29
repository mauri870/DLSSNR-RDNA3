// The wilsjo2 style's GPU half and constant maths: see nr_wilsjo2.hpp. Written from
// our specification of the style (section numbers below are its; not published).
#include "nr_number.hpp"
#include "nr_wilsjo2.hpp"
#include "nr_gpu_completion.hpp"
#include "nr_log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nr::wilsjo2 {
namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("wilsjo2 stage: ") + what + " failed (" + std::to_string(int(r)) + ")");
}

bool finite(float v) { return std::isfinite(v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool parse_bool(const std::string& v, bool* out) {
    const std::string l = lower(v);
    if (l == "true" || l == "1" || l == "on" || l == "yes") { *out = true; return true; }
    if (l == "false" || l == "0" || l == "off" || l == "no") { *out = false; return true; }
    nr::bad_number(v);
    return false;
}
bool parse_float(const std::string& v, float* out) {
    size_t end = 0;
    const float f = nr::parse_float(v, 0.0f, &end);
    if (!end) return false;
    *out = f;
    return true;
}
bool parse_uint(const std::string& v, uint32_t* out) {
    bool ok = false;
    const uint32_t n = nr::parse_integer<uint32_t>(v, 0, &ok);
    if (!ok) return false;
    *out = n;
    return true;
}

std::vector<uint32_t> read_spirv(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("wilsjo2 stage: cannot open " + path);
    const std::streamoff n = f.tellg();
    std::vector<uint32_t> code(size_t(n) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), n);
    if (code.empty() || code[0] != 0x07230203u) throw std::runtime_error("wilsjo2 stage: not SPIR-V: " + path);
    return code;
}

// Every UniformConstant image variable's binding, sampled (OpTypeImage Sampled = 1) or storage (2), in binding order
// (Passes (fast)'s programs have images only; as nr_renodx.cpp reflect() reads them).
std::vector<std::pair<uint32_t, VkDescriptorType>> reflect_images(const std::vector<uint32_t>& code) {
    std::map<uint32_t, uint32_t> binding_of, pointee, image_sampled;
    std::vector<std::pair<uint32_t, uint32_t>> variables;   // (pointer type, id)
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xffffu, len = code[i] >> 16;
        if (len == 0) break;
        const uint32_t* w = &code[i];
        if (op == 71 && len >= 4 && w[2] == 33) binding_of[w[1]] = w[3];               // OpDecorate Binding
        else if (op == 32 && len >= 4) pointee[w[1]] = w[3];                            // OpTypePointer
        else if (op == 25 && len >= 9) image_sampled[w[1]] = w[7];                      // OpTypeImage
        else if (op == 59 && len >= 4 && w[3] == 0) variables.push_back({w[1], w[2]});  // OpVariable UniformConstant
        else if (op == 59 && len >= 4 && (w[3] == 2 || w[3] == 12))
            throw std::runtime_error("wilsjo2 stage: a fast program with a buffer");
        i += len;
    }
    std::map<uint32_t, VkDescriptorType> out;
    for (const auto& [ptr, id] : variables) {
        const auto b = binding_of.find(id);
        const auto t = pointee.find(ptr);
        if (b == binding_of.end() || t == pointee.end()) continue;
        const auto s = image_sampled.find(t->second);
        if (s == image_sampled.end()) throw std::runtime_error("wilsjo2 stage: a fast program with a non-image resource");
        out[b->second] = s->second == 1 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return {out.begin(), out.end()};
}

}  // namespace

// ---- settings ------------------------------------------------------------------------------------------
namespace {
// "Pass{N}{Field}" (N 2..30): its pass index (N - 1) and field, or 0
uint32_t pass_key(const std::string& key, std::string* field) {
    if (key.size() < 6 || key.compare(0, 4, "pass") != 0 || !std::isdigit(static_cast<unsigned char>(key[4]))) return 0;
    size_t i = 4;
    uint32_t n = 0;
    while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i])) && n < 100) n = n * 10 + uint32_t(key[i++] - '0');
    if (n < 2 || n > kMaxPasses) return 0;
    *field = key.substr(i);
    static const char* const fields[] = {"preset", "style", "intensity", "localstructure", "localtone", "skinstructure",
                                         "automask"};
    for (const char* f : fields)
        if (*field == f) return (*field == "preset" && n > 3) ? 0 : n - 1;
    return 0;
}
}  // namespace

bool parse_key(Settings& s, const std::string& raw_key, const std::string& value) {
    const std::string key = lower(raw_key);
    const std::string v = lower(value);
    std::string field;
    if (const uint32_t k = pass_key(key, &field)) {   // a later pass's own value; auto / empty: inherit
        if (v.empty() || v == "auto") return true;
        PassOverride& o = s.pass[k];
        if (field == "preset") return o.has_preset = parse_uint(value, &o.preset);
        if (field == "style") return o.has_style = parse_uint(value, &o.style);
        if (field == "intensity") return o.has_intensity = parse_float(value, &o.intensity);
        if (field == "localstructure") return o.has_local_structure = parse_float(value, &o.local_structure);
        if (field == "localtone") return o.has_local_tone = parse_float(value, &o.local_tone);
        if (field == "skinstructure") return o.has_skin_structure = parse_float(value, &o.skin_structure);
        return o.has_auto_mask = parse_bool(value, &o.auto_mask);
    }
    if (v.empty() || v == "auto") {
        // the default stays; still one of ours?
        static const char* const known[] = {"runbeforesr", "passes", "unlockpasses", "workingscale", "scalingdownscaler",
                                            "spatialcompression", "spatialcenterx", "spatialcentery", "spatialworkx",
                                            "spatialworky", "spatialoffsetx", "spatialoffsety", "spatialshiftx",
                                            "spatialshifty",
                                            "preset", "style", "intensity", "localstructure", "localtone",
                                            "skinstructure", "automask", "history", "noiseeveryframe", "transferstrength", "colourstrength", "maxratio",
                                            "reversiblemode", "replacedetailstrength", "transfer", "whitepointscale",
                                            "whitepointsource", "whitepointtrim", "exposuretrimanchors", "autoexposuretrim",
                                            "autoexposurehighlightprotection", "autoexposuretrimanchors", "skinprotection",
                                            "skindetail", "skincolour", "environmentdetail", "environmentcolour",
                                            "classicscaler", "downscale", "passesmode", "fastmethod", "fasteditstrength", "colourlimit",
                                            "fastextrapolationpasses", "fastextrapolationhalolimit", "fastextrapolationfine",
                                            "fastfeedbackpasses", "fastfeedbackhalolimit", "fastfeedbackfine",
                                            "fasthybridpasses", "fasthybridhalolimit", "fasthybridfine"};
        for (const char* k : known)
            if (key == k) return true;
        return false;
    }
    if (key == "runbeforesr") return parse_bool(value, &s.run_before_sr);
    if (key == "passes") return parse_uint(value, &s.passes);
    if (key == "history") {
        if (!parse_float(value, &s.history_strength)) return false;
        s.history_strength = std::clamp(s.history_strength, 0.f, 1.f);
        return true;
    }
    if (key == "workingscale") return parse_float(value, &s.working_scale);
    if (key == "spatialcompression") return parse_bool(value, &s.spatial);
    if (key == "spatialcenterx") return parse_float(value, &s.spatial_centre[0]);
    if (key == "spatialcentery") return parse_float(value, &s.spatial_centre[1]);
    if (key == "spatialworkx") return parse_float(value, &s.spatial_work[0]);
    if (key == "spatialworky") return parse_float(value, &s.spatial_work[1]);
    if (key == "spatialoffsetx") return parse_float(value, &s.spatial_offset[0]);
    if (key == "spatialoffsety") return parse_float(value, &s.spatial_offset[1]);
    if (key == "spatialshiftx") return parse_float(value, &s.spatial_shift[0]);
    if (key == "spatialshifty") return parse_float(value, &s.spatial_shift[1]);
    if (key == "scalingdownscaler") {   // 0..7, anything else the default (Lanczos3)
        uint32_t v = 4;
        if (!parse_uint(value, &v)) return false;
        s.scaling_downscaler = v <= 7 ? v : 4u;
        return true;
    }
    if (key == "unlockpasses") return parse_bool(value, &s.unlock_passes);
    if (key == "colourlimit") return parse_bool(value, &s.colour_limit);
    if (key == "preset") return parse_uint(value, &s.preset);
    if (key == "style") return parse_uint(value, &s.style);
    if (key == "intensity") return parse_float(value, &s.intensity);
    if (key == "localstructure") return parse_float(value, &s.local_structure);
    if (key == "localtone") return parse_float(value, &s.local_tone);
    if (key == "skinstructure") return parse_float(value, &s.skin_structure);
    if (key == "automask") return parse_bool(value, &s.auto_mask);
    if (key == "noiseeveryframe") return parse_bool(value, &s.noise_every_frame);
    if (key == "transferstrength") return parse_float(value, &s.transfer_strength);
    if (key == "colourstrength") return parse_float(value, &s.colour_strength);
    if (key == "maxratio") return parse_float(value, &s.max_ratio);
    if (key == "reversiblemode") return parse_uint(value, &s.reversible_mode);
    if (key == "replacedetailstrength") return parse_float(value, &s.replace_detail_strength);
    if (key == "transfer") {   // 0 bilinear, 1 matched residual, 2 lighting + colour, 3 Edge-aware, 4 Classic
        uint32_t v = 1;
        if (!parse_uint(value, &v)) return false;
        s.transfer = v <= 4 ? v : 1u;
        return true;
    }
    if (key == "classicscaler") {   // Classic's scaler: bilinear, Catmull-Rom (the default), Lanczos3, FSR 1
        if (v == "bilinear" || v == "0") s.classic_scaler = 0;
        else if (v == "catmullrom" || v == "catmull-rom" || v == "1") s.classic_scaler = 1;
        else if (v == "lanczos" || v == "lanczos3" || v == "2") s.classic_scaler = 2;
        else if (v == "fsr1" || v == "fsr" || v == "3") s.classic_scaler = 3;
        else return false;
        return true;
    }
    if (key == "downscale") {   // below 100 %: 0 area after encoding (the original's), 1 bilinear before encoding
        if (v == "area" || v == "0") s.downscale = 0;
        else if (v == "bilinear" || v == "1") s.downscale = 1;
        else return false;
        return true;
    }
    if (key == "passesmode") {   // Passes (legacy) or Passes (fast)
        if (v == "legacy" || v == "0") s.passes_mode = 0;
        else if (v == "fast" || v == "1") s.passes_mode = 1;
        else return false;
        return true;
    }
    if (key == "fastmethod") {
        if (v == "extrapolation" || v == "0") s.fast_method = 0;
        else if (v == "feedback" || v == "1") s.fast_method = 1;
        else if (v == "hybrid" || v == "2") s.fast_method = 2;
        else if (v == "edit strength" || v == "editstrength" || v == "3") s.fast_method = 3;
        else return false;
        return true;
    }
    // each method's own: Fast<Method>Passes (1..8) / HaloLimit / Fine (0..2), <Method> Extrapolation, Feedback, Hybrid
    for (uint32_t m = 0; m < 3; ++m) {
        static const char* const names[] = {"fastextrapolation", "fastfeedback", "fasthybrid"};
        const size_t n = std::strlen(names[m]);
        if (key.compare(0, n, names[m]) != 0) continue;
        const std::string rest = key.substr(n);
        if (rest == "passes") {
            float f = 3.0f;
            if (!parse_float(value, &f)) return false;
            s.fast_passes[m] = finite(f) ? clampf(f, 1.0f, 8.0f) : 3.0f;
            return true;
        }
        if (rest == "halolimit") return parse_bool(value, &s.fast_halo_limit[m]);
        if (rest == "fine") {
            float f = m == 0 ? 0.f : 1.f;
            if (!parse_float(value, &f)) return false;
            s.fast_fine[m] = finite(f) ? clampf(f, 0.0f, 2.0f) : (m == 0 ? 0.f : 1.f);
            return true;
        }
    }
    if (key == "fasteditstrength") {   // 0..4 (Edit strength)
        float f = 2.0f;
        if (!parse_float(value, &f)) return false;
        s.fast_edit_strength = finite(f) ? clampf(f, 0.0f, 4.0f) : 2.0f;
        return true;
    }
    if (key == "whitepointscale") return parse_float(value, &s.white_point_scale);
    if (key == "whitepointsource") return parse_uint(value, &s.white_point_source);
    if (key == "whitepointtrim") return parse_float(value, &s.white_point_trim);
    if (key == "exposuretrimanchors") { s.exposure_trim_anchors = value; return true; }
    if (key == "autoexposuretrim") return parse_float(value, &s.auto_exposure_trim);
    if (key == "autoexposurehighlightprotection") return parse_float(value, &s.auto_exposure_highlight_protection);
    if (key == "autoexposuretrimanchors") { s.auto_exposure_trim_anchors = value; return true; }
    if (key == "skinprotection") return parse_bool(value, &s.skin_protection);
    if (key == "skindetail") return parse_float(value, &s.skin_detail);
    if (key == "skincolour") return parse_float(value, &s.skin_colour);
    if (key == "environmentdetail") return parse_float(value, &s.environment_detail);
    if (key == "environmentcolour") return parse_float(value, &s.environment_colour);
    return false;
}

// Spec 9.4: pass 1's values, a later pass's overrides, then preset and style capped, strengths sanitised and clamped.
PassControls pass_controls(const Settings& s, uint32_t k) {
    uint32_t preset = s.preset, style = s.style;
    float intensity = s.intensity, local_structure = s.local_structure, local_tone = k == 0 ? s.local_tone : 0.0f;
    float skin_structure = s.skin_structure;
    bool auto_mask = s.auto_mask;
    if (k >= 1 && k < kMaxPasses) {
        const PassOverride& o = s.pass[k];
        if (o.has_preset && k <= 2) preset = o.preset;
        if (o.has_style) style = o.style;
        if (o.has_intensity) intensity = o.intensity;
        if (o.has_local_structure) local_structure = o.local_structure;
        if (o.has_local_tone) local_tone = o.local_tone;
        if (o.has_skin_structure) skin_structure = o.skin_structure;
        if (o.has_auto_mask) auto_mask = o.auto_mask;
    }
    PassControls p;
    p.preset = std::min(preset, 3u);
    p.style = std::min(style, 2u);
    p.intensity = clampf(finite(intensity) ? intensity : 1.0f, 0.0f, 2.0f);
    p.local_structure = clampf(finite(local_structure) ? local_structure : 1.0f, 0.0f, 2.0f);
    p.local_tone = clampf(finite(local_tone) ? local_tone : (k == 0 ? 1.0f : 0.0f), 0.0f, 2.0f);
    p.skin_structure = clampf(finite(skin_structure) ? skin_structure : -1.0f, -1.0f, 2.0f);
    p.auto_mask = auto_mask;
    return p;
}

PassControls pass0_controls(const Settings& s) { return pass_controls(s, 0); }

float working_scale(const Settings& s) {
    return finite(s.working_scale) ? clampf(s.working_scale, 0.25f, 2.0f) : 1.0f;
}

void work_extent(float scale, uint32_t width, uint32_t height, uint32_t* work_width, uint32_t* work_height) {
    // the f32 product, then + 0.5, then the conversion to unsigned (truncation)
    const float w = float(width) * scale, h = float(height) * scale;
    *work_width = uint32_t(w + 0.5f);
    *work_height = uint32_t(h + 0.5f);
}

void work_extent(const Settings& s, float scale, uint32_t width, uint32_t height, uint32_t* work_width, uint32_t* work_height) {
    if (s.downscale != 1 || !(scale < 1.0f)) { work_extent(scale, width, height, work_width, work_height); return; }
    // Bilinear before encoding (the ReShade route's way): rounded to a multiple of 8, never above the frame
    const auto aligned = [scale](uint32_t v) {
        uint32_t m = uint32_t(std::lround(double(v) * scale));
        m = (m + 4) / 8 * 8;
        return std::min(std::max(m, 8u), v);
    };
    *work_width = aligned(width);
    *work_height = aligned(height);
}

// ---- peripheral compression: nr_wilsjo2.hpp (inline: the ReShade route's runtime uses it too) ----

uint32_t passes_requested(const Settings& s) {
    if (s.passes_mode == 1) return 1;   // Passes (fast): one network pass
    return std::clamp(s.passes, 1u, s.unlock_passes ? kMaxPasses : 3u);
}

// Spec 8.4.
void parse_anchors(const std::string& text, Constants& c) {
    std::vector<std::pair<float, float>> entries;
    std::istringstream in(text);
    while (entries.size() < 8) {
        float point = 0.f, trim = 0.f;
        char sep = 0;
        if (!(in >> point)) break;
        if (!(in >> sep)) break;
        if (!(in >> trim)) break;
        if (sep != ':' || !finite(point) || !finite(trim) || !(point > 0.f) || !(trim > 0.f)) continue;
        entries.emplace_back(point, clampf(trim, 0.001f, 1000.0f));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const std::pair<float, float>& a, const std::pair<float, float>& b) {
        return a.first < b.first || (a.first == b.first && a.second < b.second);
    });
    std::vector<std::pair<float, float>> kept;
    for (size_t i = 0; i < entries.size(); ++i)
        if (i + 1 == entries.size() || entries[i + 1].first != entries[i].first) kept.push_back(entries[i]);
    c.exposure_anchor_count = uint32_t(kept.size());
    for (size_t k = 0; k < kept.size(); ++k) {
        c.exposure_anchors[2 * k] = kept[k].first;
        c.exposure_anchors[2 * k + 1] = kept[k].second;
    }
}

// Spec 8.1.
void exposure_constants(const Settings& s, bool measure, float frame_pre_exposure, uint32_t width, uint32_t height,
                        Constants& c) {
    if (!measure) return;
    const uint32_t src = s.white_point_source;
    const bool automatic = src == 3;
    c.exposure_mode = src;
    c.pre_exposure = finite(frame_pre_exposure) && frame_pre_exposure > 1e-6f ? frame_pre_exposure : 1.0f;
    const float trim = automatic ? s.auto_exposure_trim : s.white_point_trim;
    c.exposure_trim = clampf(finite(trim) ? trim : 1.0f, 0.001f, 1000.0f);
    const float protection = s.auto_exposure_highlight_protection;
    c.exposure_protection = clampf(finite(protection) ? protection : 0.0f, 0.0f, 100.0f);
    parse_anchors(automatic ? s.auto_exposure_trim_anchors : s.exposure_trim_anchors, c);
    c.exposure_source_width = width;
    c.exposure_source_height = height;
}

// Spec 8.3 / 6.4: the meter is dispatched as 64 x 64 groups (Width and Height are its group counts), the automatic
// exposure as one group (Width = Height = 1).
Constants meter_constants(Constants c) {
    c.mode = kMeter;
    c.width = 64;
    c.height = 64;
    return c;
}

Constants auto_exposure_constants(Constants c) {
    c.mode = kAutoExposure;
    c.width = 1;
    c.height = 1;
    return c;
}

Constants encode_constants(const Settings& s, Constants c, bool passthrough, uint32_t width, uint32_t height) {
    c.mode = kEncode;
    c.passthrough = passthrough ? 1u : 0u;
    c.white_point = s.white_point_scale;
    c.reversible_mode = s.reversible_mode;
    c.width = width;
    c.height = height;
    return c;
}

// Spec 8.14.
Constants resolve_constants(const Settings& s, Constants c, bool passthrough, uint32_t width, uint32_t height,
                            float work_scale) {
    const auto unit = [](float v) { return clampf(finite(v) ? v : 1.0f, 0.0f, 1.0f); };
    c.mode = kResolve;
    c.white_point = s.white_point_scale;
    c.width = width;
    c.height = height;
    c.transfer_strength = s.transfer_strength;
    c.colour_strength = s.colour_strength;
    c.max_ratio = s.max_ratio;
    c.skin_protection = s.skin_protection ? 1u : 0u;
    c.skin_detail = unit(s.skin_detail);
    c.skin_colour = unit(s.skin_colour);
    c.environment_detail = unit(s.environment_detail);
    c.environment_colour = unit(s.environment_colour);
    c.debug_view = std::min(s.debug_view, 3u);
    c.show_skin_mask = s.show_skin_mask ? 1u : 0u;
    const uint32_t t = s.transfer;
    // Transfer 0 / 1 / 2 -> the shader's 0 / 1 / 3; ours 3 Edge-aware -> 5, 4 Classic -> 6
    c.transfer = t == 2 ? 3u : t == 3 ? 5u : t == 4 ? 6u : std::min(t, 1u);
    c.debug_scale = s.white_point_scale;
    c.passthrough = passthrough ? 1u : 0u;
    c.reversible_mode = s.reversible_mode;
    c.apply_model = 1;
    c.compare_zoom = 1.0f;
    c.replace_detail_strength = s.replace_detail_strength;
    c.model_work_scale = work_scale;
    c.colour_limit = s.colour_limit ? 1u : 0u;
    return c;
}

Constants hudfix_constants(uint32_t mode, uint32_t w, uint32_t h, uint32_t source, bool show_mask, bool fresh,
                           uint32_t parity) {
    Constants c{};
    c.passthrough = fresh ? 1u : 0u;                          // Fresh
    const uint32_t p = parity & 1u;
    std::memcpy(&c.max_ratio, &p, sizeof p);                  // Parity (a uint in that slot)
    c.mode = mode;
    c.width = w;
    c.height = h;
    c.debug_view = show_mask ? 1u : 0u;                       // ShowMask
    c.guide_width = uint32_t(0.25 * double(w) * double(h));   // HudLimit
    c.guide_height = 1u;                                      // Residual
    c.compare_mode = source == 4 ? 3u : source;               // HudMode
    c.compare_split = 1.0f / 510.0f;                          // Epsilon
    c.compare_zoom = 0.25f;                                   // Threshold
    return c;
}

void hudfix_motion(Constants& c, float scale_x, float scale_y, uint32_t x, uint32_t y, float step_x, float step_y) {
    c.mv_scale_x = scale_x;                                   // MvScaleX / Y
    c.mv_scale_y = scale_y;
    c.compare_swap = 1u;                                      // MotionOn
    c.transfer = (std::min(x, 0xffffu)) | (std::min(y, 0xffffu) << 16);   // MotionBase
    c.debug_scale = step_x;                                   // MotionStepX
    std::memcpy(&c.reversible_mode, &step_y, sizeof step_y);  // MotionStepY (a float in that slot)
}

Constants copy_constants(Mode mode, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    Constants c;
    c.mode = mode;
    c.width = w; c.height = h;
    c.guide_width = std::min(sx, 0xffffu) | (std::min(sy, 0xffffu) << 16);    // Source (wilsjo2_copy.comp)
    c.guide_height = std::min(dx, 0xffffu) | (std::min(dy, 0xffffu) << 16);   // Target
    return c;
}

// ---- the GPU half -------------------------------------------------------------------------------------------------
// A mode's name in NR_WILSJO2_PROF's log.
static const char* mode_name(uint32_t mode) {
    switch (mode) {
    case kEncode: return "encode"; case kResolve: return "resolve"; case kDownsample: return "downsample";
    case kMeter: return "meter"; case kClamp: return "clamp"; case kHalfCopy: return "half_copy";
    case kCopy: return "copy"; case kCopyLoad: return "copy_load";
    case kAutoExposure: return "autoexposure"; case kFinishedToLinear: return "finished_linear";
    case kFinishedToPq: return "finished_pq"; case kOsUp: return "os_up"; case kOsDown: return "os_down";
    case kOsEasu: return "os_easu"; case kClassicUp: return "classic"; case kInspect: return "inspect";
    case kLight: return "light"; case kPrepMeter: return "prep_meter"; case kPrepForward: return "prep_forward";
    case kPrepBack: return "prep_back"; case kHudClear: return "hud_clear"; case kHudCount: return "hud_count";
    case kHudEstimate: return "hud_estimate"; case kHudCompose: return "hud_compose";
    case kSpatialPack: return "spatial_pack"; case kSpatialGuides: return "spatial_guides";
    case kSpatialUnpack: return "spatial_unpack";
    default: return "other";
    }
}
struct Stage::Impl {
    VkDevice device{};
    VkPhysicalDevice physical{};
    bool pipeline_robustness{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkSampler sampler{};
    struct Program { VkShaderModule module{}; VkPipeline pipeline{}; };
    std::array<Program, 103> programs{};   // by Mode
    // Passes (fast)'s programs (nr::fast::Program order): their own set layouts from their SPIR-V
    struct FastProgram { VkShaderModule module{}; VkDescriptorSetLayout dsl{}; VkPipelineLayout layout{};
                         VkPipeline pipeline{}; std::vector<std::pair<uint32_t, VkDescriptorType>> bindings;
                         uint32_t push_bytes = 0; };
    std::array<FastProgram, nr::fast::kProgramCount> fast{};
    bool fast_loaded = false;
    // The views and sets of one recording, freed once 64 later recordings have passed AND 2 s (the game's image
    // handles can name another image once the game frees one, so nothing is kept across recordings). The recordings of
    // one frame are in command lists the game submits later: a fixed ring wrapped inside one frame with many passes and
    // layers (31 recordings a 30-pass layer) and reset sets still to run. The ring grows instead: the oldest slot is
    // taken only when free by both rules, else a new one goes in front of it; at kMaxSlots a recording that would need
    // one more is refused (record() throws: the frame is left as it came), never a slot still in use.
    struct Slot { nr::gpu::Ticket gpu; VkDescriptorPool pool{}, fast_pool{}; std::vector<VkImageView> views; uint64_t recording = ~0ull;
                  std::chrono::steady_clock::time_point used{};
                  VkQueryPool prof_pool{}; std::vector<const char*> prof_names; };   // NR_WILSJO2_PROF's
    // NR_WILSJO2_PROF (a measurement aid, as NR_RENODX_PROF): a timestamp before and after every dispatch of a
    // recording, read when its slot is taken again (its GPU work done); each step's time summed over the recordings
    // and logged every <n> recordings (=1: 300) as the mean a recording.
    struct Prof {
        uint64_t every = [] { const char* e = std::getenv("NR_WILSJO2_PROF"); const long v = e ? std::atol(e) : 0;
                              return uint64_t(v > 1 ? v : v == 1 ? 300 : 0); }();
        double ns = 1.0;
        uint64_t recordings = 0;
        std::vector<std::string> order;
        std::map<std::string, double> ms;
    } prof;
    static constexpr uint32_t kProfQueries = 512;
    void prof_take(Slot& s) {
        if (s.prof_names.empty()) return;
        std::vector<uint64_t> t(s.prof_names.size() * 2);
        if (vkGetQueryPoolResults(device, s.prof_pool, 0, uint32_t(t.size()), t.size() * 8, t.data(), 8,
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (size_t i = 0; i < s.prof_names.size(); ++i) {
                const std::string k = s.prof_names[i];
                if (!prof.ms.count(k)) prof.order.push_back(k);
                prof.ms[k] += double(t[2 * i + 1] - t[2 * i]) * prof.ns * 1e-6;
            }
            if (++prof.recordings % prof.every == 0) {
                std::string line = "[nr] wilsjo2 stage time a recording (ms):";
                double total = 0;
                for (const auto& k : prof.order) {
                    const double v = prof.ms[k] / double(prof.recordings);
                    total += v;
                    char b[96];
                    std::snprintf(b, sizeof b, " %s %.3f", k.c_str(), v);
                    line += b;
                }
                char b[64];
                std::snprintf(b, sizeof b, " total %.3f (%llu recordings)", total, (unsigned long long)prof.recordings);
                nr::logf("%s%s", line.c_str(), b);
                prof.ms.clear(); prof.order.clear(); prof.recordings = 0;
            }
        }
        s.prof_names.clear();
    }
    // before (`after` false) and after a dispatch of `name`
    void prof_mark(Slot& s, VkCommandBuffer cmd, const char* name, bool after) {
        if (!prof.every) return;
        if (!s.prof_pool) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kProfQueries;
            if (vkCreateQueryPool(device, &qi, nullptr, &s.prof_pool) != VK_SUCCESS) { s.prof_pool = VK_NULL_HANDLE; return; }
        }
        const uint32_t q = uint32_t(s.prof_names.size()) * 2 + (after ? 1u : 0u);
        if (q >= kProfQueries) return;
        if (!after) {
            if (s.prof_names.empty()) vkCmdResetQueryPool(cmd, s.prof_pool, 0, kProfQueries);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.prof_pool, q);
        } else {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.prof_pool, q);
            s.prof_names.push_back(name);
        }
    }
    static constexpr size_t kMaxSlots = 8192;
    static constexpr uint64_t kSlotRecordings = 64;
    static constexpr std::chrono::milliseconds kSlotTime{2000};
    std::vector<std::unique_ptr<Slot>> slots;   // a ring, oldest at `oldest`
    size_t oldest = 0;
    Slot* current = nullptr;
    bool said_full = false;
    uint64_t recordings = 0;

    void load(Mode mode, const std::string& path) {
        Program& p = programs[mode];
        const auto code = read_spirv(path);
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = code.size() * 4; si.pCode = code.data();
        check(vkCreateShaderModule(device, &si, nullptr, &p.module), "shader module");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = p.module; ci.stage.pName = "main";
        ci.layout = layout;
        VkPipelineRobustnessCreateInfoEXT robustness{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
        robustness.storageBuffers = robustness.uniformBuffers = robustness.vertexInputs =
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
        robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
        if (pipeline_robustness) ci.pNext = &robustness;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline), path.c_str());
    }

    void load_fast(nr::fast::Program which, const std::string& path, uint32_t push_bytes) {
        FastProgram& p = fast[which];
        const auto code = read_spirv(path);
        p.bindings = reflect_images(code);
        p.push_bytes = push_bytes;
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const auto& [b, type] : p.bindings) lb.push_back({b, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = uint32_t(lb.size()); li.pBindings = lb.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &p.dsl), "fast set layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &p.dsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(device, &pli, nullptr, &p.layout), "fast pipeline layout");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = code.size() * 4; si.pCode = code.data();
        check(vkCreateShaderModule(device, &si, nullptr, &p.module), "shader module");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = p.module; ci.stage.pName = "main";
        ci.layout = p.layout;
        VkPipelineRobustnessCreateInfoEXT robustness{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
        robustness.storageBuffers = robustness.uniformBuffers = robustness.vertexInputs =
            VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
        robustness.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
        if (pipeline_robustness) ci.pNext = &robustness;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline), path.c_str());
    }
    void fast_clear() {
        for (FastProgram& p : fast) {
            if (p.pipeline) vkDestroyPipeline(device, p.pipeline, nullptr);
            if (p.module) vkDestroyShaderModule(device, p.module, nullptr);
            if (p.layout) vkDestroyPipelineLayout(device, p.layout, nullptr);
            if (p.dsl) vkDestroyDescriptorSetLayout(device, p.dsl, nullptr);
            p = {};
        }
        fast_loaded = false;
    }

    Slot& slot() {
        if (current && current->recording == recordings) return *current;
        const auto now = std::chrono::steady_clock::now();
        Slot* s = nullptr;
        if (!slots.empty()) {
            Slot& o = *slots[oldest];
            const bool free = o.gpu ? o.gpu->ready() : recordings - o.recording >= kSlotRecordings && now - o.used >= kSlotTime;
            if (free) {
                s = &o;
                oldest = (oldest + 1) % slots.size();
            } else if (slots.size() >= kMaxSlots) {
                // never a slot still in use: this recording does not happen (the flow leaves the frame as it came)
                if (!said_full) {
                    said_full = true;
                    std::fprintf(stderr, "[nr] wilsjo2 stage: %zu recordings' sets within 2 s; recordings skipped\n", slots.size());
                }
                throw std::runtime_error("wilsjo2 stage: too many recordings in 2 s");
            }
        }
        if (!s) {   // a new slot, the youngest: in front of the oldest, which stays the oldest
            slots.insert(slots.begin() + long(oldest), std::make_unique<Slot>());
            s = slots[oldest].get();
            oldest = (oldest + 1) % slots.size();
        }
        for (VkImageView v : s->views) vkDestroyImageView(device, v, nullptr);
        s->views.clear();
        prof_take(*s);
        if (!s->pool) {   // the light, resample, meter and forward bind up to four sets
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * 8},
                                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 8}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 8; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
            check(vkCreateDescriptorPool(device, &pi, nullptr, &s->pool), "descriptor pool");
        } else {
            vkResetDescriptorPool(device, s->pool, 0);
        }
        if (s->fast_pool) vkResetDescriptorPool(device, s->fast_pool, 0);
        s->gpu = nr::gpu::current();
        s->recording = recordings;
        s->used = now;
        current = s;
        return *s;
    }

    VkImageView view(Slot& s, const Image& im, VkImageUsageFlags usage) {
        if (im.view && usage == VK_IMAGE_USAGE_SAMPLED_BIT) return im.view;   // the caller's (not ours to free)
        VkImageViewUsageCreateInfo ui{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        ui.usage = usage;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &ui};
        vi.image = im.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = im.format;
        // a depth guide (the peripheral compression's guide pass reads the game's depth) through its depth aspect
        const bool depth = im.format == VK_FORMAT_D32_SFLOAT || im.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                           im.format == VK_FORMAT_D24_UNORM_S8_UINT || im.format == VK_FORMAT_D16_UNORM ||
                           im.format == VK_FORMAT_X8_D24_UNORM_PACK32 || im.format == VK_FORMAT_D16_UNORM_S8_UINT;
        vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), im.mip, 1,
                               im.layer, 1};
        VkImageView v{};
        check(vkCreateImageView(device, &vi, nullptr, &v), "image view");
        s.views.push_back(v);
        return v;
    }
};

namespace {
// a program partly loaded when a later one of its group failed: none of the group
void programs_clear(Stage::Impl& m, Mode mode) {
    auto& p = m.programs[mode];
    if (p.pipeline) vkDestroyPipeline(m.device, p.pipeline, nullptr);
    if (p.module) vkDestroyShaderModule(m.device, p.module, nullptr);
    p = {};
}
}  // namespace

Stage::Stage(const HostDevice& host, const std::string& shaders) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.device = host.device;
    m.physical = host.physical;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(host.physical, &props);
        m.prof.ns = props.limits.timestampPeriod;
    }
    {
        VkPhysicalDevicePipelineRobustnessFeaturesEXT prf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &prf};
        vkGetPhysicalDeviceFeatures2(host.physical, &f2);
        m.pipeline_robustness = prf.pipelineRobustness != 0;
    }
    VkDescriptorSetLayoutBinding b[6]{};
    for (uint32_t i = 0; i < 6; ++i)
        b[i] = {i, i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6; li.pBindings = b;
    check(vkCreateDescriptorSetLayout(m.device, &li, nullptr, &m.dsl), "set layout");
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &m.dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    check(vkCreatePipelineLayout(m.device, &pli, nullptr, &m.layout), "pipeline layout");
    // Spec 1.2: bilinear, mip-linear, clamp to the edge on all three axes.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    check(vkCreateSampler(m.device, &sci, nullptr, &m.sampler), "sampler");
    m.load(kEncode, shaders + "/wilsjo2_encode.spv");
    m.load(kResolve, shaders + "/wilsjo2_resolve.spv");
    m.load(kMeter, shaders + "/wilsjo2_meter.spv");
    m.load(kClamp, shaders + "/wilsjo2_clamp.spv");
    m.load(kDownsample, shaders + "/wilsjo2_downsample.spv");
    // The programs of features a layer may not use are optional (0.0.7 made its peripheral compression's on demand):
    // without them that feature does not run (flow_run / the Present conversion say so), plain wilsjo2 does.
    try {   // WorkingScale above 1 (supersampling): without them a layer set so skips its frames
        for (Mode mode : {kOsUp, kOsDown, kOsEasu}) m.load(mode, shaders + "/wilsjo2_os.spv");
    } catch (const std::exception&) {
        for (Mode mode : {kOsUp, kOsDown, kOsEasu}) programs_clear(m, mode);
    }
    try {   // peripheral compression: without them the whole picture
        for (Mode mode : {kSpatialPack, kSpatialUnpack}) m.load(mode, shaders + "/wilsjo2_spatial.spv");
        m.load(kSpatialGuides, shaders + "/wilsjo2_spatial_guides.spv");
    } catch (const std::exception&) {
        for (Mode mode : {kSpatialPack, kSpatialUnpack, kSpatialGuides}) programs_clear(m, mode);
    }
    try {   // WhitePointSource 3 (automatic exposure): without it the manual white point
        m.load(kAutoExposure, shaders + "/wilsjo2_autoexposure.spv");
    } catch (const std::exception&) {
        programs_clear(m, kAutoExposure);
    }
    try {   // the Present placement's finished-picture conversion: without it wilsjo2 does not run at Present
        m.load(kFinishedToLinear, shaders + "/wilsjo2_finished.spv");
        m.load(kFinishedToPq, shaders + "/wilsjo2_finished.spv");
    } catch (const std::exception&) {
        for (Mode mode : {kFinishedToLinear, kFinishedToPq}) programs_clear(m, mode);
    }
    try {   // Optional programs: a layer with active Preprocess skips frames when these are unavailable.
        m.load(kLight, shaders + "/wilsjo2_light.spv");
        m.load(kPrepMeter, shaders + "/wilsjo2_prep_meter.spv");
        m.load(kPrepForward, shaders + "/wilsjo2_prep.spv");
        m.load(kPrepBack, shaders + "/wilsjo2_prep.spv");
    } catch (const std::exception&) {
        for (Mode mode : {kLight, kPrepMeter, kPrepForward, kPrepBack}) programs_clear(m, mode);
    }
    try {   // Enlargement Classic: optional (without it Classic composes the bilinear pair, as Transfer 0)
        m.load(kClassicUp, shaders + "/wilsjo2_classic.spv");
    } catch (const std::exception&) {
        programs_clear(m, kClassicUp);
    }
    try {   // Passes (fast): optional (without them a fast layer runs its one network pass, and says so)
        static const uint32_t push[] = {sizeof(nr::fast::FbInPush), sizeof(nr::fast::PrepPush), sizeof(nr::fast::BoxPush),
                                        sizeof(nr::fast::ApplyPush)};
        for (uint32_t k = 0; k < nr::fast::kProgramCount; ++k)
            m.load_fast(nr::fast::Program(k), shaders + "/wilsjo2_" + nr::fast::program_name(nr::fast::Program(k)) + ".spv",
                        push[k]);
        m.load(kHalfCopy, shaders + "/wilsjo2_half_copy.spv");
        m.fast_loaded = true;
    } catch (const std::exception&) {
        m.fast_clear();
        programs_clear(m, kHalfCopy);
    }
    try {   // the .asi's Vulkan copies: optional (without them a copy a transfer cannot make does not happen)
        m.load(kCopy, shaders + "/wilsjo2_copy.spv");
        m.load(kCopyLoad, shaders + "/wilsjo2_copy_load.spv");
    } catch (const std::exception&) {
        programs_clear(m, kCopy);
        programs_clear(m, kCopyLoad);
    }
    try {   // NR Control's comparison: optional (without it the comparison is refused, the style runs)
        m.load(kInspect, shaders + "/wilsjo2_inspect.spv");
    } catch (const std::exception&) {
    }
    try {   // HUD FIX: optional too (without it HUD FIX says it is unavailable)
        m.load(kHudClear, shaders + "/wilsjo2_hudfix_clear.spv");
        m.load(kHudCount, shaders + "/wilsjo2_hudfix_count.spv");
        m.load(kHudEstimate, shaders + "/wilsjo2_hudfix_estimate.spv");
        m.load(kHudCompose, shaders + "/wilsjo2_hudfix_compose.spv");
    } catch (const std::exception&) {
    }
}

Stage::~Stage() {
    Impl& m = *impl_;
    for (auto& s : m.slots) {
        for (VkImageView v : s->views) vkDestroyImageView(m.device, v, nullptr);
        if (s->pool) vkDestroyDescriptorPool(m.device, s->pool, nullptr);
        if (s->fast_pool) vkDestroyDescriptorPool(m.device, s->fast_pool, nullptr);
        if (s->prof_pool) vkDestroyQueryPool(m.device, s->prof_pool, nullptr);
    }
    m.fast_clear();
    for (auto& p : m.programs) {
        if (p.pipeline) vkDestroyPipeline(m.device, p.pipeline, nullptr);
        if (p.module) vkDestroyShaderModule(m.device, p.module, nullptr);
    }
    if (m.sampler) vkDestroySampler(m.device, m.sampler, nullptr);
    if (m.layout) vkDestroyPipelineLayout(m.device, m.layout, nullptr);
    if (m.dsl) vkDestroyDescriptorSetLayout(m.device, m.dsl, nullptr);
}

void Stage::record(VkCommandBuffer cmd, const Dispatch& d) {
    Impl& m = *impl_;
    const uint32_t mode = d.constants.mode;
    if (mode >= m.programs.size() || !m.programs[mode].pipeline) throw std::runtime_error("wilsjo2 stage: no program for mode " + std::to_string(mode));
    if (!d.t0 || !d.u0) throw std::runtime_error("wilsjo2 stage: a dispatch without t0 or u0");
    Impl::Slot& s = m.slot();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &m.dsl;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(m.device, &ai, &set), "descriptor set");
    // Spec 6.3: a read slot without a resource is given t0's, a missing u1 is given u0's.
    const Image* reads[4] = {&d.t0, d.t1 ? &d.t1 : &d.t0, d.t2 ? &d.t2 : &d.t0, d.t3 ? &d.t3 : &d.t0};
    const Image* writes[2] = {&d.u0, d.u1 ? &d.u1 : &d.u0};
    VkDescriptorImageInfo info[6]{};
    VkWriteDescriptorSet w[6]{};
    for (uint32_t i = 0; i < 6; ++i) {
        const Image& im = i < 4 ? *reads[i] : *writes[i - 4];
        info[i].imageView = m.view(s, im, i < 4 ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
        info[i].imageLayout = im.layout;
        info[i].sampler = i < 4 ? m.sampler : VK_NULL_HANDLE;
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(m.device, 6, w, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.programs[mode].pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants), &d.constants);
    m.prof_mark(s, cmd, mode_name(mode), false);
    // Spec 6.4: 8x8 groups over Width x Height; the meter one group a tile, Width x Height groups.
    if (mode == kMeter) vkCmdDispatch(cmd, d.constants.width, d.constants.height, 1);
    else if (mode == kPrepMeter) vkCmdDispatch(cmd, 1, 1, 1);   // one 16 x 16 group over the whole image
    else vkCmdDispatch(cmd, (d.constants.width + 7) / 8, (d.constants.height + 7) / 8, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    m.prof_mark(s, cmd, mode_name(mode), true);
}

bool Stage::has_fast() const { return impl_->fast_loaded; }

void Stage::record_fast(VkCommandBuffer cmd, uint32_t program, const FastImage* images, uint32_t count, const void* push,
                        uint32_t bytes, uint32_t gx, uint32_t gy) {
    Impl& m = *impl_;
    if (!m.fast_loaded || program >= nr::fast::kProgramCount) throw std::runtime_error("wilsjo2 stage: no fast program");
    const Impl::FastProgram& pr = m.fast[program];
    Impl::Slot& s = m.slot();
    if (!s.fast_pool) {   // a frame's fast work: up to 7 sets
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5 * 24}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 24}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 24; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(m.device, &pi, nullptr, &s.fast_pool), "fast descriptor pool");
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.fast_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pr.dsl;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(m.device, &ai, &set), "fast descriptor set");
    std::vector<VkDescriptorImageInfo> info(pr.bindings.size());
    std::vector<VkWriteDescriptorSet> w(pr.bindings.size());
    for (size_t i = 0; i < pr.bindings.size(); ++i) {
        const auto [binding, type] = pr.bindings[i];
        const Image* im = nullptr;
        for (uint32_t j = 0; j < count; ++j)
            if (images[j].binding == binding) im = &images[j].image;
        if (!im || !*im) throw std::runtime_error("wilsjo2 stage: a fast program's image is missing");
        const bool sampled = type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        info[i].imageView = m.view(s, *im, sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
        info[i].imageLayout = im->layout;
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set; w[i].dstBinding = binding; w[i].descriptorCount = 1;
        w[i].descriptorType = type;
        w[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(m.device, uint32_t(w.size()), w.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pr.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pr.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, pr.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, std::min(bytes, pr.push_bytes), push);
    m.prof_mark(s, cmd, nr::fast::program_name(nr::fast::Program(program)), false);
    vkCmdDispatch(cmd, gx, gy, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    m.prof_mark(s, cmd, nr::fast::program_name(nr::fast::Program(program)), true);
}

void Stage::end_recording() { ++impl_->recordings; }

bool Stage::has(Mode mode) const { return mode < impl_->programs.size() && impl_->programs[mode].pipeline; }

}  // namespace nr::wilsjo2
