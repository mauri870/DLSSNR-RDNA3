// RenoDX-style processing: see nr_renodx.hpp.
//
// The host logic below follows the RenoDX DLSS5 add-on v8.5.0-rc10 (PEQHUB/RenoDX-DLSS5-Generic,
// MIT, renodx/LICENSE) line for line where it decides what the shaders are given: dlssnr.hpp
// FrameHdrMode, InterpretSourceUnits, V6EncodingConstant, V6DarkGate, FrameDisplayCodec,
// FramePedestalRemoval, FrameCodecDivisor, MakeV6Constants, PlanFrameScale, RunBlackLevelRestore,
// LookSettingsSnapshot, PlanLookHistory and FinishLookPass, look_stage.hpp Settings / Constants /
// MakeConstants / RecordCompose / RecordUpsample, ChainActive, and the dispatch order of its D3D12 after path
// (ProcessInline: the stack loop, RunLookStage, the write-back). codec_gain.hpp is
// included as it is. The shaders are RenoDX's, compiled for Vulkan as RenoDX compiles them.
#include "nr_renodx.hpp"
#include "nr_log.hpp"
#include "renodx/codec_gain.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace nr::renodx {
namespace {

namespace codec = ::renodx::dlss5::codec;

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("RenoDX stage: ") + what + " " + std::to_string(r));
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- RenoDX host logic (dlssnr.hpp), on a Settings value instead of its globals ------------------

// InferHdrMode / FrameHdrMode, on Vulkan formats.
uint8_t infer_hdr_mode(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return 1;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return 2;
        default:
            return 0;
    }
}
uint8_t frame_hdr_mode(VkFormat f, bool hdr_flag) {
    if (hdr_flag) return std::max<uint8_t>(infer_hdr_mode(f), 1u);
    if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32) return 0;
    return infer_hdr_mode(f);
}

codec::SourceUnits interpret_source_units(const Settings& s, uint8_t hdr_mode) {
    codec::SourceUnits units;
    uint8_t choice = 0;
    if (s.source_encoding != 0u) choice = uint8_t(s.source_encoding);
    else if (hdr_mode == 2u) choice = 3u;
    else if (hdr_mode == 1u) choice = 2u;
    switch (choice) {
        case 1u: units.encoding = codec::Encoding::Sdr; break;
        case 2u: units.encoding = codec::Encoding::Linear; break;
        case 3u: units.encoding = codec::Encoding::Pq; units.absolute = true; units.unit_nits = 10000.f; break;
        default: units.encoding = codec::Encoding::Linear; break;
    }
    if (units.encoding == codec::Encoding::Linear && s.linear_unit_nits > 0.f) {
        units.absolute = true;
        units.unit_nits = s.linear_unit_nits;
    }
    return units;
}

uint32_t v6_encoding_constant(const codec::SourceUnits& u) {
    switch (u.encoding) {
        case codec::Encoding::Pq: return u.bt2020 ? 4u : 2u;
        case codec::Encoding::Sdr: return 3u;
        default: return 1u;
    }
}

float v6_dark_gate(const codec::SourceUnits& u) {
    return u.absolute && u.unit_nits > 0.f ? 1.f / u.unit_nits : 0.005f;
}

bool frame_display_codec(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u) {
    if (hdr_mode == 0) return false;
    if (s.codec_mode == 3u) return true;
    if (s.codec_mode == 2u) return !(u.absolute && u.unit_nits > 0.f);
    return false;
}

bool frame_pedestal_removal(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u) {
    if (!frame_display_codec(s, hdr_mode, u)) return true;
    if (s.display_pedestal == 1u) return true;
    return u.absolute && u.unit_nits > 0.f;
}

float frame_codec_divisor(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u) {
    if (hdr_mode == 0) return 1.f;
    if (!u.absolute || !(u.unit_nits > 0.f)) return 0.f;
    if (frame_display_codec(s, hdr_mode, u))
        return codec::DisplayAnchorLinear(u, s.proxy_anchor_nits, s.diffuse_white_nits);
    if (s.codec_mode == 1u) return codec::AnchoredDivisor(s.proxy_anchor_nits, u);
    return u.encoding == codec::Encoding::Pq
        ? codec::ClassicPqDivisor(s.diffuse_white_nits, s.paper_white_scale, s.pq_calibration)
        : codec::ClassicLinearDivisor(s.paper_white_scale);
}

// cbuffer CodecConstants of v6_common.hlsli (V6CodecConstants).
struct V6Constants {
    uint32_t width, height, source_width, source_height, source_base_x, source_base_y;
    uint32_t proxy_width, proxy_height, neural_width, neural_height;
    float divisor, pedestal_cap, transfer_strength, color_strength;
    uint32_t encoding;
    float dark_gate;
    uint32_t curve;
    float pedestal_gate, slew_stops, snap_now, chroma_clamp_stops, release_stops;
    uint32_t governor_mode, transfer_mode;
    float highlight_guard_stops;
    uint32_t dark_gate_mode;
    uint32_t padding[2];
};
static_assert(sizeof(V6Constants) == 28 * 4);

struct Frame6 {   // the frame's snapshots (ProcessInline): one divisor, encoding, gate and cap
    uint8_t hdr_mode{};
    codec::SourceUnits units;
    float divisor{}, cap{}, gate{};
    uint32_t encoding{};
};

V6Constants make_v6(const Settings& s, const Frame6& f, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                    uint32_t pw, uint32_t ph, uint32_t nw, uint32_t nh, float transfer, float color, float chroma,
                    bool ratio_resolve = false) {
    V6Constants c{};
    c.width = w; c.height = h; c.source_width = sw; c.source_height = sh;
    c.proxy_width = pw; c.proxy_height = ph; c.neural_width = nw; c.neural_height = nh;
    c.divisor = f.divisor; c.pedestal_cap = f.cap;
    c.transfer_strength = transfer; c.color_strength = color;
    c.encoding = f.encoding; c.dark_gate = f.gate;
    c.curve = ratio_resolve || frame_display_codec(s, f.hdr_mode, f.units) ? 1u : s.neural_floor_guard ? 2u : 0u;
    c.pedestal_gate = frame_pedestal_removal(s, f.hdr_mode, f.units) ? 1.f : 0.f;
    c.chroma_clamp_stops = chroma;
    c.transfer_mode = s.transfer_mode;
    c.highlight_guard_stops = s.style_highlight_guard;
    c.dark_gate_mode = s.display_pedestal;
    return c;
}

// The legacy cbuffer (legacy_common.hlsli, BindCodec's 20 dwords).
struct LegacyConstants {
    uint32_t size[2], source_size[2], source_base[2], proxy_size[2], neural_size[2];
    float paper_white_scale, transfer_strength, color_strength;
    uint32_t hdr;
    float diffuse_white_nits;
    float unread;
    float padding[4];
};
static_assert(sizeof(LegacyConstants) == 80);

// FeedSource (dlssnr.hpp): where a relative-HDR frame's NR input scale comes from.
enum Feed : uint32_t { kFeedV1 = 0, kFeedTexture = 1, kFeedFixed = 2 };

// The governor's per-stream state (NormStream) and PlanFrameScale.
struct NormStream {
    bool primed = false;
    uint32_t feed = kFeedV1;
    int64_t last_ns = 0, last_reset_ns = 0;
};
constexpr int64_t kNormResetQuietNs = 250'000'000;

struct ScalePlan {
    uint32_t governor[5] = {};   // slew_stops .. governor_mode
    bool guarded = false;        // Auto's texture feed, range-guarded against v1
    bool v2_snap = false;        // v6_exposure_scale's SnapNow
};
uint32_t bits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

// SelectFeed: v2 needs positive evidence (a readable exposure texture on a stream without AutoExposure).
uint32_t select_feed(const Settings& s, uint8_t hdr_mode, const codec::SourceUnits& u, bool readable, bool auto_flag) {
    if (hdr_mode == 0 || u.absolute || u.encoding == codec::Encoding::Sdr || s.feed_mode == 1u) return kFeedV1;
    if (s.feed_mode == 2u) return readable ? kFeedTexture : kFeedFixed;
    return readable && !auto_flag ? kFeedTexture : kFeedV1;
}

ScalePlan plan_frame_scale(const Settings& s, NormStream& stream, bool frame_reset, uint32_t feed, bool epoch) {
    ScalePlan plan;
    const uint32_t mode = s.norm_governor == 2u && s.norm_cut_aware ? 3u : s.norm_governor;
    const bool stable = mode >= 2u, cut_aware = mode >= 3u;
    const int64_t now = now_ns();
    bool reset_snap = !stable && frame_reset;
    if (cut_aware && frame_reset) {
        reset_snap = stream.last_reset_ns == 0 || now - stream.last_reset_ns >= kNormResetQuietNs;
        stream.last_reset_ns = now;
    }
    // The reset-epoch snap of modes 0/1: RenoDX's epoch advances on every history reset it requests - a kHistory
    // setting's change, the area-input crossing, a feature recreate (NR turned on again, a kRecreate setting) -
    // here `epoch` (Impl::recreate or Impl::history_reset).
    const bool prime = !stream.primed, feed_change = stream.primed && stream.feed != feed;
    const bool snap = prime || reset_snap || feed_change || (!stable && epoch);
    stream.primed = true;
    stream.feed = feed;
    const float frame_seconds = stream.last_ns != 0
        ? std::clamp((now - stream.last_ns) / 1e9f, cut_aware ? 0.f : 1.f / 240.f, 0.1f)
        : 1.f / 60.f;
    stream.last_ns = now;
    const float attack = stable ? s.norm_attack_stops * frame_seconds : s.norm_slew_stops;
    const float release = stable ? s.norm_release_stops * frame_seconds : 0.f;
    plan.governor[0] = bits(mode == 0u ? 0.f : attack);
    plan.governor[1] = bits(snap ? 1.f : 0.f);
    plan.governor[3] = bits(release);
    plan.governor[4] = mode;
    plan.guarded = feed == kFeedTexture && s.feed_mode == 0u;
    // v6_exposure_scale's snap leaves out cut-aware's Reset re-prime, which is v1's.
    plan.v2_snap = prime || feed_change || (reset_snap && !cut_aware);
    return plan;
}

// ---- Preprocess on this path (renodx_prep.comp): runtime_prep.comp's push block and curve anchors ------
struct PrepPush {
    uint32_t w, h, mode, flags, curve;
    float bias, contrast, saturation, k, cap;
    float dt;
};
enum : uint32_t { kPrepAuto = 1, kPrepRestore = 4, kPrepReset = 8, kPrepLinear = 16 };
// nr_runtime.cpp kPrepAnchor: each curve's input scale that leaves mid grey where it was.
constexpr float kPrepAnchor[7] = {1.0f, 1.0f, 1.2195122f, 2.9275228f, 1.0052344f, 0.7231708f, 0.8083602f};

// ---- look_stage.hpp ----------------------------------------------------------------------------------
namespace look {
constexpr uint32_t kFlagBands = 1u << 0, kFlagTemporal = 1u << 1, kFlagMotion = 1u << 2,
                   kFlagTemporalDetail = 1u << 3, kFlagTransport = 1u << 4, kFlagRefLinear = 1u << 5,
                   kFlagHistoryValid = 1u << 6, kFlagRangeConfidence = 1u << 7;
constexpr float kHistoryMaxGapSeconds = 0.25f;
constexpr uint32_t kUpsampleAuto = 0, kUpsampleEdgeAware = 1, kUpsampleClassic = 2, kUpsampleAreaInput = 3;
constexpr float kAreaEncode = 1.f;   // legacy_encode's Padding.x: the area footprint instead of a point
constexpr float kDetailStabilityAlpha = 0.15f;   // dlssnr.hpp defaults::kDetailStabilityAlpha

struct Constants {
    uint32_t size[2], nr_size[2], full_size[2], flags, mode;
    float strength, brighten, darken, max_brighten, max_darken, colour, hue, max_colour, shadows, midtones,
        highlights, tone, detail, halo;
    uint32_t radius;
    float alpha, motion_scale[2];
    uint32_t motion_base[2], motion_grid[2];
    float gf_eps, halo_bright;
    uint32_t min_radius, mid_radius, padding;
};
static_assert(sizeof(Constants) == 35 * 4);

struct Settings {
    float strength = 1.f, brighten = 1.f, darken = 1.f, max_brighten = 0.f, max_darken = 0.f, colour = 1.f,
          hue = 1.f, max_colour = 0.f, shadows = 1.f, midtones = 1.f, highlights = 1.f, tone = 1.f, detail = 1.f,
          halo = 0.f, halo_bright = 0.f, detail_radius = 1.f;
    uint32_t stabilize = 0;
    float stabilize_ms = 60.f;
    bool stabilize_detail = false;
    float stabilize_alpha = 0.f;
    uint32_t upsample = 0;
};

bool gains_neutral(const Settings& s) {
    return s.strength == 1.f && s.brighten == 1.f && s.darken == 1.f && s.max_brighten <= 0.f && s.max_darken <= 0.f &&
           s.colour == 1.f && s.hue == 1.f && s.max_colour <= 0.f && s.shadows == 1.f && s.midtones == 1.f &&
           s.highlights == 1.f && s.tone == 1.f && s.detail == 1.f && s.halo <= 0.f && s.halo_bright <= 0.f;
}
bool bands_wanted(const Settings& s) {
    return s.tone != 1.f || s.detail != 1.f || s.halo > 0.f || s.halo_bright > 0.f ||
           (s.stabilize != 0 && s.stabilize_alpha <= 0.f);
}
bool dispatched(const Settings& s, bool transport) { return !gains_neutral(s) || s.stabilize != 0 || transport; }
bool transport_wanted(const Settings& s, uint32_t nw, uint32_t nh, uint32_t fw, uint32_t fh) {
    return s.upsample != kUpsampleClassic && nw <= fw && nh <= fh && (nw != fw || nh != fh);
}

struct Frame {
    uint32_t nr_width = 0, nr_height = 0, full_width = 0, full_height = 0;
    bool reference_linear = false, transport = false;
    float frame_seconds = 1.f / 60.f;
    bool history_valid = false, motion = false;
    float motion_scale[2] = {0.f, 0.f};
    uint32_t motion_base[2] = {0, 0}, motion_grid[2] = {1, 1};
};

uint32_t band_radius(const Settings& s, uint32_t nr_height) {
    const float half_pixels = s.detail_radius * 0.01f * float(nr_height) * 0.5f;
    return std::clamp(uint32_t(std::lround(half_pixels)), 1u, 64u);
}

Constants make_constants(const Settings& s, const Frame& f) {
    Constants c{};
    c.size[0] = f.nr_width; c.size[1] = f.nr_height;
    c.nr_size[0] = f.nr_width; c.nr_size[1] = f.nr_height;
    c.full_size[0] = f.full_width; c.full_size[1] = f.full_height;
    const bool temporal = s.stabilize != 0;
    c.flags = (bands_wanted(s) ? kFlagBands : 0u) | (temporal ? kFlagTemporal : 0u) |
              (temporal && s.stabilize == 2u && f.motion ? kFlagMotion : 0u) |
              (temporal && s.stabilize_detail ? kFlagTemporalDetail : 0u) | (f.transport ? kFlagTransport : 0u) |
              (f.reference_linear ? kFlagRefLinear : 0u) | (temporal && f.history_valid ? kFlagHistoryValid : 0u) |
              (temporal && s.stabilize_alpha > 0.f ? kFlagRangeConfidence : 0u);
    c.strength = s.strength; c.brighten = s.brighten; c.darken = s.darken;
    c.max_brighten = s.max_brighten; c.max_darken = s.max_darken;
    c.colour = s.colour; c.hue = s.hue; c.max_colour = s.max_colour;
    c.shadows = s.shadows; c.midtones = s.midtones; c.highlights = s.highlights;
    c.tone = s.tone; c.detail = s.detail; c.halo = s.halo; c.halo_bright = s.halo_bright;
    c.radius = band_radius(s, f.nr_height);
    c.min_radius = std::min(c.radius * 4u, 48u);
    c.mid_radius = std::min(c.radius * 6u, 64u);
    c.alpha = s.stabilize_alpha > 0.f ? s.stabilize_alpha
                                      : 1.f - std::exp(-f.frame_seconds / std::max(s.stabilize_ms * 1e-3f, 1e-3f));
    c.motion_scale[0] = f.motion_scale[0]; c.motion_scale[1] = f.motion_scale[1];
    c.motion_base[0] = f.motion_base[0]; c.motion_base[1] = f.motion_base[1];
    c.motion_grid[0] = std::max(f.motion_grid[0], 1u); c.motion_grid[1] = std::max(f.motion_grid[1], 1u);
    c.gf_eps = 0.01f;
    return c;
}

// LookSettingsSnapshot: with NRLookMode off every gain is the identity (Upsampling still applies); Detail
// stability rides on top when the player's own Stabilize is off.
Settings snapshot(const nr::renodx::Settings& s, bool auto_engages) {
    Settings out{};
    out.upsample = s.look_upsample;
    if (s.look_mode) {
        out.strength = s.look_strength; out.brighten = s.look_brighten; out.darken = s.look_darken;
        out.max_brighten = s.look_max_brighten; out.max_darken = s.look_max_darken;
        out.colour = s.look_colour; out.hue = s.look_hue; out.max_colour = s.look_max_colour;
        out.shadows = s.look_shadows; out.midtones = s.look_midtones; out.highlights = s.look_highlights;
        out.tone = s.look_tone; out.detail = s.look_detail; out.halo = s.look_halo; out.halo_bright = s.look_halo_bright;
        out.detail_radius = s.look_detail_radius;
        out.stabilize = s.look_stabilize; out.stabilize_ms = s.look_stabilize_ms;
        out.stabilize_detail = s.look_stabilize_detail;
    }
    if (out.stabilize == 0u && (s.detail_stability == 2u || (s.detail_stability == 0u && auto_engages))) {
        out.stabilize = 2u;
        out.stabilize_detail = true;
        out.stabilize_alpha = kDetailStabilityAlpha;
    }
    return out;
}
}  // namespace look

// ---- Vulkan: images and programs ----------------------------------------------------------------

struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImageView view{};
    uint32_t w{}, h{};
    VkFormat format{};
};

// A program: one RenoDX shader, its descriptor layout read from the SPIR-V (binding -> sampled or
// storage image), its push range.
struct Program {
    VkShaderModule module{};
    VkDescriptorSetLayout dsl{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    std::map<uint32_t, VkDescriptorType> bindings;
    uint32_t push_bytes{};
    std::string name;
};

std::vector<uint32_t> read_spirv(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("RenoDX stage: cannot open " + path);
    const std::streamoff n = f.tellg();
    std::vector<uint32_t> code(size_t(n) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), n);
    if (code.empty() || code[0] != 0x07230203u) throw std::runtime_error("RenoDX stage: not SPIR-V: " + path);
    return code;
}

// Every UniformConstant image variable's binding and whether it is sampled (Sampled = 1) or storage (2), and every
// StorageBuffer variable's (Preprocess's state).
std::map<uint32_t, VkDescriptorType> reflect(const std::vector<uint32_t>& code) {
    std::map<uint32_t, uint32_t> binding_of, pointee, image_sampled;
    std::vector<std::pair<uint32_t, uint32_t>> variables;   // (pointer type, id)
    std::vector<uint32_t> buffers;
    std::vector<std::pair<uint32_t, uint32_t>> uniforms;    // (pointer type, id)
    std::set<uint32_t> buffer_blocks;
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xffffu, len = code[i] >> 16;
        if (len == 0) break;
        const uint32_t* w = &code[i];
        if (op == 71 && len >= 4 && w[2] == 33) binding_of[w[1]] = w[3];            // OpDecorate Binding
        else if (op == 71 && len >= 3 && w[2] == 3) buffer_blocks.insert(w[1]);      // OpDecorate BufferBlock
        else if (op == 32 && len >= 4) pointee[w[1]] = w[3];                         // OpTypePointer
        else if (op == 25 && len >= 9) image_sampled[w[1]] = w[7];                   // OpTypeImage
        else if (op == 59 && len >= 4 && w[3] == 0) variables.push_back({w[1], w[2]}); // OpVariable UniformConstant
        else if (op == 59 && len >= 4 && w[3] == 12) buffers.push_back(w[2]);           // OpVariable StorageBuffer
        else if (op == 59 && len >= 4 && w[3] == 2) uniforms.push_back({w[1], w[2]});   // OpVariable Uniform
        i += len;
    }
    // Slang's glsl_460 storage buffers: Uniform variables of a BufferBlock struct.
    for (const auto& [ptr, id] : uniforms) {
        const auto t = pointee.find(ptr);
        if (t == pointee.end() || !buffer_blocks.count(t->second))
            throw std::runtime_error("RenoDX stage: a uniform buffer");
        buffers.push_back(id);
    }
    std::map<uint32_t, VkDescriptorType> out;
    for (uint32_t id : buffers) {
        const auto b = binding_of.find(id);
        if (b != binding_of.end()) out[b->second] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    for (const auto& [ptr, id] : variables) {
        const auto b = binding_of.find(id);
        const auto t = pointee.find(ptr);
        if (b == binding_of.end() || t == pointee.end()) continue;
        const auto s = image_sampled.find(t->second);
        if (s == image_sampled.end()) throw std::runtime_error("RenoDX stage: a non-image resource");
        out[b->second] = s->second == 1 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return out;
}

}  // namespace

struct Stage::Impl {
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceMemoryProperties mem{};
    // The driver has VK_EXT_pipeline_robustness: the programs are compiled without robust buffer and image access,
    // as the network's (nrvk::Kernel::create); DXVK and vkd3d-proton create the game's device robust.
    bool pipeline_robustness{};
    StageConfig cfg;
    Settings settings;
    uint8_t hdr{};
    codec::SourceUnits units;
    VkDescriptorPool pool{};

    enum P { kLinearize, kAutoscale, kExposureScale, kEncode6, kResolve6, kReduce6, kReduce6Unshaped, kCommit6,
             kCommitExposure, kEncodeL, kDecodeL, kReduceL, kReduceLUnshaped, kApplyL, kCompose, kUpsample, kBand,
             kPrep, kPrepEncode, kResolveCommit6, kCount };
    Program programs[kCount];

    // The stack's ping-pong: HDR resolves pass k into work_a / work_b (k even / odd), SDR decodes into
    // decoded / decoded_alt. The second of each is made when a stack first needs it.
    Image original, work0, work_a, work_b, proxy, neural, look_out, decoded, decoded_alt, block_mean, scale, commit;
    Image up_proxy, up_neural;     // look_upsample's P_up and N'_up at the frame's extent (network below it)
    Image band_a, band_b, band_max_a, band_max_b;   // the look's bands at half the network's extent, when wanted
    // Preprocess: what the network is shown, made from the pass's running image (HDR) or the kept proxy (SDR),
    // and its meter's state.
    Image work_p, prep_keep;
    VkBuffer prep_state{};
    VkDeviceMemory prep_state_memory{};
    // The engine-data panel's divisor: the scale texel copied here every 32nd recording (host-visible, read by facts()
    // a few frames later; a torn or stale value is only a display).
    VkBuffer probe{};
    VkDeviceMemory probe_memory{};
    float* probe_mapped{};
    VkBuffer unread{};             // a buffer for the look programs' trace slot, which the stage never reads
    VkDeviceMemory unread_memory{};
    bool prep_state_fresh = false, prep_was_on = false;
    Preprocess prep_last{};
    int64_t prep_metered_ns = 0;
    bool area_input = false, area_known = false, history_reset = false;
    // NR was off (NeuralUplift, or every pass at zero strength): turning it on again is RenoDX's RecreateFeatures -
    // its RequestHistoryReset (every history from the start) and the reset-epoch snap of the governor.
    bool recreate = false;
    // Each pass of the stack has its own look history (RenoDX's slot.look_history).
    struct History { uint32_t written = 0; bool valid = false, whole_edit = false; int64_t last_ns = 0; };
    // The history's three words a texel: RG32_UINT + R32_UINT (look_common.hlsli DLSSNR_HISTORY_SPLIT).
    struct PassLook { Image history[2], history_z[2]; History hist; };
    std::vector<PassLook> looks;
    uint32_t logged_passes = 1;
    Image motion;                  // the game's motion vectors, copied (region at the origin)
    Image exposure;                // texel (0,0) of the game's exposure texture, copied
    bool commit_cleared = false;
    bool decoded_storage = false;  // decoded is the output's own format (else RGBA16F + blit)
    NormStream norm;
    std::vector<std::pair<uint64_t, Image>> retired;
    std::vector<std::pair<uint64_t, VkDescriptorSet>> retired_sets;
    std::vector<VkImage> fresh;    // made, not yet in GENERAL
    std::map<std::string, VkDescriptorSet> sets;
    uint64_t recordings = 0;
    // Per frame.
    Frame6 f6;
    uint32_t feed = kFeedV1;       // this frame's
    uint32_t logged_feed = ~0u;
    Stage::Facts facts;            // the last frame's, for facts()
    PrepPush prep_push{};          // this frame's Preprocess
    bool prep_on = false, ratio_resolve = false;

    Image make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
        Image im; im.w = w; im.h = h; im.format = f;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(device, &ci, nullptr, &im.handle), "image");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, im.handle, &req);
        im.memory = allocate(req, "an image");
        check(vkBindImageMemory(device, im.handle, im.memory, 0), "bind");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.handle; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &im.view), "view");
        fresh.push_back(im.handle);
        return im;
    }
    VkDeviceMemory allocate(const VkMemoryRequirements& req, const char* what) {
        uint32_t type = mem.memoryTypeCount;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i; break;
            }
        if (type == mem.memoryTypeCount) throw std::runtime_error(std::string("RenoDX stage: no device-local memory for ") + what);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        VkDeviceMemory m{};
        check(vkAllocateMemory(device, &ai, nullptr, &m), "memory");
        return m;
    }
    void make_buffer(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &bi, nullptr, buffer), "buffer");
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, *buffer, &req);
        *memory = allocate(req, "a buffer");
        check(vkBindBufferMemory(device, *buffer, *memory, 0), "bind");
    }
    void destroy(Image& im) {
        if (im.view) vkDestroyImageView(device, im.view, nullptr);
        if (im.handle) vkDestroyImage(device, im.handle, nullptr);
        if (im.memory) vkFreeMemory(device, im.memory, nullptr);
        im = {};
    }
    // Every image made since the last call into GENERAL, where it stays.
    void init_fresh(VkCommandBuffer cmd) {
        for (VkImage im : fresh)
            image_barrier(cmd, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        fresh.clear();
    }

    void load(Program& p, const std::string& name, uint32_t push_bytes) {
        p.name = name;
        const auto code = read_spirv(cfg.shaders + "/renodx_" + name + ".spv");
        p.bindings = reflect(code);
        p.push_bytes = push_bytes;
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const auto& [b, t] : p.bindings) lb.push_back({b, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = uint32_t(lb.size()); li.pBindings = lb.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &p.dsl), "set layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &p.dsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(device, &pli, nullptr, &p.layout), "pipeline layout");
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
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline), name.c_str());
    }

    // One descriptor set of `p` with `views[binding]`; every binding the program declares must be given.
    VkDescriptorSet set_for(const Program& p, const std::map<uint32_t, VkImageView>& views,
                            const std::map<uint32_t, VkBuffer>& buffers, VkDescriptorPool from = VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = from ? from : pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p.dsl;
        VkDescriptorSet set{};
        check(vkAllocateDescriptorSets(device, &ai, &set), "descriptor set");
        std::vector<VkDescriptorImageInfo> ii;
        ii.reserve(p.bindings.size());
        std::vector<VkDescriptorBufferInfo> bi;
        bi.reserve(p.bindings.size());
        std::vector<VkWriteDescriptorSet> w;
        for (const auto& [b, t] : p.bindings) {
            if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const auto buf = buffers.find(b);
                if (buf == buffers.end()) throw std::runtime_error("RenoDX stage: buffer binding " + std::to_string(b) + " not given");
                bi.push_back({buf->second, 0, VK_WHOLE_SIZE});
                VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                wd.dstSet = set; wd.dstBinding = b; wd.descriptorCount = 1; wd.descriptorType = t;
                wd.pBufferInfo = &bi.back();
                w.push_back(wd);
                continue;
            }
            const auto v = views.find(b);
            if (v == views.end()) throw std::runtime_error("RenoDX stage: binding " + std::to_string(b) + " not given");
            ii.push_back({VK_NULL_HANDLE, v->second, VK_IMAGE_LAYOUT_GENERAL});
            VkWriteDescriptorSet wd{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wd.dstSet = set; wd.dstBinding = b; wd.descriptorCount = 1; wd.descriptorType = t;
            wd.pImageInfo = &ii.back();
            w.push_back(wd);
        }
        vkUpdateDescriptorSets(device, uint32_t(w.size()), w.data(), 0, nullptr);
        return set;
    }
    // The set of `p` over these views, made on first use and kept (every image is the stage's own; a retired
    // image takes every kept set with it, see retire()).
    VkDescriptorSet S(P p, const std::map<uint32_t, VkImageView>& views, const std::map<uint32_t, VkBuffer>& buffers = {}) {
        std::string key = std::to_string(int(p));
        for (const auto& [b, v] : views) key += "," + std::to_string(b) + ":" + std::to_string(uint64_t(v));
        for (const auto& [b, v] : buffers) key += ";" + std::to_string(b) + ":" + std::to_string(uint64_t(v));
        const auto it = sets.find(key);
        if (it != sets.end()) return it->second;
        const VkDescriptorSet set = set_for(programs[p], views, buffers);
        sets.emplace(key, set);
        return set;
    }

    // The HDR frame read in place and the answer written straight into the output, the game's motion vectors read
    // in place by Detail stability (DLSSNR-AMD: no copy where the images allow it). The views of the game's images
    // and the sets naming them (the linearize's, the commit's, each pass's compose) are this recording's only - a
    // game's image handle can come back for another image once the game frees it - and are freed once the frames in
    // flight are past them, as the retired images are.
    struct Borrow { VkImageView in{}, out{}, motion{}; VkDescriptorPool pool{}; };
    std::vector<Borrow> borrows;
    bool in_place_logged = false, motion_logged = false;
    Borrow& borrow_slot() {
        if (borrows.empty()) borrows.resize(66);   // retire()'s 64 recordings, and two
        Borrow& b = borrows[size_t(recordings % borrows.size())];
        if (b.in) vkDestroyImageView(device, b.in, nullptr);
        if (b.out) vkDestroyImageView(device, b.out, nullptr);
        if (b.motion) vkDestroyImageView(device, b.motion, nullptr);
        b.in = b.out = b.motion = VK_NULL_HANDLE;
        if (!b.pool) {
            constexpr uint32_t kSets = 2 + kMaxUnlockedPasses;
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * kSets},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12 * kSets},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kSets}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSets; pi.poolSizeCount = 3; pi.pPoolSizes = sizes;
            check(vkCreateDescriptorPool(device, &pi, nullptr, &b.pool), "descriptor pool");
        } else {
            vkResetDescriptorPool(device, b.pool, 0);
        }
        return b;
    }
    VkImageView view_of(VkImage im, VkFormat f) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v{};
        return vkCreateImageView(device, &vi, nullptr, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
    }

    // NR_RENODX_PROF (a measurement aid): a timestamp after every step of the stage, read back a few frames later,
    // each step's time (from the previous timestamp) averaged and logged.
    struct Prof {
        static constexpr uint32_t kSlots = 4, kMax = 128;
        // NR_RENODX_PROF=1: logged every 300 frames; =<n> (n > 1): every n frames
        uint64_t every = [] { const char* e = std::getenv("NR_RENODX_PROF"); const long v = e ? std::atol(e) : 0;
                              return uint64_t(v > 1 ? v : v == 1 ? 300 : 0); }();
        bool on = every != 0;
        VkQueryPool pool{};
        double ns = 1.0;
        uint32_t slot = 0;
        uint64_t frames = 0;
        std::vector<std::string> names[kSlots];
        std::vector<std::string> order;
        std::map<std::string, std::pair<double, uint32_t>> acc;
    } prof;
    void mark(VkCommandBuffer cmd, const std::string& name) {
        if (!prof.on || !prof.pool) return;
        auto& n = prof.names[prof.slot];
        if (n.size() >= Prof::kMax) return;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, prof.pool, prof.slot * Prof::kMax + uint32_t(n.size()));
        n.push_back(name);
    }
    void prof_begin(VkCommandBuffer cmd) {
        if (!prof.on) return;
        if (!prof.pool) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical, &props);
            prof.ns = props.limits.timestampPeriod;
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = Prof::kSlots * Prof::kMax;
            if (vkCreateQueryPool(device, &qi, nullptr, &prof.pool) != VK_SUCCESS) { prof.on = false; return; }
        }
        vkCmdResetQueryPool(cmd, prof.pool, prof.slot * Prof::kMax, Prof::kMax);
        prof.names[prof.slot].clear();
        mark(cmd, "start");
    }
    void prof_end() {
        if (!prof.on || !prof.pool) return;
        prof.slot = (prof.slot + 1) % Prof::kSlots;
        if (++prof.frames <= Prof::kSlots) return;
        auto& n = prof.names[prof.slot];   // the oldest
        if (n.size() < 2) return;
        std::vector<uint64_t> t(n.size());
        if (vkGetQueryPoolResults(device, prof.pool, prof.slot * Prof::kMax, uint32_t(n.size()), t.size() * 8, t.data(), 8,
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        for (size_t i = 1; i < n.size(); ++i) {
            if (!prof.acc.count(n[i])) prof.order.push_back(n[i]);
            auto& a = prof.acc[n[i]];
            a.first += double(t[i] - t[i - 1]) * prof.ns * 1e-6;
            ++a.second;
        }
        if (prof.frames % prof.every == 0) {
            const double frames = double(prof.acc[prof.order.front()].second);
            std::string line = "[nr] RenoDX stage time a frame (ms):";
            double total = 0;
            for (const auto& k : prof.order) {
                const double ms = prof.acc[k].first / frames;
                total += ms;
                char b[96];
                std::snprintf(b, sizeof b, " %s %.3f", k.c_str(), ms);
                line += b;
            }
            char b[48];
            std::snprintf(b, sizeof b, "; total %.3f", total);
            nr::logf("%s%s", line.c_str(), b);
            prof.acc.clear();
            prof.order.clear();
        }
    }

    void dispatch(VkCommandBuffer cmd, P p, VkDescriptorSet set, const void* push, uint32_t bytes, uint32_t x, uint32_t y) {
        const Program& pr = programs[p];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pr.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pr.layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pr.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, std::min(bytes, pr.push_bytes), push);
        vkCmdDispatch(cmd, x, y, 1);
        mark(cmd, pr.name);
        // Every dispatch's writes before whatever the next one reads (RenoDX orders them the same way,
        // UAV barrier / storage-to-storage transition after each pass).
        memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    }
    static void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst,
                               VkAccessFlags da) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = sa; b.dstAccessMask = da;
        vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    static void image_barrier(VkCommandBuffer cmd, VkImage im, VkImageLayout from, VkImageLayout to,
                              VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // One of the stage's images onto another of the same extent (CopyMip0), both in GENERAL.
    static void copy_general(VkCommandBuffer cmd, const Image& from, const Image& to) {
        memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {from.w, from.h, 1};
        vkCmdCopyImage(cmd, from.handle, VK_IMAGE_LAYOUT_GENERAL, to.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    bool storage_capable(VkFormat f) {
        VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
        return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
    }
    // An image out of use: destroyed once frames in flight are past it, with every kept set (one may name it).
    void retire(Image& im) {
        if (im.handle) {
            retired.push_back({recordings + 64, im});
            for (const auto& [key, set] : sets) retired_sets.push_back({recordings + 64, set});
            sets.clear();
        }
        im = {};
    }
    void collect() {
        for (auto it = retired.begin(); it != retired.end();)
            if (it->first <= recordings) { destroy(it->second); it = retired.erase(it); } else ++it;
        for (auto it = retired_sets.begin(); it != retired_sets.end();)
            if (it->first <= recordings) { vkFreeDescriptorSets(device, pool, 1, &it->second); it = retired_sets.erase(it); }
            else ++it;
    }

    // The exposure texel's image, (re)made when the game's exposure format changes.
    void ensure_exposure(VkFormat f) {
        if (exposure.handle && exposure.format == f) return;
        retire(exposure);
        exposure = make_image(1, 1, f, VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    // Preprocess's images, state and programs, made the first time it runs.
    void ensure_prep() {
        if (prep_state) return;
        const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if (hdr) work_p = make_image(cfg.width, cfg.height, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
        else prep_keep = make_image(cfg.nr_width, cfg.nr_height, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
        make_buffer(16, &prep_state, &prep_state_memory);
        prep_state_fresh = true;
        load(programs[kPrep], "prep", sizeof(PrepPush));
        if (hdr) load(programs[kPrepEncode], "prep_encode", 16);
    }
    Image& stack_out(uint32_t pass) { return hdr ? ((pass & 1u) ? work_b : work_a) : ((pass & 1u) ? decoded_alt : decoded); }
    VkImageView scale_or_work0() const { return units.absolute ? work0.view : scale.view; }

    // PassTransferStrength / PassColorStrength: pass 1 is the global pair, passes 2.. their own (4 and later: 4's).
    void strengths(uint32_t pass, float* transfer, float* color) const {
        if (pass == 0) { *transfer = settings.transfer_strength; *color = settings.color_strength; return; }
        const uint32_t i = std::min(pass, 3u) - 1u;
        *transfer = settings.pass_transfer[i];
        *color = settings.pass_color[i];
    }
    // ChainActive: whether any pass changes the image. A Display frame blends by colour strength (transfer only
    // scales its pedestal removal); a classic frame by transfer strength.
    bool chain_active(uint32_t first, uint32_t passes) const {
        const bool display = frame_display_codec(settings, hdr, units);
        const bool pedestal = frame_pedestal_removal(settings, hdr, units);
        for (uint32_t k = 0; k < passes; ++k) {
            float t = 0.f, c = 0.f;
            strengths(first + k, &t, &c);
            if (display ? (pedestal && t > 0.f) || c > 0.f : t > 0.f) return true;
        }
        return false;
    }

    // RunLookStage for one pass: the bands, the temporal filter on the pass's own history and every gain into
    // look_out (N'), then, below the frame's extent, P_up and N'_up. Returns whether it shaped.
    struct Route { bool shaped = false, transport = false; };
    // `borrow`: the recording's borrowed views and pool when the game's motion vectors are read in place
    // (borrow->motion), else null.
    Route run_look(VkCommandBuffer cmd, uint32_t pass, const Image& ref, const look::Settings& ls, bool motion_copied,
                   const StageFrame& fr, bool reset, const Borrow* borrow) {
        const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
        Route route;
        route.transport = up_proxy.handle && look::transport_wanted(ls, NW, NH, W, H);
        route.shaped = look::dispatched(ls, route.transport);
        PassLook& pl = looks[pass];
        bool temporal = route.shaped && ls.stabilize != 0;
        if (!temporal) {
            pl.hist.valid = false;
            if (pl.history[0].handle)
                for (int i = 0; i < 2; ++i) { retire(pl.history[i]); retire(pl.history_z[i]); }
        }
        if (!route.shaped) return route;
        if (look::bands_wanted(ls) && !band_a.handle) {   // sticky, as RenoDX's workset
            const uint32_t bw = (NW + 1) / 2, bh = (NH + 1) / 2;
            band_a = make_image(bw, bh, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
            band_b = make_image(bw, bh, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
            band_max_a = make_image(bw, bh, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
            band_max_b = make_image(bw, bh, VK_FORMAT_R32G32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
            if (!programs[kBand].pipeline) load(programs[kBand], "look_band", sizeof(look::Constants));
        }
        if (temporal && !pl.history[0].handle) {
            for (Image& h : pl.history) h = make_image(NW, NH, VK_FORMAT_R32G32_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
            for (Image& h : pl.history_z) h = make_image(NW, NH, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
            pl.hist = {};
        }
        init_fresh(cmd);
        const int64_t now = now_ns();
        look::Frame lf;
        lf.nr_width = NW; lf.nr_height = NH; lf.full_width = W; lf.full_height = H;
        lf.reference_linear = hdr != 0;
        lf.transport = route.transport;
        const bool whole_edit = ls.stabilize_alpha > 0.f;
        if (temporal) {
            // PlanLookHistory
            const float gap = pl.hist.last_ns != 0 ? (now - pl.hist.last_ns) / 1e9f : 0.f;
            const bool restart = !pl.hist.valid || reset || pl.hist.whole_edit != whole_edit ||
                                 (!whole_edit && gap > look::kHistoryMaxGapSeconds);
            lf.history_valid = !restart;
            lf.frame_seconds = pl.hist.last_ns != 0 ? std::clamp(gap, 1.f / 240.f, look::kHistoryMaxGapSeconds) : 1.f / 60.f;
            if (ls.stabilize == 2u && motion_copied) {
                // DLSS motion vectors scaled to the NR grid (RunLookStage): texel * MVScale * NR / motion
                // extent; EngineFrame's scale is MVScale / motion extent already.
                lf.motion = true;
                lf.motion_scale[0] = fr.motion_scale_x * float(NW);
                lf.motion_scale[1] = fr.motion_scale_y * float(NH);
                lf.motion_grid[0] = fr.motion.width;
                lf.motion_grid[1] = fr.motion.height;
                if (borrow && borrow->motion) {   // the vectors' region where it is in the game's image
                    lf.motion_base[0] = fr.motion_x;
                    lf.motion_base[1] = fr.motion_y;
                }
            }
        }
        look::Settings shaping = ls;
        if (!temporal) shaping.stabilize = 0;
        look::Constants lc = look::make_constants(shaping, lf);
        const VkImageView mv = motion.view ? motion.view : original.view;
        const bool motion_borrowed = borrow && borrow->motion;
        const VkImageView hist_out = temporal ? pl.history[pl.hist.written ^ 1u].view : look_out.view;
        const VkImageView hist_in = temporal ? pl.history[pl.hist.written].view : look_out.view;
        const VkImageView hist_out_z = temporal ? pl.history_z[pl.hist.written ^ 1u].view : look_out.view;
        const VkImageView hist_in_z = temporal ? pl.history_z[pl.hist.written].view : look_out.view;
        const VkImageView upp = up_proxy.handle ? up_proxy.view : look_out.view;
        const VkImageView upn = up_neural.handle ? up_neural.view : look_out.view;
        const VkImageView ba = band_a.handle ? band_a.view : look_out.view, bb = band_b.handle ? band_b.view : look_out.view;
        const VkImageView ma = band_max_a.handle ? band_max_a.view : look_out.view;
        const VkImageView mb = band_max_b.handle ? band_max_b.view : look_out.view;
        // The codec set [ref, P, N, ref] and, for the upsample, the look set [ref, P, N', ref].
        const auto look_views = [&](VkImageView n) {
            return std::map<uint32_t, VkImageView>{{0, ref.view}, {1, proxy.view}, {2, n}, {3, ref.view}, {4, mv},
                {5, look_out.view}, {6, ba}, {7, bb}, {8, ma}, {9, mb}, {10, upp}, {11, upn}, {13, look_out.view},
                {14, hist_out}, {15, hist_in}, {16, hist_out_z}, {17, hist_in_z}};
        };
        const std::map<uint32_t, VkBuffer> trace{{12, unread}};
        // RecordCompose: the five band passes at half the network's extent, then the compose.
        if (lc.flags & look::kFlagBands) {
            look::Constants band = lc;
            band.size[0] = (NW + 1) / 2; band.size[1] = (NH + 1) / 2;
            const VkDescriptorSet set = S(kBand, look_views(neural.view), trace);
            for (uint32_t mode = 0; mode < 5; ++mode) {
                band.mode = mode;
                dispatch(cmd, kBand, set, &band, sizeof band, (band.size[0] + 7) / 8, (band.size[1] + 7) / 8);
            }
        }
        lc.mode = 0;
        if (motion_borrowed) {
            auto views = look_views(neural.view);
            views[4] = borrow->motion;
            dispatch(cmd, kCompose, set_for(programs[kCompose], views, trace, borrow->pool), &lc, sizeof lc,
                     (NW + 15) / 16, (NH + 15) / 16);
        } else {
            dispatch(cmd, kCompose, S(kCompose, look_views(neural.view), trace), &lc, sizeof lc, (NW + 15) / 16,
                     (NH + 15) / 16);
        }
        if (route.transport) {   // RecordUpsample: P_up and N'_up at the frame's extent
            look::Constants up = lc;
            up.size[0] = W; up.size[1] = H;
            dispatch(cmd, kUpsample, S(kUpsample, look_views(look_out.view), trace), &up, sizeof up, (W + 15) / 16,
                     (H + 15) / 16);
        }
        if (temporal) {   // FinishLookPass
            pl.hist.written ^= 1u;
            pl.hist.valid = true;
            pl.hist.whole_edit = whole_edit;
            pl.hist.last_ns = now;
        }
        return route;
    }
};

namespace {
VkFormat unorm_twin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}
}  // namespace

Stage::Stage(const HostDevice& host, const StageConfig& cfg) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.device = host.device; m.physical = host.physical; m.cfg = cfg;
    m.settings = cfg.settings;
    vkGetPhysicalDeviceMemoryProperties(host.physical, &m.mem);
    {
        VkPhysicalDevicePipelineRobustnessFeaturesEXT prf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &prf};
        vkGetPhysicalDeviceFeatures2(host.physical, &f2);
        m.pipeline_robustness = prf.pipelineRobustness && !(std::getenv("NR_RENODX_ROBUST") && std::atoi(std::getenv("NR_RENODX_ROBUST")));
    }
    if (cfg.present) {
        // present_path.hpp: the colour space decides, units are absolute (PQ's 10000 nits, scRGB's 80).
        m.hdr = cfg.present == 2 ? 2 : cfg.present == 3 ? 1 : 0;
        if (cfg.present == 2) m.units = {codec::Encoding::Pq, true, 10000.f, true};
        else if (cfg.present == 3) m.units = {codec::Encoding::Linear, true, 80.f, false};
    } else {
        m.hdr = frame_hdr_mode(cfg.frame_format, cfg.hdr_flag);
        m.units = interpret_source_units(m.settings, m.hdr);
    }

    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 512; dpi.poolSizeCount = 3; dpi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(m.device, &dpi, nullptr, &m.pool), "descriptor pool");

    const uint32_t v6 = sizeof(V6Constants), lg = sizeof(LegacyConstants);
    if (m.hdr) {
        m.load(m.programs[Impl::kLinearize], "v6_linearize", v6);
        m.load(m.programs[Impl::kAutoscale], "v6_autoscale", v6);
        m.load(m.programs[Impl::kExposureScale], "v6_exposure_scale", 7 * 4);
        m.load(m.programs[Impl::kCommitExposure], "v6_commit_exposure", v6);
        m.load(m.programs[Impl::kEncode6], "v6_encode", v6);
        m.load(m.programs[Impl::kResolve6], "v6_resolve", v6);
        m.load(m.programs[Impl::kReduce6], "v6_pedestal_reduce", v6);
        m.load(m.programs[Impl::kReduce6Unshaped], "v6_pedestal_reduce_unshaped", v6);
        m.load(m.programs[Impl::kCommit6], "v6_commit", v6);
        m.load(m.programs[Impl::kResolveCommit6], "v6_resolve_commit", v6);
    } else {
        m.load(m.programs[Impl::kEncodeL], "legacy_encode", lg);
        m.load(m.programs[Impl::kDecodeL], "legacy_decode", lg);
        m.load(m.programs[Impl::kReduceL], "legacy_reduce", lg);
        m.load(m.programs[Impl::kReduceLUnshaped], "legacy_reduce_unshaped", lg);
        m.load(m.programs[Impl::kApplyL], "legacy_apply", lg);
    }
    m.load(m.programs[Impl::kCompose], "look_compose", sizeof(look::Constants));
    if (cfg.nr_width < cfg.width || cfg.nr_height < cfg.height)
        m.load(m.programs[Impl::kUpsample], "look_upsample", sizeof(look::Constants));

    const uint32_t W = cfg.width, H = cfg.height, NW = cfg.nr_width, NH = cfg.nr_height;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const VkFormat frame = unorm_twin(cfg.frame_format);
    m.original = m.make_image(W, H, frame, VK_IMAGE_USAGE_SAMPLED_BIT);
    m.decoded_storage = m.storage_capable(frame);
    m.decoded = m.make_image(W, H, m.decoded_storage ? frame : VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    // Proxy, neural and look output are RGBA16F (dlssnr.hpp 5866, 5885), as are the v6 work surfaces.
    m.proxy = m.make_image(NW, NH, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    m.neural = m.make_image(NW, NH, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    m.look_out = m.make_image(NW, NH, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    m.block_mean = m.make_image((W + 31) / 32, (H + 31) / 32, VK_FORMAT_R32G32B32A32_SFLOAT, rw);
    if (NW < W || NH < H) {
        m.up_proxy = m.make_image(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
        m.up_neural = m.make_image(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    }
    if (m.hdr) {
        m.work0 = m.make_image(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
        m.work_a = m.make_image(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
        m.scale = m.make_image(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, rw);
        m.commit = m.make_image(1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, rw);
    }
    m.make_buffer(16 * 1024, &m.unread, &m.unread_memory);
    m.looks.resize(1);
    nr::logf("[nr] RenoDX stage: %ux%u (network %ux%u), %s, frame format %d%s", W, H, NW, NH,
             m.hdr == 0 ? "SDR (legacy codec)" : m.units.absolute ? "absolute HDR (v6 codec)" : "relative HDR (v6 codec)",
             int(cfg.frame_format), m.decoded_storage ? "" : ", result through RGBA16F and a blit");
}

Stage::~Stage() {
    Impl& m = *impl_;
    if (m.prof.pool) vkDestroyQueryPool(m.device, m.prof.pool, nullptr);
    if (m.probe_memory) { vkUnmapMemory(m.device, m.probe_memory); vkFreeMemory(m.device, m.probe_memory, nullptr); }
    if (m.probe) vkDestroyBuffer(m.device, m.probe, nullptr);
    for (auto& b : m.borrows) {
        if (b.in) vkDestroyImageView(m.device, b.in, nullptr);
        if (b.out) vkDestroyImageView(m.device, b.out, nullptr);
        if (b.motion) vkDestroyImageView(m.device, b.motion, nullptr);
        if (b.pool) vkDestroyDescriptorPool(m.device, b.pool, nullptr);
    }
    for (Image* im : {&m.original, &m.work0, &m.work_a, &m.work_b, &m.proxy, &m.neural, &m.look_out, &m.decoded,
                      &m.decoded_alt, &m.block_mean, &m.scale, &m.commit, &m.motion, &m.exposure, &m.up_proxy,
                      &m.up_neural, &m.band_a, &m.band_b, &m.band_max_a, &m.band_max_b, &m.work_p, &m.prep_keep})
        m.destroy(*im);
    for (auto& pl : m.looks)
        for (int i = 0; i < 2; ++i) { m.destroy(pl.history[i]); m.destroy(pl.history_z[i]); }
    for (auto& r : m.retired) m.destroy(r.second);
    for (Program& p : m.programs) {
        if (p.pipeline) vkDestroyPipeline(m.device, p.pipeline, nullptr);
        if (p.module) vkDestroyShaderModule(m.device, p.module, nullptr);
        if (p.layout) vkDestroyPipelineLayout(m.device, p.layout, nullptr);
        if (p.dsl) vkDestroyDescriptorSetLayout(m.device, p.dsl, nullptr);
    }
    if (m.pool) vkDestroyDescriptorPool(m.device, m.pool, nullptr);
    for (auto [b, mem] : {std::pair{m.prep_state, m.prep_state_memory}, std::pair{m.unread, m.unread_memory}}) {
        if (b) vkDestroyBuffer(m.device, b, nullptr);
        if (mem) vkFreeMemory(m.device, mem, nullptr);
    }
}

namespace {
// RenoDX's SettingEffect: a change of a kHistory setting restarts every history (RequestHistoryReset), of a
// kRecreate one recreates the features (RecreateFeatures). The keys as RenoDX's own table marks them, those this
// stage has; NRUnlockPasses (ours) as NRPasses.
bool history_settings_differ(const Settings& a, const Settings& b) {
    auto same3 = [](const auto& x, const auto& y) { return x[0] == y[0] && x[1] == y[1] && x[2] == y[2]; };
    return a.auto_mask != b.auto_mask || a.chained_history != b.chained_history || a.codec_mode != b.codec_mode ||
           a.depth_mode != b.depth_mode || a.feed_mode != b.feed_mode || a.intensity != b.intensity ||
           a.local_structure != b.local_structure || a.local_tone != b.local_tone ||
           a.mvec_scale_x != b.mvec_scale_x || a.mvec_scale_y != b.mvec_scale_y ||
           a.norm_governor != b.norm_governor || a.norm_slew_stops != b.norm_slew_stops ||
           a.pq_calibration != b.pq_calibration || a.paper_white_scale != b.paper_white_scale ||
           !same3(a.pass_auto_mask, b.pass_auto_mask) || !same3(a.pass_follow, b.pass_follow) ||
           !same3(a.pass_intensity, b.pass_intensity) || !same3(a.pass_local_tone, b.pass_local_tone) ||
           !same3(a.pass_skin_structure, b.pass_skin_structure) ||
           !same3(a.pass_local_structure, b.pass_local_structure) || !same3(a.pass_style, b.pass_style) ||
           a.present_encoding != b.present_encoding || a.present_guides != b.present_guides ||
           a.present_white_nits != b.present_white_nits || a.skin_independent != b.skin_independent ||
           a.skin_structure != b.skin_structure || a.style != b.style;
}
bool recreate_settings_differ(const Settings& a, const Settings& b) {
    return a.hook_point != b.hook_point || a.linear_unit_nits != b.linear_unit_nits || a.passes != b.passes ||
           a.unlock_passes != b.unlock_passes || a.source_encoding != b.source_encoding;
}
}  // namespace

void Stage::set(const Settings& s) {
    Impl& m = *impl_;
    if (recreate_settings_differ(m.settings, s)) m.recreate = true;
    else if (history_settings_differ(m.settings, s)) m.history_reset = true;
    if (m.cfg.present) {   // the Present path's units are its colour space's
        m.settings = s;
        return;
    }
    const codec::SourceUnits units = interpret_source_units(s, m.hdr);
    if (units.absolute != m.units.absolute)
        throw std::runtime_error("RenoDX stage: a change between relative and absolute units needs a new stage");
    m.settings = s;
    m.units = units;
}

uint8_t Stage::hdr_mode() const { return impl_->hdr; }
Stage::Facts Stage::facts() const {
    Facts f = impl_->facts;
    if (impl_->probe_mapped && f.hdr_mode && !f.absolute) {
        const float d = impl_->probe_mapped[0];
        if (d > 0.f && d < 1e9f) f.divisor = d;
    }
    return f;
}

void working_resolution(const Settings& s, uint32_t rw, uint32_t rh, uint32_t gw, uint32_t gh, uint32_t* nw,
                        uint32_t* nh) {
    if (s.follow_input_res && gw != 0 && gh != 0) {
        *nw = std::min(gw, rw);
        *nh = std::min(gh, rh);
    } else {
        const float scale = std::clamp(s.resolution_scale, 0.33f, 1.f);
        *nw = std::max(1u, uint32_t(std::lround(double(rw) * scale)));
        *nh = std::max(1u, uint32_t(std::lround(double(rh) * scale)));
    }
}

uint32_t Stage::record(VkCommandBuffer cmd, const StageFrame& fr, const Network& network) {
    Impl& m = *impl_;
    ++m.recordings;
    m.collect();
    const Settings& s = m.settings;
    const uint32_t W = m.cfg.width, H = m.cfg.height, NW = m.cfg.nr_width, NH = m.cfg.nr_height;
    const uint32_t gx = (W + 15) / 16, gy = (H + 15) / 16, nx = (NW + 15) / 16, ny = (NH + 15) / 16;
    const uint32_t passes = stack_passes(s);

    // The colour into the output as it is: NR off (NeuralUplift), or every pass at zero strength (RenoDX's explicit
    // bypass).
    if (!s.nr_enabled || (m.hdr && !m.chain_active(0, passes))) {
        if (fr.colour.image != fr.output.image) {
            Impl::image_barrier(cmd, fr.colour.image, fr.colour.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                fr.colour.before_stage, fr.colour.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT);
            Impl::image_barrier(cmd, fr.output.image, fr.output.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                fr.output.before_stage, fr.output.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {W, H, 1};
            vkCmdCopyImage(cmd, fr.colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.output.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            Impl::image_barrier(cmd, fr.output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fr.output.after,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, fr.output.after_stage,
                                fr.output.after_access);
            Impl::image_barrier(cmd, fr.colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.colour.after,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, fr.colour.after_stage, fr.colour.after_access);
        } else if (fr.colour.before != fr.colour.after) {
            Impl::image_barrier(cmd, fr.colour.image, fr.colour.before, fr.colour.after, fr.colour.before_stage,
                                fr.colour.before_access, fr.colour.after_stage, fr.colour.after_access);
        }
        m.recreate = true;
        return 0;
    }
    if (m.recreate) m.history_reset = true;

    // The stack's own images and look histories, as this frame's depth needs them.
    if (passes > 1) {
        const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if (m.hdr && !m.work_b.handle) m.work_b = m.make_image(W, H, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
        if (!m.hdr && !m.decoded_alt.handle) m.decoded_alt = m.make_image(W, H, m.decoded.format, rw);
    }
    while (m.looks.size() > passes) {
        for (int i = 0; i < 2; ++i) { m.retire(m.looks.back().history[i]); m.retire(m.looks.back().history_z[i]); }
        m.looks.pop_back();
    }
    m.looks.resize(passes);
    if (passes != m.logged_passes) {
        m.logged_passes = passes;
        nr::logf("[nr] RenoDX stage: %u model passes a frame", passes);
    }
    m.init_fresh(cmd);

    {
        const bool area = s.look_upsample == look::kUpsampleAreaInput;
        if (m.area_known && area != m.area_input) m.history_reset = true;
        m.area_input = area; m.area_known = true;
    }
    m.f6.hdr_mode = m.hdr;
    m.f6.units = m.units;
    m.f6.divisor = m.hdr ? frame_codec_divisor(s, m.hdr, m.units) : 0.f;
    // The Present path's own divisor, from the HDR diffuse white; Anchored (1) and Display (3) keep their anchors.
    if (m.cfg.present && m.hdr && s.codec_mode != 1u && s.codec_mode != 3u)
        m.f6.divisor = codec::PresentDivisor(m.units, fr.present_white_nits, s.paper_white_scale, s.pq_calibration);
    m.f6.encoding = v6_encoding_constant(m.units);
    m.f6.gate = v6_dark_gate(m.units);
    m.f6.cap = codec::BlackPedestalCap(m.units);

    // Preprocess this frame (runtime_prep's push block; RESET when it has just come on).
    const Preprocess& pp = fr.preprocess;
    m.prep_on = pp.active();
    if (m.prep_on != m.prep_was_on || (m.prep_on && pp != m.prep_last)) m.history_reset = true;
    if (m.prep_on) {
        m.ensure_prep();
        m.init_fresh(cmd);
        if (m.prep_state_fresh) {
            vkCmdFillBuffer(cmd, m.prep_state, 0, VK_WHOLE_SIZE, 0);
            Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            m.prep_state_fresh = false;
        }
        const uint32_t curve = uint32_t(std::clamp(pp.curve, 0, 6));
        const int64_t now = now_ns();
        m.prep_push = {m.hdr ? W : NW, m.hdr ? H : NH, 0u,
                       (pp.exposure == 1 ? uint32_t(kPrepAuto) : 0u) | (m.hdr ? uint32_t(kPrepLinear) : 0u) |
                           (m.prep_was_on ? 0u : uint32_t(kPrepReset)),
                       curve, pp.exposure == 0 ? 0.0f : std::clamp(pp.bias_ev, -8.0f, 8.0f),
                       std::clamp(pp.contrast, 0.5f, 2.0f), std::clamp(pp.saturation, 0.05f, 2.0f), kPrepAnchor[curve],
                       4.0f, m.prep_was_on ? float(now - m.prep_metered_ns) / 1e9f : 0.0f};
        m.prep_metered_ns = now;
    }
    m.prep_was_on = m.prep_on;
    m.prep_last = pp;
    const bool prep_exposure = m.prep_on && pp.exposure != 0;
    m.facts = Stage::Facts{};
    m.facts.hdr_mode = m.hdr;
    m.facts.absolute = m.units.absolute;
    m.facts.prep_exposure = prep_exposure;
    // Preprocess's exposure replaces RenoDX's divisor; a frame of absolute units then takes the N/P resolve too,
    // so that Preprocess's change comes out of it as it does on relative frames.
    m.ratio_resolve = m.prep_on && m.hdr && !frame_display_codec(s, m.hdr, m.units);
    if (prep_exposure && m.hdr) m.f6.divisor = 1.f;
    const std::map<uint32_t, VkBuffer> prep_state{{0, m.prep_state}};
    // Preprocess's forward half (HDR): the pass's running image -> work_p, its meter first on the frame (work0).
    const auto prep_forward_hdr = [&](const Image& from, bool meter) {
        PrepPush pp_push = m.prep_push;
        const VkDescriptorSet set = m.S(Impl::kPrep, {{1, from.view}, {2, m.work_p.view}, {3, m.work_p.view}}, prep_state);
        if (meter && pp.exposure == 1) m.dispatch(cmd, Impl::kPrep, set, &pp_push, sizeof pp_push, 1, 1);
        pp_push.mode = 1;
        m.dispatch(cmd, Impl::kPrep, set, &pp_push, sizeof pp_push, gx, gy);
    };

    m.prof_begin(cmd);
    // In place (HDR): the frame read where it is when it can be sampled as it is; the answer stored into the output
    // when the output takes storage in the answer's format. The frame may be the output itself when both hold: only
    // the linearize reads the frame, and the commit, the stage's last step, writes it.
    Impl::Borrow* borrow = nullptr;
    const bool same = fr.colour.image == fr.output.image;
    if (m.hdr) {
        const bool out_ok = m.decoded_storage && (fr.output.usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
                            fr.output.format == m.decoded.format && fr.output.width == W && fr.output.height == H;
        const bool in_ok = (!same || out_ok) && (fr.colour.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
                           fr.colour.format == m.original.format && fr.colour.width == W && fr.colour.height == H;
        if (in_ok || out_ok) {
            borrow = &m.borrow_slot();
            if (in_ok) borrow->in = m.view_of(fr.colour.image, fr.colour.format);
            if (out_ok) borrow->out = m.view_of(fr.output.image, fr.output.format);
        }
    }
    const bool read_in_place = borrow && borrow->in;
    const bool store_in_place = borrow && borrow->out;
    if (m.hdr && !m.in_place_logged) {
        m.in_place_logged = true;
        nr::logf("[nr] RenoDX stage: the frame %s, the answer %s", read_in_place ? "read in place" : "copied in",
                 store_in_place ? "stored into the output" : "copied out");
    }
    // The output in GENERAL for the HDR store into it (after the linearize's reads when it is the frame itself).
    const auto prepare_store = [&] {
        if (store_in_place && same && read_in_place)
            Impl::image_barrier(cmd, fr.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        else if (store_in_place)
            Impl::image_barrier(cmd, fr.output.image, fr.output.before, VK_IMAGE_LAYOUT_GENERAL, fr.output.before_stage,
                                fr.output.before_access, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    };
    // The last pass's resolve and the commit in one dispatch when the commit takes no pedestal (v6_resolve_commit:
    // the same output bits). NR_RENODX_FUSE=0 keeps the two (a measurement aid).
    static const bool fuse_off = [] { const char* e = std::getenv("NR_RENODX_FUSE"); return e && e[0] == '0'; }();
    const bool fuse = m.hdr && !fuse_off && !frame_pedestal_removal(s, m.hdr, m.units);
    bool fused = false;
    if (read_in_place) {
        Impl::image_barrier(cmd, fr.colour.image, fr.colour.before, VK_IMAGE_LAYOUT_GENERAL, fr.colour.before_stage,
                            fr.colour.before_access, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
    // 1. The frame into `original` (its own bits; an *_SRGB frame into the UNORM twin).
    Impl::image_barrier(cmd, fr.colour.image, fr.colour.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        fr.colour.before_stage, fr.colour.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
    Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {W, H, 1};
    vkCmdCopyImage(cmd, fr.colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.original.handle,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    m.mark(cmd, "copy-in");
    Impl::image_barrier(cmd, fr.colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.colour.before,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    if (m.hdr) {
        // 2a. linearize: original (or the frame itself) -> work0, the linear source everything after reads.
        const V6Constants lin = make_v6(s, m.f6, W, H, W, H, W, H, W, H, 0.f, 0.f, 0.f);
        const VkImageView src = read_in_place ? borrow->in : m.original.view;
        const std::map<uint32_t, VkImageView> lv{{0, src}, {1, src}, {2, src}, {3, src}, {4, m.work0.view}};
        m.dispatch(cmd, Impl::kLinearize,
                   read_in_place ? m.set_for(m.programs[Impl::kLinearize], lv, {}, borrow->pool) : m.S(Impl::kLinearize, lv),
                   &lin, sizeof lin, gx, gy);
        if (m.prep_on) prep_forward_hdr(m.work0, true);
        const Image& metered = m.prep_on ? m.work_p : m.work0;
        m.feed = kFeedV1;
        if (!m.units.absolute && !prep_exposure) {
            // 2b. this frame's scale texel: the v1 meter and the governor (feed v1).
            if (!m.commit_cleared) {
                Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                const VkClearColorValue zero{};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, m.commit.handle, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
                Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                m.commit_cleared = true;
            }
            // The float formats v6_exposure_scale reads (.r), as RenoDX's Vulkan path lists them.
            bool readable = false;
            switch (fr.exposure.image ? fr.exposure.format : VK_FORMAT_UNDEFINED) {
                case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
                case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                    readable = true;
                    break;
                default:
                    break;
            }
            m.feed = select_feed(s, m.hdr, m.units, readable, fr.auto_exposure || !fr.create_flags_known);
            m.facts.feed = m.feed == kFeedV1 ? 0 : m.feed == kFeedTexture ? 1 : 2;
            m.facts.exposure_texture = !fr.exposure.image ? 0 : readable ? 1 : 2;
            if (m.feed != m.logged_feed) {   // SelectFeed's line, once per decision
                m.logged_feed = m.feed;
                nr::logf("[nr] RenoDX stage: NR feed %s (NRFeedMode=%u; exposure texture %s, auto exposure %s, "
                         "pre-exposure %.4g, exposure scale %.4g)",
                         m.feed == kFeedV1 ? "v1 (the content meter)" : m.feed == kFeedTexture ? "v2 (the game's exposure)"
                                                                                       : "v2 (fixed 1)",
                         s.feed_mode, !fr.exposure.image ? "none" : readable ? "readable" : "unreadable",
                         !fr.create_flags_known ? "unknown" : fr.auto_exposure ? "1" : "0", fr.pre_exposure,
                         fr.exposure_scale);
            }
            const ScalePlan plan = plan_frame_scale(s, m.norm, fr.reset, m.feed, m.recreate || m.history_reset);
            if (m.feed == kFeedV1 || plan.guarded) {
                V6Constants meter = make_v6(s, m.f6, 1, 1, W, H, 1, 1, 1, 1, 0.f, 0.f, 0.f);
                meter.divisor = 0.f; meter.pedestal_cap = 0.f; meter.encoding = 1u; meter.dark_gate = 0.f;
                std::memcpy(&meter.slew_stops, plan.governor, sizeof plan.governor);
                m.dispatch(cmd, Impl::kAutoscale,
                           m.S(Impl::kAutoscale, {{0, metered.view}, {1, metered.view}, {2, metered.view},
                                                  {3, metered.view}, {4, m.scale.view}, {5, m.commit.view}}),
                           &meter, sizeof meter, 1, 1);
            }
            if (m.feed != kFeedV1) {
                const bool texture = m.feed == kFeedTexture && readable;
                if (texture) {
                    // Texel (0,0), the one v6_exposure_scale reads, into the stage's own image.
                    m.ensure_exposure(fr.exposure.format);
                    m.init_fresh(cmd);
                    Impl::image_barrier(cmd, fr.exposure.image, fr.exposure.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        fr.exposure.before_stage, fr.exposure.before_access,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                    Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                    VkImageCopy one{};
                    one.srcSubresource = one.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    one.extent = {1, 1, 1};
                    vkCmdCopyImage(cmd, fr.exposure.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.exposure.handle,
                                   VK_IMAGE_LAYOUT_GENERAL, 1, &one);
                    Impl::image_barrier(cmd, fr.exposure.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.exposure.after,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, fr.exposure.after_stage,
                                        fr.exposure.after_access);
                    Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                }
                // v6_exposure_scale's own cbuffer view: dwords 0..6. The fixed feed reads no texture: the metered
                // image fills the slot, as RenoDX's own sets do.
                const uint32_t feed_constants[7] = {texture ? uint32_t(kFeedTexture) : uint32_t(kFeedFixed),
                                                    bits(fr.pre_exposure > 0.f ? fr.pre_exposure : 1.f),
                                                    bits(fr.exposure_scale > 0.f ? fr.exposure_scale : 1.f),
                                                    bits(plan.v2_snap ? 1.f : 0.f), plan.guarded ? 1u : 0u, W, H};
                const VkImageView tex = texture ? m.exposure.view : metered.view;
                m.dispatch(cmd, Impl::kExposureScale,
                           m.S(Impl::kExposureScale, {{0, tex}, {1, metered.view}, {2, tex}, {3, tex}, {4, m.scale.view},
                                                      {5, m.commit.view}}),
                           feed_constants, sizeof feed_constants, 1, 1);
            }
        }
    }

    // 3. The stack: each pass encodes the running image, runs the network on it, shapes its edit and resolves it
    // onto that image with its own strengths; the next pass takes the result.
    const bool have_motion = fr.motion.image && fr.motion.width && fr.motion.height;
    const look::Settings ls = look::snapshot(s, fr.after_upscale && have_motion);
    bool motion_copied = false;
    // Read in place where the game's image stays in GENERAL and can be sampled (the compose reads it at the region's
    // base); else the vectors' region, copied to the origin of an image of its own (once a frame, every pass reads it).
    const bool motion_wanted = ls.stabilize == 2u && have_motion && look::dispatched(ls, false);
    if (motion_wanted && fr.motion.before == VK_IMAGE_LAYOUT_GENERAL && fr.motion.after == VK_IMAGE_LAYOUT_GENERAL &&
        (fr.motion.usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        if (!borrow) borrow = &m.borrow_slot();
        borrow->motion = m.view_of(fr.motion.image, fr.motion.format);
        motion_copied = borrow->motion != VK_NULL_HANDLE;
        if (motion_copied && !m.motion_logged) {
            m.motion_logged = true;
            nr::logf("[nr] RenoDX stage: Detail stability reads the motion vectors in place");
        }
    }
    if (motion_wanted && !motion_copied) {
        if (m.motion.w != fr.motion.width || m.motion.h != fr.motion.height || m.motion.format != fr.motion.format) {
            m.retire(m.motion);
            m.motion = m.make_image(fr.motion.width, fr.motion.height, fr.motion.format, VK_IMAGE_USAGE_SAMPLED_BIT);
            m.init_fresh(cmd);
        }
        Impl::image_barrier(cmd, fr.motion.image, fr.motion.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            fr.motion.before_stage, fr.motion.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
        Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy mc{};
        mc.srcSubresource = mc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        mc.srcOffset = {int32_t(fr.motion_x), int32_t(fr.motion_y), 0};
        mc.extent = {fr.motion.width, fr.motion.height, 1};
        vkCmdCopyImage(cmd, fr.motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.motion.handle,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &mc);
        Impl::image_barrier(cmd, fr.motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.motion.after,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, fr.motion.after_stage, fr.motion.after_access);
        Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        motion_copied = true;
    }
    const bool reset_all = fr.reset || m.history_reset;
    m.history_reset = false;
    m.recreate = false;
    ColourFrame p;
    p.image = m.proxy.handle; p.format = m.proxy.format; p.width = NW; p.height = NH;
    p.before = p.after = VK_IMAGE_LAYOUT_GENERAL;
    p.before_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; p.before_access = VK_ACCESS_SHADER_WRITE_BIT;
    p.after_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; p.after_access = VK_ACCESS_SHADER_READ_BIT;
    p.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ColourFrame n = p;
    n.image = m.neural.handle;
    n.before_access = VK_ACCESS_SHADER_READ_BIT;
    int32_t executed = -1;
    uint32_t ran = 0;
    float final_transfer = 0.f, final_color = 0.f;
    bool final_shaped = false;
    for (uint32_t k = 0; k < passes; ++k) {
        float transfer = 1.f, color = 1.f;
        m.strengths(k, &transfer, &color);
        const Image& ref = k == 0 ? (m.hdr ? m.work0 : m.original) : m.stack_out(k - 1);
        Image& write = m.stack_out(k);
        if (m.hdr && transfer <= 0.f && color <= 0.f) {
            // An explicit zero-strength pass carries the running image across exactly.
            Impl::copy_general(cmd, ref, write);
            executed = int32_t(k);
            final_transfer = final_color = 0.f;
            final_shaped = false;
            continue;
        }
        // 3a. encode: the running image -> the proxy at the network's extent.
        if (m.hdr) {
            if (m.prep_on && k > 0) prep_forward_hdr(ref, false);
            const Image& src = m.prep_on ? m.work_p : ref;
            if (m.prep_on && pp.curve != 0) {
                // Preprocess's curve in place of RenoDX's: the same footprint and sRGB, no toe or shoulder.
                const uint32_t push[4] = {NW, NH, W, H};
                m.dispatch(cmd, Impl::kPrepEncode, m.S(Impl::kPrepEncode, {{0, m.work_p.view}, {4, m.proxy.view}}), push,
                           sizeof push, nx, ny);
            } else {
                const V6Constants enc = make_v6(s, m.f6, NW, NH, W, H, NW, NH, NW, NH, transfer, color, 0.f);
                m.dispatch(cmd, Impl::kEncode6,
                           m.S(Impl::kEncode6, {{0, src.view}, {1, src.view}, {2, src.view}, {3, m.scale_or_work0()},
                                                {4, m.proxy.view}}),
                           &enc, sizeof enc, nx, ny);
            }
        } else {
            LegacyConstants c{};
            c.size[0] = NW; c.size[1] = NH; c.source_size[0] = W; c.source_size[1] = H;
            c.proxy_size[0] = c.neural_size[0] = NW; c.proxy_size[1] = c.neural_size[1] = NH;
            c.paper_white_scale = s.paper_white_scale; c.transfer_strength = transfer; c.color_strength = color;
            c.hdr = 0; c.diffuse_white_nits = s.diffuse_white_nits;
            // NRLookUpsample 3 (area input) feeds the network the area mean instead of a point, where the edit is
            // carried back by the transport (RenoDX sets it after the shared bind, dword 16).
            if (ls.upsample == look::kUpsampleAreaInput && m.up_proxy.handle && look::transport_wanted(ls, NW, NH, W, H))
                c.padding[0] = look::kAreaEncode;
            m.dispatch(cmd, Impl::kEncodeL,
                       m.S(Impl::kEncodeL, {{0, ref.view}, {1, ref.view}, {2, ref.view}, {3, ref.view}, {4, m.proxy.view}}),
                       &c, sizeof c, nx, ny);
            if (m.prep_on) {   // the network's input in place, P kept (runtime_prep.comp mode 1); the meter once a frame
                PrepPush pp_push = m.prep_push;
                const VkDescriptorSet set = m.S(Impl::kPrep, {{1, m.proxy.view}, {2, m.prep_keep.view}, {3, m.neural.view}},
                                                prep_state);
                if (k == 0 && pp.exposure == 1) m.dispatch(cmd, Impl::kPrep, set, &pp_push, sizeof pp_push, 1, 1);
                pp_push.mode = 1;
                m.dispatch(cmd, Impl::kPrep, set, &pp_push, sizeof pp_push, nx, ny);
            }
        }
        // 3b. the network.
        const bool reset = reset_all || (k > 0 && !s.chained_history);
        m.mark(cmd, "before-network");
        network(k, p, n, reset);
        m.mark(cmd, "network");
        ++ran;
        // Its answer, whoever wrote it, before the compose and the resolve read it.
        Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                 VK_ACCESS_TRANSFER_WRITE_BIT);
        // SDR with Preprocess: its change taken back out of the answer, P back into the proxy (mode 2, RESTORE).
        if (m.prep_on && !m.hdr) {
            PrepPush back = m.prep_push;
            back.mode = 2;
            back.flags = (back.flags & ~uint32_t(kPrepReset)) | uint32_t(kPrepRestore);
            m.dispatch(cmd, Impl::kPrep,
                       m.S(Impl::kPrep, {{1, m.proxy.view}, {2, m.prep_keep.view}, {3, m.neural.view}}, prep_state),
                       &back, sizeof back, nx, ny);
        }
        // 3c. the look: Adjust NR's look, Detail stability, edge-aware upsampling.
        const Impl::Route route = m.run_look(cmd, k, ref, ls, motion_copied, fr, reset, borrow);
        const VkImageView rp = route.transport ? m.up_proxy.view : m.proxy.view;
        const VkImageView rn = route.transport ? m.up_neural.view : route.shaped ? m.look_out.view : m.neural.view;
        const uint32_t pw = route.transport ? W : NW, ph = route.transport ? H : NH;
        // 3d. resolve onto the running image.
        if (m.hdr && fuse && k + 1 == passes) {
            const V6Constants res = make_v6(s, m.f6, W, H, W, H, pw, ph, pw, ph, transfer, color, s.chroma_clamp_stops,
                                            m.ratio_resolve);
            const std::map<uint32_t, VkImageView> fv{{0, ref.view}, {1, rp}, {2, rn}, {3, m.scale_or_work0()},
                                                     {4, store_in_place ? borrow->out : m.decoded.view}};
            prepare_store();
            m.dispatch(cmd, Impl::kResolveCommit6,
                       store_in_place ? m.set_for(m.programs[Impl::kResolveCommit6], fv, {}, borrow->pool)
                                      : m.S(Impl::kResolveCommit6, fv),
                       &res, sizeof res, gx, gy);
            fused = true;
        } else if (m.hdr) {
            const V6Constants res = make_v6(s, m.f6, W, H, W, H, pw, ph, pw, ph, transfer, color, s.chroma_clamp_stops,
                                            m.ratio_resolve);
            m.dispatch(cmd, Impl::kResolve6,
                       m.S(Impl::kResolve6, {{0, ref.view}, {1, rp}, {2, rn}, {3, m.scale_or_work0()}, {4, write.view}}),
                       &res, sizeof res, gx, gy);
        } else {
            LegacyConstants c{};
            c.size[0] = W; c.size[1] = H; c.source_size[0] = W; c.source_size[1] = H;
            c.proxy_size[0] = c.neural_size[0] = pw; c.proxy_size[1] = c.neural_size[1] = ph;
            c.paper_white_scale = s.paper_white_scale; c.transfer_strength = transfer; c.color_strength = color;
            c.hdr = 0; c.diffuse_white_nits = s.diffuse_white_nits;
            m.dispatch(cmd, Impl::kDecodeL,
                       m.S(Impl::kDecodeL, {{0, ref.view}, {1, rp}, {2, rn}, {3, ref.view}, {4, write.view}}), &c, sizeof c,
                       gx, gy);
            // RunBlackLevelRestore (D3D12 only in RenoDX, kept here on every route): the reduce on the pass's codec
            // set at the network's extent, the unshaped one when the look shaped the answer, then the apply.
            if (s.codec_mode != 0u) {
                c.proxy_size[0] = c.neural_size[0] = NW; c.proxy_size[1] = c.neural_size[1] = NH;
                const std::map<uint32_t, VkImageView> set{{0, ref.view}, {1, m.proxy.view}, {2, m.neural.view},
                                                          {3, ref.view}, {4, write.view}, {5, m.block_mean.view}};
                const Impl::P reduce = route.shaped ? Impl::kReduceLUnshaped : Impl::kReduceL;
                m.dispatch(cmd, reduce, m.S(reduce, set), &c, sizeof c, (W + 31) / 32, (H + 31) / 32);
                LegacyConstants a = c;
                a.padding[0] = s.sdr_black_restore == 1u ? 2.f : 0.061f;
                a.padding[1] = float(s.sdr_black_restore);
                m.dispatch(cmd, Impl::kApplyL, m.S(Impl::kApplyL, set), &a, sizeof a, gx, gy);
            }
        }
        executed = int32_t(k);
        final_transfer = transfer;
        final_color = color;
        final_shaped = route.shaped;
    }

    // 4. The last pass's result back into the frame's encoding (HDR: the pedestal block means - a single shaped
    // pass measures NR's own lift, unshaped - and the commit), then into the output.
    const Image* result = &m.decoded;
    if (m.hdr && fused) {
        // committed by the last pass's resolve
    } else if (m.hdr) {
        const Image& work_final = m.stack_out(uint32_t(executed));
        if (frame_pedestal_removal(s, m.hdr, m.units)) {
            const bool unshaped = executed == 0 && final_shaped;
            const V6Constants red = make_v6(s, m.f6, W, H, W, H, NW, NH, NW, NH, final_transfer, final_color,
                                            s.chroma_clamp_stops, m.ratio_resolve);
            const VkDescriptorSet set = unshaped
                ? m.S(Impl::kReduce6Unshaped, {{0, m.work0.view}, {1, m.proxy.view}, {2, m.neural.view},
                                               {3, m.scale_or_work0()}, {4, m.block_mean.view}})
                : m.S(Impl::kReduce6, {{0, work_final.view}, {1, m.work0.view}, {2, m.scale_or_work0()},
                                       {3, m.work0.view}, {4, m.block_mean.view}});
            m.dispatch(cmd, unshaped ? Impl::kReduce6Unshaped : Impl::kReduce6, set, &red, sizeof red, (W + 31) / 32,
                       (H + 31) / 32);
        }
        const V6Constants com = make_v6(s, m.f6, W, H, W, H, W, H, W, H, final_transfer, final_color, 0.f);
        const bool v2 = !m.units.absolute && m.feed != kFeedV1;
        const Impl::P commit = v2 ? Impl::kCommitExposure : Impl::kCommit6;
        const std::map<uint32_t, VkImageView> cv{{0, work_final.view}, {1, m.work0.view},
                                                 {2, v2 ? m.scale.view : m.scale_or_work0()}, {3, m.work0.view},
                                                 {4, store_in_place ? borrow->out : m.decoded.view}, {5, m.block_mean.view}};
        prepare_store();
        m.dispatch(cmd, commit, store_in_place ? m.set_for(m.programs[commit], cv, {}, borrow->pool) : m.S(commit, cv),
                   &com, sizeof com, gx, gy);
    } else {
        result = &m.stack_out(uint32_t(executed));
    }

    if (store_in_place) {
        Impl::image_barrier(cmd, fr.output.image, VK_IMAGE_LAYOUT_GENERAL, fr.output.after,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, fr.output.after_stage,
                            fr.output.after_access);
    } else {
    Impl::image_barrier(cmd, fr.output.image, fr.output.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        fr.output.before_stage, fr.output.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
    if (m.decoded_storage) {
        VkImageCopy out{};
        out.srcSubresource = out.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        out.extent = {W, H, 1};
        vkCmdCopyImage(cmd, result->handle, VK_IMAGE_LAYOUT_GENERAL, fr.output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &out);
    } else {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(W), int32_t(H), 1};
        vkCmdBlitImage(cmd, result->handle, VK_IMAGE_LAYOUT_GENERAL, fr.output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);
    }
    Impl::image_barrier(cmd, fr.output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fr.output.after,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, fr.output.after_stage,
                        fr.output.after_access);
    }
    if (read_in_place && !same)
        Impl::image_barrier(cmd, fr.colour.image, VK_IMAGE_LAYOUT_GENERAL, fr.colour.after, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0, fr.colour.after_stage, fr.colour.after_access);
    else if (fr.colour.image != fr.output.image && fr.colour.before != fr.colour.after)
        Impl::image_barrier(cmd, fr.colour.image, fr.colour.before, fr.colour.after, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            0, fr.colour.after_stage, fr.colour.after_access);
    // The next frame's writes into the stack's images after this copy read them.
    Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    m.mark(cmd, "copy-out");
    // the engine-data panel's divisor (Facts::divisor): the scale texel, every 32nd frame
    if (m.hdr && !m.units.absolute && m.scale.handle && (m.recordings & 31u) == 1u) {
        if (!m.probe) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = 256; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(m.device, &bi, nullptr, &m.probe) == VK_SUCCESS) {
                VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m.device, m.probe, &req);
                const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                uint32_t type = m.mem.memoryTypeCount;
                for (uint32_t i = 0; i < m.mem.memoryTypeCount; ++i)
                    if ((req.memoryTypeBits & (1u << i)) && (m.mem.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = type;
                void* p = nullptr;
                if (type == m.mem.memoryTypeCount || vkAllocateMemory(m.device, &ai, nullptr, &m.probe_memory) != VK_SUCCESS ||
                    vkBindBufferMemory(m.device, m.probe, m.probe_memory, 0) != VK_SUCCESS ||
                    vkMapMemory(m.device, m.probe_memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
                    if (m.probe_memory) vkFreeMemory(m.device, m.probe_memory, nullptr);
                    vkDestroyBuffer(m.device, m.probe, nullptr);
                    m.probe = VK_NULL_HANDLE; m.probe_memory = VK_NULL_HANDLE;
                } else {
                    m.probe_mapped = static_cast<float*>(p);
                    m.probe_mapped[0] = 0.f;
                }
            }
        }
        if (m.probe) {
            Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, m.scale.handle, VK_IMAGE_LAYOUT_GENERAL, m.probe, 1, &c);
            Impl::memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        }
    }
    m.prof_end();
    return ran;
}

}  // namespace nr::renodx
