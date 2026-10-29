#pragma once
// The RenoDX-style option's NR screenshot (RenoDX v8.5.0-rc10 screenshot.hpp, MIT): one frame, saved before and after
// NR - PNG (16-bit PQ BT.2020 for HDR, 8-bit sRGB for SDR), JPEG, or both; optionally a side-by-side file. The
// encoding, the files, their names and the colour signalling (PNG cICP / iCCP / cLLi, JPEG ICC) are RenoDX's,
// ported as they are; what differs is where the two copies come from (our Vulkan session, nr_pe_session.cpp):
//   - the final picture before and after NR: on the ReShade route the presented frame (RenoDX's Present hook point);
//     on the OptiScaler route OptiScaler's finished picture, NR moved there for the capture as RenoDX moves it to
//     Present (nr_pe_optifix.cpp);
//   - NRScreenshotSource = 2: the model's input and output (RenoDX's evaluated-buffer diagnostic).
// Files go to <game>\NR Screenshots. Idle, it costs a key poll a frame; everything heavy runs on a worker thread.
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace nr::pe::screenshot {

// [RenoDX] NRScreenshot* (RenoDX's names and ranges).
struct Options {
    uint32_t source = 0;         // 0 automatic (the final picture), 1 the final picture, 2 the model's input / output
    uint32_t format = 0;         // 0 PNG, 1 JPEG, 2 both
    uint32_t layout = 0;         // 0 the pair, 1 the pair and a side-by-side file
    uint32_t jpeg_quality = 95;  // 80 .. 100
    uint32_t max_megabytes = 10; // a JPEG's size cap, 0 none
};
void configure(const Options& o);

// Arms one capture (the hotkey, the menus' button). False while one is armed or in flight.
bool request();
bool armed();
bool wants_final();   // armed for the final picture
bool wants_model();   // armed for the model's input / output
// Ends an armed request without files: `outcome` is RenoDX's (unsupported_present, nr_disabled, ...).
void fail(const char* outcome, const char* explanation);
// Once a frame from the thread that records NR: an armed request with no capture frame for 600 frames gives up.
void tick();
// The menus' status line (RenoDX's StatusText).
const char* status();

// One captured pair, read by the worker from host memory the GPU wrote. `release` is called on the worker once it
// has read both (the memory then belongs to its owner again); `unwritten` / `black` are checked there as RenoDX does.
struct Pair {
    bool model_diagnostic = false;
    uint32_t width = 0, height = 0;
    uint32_t row_pitch = 0;          // bytes a row in both images
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkFormat pre_format = VK_FORMAT_UNDEFINED; // optional differing model-input format
    uint32_t pre_row_pitch = 0;
    uint8_t hdr_mode = 0;            // 0 SDR (display codes), 1 linear HDR (scRGB), 2 PQ HDR
    float linear_unit_nits = 80.0f;  // hdr_mode 1: nits of 1.0
    const void* pre = nullptr;
    const void* post = nullptr;
    uint64_t bytes = 0;              // each image's size
    std::string meta;
    void (*release)(void* owner) = nullptr;
    void* owner = nullptr;
};
// Bytes a readback starts with before the copy: still there when it is read = the copy never ran.
void stamp(void* mapped, uint64_t bytes);
// Both copies of a capture are recorded: no longer armed, waiting for the GPU (the menus say so).
void recorded();
// Hands a captured pair to the worker.
void deliver(Pair pair);
// A capture prepared but abandoned (no files): the request ends with `outcome`.
void abandon(const char* outcome, const char* explanation);

// Where the files go; by default <game>\NR Screenshots (the folder of this module).
void set_folder(const std::wstring& folder);

// The formats a capture can be read from.
bool readable(VkFormat format);
uint32_t bytes_per_pixel(VkFormat format);

}  // namespace nr::pe::screenshot
