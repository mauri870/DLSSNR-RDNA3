#pragma once
// Passes (fast): one network pass strengthened after it, its effect referred to legacy pass counts - the wilsjo2
// layers' copy of the arithmetic the RenoDX-style stage runs (nr_renodx.cpp keeps its own, on purpose: the two stay
// independent; shaders linux/shaders/renodx-fast/st_*.comp): the strength table, the programs' push blocks and the
// order of their dispatches. A change to the table belongs in both places. A caller
// holds the images and records the dispatches (`Dispatcher`); the network's input P and answer N are RGBA16F storage
// images at the network's extent, N edited in place, P given the feedback and given back after the network.
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nr::fast {

enum Program : uint32_t { kFbIn, kPrep, kBox, kApply, kProgramCount };
inline const char* program_name(Program p) {
    static const char* const names[] = {"st_fb_in", "st_prep", "st_box", "st_apply"};
    return names[p];
}
// 16 x 16 groups over the network's extent (st_box: over its quarter-size grid).
inline uint32_t groups(uint32_t n) { return (n + 15) / 16; }
inline uint32_t low(uint32_t n) { return (n + 3) / 4; }

// The table: k the extrapolation's gain, w the feedback's weight; 1..8 legacy passes.
struct Strength { float k, w, n; };
inline Strength strength(float passes) {
    const float n = std::clamp(passes, 1.f, 8.f);
    const uint32_t segment = std::min(uint32_t(n), 3u) - 1u;
    const float kt[] = {1.f, 1.77f, 2.36f, 2.94f}, wt[] = {0.f, 0.5f, 0.67f, 0.75f};
    const float part = n - float(segment + 1u);
    Strength s;
    s.k = n > 4.f ? 2.94f + 0.58f * (n - 4.f) : kt[segment] + part * (kt[segment + 1] - kt[segment]);
    s.w = n > 4.f ? std::min(0.8f, 1.f - 1.f / n) : wt[segment] + part * (wt[segment + 1] - wt[segment]);
    s.n = n;
    return s;
}

// The images of one layer's fast work: the quarter-size guided-filter work images (RGBA32F), the feedback history
// pair (RGBA32F, quarter size, two frames), the full-size working images (kFineH RG16F: the edit's fine part and the
// answer's lightness; kFbAdd RGBA16F). kFbAdd and the history only for Feedback / Hybrid (feedback_image).
enum Image : uint32_t {
    kP, kN, kMotion,                 // the network's input, its answer, the game's motion vectors (the caller's)
    kFbAdd, kFineH,                  // full size: RGBA16F, RG16F
    kD0, kD1, kT0, kT1, kG0, kG1,    // RGBA32F, quarter size
    kF0a, kF0b, kF1a, kF1b,          // RGBA32F, quarter size: the feedback history (a: written in even frames)
    kImageCount
};
struct Binding { uint32_t binding; Image image; };
inline bool feedback_image(Image i) { return i == kFbAdd || (i >= kF0a && i <= kF1b); }

struct FbInPush { uint32_t size[2], low[2], mbase[2], mgrid[2]; float mscale[2], w; uint32_t flags, block; float cstop, darkcol; };
struct PrepPush { uint32_t size[2], low[2], src; };
struct BoxPush { uint32_t low[2], radius, coef; float eps; };
struct ApplyPush {
    uint32_t size[2], low[2]; float k; uint32_t mode; float halo, chroma; uint32_t satkeep;
    float fine, base, rho, lam, dcol, cfloor, ring, ccap, nohalo; uint32_t restored; float ocap, darkcol;
};

struct Settings {
    float passes = 3.f;          // 1..8
    uint32_t method = 0;         // 0 Extrapolation, 1 Feedback, 2 Hybrid, 3 Edit strength
    bool halo_limit = false;
    float fine = 1.0f;           // the fine-detail part x (the method's own Fine; 1: as made)
    float edit_strength = 2.0f;  // Edit strength (method 3): the edit's stops x, as the look's Edit strength
};
// Whether the fast work runs at all (one legacy pass is the network's own answer).
inline bool active(const Settings& s) { return s.method == 3u ? s.edit_strength != 1.f : s.passes > 1.f; }

// What one frame's dispatches are recorded through: `dispatch(program, bindings, count, push, bytes, gx, gy)` with a
// barrier after it (every write visible to whatever comes next).
struct Dispatcher {
    virtual ~Dispatcher() = default;
    virtual bool dispatch(Program p, const Binding* bindings, uint32_t count, const void* push, uint32_t bytes,
                          uint32_t gx, uint32_t gy) = 0;
};

// The feedback's motion: the vectors' region in their texture and the scale from a texel to the network's pixels.
struct Motion { uint32_t base[2] = {0, 0}, grid[2] = {0, 0}; float scale[2] = {0.f, 0.f}; };

// One layer's state across frames: which history image holds the last frame's, and whether it is valid.
struct History { uint32_t current = 0; bool valid = false; };

inline Image history_a(const History& h, bool next) { return (h.current ^ (next ? 1u : 0u)) ? kF0b : kF0a; }
inline Image history_b(const History& h, bool next) { return (h.current ^ (next ? 1u : 0u)) ? kF1b : kF1a; }

// The guided filter's rest, from its 4 x 4 cell means in (kD0, kD1): the box mean, the coefficients, their box mean
// into (outa, outb).
inline bool guided_filter(Dispatcher& d, uint32_t w, uint32_t h, Image outa, Image outb, uint32_t radius, float eps) {
    const uint32_t gx = groups(low(w)), gy = groups(low(h));
    BoxPush g{{low(w), low(h)}, radius, 1u, eps};
    const Binding first[] = {{0, kD0}, {1, kD1}, {4, kT0}, {5, kT1}};
    if (!d.dispatch(kBox, first, 4, &g, sizeof g, gx, gy)) return false;
    g.coef = 0u;
    const Binding second[] = {{0, kT0}, {1, kT1}, {4, outa}, {5, outb}};
    return d.dispatch(kBox, second, 4, &g, sizeof g, gx, gy);
}

// Before the network: with Feedback / Hybrid and motion, the last frame's edit moved by the motion vectors into P
// (kept in kFbAdd to be given back). `feedback`: the method runs as Feedback / Hybrid this frame.
inline bool feed(Dispatcher& d, const Settings& s, uint32_t w, uint32_t h, const Motion& m, const History& hist,
                 bool feedback) {
    if (!feedback) return true;
    const Strength st = strength(s.passes);
    FbInPush fp{{w, h}, {low(w), low(h)}, {m.base[0], m.base[1]}, {m.grid[0], m.grid[1]}, {m.scale[0], m.scale[1]},
                st.w, 16u | 2u | (hist.valid ? 1u : 0u), 4u, 0.f, 0.45f};
    const Binding b[] = {{0, history_a(hist, false)}, {2, history_b(hist, false)}, {1, kMotion}, {4, kP}, {5, kFbAdd}};
    return d.dispatch(kFbIn, b, 5, &fp, sizeof fp, groups(w), groups(h));
}

// After the network: N strengthened in place; with feedback, P given back and this frame's edit kept as the next
// frame's history (`hist` advanced).
inline bool apply(Dispatcher& d, const Settings& s, uint32_t w, uint32_t h, History& hist, bool feedback) {
    const Strength st = strength(s.passes);
    const bool fb = feedback;
    ApplyPush ap{{w, h}, {low(w), low(h)}, st.k, fb ? (s.method == 2u ? 8u : 6u) : 9u,
                 fb ? st.n - 1.f : (s.halo_limit ? 0.f : 1.f), fb ? 0.6f : 0.3f, 0u,
                 fb ? 1.f : 2.f, 1.f, 0.3f, 0.f, 0.5f, fb ? 0.f : 0.6f, 2.f, fb ? 0.f : 2.5f,
                 fb ? (s.halo_limit ? 1.f : 0.5f) : 0.f, fb ? 1u : 0u, fb ? -0.5f : 0.f, fb ? 0.45f : 0.f};
    const float half = 0.5f;
    std::memcpy(&ap.satkeep, &half, sizeof half);   // the slot holds 0.5's bits
    if (fb && s.method == 1u) ap.k = st.n - 1.f;
    // Fine detail: Extrapolation's fine band, Feedback's detail add (k), Hybrid's feedback branch's (halo); 1 as made
    const float fine = s.fine;
    if (fine != 1.f) {
        if (!fb) ap.fine *= fine;
        else if (s.method == 1u) ap.k *= fine;
        else { ap.halo *= fine; ap.fine = fine; }   // Hybrid: its feedback branch and its overlay's fine part
    }
    const bool edit = s.method == 3u;   // Edit strength: the apply alone (no fine part, no guided filter)
    if (edit) { ap.mode = 10u; ap.k = std::clamp(s.edit_strength, 0.f, 4.f); }
    const uint32_t gx = groups(w), gy = groups(h);
    if (!edit) {   // the edit's fine part and the guided filter's cell means (of the lightness edit, or with feedback the RGB one)
        const PrepPush pp{{w, h}, {low(w), low(h)}, fb ? 1u : 0u};
        const Binding b[] = {{0, kP}, {1, kN}, {4, kFineH}, {5, kD0}, {6, kD1}};
        if (!d.dispatch(kPrep, b, 5, &pp, sizeof pp, gx, gy)) return false;
        if (!guided_filter(d, w, h, kG0, kG1, fb ? 2u : 4u, fb ? 1e9f : 1e-3f)) return false;
    }
    // N strengthened; with feedback (`restored`) also P given back and this frame's edit's cell means
    {
        const Binding b[] = {{0, kP}, {1, fb ? kFbAdd : kFineH}, {2, kG0}, {3, kG1}, {4, kN}, {5, kFineH}, {6, kD0}, {7, kD1}};
        if (!d.dispatch(kApply, b, 8, &ap, sizeof ap, gx, gy)) return false;
    }
    if (fb) {   // filtered into the next frame's history
        if (!guided_filter(d, w, h, history_a(hist, true), history_b(hist, true), 2u, 1e9f)) return false;
        hist.current ^= 1u;
        hist.valid = true;
    }
    return true;
}

}  // namespace nr::fast
