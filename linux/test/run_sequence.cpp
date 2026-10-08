// run_sequence <root> <width> <height> <frames> <reuse_every> <in %02d.rgba8> <motion %02d.f32> <out %02d.rgba8>
//
// Consecutive frames through nr::Runtime::record_engine, the way an upscaler host feeds them: the frame as
// FP16, and per frame the engine's backward motion (this frame's pixel -> where it was in the previous frame,
// as a uv offset: two floats a pixel, in <motion>; frame 0's is not read). With reuse_every >= 2 the runtime
// runs the network on every Nth frame and carries its edit across the others (RuntimeConfig::reuse). The
// runtime is always built with reuse allowed, so reuse_every 0 or 1 - the network on every frame - takes the
// same paths and is the reference to score the others against. Prints what each frame cost (wall time of
// the submit, GPU idle) and whether it was reused; writes each frame's answer as RGBA8.
#include "nr_runtime.hpp"
#include "nrvk.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

static uint16_t to_half(float f) {      // round to nearest even, signed, flushes below the half normal range
    uint32_t u; std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    const int exponent = int((u >> 23) & 0xFF) - 127 + 15;
    if (exponent <= 0) return uint16_t(sign);
    if (exponent >= 31) return uint16_t(sign | 0x7BFFu);
    uint32_t h = (uint32_t(exponent) << 10) | ((u & 0x7FFFFFu) >> 13);
    const uint32_t rest = u & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return uint16_t(sign | h);
}
static float from_half(uint16_t h) {
    const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 0x400), e - 25);
    return (h & 0x8000) ? -v : v;
}
static std::vector<uint8_t> slurp(const char* pattern, int index) {
    char path[1024]; std::snprintf(path, sizeof path, pattern, index);
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("cannot read ") + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

int main(int argc, char** argv) try {
    if (argc < 9) { std::fprintf(stderr, "usage: run_sequence <root> <w> <h> <frames> <reuse_every> <in> <motion> <out>\n"); return 2; }
    const uint32_t width = uint32_t(atoi(argv[2])), height = uint32_t(atoi(argv[3]));
    const int frames = atoi(argv[4]), reuse_every = atoi(argv[5]);
    const char *in_pattern = argv[6], *motion_pattern = argv[7], *out_pattern = argv[8];

    nrvk::Context ctx;
    ctx.create();
    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    auto image = ctx.image(width, height, format, true, true, true);
    auto motion_image = ctx.image(width, height, VK_FORMAT_R16G16_SFLOAT, true, true, true);

    nr::HostDevice host;
    host.instance = ctx.instance; host.physical = ctx.physical; host.device = ctx.device;
    host.queue = ctx.queue; host.queue_family = ctx.family;
    nr::RuntimeConfig config;
    config.root = argv[1]; config.width = width; config.height = height;
    config.colour_format = format;
    config.native_compose = std::getenv("RUN_SEQUENCE_NATIVE") != nullptr;
    // RUN_SEQUENCE_NOREUSE=1 builds the runtime as it was before temporal reuse (direct sampling and storing
    // on), to price what allowing reuse costs the frames that run the network.
    config.reuse = !std::getenv("RUN_SEQUENCE_NOREUSE");
    nr::TemporalConfig temporal;
    temporal.enable = true;
    temporal.shaders = std::string(argv[1]) + "/dlssnr-amd/shaders/temporal";
    nr::Runtime runtime(host, config, nr::ControlMaskConfig{}, temporal);

    nr::EngineFrame frame;
    frame.colour.image = image.handle; frame.colour.format = format; frame.colour.width = width; frame.colour.height = height;
    frame.colour.before = image.layout; frame.colour.after = image.layout;
    frame.colour.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    frame.motion.image = motion_image.handle; frame.motion.format = VK_FORMAT_R16G16_SFLOAT;
    frame.motion.width = width; frame.motion.height = height;
    frame.motion.before = motion_image.layout; frame.motion.after = motion_image.layout;
    frame.motion.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    frame.feature = 1;
    nr::Controls controls;
    controls.reuse_every = reuse_every;
    if (const char* gate = std::getenv("RUN_SEQUENCE_GATE")) controls.reuse_gate = float(atof(gate));

    // RUN_SEQUENCE_ESTIMATOR=1: no motion from the "engine" - the runtime's own estimator finds it from successive
    // frames (record_temporal), which is what a game with no motion vectors gets on the ReShade route.
    const bool estimator = std::getenv("RUN_SEQUENCE_ESTIMATOR") != nullptr;
    auto record = [&](VkCommandBuffer cmd) {
        if (estimator) {
            nr::EngineResult r{};
            r.frame = runtime.record_temporal(cmd, frame.colour, controls, nr::TemporalFrame{frame.reset, frame.feature}).frame;
            return r;
        }
        return runtime.record_engine(cmd, frame, controls);
    };
    std::printf("%5s %8s %6s %6s %9s\n", "frame", "reused", "gated", "disp", "wall_ms");
    for (int i = 0; i < frames; ++i) {
        const auto rgba = slurp(in_pattern, i);
        if (rgba.size() != size_t(width) * height * 4) throw std::runtime_error("input is not width x height RGBA8");
        std::vector<uint16_t> half(rgba.size());
        for (size_t k = 0; k < rgba.size(); ++k) half[k] = to_half(float(rgba[k]) / 255.0f);
        ctx.upload(image, half.data(), half.size() * 2);
        std::vector<uint16_t> mv(size_t(width) * height * 2, 0);
        if (i > 0) {
            const auto raw = slurp(motion_pattern, i);
            if (raw.size() != mv.size() * 4) throw std::runtime_error("motion is not width x height x 2 floats");
            const float* f = reinterpret_cast<const float*>(raw.data());
            for (size_t k = 0; k < mv.size(); ++k) mv[k] = to_half(f[k]);
        }
        ctx.upload(motion_image, mv.data(), mv.size() * 2);
        frame.reset = i == 0;
        nr::EngineResult result{};
        const auto begin = std::chrono::steady_clock::now();
        ctx.one_shot([&](VkCommandBuffer cmd) { result = record(cmd); });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::printf("%5d %8s %6s %6u %9.3f\n", i, result.frame.reused ? "yes" : "no", result.frame.reuse_gated ? "yes" : "no",
                    result.frame.network_dispatches, ms);
        std::vector<uint16_t> out(rgba.size());
        ctx.download(image, out.data(), out.size() * 2);
        std::vector<uint8_t> bytes(rgba.size());
        for (size_t k = 0; k < out.size(); ++k)
            bytes[k] = uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, from_half(out[k]))) * 255.0f));
        char path[1024]; std::snprintf(path, sizeof path, out_pattern, i);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    // RUN_SEQUENCE_LOOP=L: time the sequence back to back, L warm-up loops and L timed ones, with no upload or
    // readback between frames - the way a game submits, so the clocks stay up. Prints the cost of a frame that
    // runs the network, of one that skips it and the average over the loop.
    if (const char* loop = std::getenv("RUN_SEQUENCE_LOOP")) {
        const int loops = std::max(1, atoi(loop));
        double network_ms = 0, skip_ms = 0, total_ms = 0;
        int network_count = 0, skip_count = 0;
        for (int l = 0; l < 2 * loops; ++l)
            for (int i = 0; i < frames; ++i) {
                frame.reset = false;
                nr::EngineResult result{};
                const auto begin = std::chrono::steady_clock::now();
                ctx.one_shot([&](VkCommandBuffer cmd) { result = record(cmd); });
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                if (l < loops) continue;
                total_ms += ms;
                if (result.frame.reused) { skip_ms += ms; ++skip_count; } else { network_ms += ms; ++network_count; }
            }
        std::printf("timed %d frames: network frames %.3f ms (%d), skipped frames %.3f ms (%d), average %.3f ms\n",
                    loops * frames, network_count ? network_ms / network_count : 0.0, network_count,
                    skip_count ? skip_ms / skip_count : 0.0, skip_count, total_ms / (loops * frames));
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
}
