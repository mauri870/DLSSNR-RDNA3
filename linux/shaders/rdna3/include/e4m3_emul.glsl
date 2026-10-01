// e4m3 on a part with no FP8: the value set is kept, the storage is FP16.
//
// RDNA3 (gfx11) has no FP8 conversion and no FP8 WMMA. Every e4m3 value is an
// FP16 value (3 mantissa bits into 10, exponent range inside FP16's), so a
// network that is e4m3 at every operation boundary can hold those values in
// float16_t, multiply them in an FP16 WMMA with an FP32 accumulator, and get
// the same products the FP8 WMMA gets. Only the *narrowing* needs work, and it
// is done here in integer/f32 arithmetic with the semantics of
// `cvt.rn.satfinite.e4m3x2.f16x2`: round to nearest even, saturate at +-448
// (infinity included), NaN stays NaN.
//
// Exhaustively checked on gfx1100 against tin::f_to_e4m3 / tin::e4m3_to_f over
// all 65536 f16 inputs and all 256 codes.
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require

// A code byte (e4m3, 0x7F/0xFF are NaN) to the f16 with the same value. The
// exponent field is moved into f16's and the 2^8 bias difference is one exact
// multiply; subnormals come out right because f16's subnormal grid is finer.
// 0x7F/0xFF would land on a finite f16 (480), so the NaN is selected explicitly.
float16_t nr_e4m3_decode(uint code) {
    const uint h = ((code & 0x80u) << 8) | ((code & 0x7Fu) << 7);
    const float16_t v = unpackFloat2x16(h)[0] * float16_t(256.0);
    return (code & 0x7Fu) == 0x7Fu ? unpackFloat2x16(0x7E00u)[0] : v;
}

// f32 value (already an f16 value) to the nearest e4m3 value, as f32.
float nr_e4m3_round(float x) {
    const float a = abs(x);
    float q;
    if (a < 0.015625) {                         // 2^-6: the subnormal grid, step 2^-9
        q = roundEven(a * 512.0) * (1.0 / 512.0);
    } else {                                    // keep 3 mantissa bits, ties to even
        uint u = floatBitsToUint(a);
        u += 0x7FFFFu + ((u >> 20) & 1u);
        q = uintBitsToFloat(u & 0xFFF00000u);
    }
    q = a >= 448.0 ? 448.0 : q;
    q = isnan(x) ? x : q;
    return x < 0.0 ? -q : q;
}

// An f16 input is already on f16's grid, so both entry points are the same direct rounding;
// an f32 input is rounded once, straight onto the e4m3 grid.
float16_t nr_quant_e4m3(float16_t v) { return float16_t(nr_e4m3_round(float(v))); }
float16_t nr_quant_e4m3(float v)     { return float16_t(nr_e4m3_round(v)); }
