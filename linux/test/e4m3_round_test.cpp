// e4m3_round_test <e4m3_round_test.spv>
// nr_e4m3_round over all 2^32 f32 bit patterns and nr_e4m3_round_pair over all 2^32 pairs of f16 bit
// patterns, on the GPU, against the bit-level reference in the shader. NaN inputs are skipped.
#include "vkrun.hpp"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Ctx ctx = mk();
    std::vector<Buf> buffers = {mkbuf(ctx, 8), mkbuf(ctx, 16)};
    auto* params = static_cast<uint32_t*>(buffers[0].p);
    auto* result = static_cast<uint32_t*>(buffers[1].p);
    const uint32_t chunk = 1u << 24;
    int bad = 0;
    for (uint32_t mode = 0; mode < 2; ++mode) {
        result[0] = result[1] = result[2] = result[3] = 0;
        params[1] = mode;
        for (uint64_t base = 0; base < (1ull << 32); base += chunk) {
            params[0] = uint32_t(base);
            run(ctx, argv[1], buffers, chunk / 64);
        }
        std::printf("%s: %u mismatches over 2^32 inputs", mode ? "f16 pairs" : "f32", result[0]);
        if (result[0]) std::printf(" (first: input %08x, got %08x, want %08x)", result[1], result[2], result[3]);
        std::printf("\n");
        bad += result[0] != 0;
    }
    return bad;
}
