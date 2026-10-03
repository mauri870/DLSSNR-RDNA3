// run_frame <root> <in.rgba8> <width> <height> <out.rgba8> [timed passes [model scale]]
//
// One network pass through nr::Runtime on the first discrete GPU, the way a game calls it: an
// 8-bit RGBA frame in, the defaults of nr::Controls, an 8-bit frame out. <root> holds
// dlssnr-amd/{shaders,dlssnr.bin} (the installed layout). The saved output is the second pass,
// on a re-uploaded input, so the cold first pass (pipeline compilation) is never compared.
// With timed passes, prints the mean wall time of a submitted pass over them. The model scale
// (default 1) is RuntimeConfig::model_scale: the network runs at that fraction of the frame.
// RUN_FRAME_FORMAT=fp16 hands the runtime the frame as R16G16B16A16_SFLOAT with sampled and storage
// usage, the way an engine does: the network then reads the caller's image in place and writes its answer
// straight into it (the OptiScaler route's path), which the 8-bit default never takes. The saved output is
// converted back to 8 bits. RUN_FRAME_ENGINE=1 (with fp16) goes through the engine path an upscaler host uses
// (Runtime::record_engine with an R16G16_SFLOAT motion image of zero vectors and the temporal variants), which
// samples the colour and motion in place with descriptor sets of its own.
#include "nr_runtime.hpp"
#include "nrvk.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

static uint16_t to_half(float f) {      // round to nearest even, values in [0, 1]
    uint32_t u; std::memcpy(&u, &f, 4);
    if (f <= 0.0f) return 0;
    const int exponent = int((u >> 23) & 0xFF) - 127 + 15;
    if (exponent <= 0) {                   // subnormal half: at most 2^-14
        const uint32_t m = (u & 0x7FFFFFu) | 0x800000u;
        const int shift = 14 - exponent;
        if (shift > 24) return 0;
        uint32_t h = m >> shift, rest = m & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1u))) ++h;
        return uint16_t(h);
    }
    uint32_t h = (uint32_t(exponent) << 10) | ((u & 0x7FFFFFu) >> 13);
    const uint32_t rest = u & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;
    return uint16_t(h);
}
static float from_half(uint16_t h) {
    const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) return std::ldexp(float(m), -24);
    return std::ldexp(float(m | 0x400), e - 25);
}

int main(int argc, char** argv) try {
    if (argc < 6) { std::fprintf(stderr, "usage: run_frame <root> <in.rgba8> <w> <h> <out.rgba8> [timed passes]\n"); return 2; }
    const uint32_t width = uint32_t(atoi(argv[3])), height = uint32_t(atoi(argv[4]));
    const int timed = argc > 6 ? atoi(argv[6]) : 0;
    const float scale = argc > 7 ? float(atof(argv[7])) : 1.0f;
    std::ifstream file(argv[2], std::ios::binary);
    const std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});
    if (input.size() != size_t(width) * height * 4) { std::fprintf(stderr, "input is not %ux%u RGBA8\n", width, height); return 2; }

    const bool engine = std::getenv("RUN_FRAME_ENGINE") && std::atoi(std::getenv("RUN_FRAME_ENGINE")) == 1;
    const bool fp16 = engine || (std::getenv("RUN_FRAME_FORMAT") && !std::strcmp(std::getenv("RUN_FRAME_FORMAT"), "fp16"));
    const VkFormat format = fp16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint16_t> input_half;
    if (fp16) {
        input_half.resize(input.size());
        for (size_t i = 0; i < input.size(); ++i) input_half[i] = to_half(float(input[i]) / 255.0f);
    }
    const void* upload_data = fp16 ? static_cast<const void*>(input_half.data()) : static_cast<const void*>(input.data());
    const size_t upload_bytes = fp16 ? input_half.size() * 2 : input.size();

    nrvk::Context ctx;
    ctx.create();
    auto image = ctx.image(width, height, format, fp16, true, true);
    ctx.upload(image, upload_data, upload_bytes);

    nr::HostDevice host;
    host.instance = ctx.instance; host.physical = ctx.physical; host.device = ctx.device;
    host.queue = ctx.queue; host.queue_family = ctx.family;
    nr::RuntimeConfig config;
    config.root = argv[1]; config.width = width; config.height = height;
    config.colour_format = format;
    config.model_scale = scale;
    nr::TemporalConfig temporal;
    temporal.enable = engine;
    temporal.shaders = std::string(argv[1]) + "/dlssnr-amd/shaders/temporal";
    nr::Runtime runtime(host, config, nr::ControlMaskConfig{}, temporal);

    nr::ColourFrame frame;
    frame.image = image.handle; frame.format = format; frame.width = width; frame.height = height;
    frame.before = image.layout; frame.after = image.layout;
    frame.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  (fp16 ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const nr::Controls controls;
    nrvk::Context::Image motion_image{};
    nr::EngineFrame engine_frame;
    if (engine) {
        motion_image = ctx.image(width, height, VK_FORMAT_R16G16_SFLOAT, true, true, true);
        const std::vector<uint8_t> zero(size_t(width) * height * 4, 0);
        ctx.upload(motion_image, zero.data(), zero.size());
        engine_frame.colour = frame;
        engine_frame.motion.image = motion_image.handle; engine_frame.motion.format = VK_FORMAT_R16G16_SFLOAT;
        engine_frame.motion.width = width; engine_frame.motion.height = height;
        engine_frame.motion.before = motion_image.layout; engine_frame.motion.after = motion_image.layout;
        engine_frame.motion.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        engine_frame.feature = 1;
    }
    auto pass = [&] {
        nr::RecordResult result{};
        if (engine) {
            ctx.one_shot([&](VkCommandBuffer cmd) { result = runtime.record_engine(cmd, engine_frame, controls).frame; });
            engine_frame.reset = false;
        } else {
            ctx.one_shot([&](VkCommandBuffer cmd) { result = runtime.record(cmd, frame, controls); });
        }
        if (!result.applied) throw std::runtime_error("the network was not applied");
    };

    pass();                                              // cold: pipelines, first touch
    ctx.upload(image, upload_data, upload_bytes);
    pass();                                              // the pass whose output is compared
    std::vector<uint8_t> output(input.size());
    if (fp16) {
        std::vector<uint16_t> half(input.size());
        ctx.download(image, half.data(), half.size() * 2);
        for (size_t i = 0; i < half.size(); ++i)
            output[i] = uint8_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, from_half(half[i]))) * 255.0f));
    } else {
        ctx.download(image, output.data(), output.size());
    }
    std::ofstream(argv[5], std::ios::binary).write(reinterpret_cast<const char*>(output.data()), std::streamsize(output.size()));

    if (timed > 0) {
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < timed; ++i) pass();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::printf("frame_ms %.3f over %d passes\n", ms / timed, timed);
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
}
