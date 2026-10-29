#pragma once
// A wilsjo2 layer's keys for NR Control (the .asi's [LayerN] section with Processing = wilsjo2): name (the fork's [DlssNr]
// name, as nr_wilsjo2.cpp parse_key reads it), kind, range, default, group, label, help and combo items - the table
// shape of nr_renodx_keys.hpp. Ranges and defaults as the fork's [DlssNr]; only
// the keys the wilsjo2 processing runs so far.
#include "nr_renodx_keys.hpp"
#include "nr_wilsjo2_shared_keys.hpp"

namespace nr::wilsjo2_keys {
using renodx_keys::Key;
using renodx_keys::kBool;
using renodx_keys::kFloat;
using renodx_keys::kText;
using renodx_keys::kUint;
inline constexpr Key kKeys[] = {
    wilsjo2_keys::kHistory,
    {"Style", kUint, 0, 0.0f, 2.0f, 0.0f, "Network", "Style", "Style.", "Standard\0Natural\0Cinematic\0"},
    {"Intensity", kFloat, 2, 0.0f, 2.0f, 1.0f, "Network", "Intensity", "Intensity. Enhancement strength.", nullptr},
    {"LocalStructure", kFloat, 2, 0.0f, 2.0f, 1.0f, "Network", "Local structure", "LocalStructure.", nullptr},
    {"LocalTone", kFloat, 2, 0.0f, 2.0f, 1.0f, "Network", "Local tone", "LocalTone.", nullptr},
    {"SkinStructure", kFloat, 2, -1.0f, 2.0f, -1.0f, "Network", "Skin structure", "SkinStructure. -1: follows Local structure.", nullptr},
    {"AutoMask", kBool, 0, 0.0f, 0.0f, 1.0f, "Network", "Character mask", "AutoMask.", nullptr},
    {"NoiseEveryFrame", kBool, 0, 0.0f, 0.0f, 1.0f, "Network", "New noise without motion vectors", "NoiseEveryFrame (ours). Frames without motion vectors: a new noise seed every frame, as NVIDIA's DLL does (default). Off: the seed stays. Frames with motion vectors always get a new one.", nullptr},
    {"TransferStrength", kFloat, 2, 0.0f, 2.0f, 1.0f, "Composition", "Detail strength", "TransferStrength. How much of the network's detail change is kept.", nullptr},
    {"ColourStrength", kFloat, 2, 0.0f, 2.0f, 1.0f, "Composition", "Colour strength", "ColourStrength. How much of the network's colour change is kept.", nullptr},
    {"MaxRatio", kFloat, 2, 1.0f, 8.0f, 2.0f, "Composition", "Highlight guard", "MaxRatio. The most a pixel's brightness may change, either way.", nullptr},
    {"ReversibleMode", kUint, 0, 0.0f, 4.0f, 0.0f, "Composition", "HDR mapping", "ReversibleMode. The highlight curve the network sees HDR through, and whether its answer is composed onto the frame or replaces it.", "Soft knee\0Neutwo\0Neutwo, replace\0Hybrid\0Hybrid, replace\0"},
    {"ReplaceDetailStrength", kFloat, 2, 0.0f, 2.0f, 0.0f, "Composition", "Restore sharpness", "ReplaceDetailStrength. Detail restoration in the replace modes.", nullptr},
    {"WorkingScale", kFloat, 2, 0.25f, 2.0f, 1.0f, "Resolution", "Model resolution", "WorkingScale. The network's resolution as a fraction of the frame's: below 1 faster, its answer enlarged back; above 1 (supersampling) the frame is enlarged for the network and its answer brought back, slower (the network's cost grows with the area).", nullptr},
    {"Downscale", kUint, 0, 0.0f, 1.0f, 0.0f, "Resolution", "Downscale (below 100 %)", "Downscale. Below 100 % only: how the frame is brought to the network's resolution - area after encoding (the original's), or bilinear before encoding.", "Area after encoding\0Bilinear before encoding\0"},
    {"ScalingDownscaler", kUint, 0, 0.0f, 7.0f, 4.0f, "Resolution", "Supersampling filter", "ScalingDownscaler. Above 100 % only: the filter that brings the answer back to the frame's size (FSR 1: both ways as OptiScaler runs it, which is a bilinear resample; the others enlarge the frame bicubically).", "FSR 1\0Bicubic\0Catmull-Rom\0Lanczos 2\0Lanczos 3\0Kaiser 2\0Kaiser 3\0MAGIC\0"},
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
    {"WhitePointSource", kUint, 0, 0.0f, 3.0f, 1.0f, "Exposure", "White point source", "WhitePointSource. HDR frames only.", "Manual\0The game's exposure\0Manual (2)\0Automatic\0"},
    {"WhitePointScale", kFloat, 2, 0.01f, 16.0f, 1.0f, "Exposure", "White point", "WhitePointScale. The manual white point.", nullptr},
    {"WhitePointTrim", kFloat, 2, 0.001f, 16.0f, 1.0f, "Exposure", "Exposure trim", "WhitePointTrim. Scales the game's exposure.", nullptr},
    {"ExposureTrimAnchors", kText, 0, 0.0f, 0.0f, 0.0f, "Exposure", "Exposure trim curve", "ExposureTrimAnchors. point:trim pairs, e.g. 0.5:1.2 2:0.9; empty: none.", nullptr},
    {"AutoExposureTrim", kFloat, 2, 0.001f, 16.0f, 5.0f, "Exposure", "Automatic exposure trim", "AutoExposureTrim. Scales the automatic exposure.", nullptr},
    {"AutoExposureHighlightProtection", kFloat, 0, 0.0f, 100.0f, 0.0f, "Exposure", "Highlight protection", "AutoExposureHighlightProtection. Keeps bright areas from pulling the automatic exposure down.", nullptr},
    {"AutoExposureTrimAnchors", kText, 0, 0.0f, 0.0f, 0.0f, "Exposure", "Automatic exposure trim curve", "AutoExposureTrimAnchors. point:trim pairs; empty: none.", nullptr},
    {"SkinProtection", kBool, 0, 0.0f, 0.0f, 0.0f, "Skin and environment", "Skin protection", "SkinProtection. Softens the edit on skin colours.", nullptr},
    {"SkinDetail", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Skin detail", "SkinDetail.", nullptr},
    {"SkinColour", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Skin colour", "SkinColour.", nullptr},
    {"EnvironmentDetail", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Environment detail", "EnvironmentDetail.", nullptr},
    {"EnvironmentColour", kFloat, 2, 0.0f, 1.0f, 1.0f, "Skin and environment", "Environment colour", "EnvironmentColour.", nullptr},
};
inline constexpr int kCount = int(sizeof kKeys / sizeof kKeys[0]);
}  // namespace nr::wilsjo2_keys
