// wmma_half_probe <wmma_half_probe.spv>
// How the wave32 WMMA of this GPU uses the two lane halves of its operands. A loaded A or B
// fragment holds the same sixteen values in lanes l and l^16; the probe overwrites one half and
// multiplies by an identity matrix, so the product shows which half each element came from.
//
// What an RX 7900 XTX (gfx1100, RADV) answers, and the RDNA3 kernels rely on: output row m is
// computed from lane half m%2 alone - that half's A row m and that half's copy of every B
// column. So a B operand must hold all sixteen k in both halves, and the two halves may hold
// them in different k orders as long as the A rows that half uses are in the same order
// (fswin_t.comp, NR_OPPUT). A part that answers differently cannot run these kernels.
#include "vkrun.hpp"
#include <cstdint>

static uint16_t half_of_small_integer(float f) {      // exact for the values used here
    uint32_t u; std::memcpy(&u, &f, 4);
    if (f == 0.0f) return 0;
    const int exponent = int((u >> 23) & 255) - 127 + 15;
    return uint16_t((exponent << 10) | ((u & 0x7FFFFF) >> 13));
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Ctx ctx = mk();
    std::vector<Buf> buffers = {mkbuf(ctx, 512 * 2), mkbuf(ctx, 4 * 256 * 4)};
    uint16_t* in = static_cast<uint16_t*>(buffers[0].p);
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) {
            in[i * 16 + j] = half_of_small_integer(i == j ? 1.0f : 0.0f);
            in[256 + i * 16 + j] = half_of_small_integer(float(16 * i + j + 1));
        }
    std::memset(buffers[1].p, 0, buffers[1].n);
    run(ctx, argv[1], buffers, 4);
    const float* out = static_cast<const float*>(buffers[1].p);
    // Element (i, j) of mode m: the data value when row i was computed from the unmarked half,
    // the marker 1000 + k when it was computed from the marked half (B: k = i, since the
    // identity A picks B's row i; A: k = j, since the marked A row i is multiplied by the identity).
    long wrong = 0;
    for (int mode = 0; mode < 4; ++mode)
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 16; ++j) {
                const float got = out[mode * 256 + i * 16 + j];
                const bool from_upper = (i & 1) != 0;
                const bool marked = mode == 0 || mode == 2 ? from_upper : !from_upper;
                const float want = marked ? 1000.0f + float(mode < 2 ? i : j) : float(16 * i + j + 1);
                wrong += got != want;
            }
    std::printf("wmma lane halves: output row m from lane half m%%2 of both operands: %s (%ld of 1024 elements differ)\n",
                wrong ? "NO" : "yes", wrong);
    return wrong != 0;
}
