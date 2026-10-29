#pragma once
// The menus' engine-data panel: what the network actually gets - the colour as it is encoded for it and by whom, the
// exposure in effect with its value, the motion vectors as they are read - and, for what the game does not hand over,
// who covers it. NR Control (the OptiScaler routes, through nr_pe_control / control.bin) and the ReShade add-on's page
// draw the same lines (split). Every line starts with its state, the colour of its label: '+' from the game or not
// needed, '~' covered by something else (a meter, Preprocess, a ReShade effect, a setting), '-' missing with nothing
// covering it, '=' information; '!' is the hint line (the game gives no exposure and nothing covers it), all in colour.
#include "nr_control_shared.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nr::pe::panel {

struct Facts {
    bool reshade = false;            // the ReShade route (NR on the finished picture), else OptiScaler's
    bool renodx = false;             // the RenoDX-style option runs
    // The colour
    std::string format;              // the frame's format, by name
    int hdr = -1;                    // -1 not known, 0 SDR, 1 HDR (linear light), 2 HDR10 (PQ), 3 scRGB
    std::string hdr_why;             // what says so ("the swap chain's colour space", ...)
    // OptiScaler's own encode as it ran (renodx_route::encode_seen): -1 not seen, 0 the frame passed through as it is
    // (already tone mapped), 1 linear light divided by encode_white
    int encode = -1;
    float encode_white = 0.f;
    // The OptiScaler routes: the game's DLSS evaluate as OptiScaler handed it on (its frame seen: frame_known)
    bool frame_known = false;
    bool auto_exposure = false;      // the game created its DLSS with AutoExposure
    int exposure_texture = -1;       // -1 not known, 0 none, 1 given (RenoDX style: readable), 2 given, a format not read
    int white_point_source = -1;     // OptiScaler.ini [DlssNr] WhitePointSource (-1 not read)
    // The RenoDX-style option's last frame (renodx::Stage::Facts)
    bool renodx_known = false;
    int renodx_hdr = 0;              // 0 SDR, 1 linear HDR, 2 PQ
    bool renodx_absolute = false;
    int renodx_feed = -1;            // -1 none, 0 the content meter, 1 the game's texture, 2 fixed
    float renodx_divisor = 0.f;      // the divisor applied, 0 not known
    // [Preprocess]: 0 its exposure off (or Preprocess off), 1 auto, 2 fixed; its gain in EV (NaN: not metered yet)
    int preprocess_exposure = 0;
    float preprocess_ev = NAN;
    float white_point = 0.f;         // the ReShade route's [DlssNr] WhitePoint (linear frames), 0 not used
    // Motion vectors
    int motion = -1;                 // -1 not known, 0 none, 1 given
    uint32_t motion_width = 0, motion_height = 0;
    bool motion_nearest = false;     // read at the nearest texel (as NVIDIA's DLL), else bilinear
    float mv_scale_x = 0.f, mv_scale_y = 0.f;   // the scale the caller gave, 0 not known
    std::string motion_source;       // the ReShade route: the effect that makes them
    // Depth: shown only when the network reads it (Motion Without Depth = 0); NVIDIA's DLL never does
    int depth = -1;                  // -1 not known, 0 none, 1 given
    bool motion_without_depth = true;
    // Sizes and resets
    uint64_t resizes = 0, carried = 0, resets = 0;
    // The .asi's wilsjo2 processing (renodx_route::Wilsjo2Facts)
    bool wilsjo2 = false;
    bool wilsjo2_hdr = false;
    int wilsjo2_exposure = 0;        // 0 manual white point, 1 the game's exposure texture, 3 metered
    uint32_t wilsjo2_asked = 0;      // WhitePointSource
    float wilsjo2_white = 1.0f;      // WhitePointScale
    uint32_t wilsjo2_mode = 0;       // ReversibleMode
};

inline std::string ev(float gain) {   // a gain as EV, "+1.25 EV"
    char b[32];
    std::snprintf(b, sizeof b, "%+.2f EV", double(std::log2(gain)));
    return b;
}

inline std::string exposure_line(const Facts& f, bool* hint) {
    *hint = false;
    char b[224];
    if (f.reshade) {
        if (f.preprocess_exposure == 1 && f.preprocess_ev == f.preprocess_ev) {
            std::snprintf(b, sizeof b, "~Exposure: Preprocess auto %+.2f EV on the finished picture", double(f.preprocess_ev));
            return b;
        }
        return "+Exposure: not needed - NR works on the finished picture (the game's exposure and tone mapping are in it)";
    }
    if (f.wilsjo2) {
        if (!f.wilsjo2_hdr) return "+Exposure: not needed - an SDR picture is already exposed by the game";
        if (f.wilsjo2_exposure == 1) return "+Exposure: the wilsjo2 encode's white point, from the game's exposure texture";
        if (f.wilsjo2_exposure == 3)
            return "~Exposure: the wilsjo2 automatic exposure, metered from the frame (WhitePointSource = 3)";
        std::snprintf(b, sizeof b, "a manual white point %.3g (WhitePointScale)", double(f.wilsjo2_white));
        if (f.wilsjo2_asked == 1) {
            *hint = true;
            return std::string("-Exposure: ") + b + " - WhitePointSource = 1, but the game gives no exposure texture";
        }
        return std::string("~Exposure: ") + b;
    }
    std::string prep;
    if (f.preprocess_exposure == 1)
        prep = f.preprocess_ev == f.preprocess_ev ? "Preprocess auto exposure, " + ev(std::exp2(f.preprocess_ev)) : "Preprocess auto exposure";
    else if (f.preprocess_exposure == 2)
        prep = "Preprocess fixed exposure";
    if (f.renodx && f.renodx_known) {
        if (f.renodx_hdr == 0) return "+Exposure: not needed - an SDR picture is already exposed by the game";
        if (f.renodx_absolute) return "+Exposure: not needed - the picture is in absolute units (nits)";
        if (!prep.empty()) return "~Exposure: " + prep + " (replaces RenoDX style's divisor)";
        std::string value;
        if (f.renodx_divisor > 0.f) {
            std::snprintf(b, sizeof b, "RenoDX style divides the frame by %.3g (%s)", double(f.renodx_divisor),
                          ev(1.f / f.renodx_divisor).c_str());
            value = b;
        } else {
            value = "RenoDX style divides the frame by a measured value";
        }
        switch (f.renodx_feed) {
            case 1: return "+Exposure: " + value + ", from the game's exposure texture";
            case 0:
                if (f.exposure_texture == 0) return "~Exposure: " + value + ", from its content meter - the game gives none";
                if (f.exposure_texture == 2) return "~Exposure: " + value + ", from its content meter - the game's texture is a format it does not read";
                if (f.auto_exposure) return "~Exposure: " + value + ", from its content meter - the game asks for auto exposure";
                return "~Exposure: " + value + ", from its content meter (NRFeedMode)";
            case 2:
                *hint = true;
                return "-Exposure: " + value + " - fixed (NRFeedMode = 2); the game gives no exposure texture";
            default: break;
        }
        return "=Exposure: not known yet";
    }
    // the plain OptiScaler route: OptiScaler's encode decides
    if (f.encode == 0) return "+Exposure: not needed - OptiScaler passes the frame on as already tone mapped";
    std::string value = "OptiScaler encodes it";
    if (f.encode == 1 && f.encode_white > 0.f) {
        std::snprintf(b, sizeof b, "OptiScaler's encode divides by white point %.3g (%s)", double(f.encode_white),
                      ev(1.f / f.encode_white).c_str());
        value = b;
    }
    if (!prep.empty()) return "~Exposure: " + prep + ", then " + value;
    if (!f.frame_known || f.exposure_texture < 0) return "=Exposure: " + value + " (where its white point comes from is not known)";
    if (f.exposure_texture > 0) {
        if (f.white_point_source == 0) return "~Exposure: " + value + " - a fixed paper white (WhitePointSource = 0), the game's exposure not used";
        return "+Exposure: " + value + ", from the game's exposure texture";
    }
    *hint = true;
    return "-Exposure: " + value + " - a fixed white point: the game gives no exposure" +
           (f.auto_exposure ? " (it asks for auto exposure)" : "");
}

inline std::vector<std::string> lines(const Facts& f) {
    std::vector<std::string> out;
    char b[224];
    // The colour, and what the network is shown
    {
        const char* kind = f.hdr == 0 ? "SDR" : f.hdr == 1 ? "HDR (linear light)" : f.hdr == 2 ? "HDR10 (PQ)"
                           : f.hdr == 3 ? "scRGB" : "not known";
        std::snprintf(b, sizeof b, "=Colour: %s, %s%s%s%s", f.format.empty() ? "format not known" : f.format.c_str(), kind,
                      f.hdr_why.empty() ? "" : " (", f.hdr_why.c_str(), f.hdr_why.empty() ? "" : ")");
        out.push_back(b);
        std::string shown;
        static const char* const kModes[] = {"soft knee", "Neutwo", "Neutwo, replace", "Hybrid", "Hybrid, replace"};
        if (f.wilsjo2) {
            shown = f.wilsjo2_hdr ? std::string("linear light through the wilsjo2 encode's ") + kModes[f.wilsjo2_mode % 5] +
                                        " curve"
                                  : "the picture as it is (SDR), the wilsjo2 encode";
        } else if (f.renodx && f.renodx_known) {
            shown = f.renodx_hdr == 0 ? "the picture as it is (SDR), through RenoDX style's legacy codec"
                    : f.renodx_hdr == 2 ? "PQ decoded to linear light, RenoDX style's v6 encode"
                    : f.renodx_absolute ? "linear light in absolute units, RenoDX style's v6 encode"
                    : "linear light divided as below, RenoDX style's v6 encode";
        } else if (f.reshade) {
            shown = f.hdr == 3 && f.white_point > 0.f ? "linear light, encoded by this add-on (WhitePoint " +
                    std::to_string(f.white_point).substr(0, 5) + ")"
                    : f.hdr == 2 && f.white_point > 0.f ? "HDR10 (PQ) decoded to linear light, encoded by this add-on "
                                                          "(WhitePoint " + std::to_string(f.white_point).substr(0, 5) + ")"
                    : "the picture as it is";
        } else if (f.encode == 1) {
            shown = "linear light encoded by OptiScaler's DLSS-NR encode";
        } else if (f.encode == 0) {
            shown = "the frame as it is (OptiScaler: already tone mapped)";
        } else {
            shown = "OptiScaler's encode (not seen yet)";
        }
        out.push_back("=The network sees: " + shown);
    }
    bool hint = false;
    out.push_back(exposure_line(f, &hint));
    // Motion
    if (f.reshade) {
        if (f.motion == 1)
            std::snprintf(b, sizeof b, "~Motion vectors: not from the game - made by %s (DLSS5_Feed), read bilinear",
                          f.motion_source.empty() ? "a ReShade effect" : f.motion_source.c_str());
        else if (f.motion == 0)
            std::snprintf(b, sizeof b, "-Motion vectors: none (DLSS5_Feed missing) - NR runs with no motion");
        else
            std::snprintf(b, sizeof b, "=Motion vectors: not known yet");
    } else if (f.motion == 1) {
        char scale[64] = "";
        if (f.mv_scale_x != 0.f || f.mv_scale_y != 0.f)
            std::snprintf(scale, sizeof scale, ", scale %.4g x %.4g", double(f.mv_scale_x), double(f.mv_scale_y));
        std::snprintf(b, sizeof b, "+Motion vectors: from the game, %ux%u, read at the %s%s", f.motion_width, f.motion_height,
                      f.motion_nearest ? "nearest texel (as NVIDIA's DLL)" : "bilinear", scale);
    } else {
        std::snprintf(b, sizeof b, "%sMotion vectors: %s", f.motion == 0 ? "-" : "=", f.motion == 0 ? "none from the game" : "not known");
    }
    out.push_back(b);
    // Depth: NVIDIA's DLL never reads it; a line only when ours does (Motion Without Depth = 0)
    if (!f.motion_without_depth)
        out.push_back(f.depth == 1 ? "+Depth: read for the motion lookup (Motion Without Depth = 0; NVIDIA's DLL does not)"
                                   : "-Depth: none, though Motion Without Depth = 0 asks for it");
    // Size changes (dynamic resolution) and resets
    if (f.resizes) {
        std::snprintf(b, sizeof b, "=Size changes: %llu (%s)", static_cast<unsigned long long>(f.resizes),
                      f.carried ? "history carried over, Resize History = 1" : "history restarted each time, as NVIDIA's DLL");
    } else {
        std::snprintf(b, sizeof b, "=Size changes: none");
    }
    out.push_back(b);
    if (!f.reshade) {
        std::snprintf(b, sizeof b, "=Resets from the game: %llu", static_cast<unsigned long long>(f.resets));
        out.push_back(b);
    }
    if (hint && f.wilsjo2)
        out.push_back(std::string(nr::control::kNoExposureHint) +
                      " If the picture looks wrong, try White point source = Automatic");
    else if (hint)
        out.push_back(std::string(nr::control::kNoExposureHint) +
                      " If the picture looks wrong, try Preprocess with Exposure = auto");
    return out;
}

// The colour of a line's state ('+', '~', '-', '!'; '=' and anything else: none), RGBA.
inline bool state_colour(char c, float rgba[4]) {
    const float* v = nullptr;
    static const float good[4] = {.45f, .85f, .45f, 1.f}, covered[4] = {.95f, .85f, .35f, 1.f},
                       missing[4] = {1.f, .55f, .3f, 1.f}, hint[4] = {1.f, .75f, .3f, 1.f};
    switch (c) {
        case '+': v = good; break;
        case '~': v = covered; break;
        case '-': v = missing; break;
        case '!': v = hint; break;
        default: return false;
    }
    for (int i = 0; i < 4; ++i) rgba[i] = v[i];
    return true;
}

// One line split for drawing: the label up to its ':' in the state's colour, the rest in the text colour; the hint
// line all in colour (whole); '=' lines, and lines without a label, plain (coloured false).
struct Drawn {
    bool coloured = false, whole = false;
    float rgba[4] = {};
    std::string label, rest;
};
inline Drawn split(const std::string& l) {
    Drawn d;
    if (l.empty()) return d;
    d.coloured = state_colour(l[0], d.rgba);
    const std::string body = d.coloured || l[0] == '=' ? l.substr(1) : l;
    const size_t colon = body.find(':');
    if (l[0] == '!' || (d.coloured && colon == std::string::npos)) {
        d.whole = d.coloured;
        d.rest = body;
    } else if (d.coloured) {
        d.label = body.substr(0, colon + 1);
        d.rest = body.substr(colon + 1);
    } else {
        d.rest = body;
    }
    return d;
}

// A format by name: DXGI_FORMAT (the D3D12 route) or VkFormat (the Vulkan routes); the number when not listed.
inline std::string dxgi_format_name(uint32_t f) {
    switch (f) {
        case 2: return "R32G32B32A32_FLOAT";
        case 6: return "R32G32B32_FLOAT";
        case 9: return "R16G16B16A16_TYPELESS";
        case 10: return "R16G16B16A16_FLOAT";
        case 11: return "R16G16B16A16_UNORM";
        case 23: return "R10G10B10A2_TYPELESS";
        case 24: return "R10G10B10A2_UNORM";
        case 26: return "R11G11B10_FLOAT";
        case 27: return "R8G8B8A8_TYPELESS";
        case 28: return "R8G8B8A8_UNORM";
        case 29: return "R8G8B8A8_UNORM_SRGB";
        case 67: return "R9G9B9E5_SHAREDEXP";
        case 87: return "B8G8R8A8_UNORM";
        case 90: return "B8G8R8A8_TYPELESS";
        case 91: return "B8G8R8A8_UNORM_SRGB";
        default: return "DXGI format " + std::to_string(f);
    }
}
inline std::string vk_format_name(uint32_t f) {
    switch (f) {
        case 37: return "R8G8B8A8_UNORM";
        case 43: return "R8G8B8A8_SRGB";
        case 44: return "B8G8R8A8_UNORM";
        case 50: return "B8G8R8A8_SRGB";
        case 58: return "A2R10G10B10_UNORM";
        case 64: return "A2B10G10R10_UNORM";
        case 91: return "R16G16B16A16_UNORM";
        case 97: return "R16G16B16A16_SFLOAT";
        case 109: return "R32G32B32A32_SFLOAT";
        case 122: return "B10G11R11_UFLOAT";
        case 123: return "E5B9G9R9_UFLOAT";
        default: return "VkFormat " + std::to_string(f);
    }
}
// Whether a format holds linear light (a float format), for the HDR judgement where no flag says.
inline bool dxgi_float(uint32_t f) { return f == 2 || f == 6 || f == 10 || f == 26 || f == 67; }
inline bool vk_float(uint32_t f) { return f == 97 || f == 109 || f == 122 || f == 123; }

}  // namespace nr::pe::panel
