# Third-party components

Apart from the AMD FSR 1 headers and RenoDX's shaders, none of these are stored in this repository. `fetch_deps.sh` downloads them at pinned versions;
the build compiles against some of them and the packages carry some of them unmodified, each with
its licence file.

| Component | Licence | Used for |
| --- | --- | --- |
| [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) | MIT | The ReShade add-on (`*/src/pe/nr_reshade_addon.cpp`) follows its design; `DLSS5_Feed.fx` is shipped. Its bundled MinHook, ReShade add-on headers and Dear ImGui headers are compiled in. |
| [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause | Compiled into the DLLs (from DLSS5-Feeder's copy). |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | Headers, for the ReShade add-on's settings page; compiled into NR Control (the OptiScaler route's NR menu: the Linux plugin's `linux/src/pe/nr_pe_menu.cpp` with the D3D11, D3D12 and Vulkan backends, and the Windows NGX core's `windows/src/pe/nr_pe_menu.cpp` with the D3D11 and D3D12 backends) from the copy in OptiScaler-NR's source tree; its licence ships beside the Linux plugin. |
| [ReShade](https://github.com/crosire/reshade) 6.8.0 | BSD-3-Clause | Add-on headers; `ReShade32/64.dll` shipped in the ReShade routes. |
| [reshade-shaders](https://github.com/crosire/reshade-shaders) | BSD-3-Clause | `ReShade.fxh`, `ReShadeUI.fxh` shipped. |
| [vort_Shaders](https://github.com/vortigern11/vort_Shaders) | MIT | Motion vectors for the ReShade routes, shipped. |
| [OptiScaler](https://github.com/optiscaler/OptiScaler) | GPL-3.0 | The Linux OptiScaler route's host, the original, shipped unchanged with its licence; this project's NR is a separate plugin it loads (`dlssnr-amd.asi`). |
| [OptiScaler-NR](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) 0.8.91 | GPL-3.0 | The Windows OptiScaler route's host, shipped as released; this project's DLLs are separate modules it loads. |
| [Inter](https://github.com/rsms/inter) 4.1 | SIL Open Font License 1.1 | NR Control's font in the Linux plugin (embedded in `dlssnr-amd.asi`); its licence ships beside the plugin. |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (`ffx_a.h`, `ffx_fsr1.h`) | MIT | Stored in `linux/shaders/passes/include/fsr1/` and `windows/shaders/passes/include/fsr1/` with its licence; compiled into `wilsjo2_classic.spv` on Linux and `runtime_upscale.spv` on Windows (the Classic enlargement's FSR 1 choice), which ship with the licence beside them. |
| [RenoDX DLSS5 add-on](https://github.com/PEQHUB/RenoDX-DLSS5-Generic) v8.5.0-rc10 | MIT | The RenoDX-style option: its codec and look shaders are stored in `linux/shaders/renodx/` and `windows/shaders/renodx/` (unchanged apart from the speed changes listed in the README there) and one header in `*/src/core/renodx/`, each with the licence; the compiled shaders ship with `RenoDX-LICENSE.txt` beside them. |
| [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) 1.4.357 | Apache-2.0 | Built with the patches in `*/vulkan-loader/` and shipped; the changes are stated in `PATCHES.diff`. |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) 1.4.357 | Apache-2.0 / MIT | Build only. |
| [glslang](https://github.com/KhronosGroup/glslang) 16.5.0 | BSD-3-Clause and others | Build only (GLSL -> SPIR-V). |
| [Slang](https://github.com/shader-slang/slang) 2026.19 | Apache-2.0 with LLVM exception | Build only (RenoDX's HLSL -> SPIR-V). |
| [DXVK](https://github.com/doitsujin/dxvk) from [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom) 11-7 | zlib | Windows package only (the DX9 route), shipped unmodified. |

NVIDIA's `nvngx_dlssnr.dll` and the weights in it are NVIDIA's property. This project contains no part
of them; `linux/package/model-tools/` and `windows/package/model-tools/` read them from a copy the user
supplies. The layer tables in `*/src/core/nr_native_plan_data.hpp` and `*/src/core/nr_native_plan_data_small.hpp` and
`linux/package/model-tools/descriptor.json` and `descriptor-small.json` describe the networks' structure
(layer types, shapes, offsets), not their weights. `linux/data/int4/` holds the int4
mixed option's tables: per matrix the clip step index of each row, the correction of each weight against
plain rounding, and calibration means of the inputs; no weight values. The int4 weights are made at
install from the user's own model. `linux/data/experimental/` holds the difference of CC_SMALL_FP8_EXPERIMENTAL's
fitted weights from CC_SMALL_FP8's (each changed entry XOR the original), from which the installer makes its weights
out of the user's own model; it holds none of NVIDIA's weights.

## DLSS5-Feeder licence notice

```
MIT License

Copyright (c) 2026 Jean-Laurent ROUZIES

Portions derived from dlss5-dx11-bridge, Copyright (c) 2026 NIGos (MIT), see external/bridge-1.0.19/LICENSE.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
