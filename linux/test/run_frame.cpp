// run_frame <root> <in.rgba8> <width> <height> <out.rgba8> [timed passes [model scale]]
//
// One network pass through nr::Runtime on the first discrete GPU, the way a game calls it: an
// 8-bit RGBA frame in, the defaults of nr::Controls, an 8-bit frame out. <root> holds
// dlssnr-amd/{shaders,dlssnr.bin} (the installed layout). The saved output is the second pass,
// on a re-uploaded input, so the cold first pass (pipeline compilation) is never compared.
// With timed passes, prints the mean wall time of a submitted pass over them. The model scale
// (default 1) is RuntimeConfig::model_scale: the network runs at that fraction of the frame.
#include "nr_runtime.hpp"
#include "nrvk.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) try {
    if (argc < 6) { std::fprintf(stderr, "usage: run_frame <root> <in.rgba8> <w> <h> <out.rgba8> [timed passes]\n"); return 2; }
    const uint32_t width = uint32_t(atoi(argv[3])), height = uint32_t(atoi(argv[4]));
    const int timed = argc > 6 ? atoi(argv[6]) : 0;
    const float scale = argc > 7 ? float(atof(argv[7])) : 1.0f;
    std::ifstream file(argv[2], std::ios::binary);
    const std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});
    if (input.size() != size_t(width) * height * 4) { std::fprintf(stderr, "input is not %ux%u RGBA8\n", width, height); return 2; }

    nrvk::Context ctx;
    ctx.create();
    auto image = ctx.image(width, height, VK_FORMAT_R8G8B8A8_UNORM, false, true, true);
    ctx.upload(image, input.data(), input.size());

    nr::HostDevice host;
    host.instance = ctx.instance; host.physical = ctx.physical; host.device = ctx.device;
    host.queue = ctx.queue; host.queue_family = ctx.family;
    nr::RuntimeConfig config;
    config.root = argv[1]; config.width = width; config.height = height;
    config.colour_format = VK_FORMAT_R8G8B8A8_UNORM;
    config.model_scale = scale;
    nr::Runtime runtime(host, config);

    nr::ColourFrame frame;
    frame.image = image.handle; frame.format = VK_FORMAT_R8G8B8A8_UNORM; frame.width = width; frame.height = height;
    frame.before = image.layout; frame.after = image.layout;
    frame.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const nr::Controls controls;
    auto pass = [&] {
        nr::RecordResult result{};
        ctx.one_shot([&](VkCommandBuffer cmd) { result = runtime.record(cmd, frame, controls); });
        if (!result.applied) throw std::runtime_error("the network was not applied");
    };

    pass();                                              // cold: pipelines, first touch
    ctx.upload(image, input.data(), input.size());
    pass();                                              // the pass whose output is compared
    std::vector<uint8_t> output(input.size());
    ctx.download(image, output.data(), output.size());
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
