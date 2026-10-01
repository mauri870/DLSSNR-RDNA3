# Checking DLSSNR-AMD against NVIDIA's own DLL

This compares the picture DLSSNR-AMD produces with the picture NVIDIA's DLSS 5 Neural Rendering DLL
produces for the same input and the same settings: single frames, and short moving sequences where
motion vectors and the previous frame (history) are used.

## What ran on each side

**NVIDIA side (the reference).** The unmodified `nvngx_dlssnr.dll` 310.8.0.0 (DLSS 5 Neural Rendering),
called through NVIDIA NGX the way a game calls it: `NVSDK_NGX_D3D12_Init_Ext`, then
`NVSDK_NGX_D3D12_CreateFeature` for the Neural Rendering feature with the `DLSSNR.*` creation parameters,
then `NVSDK_NGX_D3D12_EvaluateFeature` with the `DLSSNR.*` evaluation parameters. The DLL ran its own
host code and its own GPU kernels end to end; nothing was extracted from it, re-implemented or replayed.

- GPU: NVIDIA GeForce RTX 5090, Linux driver 580.105.08.
- NGX core: `nvngx.dll` / `_nvngx.dll` from the NVIDIA Linux driver package 615.71.09 (the Wine builds NVIDIA
  ships with the driver). The NGX cores of 580.105.08 and 595.71.05 reject this feature as out of date
  (`0xBAD0000C`), so the 615.71.09 core was used and the driver version was reported as 615.71.
- D3D12 on Linux: Wine 11.18 (staging, wow64 build), vkd3d-proton, DXVK's DXGI and dxvk-nvapi
  (DLLs from GE-Proton 11-7), on NVIDIA's Vulkan driver.
- Host programs: small D3D12 programs that load `_nvngx.dll`, create the feature, upload the input and
  read the output back.
  - Single frames: `DLSSNR.Reset` is set on every evaluate, so each output is one frame with no history.
  - Moving sequences: one evaluate per frame with `DLSSNR.Color`, `DLSSNR.MVec`, `DLSSNR.Depth`,
    `DLSSNR.MVecScale`, `DLSSNR.DepthInverted` and the guide subrects set, the way OptiScaler sets them;
    `DLSSNR.Reset` only on the first frame, so every later frame uses the DLL's own history.

**DLSSNR-AMD side.** The Linux build of DLSSNR-AMD 0.0.2.5 on an AMD Radeon RX 9070 XT (Mesa 26.2.3
RADV), same input files, same parameters, through the same code path a game uses. For single frames
0.0.2.5 gives byte-identical output to 0.0.2.2, 0.0.2.3 and 0.0.2.4 (its changes only affect frames that
use history).

## Single frames

### Input and settings

- Input: one Tomb Raider (2013) frame, 3840x2160, 8-bit, and its Lanczos downscales to 2560x1440 and
  1920x1080 (`single-frame-inputs/`). Handed to both sides as the colour input with exact 8-bit values,
  constant depth, zero motion.
- Settings: DLSSNR-AMD's defaults on both sides: `Intensity` 1, `Style` 0, `LocalToneStrength` 1,
  `LocalStructureStrength` 1, `SkinStructureStrength` -1, `UseAutoMask` 1.

### Results

Both outputs compared as 8-bit RGB. SSIM is the mean over R, G and B (11x11 Gaussian window, sigma 1.5).
"Edit" is output minus input, i.e. what the network changed.

| Resolution | PSNR vs NVIDIA | SSIM vs NVIDIA | Correlation of the edits | Mean difference (1/255, R G B) | Pixels with all channels within one 8-bit step |
|---|---|---|---|---|---|
| 1920x1080 | 45.56 dB | 0.9961 | 0.9949 | +0.36 +0.44 +0.38 | 67.5% |
| 2560x1440 | 47.99 dB | 0.9968 | 0.9957 | +0.08 +0.09 +0.08 | 80.8% |
| 3840x2160 | 49.06 dB | 0.9970 | 0.9959 | +0.05 +0.05 +0.06 | 85.7% |

For scale, the input itself (Neural Rendering off) against NVIDIA's output: PSNR 26.44 / 27.72 / 28.69 dB,
SSIM 0.9619 / 0.9682 / 0.9727. Size of the edit (RMS, 1/255): NVIDIA 12.15 / 10.48 / 9.37,
DLSSNR-AMD 12.33 / 10.51 / 9.37.

NVIDIA's DLL gave byte-identical output in two separate runs of the same input, so the differences above
are not run-to-run noise on the NVIDIA side; they are what remains between the two implementations. They
are below one 8-bit step on average and are largest at 1080p, where DLSSNR-AMD is about 0.4/255 brighter
over smooth areas.

Pictures (`single-frame-images/`): input, NVIDIA and DLSSNR-AMD side by side, for each resolution, full
frame and a close-up.

Raw outputs (`single-frame-outputs/`): both sides at every resolution as lossless 8-bit PNG, exactly the
pixels compared above.

### The RDNA3 build

The same three inputs and settings through the RDNA3 build (`linux/shaders/rdna3/`) on an AMD Radeon
RX 7900 XTX (Mesa 26.2.3 RADV), against the same NVIDIA outputs, measured the same way. The RDNA3 build
has no FP8 instructions: it holds the network's e4m3 values as FP16 and multiplies them in the FP16 matrix
instructions with FP32 accumulation, so the products are the ones the FP8 build forms and the activation
arena and weights take twice the memory. The run goes through `nr::Runtime` the way a game does
(8-bit RGBA input, the defaults above, one network pass, 8-bit output); the output is the same on every
run. Single frames only: the moving sequences have not been run on RDNA3.

| Resolution | PSNR vs NVIDIA | SSIM vs NVIDIA | Correlation of the edits | Mean difference (1/255, R G B) | Pixels with all channels within one 8-bit step |
|---|---|---|---|---|---|
| 1920x1080 | 45.21 dB | 0.9959 | 0.9940 | +0.31 +0.43 +0.41 | 65.4% |
| 2560x1440 | 47.99 dB | 0.9968 | 0.9953 | +0.10 +0.10 +0.08 | 80.7% |
| 3840x2160 | 48.95 dB | 0.9969 | 0.9953 | +0.04 +0.05 +0.04 | 85.6% |

Outputs: `single-frame-outputs/<resolution>_dlssnr-amd-rdna3.png`; numbers in
`single-frame-results-rdna3.json`.

## Moving sequences

### Input and settings

Four sequences of 10 frames at 1920x1080, 2560x1440 and 3840x2160, made from the same Tomb Raider frame:

- **Still scene**: the frame, unchanged for 10 frames.
- **Moving object**: a disc textured with the mirrored frame moves across the still frame
  (6 pixels a frame at 1080p).
- **Panning camera + moving object**: the camera moves 1.5 x 0.5 pixels a frame at 1080p (resampled
  from the 4K frame, so the step is exact) while the disc moves 5 x 2 pixels a frame.
- **Same, NR after an upscaler**: the previous sequence with the motion vectors and depth at half the
  resolution of the colour, which is what a game hands over when Neural Rendering runs after an
  upscaler.

Steps scale with the resolution. Each frame hands both sides its colour (8-bit values, exact), its
motion vectors and its depth. Motion vectors are exact (in pixels, previous = current + vector) and depth
is reversed-Z with the disc in front; depth only chooses where the motion vector is read, as in NVIDIA's
DLL, and is not itself an input of the network. Settings: the defaults above on both sides.

### Results

Every frame compared as 8-bit RGB the same way as above; "mean" is over the 10 frames. The first frame
has no history yet, so it is the single-frame case; the last frame is the tenth. CIEDE2000 is the colour
difference as the eye sees it (below 1 is generally not visible). "Change per frame" is how much each
output still changes from one frame to the next once the previous frame is moved by the motion vectors
(mean over all pixels, 1/255); it shows whether both sides carry the previous frame forward the same way.

**1920x1080**

| Sequence | PSNR vs NVIDIA (mean / first frame / last frame) | SSIM vs NVIDIA | Correlation of the edits | CIEDE2000 mean / 95th percentile | Change per frame: NVIDIA / DLSSNR-AMD / input |
|---|---|---|---|---|---|
| Still scene | 47.02 / 45.73 / 47.38 dB | 0.9975 | 0.9964 | 0.54 / 1.14 | 0.324 / 0.300 / 0.000 |
| Moving object | 46.84 / 45.62 / 47.26 dB | 0.9975 | 0.9964 | 0.54 / 1.15 | 0.345 / 0.321 / 0.038 |
| Panning camera + moving object | 46.29 / 45.62 / 46.46 dB | 0.9971 | 0.9959 | 0.57 / 1.21 | 2.017 / 1.956 / 2.054 |
| Same, NR after an upscaler | 46.31 / 45.62 / 46.62 dB | 0.9970 | 0.9959 | 0.56 / 1.20 | 2.020 / 1.967 / 2.056 |

**2560x1440**

| Sequence | PSNR vs NVIDIA (mean / first frame / last frame) | SSIM vs NVIDIA | Correlation of the edits | CIEDE2000 mean / 95th percentile | Change per frame: NVIDIA / DLSSNR-AMD / input |
|---|---|---|---|---|---|
| Still scene | 50.81 / 48.11 / 51.99 dB | 0.9980 | 0.9975 | 0.42 / 1.00 | 0.317 / 0.318 / 0.000 |
| Moving object | 50.88 / 48.33 / 52.00 dB | 0.9980 | 0.9976 | 0.41 / 1.00 | 0.339 / 0.344 / 0.038 |
| Panning camera + moving object | 50.41 / 48.33 / 51.01 dB | 0.9978 | 0.9974 | 0.43 / 1.03 | 1.272 / 1.276 / 1.229 |
| Same, NR after an upscaler | 50.13 / 48.33 / 50.83 dB | 0.9977 | 0.9973 | 0.43 / 1.03 | 1.271 / 1.281 / 1.231 |

**3840x2160**

| Sequence | PSNR vs NVIDIA (mean / first frame / last frame) | SSIM vs NVIDIA | Correlation of the edits | CIEDE2000 mean / 95th percentile | Change per frame: NVIDIA / DLSSNR-AMD / input |
|---|---|---|---|---|---|
| Still scene | 51.57 / 48.95 / 52.57 dB | 0.9980 | 0.9974 | 0.39 / 0.98 | 0.289 / 0.291 / 0.000 |
| Moving object | 51.42 / 48.94 / 52.33 dB | 0.9980 | 0.9973 | 0.39 / 0.99 | 0.315 / 0.317 / 0.038 |
| Panning camera + moving object | 51.49 / 48.94 / 52.44 dB | 0.9980 | 0.9974 | 0.39 / 0.99 | 0.418 / 0.419 / 0.046 |
| Same, NR after an upscaler | 51.10 / 48.94 / 51.98 dB | 0.9979 | 0.9972 | 0.40 / 0.99 | 0.418 / 0.423 / 0.048 |

Both sides get closer to each other once history is in use: in every sequence the last frame is closer
than the first, and in the per-frame data (`moving-sequence-results.json`) every frame after the first is
closer than the first and the difference does not grow over the 10 frames. The output changes from frame
to frame on both sides even for the still scene: the DLL feeds the network fresh noise every frame, and
DLSSNR-AMD does the same.

DLSSNR-AMD 0.0.2.4, the previous release, on the same sequences against the same NVIDIA outputs,
measured the same way (PSNR vs NVIDIA, mean over the 10 frames):

| Sequence | 1920x1080 | 2560x1440 | 3840x2160 |
|---|---|---|---|
| Still scene | 45.64 dB | 48.16 dB | 49.26 dB |
| Moving object | 44.98 dB | 48.12 dB | 48.85 dB |
| Panning camera + moving object | 44.79 dB | 47.95 dB | 48.75 dB |
| Same, NR after an upscaler | 40.49 dB | 41.71 dB | 42.77 dB |

Data (`moving-sequence-results.json`): every number above, per frame. The entries named
`dlssnr_amd_0.0.2.4` are the previous release, with the other measures as well; the rest are 0.0.2.5.

The moving sequences come without pictures. They are synthetic, a disc sliding over a still photo,
made to be measured rather than looked at: pictures of them look odd and would add little, and stills and
animations for all twelve sequences would take about 80 MB. This part of the check is the numbers above;
the single-frame pictures show what the two outputs look like.

## Limits

- One picture. The single frames cover three resolutions; the moving sequences are short (10 frames),
  made from that picture with exact motion vectors, not recorded from a game.
- The input is handed straight to the network. How a game or OptiScaler prepares its frame before
  Neural Rendering is not part of this comparison.
- One AMD card (RX 9070 XT) and one NVIDIA card (RTX 5090).
