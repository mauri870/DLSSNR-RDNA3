// Matrix throughput of the first discrete GPU: 16x16x16 cooperative-matrix multiply-adds in f16 (f32 accumulator)
// and int8 (int32 accumulator). run.sh builds the four shaders this loads (f1, f2: f16 with 5000 and 20000 loop
// iterations; i1, i2: the same in int8) and this program. Each pass is timed from the host, including pipeline
// creation, so the rate comes from the difference between the two loop lengths, which cancels that cost.
// On an RX 7900 XTX both types run at about 132 to 136 T(FL)OPS: int8 is not faster than f16 on RDNA3.
#include "../vkrun.hpp"
#include <chrono>

int main() {
    Ctx ctx = mk();
    const uint32_t workgroups = 96 * 8;         // 96 compute units, eight workgroups each
    Buf out = mkbuf(ctx, size_t(workgroups) * 512 * 4 + 4096), in = mkbuf(ctx, 1 << 16);
    memset(in.p, 1, in.n);
    memset(out.p, 0, out.n);
    std::vector<Buf> buffers{out, in};
    auto time_ms = [&](const char* spv) {
        const auto start = std::chrono::steady_clock::now();
        run(ctx, spv, buffers, workgroups);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    // 128 invocations a workgroup = 4 waves; 8 independent accumulators a wave; 2*16*16*16 operations a multiply-add.
    const double operations = double(workgroups) * 4 * 8 * 2 * 16 * 16 * 16 * (20000 - 5000);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const double f16_short = time_ms("f1.spv"), f16_long = time_ms("f2.spv");
        const double int8_short = time_ms("i1.spv"), int8_long = time_ms("i2.spv");
        std::printf("f16: %.1f ms -> %.1f ms  %.1f TFLOPS | int8: %.1f ms -> %.1f ms  %.1f TOPS\n",
                    f16_short, f16_long, operations / ((f16_long - f16_short) * 1e-3) / 1e12,
                    int8_short, int8_long, operations / ((int8_long - int8_short) * 1e-3) / 1e12);
    }
}
