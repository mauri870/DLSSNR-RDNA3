#pragma once
// What the RenoDX-style option's OptiScaler hook (nr_pe_optifix.cpp) learns, for the model (nr_dlssnr_model.cpp).
// Its own unit because the model is linked into DLLs that carry no OptiScaler fixes: there nothing is ever set.
#include <cstdint>

namespace nr::pe {

// The RenoDX-style option on the OptiScaler route ([RenoDX] Enabled = 1 in dlssnr-amd.ini when this module
// loads; nothing of this is installed otherwise). OptiScaler then hands the frame to the model and takes the
// model's answer back as they are, so the RenoDX processing around the network (nr_renodx.hpp) is all there
// is: DlssNr_Dx12::DispatchPass is hooked and, for its encode and resolve, given Passthrough = 1 (the frame
// copied, the answer output, no white point, no proxy curve) and ReversibleMode = 2 (the answer replaces the
// frame, none of OptiScaler's composition), its resolve also SkinProtection = 0 (that filter works on the
// composed edit, and with Passthrough it would judge a linear frame as a display one); its ClampProxy between
// two model passes becomes the same-size copy of its Downsample, so a linear frame is not cut at 1. Where the frame
// can be read and written in place (set_direct below), the encode and the resolve are skipped altogether (its debug
// views and comparison, which would read their copies, are locked off while the option runs). Found in v0.8.91 by the DispatchCompute
// string, the argument set-up of DispatchPass's call to it and that function's prologue, each exactly once; any other
// build is left alone and the option stays off. D3D12 only (OptiScaler's Vulkan path has its own code).
namespace renodx_route {
bool installed();
void set_installed();
// OptiScaler's own judgement at its most recent encode (Passthrough was 0: a linear HDR frame), that encode's
// extent (the frame's) and how many encodes it has made; the model reads it for the frame it is handed right
// after. A model input smaller than the encode means OptiScaler's Model resolution or Peripheral compression made
// it smaller, which this route cannot carry: the answer comes back enlarged as it is.
struct Encode {
    uint64_t count = 0;
    bool hdr = false;
    uint32_t width = 0, height = 0;
};
Encode last_encode();
void note_encode(bool hdr, uint32_t width, uint32_t height);
// What OptiScaler's own encode used for the last frame (its DlssNrConstants, read without changing them, on every
// OptiScaler route): linear HDR or the frame passed through as it is (already tone mapped), and the white point it
// divided the linear frame by. For the menus' engine-data panel.
struct EncodeSeen {
    uint64_t count = 0;
    bool linear = false;
    float white_point = 0.f;
};
EncodeSeen encode_seen();
void note_encode_seen(bool linear, float white_point);
// The frame OptiScaler is running NR on (its DlssNrFrameInfo, read by the hook on DlssNr_Dx12::State::Run while
// that call is on this thread; the model's evaluate happens inside it), with what its MakeDlssNrPass saw of the
// game's DLSS evaluate just before (create flags, DLSS.Exposure.Scale). `known` is false outside such a call.
struct Frame {
    bool known = false;
    bool before_upscale = false;
    bool finished_picture = false;
    float pre_exposure = 1.0f;
    void* exposure = nullptr;          // the game's ID3D12Resource, borrowed for this evaluation
    // OptiScaler's Vulkan path: the game's exposure image (VkImage, its VkFormat and extent), in GENERAL.
    uint64_t vk_exposure = 0;
    uint32_t vk_exposure_format = 0, vk_exposure_width = 0, vk_exposure_height = 0;
    uint32_t exposure_state = 0;       // its D3D12_RESOURCE_STATES during the evaluation
    bool flags_known = false;          // MakeDlssNrPass ran for this frame
    uint32_t create_flags = 0;         // the game's NVSDK_NGX_DLSS_Feature_Flags
    float exposure_scale = 1.0f;
    // The game's DLSS evaluate as MakeDlssNrPass saw it: its render extent (DLSS.Render.Subrect.Dimensions, else
    // Width / Height) and the upscaler's output (OutWidth / OutHeight); 0 not given.
    uint32_t render_width = 0, render_height = 0, output_width = 0, output_height = 0;
};
Frame current_frame();
// The frame OptiScaler's encode would have copied, when the hook skipped that encode (and so its resolve): the model
// then reads and writes this resource itself (in UNORDERED_ACCESS, OptiScaler's state for it between the two), and a
// frame NR does not run on stays as it is. Null outside such a run.
void set_direct(void* frame);
void* direct();
void begin_run(const Frame& f);
void end_run();
void note_pass(uint32_t create_flags, float exposure_scale, uint32_t render_width, uint32_t render_height,
               uint32_t output_width, uint32_t output_height);
}  // namespace renodx_route

}  // namespace nr::pe
