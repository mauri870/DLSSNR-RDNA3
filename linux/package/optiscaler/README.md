# DLSS Neural Rendering on AMD, through OptiScaler

OptiScaler's DLSS-NR forks already know how to find a game's colour, depth and motion vectors, encode
a display-referred proxy, run a Neural Rendering model over it and composite the answer back. What
they do not have is a model that runs on a Radeon: they reach NVIDIA's `nvngx_dlssnr.dll`, and that
snippet is Blackwell-only.

This replaces the model, not OptiScaler. OptiScaler is not modified, not rebuilt and not patched — it
loads the same file names it always loads, calls the same entry points in the same order, and gets
our network on the AMD card instead of NVIDIA's on theirs.

## Two lineages, two doors, one model

The fork split, and the two halves reach feature 18 differently. Both are answered.

| lineage | how it reaches the model | what it loads |
| --- | --- | --- |
| **wilsjo2 ≥ 0.8.1** (`OptiScaler-NR-v0.8.91.zip`, **the bundled one**) | `NVSDK_NGX_D3D12_CreateFeature(cmdList, (NVSDK_NGX_Feature) 18, params, &feature)` on the NGX core, then `D3D12_EvaluateFeature` / `D3D12_ReleaseFeature`. Vulkan: `VULKAN_CreateFeature1` / `VULKAN_EvaluateFeature` / `VULKAN_ReleaseFeature`. | **`_nvngx.dll` only.** Its `INSTALL-DLSSNR.md` says "No NR helper DLL is required; remove the obsolete `nvngx.dll_dlssnr.dll` when upgrading." |
| **Dagherbou** (`OptiScaler-DLSSNR-v0.2.0.zip`) | `dlssnr_call_create` / `_evaluate` / `_release` / `_set_extras` / `_probe_float` / `_set_float_slot` in a forwarder DLL beside itself. | `nvngx.dll_dlssnr.dll`, with `nvngx_dlssnr.dll` as a presence check, plus `_nvngx.dll` for the parameter block. |

Behind both doors is one body of code, `linux/src/pe/nr_dlssnr_model.cpp`: one `nr::pe::Session` per graphics
API, a feature handle carrying the extent and the six controls, and `Session::run_after` (D3D12) or
`Session::run_vulkan` (the network reads the colour and its post block stores into the output; the colour
is copied into the output first only when the model is not applied or the extents differ). `linux/src/pe/nr_ngx_core.cpp` and
`linux/src/pe/nr_dlssnr_forwarder.cpp` are the two ABIs over it and nothing else. A process that somehow
loaded both would still build one network: the core checks for `nvngx.dll_dlssnr.dll` in its module
list with `GetModuleHandleW` (never `LoadLibrary`) and routes through its exports if it is there.

## What goes in the game folder

| file | what it is | where it comes from |
| --- | --- | --- |
| `dxgi.dll` | OptiScaler itself, renamed | the release archive, renamed as its own `setup_linux.sh` would |
| `OptiScaler.ini` | its configuration | the release archive, three keys rewritten (below) |
| `OptiScaler/`, `docs/`, `Licenses/` | its FSR/XeSS libraries and papers | the release archive, untouched |
| **`dlssnr_core.dll`** | **the NGX core, and feature 18** (built as `_nvngx.dll`, shipped renamed; below) | `linux/build/build_optiscaler_nr.sh` |
| **`nvngx.dll_dlssnr.dll`** | **the forwarder, for the older lineage** | same |
| **`nvngx_dlssnr.dll`** | **a byte copy of the forwarder** | same |
| `dlssnr-amd/` | the model, the network's SPIR-V, the pipeline cache (and with int4 mixed its network and Vulkan layer) | `linux/build/build_package.sh`'s package |
| `dlssnr-amd.ini` | our own settings: `[Preprocess]`, and `[Int4Mixed]` with int4 mixed | written at install |

The package's `install.sh <game-dir> optiscaler` does all of it. (`install_optiscaler_nr.sh` in this
directory predates the package layout - it expects `artifacts/optiscaler/nr` and a `build/` +
`artifacts/` payload - and no longer works; use the package.)

The release that ships:

- <https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases> — `OptiScaler-NR-v0.8.91.zip`
- sha256 `19a2852bb3f88e09075e3ccc66e0318c52e83a5901d9020a10384ae49d59ff77`
- source tag `v0.8.91`, commit `f45ccf3`, cloned to
  `artifacts/ref/wilsjo2-OptiScaler-DLSSNR-PreSR-Multipass`
- the previous one, `OptiScaler-NR-v0.8.4.zip` (sha256 `8789912859882e66b3f3a1aa768db947da779dfd65225df69ea919052e73a2e4`,
  tag `v0.8.4`), still works with the same core: `NR_OPTI_ZIP=` bundles it instead

It is a binary release: `OptiScaler.dll` (26 MB), `OptiScaler.ini`, the `OptiScaler/` library folder,
`docs/`, `Licenses/`, `setup_linux.sh` / `setup_windows.bat`, the `!! EXTRACT ALL FILES TO GAME
FOLDER !!` marker, and `SHA256SUMS.txt`. **It ships no `nvngx.dll_dlssnr.dll` at all** — confirmed by
listing the archive and by `grep -rni 'nvngx.dll_dlssnr' ` over its source tree, whose only hit is the
line telling users to delete it. Set `NR_OPTI_ZIP=` to bundle the other lineage instead.

## Why three DLLs and not one

**The NGX core** (built as `_nvngx.dll`, installed as `dlssnr_core.dll`) is the one that matters now.

It is not installed under NVIDIA's name. A Streamline game (007 First Light) initialises NGX itself,
and a module called `_nvngx.dll` already loaded in the process -- ours, loaded early through
`NvngxPath` -- is taken for NVIDIA's core: Streamline's NGX start fails (`ngxResult failed
0xbad00002`, "Missing NGX context") and the game greys out DLSS. Unmodified OptiScaler has no such
module, so Streamline's attempt to load the core reaches OptiScaler's own hook and DLSS stays
selectable. Renamed, ours no longer stands in the way, and the DLSS option is back.

It has two jobs.

*The parameter block.* Every fork needs one and will only take the core's capability block:
`NVNGXProxy::InitDx12(device)` and then `D3D12_GetCapabilityParameters()`, and it gives up if either
declines. Ownership of the returned block transfers to the caller, which frees it through our
`DestroyParameters`. The block is not a C++ class deriving from the SDK's `NVSDK_NGX_Parameter`: MSVC
lays overloaded virtuals out in reverse declaration order and GCC does not, so the vtable is written
out by hand in MSVC's order (`linux/src/pe/nr_ngx_abi.hpp` has the table and the evidence).

*Feature 18.* `NVSDK_NGX_D3D12_CreateFeature` reads `DLSSNR.Width` / `.Height` /
`.Hint.Render.Preset` / `.UICorrection` and the six controls out of that block — directly out of its
own `std::map`, since the block is ours, with a fall-back through the published vtable for a block
from anywhere else — builds a feature and returns Success with a non-null handle. Two rules come from
the caller and neither is optional:

- **Create must succeed with a handle.** `DlssNr_Proxy.cpp:180` falls back to
  `DlssNr_CompatibilityRuntime` — which `LoadLibraryExW`s NVIDIA's own `nvngx_dlssnr.dll` — when and
  only when create fails *and* leaves the handle null. On an AMD machine that must never fire. (If it
  ever did it would find our forwarder under that name, which exports no `NVSDK_NGX_D3D12_Init_Ext`,
  and give up with "is missing required NR exports", preserving our failure rather than running an
  NVIDIA snippet. Safe, but not the intended path.)
- **Handles must be unique in both pointer and `Id`.** `DlssNrFeature_Vk_Model.cpp:111-126` fails the
  whole pass if a second create answers with either one already in use, because identical-profile
  multi-pass layers still need independent temporal histories. The handle is the first member of the
  feature object and `Id` is a process-wide counter.

`EvaluateFeature` re-reads everything every call, which is how the caller drives it: `DLSSNR.Color`,
`.Depth`, `.MVec`, `.Output`, the four subrect quartets, `.DepthInverted`, `.Reset`, `.MVecScaleX/Y`
and the six controls.

**`nvngx.dll_dlssnr.dll`** is Dagherbou's door. `shaders/dlssnr/DlssNr_Dx12.cpp` there looks for
exactly that name beside OptiScaler, then beside the executable, resolves `dlssnr_call_*` /
`dlssnr_vk_*` and drives the pass through them. Ours exports 28 symbols — their 25 plus the pre-SR
fork's `dlssnr_call_evaluate_v2`, `dlssnr_vk_evaluate_v2` and `dlssnr_call_error` — with the same
signatures, and runs the same model.

On NVIDIA this file exists for a reason that does not apply here: NVIDIA's snippet resolves the
module that owns its caller's return address and rejects anything whose path does not contain
`nvngx.dll`. There is no snippet on our side and no caller gate. The name is kept only because it is
what OptiScaler looks for.

**`nvngx_dlssnr.dll`** is a presence check in the older fork, which searches for a file by that name
and gives up with *"nvngx_dlssnr.dll was not found beside OptiScaler or the game"* if it is absent. On
NVIDIA it is the 165 MB model out of a driver package. Here it is a byte copy of the forwarder, and on
the normal path nothing ever loads it.

## The multi-pass rule

wilsjo2's `Passes` slider creates **one NGX feature per pass** — up to 3 by default, 30 with
`UnlockPasses` — and keeps every handle alive, chaining their outputs through a ping-pong scratch
texture. Each pass gets its own capability parameter block and its own profile; pass 2 and later
always get `LocalToneStrength = 0`.

Every create here returns a distinct handle, and every feature has its own temporal history (the
runtime keeps one per feature id), so pass 2 never reads what pass 1 wrote in the same frame. (Before
that, the first handle kept the only history and later passes ran with reset forced on.)

## The ini keys, and the one that is deliberately left alone

Written by the installer into the archive's own `OptiScaler.ini`. They ship as `<key>=auto` and are
rewritten in place; a required key that has been renamed upstream shows up as an error rather than as
a silently ignored line.

```ini
[DlssNr]
Enabled=true
WhitePointSource=1

[Libraries]
NvngxPath=<game-dir>\dlssnr_core.dll
```

- `[DlssNr] Enabled` — the key name is `Enabled` inside section `DlssNr`, not `DlssNrEnabled`
  (`Config.cpp`: `readBool("DlssNr", "Enabled")`). Default (auto) is `false`.
- `[Libraries] NvngxPath` — `Util::LoadProxyLibrary` accepts a directory (it appends `_nvngx.dll`,
  the first of the two names it tries) or a full file path. A file path is written, and it names
  `dlssnr_core.dll`, never `_nvngx.dll` (see "Why three DLLs").
- `[DlssNr] WhitePointSource=1` — the white point from the game's own exposure (the 1x1 exposure
  texture and pre-exposure it hands DLSS), as v0.8.4 did by default. v0.8.5 made 0, a fixed paper
  white, the default; a game that supplies its exposure then gets a different picture (upstream
  issue #96: "NR does nothing" until "game exposure" is chosen). A frame without a valid exposure
  texture falls back to the paper white either way. v0.8.4's ini has no such key (1 is its built-in
  default), so the key is optional: rewritten where present, not an error where absent.
- `[Spoofing]` stays at the release's `auto` values: `StreamlineSpoofing` is true, `Dxgi` is true on
  an AMD card, and the GPU reported to the game is an NVIDIA RTX 4090 (`SpoofedVendorId` 0x10de,
  `SpoofedDeviceId` 0x2684), which is what makes a game offer DLSS at all. `Dxgi` depends on the game
  and the system: if the NR page stays at "Waiting for the upscaler to run", try `Dxgi=false`
  (Dying Light: The Beast has needed it). The package README and install.sh give the same advice.
- **`[DlssNr] RunBeforeSR` is left at the release default** (`auto`, which is `false`): NR runs after
  the upscaler, where the official pipeline puts it. It is a live control in OptiScaler's own overlay
  and choosing it here would override a decision that is the user's.
- **`[Upscalers] Dx12Upscaler` is left at `auto` on purpose.** Left alone it picks FSR4 on a capable
  Radeon and XeSS otherwise, which is OptiScaler's own choice; pinning it here would override a
  decision that has nothing to do with Neural Rendering.

## Fixes applied to OptiScaler in memory

OptiScaler itself is shipped as released and never rebuilt. The NGX core corrects seven defects of
OptiScaler-NR v0.8.4 and v0.8.91 in memory when OptiScaler loads it, before the game creates a Vulkan device or
presents (`linux/src/pe/nr_pe_optifix.cpp`). Each is found by exact byte signatures; another
OptiScaler build is left untouched and `dlssnr-amd.log` says so. `NR_OPTISCALER_FIX=0` turns all off.

- **Startup abort (007 First Light).** The fork's `vkCreateDevice` hook queries device extensions
  through `vkGetInstanceProcAddr` on the most recently *created* VkInstance, which it never forgets
  when that instance is destroyed. A game that creates and destroys several instances at startup
  (Streamline, AMD AGS, DXVK/vkd3d factories) reaches the next device with a dead handle, and the
  Linux loader aborts the process ("vkGetInstanceProcAddr: Invalid instance") before a window opens.
  The query now passes a null instance, which returns no function; that hook then adds no extension
  (on a Radeon it only ever added a name DXVK and vkd3d-proton already use as Vulkan 1.3 core).
- **Finished Picture under DXVK / vkd3d-proton.** OptiScaler's Present wrapper returns early when the
  GPU runs DXVK, before the call that applies NR to the finished picture, so `[DlssNr]
  FinishedPicture=true` never ran under Proton (status "Waiting for the previous picture to
  finish.", NR off). The DXVK path now makes the same call under the same condition before its
  Present; nothing else on that path changes.
- **A 0 x 0 (window-sized) swapchain taken for an overlay (Helldivers 2).** `CreateSwapChainForHwnd`
  treats any width or height under 100 as an overlay and never wraps it, so NR never became ready. A
  zero, which DXGI defines as "the window's size", now takes the normal path; 1..99 still count as an
  overlay.
- **A D24S8 depth guide copied into an R24X8 texture (Helldivers 2, Kingdom Come: Deliverance II).**
  vkd3d-proton makes D24S8 a D32S8 image on AMD, and the copy into an R24X8 clone hung the GPU
  (amdgpu reset). The guide is now passed on in its own format; the NR runtime reads the depth aspect
  of depth/stencil images (`linux/shaders/passes/runtime_depth.comp`).
- **A float colour with AutoExposure but no HDR flag (007 First Light, Helldivers 2).** OptiScaler
  treated it as finished SDR and handed NR raw scene-linear values (up to 563 in Helldivers 2). It is
  now encoded as linear HDR, the same as a game that sets the HDR flag.
- **A released feature's GPU work outliving it (S.T.A.L.K.E.R. 2).** `ReleaseFeature` freed the
  feature's timestamp heaps while the game's list still had to write them; the release now waits
  until the GPU has run the feature's last evaluate.
- **R9G9B9E5 and R32G32B32 typeless colour taken as SDR (Resident Evil Requiem).** They are float
  formats and now count as linear HDR like the others.

v0.8.91 has the same seven defects. Fixes 1, 3, 4 and 6 find it by the v0.8.4 signatures; the
Finished Picture, AutoExposure and float-format fixes have a v0.8.91 signature of their own (its
Finished Picture condition also skips XeFG's game picture, as its native path does).

007 First Light declares AutoExposure and hands over no exposure, so OptiScaler's encode falls back to
a fixed white point (paper white 1.0) and the model sees a frame about five stops too dark (mean 0.028
against 0.2-0.4 for its finished picture), before or after SR. `[Preprocess]` in `dlssnr-amd.ini`
(auto exposure, filmic curve by default) is the fix for that input; Finished Picture is also correct
there.

## What the core does not do

It does not make OptiScaler think real DLSS is available, and it could not if it tried.
`GetUpscalerBackend` needs `NVNGXProxy::IsDx12Inited() && primaryGpu.dlssCapable`, and
`dlssCapable` comes from `misc/IdentifyGpu.cpp`:

```cpp
gpuInfo.dlssCapable = gpuInfo.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_TU100;
```

`nvidiaArchInfo` is filled by `NvAPI_GPU_GetArchInfo` on a physical GPU handle matched by LUID
through the real `nvapi64.dll`. On a Radeon that handle is never found, the struct stays zero, and
the flag is false. Nothing in a parameter block, and nothing this core returns, feeds that decision.

Going the other way: on the path a *game* sees, OptiScaler runs `InitNGXParameters` over whatever
block we hand back, and its first line is `Set("SuperSampling.Available", 1)` — it tells the game
DLSS *is* available so the game asks for it and OptiScaler can substitute FSR or XeSS. That is
deliberate on their side and we do not fight it.

## What the diagnostics will say

`docs/NR-INITIALIZATION-DIAGNOSTICS.md` in the release describes `NgxDiagnostics::RuntimeReport`,
which runs before and after `CreateFeature(18)` with `[Log] LogLevel=2`. It **queries the core for
nothing**: it logs the device and command-list LUIDs, `GetDeviceRemovedReason`, `GetNodeCount`, the
path/size/version of the loaded dispatcher (our `dlssnr_core.dll` — it has no VERSIONINFO resource, so that
field reads `unknown`), the size and SHA-256 of every `nvngx_dlssnr.dll` candidate it can find, and
whether a module by that name is in the process list. On our path it is not, so it always logs
`nvngx_dlssnr.dll is not present in the process module list` — a log line with no effect on behaviour.

The one real interface obligation is the logging callback in `NVSDK_NGX_FeatureCommonInfo`, which the
diagnostics installs over the game's. We accept the struct and never call the callback; NGX logging
is optional and there is nothing NVIDIA-shaped to relay.

## Where the pass runs, and what that means for the picture

Immediately after the game's upscaler by default, on the game's own command list, before the UI is
drawn. OptiScaler hands us a *display-referred proxy* it has already tone-mapped, not the game's
linear-light buffer — so the runtime is told not to encode it a second time
(`Session::EngineResources::colour_encoded`). Blending the answer back into the frame, including
`TransferStrength` and `ColourStrength`, is entirely OptiScaler's resolve shader and is not touched
here.

Nothing is recorded into the command list at create: NGX builds its feature there, whereas
`nr::Runtime` is built on the session's own queue on a background thread. The caller already allows
for that — it returns without evaluating after a create and waits a submission epoch — and the first
frames pass through as an exact copy of the input either way, which composites to exactly the proxy.

## What is not implemented

- **Native D3D11.** `dlssnr_d3d11_probe` returns 0, the reference's "no such surface" answer, and
  OptiScaler keeps routing D3D11 games through its existing Dx11-on-Dx12 bridge, which ends in the
  D3D12 path above. `NVSDK_NGX_D3D11_CreateFeature` likewise still declines feature 18.
- **`dlssnr_query_scaling_ratio`** returns 0, "the callback was never published". The network is
  same-resolution by construction, so there is no ratio to report; OptiScaler logs it and runs at
  native.
- **`DLSSNR.GlobalToneStrength`** is read and ignored. So is it in the reference: a scan of
  `nvngx_dlssnr.dll` for `DLSSNR.*` yields 61 names and this is not one of them, which is why
  wilsjo2's own `DlssNr_Common.h` leaves the constant commented out and never writes it. Our network
  has no such control either — `nr::Controls` carries `local_tone`, which is a different thing.
- **`DLSSNR.UICorrection`, `.UI`, `.UIAlpha`, `.Backbuffer`** and their twelve subrect keys are read
  and ignored: there is no UI-correction pass on this side, and the caller writes null and zero for
  all of them anyway.
- **A non-zero subrect origin.** The pass can express a subrect *extent* but not an origin; a guide
  whose data starts at a non-zero corner is read as the whole allocation, said once in the log rather
  than silently. The caller always writes origin 0 for colour and output, and real origins only for
  depth and motion.
- **Vulkan needs a queue it has no way to ask for.** `NVSDK_NGX_VULKAN_Init*` carries the instance,
  the physical device and the device, but no queue, and constructing `nr::Runtime` has to submit the
  weight upload. The device watcher (`linux/src/pe/nr_pe_vkdevice.cpp`) hooks `vkGetDeviceQueue` and
  remembers what the game asked for, which only works if it was installed before the game created its
  device. When it has nothing the Vulkan path declines and says so, rather than calling
  `vkGetDeviceQueue` on a family the game may never have requested.

## How it is tested

In games (Dying Light: The Beast, Kingdom Come: Deliverance II, 007 First Light, Helldivers 2, Resident
Evil Requiem and others) and offline: the core's OptiScaler fixes are applied to the real OptiScaler
DLL under Wine and every patched byte is checked, and a stand-in D3D12 DLSS game drives the whole route
(OptiScaler as `dxgi.dll`, vkd3d-proton, our core) frame by frame, comparing outputs byte for byte
between releases.
