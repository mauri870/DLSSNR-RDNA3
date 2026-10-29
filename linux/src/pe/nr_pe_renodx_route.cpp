#include "nr_pe_renodx_route.hpp"

#include <atomic>

namespace nr::pe::renodx_route {
namespace {
std::atomic<bool> hooked{false};
std::atomic<uint64_t> encodes{0};
std::atomic<bool> encode_hdr{false};
std::atomic<uint64_t> encode_extent{0};
}  // namespace

namespace {
thread_local int t_call_style = -1;   // -1: the process's (hooked); 0 / 1: this call's, from CallStyle
}  // namespace
bool installed() { return t_call_style >= 0 ? t_call_style == 1 : hooked.load(); }
void set_installed() { hooked.store(true); }
CallStyle::CallStyle(bool renodx) : previous(t_call_style) { t_call_style = renodx ? 1 : 0; }
CallStyle::~CallStyle() { t_call_style = previous; }
namespace {
thread_local const renodx::Settings* t_call_settings = nullptr;
}  // namespace
CallSettings::CallSettings(const renodx::Settings* settings) : previous(t_call_settings) { t_call_settings = settings; }
CallSettings::~CallSettings() { t_call_settings = previous; }
const renodx::Settings* call_settings() { return t_call_settings; }
namespace {
thread_local const Wilsjo2Facts* t_call_wilsjo2 = nullptr;
}  // namespace
CallWilsjo2::CallWilsjo2(const Wilsjo2Facts* facts) : previous(t_call_wilsjo2) { t_call_wilsjo2 = facts; }
CallWilsjo2::~CallWilsjo2() { t_call_wilsjo2 = previous; }
const Wilsjo2Facts* call_wilsjo2() { return t_call_wilsjo2; }
namespace {
thread_local int t_call_network = -1;
}  // namespace
CallNetwork::CallNetwork(int network) : previous(t_call_network) { t_call_network = network; }
CallNetwork::~CallNetwork() { t_call_network = previous; }
int call_network() { return t_call_network; }
namespace {
thread_local int t_call_layer = 0;
}  // namespace
CallLayer::CallLayer(int number) : previous(t_call_layer) { t_call_layer = number; }
CallLayer::~CallLayer() { t_call_layer = previous; }
int call_layer() { return t_call_layer; }

Encode last_encode() {
    const uint64_t e = encode_extent.load();
    return {encodes.load(), encode_hdr.load(), uint32_t(e), uint32_t(e >> 32)};
}
std::atomic<uint64_t> seen_count{0};
std::atomic<bool> seen_linear{false};
std::atomic<float> seen_white{0.f};
EncodeSeen encode_seen() { return {seen_count.load(), seen_linear.load(), seen_white.load()}; }
void note_encode_seen(bool linear, float white_point) {
    seen_linear.store(linear);
    seen_white.store(white_point);
    seen_count.fetch_add(1);
}
void note_encode(bool hdr, uint32_t width, uint32_t height) {
    encode_hdr.store(hdr);
    encode_extent.store(uint64_t(width) | uint64_t(height) << 32);
    encodes.fetch_add(1);
}

namespace {
// The render thread's: OptiScaler builds the pass (MakeDlssNrPass), runs it (State::Run) and the model
// evaluates inside that run, all on the thread that called the game's DLSS evaluate.
thread_local Frame t_frame{};
thread_local bool t_pass_fresh = false;
thread_local uint32_t t_flags = 0;
thread_local float t_scale = 1.0f;
thread_local uint32_t t_sizes[4] = {};
thread_local void* t_direct = nullptr;
}  // namespace

Frame current_frame() { return t_frame; }
void set_direct(void* frame) { t_direct = frame; }
void* direct() { return t_direct; }
void begin_run(const Frame& f) {
    t_frame = f;
    t_frame.known = true;
    t_frame.flags_known = t_pass_fresh;
    t_frame.create_flags = t_pass_fresh ? t_flags : 0;
    t_frame.exposure_scale = t_pass_fresh ? t_scale : 1.0f;
    if (t_pass_fresh) {
        t_frame.render_width = t_sizes[0]; t_frame.render_height = t_sizes[1];
        t_frame.output_width = t_sizes[2]; t_frame.output_height = t_sizes[3];
    }
    t_pass_fresh = false;
    t_direct = nullptr;
}
void end_run() {
    t_frame = Frame{};
    t_direct = nullptr;
}
void note_pass(uint32_t flags, float scale, uint32_t render_width, uint32_t render_height, uint32_t output_width,
               uint32_t output_height) {
    t_sizes[0] = render_width; t_sizes[1] = render_height; t_sizes[2] = output_width; t_sizes[3] = output_height;
    t_flags = flags;
    t_scale = scale > 0.0f && scale == scale ? scale : 1.0f;
    t_pass_fresh = true;
}

}  // namespace nr::pe::renodx_route
