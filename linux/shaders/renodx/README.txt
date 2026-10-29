RenoDX DLSS5 add-on codec shaders, v8.5.0-rc10 (PEQHUB/RenoDX-DLSS5-Generic, branch v8.5dev), with the changes listed below. The speed changes matched the baseline bytes in the recorded fixtures; the Preprocess inverse correction changes Curve=none output. MIT, see LICENSE. Built for Vulkan by
linux/build/build_renodx.sh the way RenoDX builds its own Vulkan path (Slang, RENODX_VULKAN). Used by the
RenoDX-style option ([RenoDX] in dlssnr-amd.ini, linux/src/core/nr_renodx.cpp).

Changes (DLSSNR-AMD):
  look_compose.cs_5_1.hlsl   the 3x3 clip's neighbour values computed once per pixel for the group's tile
  look_common.hlsli,         the Detail stability history's three words in an RG32_UINT and an R32_UINT image
  look_compose.cs_5_1.hlsl   (12 bytes a texel; the RGBA32_UINT image carried a spare word)
  v6_resolve_commit.cs_5_1.hlsl (new)  the last pass's resolve and the commit in one pass when the commit takes no
                             pedestal
  look_compose.cs_5_1.hlsl   each thread's own pixel's band values computed once for the tile (the border from the
                             first 68 threads)
  legacy_reduce.cs_5_1.hlsl, the block's four running totals summed on four threads side by side, each in its order
  v6_pedestal_reduce.cs_5_1.hlsl  (partial 0, 1, ... 255), instead of all four on one thread
  legacy_decode_reduce.cs_5_1.hlsl (new)  the decode and the black-level restore's reduce in one pass, for the frame
                             decoded in place (each thread its 2x2 in the reduce's order, the stored value read back)

  v6_common.hlsli          NR_PREP_RESOLVE only: include our prep_resolve.hlsli after DisplayCurveInverse, so the
                             Preprocess Curve=none inverse can match v6_encode's toe/shoulder (or shoulder alone).
                             Other Preprocess curves and builds without NR_PREP_RESOLVE keep their formulas.

The none-curve Preprocess ratio explicitly returns one on unchanged channels to avoid reciprocal rounding; other curve branches keep their previous arithmetic.

Upstream comments are retained verbatim. Their ideal-arithmetic claims do not establish
finite-precision identity; the measured limits are recorded in the implementation log.

Fast passes (2026-10-25): the accepted extrapolation / feedback / hybrid passes live in
../renodx-fast, built as renodx_st_*.spv. The upstream RenoDX shaders in this directory
are unchanged by this port. The host runs these passes on the network input / answer,
including the separate Preprocess input, before the existing inverse and composition.
