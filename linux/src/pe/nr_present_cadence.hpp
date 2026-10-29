#pragma once
// How many presents a game frame has under frame generation, for the .asi's Present layers: RenoDX's PresentCadence
// (RenoDX-DLSS5-Generic v8.5.0-rc10, present_cadence.hpp, MIT, Copyright (C) 2026), unchanged but for its namespace.
// A declared DLSS-G rate (DLSSG.MultiFrameCount + 1) wins at once; else whole-number multipliers are taken from the
// presents per game frame over one-second windows of at least 16 game frames, a new one only after two windows agree,
// and a stall or a stretch without fresh guides restarts the window (NRPresentFgSource 2: the older timing rule).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nr::pe::present {

// NRPresentFgSource: declared DLSS-G count with a stable timing fallback, stable timing only, or the pre-hardening
// timing rule for compatibility.
enum class FgSource : uint32_t { kAuto = 0, kTiming = 1, kLegacyTiming = 2 };

// Presentation cadence is evidence about a stream, not an instantaneous frame-generation setting. A one-second window
// can straddle FG activation, a CPU/GPU hitch, or queued presents; publishing its nearest integer once produced
// 3x -> 4x -> 3x -> 4x on a declared 4x field session (RenoDX, 2026-09-25).
struct Cadence {
    static constexpr int64_t kWindowNs = 1'000'000'000;
    static constexpr int64_t kStallNs = 250'000'000;
    int64_t window_start_ns = 0;
    int64_t last_present_ns = 0;
    uint64_t window_presents = 0;
    uint64_t window_captures = 0;
    float cadence = 1.f;
    uint32_t multiplier = 1;
    uint32_t candidate = 0;
    uint32_t candidate_windows = 0;
    uint32_t low_windows = 0;
    FgSource source = FgSource::kAuto;
    bool declared = false;

    // `declaration` is 0 when absent/ambiguous, otherwise the observed DLSSG.MultiFrameCount + 1. A declaration wins
    // immediately; losing one keeps the current multiplier until the fallback has actual evidence for another value.
    // True when the multiplier changed.
    bool observe(int64_t now_ns, uint64_t captures, bool fresh, uint32_t declaration, FgSource mode) {
        const uint32_t previous = multiplier;
        const bool legacy = mode == FgSource::kLegacyTiming;
        const bool use_declaration = mode == FgSource::kAuto && declaration >= 1 && declaration <= 8;
        const bool restart = window_start_ns == 0 || mode != source || declared != use_declaration ||
                             captures < window_captures ||
                             (!legacy && (now_ns - last_present_ns > kStallNs || !fresh));
        source = mode;
        declared = use_declaration;
        last_present_ns = now_ns;
        if (use_declaration) multiplier = declaration;
        if (restart) {
            window_start_ns = now_ns;
            window_presents = 0;
            window_captures = captures;
            candidate = candidate_windows = low_windows = 0;
            return multiplier != previous;
        }
        if (fresh) ++window_presents;
        if (now_ns - window_start_ns < kWindowNs) return multiplier != previous;
        const uint64_t count = captures - window_captures;
        // More complete game frames bound the phase error at the window edges. Slow but continuous streams accumulate
        // them across multiple seconds.
        if (!legacy && count < 16) return multiplier != previous;
        if (count >= 8) {
            cadence = static_cast<float>(window_presents) / static_cast<float>(count);
            if (!use_declaration && legacy) {
                low_windows = cadence < 1.25f ? low_windows + 1 : 0;
                const auto nearest = static_cast<uint32_t>(std::clamp(std::lround(cadence), 2l, 8l));
                if (multiplier == 1) {
                    if (cadence >= 1.5f) multiplier = nearest;
                } else if (low_windows >= 2) {
                    multiplier = 1;
                } else if (std::abs(cadence - static_cast<float>(multiplier)) > 0.75f) {
                    multiplier = nearest;
                }
            } else if (!use_declaration) {
                const auto nearest = static_cast<uint32_t>(std::clamp(std::lround(cadence), 1l, 8l));
                const uint32_t measured =
                    cadence < 1.25f ? 1u
                    : cadence >= 1.5f && std::abs(cadence - static_cast<float>(nearest)) <= 0.25f ? nearest : 0u;
                if (measured == 0 || measured == multiplier) {
                    candidate = candidate_windows = 0;
                } else {
                    candidate_windows = candidate == measured ? candidate_windows + 1 : 1;
                    candidate = measured;
                    if (candidate_windows >= 2) {
                        multiplier = measured;
                        candidate = candidate_windows = 0;
                    }
                }
            }
        }
        window_start_ns = now_ns;
        window_presents = 0;
        window_captures = captures;
        return multiplier != previous;
    }
};

}  // namespace nr::pe::present
