// e4m3_emul_test <e4m3_emul_test.spv>
// The RDNA3 e4m3 quantiser and decoder (linux/shaders/rdna3/include/e4m3_emul.glsl) against the
// host's reference (tinlayout.hpp), over all 65536 f16 inputs and all 256 codes, on the GPU.
#include "vkrun.hpp"
#include "tinlayout.hpp"
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Ctx ctx = mk();
    const size_t count = 65536 + 256;
    std::vector<Buf> buffers = {mkbuf(ctx, count * 4), mkbuf(ctx, count * 4)};
    for (uint32_t i = 0; i < 65536; ++i) static_cast<uint32_t*>(buffers[0].p)[i] = i;
    run(ctx, argv[1], buffers, uint32_t(count / 64));
    const float* got = static_cast<const float*>(buffers[1].p);
    auto same = [](float want, float have) { return std::isnan(want) ? std::isnan(have) : want == have; };
    long quantise = 0, decode = 0;
    for (uint32_t i = 0; i < 65536; ++i)
        quantise += !same(tin::e4m3_to_f(tin::f_to_e4m3(tin::f16_to_f(uint16_t(i)))), got[i]);
    for (int code = 0; code < 256; ++code)
        decode += !same(tin::e4m3_to_f(uint8_t(code)), got[65536 + code]);
    std::printf("e4m3 quantise mismatches %ld / 65536, decode mismatches %ld / 256\n", quantise, decode);
    return quantise || decode;
}
