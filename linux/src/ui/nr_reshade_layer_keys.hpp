#pragma once
// A wilsjo2 layer's keys on the ReShade route ([LayerN] Processing = wilsjo2, read by nr_pe_config.cpp layer_config): the
// original's [DlssNr] names as the .asi's layers have them (nr_wilsjo2_keys.hpp), only those this route runs, Transfer
// with our two enlargements as 5 and 6, and the route's own ClassicScaler, History and NoiseEveryFrame. The table shape of
// nr_renodx_keys.hpp; Passes and Pass<N> are drawn apart (nr_reshade_addon.cpp).
#include "nr_renodx_keys.hpp"
#include "nr_wilsjo2_shared_keys.hpp"

namespace nr::reshade_layer_keys {
using renodx_keys::Key;
using renodx_keys::kBool;
using renodx_keys::kFloat;
using renodx_keys::kText;
using renodx_keys::kUint;
inline constexpr Key kKeys[] = {
    wilsjo2_keys::kHistory,
    {"Style", kUint, 0, 0.0f, 2.0f, 0.0f, "Network", "Style", "Style.", "Standard\0Natural\0Cinematic\0"},
    {"Intensity", kFloat, 2, 0.0f, 2.0f, 1.0f, "Network", "Intensity", "Intensity. Enhancement strength.", nullptr},
    {"LocalStructure", kFloat, 2, 0.0f, 2.0f, 1.0f, "Network", "Local structure", "LocalStructure. Fine detail and local contrast.", nullptr},
    {"LocalTone", kFloat, 2, 0.0f, 2.0f, 1.0f, "Network", "Local tone", "LocalTone. Broad lighting changes.", nullptr},
    {"SkinStructure", kFloat, 2, -1.0f, 2.0f, -1.0f, "Network", "Skin structure", "SkinStructure. -1: follows Local structure.", nullptr},
    {"AutoMask", kBool, 0, 0.0f, 0.0f, 1.0f, "Network", "Auto skin mask", "AutoMask. Model-based skin selection.", nullptr},
    {"TransferStrength", kFloat, 2, 0.0f, 2.0f, 1.0f, "Composition", "Detail strength", "TransferStrength. 0 = no detail change, 1 = normal.", nullptr},
    {"ColourStrength", kFloat, 2, 0.0f, 4.0f, 1.0f, "Composition", "Colour strength", "ColourStrength. 0 = game colours, 1 = model colours, above 1 boosts saturation.", nullptr},
    {"MaxRatio", kFloat, 1, 1.0f, 8.0f, 2.0f, "Composition", "Highlight guard", "MaxRatio. Limits pixel brightening and darkening.", nullptr},
    {"ReversibleMode", kUint, 0, 0.0f, 4.0f, 0.0f, "Composition", "HDR mapping", "ReversibleMode. The highlight curve a floating-point HDR frame is shown to the network through; the replace modes put the network's picture in place of the composition (SDR frames too).", "Soft knee\0Neutwo\0Neutwo, replace\0Hybrid\0Hybrid, replace\0"},
    {"ReplaceDetailStrength", kFloat, 2, 0.0f, 2.0f, 0.0f, "Composition", "Restore sharpness", "ReplaceDetailStrength. The replace modes below 100 % model resolution: the game's local contrast put back on the network's picture.", nullptr},
    {"WorkingScale", kFloat, 2, 0.25f, 2.0f, 1.0f, "Resolution", "Model resolution", "WorkingScale. The network's resolution as a fraction of the frame's: below 1 faster, its answer enlarged back; above 1 (supersampling) the frame is enlarged for the network and its answer brought back, slower.", nullptr},
    {"Downscale", kUint, 0, 0.0f, 1.0f, 0.0f, "Resolution", "Downscale (below 100 %)", "Downscale. Below 100 % only: how the frame is brought to the network's resolution - area after encoding (the original's), or bilinear before encoding.", "Area after encoding\0Bilinear before encoding\0"},
    {"ScalingDownscaler", kUint, 0, 0.0f, 7.0f, 4.0f, "Resolution", "Supersampling filter", "ScalingDownscaler. Above 100 % only: the filter that brings the answer back to the frame's size (FSR 1: a bilinear resample both ways; the others enlarge the frame bicubically).", "FSR 1\0Bicubic\0Catmull-Rom\0Lanczos 2\0Lanczos 3\0Kaiser 2\0Kaiser 3\0MAGIC\0"},
    {"Transfer", kUint, 0, 0.0f, 4.0f, 1.0f, "Resolution", "Enlargement", "Transfer. Below 100 % only: how the network's answer is brought back to the frame.", "Bilinear\0Matched residual\0Lighting + colour\0Edge-aware lighting + colour\0Classic\0"},
    {"ClassicScaler", kUint, 0, 0.0f, 3.0f, 1.0f, "Resolution", "Classic scaler", "ClassicScaler. Enlargement Classic: the scaler.", "Bilinear\0Catmull-Rom\0Lanczos3\0FSR 1\0"},
    {"SpatialCompression", kBool, 0, 0.0f, 0.0f, 0.0f, "Peripheral compression", "Peripheral compression", "SpatialCompression. The network sees the centre band at full density and the periphery compressed: faster, the edges of the picture get less of NR.", nullptr},
    {"SpatialCenterX", kFloat, 1, 1.0f, 99.0f, 80.0f, "Peripheral compression", "Centre width %", "SpatialCenterX. The full-density band's width, percent of the frame.", nullptr},
    {"SpatialCenterY", kFloat, 1, 1.0f, 99.0f, 80.0f, "Peripheral compression", "Centre height %", "SpatialCenterY.", nullptr},
    {"SpatialWorkX", kFloat, 1, 25.0f, 100.0f, 90.0f, "Peripheral compression", "Work width %", "SpatialWorkX. The packed picture's width, percent of the frame; above the centre band.", nullptr},
    {"SpatialWorkY", kFloat, 1, 25.0f, 100.0f, 90.0f, "Peripheral compression", "Work height %", "SpatialWorkY.", nullptr},
    {"SpatialOffsetX", kFloat, 1, -50.0f, 50.0f, 0.0f, "Peripheral compression", "Centre offset X %", "SpatialOffsetX. Moves the centre band.", nullptr},
    {"SpatialOffsetY", kFloat, 1, -50.0f, 50.0f, 0.0f, "Peripheral compression", "Centre offset Y %", "SpatialOffsetY.", nullptr},
    {"SpatialShiftX", kFloat, 1, -50.0f, 50.0f, 0.0f, "Peripheral compression", "Work shift X %", "SpatialShiftX. Moves the room between the sides (clamped to what they have).", nullptr},
    {"SpatialShiftY", kFloat, 1, -50.0f, 50.0f, 0.0f, "Peripheral compression", "Work shift Y %", "SpatialShiftY.", nullptr},
    {"WhitePointScale", kFloat, 2, 0.01f, 16.0f, 1.0f, "Exposure (HDR)", "White point", "WhitePointScale. Floating-point HDR frames: the value shown to the network as white. 1 = default.", nullptr},
    {"SkinProtection", kBool, 0, 0.0f, 0.0f, 0.0f, "Skin and environment", "Skin protection", "SkinProtection. NR's change kept by how skin-coloured each pixel is.", nullptr},
    {"SkinDetail", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Skin detail", "SkinDetail.", nullptr},
    {"SkinColour", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Skin colour", "SkinColour.", nullptr},
    {"EnvironmentDetail", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Environment detail", "EnvironmentDetail.", nullptr},
    {"EnvironmentColour", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Environment colour", "EnvironmentColour.", nullptr},
    {"NoiseEveryFrame", kBool, 0, 0.0f, 0.0f, 0.0f, "This route's own", "New noise without motion vectors", "NoiseEveryFrame. Frames without motion vectors: a new noise seed every frame, as NVIDIA's DLL does.", nullptr},
};
inline constexpr int kCount = int(sizeof kKeys / sizeof kKeys[0]);
}  // namespace nr::reshade_layer_keys
