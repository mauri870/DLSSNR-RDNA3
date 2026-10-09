// Creates every int4 pipeline of an int4 network directory (the g_<name>.spv that have a g_<name>.spv.iu4 marker)
// on the first discrete GPU, with the WMMA rewrite of nrvk.hpp, and prints how many WMMAs each rewrote. A pipeline is
// made when a frame first needs it, so a frame leaves untested the ones the persistent kernels replace; this does not.
//   int4_pipelines <int4 shaders dir>
#include "nrvk.hpp"
#include <algorithm>
#include <filesystem>

int main(int argc, char** argv) try {
    if (argc != 2) { std::fprintf(stderr, "usage: %s <int4 shaders dir>\n", argv[0]); return 2; }
    nrvk::Context ctx;
    ctx.create();
    if (!ctx.pipeline_binary) { std::fprintf(stderr, "the device has no VK_KHR_pipeline_binary\n"); return 1; }
    auto buffer = ctx.buffer(1 << 20);
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        if (e.path().extension() == ".iu4") { std::filesystem::path spv = e.path(); spv.replace_extension(""); names.push_back(spv.string()); }
    std::sort(names.begin(), names.end());
    int failed = 0;
    for (const auto& spv : names) {
        nrvk::Kernel kernel;
        try {
            kernel.create(ctx, spv, std::vector<VkBuffer>(8, buffer.handle), 256);
            std::printf("%-28s %5d WMMAs rewritten\n", std::filesystem::path(spv).filename().string().c_str(), kernel.iu4_rewrites);
            kernel.destroy();
        } catch (const std::exception& error) {
            std::printf("%-28s FAIL %s\n", std::filesystem::path(spv).filename().string().c_str(), error.what());
            ++failed;
        }
    }
    std::printf("%zu int4 pipelines, %d failed\n", names.size(), failed);
    return failed ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
