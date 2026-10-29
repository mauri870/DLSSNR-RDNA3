#pragma once
#include <cstdint>
#include <string>
namespace nr {
struct NativePlan {
    uint32_t width{}, height{}, reductions_x{}, reductions_y{};
    std::string text, model;
};
// Source and returned working dimensions are raster dimensions. Plan scalars
// preserve the recovered host order (height, width); pixels are never rotated.
// The network the plan walks: the 310.8 model (also preset 1 of the 310.9 DLL) or the
// 310.9 small model (preset 2, CC_SMALL_FP8): same layer shapes and kernels, fewer blocks.
enum class NetworkModel { Original, Small };
NativePlan make_native_plan(uint32_t source_width, uint32_t source_height,
                            NetworkModel model = NetworkModel::Original);
}
