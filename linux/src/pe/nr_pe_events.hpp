#pragma once
// NR's state changes as events, for NR Control's Diagnostics (nr_control_pages.hpp draw_diagnostics) and its report:
// what the log says of them, kept in memory too - a ring of the last kRing, each with its time and category, a repeat
// within two seconds counted on the last one. Also the layers' run state as it changes (the last stop and its reason,
// a layer switching between running and not many times a second) and the video memory's residency as the session last
// decided it. Everything here is CPU-side and in this process (the .asi and the ReShade add-on draw it themselves).
#include "nr_pe_log.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace nr::pe::events {

enum Category : uint8_t { kNetwork, kResidency, kRun, kSettings, kCategories };
inline const char* category_name(Category c) {
    static const char* const names[] = {"network", "memory", "run", "settings"};
    return c < kCategories ? names[c] : "other";
}

struct Event {
    uint64_t ms = 0;        // GetTickCount64 of its last time
    Category category = kNetwork;
    uint32_t count = 1;     // the same text again within two seconds: counted here
    char text[176] = {};
};
constexpr size_t kRing = 200;

struct Store {
    std::mutex m;
    Event ring[kRing];
    uint64_t written = 0;
    // the layers' run state (by layer number, 0..kMaxLayers): 1 ran, else not, with the note
    struct Layer { uint32_t state = ~0u; uint64_t changes[32] = {}; uint32_t next = 0; char why[96] = {}; };
    Layer layers[9];
    uint64_t stop_ms = 0;
    int stop_layer = 0;
    char stop_why[96] = {};
};
inline Store& store() { static Store s; return s; }

inline void record(Category c, const char* text) {
    Store& s = store();
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> g(s.m);
    if (s.written) {
        Event& last = s.ring[(s.written - 1) % kRing];
        if (last.category == c && now - last.ms < 2000 && std::strncmp(last.text, text, sizeof last.text - 1) == 0) {
            ++last.count;
            last.ms = now;
            return;
        }
    }
    Event& e = s.ring[s.written++ % kRing];
    e.ms = now; e.category = c; e.count = 1;
    std::snprintf(e.text, sizeof e.text, "%s", text);
}

// A log line that is also an event: "[nr] " + the text in dlssnr-amd.log, the text in the ring.
inline void note(Category c, const char* fmt, ...) {
    char text[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    nr::pe::log("[nr] %s", text);
    record(c, text);
}

// The last `n` events, oldest first.
inline std::vector<Event> recent(size_t n = kRing) {
    Store& s = store();
    std::lock_guard<std::mutex> g(s.m);
    const uint64_t have = std::min<uint64_t>(s.written, kRing), take = std::min<uint64_t>(have, n);
    std::vector<Event> out;
    for (uint64_t i = s.written - take; i < s.written; ++i) out.push_back(s.ring[i % kRing]);
    return out;
}

// A layer's run state as its status row is written (nr_pe_control.cpp, the add-on): 1 ran, 2 its network is being
// built, 3 skipped (`why`). A change is an event; a stop is kept as the last one. "not run on this presented frame"
// (a Present without a game frame, frame generation's) is not a change of state.
inline void layer_state(int layer, uint32_t state, const char* why) {
    if (layer < 0 || layer > 8) return;
    if (state == 3u && why && std::strcmp(why, "not run on this presented frame") == 0) return;
    Store& s = store();
    const uint64_t now = GetTickCount64();
    char text[176] = {};
    {
        std::lock_guard<std::mutex> g(s.m);
        Store::Layer& l = s.layers[layer];
        const bool ran = state == 1u, was = l.state == 1u;
        if (l.state == state && (ran || std::strncmp(l.why, why ? why : "", sizeof l.why - 1) == 0)) return;
        const bool first = l.state == ~0u;
        l.state = state;
        std::snprintf(l.why, sizeof l.why, "%s", why ? why : "");
        if (first && ran) return;
        l.changes[l.next++ % 32] = now;
        const char* what = state == 2u ? "waits for its network's build" : why && why[0] ? why : "not run";
        if (ran) std::snprintf(text, sizeof text, "Layer %d runs%s", layer, was ? "" : " again");
        else std::snprintf(text, sizeof text, "Layer %d stopped: %s", layer, what);
        if (!ran) { s.stop_ms = now; s.stop_layer = layer; std::snprintf(s.stop_why, sizeof s.stop_why, "%s", what); }
    }
    record(kRun, text);
}

struct LastStop { uint64_t ms = 0; int layer = 0; std::string why; };
inline LastStop last_stop() {
    Store& s = store();
    std::lock_guard<std::mutex> g(s.m);
    return {s.stop_ms, s.stop_layer, s.stop_why};
}

// Layers that changed between running and not more than `threshold` times in the last second, with the reasons seen.
struct Flapping { bool on = false; std::string what; };
inline Flapping flapping(uint32_t threshold = 6) {
    Store& s = store();
    const uint64_t now = GetTickCount64();
    Flapping f;
    std::lock_guard<std::mutex> g(s.m);
    for (int layer = 0; layer <= 8; ++layer) {
        const Store::Layer& l = s.layers[layer];
        uint32_t n = 0;
        for (uint64_t t : l.changes) n += t && now - t < 1000;
        if (n < threshold) continue;
        f.on = true;
        if (!f.what.empty()) f.what += "; ";
        f.what += "Layer " + std::to_string(layer) + ": " + std::to_string(n) + " changes in the last second";
    }
    if (f.on) {   // the reasons in that second (the stops' texts)
        std::string reasons;
        const uint64_t have = std::min<uint64_t>(s.written, kRing);
        for (uint64_t i = s.written - have; i < s.written; ++i) {
            const Event& e = s.ring[i % kRing];
            if (e.category != kRun || now - e.ms >= 1000) continue;
            const char* colon = std::strstr(e.text, "stopped: ");
            if (!colon) continue;
            const std::string why = colon + 9;
            if (reasons.find(why) == std::string::npos) reasons += (reasons.empty() ? "" : ", ") + why;
        }
        if (!reasons.empty()) f.what += " (" + reasons + ")";
    }
    return f;
}

// The video memory's residency as the session last decided it (nr_pe_session.cpp residency()).
struct Residency {
    bool known = false;
    int mode = 0;                       // [Network] Residency: 0 auto, 1 resident, 2 offload, 3 minimal
    uint64_t budget = 0, usage = 0, reserve = 0, heap = 0;   // bytes (the device-local heap, VK_EXT_memory_budget)
    std::string status;                 // what it does now ("" nothing)
    std::string reason;                 // why
    // a row per network: 0 not loaded, 1 serving, 2 kept in video memory (parked), 3 moving to RAM, 4 in RAM
    struct Net { int net = -1; int state = 0; uint32_t width = 0, height = 0; };
    std::vector<Net> nets;
    uint64_t need = 0;                  // about what a network at the serving extent needs (fits_in_memory's estimate)
    uint32_t need_w = 0, need_h = 0;
    uint64_t ms = 0;
};
struct ResidencyStore { std::mutex m; Residency r; };
inline ResidencyStore& residency_store() { static ResidencyStore s; return s; }
inline void publish(Residency r) {
    r.known = true;
    r.ms = GetTickCount64();
    std::lock_guard<std::mutex> g(residency_store().m);
    residency_store().r = std::move(r);
}
inline Residency residency() {
    std::lock_guard<std::mutex> g(residency_store().m);
    return residency_store().r;
}

}  // namespace nr::pe::events
