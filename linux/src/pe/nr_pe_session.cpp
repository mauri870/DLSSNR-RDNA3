#include "nr_pe_session.hpp"
#include "nr_pe_events.hpp"
#include "nr_gpu_completion.hpp"
#include "nr_wilsjo2.hpp"
#include "nr_wilsjo2_flow.hpp"
#include "nr_vram_tag.hpp"
#include "nr_vram_ledger.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_config.hpp"   // NetworkSwitch
#include "nr_pe_log.hpp"
#include "nr_log.hpp"
#include "nr_screenshot.hpp"
#include "nr_test_fault.hpp"
#include <windows.h>
#include "nr_pe_watchdog.hpp"
#include "nr_pe_vkdevice.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace nr::pe {
namespace {

using nr::pe::log;

// `f` at the end of the scope unless disarmed: a command buffer of ours recorded and then not submitted (any early
// return, or a failed submit) has its timing discarded (Session::Impl::discard_timing).
template <class F> struct OnExit {
    F f;
    bool armed = true;
    ~OnExit() { if (armed) f(); }
};
template <class F> OnExit<F> on_exit(F f) { return OnExit<F>{std::move(f)}; }

// What the process has left of its own address space.
//
// This exists because of one log line: `neural rendering unavailable:
// std::bad_alloc`, from a 32-bit game at 4K with 11979 MB of VRAM free, 482 ms
// into a build - and nothing else. A `std::bad_alloc` is `operator new`
// refusing, which in a 32-bit process almost always means the 2 GB of virtual
// address space is gone, not that the machine is out of memory; a Vulkan
// failure would have come back through NRVK_CHECK with a VkResult instead. The
// two numbers that tell those apart are the total free and the **largest
// contiguous free block** - a 300 MB request fails against 900 MB of free space
// scattered in 60 MB pieces, and only the second number shows it.
//
// A 64-bit process has terabytes and this reads as such; the walk costs one
// VirtualQuery per region either way, and it is only ever called on a failure.
std::string address_space_report() {
    MEMORY_BASIC_INFORMATION region{};
    unsigned long long free_total = 0, largest = 0;
    auto* address = static_cast<unsigned char*>(nullptr);
    while (VirtualQuery(address, &region, sizeof region) == sizeof region) {
        if (region.State == MEM_FREE) {
            free_total += region.RegionSize;
            if (region.RegionSize > largest) largest = region.RegionSize;
        }
        auto* next = static_cast<unsigned char*>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;   // no progress, or the pointer wrapped
        address = next;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    GlobalMemoryStatusEx(&memory);
    char text[256];
    std::snprintf(text, sizeof text,
                  "address space: %.0f MB free in this process, largest contiguous block %.0f MB "
                  "(of %.0f MB total); %.0f MB of RAM free",
                  double(free_total) / (1 << 20), double(largest) / (1 << 20),
                  double(memory.ullTotalVirtual) / (1 << 20),
                  double(memory.ullAvailPhys) / (1 << 20));
    return text;
}

}  // namespace

// The network a runtime config builds (RuntimeConfig::network / int4_mixed), as nr::NetworkChoice.
static int config_net(const RuntimeConfig& c) {
#if NR_INT4
    if (c.network != 1 && c.int4_mixed == 1) return nr::kSilverAardwoldInt4;
#endif
    return c.network == 2 ? nr::kSmallFp8Experimental : c.network == 1 ? nr::kSmallFp8 : nr::kSilverAardwold;
}

struct Session::Impl {
    std::string root;
    std::string status;
    // ---- one built network per extent ----------------------------------------
    //
    // There used to be exactly one, rebuilt whenever anything about it changed.
    // OptiScaler keeps neural-rendering features alive at *two* extents at once
    // - pre-upscale at the render resolution and post-upscale at the display
    // one - and creates and destroys them on every toggle, so a single slot
    // thrashed: 1920x1080, then 3840x2160, then 1920x1080 again, a full rebuild
    // each time, with frames passing through unenhanced in between. That is the
    // stall the user reported and it is also why "passes" appeared to do
    // nothing: the network was never finished long enough to run.
    //
    // So built networks are kept, keyed by everything that makes one different,
    // and a switch between two of them is a pointer assignment. They are
    // evicted least-recently-used, and only when the card cannot hold the next
    // one (fits_in_memory) - the arena is 1.05 GB at 1080p and 4.07 GB at 4K, so
    // this is not a cache that can be allowed to grow.
    struct Built {
        std::unique_ptr<Runtime> runtime;
        uint32_t width{}, height{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        // The NR-chain layers (set_call_layer) that selected it, and when: a network another layer ran lately is that
        // layer's (the ReShade route's layers each have their own).
        std::map<int, std::chrono::steady_clock::time_point> layers;
        int net{};     // the network it runs (nr::NetworkChoice)
        bool reuse{};  // RuntimeConfig::reuse
        uint64_t used{};   // LRU stamp
        std::chrono::steady_clock::time_point last{};   // when it was last selected
        bool light{};      // built inside a recording (inframe): its images in its network memory's heap
    };
    // A cached network taken up again after serving nothing for this long starts its features fresh (as after a
    // Reset), so no frame is blended with a history that old; RenoDX's own look history uses the same bound.
    static constexpr std::chrono::milliseconds kStaleHistory{250};
    std::vector<Built> cache;
    uint64_t use_stamp{};
    // The NR-chain layers (set_call_layer) seen lately, and when. With two or more running, a network a layer ran within
    // kNetIdle is that layer's: neither the drops of a finished build nor the per-kind limit take it (a slow frame
    // during a build - 32-bit games - is longer than kIdleDrop); short memory is still the residency's.
    std::map<int, std::chrono::steady_clock::time_point> layer_seen;
    bool chain_active(std::chrono::steady_clock::time_point now) const {
        int n = 0;
        for (const auto& [layer, at] : layer_seen) n += now - at < kNetIdle;
        return n >= 2;
    }
    bool layer_held(const Built& b, std::chrono::steady_clock::time_point now) const {
        if (!chain_active(now)) return false;
        for (const auto& [layer, at] : b.layers) if (now - at < kNetIdle) return true;
        return false;
    }
    // Networks of one [Int4Mixed] kind kept at once (see the adoption in ensure_runtime_), and runtimes: those built
    // from another's network (Runtime::share_for, an extent with the same working extent) hold their own images only.
    static constexpr size_t kMaxResident = 2;
    static constexpr size_t kMaxEntries = 4;
    // 0.5 s: OptiScaler's two extents (NR before and after the upscaler) are both used every frame; a game's dynamic
    // resolution leaves sizes behind several times a second (2 s kept 13 networks, 7.2 GB).
    static constexpr std::chrono::milliseconds kIdleDrop{500};
    static constexpr std::chrono::seconds kNetIdle{5};
    // A whole build (another working extent) starts only for an extent asked for kSettle times, at most kSettleGap
    // evaluations apart: a size that moves on every frame (a game's dynamic resolution) asked for one build per frame,
    // never ready before the size had moved again (608 builds in 200 s, a CPU core busy). Counted in evaluations,
    // not time: a game at 5 fps is as entitled to its network as one at 200.
    // ---- the resize path inside the frame --------------------------------------------------------------------
    // Plans (Runtime::plan): one per working extent and configuration, lowered at the tallest frame height of that
    // working extent so it runs every height of it. With one in the cache and a resident network whose memory holds
    // it, a frame at a new size gets its runtime inside the recording (RuntimeConfig::deferred, ~1 ms) and runs the
    // same frame, as NVIDIA's DLL evaluates one feature at any size. A plan is built in the background (~0.15 s) the
    // first time its working extent is asked for.
    struct PlanEntry {
        VkFormat format{}; int net{};
        uint32_t ww{}, wh{}, sh{};   // working extent, tallest frame height
        std::shared_ptr<const RuntimeCore> core;
        uint64_t used{};
    };
    std::vector<PlanEntry> plans;
    static constexpr size_t kMaxPlans = 48;
    bool build_plan{};   // the background build is a plan (RuntimeConfig::plan_only)
    uint64_t inframe_built{}, plans_built{};
    double inframe_ms_max{};
    bool same_config_but_format(const Built& e) const { return same_config(e, e.format); }
    bool same_config(const Built& e, VkFormat f) const { return e.format == f && e.net == net_want && e.reuse == reuse_want; }
    // The network a runtime runs (nr::NetworkChoice).
    static int net_of(const Runtime& r) {
#if NR_INT4
        if (r.int4_mixed()) return nr::kSilverAardwoldInt4;
#endif
        if (r.experimental()) return nr::kSmallFp8Experimental;
        return r.network() == NetworkModel::Small ? nr::kSmallFp8 : nr::kSilverAardwold;
    }
    RuntimeConfig make_config(uint32_t w, uint32_t h, VkFormat f) const;
    HostDevice make_host() const;
    TemporalConfig make_temporal() const {
        TemporalConfig temporal; temporal.enable = true;
        temporal.history_strength = history_strength;
        return temporal;
    }
    const PlanEntry* plan_for(uint32_t w, uint32_t h, VkFormat f, NetworkModel model);
    // `tried`: the network it would have run on, when building inside the frame failed (a pipeline not made yet):
    // the background build then makes that runtime.
    bool inframe(uint32_t w, uint32_t h, VkFormat f, std::shared_ptr<const RuntimeCore>* tried);
    bool start_plan(uint32_t w, uint32_t h, VkFormat f);
    // While a game's dynamic resolution moves the frame size (a resize in the last kDrsActive): the plans of the
    // working extents nearest the latest size, one at a time in the background, so a sweep finds them made.
    static constexpr std::chrono::seconds kDrsActive{2};
    std::chrono::steady_clock::time_point last_resize{};
    uint32_t last_w{}, last_h{};
    VkFormat last_format{VK_FORMAT_UNDEFINED};
    std::pair<uint32_t, uint32_t> prefetched_around{};   // the size whose neighbourhood is all planned
    bool drs_active() const {
        return last_resize != std::chrono::steady_clock::time_point{} &&
               std::chrono::steady_clock::now() - last_resize < kDrsActive;
    }
    void prefetch();
    // A whole network while the size moves gets this much room by area (1.25 linear): the plans of a sweep up to that
    // run in it instead of each asking for a bigger network.
    static constexpr double kDrsCapacity = 1.5625;
    // The game's guides' allocation (pixels), when the frame has them: a game with dynamic resolution allocates them
    // once at its biggest render size and renders into a corner. A whole network built while the size moves gets
    // room for that (else kDrsCapacity), so one network and one image heap cover the whole range.
    uint64_t size_hint_px{};
    // [Network] Resize History (dlssnr-amd.ini): 0 (the default) a feature at a new size starts its history afresh, as
    // NVIDIA's DLL; 1 its history is carried over, scaled to the new size (Runtime::carry_history). Re-read at most once
    // a second, on a resize.
    bool resize_history{};
    std::chrono::steady_clock::time_point resize_history_read{};
    // The runtime each feature ran on last, and the one before it (the size before a resize).
    std::map<uint64_t, Runtime*> feature_runtime, feature_prev_runtime;
    uint64_t carried{};
    // The last RenoDX-style frame's decisions (renodx::Stage::facts), for the menus' engine-data panel.
    renodx::Stage::Facts renodx_facts{};
    bool renodx_facts_known{};
    bool carry(VkCommandBuffer cmd, uint64_t feature);
    // The feature being evaluated, and the network memory each feature last ran in: a new frame size of a feature
    // takes its memory again when it fits (OptiScaler's NR before and after the upscaler keep one each, and one
    // stream does not move between memories).
    uint64_t current_feature{};
    std::map<uint64_t, const void*> feature_slot;
    // The image heap being made in the background for a network memory (Runtime::image_heap_wanted).
    std::thread heap_thread;
    std::mutex heap_lock;
    bool heap_busy{}, heap_done{};
    const void* heap_for{};
    std::shared_ptr<ImageHeapRef> heap_made;
    // The network memory a frame waits for a heap in (inframe, past kOwnImages): asked for by heap_poll whether or not
    // the frame size moves (a placement or colour format of its own needs a runtime there too, not only DRS).
    const void* heap_ask{};
    void heap_poll();
    // Runtimes built inside frames with images of their own (no heap yet) kept at most this many.
    static constexpr size_t kOwnImages = 2;
    bool inframe_waiting{};
    // Since when frames wait for that heap: past kHeapWait the background build serves them instead (frames never
    // pass through for good waiting for a heap that cannot come).
    std::chrono::steady_clock::time_point inframe_wait_since{};
    static constexpr std::chrono::seconds kHeapWait{2};
    static constexpr uint32_t kSettle = 3;
    static constexpr uint64_t kSettleGap = 8;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint64_t>> asked;   // extent -> count, last call
    bool settled(uint32_t w, uint32_t h) {
        for (auto it = asked.begin(); it != asked.end();)
            it = calls - it->second.second > kSettleGap ? asked.erase(it) : std::next(it);
        auto& e = asked[{w, h}];
        e.first += 1; e.second = calls;
        return e.first >= kSettle;
    }
    void trim_cache(const Built* adopting = nullptr);
    // Evicted networks wait here before they are destroyed. run_after and run_present record the network into
    // the game's own command list (vkd3d-proton's command buffer), which the game submits and the GPU runs frames
    // later: a network that served the previous frame can still be read by the GPU when its replacement is
    // adopted (the [Int4Mixed] switch, a resolution change). Destroying it then frees memory under in-flight work
    // and hangs the GPU. Freed after kRetireCalls more evaluations and kRetireTime, far past any frame in flight.
    struct Retired { std::unique_ptr<Runtime> runtime; uint64_t call; std::chrono::steady_clock::time_point at; };
    std::vector<Retired> retired;
    uint64_t calls{};   // ensure_runtime calls (one per evaluation)
    // Evaluations that ran the network and that passed the frame through (building, freeing, failed): in the log
    // every kPassLog while NR is asked for, and for the whole session at its end.
    uint64_t ran{}, passed{}, ran_total{}, passed_total{};
    bool totals_logged{};   // Session::log_totals
    std::chrono::steady_clock::time_point pass_logged{};
    static constexpr std::chrono::seconds kPassLog{10};
    static constexpr uint64_t kRetireCalls = 16;
    static constexpr std::chrono::seconds kRetireTime{1};
    void free_retired();
    // ---- [Network] Residency: the networks not running ------------------------------------------------------------
    // A network of another kind than the running one (and, with no evaluation for kNrOff, the running one too: NR off)
    // is inactive. resident keeps them; offload moves their weights to RAM and minimal frees them; auto keeps them
    // while the device-local heap has room:
    // free = min(budget, heap) - usage under max(1 GiB, 10 % of the heap) frees the least recently used one (one a
    // check; the budget lags a free, so kFreedQuiet after one); at a switch the left network stays only with 1.5x that
    // free; above 70 % of the budget an inactive one stays at most 60 s, down to 1 s at 100 %. The running network is
    // never freed while NR runs.
    static constexpr std::chrono::seconds kNrOff{10};
    static constexpr std::chrono::milliseconds kResidencyCheck{500};
    static constexpr std::chrono::seconds kFreedQuiet{1};
    std::chrono::steady_clock::time_point last_call{}, residency_checked{}, residency_freed{};
    std::string residency_why;   // residency()'s last reason to act (NR Control)
    void publish_residency(int mode, bool nr_off);
    struct Vram { uint64_t heap{}, budget{}, usage{}; };
    bool vram(Vram& v) const;   // the largest device-local heap
    void residency(bool switched, bool nr_off);
    // The activation arena a whole build of network `net` may take (Runtime::act_arena): one of a network of another
    // kind - the feature's last one first, else the biggest - never one a network of `net` runs in (two networks of
    // one kind can run in one frame: OptiScaler's NR before and after the upscaler). A new arena gets kActSlack room
    // so the other kinds fit it (CC_SMALL_FP8 needs 4.6 % more than CC_SILVER_AARDWOLD at 1080p).
    static constexpr double kActSlack = 1.06;
    std::shared_ptr<ActArenaRef> act_for(int net) const;
    // A network kept as its memory alone (auto): its weights, its share of an activation arena and its pipelines, held
    // by a core (Runtime::share_for); its runtimes and images are freed. Asked for again, a runtime is built from it in
    // the background (RuntimeConfig::share, ~10 ms) instead of a whole build.
    struct Parked {
        std::shared_ptr<const RuntimeCore> core;
        VkFormat format{}; int net{};
        uint32_t width{}, height{};
        std::chrono::steady_clock::time_point last{};
    };
    std::vector<Parked> parked;
    // Park entry `index`'s network (once per network) and drop every runtime of it.
    void park(size_t index, const char* why);
    // A parked network's weights moved to RAM (offload_network) on a thread of its own, one at a time, once no runtime
    // of it is left (retired ones included: their frames may be in flight). Polled by residency().
    std::thread offload_thread;
    std::atomic<bool> offload_done{false};
    bool offload_ok{};
    std::shared_ptr<const RuntimeCore> offloading;
    std::vector<uint8_t> offload_weights;   // the read-back, released into the network by poll_offload
    // The parked network a background build makes a runtime from (a failed build drops it: its memory may be half back).
    std::shared_ptr<const RuntimeCore> build_share;
    bool start_offload(Parked& p);
    void poll_offload();
    // RAM below max(2 GiB, 10 %) available: offloaded networks are freed, the oldest first.
    static bool ram_short();
    // The RenoDX-style option: one stage per feature, made at the frame's extent, format and HDR judgement.
    // A replaced or released one is kept for kRetireCalls more evaluations and kRetireTime, as networks are.
    struct RenoDXEntry {
        std::unique_ptr<renodx::Stage> stage;
        uint32_t w{}, h{}, nw{}, nh{}, present{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        bool hdr{};
        // RenoDX's CommitNrResolution: the network's extent in use, and one the game's guides moved to waiting
        // out kContractDebounceFrames (a settings change is taken at once).
        uint32_t applied_w{}, applied_h{}, pending_w{}, pending_h{}, pending_frames{};
        float scale{-1.0f};
        bool follow{};
        uint32_t stack{1};   // the model passes its stack last ran: pass k > 0 keeps a history of its own
    };
    std::map<uint64_t, RenoDXEntry> renodx;
    // ---- wilsjo2 layers of the ReShade route: nr_wilsjo2_flow.hpp, the flow the .asi runs, around the network (which
    // runs network-only, as for RenoDX). Per feature: the flow's state; the surfaces ours, retired once the GPU is done
    // with them (or, without a proof, after 64 evaluations and 2 s).
    struct W2Surface { VkImage image{}; VkDeviceMemory memory{}; VkFormat format{VK_FORMAT_UNDEFINED}; uint32_t w{}, h{}; };
    struct W2Entry { nr::wilsjo2::FlowState flow; W2Surface* work{}; uint32_t passes{}; };
    std::map<uint64_t, W2Entry> w2;
    std::unique_ptr<nr::wilsjo2::Stage> w2_stage;
    // the wilsjo2 Stage could not be made: when (steady ms; 0 never), tried again 5 s later; said once
    uint64_t w2_stage_failed_ms{};
    bool w2_stage_said{};
    float w2_fast_passes = 0.f;                 // the last wilsjo2 run: Passes (fast), Session::state()
    bool w2_fast_fallback = false, w2_motion = false;
    nr::wilsjo2::SpatialLayout w2_spatial{};    // the last wilsjo2 run's peripheral compression (Session::spatial_layout)
    bool w2_spatial_on = false;
    struct W2Retired { W2Surface* s; nr::gpu::Uses uses; uint64_t call; std::chrono::steady_clock::time_point at; };
    std::vector<W2Retired> w2_retired;
    nr::gpu::Uses w2_uses;
    VkPhysicalDeviceMemoryProperties w2_mem{};
    bool w2_mem_known{};
    void w2_retire(W2Surface*& x) {
        if (x) w2_retired.push_back({x, w2_uses, calls, std::chrono::steady_clock::now()});
        x = nullptr;
    }
    void w2_destroy(W2Surface* x) {
        if (!x) return;
        if (x->image) vkDestroyImage(handles.device, x->image, nullptr);
        if (x->memory) vkFreeMemory(handles.device, x->memory, nullptr);
        delete x;
    }
    void w2_free_retired() {
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = w2_retired.size(); i-- > 0;)
            if (calls - w2_retired[i].call > 64 && now - w2_retired[i].at > std::chrono::seconds(2) && w2_retired[i].uses.ready()) {
                w2_destroy(w2_retired[i].s);
                w2_retired.erase(w2_retired.begin() + std::ptrdiff_t(i));
            }
    }
    W2Surface* w2_make(VkCommandBuffer cmd, VkFormat f, uint32_t w, uint32_t h);
    void wilsjo2_release(uint64_t feature);
    bool wilsjo2_runtime(const Controls& controls, uint32_t w, uint32_t h, VkFormat f, Controls* c);
    bool wilsjo2_prepare(const Session::Wilsjo2Frame& wf, uint64_t feature, uint32_t w, uint32_t h, VkFormat f, bool guides,
                         const Controls& controls, Controls* net);
    bool record_wilsjo2(VkCommandBuffer cmd, const ColourFrame& colour, const EngineFrame& engine, const Controls& net,
                        const Session::Wilsjo2Frame& wf);
    // The history pass k of a feature's stack runs on (pass 0: the feature's own).
    static uint64_t stack_feature(uint64_t feature, uint32_t k) {
        return k == 0 ? feature : (uint64_t(1) << 62) | (feature << 8) | k;
    }
    // Pass 1's name on a route whose frames carry none (feature 0), when it stacks; no game feature is numbered so.
    static constexpr uint64_t kUnnamedStack = 0xFFFFFFu;
    void release_everywhere(uint64_t feature) {
        for (auto& e : cache) e.runtime->release_feature(feature);
    }
    // RenoDX's neutral guides (NRPresentGuides): zero motion vectors at the frame's extent, for a Present-path frame
    // without the game's (Optional) or always (Never). Made and cleared on first use, kept.
    struct NeutralRetired { VkImage image; VkDeviceMemory memory; nr::gpu::Uses uses; uint64_t call; std::chrono::steady_clock::time_point at; };
    std::vector<NeutralRetired> neutral_retired;
    nr::gpu::Uses neutral_uses;
    uint64_t neutral_calls = 0;
    VkImage neutral_mv{};
    VkDeviceMemory neutral_mv_memory{};
    uint32_t neutral_w{}, neutral_h{};
    bool neutral_cleared{};
    bool neutral_motion(VkCommandBuffer cmd, uint32_t w, uint32_t h, ColourFrame* out);
    // Whether a RenoDX frame of this route runs without the game's motion vectors (the Present path's Optional and
    // Never; Required, and every other route, need them).
    static bool renodx_needs_motion(const Session::RenoDXFrame& rf) {
        return !rf.present || rf.settings.present_guides == 1u;
    }
    // The RenoDX-style option's cost for NR Control and the log: a timestamp pair around the whole stage and one
    // around each pass's network, in the same frame, a ring of frames read back without waiting, averaged as the
    // runtime averages its own (the runtime's own timing covers one pass at a time).
    static constexpr uint32_t kStageSlots = 8, kStageQueries = 2 + 2 * renodx::kMaxUnlockedPasses;
    VkQueryPool stage_timing{};
    bool stage_timing_tried{};
    double stage_timestamp_ns{};
    uint32_t stage_slot{};
    uint32_t stage_slot_passes[kStageSlots]{};
    uint64_t stage_recorded{};
    // The averages by NR-chain layer (Session::set_call_layer; 0 outside a chain): every layer's runs apart, and whether
    // its last run was timed here (else the runtime's own timing is its cost)
    struct StageAverage {
        float ms{}, net_ms{};
        bool renodx_timed{};
        std::chrono::steady_clock::time_point seen{};   // its last reading
    };
    std::map<int, StageAverage> stage_avg;
    int stage_slot_layer[kStageSlots]{};
    // reset_layer_timing: each layer's measurement generation, and the one each slot was recorded in
    std::map<int, uint32_t> layer_gen;
    uint32_t stage_slot_gen[kStageSlots]{};
    int call_layer{};
    // Session::outer_begin / outer_end: their own ring, in the stage's pool after the stage's queries
    static constexpr uint32_t kOuterSlots = 16;
    static constexpr uint32_t kOuterPasses = 32;   // network recordings timed inside one run (wilsjo2: up to 30 passes)
    static constexpr uint32_t kOuterQueries = 2 + 2 * kOuterPasses;
    VkQueryPool outer_timing{};
    bool outer_tried{}, outer_open{};
    uint32_t outer_slot{}, outer_passes{};
    uint64_t outer_recorded{};
    int outer_slot_layer[kOuterSlots]{};
    uint32_t outer_slot_gen[kOuterSlots]{};
    uint32_t outer_slot_passes[kOuterSlots]{};
    std::map<int, float> outer_avg, outer_net_avg;
    // When each layer's outer run was last recorded: an average is the layer's only while its runs go on (a layer that
    // was wilsjo2 and is RenoDX now, or removed and added again, must not show the old run's time for good)
    std::map<int, std::chrono::steady_clock::time_point> outer_seen;
    bool outer_fresh(int layer) const {
        const auto s = outer_seen.find(layer);
        return s != outer_seen.end() && std::chrono::steady_clock::now() - s->second < std::chrono::seconds(1);
    }
    // A network recording inside an open outer run: its start (begin) or end timestamp
    void outer_net(VkCommandBuffer cmd, bool begin) {
        if (!outer_open || outer_passes >= kOuterPasses) return;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, outer_timing,
                            outer_slot * kOuterQueries + 2 + 2 * outer_passes + (begin ? 0u : 1u));
        if (!begin) ++outer_passes;
    }
    bool outer_ready() {
        if (outer_tried) return outer_timing != VK_NULL_HANDLE;
        outer_tried = true;
        if (!stage_timing_ready()) return false;   // its timestamp period, and a queue family with timestamps
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kOuterSlots * kOuterQueries;
        if (vkCreateQueryPool(handles.device, &qi, nullptr, &outer_timing) != VK_SUCCESS) outer_timing = VK_NULL_HANDLE;
        return outer_timing != VK_NULL_HANDLE;
    }
    void discard_timing() {   // (Session::discard_timing)
        outer_recorded = 0;
        stage_recorded = 0;
        for (Built& b : cache) if (b.runtime) b.runtime->discard_timing();
        for (Retired& r : retired) if (r.runtime) r.runtime->discard_timing();
        if (built) built->discard_timing();
    }
    void read_outer_timing() {
        if (outer_recorded <= kOuterSlots) return;
        const uint32_t n = 2 + 2 * outer_slot_passes[outer_slot];
        uint64_t stamps[kOuterQueries] = {};
        if (vkGetQueryPoolResults(handles.device, outer_timing, outer_slot * kOuterQueries, n, n * sizeof(uint64_t), stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || stamps[1] <= stamps[0])
            return;
        if (outer_slot_gen[outer_slot] != layer_gen[outer_slot_layer[outer_slot]]) return;   // of the layer's old setup
        const float ms = float(double(stamps[1] - stamps[0]) * stage_timestamp_ns / 1e6);
        float& a = outer_avg[outer_slot_layer[outer_slot]];
        a = a > 0.0f ? a + (ms - a) * 0.1f : ms;
        if (!outer_slot_passes[outer_slot]) return;
        uint64_t net = 0;
        for (uint32_t p = 0; p < outer_slot_passes[outer_slot]; ++p)
            if (stamps[3 + 2 * p] > stamps[2 + 2 * p]) net += stamps[3 + 2 * p] - stamps[2 + 2 * p];
        const float net_ms = float(double(net) * stage_timestamp_ns / 1e6);
        if (std::getenv("NR_LAYER_TIMING_DIAG"))
            for (uint32_t p = 0; p < outer_slot_passes[outer_slot]; ++p)
                log("[nr] timing layer %d pass %u total %llu %llu network %llu %llu ns %.6f",
                    outer_slot_layer[outer_slot], p, (unsigned long long)stamps[0], (unsigned long long)stamps[1],
                    (unsigned long long)stamps[2 + 2 * p], (unsigned long long)stamps[3 + 2 * p], stage_timestamp_ns);
        float& b = outer_net_avg[outer_slot_layer[outer_slot]];
        b = b > 0.0f ? b + (net_ms - b) * 0.1f : net_ms;
    }
    void read_stage_timing() {
        if (stage_recorded <= kStageSlots) return;
        const uint32_t n = 2 + 2 * stage_slot_passes[stage_slot];
        uint64_t stamps[kStageQueries] = {};
        if (vkGetQueryPoolResults(handles.device, stage_timing, stage_slot * kStageQueries, n, n * sizeof(uint64_t), stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || stamps[1] <= stamps[0])
            return;   // still running: next frame
        if (stage_slot_gen[stage_slot] != layer_gen[stage_slot_layer[stage_slot]]) return;   // of the layer's old setup
        uint64_t net = 0;
        for (uint32_t p = 0; p < stage_slot_passes[stage_slot]; ++p)
            if (stamps[3 + 2 * p] > stamps[2 + 2 * p]) net += stamps[3 + 2 * p] - stamps[2 + 2 * p];
        const float ms = float(double(stamps[1] - stamps[0]) * stage_timestamp_ns / 1e6);
        const float net_ms = float(double(net) * stage_timestamp_ns / 1e6);
        if (std::getenv("NR_LAYER_TIMING_DIAG"))
            for (uint32_t p = 0; p < stage_slot_passes[stage_slot]; ++p)
                log("[nr] timing layer %d pass %u total %llu %llu network %llu %llu ns %.6f",
                    stage_slot_layer[stage_slot], p, (unsigned long long)stamps[0], (unsigned long long)stamps[1],
                    (unsigned long long)stamps[2 + 2 * p], (unsigned long long)stamps[3 + 2 * p], stage_timestamp_ns);
        StageAverage& a = stage_avg[stage_slot_layer[stage_slot]];
        // a layer timed again after a pause (it ran as another processing, or was removed and added): anew
        const auto now = std::chrono::steady_clock::now();
        const bool first = a.ms <= 0.0f || now - a.seen > std::chrono::seconds(1);
        a.seen = now;
        a.ms = first ? ms : a.ms + (ms - a.ms) * 0.1f;
        a.net_ms = first ? net_ms : a.net_ms + (net_ms - a.net_ms) * 0.1f;
    }
    bool stage_timing_ready() {
        if (stage_timing_tried) return stage_timing != VK_NULL_HANDLE;
        stage_timing_tried = true;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handles.physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(handles.physical, &n, families.data());
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(handles.physical, &props);
        if (access.family >= n || !families[access.family].timestampValidBits || props.limits.timestampPeriod <= 0.0f) return false;
        stage_timestamp_ns = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kStageSlots * kStageQueries;
        if (vkCreateQueryPool(handles.device, &qi, nullptr, &stage_timing) != VK_SUCCESS) stage_timing = VK_NULL_HANDLE;
        return stage_timing != VK_NULL_HANDLE;
    }
    // RenoDX's NR screenshot (nr_screenshot.hpp): one capture's two readbacks in one host-visible buffer (the image
    // before NR at 0, after it at `bytes`), filled by copies recorded in the frame's own command buffer; an event set
    // after them is the proof the GPU ran them (an event starts unset; a new query pool's queries are undefined until
    // the command buffer resetting them has run, and RADV reads them as available). Handed to the worker once it has,
    // freed once the worker has read it; a capture never proven run is kept until the session goes (its command buffer
    // may still run).
    struct Shot {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        uint64_t bytes{};
        VkEvent done{};
        screenshot::Pair pair;
        uint32_t frames{};
        bool handed{};
        std::atomic_bool read{false};
    };
    static constexpr uint32_t kShotSettleFrames = 240;   // RenoDX's kExactSettlePresents
    std::unique_ptr<Shot> shot;                          // recorded, waiting for the GPU
    std::unique_ptr<Shot> model_shot;
    std::unique_ptr<Shot> outside_shot;                  // capture_begin's, until capture_end
    std::vector<std::unique_ptr<Shot>> shots_out;        // with the worker, or kept
    void shot_free(Shot& sh) {
        if (sh.mapped) vkUnmapMemory(handles.device, sh.memory);
        if (sh.buffer) vkDestroyBuffer(handles.device, sh.buffer, nullptr);
        if (sh.memory) vkFreeMemory(handles.device, sh.memory, nullptr);
        if (sh.done) vkDestroyEvent(handles.device, sh.done, nullptr);
        sh.mapped = nullptr; sh.buffer = VK_NULL_HANDLE; sh.memory = VK_NULL_HANDLE; sh.done = VK_NULL_HANDLE;
    }
    // Readbacks for a w x h pair of `format`, stamped (nr_screenshot.hpp); null when anything fails.
    std::unique_ptr<Shot> shot_prepare(uint32_t w, uint32_t h, VkFormat format, VkFormat pre = VK_FORMAT_UNDEFINED) {
        auto sh = std::make_unique<Shot>();
        sh->bytes = uint64_t(w) * h * std::max(screenshot::bytes_per_pixel(format), screenshot::bytes_per_pixel(pre));
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sh->bytes * 2; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkMemoryRequirements req{};
        VkPhysicalDeviceMemoryProperties mem{};
        uint32_t type = ~0u;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (!sh->bytes || vkCreateBuffer(handles.device, &bi, nullptr, &sh->buffer) != VK_SUCCESS) goto fail;
        vkGetBufferMemoryRequirements(handles.device, sh->buffer, &req);
        vkGetPhysicalDeviceMemoryProperties(handles.physical, &mem);
        for (int pass = 0; pass < 2 && type == ~0u; ++pass)   // host-cached first: the worker reads it all
            for (uint32_t i = 0; i < mem.memoryTypeCount && type == ~0u; ++i) {
                const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits & (1u << i)) && (f & want) == want &&
                    (pass == 1 || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)))
                    type = i;
            }
        if (type == ~0u) goto fail;
        {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size; ai.memoryTypeIndex = type;
            if (vkAllocateMemory(handles.device, &ai, nullptr, &sh->memory) != VK_SUCCESS ||
                vkBindBufferMemory(handles.device, sh->buffer, sh->memory, 0) != VK_SUCCESS ||
                vkMapMemory(handles.device, sh->memory, 0, VK_WHOLE_SIZE, 0, &sh->mapped) != VK_SUCCESS) {
                sh->mapped = nullptr;
                goto fail;
            }
        }
        if (vkCreateEvent(handles.device, &ei, nullptr, &sh->done) != VK_SUCCESS) goto fail;
        screenshot::stamp(sh->mapped, sh->bytes);
        screenshot::stamp(static_cast<uint8_t*>(sh->mapped) + sh->bytes, sh->bytes);
        sh->pair.width = w; sh->pair.height = h; sh->pair.format = format;
        sh->pair.row_pitch = w * screenshot::bytes_per_pixel(format);
        sh->pair.pre_format = pre; sh->pair.pre_row_pitch = w * screenshot::bytes_per_pixel(pre);
        sh->pair.bytes = sh->bytes;
        sh->pair.pre = sh->mapped;
        sh->pair.post = static_cast<uint8_t*>(sh->mapped) + sh->bytes;
        return sh;
    fail:
        shot_free(*sh);
        return nullptr;
    }
    // `image` (in `layout`, last used by `stage` / `access`) into the readback at `offset`, and back to `layout`.
    static void shot_copy(VkCommandBuffer cmd, Shot& sh, VkImage image, VkImageLayout layout, VkPipelineStageFlags stage,
                          VkAccessFlags access, VkDeviceSize offset) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = access; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {sh.pair.width, sh.pair.height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sh.buffer, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = layout;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, stage | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    // Both copies recorded: the proof after them, and the capture is no longer armed.
    void shot_recorded(VkCommandBuffer cmd, std::unique_ptr<Shot> sh) {
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = sh->buffer; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        vkCmdSetEvent(cmd, sh->done, VK_PIPELINE_STAGE_TRANSFER_BIT);
        screenshot::recorded();
        shot = std::move(sh);
    }
    // Once a frame: a recorded capture the GPU has run goes to the worker; one the worker has read is freed.
    void shot_poll() {
        // Freed: one the worker has read, and an abandoned one (never handed) whose copies the GPU has since run.
        for (auto it = shots_out.begin(); it != shots_out.end();)
            if ((*it)->handed ? (*it)->read.load(std::memory_order_acquire)
                              : vkGetEventStatus(handles.device, (*it)->done) == VK_EVENT_SET) {
                shot_free(**it); it = shots_out.erase(it);
            } else ++it;
        if (!shot) return;
        if (vkGetEventStatus(handles.device, shot->done) == VK_EVENT_SET) {
            Shot* sh = shot.get();
            sh->handed = true;
            sh->pair.owner = sh;
            sh->pair.release = [](void* owner) { static_cast<Shot*>(owner)->read.store(true, std::memory_order_release); };
            screenshot::Pair pair = sh->pair;
            shots_out.push_back(std::move(shot));
            screenshot::deliver(std::move(pair));
        } else if (++shot->frames > kShotSettleFrames) {
            screenshot::abandon("gpu_unconfirmed", "the command buffer carrying the copies did not run within 240 frames "
                                "(buffers kept)");
            shots_out.push_back(std::move(shot));
        }
    }
    Controls shot_controls;
    const Controls& capture_controls(const Controls& source) {
        shot_poll();
        Controls& c = shot_controls;
        c = source;
        c.feature = current_feature;
        if (c.history_strength < 0.f) c.history_strength = history_strength;
        c.network_timestamp = [this](VkCommandBuffer cmd, bool begin) { outer_net(cmd, begin); };
        if (!source.model_capture_allowed || !screenshot::wants_model() || shot || outside_shot) return c;
        c.model_capture = [this](VkCommandBuffer cmd, const ColourFrame& in, const ColourFrame& out, bool after) {
            if (!after) {
                if (model_shot) shots_out.push_back(std::move(model_shot));
                if (!screenshot::wants_model()) return;
                if (!screenshot::readable(in.format) || !screenshot::readable(out.format)) {
                    screenshot::fail("unsupported_present", "the model's format cannot be read back"); return;
                }
                model_shot = shot_prepare(in.width, in.height, out.format, in.format);
                if (!model_shot) { screenshot::abandon("allocation_failed", "model readback allocation failed"); return; }
                model_shot->pair.model_diagnostic = true;
                model_shot->pair.meta = "comparison=model-input-output\nprocessing=wilsjo2\n";
                shot_copy(cmd, *model_shot, in.image, in.before, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0);
                screenshot::recorded();
            } else if (model_shot) {
                shot_copy(cmd, *model_shot, out.image, out.before, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, model_shot->bytes);
                shot_recorded(cmd, std::move(model_shot));
            }
        };
        return c;
    }
    // NR_DIAG_EXPOSURE=1, a diagnostic (off by default): on the RenoDX-style option, texel (0,0) of the game's exposure
    // texture and 64 points (an 8 x 8 grid) of the frame before NR, the network's input and output (pass 1) and the
    // frame after NR, copied into a host-visible slot in the frame's own command buffer, with DLSS.Pre.Exposure,
    // DLSS.Exposure.Scale and the style. Logged once its event is set: every 30th frame, and every frame for 240 frames
    // after the style or the intensity changed. A slot whose commands never ran stays allocated.
    struct DiagSlot {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        VkEvent done{};
        bool pending{};
        uint64_t frame{};
        uint32_t style{};
        float intensity{}, pre{}, scale{};
        bool reset{};
        VkFormat format[5]{};
        bool have[5]{};
    };
    static constexpr uint32_t kDiagSlots = 16, kDiagPoints = 64, kDiagStride = 16;
    std::vector<DiagSlot> diag;
    DiagSlot* diag_slot = nullptr;   // this frame's, while it is being recorded
    uint64_t diag_frame = 0, diag_burst_until = 0;
    uint32_t diag_style = ~0u;
    float diag_intensity = -1.0f;
    static bool diag_on() {
        static const bool on = [] { const char* e = std::getenv("NR_DIAG_EXPOSURE"); return e && e[0] == '1'; }();
        return on;
    }
    static uint32_t diag_bytes(VkFormat f) {
        switch (f) {
            case VK_FORMAT_R16_SFLOAT: return 2;
            case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
            default: return 0;
        }
    }
    // One texel as linear-looking RGB (8-bit and 10-bit formats: their codes over the maximum, not decoded).
    static bool diag_texel(VkFormat f, const uint8_t* p, float rgb[3]) {
        auto half = [](uint16_t h) {
            const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
            const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.f : std::ldexp(float(m | 1024u), int(e) - 25);
            return (h & 0x8000u) ? -v : v;
        };
        auto ufloat = [](uint32_t bits, int mbits) {   // no sign, 5-bit exponent
            const uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1u);
            return e == 0 ? std::ldexp(float(m), -14 - mbits) : std::ldexp(float(m | (1u << mbits)), int(e) - 15 - mbits);
        };
        uint32_t w = 0;
        std::memcpy(&w, p, 4);
        float a[4]{};
        switch (f) {
            case VK_FORMAT_R16_SFLOAT: { uint16_t h; std::memcpy(&h, p, 2); rgb[0] = rgb[1] = rgb[2] = half(h); return true; }
            case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: {
                uint16_t h[4]{};
                std::memcpy(h, p, f == VK_FORMAT_R16G16_SFLOAT ? 4 : 8);
                if (f == VK_FORMAT_R16G16_SFLOAT) { rgb[0] = rgb[1] = rgb[2] = half(h[0]); return true; }
                for (int i = 0; i < 3; ++i) rgb[i] = half(h[i]);
                return true;
            }
            case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: std::memcpy(a, p, 4); rgb[0] = rgb[1] = rgb[2] = a[0]; return true;
            case VK_FORMAT_R32G32B32A32_SFLOAT: std::memcpy(a, p, 12); for (int i = 0; i < 3; ++i) rgb[i] = a[i]; return true;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                rgb[0] = ufloat(w & 2047u, 6); rgb[1] = ufloat((w >> 11) & 2047u, 6); rgb[2] = ufloat(w >> 22, 5);
                return true;
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: {
                const float s = std::ldexp(1.0f, int(w >> 27) - 15 - 9);
                rgb[0] = float(w & 511u) * s; rgb[1] = float((w >> 9) & 511u) * s; rgb[2] = float((w >> 18) & 511u) * s;
                return true;
            }
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                const bool bgr = f == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
                const float v[3] = {float(w & 1023u) / 1023.f, float((w >> 10) & 1023u) / 1023.f, float((w >> 20) & 1023u) / 1023.f};
                for (int i = 0; i < 3; ++i) rgb[i] = v[bgr ? 2 - i : i];
                return true;
            }
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: {
                const bool bgra = f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
                for (int i = 0; i < 3; ++i) rgb[i] = p[bgra ? 2 - i : i] / 255.f;
                return true;
            }
            default: return false;
        }
    }
    bool diag_make(DiagSlot& d) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kDiagStride * (1 + 4 * kDiagPoints); bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(handles.device, &bi, nullptr, &d.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(handles.device, d.buffer, &req);
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(handles.physical, &mem);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = ~0u;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == ~0u; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) type = i;
        if (type == ~0u) return false;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        return vkAllocateMemory(handles.device, &ai, nullptr, &d.memory) == VK_SUCCESS &&
               vkBindBufferMemory(handles.device, d.buffer, d.memory, 0) == VK_SUCCESS &&
               vkMapMemory(handles.device, d.memory, 0, VK_WHOLE_SIZE, 0, &d.mapped) == VK_SUCCESS &&
               vkCreateEvent(handles.device, &ei, nullptr, &d.done) == VK_SUCCESS;
    }
    void diag_free(DiagSlot& d) {
        if (d.mapped) vkUnmapMemory(handles.device, d.memory);
        if (d.buffer) vkDestroyBuffer(handles.device, d.buffer, nullptr);
        if (d.memory) vkFreeMemory(handles.device, d.memory, nullptr);
        if (d.done) vkDestroyEvent(handles.device, d.done, nullptr);
        d = DiagSlot{};
    }
    // The slots the GPU has filled into the log; then this frame's slot, when this frame is one to log.
    void diag_begin(const renodx::Settings& rs, const Session::RenoDXFrame& rf, bool reset) {
        diag_slot = nullptr;
        if (!diag_on()) return;
        for (DiagSlot& d : diag) {
            if (!d.pending || vkGetEventStatus(handles.device, d.done) != VK_EVENT_SET) continue;
            float mean[5]{};
            const uint32_t count[5] = {1, kDiagPoints, kDiagPoints, kDiagPoints, kDiagPoints};
            for (int k = 0; k < 5; ++k) {
                if (!d.have[k]) continue;
                const auto* base = static_cast<const uint8_t*>(d.mapped) + kDiagStride * (k == 0 ? 0 : 1 + (k - 1) * kDiagPoints);
                double sum = 0;
                for (uint32_t n = 0; n < count[k]; ++n) {
                    float c[3]{};
                    if (!diag_texel(d.format[k], base + kDiagStride * n, c)) { d.have[k] = false; break; }
                    sum += k == 0 ? c[0] : 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
                }
                mean[k] = float(sum / count[k]);
            }
            char v[5][24];
            for (int k = 0; k < 5; ++k) {
                if (d.have[k]) std::snprintf(v[k], sizeof v[k], "%.5g", double(mean[k]));
                else std::snprintf(v[k], sizeof v[k], "-");
            }
            log("[nr] diag frame %llu: style %u intensity %.2f%s | pre-exposure %.5g exposure scale %.4g exposure texel %s "
                "(format %d) | mean luminance: frame in %s, network in %s, network out %s, frame out %s (formats %d %d %d %d)",
                static_cast<unsigned long long>(d.frame), d.style, double(d.intensity), d.reset ? " reset" : "",
                double(d.pre), double(d.scale), v[0], int(d.format[0]), v[1], v[2], v[3], v[4], int(d.format[1]),
                int(d.format[2]), int(d.format[3]), int(d.format[4]));
            vkResetEvent(handles.device, d.done);
            d.pending = false;
        }
        ++diag_frame;
        if (rs.style != diag_style || rs.intensity != diag_intensity) {
            if (diag_style != ~0u)
                log("[nr] diag frame %llu: style %u -> %u, intensity %.2f -> %.2f",
                    static_cast<unsigned long long>(diag_frame), diag_style, rs.style, double(diag_intensity),
                    double(rs.intensity));
            diag_style = rs.style;
            diag_intensity = rs.intensity;
            diag_burst_until = diag_frame + 240;
        }
        if (diag_frame % 30 != 0 && diag_frame > diag_burst_until) return;
        if (diag.empty()) diag.resize(kDiagSlots);
        for (DiagSlot& d : diag) {
            if (d.pending) continue;
            if (!d.buffer && !diag_make(d)) { diag_free(d); continue; }
            d.frame = diag_frame; d.style = rs.style; d.intensity = rs.intensity;
            d.pre = rf.pre_exposure; d.scale = rf.exposure_scale; d.reset = reset;
            for (bool& h : d.have) h = false;
            diag_slot = &d;
            return;
        }
    }
    // `image` (in `layout`, last used by `stage` / `access`): texel (0,0) (k 0) or the 8 x 8 grid (k 1..4) into this
    // frame's slot, and back to `layout`.
    void diag_copy(VkCommandBuffer cmd, int k, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                   VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access) {
        if (!diag_slot || !image || !w || !h || !diag_bytes(format) || (usage && !(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)))
            return;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = access; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy regions[kDiagPoints]{};
        const uint32_t n = k == 0 ? 1 : kDiagPoints;
        for (uint32_t i = 0; i < n; ++i) {
            VkBufferImageCopy& r = regions[i];
            r.bufferOffset = kDiagStride * (k == 0 ? 0 : 1 + (k - 1) * kDiagPoints + i);
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            if (k) r.imageOffset = {int32_t((2 * (i % 8) + 1) * w / 16), int32_t((2 * (i / 8) + 1) * h / 16), 0};
            r.imageExtent = {1, 1, 1};
        }
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, diag_slot->buffer, n, regions);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = layout;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, stage | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        diag_slot->format[k] = format;
        diag_slot->have[k] = true;
    }
    void diag_end(VkCommandBuffer cmd) {
        if (!diag_slot) return;
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = diag_slot->buffer; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        vkCmdSetEvent(cmd, diag_slot->done, VK_PIPELINE_STAGE_TRANSFER_BIT);
        diag_slot->pending = true;
        diag_slot = nullptr;
    }
    struct RenoDXRetired { std::unique_ptr<renodx::Stage> stage; uint64_t call; std::chrono::steady_clock::time_point at; };
    std::vector<RenoDXRetired> renodx_retired;
    void retire_stage(std::unique_ptr<renodx::Stage> stage) {
        if (stage) renodx_retired.push_back({std::move(stage), calls, std::chrono::steady_clock::now()});
    }
    bool renodx_runtime(const Controls& controls, uint32_t w, uint32_t h, Controls* c);
    void renodx_extent(uint64_t feature, uint32_t w, uint32_t h, uint32_t guide_w, uint32_t guide_h,
                       const renodx::Settings& settings, uint32_t* nw, uint32_t* nh);
    renodx::Stage* renodx_stage(uint64_t feature, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh, VkFormat format,
                                const Session::RenoDXFrame& rf);
    bool record_renodx(VkCommandBuffer cmd, renodx::Stage& stage, const ColourFrame& colour, const ColourFrame& output,
                       EngineFrame engine, const Controls& c, const Session::RenoDXFrame& rf,
                       const ColourFrame* exposure = nullptr);
    void free_retired_stages() {
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = renodx_retired.size(); i-- > 0;)
            if (calls - renodx_retired[i].call > kRetireCalls && now - renodx_retired[i].at > kRetireTime && renodx_retired[i].stage->recordings_done())
                renodx_retired.erase(renodx_retired.begin() + std::ptrdiff_t(i));
    }
    // The entry in `cache` the run paths are using. Raw, because the cache owns
    // them; null until the first build lands.
    Runtime* runtime{};
    uint32_t width{}, height{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    DeviceHandles handles{};
    // Our own render-resolution image, handed to the upscaler in place of the
    // game's colour. Owned as a D3D12 resource so the upscaler can take it, and
    // used as a VkImage so the network can write it - the same allocation seen
    // from both sides, which is the whole advantage of being inside vkd3d.
    ID3D12Resource* output{};
    ResourceHandle output_vk{};
    bool failed{};   // one hard failure disables the session rather than retrying into a crash
    bool typeless_noted{};
    float history_strength{1.0f};

    // A Vulkan game has no D3D12 allocation to borrow, so the output is ours.
    // Our own command buffer, for the paths that are not handed one.
    VkCommandPool pool{};
    VkCommandBuffer own_cmd{};
    // Building the runtime uploads the weights, which is a submit, not a
    // recording. Every path therefore has to find a queue before it can build
    // anything - the earlier code passed VK_NULL_HANDLE here and the runtime
    // rejects that outright, so no path had ever built a network.
    QueueAccess access{};
    ID3D12CommandQueue* owned_queue{};
    // DXVK's interop device, held for the life of the session: the queue lock
    // closures call into it every frame.
    IDXGIVkInteropDevice* dxvk{};
    // record_raw_d3d11's ring (its own pool on DXVK's queue family)
    static constexpr int kRaw11 = 4;
    VkCommandPool raw11_pool{};
    VkCommandBuffer raw11_cmd[kRaw11]{};
    VkFence raw11_fence[kRaw11]{};
    int raw11_slot = 0;

    VkImage vk_output{};
    VkImageView vk_output_view{};
    VkDeviceMemory vk_output_memory{};
    uint32_t vk_w{}, vk_h{};
    VkFormat vk_format{VK_FORMAT_UNDEFINED};

    // The one thing vkd3d-proton cannot be told any other way: that the compute
    // bind point of its command buffer no longer holds the pipeline it thinks it
    // does. See `forget_bound_pipeline` below for the whole story; these three
    // objects exist only to give it a ClearUAV to run.
    ID3D12Resource* invalidator{};
    ID3D12DescriptorHeap* invalidator_cpu_heap{};
    ID3D12DescriptorHeap* invalidator_gpu_heap{};
    bool invalidator_failed{};
    bool ensure_invalidator(ID3D12Device* device);
    void forget_bound_pipeline(ID3D12Device* device, ID3D12GraphicsCommandList* list);
    void release_invalidator();

    bool ensure_output(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT dxgi);
    void release_output();
    bool vk_output_valid(uint32_t w, uint32_t h, VkFormat format) const {
        return vk_output && vk_w == w && vk_h == h && vk_format == format;
    }
    bool ensure_vk_output(uint32_t w, uint32_t h, VkFormat format);
    void release_vk_output();
    // The network for a frame of w x h in `format` (it runs the network only; the composition is the caller's).
    // Builds in the background: returns false, with `status` saying so, until
    // the network is ready, and the caller leaves the frame alone meanwhile.
    bool ensure_runtime(const Controls& controls, uint32_t w, uint32_t h, VkFormat format);
    bool ensure_runtime_(const Controls& controls, uint32_t w, uint32_t h, VkFormat format);
    // The background build. Everything the thread needs is copied in; it hands
    // back either a runtime or an error under `build_lock`, and the render
    // thread adopts whichever on its next call.
    std::thread build_thread;
    std::mutex build_lock;
    // Background threads whose result is handed over but which may still be ending. The game's thread never waits for
    // one to end: a thread's end runs every module's thread detach under the loader lock, and waiting for that inside
    // the game's frame, with the game's and ReShade's locks held, waits on whatever any other thread of the game does
    // under the loader lock. Each is joined once it has ended (reap_threads, every call), the rest when the session goes.
    std::vector<std::thread> ending_threads;
    void let_end(std::thread& t) {
        if (t.joinable()) ending_threads.push_back(std::move(t));
    }
    // A std::thread's Win32 handle under either of mingw's thread models: win32 (the native handle is the HANDLE; the
    // x86_64 toolchain) or posix (winpthreads: a pthread_t, whose HANDLE pthread_gethandle gives; the i686 toolchain).
    static HANDLE os_handle(std::thread& t) {
#ifdef WIN_PTHREADS_H
        return static_cast<HANDLE>(pthread_gethandle(t.native_handle()));
#else
        return reinterpret_cast<HANDLE>(t.native_handle());
#endif
    }
    void reap_threads() {
        for (size_t i = ending_threads.size(); i-- > 0;) {
            std::thread& t = ending_threads[i];
            const HANDLE h = os_handle(t);
            if (!h || WaitForSingleObject(h, 0) != WAIT_OBJECT_0) continue;   // still running (or not known: kept)
            t.join();
            ending_threads.erase(ending_threads.begin() + long(i));
        }
    }
    bool building{}, build_done{};
    int build_layer{};   // the NR-chain layer whose call started the build
    std::chrono::steady_clock::time_point build_started{};
    // [Network] Model: the network the switch asks for, the one being built, and the one running (Built::net).
    NetworkSwitch net_switch;
    int net_want{}, build_net{}, running_net{-1};
    // Controls::reuse_every was asked for once: from then on every runtime is built able to reuse the network's edit.
    // Never cleared, so the cache keeps one kind of runtime.
    bool reuse_want{}, build_reuse{};
    std::chrono::steady_clock::time_point reuse_logged{};
    int call_net{-1};   // Session::set_call_network
    // The network this call wants: an NR-chain layer's own (set_call_network) when the run can serve it,
    // else [Network] Model (the switch, which also takes the hotkey: called on every call either way).
    int wanted_network() {
        const int want = wanted_network_(net_switch.frame(root + "\\dlssnr-amd.ini"));
        net_users[want][call_layer] = std::chrono::steady_clock::now();
        return want;
    }
    // Which NR-chain layers (Session::set_call_layer) ran each network lately: act_for never gives a network the
    // activation arena of one another layer still runs (every layer's network its own working memory; a
    // shared one is zero-filled at every turn).
    std::map<int, std::map<int, std::chrono::steady_clock::time_point>> net_users;
    int wanted_network_(int model) {
        const int layer = call_net;
        if (layer < 0 || layer >= nr::kNetworkChoices || layer == model) return model;
        if (net_switch.available(layer)) return layer;
        if (said_unavailable.insert(layer).second)
            log("[nr] a layer's network %s cannot run here (%s); it runs %s", nr::network_codename(layer),
                net_switch.reason(layer).c_str(), nr::network_codename(model));
        return model;
    }
    std::set<int> said_unavailable;
    // What can run in this run, checked once the game's device is known (NetworkSwitch::unavailable).
    bool availability_checked{};
    void check_availability();
    uint32_t build_w{}, build_h{};
    VkFormat build_format{VK_FORMAT_UNDEFINED};
    std::unique_ptr<Runtime> built;
    std::string build_error;
    double build_seconds{};
    // A build that ran out of *host* memory is a moment, not a verdict: the one
    // that failed did so 168 ms after a fullscreen transition, while DXVK was
    // re-making a 4K swapchain and the game was reloading into the same 2 GB.
    // The same build a few seconds later may well fit, so it is retried with a
    // backoff instead of latching `failed` and declining every frame for the
    // rest of the session, which is what used to happen. Every other exception
    // still latches - a device that cannot run this network will not start to.
    bool build_oom{};
    unsigned oom_failures{};
    static constexpr unsigned kOomRetries = 4;
    std::chrono::steady_clock::time_point retry_at{};
    bool motion_point{};     // Session::set_motion_point
    bool keep_caller_state{};   // Session::set_keep_caller_state
    // One raw-Vulkan recording into the caller's list: through vkd3d-proton's interop begin/end when the caller's
    // state is to be kept (and it can), else the plain command buffer; leave_list then puts vkd3d's view of the list
    // right for what comes after.
    struct Recording {
        ID3D12Device* device = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        bool interop = false;
        ~Recording() { if (interop) end_interop_recording(device, list); }
    };
    VkCommandBuffer begin_recording(ID3D12Device* device, ID3D12GraphicsCommandList* list, Recording& rec);
    void leave_list(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Recording& rec);
    bool ensure_own_command_buffer();
    bool ensure_d3d12_device(ID3D12Device* device);
    bool fits_in_memory(uint32_t w, uint32_t h, bool quiet = false);   // quiet: no log, no status
    // Make `entry` the one the run paths use. Returns false when there is no
    // such entry.
    bool select_runtime(uint32_t w, uint32_t h, VkFormat format);
    // The running network serves while another builds (an [Int4Mixed] or pass-count switch): it is in use.
    void touch_running() {
        for (auto& e : cache)
            if (e.runtime.get() == runtime) { e.last = std::chrono::steady_clock::now(); e.used = ++use_stamp; }
    }
    bool have_runtime(uint32_t w, uint32_t h) const {
        for (const auto& e : cache) if (e.width == w && e.height == h) return true;
        return false;
    }
    // True when the card can hold another network at this extent. Otherwise the
    // least recently used entry is evicted (one per call) and false is returned:
    // the frame passes through and the build is asked for again later.
    bool make_room(uint32_t w, uint32_t h);
    bool last_ok{true};    // the last ensure_runtime ran the network (State::passed)
    bool said_no_room{};   // make_room: a layer's network waits for room (said once until it gets it)
    // fits_in_memory's refusals, by extent: noted once, then at most every 10 s (it is asked every evaluate while the
    // card is full: a stress test, 2723 lines in 900 frames)
    std::map<uint64_t, std::chrono::steady_clock::time_point> vram_said;
    void drop(size_t index, const char* why = "to make room");
    // Feature ids, handed out by Session::create_feature. Monotonic, never
    // reused, so a stale id can only ever miss.
    uint64_t next_feature{1};
    // NVIDIA's resize semantics (310.9 on an RTX 5090): in that fixture, evaluating at another extent matched the first
    // frame of a newly created feature there. We reset history on a size change based on that measurement. `resized`
    // says so for this evaluation (the caller ORs it into the frame's Reset) and remembers the extent.
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> feature_extent;
    uint64_t resizes{};
    bool resized(uint64_t feature, uint32_t w, uint32_t h) {
        auto& e = feature_extent[feature];
        const bool changed = e.first && (e.first != w || e.second != h);
        if (changed) last_resize = std::chrono::steady_clock::now();
        if (changed && ++resizes <= 8)
            log("[nr] feature #%u: %ux%u -> %ux%u, history restarts (as a new feature at that extent)%s",
                unsigned(feature), e.first, e.second, w, h, resizes == 8 ? "; not logged again" : "");
        e = {w, h};
        return changed;
    }
    // One of everything per swapchain image. A binary semaphore may have exactly
    // one pending signal-wait pair, and the present waiting on ours is still
    // outstanding when the next frame is recorded - so a single one reused every
    // frame is undefined, which is what a black screen followed by a crash looks
    // like. Same argument for the command buffer and the fence.
    struct PresentSlot {
        VkCommandBuffer cmd{};
        VkFence fence{};
        VkSemaphore done{};
        bool pending{};
    };
    std::vector<PresentSlot> slots;
    VkCommandPool slot_pool{};
    uint32_t slot_family{~0u};
    bool ensure_slots(uint32_t count);
    bool wait_slot(PresentSlot& slot);
    void release_slots();
    // Resetting a command buffer the GPU is still reading is undefined, and at
    // a 4K working extent the GPU is a long way behind by the time the next
    // frame arrives. One fence, waited on before each reuse.
    VkFence own_fence{};
    uint32_t pool_family{~0u};
    // An extent seen but not yet built for, and how many consecutive frames it
    // has held. See run_present_vulkan.
    uint32_t pending_w{}, pending_h{}, pending_frames{};
    bool own_cmd_pending{};
    bool wait_for_own_cmd();
};

void Session::Impl::release_vk_output() {
    if (vk_output_view) { vkDestroyImageView(handles.device, vk_output_view, nullptr); vk_output_view = VK_NULL_HANDLE; }
    if (vk_output) { vkDestroyImage(handles.device, vk_output, nullptr); vk_output = VK_NULL_HANDLE; }
    if (vk_output_memory) { vkFreeMemory(handles.device, vk_output_memory, nullptr); vk_output_memory = VK_NULL_HANDLE; }
    vk_w = vk_h = 0; vk_format = VK_FORMAT_UNDEFINED;
}

bool Session::Impl::ensure_vk_output(uint32_t w, uint32_t h, VkFormat format) {
    release_vk_output();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(handles.device, &ci, nullptr, &vk_output) != VK_SUCCESS) {
        status = "could not create the render-resolution output image";
        return false;
    }
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(handles.device, vk_output, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(handles.physical, &mem);
    uint32_t type = mem.memoryTypeCount;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    if (type == mem.memoryTypeCount) { release_vk_output(); status = "no device-local memory"; return false; }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(handles.device, &alloc, nullptr, &vk_output_memory) != VK_SUCCESS ||
        vkBindImageMemory(handles.device, vk_output, vk_output_memory, 0) != VK_SUCCESS) {
        release_vk_output(); status = "could not back the output image"; return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = vk_output; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(handles.device, &vi, nullptr, &vk_output_view) != VK_SUCCESS) {
        release_vk_output(); status = "could not view the output image"; return false;
    }
    vk_w = w; vk_h = h; vk_format = format;
    return true;
}

bool Session::Impl::wait_for_own_cmd() {
    if (!own_cmd_pending || !own_fence) return true;
    // A second is far longer than any frame; reaching it means the device is
    // lost or hung, and continuing to submit into that is how a hang becomes a
    // crash.
    const VkResult r = vkWaitForFences(handles.device, 1, &own_fence, VK_TRUE, 1000000000ull);
    if (r != VK_SUCCESS) {
        status = "the previous neural rendering submit never completed";
        failed = true;
        return false;
    }
    vkResetFences(handles.device, 1, &own_fence);
    own_cmd_pending = false;
    return true;
}

bool Session::Impl::ensure_own_command_buffer() {
    // A pool belongs to one queue family for its whole life. If the family we
    // submit on has changed - and it does, because the queue a game presents on
    // is not necessarily the one we found when looking for a compute queue -
    // the pool has to go with it. Submitting a buffer from another family's pool
    // is invalid and takes the process down.
    if (own_cmd && pool_family != access.family) {
        wait_for_own_cmd();
        if (pool) { vkDestroyCommandPool(handles.device, pool, nullptr); pool = VK_NULL_HANDLE; }
        own_cmd = VK_NULL_HANDLE;
    }
    if (own_cmd) return true;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = access.family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(handles.device, &pi, nullptr, &pool) != VK_SUCCESS) {
        status = "could not create a command pool on the game's device";
        return false;
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(handles.device, &ai, &own_cmd) != VK_SUCCESS) {
        status = "could not allocate a command buffer";
        return false;
    }
    pool_family = access.family;
    if (own_fence) return true;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(handles.device, &fi, nullptr, &own_fence) != VK_SUCCESS) {
        status = "could not create the fence that paces our own submits";
        return false;
    }
    return true;
}

bool Session::Impl::wait_slot(PresentSlot& slot) {
    if (!slot.pending) return true;
    const VkResult r = vkWaitForFences(handles.device, 1, &slot.fence, VK_TRUE, 1000000000ull);
    if (r != VK_SUCCESS) {
        status = "the previous neural rendering submit never completed";
        failed = true;
        return false;
    }
    vkResetFences(handles.device, 1, &slot.fence);
    slot.pending = false;
    return true;
}

void Session::Impl::release_slots() {
    for (auto& slot : slots) {
        wait_slot(slot);
        if (slot.done) vkDestroySemaphore(handles.device, slot.done, nullptr);
        if (slot.fence) vkDestroyFence(handles.device, slot.fence, nullptr);
    }
    slots.clear();
    if (slot_pool) { vkDestroyCommandPool(handles.device, slot_pool, nullptr); slot_pool = VK_NULL_HANDLE; }
    slot_family = ~0u;
}

bool Session::Impl::ensure_slots(uint32_t count) {
    if (count == 0) count = 1;
    if (slots.size() == count && slot_family == access.family) return true;
    release_slots();

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = access.family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(handles.device, &pi, nullptr, &slot_pool) != VK_SUCCESS) {
        status = "could not create a command pool on the game's present family";
        return false;
    }
    std::vector<VkCommandBuffer> buffers(count);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = slot_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = count;
    if (vkAllocateCommandBuffers(handles.device, &ai, buffers.data()) != VK_SUCCESS) {
        status = "could not allocate the per-image command buffers";
        return false;
    }
    slots.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        slots[i].cmd = buffers[i];
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateFence(handles.device, &fi, nullptr, &slots[i].fence) != VK_SUCCESS ||
            vkCreateSemaphore(handles.device, &si, nullptr, &slots[i].done) != VK_SUCCESS) {
            status = "could not create the per-image fence and semaphore";
            release_slots();
            return false;
        }
    }
    slot_family = access.family;
    log("[nr] %u present slots on queue family %u", count, access.family);
    return true;
}

// Will the network fit in what is left of the card?
//
// The activation arena scales with the working extent and it is not small: 1.05
// GB at 1920x1088 and 4.07 GB at 3840x2176 with every value in its own bytes,
// about 500 bytes per pixel; 384 MB and 1.53 GB since values share memory by
// lifetime (nr_graph.cpp `reuse`), about 185 bytes per pixel (200 below, for
// the small extents' fixed part). NR_ARENA_REUSE=0 turns sharing off. A game at 4K has already taken most of a 16 GB card, so the
// fallback - which runs at output resolution, because it works on the finished
// frame - asks for four gigabytes that are not there. Allocation then fails
// somewhere inside the graph build, or succeeds by evicting the game's own
// resources, and what the player sees is a black frame and a crash.
//
// Refusing with a number in the log is strictly better than either.
static uint64_t arena_bytes_per_pixel() {
    const char* e = std::getenv("NR_ARENA_REUSE");
    return e && *e && std::atoi(e) == 0 ? 520 : 200;
}

bool Session::Impl::fits_in_memory(uint32_t w, uint32_t h, bool quiet) {
    // The padded working extent, which is what the arena is sized from.
    const uint64_t pixels = uint64_t((w + 63) / 64 * 64) * ((h + 63) / 64 * 64);
    const uint64_t need = pixels * arena_bytes_per_pixel() + (300ull << 20);   // arena, plus weights and scratch

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(handles.physical, &props);

    // The largest device-local heap's free space, by budget where the driver
    // reports one and by heap size where it does not.
    uint64_t available = 0;
    const auto& mem = props.memoryProperties;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (!(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        const uint64_t heap = mem.memoryHeaps[i].size;
        const uint64_t used = budget.heapUsage[i];
        const uint64_t cap = budget.heapBudget[i] ? budget.heapBudget[i] : heap;
        const uint64_t free = cap > used ? cap - used : 0;
        if (free > available) available = free;
    }
    const double need_mb = double(need) / (1 << 20);
    const double free_mb = double(available) / (1 << 20);
    if (quiet) return !available || need <= available;
    if (available && need > available) {
        char message[256];
        std::snprintf(message, sizeof message,
                      "the network needs about %.0f MB at %ux%u and only %.0f MB is free; "
                      "not building it. At output resolution this is expected - run the "
                      "network before the upscaler instead, where the extent is smaller.",
                      need_mb, w, h, free_mb);
        status = message;
        const auto now = std::chrono::steady_clock::now();
        auto& said = vram_said[uint64_t(w) << 32 | h];
        if (said == std::chrono::steady_clock::time_point{} || now - said >= std::chrono::seconds(10)) {
            said = now;
            events::note(events::kResidency, "%s", message);
        }
        return false;
    }
    vram_said.erase(uint64_t(w) << 32 | h);   // (a later refusal is noted at once)
    log("[nr] %ux%u needs about %.0f MB, %.0f MB free", w, h, need_mb, free_mb);
    return true;
}

bool Session::Impl::select_runtime(uint32_t w, uint32_t h, VkFormat format) {
    for (auto& e : cache) {
        if (e.runtime->stale()) continue;   // its images belong to a later runtime (dropped by trim_cache)
        if (e.width != w || e.height != h || e.format != format) continue;
        if (e.net != net_want) continue;   // [Network] Model: the network the switch asks for
        if (e.reuse != reuse_want) continue;
        e.used = ++use_stamp;
        const auto now = std::chrono::steady_clock::now();
        if (runtime != e.runtime.get()) {
            const bool stale = e.last != std::chrono::steady_clock::time_point{} && now - e.last > kStaleHistory;
            if (stale) e.runtime->forget_history();
            // Said when a runtime comes back, not on every change: the NR chain's layers take turns every frame.
            if (e.last == std::chrono::steady_clock::time_point{} || now - e.last > std::chrono::seconds(1))
                log("[nr] runtime cache hit: %ux%u, %u built, %u live features%s", e.width, e.height,
                    unsigned(cache.size()), e.runtime->live_features(), stale ? ", history started afresh" : "");
        }
        e.last = now;
        e.layers[call_layer] = now;
        runtime = e.runtime.get();
        width = e.width; height = e.height; this->format = e.format;
        running_net = e.net;
        return true;
    }
    return false;
}

void Session::Impl::drop(size_t index, const char* why) {
    auto& e = cache[index];
    events::note(events::kResidency, "evicting the %ux%u network %s", e.width, e.height, why);
    if (runtime == e.runtime.get()) { runtime = nullptr; width = height = 0; }
    // Nothing of ours may still be reading it. On the paths that submit for
    // themselves this waits on our own fence; on the paths that record into the
    // game's command list the game's frames in flight may still hold it, so it
    // is not destroyed here but retired (see Retired).
    wait_for_own_cmd();
    retired.push_back({std::move(e.runtime), calls, std::chrono::steady_clock::now()});
    cache.erase(cache.begin() + long(index));
}

void Session::Impl::free_retired() {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = retired.size(); i-- > 0;) {
        if (calls - retired[i].call < kRetireCalls || now - retired[i].at < kRetireTime || !retired[i].runtime->recordings_done()) continue;
        retired.erase(retired.begin() + long(i));
        events::note(events::kResidency, "evicted network freed");
    }
}

bool Session::Impl::vram(Vram& v) const {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext = &budget;
    if (!handles.physical) return false;
    vkGetPhysicalDeviceMemoryProperties2(handles.physical, &props);
    const auto& mem = props.memoryProperties;
    nr::vram::memory_types(mem);   // which of our allocations are video memory (NR Control's breakdown)
    v = {};
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        if (!(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) || mem.memoryHeaps[i].size <= v.heap) continue;
        // a driver may report a budget above the heap (llama.cpp's ggml-vulkan clamps it the same way)
        v.heap = mem.memoryHeaps[i].size;
        v.budget = budget.heapBudget[i] ? std::min(budget.heapBudget[i], v.heap) : v.heap;
        v.usage = budget.heapUsage[i];
    }
    // NR_DEBUG_VRAM_BUDGET=<MiB> (tests): a smaller budget, to see Residency under pressure
    static const uint64_t debug_budget = [] {
        char e[24] = {};
        const DWORD n = GetEnvironmentVariableA("NR_DEBUG_VRAM_BUDGET", e, sizeof e);
        return n && n < sizeof e ? uint64_t(std::atoll(e)) << 20 : 0ull;
    }();
    if (debug_budget && v.heap) v.budget = std::min(v.budget, debug_budget);
    return v.heap != 0;
}

void Session::Impl::park(size_t index, const char* why) {
    const void* net = cache[index].runtime->network_id();
    bool held = false;
    for (const auto& p : parked) held |= core_network(*p.core) == net;
    if (!held) {
        const Built& e = cache[index];
        if (auto core = e.runtime->share_for(e.width, e.height)) {
            release_image_heap(*core);
            Parked p;
            p.core = std::move(core);
            p.format = e.format; p.net = e.net; p.width = e.width; p.height = e.height; p.last = e.last;
            parked.push_back(std::move(p));
            events::note(events::kResidency, "%s at %ux%u parked: its weights stay, its runtimes and images go %s", nr::network_codename(e.net),
                e.width, e.height, why);
        }
    }
    for (size_t i = cache.size(); i-- > 0;)
        if (cache[i].runtime->network_id() == net) drop(i, why);
}

bool Session::Impl::ram_short() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m) || !m.ullTotalPhys) return false;
    return m.ullAvailPhys < std::max<uint64_t>(2ull << 30, m.ullTotalPhys / 10);
}

bool Session::Impl::start_offload(Parked& p) {
    // not while a build runs: it may be making a runtime from this very network (a parked one asked for again)
    if (offloading || building || network_offloaded(*p.core)) return false;
    const void* net = core_network(*p.core);
    for (const auto& r : retired)
        if (r.runtime && r.runtime->network_id() == net) return false;   // its frames may still be in flight
    offloading = p.core;
    offload_done = false;
    const HostDevice host = make_host();
    const QueueAccess access_copy = access;
    const auto core = p.core;
    offload_weights.clear();
    offload_thread = std::thread([this, host, access_copy, core] {
        // the read-back only: nothing of the network changes on this thread (poll_offload releases it)
        HostDevice locked = host;
        locked.queue_lock = access_copy.lock_quiet;
        locked.queue_unlock = access_copy.unlock_quiet;
        bool ok = false;
        try { ok = download_network_weights(locked, *core, offload_weights); } catch (...) { ok = false; }
        offload_ok = ok;
        offload_done = true;
    });
    return true;
}

void Session::Impl::poll_offload() {
    if (!offloading || !offload_done) return;
    let_end(offload_thread);
    if (offload_ok) {
        release_network(*offloading, std::move(offload_weights));
        offload_ok = network_offloaded(*offloading);
    }
    offload_weights.clear();
    for (const auto& p : parked)
        if (p.core == offloading)
            events::note(events::kResidency, "%s at %ux%u: %s", nr::network_codename(p.net), p.width, p.height,
                offload_ok ? "its weights moved to RAM, its video memory freed" : "its weights could not be moved to RAM");
    if (!offload_ok)   // not tried again: freed instead
        for (size_t i = parked.size(); i-- > 0;)
            if (parked[i].core == offloading) parked.erase(parked.begin() + long(i));
    offloading.reset();
}

// What residency() decided, for NR Control (nr_pe_events.hpp): every network held, where it is, the budget and why.
void Session::Impl::publish_residency(int mode, bool nr_off) {
    events::Residency r;
    r.mode = mode;
    if (Vram v; vram(v)) {
        r.budget = v.budget; r.usage = v.usage; r.heap = v.heap;
        r.reserve = std::max<uint64_t>(1ull << 30, v.heap / 10);
    }
    for (const auto& e : cache)
        r.nets.push_back({e.net, e.net == running_net && !nr_off ? 1 : 2, e.width, e.height});
    for (const auto& p : parked)
        r.nets.push_back({p.net, p.core == offloading ? 3 : network_offloaded(*p.core) ? 4 : 2, p.width, p.height});
    if (offloading) {
        for (const auto& p : parked)
            if (p.core == offloading) r.status = std::string("moving ") + nr::network_codename(p.net) + "'s weights to RAM";
    } else if (building) {
        r.status = std::string("building ") + (build_net >= 0 ? nr::network_codename(build_net) : "a network");
    }
    r.reason = residency_why;
    if (width && height) {   // fits_in_memory's estimate at the serving extent
        r.need_w = width; r.need_h = height;
        r.need = uint64_t((width + 63) / 64 * 64) * ((height + 63) / 64 * 64) * arena_bytes_per_pixel() + (300ull << 20);
    }
    events::publish(std::move(r));
}

void Session::Impl::residency(bool switched, bool nr_off) {
    const auto now = std::chrono::steady_clock::now();
    residency_checked = now;
    poll_offload();
    const int mode = net_switch.file().residency;   // 0 auto, 1 resident, 2 offload, 3 minimal
    struct Publish { Impl* m; int mode; bool nr_off; ~Publish() { m->publish_residency(mode, nr_off); } } publish{this, mode, nr_off};
    // Of another kind and not used for a second: right after a switch the left network may still serve a feature whose
    // new network is not built yet (net_only, touch_running).
    auto inactive = [&](const Built& e) { return nr_off || (e.net != running_net && now - e.last > std::chrono::seconds(1)); };
    if (mode == 1) return;
    if (mode == 3) {
        for (size_t i = cache.size(); i-- > 0;)
            if (inactive(cache[i]))
                drop(i, nr_off ? "(NR off; [Network] Residency frees it)" : "(another network runs; [Network] Residency frees it)");
        if (!parked.empty() && !offloading) { parked.clear(); events::note(events::kResidency, "parked networks freed ([Network] Residency)"); }
        return;
    }
    // offload and auto: a network of another kind is parked at once (its runtimes and images are of no use until it runs
    // again, and its history restarts then anyway); with NR off the running one too (offload), or when memory is short
    // (auto, below)
    if (!nr_off || mode == 2)
        for (size_t i = cache.size(); i-- > 0;)
            if (i < cache.size() && inactive(cache[i])) park(i, nr_off ? "(NR off)" : "(another network runs)");
    if (mode == 2) {   // offload: every parked network's weights to RAM
        for (auto& p : parked)
            if ((nr_off || p.net != net_want) && start_offload(p)) break;
        if (ram_short() && !parked.empty() && !offloading) {
            size_t lru = 0;
            for (size_t i = 1; i < parked.size(); ++i) if (parked[i].last < parked[lru].last) lru = i;
            events::note(events::kResidency, "parked %s freed (RAM is short)", nr::network_codename(parked[lru].net));
            parked.erase(parked.begin() + long(lru));
        }
        return;
    }
    // auto: by the device-local heap's room
    Vram v;
    if (!vram(v) || now - residency_freed < kFreedQuiet) return;
    const uint64_t reserve = std::max<uint64_t>(1ull << 30, v.heap / 10);
    const uint64_t free = v.budget > v.usage ? v.budget - v.usage : 0;
    const double pressure = v.budget ? double(v.usage) / double(v.budget) : 0.0;
    const bool short_now = free < (switched ? reserve + reserve / 2 : reserve);
    residency_why = short_now ? "remaining budget below the reserve" : pressure > 0.7 ? "video memory over 70 % of the budget: idle networks kept for a shorter time" : "";
    // the keep-alive in video memory of what is not running shrinks as the heap fills (the D3D12 Residency Starter
    // Library's grace period): above 70 % of the budget 60 s, down to 1 s at 100 %
    const double keep = pressure > 0.7 ? 60.0 - 59.0 * std::min(1.0, (pressure - 0.7) / 0.3) : 1e30;
    auto log_state = [&] {
        log("[nr] Residency auto: %.0f MiB free of a %.0f MiB budget (%.0f %% used), reserve %.0f MiB",
            double(free) / (1 << 20), double(v.budget) / (1 << 20), pressure * 100.0, double(reserve) / (1 << 20));
    };
    // NR off: the running network is parked when memory is short or it outlived the keep-alive
    if (nr_off && (short_now || std::chrono::duration<double>(now - last_call).count() > keep))
        for (size_t i = cache.size(); i-- > 0;)
            if (i < cache.size()) { log_state(); park(i, "(NR off)"); residency_freed = now; return; }
    // RAM short: the oldest network in RAM goes
    if (ram_short()) {
        size_t lru = parked.size();
        for (size_t i = 0; i < parked.size(); ++i)
            if (network_offloaded(*parked[i].core) && (lru == parked.size() || parked[i].last < parked[lru].last)) lru = i;
        if (lru < parked.size()) {
            events::note(events::kResidency, "%s freed from RAM (RAM is short)", nr::network_codename(parked[lru].net));
            parked.erase(parked.begin() + long(lru));
        }
    }
    // video memory: the least recently used parked network still in it moves to RAM - not the one NR asks for now
    // (NR back on: it is about to run again)
    size_t lru = parked.size();
    for (size_t i = 0; i < parked.size(); ++i)
        if (!network_offloaded(*parked[i].core) && parked[i].core != offloading && (nr_off || parked[i].net != net_want) &&
            (lru == parked.size() || parked[i].last < parked[lru].last))
            lru = i;
    if (lru == parked.size()) return;
    const bool old = std::chrono::duration<double>(now - parked[lru].last).count() > keep;
    if (!short_now && !old) return;
    if (ram_short()) {
        log_state();
        events::note(events::kResidency, "parked %s at %ux%u freed (%s; RAM is short too)", nr::network_codename(parked[lru].net),
            parked[lru].width, parked[lru].height, short_now ? "video memory is short" : "video memory is getting full");
        parked.erase(parked.begin() + long(lru));
    } else if (start_offload(parked[lru])) {
        log_state();
        events::note(events::kResidency, "parked %s at %ux%u: moving its weights to RAM (%s)", nr::network_codename(parked[lru].net),
            parked[lru].width, parked[lru].height, short_now ? "video memory is short" : "video memory is getting full");
    } else {
        return;   // still in flight: next check
    }
    residency_freed = now;
}

std::shared_ptr<ActArenaRef> Session::Impl::act_for(int net) const {
    // every network's arena, resident or parked, with its kind and its memory
    struct Held { int net; const void* network; std::shared_ptr<ActArenaRef> act; };
    std::vector<Held> held;
    for (const auto& e : cache) held.push_back({e.net, e.runtime->network_id(), e.runtime->act_arena()});
    for (const auto& p : parked) held.push_back({p.net, core_network(*p.core), core_act_arena(*p.core)});
    std::set<const void*> busy;
    for (const auto& h : held)
        if (h.net == net && h.act) busy.insert(act_arena_id(*h.act));
    const auto now = std::chrono::steady_clock::now();
    for (const auto& h : held) {   // another layer's network
        if (h.net == net || !h.act) continue;
        const auto users = net_users.find(h.net);
        if (users == net_users.end()) continue;
        for (const auto& [layer, at] : users->second)
            if (layer != call_layer && now - at < std::chrono::seconds(2)) busy.insert(act_arena_id(*h.act));
    }
    const auto pref = feature_slot.find(current_feature);
    std::shared_ptr<ActArenaRef> best;
    for (const auto& h : held) {
        if (h.net == net || !h.act || busy.count(act_arena_id(*h.act))) continue;
        if (pref != feature_slot.end() && h.network == pref->second) return h.act;
        if (!best || act_arena_bytes(*h.act) > act_arena_bytes(*best)) best = h.act;
    }
    return best;
}

// At most kMaxResident networks of each [Int4Mixed] kind: one per extent the session serves at once (OptiScaler's NR
// before and after the upscaler), and kMaxEntries runtimes (several extents of one working extent share a network).
// Extents left behind - a resolution change, a RenoDX-style resolution scale, a game's dynamic resolution - go least
// recently used first, once unused for kIdleDrop: more extents in use at once than the caps stay, rather than being
// rebuilt every frame. A network goes with all of its runtimes. `adopting`: a runtime about to join the cache
// (counted, not dropped).
void Session::Impl::trim_cache(const Built* adopting) {
    const auto now = std::chrono::steady_clock::now();
    // Runtimes built inside a frame whose images a later one has taken: never selected again.
    for (size_t i = cache.size(); i-- > 0;)
        if (cache[i].runtime->stale()) drop(i, "(its images went to a later frame size)");
    for (int kind = 0; kind < nr::kNetworkChoices; ++kind) {
        const auto of_kind = [kind](const Built& e) {
            return e.net == kind;
        };
        for (;;) {
            // This kind's runtimes, and its networks with their latest use.
            struct Use { uint64_t used{}; std::chrono::steady_clock::time_point last{}; bool held{}; };
            std::map<const void*, Use> nets;
            size_t entries = 0, oldest = cache.size();
            for (size_t i = 0; i < cache.size(); ++i) {
                if (!of_kind(cache[i])) continue;
                ++entries;
                Use& u = nets[cache[i].runtime->network_id()];
                u.used = std::max(u.used, cache[i].used); u.last = std::max(u.last, cache[i].last);
                u.held |= layer_held(cache[i], now);
                if (oldest == cache.size() || cache[i].used < cache[oldest].used) oldest = i;
            }
            if (adopting && of_kind(*adopting)) {
                ++entries;
                Use& u = nets[adopting->runtime->network_id()];
                u.used = ~uint64_t(0); u.last = now;
            }
            // More than one network of a kind: one no feature runs in any more goes once unused for kIdleDrop (a smaller
            // memory an upward sweep left behind), any other once unused for kNetIdle.
            if (nets.size() > 1) {
                const void* idle = nullptr;
                for (const auto& [id, u] : nets) {
                    if (!id || (adopting && id == adopting->runtime->network_id()) || u.held) continue;
                    bool referenced = false;
                    for (const auto& fs : feature_slot) referenced |= fs.second == id;
                    if (now - u.last > (referenced ? std::chrono::duration_cast<std::chrono::milliseconds>(kNetIdle) : kIdleDrop)) {
                        idle = id; break;
                    }
                }
                if (idle) {
                    for (size_t i = cache.size(); i-- > 0;)
                        if (of_kind(cache[i]) && cache[i].runtime->network_id() == idle) drop(i, "(unused for a while; another network serves)");
                    continue;
                }
            }
            if (nets.size() > kMaxResident) {
                const void* victim = nullptr;
                Use v{~uint64_t(0), {}};
                for (const auto& [id, u] : nets) if (!u.held && u.used < v.used) { v = u; victim = id; }
                if (!victim || now - v.last < kIdleDrop) break;
                for (size_t i = cache.size(); i-- > 0;)
                    if (of_kind(cache[i]) && cache[i].runtime->network_id() == victim)
                        drop(i, "(least recently used; more extents than are served at once)");
                continue;
            }
            // One built inside a frame goes as soon as the last few evaluations did not use it (another is a millisecond
            // away); the others once idle.
            if (entries <= kMaxEntries || oldest == cache.size() ||
                (cache[oldest].light ? use_stamp - cache[oldest].used < 4 : now - cache[oldest].last < kIdleDrop))
                break;
            drop(oldest, "(least recently used; more extents of one network than are served at once)");
        }
    }
}

bool Session::Impl::make_room(uint32_t w, uint32_t h) {
    if (fits_in_memory(w, h, said_no_room)) return true;   // (quiet while a layer already waits for room)
    if (cache.empty()) return false;
    // Never a network another NR-chain layer is running: with two layers on two networks that do not both fit, each
    // layer's build evicted the other's network every few frames and NR went on and off for good. The layer whose
    // network does not fit waits (it passes its frame through, said once) until memory frees.
    const auto now = std::chrono::steady_clock::now();
    const auto other_layer = [&](const Built& b) {
        if (!chain_active(now)) return false;
        for (const auto& [layer, at] : b.layers)
            if (layer != call_layer && now - at < kNetIdle) return true;
        return false;
    };
    size_t oldest = cache.size();
    for (size_t i = 0; i < cache.size(); ++i)
        if (!other_layer(cache[i]) && (oldest == cache.size() || cache[i].used < cache[oldest].used)) oldest = i;
    if (oldest == cache.size()) {
        status = "not enough video memory for this layer's network beside the other layers' (it waits for room)";
        if (!said_no_room) {
            said_no_room = true;
            log("[nr] NR-chain layer %d: %s", call_layer, status.c_str());
        }
        return false;
    }
    said_no_room = false;
    // One at a time: an evicted network keeps its memory until it is freed (see Retired), and the build is asked
    // for again once it has been.
    drop(oldest);
    status = "freeing the least recently used network to make room";
    return false;
}

bool Session::Impl::ensure_runtime(const Controls& controls, uint32_t w, uint32_t h, VkFormat format) {
    const bool ok = ensure_runtime_(controls, w, h, format);
    if (ok && runtime && controls.reuse_every >= 2 && runtime->reuse_allowed()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - reuse_logged > std::chrono::seconds(5)) {
            reuse_logged = now;
            const auto n = runtime->reuse_counts();
            const double frames = double(n.ran + n.skipped);
            log("[nr] temporal reuse, every %d: %llu frames ran the network, %llu skipped it (%.0f %%), the motion gate "
                "ran it %llu times; motion since the last line: mean %.1f px, peak %.1f px (Gate %.0f)",
                controls.reuse_every, (unsigned long long)n.ran, (unsigned long long)n.skipped,
                frames > 0 ? 100.0 * double(n.skipped) / frames : 0.0, (unsigned long long)n.gated,
                n.motion_samples ? n.motion_sum / double(n.motion_samples) : 0.0, double(n.motion_peak),
                double(controls.reuse_gate));
        }
    }
    ++(ok ? ran : passed);
    last_ok = ok;
    last_w = w; last_h = h; last_format = format;
    if (ok && runtime) {
        runtime->set_style_grade(net_switch.file().style_grade);   // [Network] Style Grade, live
        feature_slot[current_feature] = runtime->network_id();
        Runtime*& last = feature_runtime[current_feature];
        if (last != runtime) { feature_prev_runtime[current_feature] = last; last = runtime; }
    }
    heap_poll();
    if (!building && drs_active()) prefetch();
    {
        const auto now = std::chrono::steady_clock::now();
        if (pass_logged == std::chrono::steady_clock::time_point{}) pass_logged = now;
        if (now - pass_logged >= kPassLog) {
            // every 10 s with NR asked for: a log that shows NR running as well as not (the OptiScaler route has no
            // frame line of its own)
            if (passed || ran)
                log("[nr] last %.0f s: %llu evaluations ran the network, %llu passed the frame through (%s)",
                    std::chrono::duration<double>(now - pass_logged).count(), (unsigned long long)ran,
                    (unsigned long long)passed, ok ? "running now" : status.c_str());
            ran_total += ran; passed_total += passed; ran = passed = 0;
            pass_logged = now;
        }
    }
    return ok;
}

bool Session::Impl::ensure_runtime_(const Controls& controls, uint32_t w, uint32_t h, VkFormat format) {
    ++calls;
    last_call = std::chrono::steady_clock::now();
    layer_seen[call_layer] = last_call;
    if (!ending_threads.empty()) reap_threads();
    if (last_call - residency_checked >= kResidencyCheck) residency(false, false);
    if (!retired.empty()) free_retired();
    if (cache.size() > kMaxResident) trim_cache();
    if (!availability_checked) check_availability();
    net_want = wanted_network();
    if (controls.reuse_every >= 2 && !reuse_want) {
        reuse_want = true;
        log("[nr] ReuseEvery %d asked for: networks are rebuilt able to reuse the network's edit (once)", controls.reuse_every);
    }
    // Adopt a finished background build first.
    {
        nr::pe::watchdog::Phase phase("adopting a finished network build (build lock)");
        std::lock_guard<std::mutex> guard(build_lock);
        if (build_done) {
            let_end(build_thread);
            build_done = false; building = false;
            if (build_plan) {
                // A plan (RuntimeConfig::plan_only): cached; its runtimes are built inside the frames that ask.
                build_plan = false;
                if (built && built->plan()) {
                    PlanEntry pe;
                    pe.format = build_format;
                    pe.net = net_of(*built);
                    pe.core = built->plan();
                    plan_extent(*pe.core, &pe.ww, &pe.wh, &pe.sh);
                    pe.used = ++use_stamp;
                    if (plans.size() >= kMaxPlans) {
                        size_t oldest = 0;
                        for (size_t i = 1; i < plans.size(); ++i) if (plans[i].used < plans[oldest].used) oldest = i;
                        plans.erase(plans.begin() + long(oldest));
                    }
                    plans.push_back(std::move(pe));
                    if (++plans_built <= 8 || plans_built % 50 == 0)
                        log("[nr] plan for the working extent %ux%u (frames up to %u rows) in %.2f s; %zu cached",
                            plans.back().ww, plans.back().wh, plans.back().sh, build_seconds, plans.size());
                } else if (build_error == "nothing left to plan around this size") {
                    prefetched_around = {build_w, build_h};
                } else {
                    log("[nr] a plan could not be built: %s", build_error.c_str());
                }
                built.reset();
            } else if (built) {
                Built entry;
                entry.width = build_w; entry.height = build_h; entry.format = build_format;
                // what was built, not what was asked: a network without what it needs is another one (the runtime
                // says why); the one asked for is not asked again this run
                entry.net = net_of(*built);
                entry.reuse = build_reuse;
                if (entry.net != build_net) {
                    net_switch.unavailable(build_net, built->network_note().empty() ? "could not be built (see dlssnr-amd.log)"
                                                                                    : built->network_note());
                    net_want = wanted_network();
                }
                entry.used = ++use_stamp;
                entry.last = std::chrono::steady_clock::now();
                entry.runtime = std::move(built);
                events::note(events::kNetwork, "network built at %ux%u on queue family %u in %.1f s; %u networks resident", entry.width,
                    entry.height, access.family, build_seconds, unsigned(cache.size() + 1));
                trim_cache(&entry);
                cache.push_back(std::move(entry));
                auto& adopted = cache.back();
                adopted.layers[build_layer] = std::chrono::steady_clock::now();
                // a parked network in use again: its runtime holds it now
                for (size_t i = parked.size(); i-- > 0;)
                    if (core_network(*parked[i].core) == adopted.runtime->network_id()) parked.erase(parked.begin() + long(i));
                runtime = adopted.runtime.get();
                width = adopted.width; height = adopted.height; this->format = adopted.format;
                running_net = adopted.net;
                // A network switch: what happens to the networks it left ([Network] Residency)
                residency(true, false);
                if (Vram v; vram(v))
                    log("[nr] %s serving; %zu runtimes, %zu parked; this process's video memory %.0f MiB (budget %.0f MiB)",
                        nr::network_codename(running_net), cache.size(), parked.size(), double(v.usage) / (1 << 20),
                        double(v.budget) / (1 << 20));
                oom_failures = 0;
                retry_at = {};
            } else {
                // a runtime from a parked network that failed: the network goes (its weights may be half back)
                for (size_t i = parked.size(); i-- > 0;)
                    if (build_share && parked[i].core == build_share) parked.erase(parked.begin() + long(i));
                build_share.reset();
                status = build_error;
                events::note(events::kNetwork, "%s", status.c_str());
                if (build_oom && oom_failures < kOomRetries) {
                    // 4, 8, 16, 32 seconds. Long enough that the load or the
                    // resolution change that took the address space has
                    // finished, short enough that a player does not give up on
                    // the feature first.
                    const unsigned wait = 4u << oom_failures;
                    ++oom_failures;
                    retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
                    events::note(events::kNetwork, "retrying the build in %u s (attempt %u of %u)", wait, oom_failures,
                        kOomRetries);
                } else {
                    failed = true;
                }
                return false;
            }
        }
    }
    if (select_runtime(w, h, format)) return true;
    // The resize path: a new frame size from a resident network or a cached plan, inside this recording.
    std::shared_ptr<const RuntimeCore> tried;
    inframe_waiting = false;
    if (inframe(w, h, format, &tried)) { inframe_wait_since = {}; return true; }
    if (inframe_waiting) {
        const auto now = std::chrono::steady_clock::now();
        if (inframe_wait_since == std::chrono::steady_clock::time_point{}) inframe_wait_since = now;
        if (now - inframe_wait_since < kHeapWait) {
            status = "making memory for the game's changing frame size; frames pass through until it is ready";
            return false;
        }
        static bool said = false;
        if (!said) {
            said = true;
            log("[nr] %ux%u: no image heap for it after %lld s; built in the background instead", w, h,
                (long long)kHeapWait.count());
        }
    } else {
        inframe_wait_since = {};
    }
    // A network switch: the running network keeps serving its frames while the other one builds, so the
    // comparison has no gap but the build itself (the first int4 build also compiles its pipelines).
    const bool net_only = runtime && running_net >= 0 && running_net != net_want && width == w && height == h &&
                           this->format == format;
    if (building) {
        if (net_only) { touch_running(); return true; }
        status = "building the network in the background; frames pass through until it is ready";
        return false;
    }
    if (!access.valid()) {
        status = "no submittable queue on the game's device; the weights cannot be uploaded";
        failed = true;
        return false;
    }
    // A host-memory failure is waiting out its backoff; the frame passes through
    // with the reason still in `status`, exactly as it does while a build runs.
    if (retry_at != std::chrono::steady_clock::time_point{}) {
        if (std::chrono::steady_clock::now() < retry_at) {
            status = build_error;
            return false;
        }
        retry_at = {};
    }
    // Checked before anything is allocated, and not treated as a hard failure:
    // a smaller extent may well fit later, and a game changing resolution is
    // exactly when that happens. Cached networks at other extents are given up
    // one at a time, oldest first, until this one fits.
    // An evicted network still holds its memory until it is freed. If the next one does not fit beside it, wait
    // for that rather than push out another: the switch keeps the running network serving, anything else passes
    // through.
    // The resize path: a cached network of the same working extent and configuration gives this extent its network
    // (Runtime::share_for) - only the images and descriptor sets are made, about 10 ms instead of a whole build, and no
    // more arena memory.
    std::shared_ptr<const RuntimeCore> share = tried;
    for (const auto& e : cache) {
        if (share) break;
        if (!same_config(e, format) || e.runtime->stale()) continue;
        if ((share = e.runtime->share_for(w, h)))
            log("[nr] %ux%u has the working extent of the %ux%u network: built from it", w, h, e.width, e.height);
    }
    // A parked network of this configuration (Residency auto): its memory is still there, only a runtime is made. The
    // runtime checks the core fits this extent and makes a whole network when it does not (the log says so).
    poll_offload();
    for (const auto& p : parked) {
        if (share) break;
        if (p.net != net_want || p.format != format || !core_runs(*p.core, w, h))
            continue;
        if (p.core == offloading) {   // its weights are on their way to RAM: built from them once they are there
            if (net_only) { touch_running(); return true; }
            status = "the network's weights are moving to RAM; frames pass through until it is back";
            return false;
        }
        share = p.core;
        log("[nr] %ux%u: %s is parked (its weights kept): a runtime is made from it", w, h, nr::network_codename(p.net));
    }
    // A frame size of a working extent no plan covers yet, with a resident network of this configuration: the plan in
    // the background (no device memory, ~0.15 s), then its runtimes inside the frames (inframe).
    if (!share && start_plan(w, h, format)) {
        if (net_only) { touch_running(); return true; }
        status = "building the plan for this frame size in the background; frames pass through until it is ready";
        return false;
    }
    // The size must hold before a whole network is built for it (a size moving every frame asked for one per frame) -
    // unless no network of this configuration is resident (the first; the plans need one), or a plan for it exists and
    // only no resident memory holds it (a sweep past the biggest network so far).
    bool have_network = false;
    const Built* any_net = nullptr;
    for (const auto& e : cache)
        if (same_config(e, format) && !e.runtime->stale()) { have_network = true; any_net = &e; break; }
    const bool capacity_miss = any_net && plan_for(w, h, format, any_net->runtime->network()) != nullptr;
    if (!share && have_network && !capacity_miss && !settled(w, h)) {
        if (net_only) { touch_running(); return true; }
        status = "the frame's size is changing; the network is built once it holds";
        return false;
    }
    if (!share && !retired.empty() && !fits_in_memory(w, h, true)) {
        if (net_only) { touch_running(); return true; }
        status = "freeing the previous network before building another";
        return false;
    }
    // a parked network whose weights are in RAM needs room too (the weights, perhaps an arena)
    if ((!share || network_offloaded(*share)) && !make_room(w, h)) return false;
    RuntimeConfig config = make_config(w, h, format);
    config.share = share;
    // the activation arena: a whole build's own or another kind's; a parked network moved to RAM gets one back too
    if (!share || network_offloaded(*share)) { config.act_arena = act_for(net_want); config.act_slack = kActSlack; }
    // Room for the game's whole render range: its guides allocated bigger than this frame (a game with dynamic
    // resolution renders into a corner of them) size even the first network, so every later size runs in it; else, while
    // the size moves, kDrsCapacity.
    if (!share) {
        const double px = double(w) * double(h);
        if (size_hint_px > uint64_t(px * 1.02)) config.capacity = std::min(4.0, double(size_hint_px) / px * 1.02);
        else if (capacity_miss || drs_active()) config.capacity = kDrsCapacity;
    }
    const TemporalConfig temporal = make_temporal();
    const HostDevice host = make_host();
    // Off the render thread. Building reads the weights, creates every pipeline
    // and allocates the arena, and doing that inside the upscaler's call froze
    // the game for the duration every time the feature was first turned on. The
    // queue lock is a mutex and is taken from this thread exactly as it would be
    // from the render thread; the game keeps rendering unenhanced frames until
    // the build is adopted above.
    const QueueAccess access_copy = access;
    build_share.reset();
    for (const auto& p : parked) if (p.core == share) build_share = share;
    building = true; build_w = w; build_h = h; build_format = format;
    build_started = std::chrono::steady_clock::now();
    nr::g_build_stage = 1; nr::g_build_pipes_done = 0; nr::g_build_pipes_total = 0;
    build_layer = call_layer;
    build_net = net_want;
    build_reuse = config.reuse;
    status = "building the network in the background; frames pass through until it is ready";
    events::note(events::kNetwork, "network asked for: %s", nr::network_codename(net_want));
    events::note(events::kNetwork, "building the network at %ux%u in the background", w, h);
    build_thread = std::thread([this, host, config, temporal, access_copy] {
        nr::pe::watchdog::Phase phase("building the network (background)");
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Runtime> made;
        std::string error;
        bool oom = false;
        try {
            // The queue lock around each submit of the build, not around the
            // build: held for the whole of it, the pipeline compile (7.75 s cold
            // at 4K, S.T.A.L.K.E.R. 2) blocked vkd3d's own submissions and
            // presents on the shared queue, and the game froze for as long
            // (an 8.4 s frame). **Quiet**: this is not the render thread, and
            // the flush half of the other lock drives DXVK's immediate context,
            // which that thread owns. See QueueAccess::lock_quiet.
            if (test_fault("net_build")) throw std::runtime_error("NR_TEST_FAULT net_build");
            if (test_fault("net_oom")) throw std::bad_alloc();   // (the retried kind: 4, 8, 16, 32 s)
            HostDevice locked = host;
            locked.queue_lock = access_copy.lock_quiet;
            locked.queue_unlock = access_copy.unlock_quiet;
            // a parked network whose weights went to RAM (Residency offload): back first
            if (config.share && network_offloaded(*config.share)) {
                const auto r0 = std::chrono::steady_clock::now();
                nr::vram::Scope owner(nr::vram::kUnset, config_net(config));
                if (!reload_network(locked, *config.share, config.act_arena, config.act_slack))
                    throw std::runtime_error("the network's weights could not be brought back from RAM");
                log("[nr] network weights back from RAM in %.0f ms",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count());
            }
#if NR_INT4
            try {   // an int4 mixed build that fails (its weights file damaged, from other data...) gives the default one
#endif
            made = [&] {   // its memory under its network in NR Control's VRAM breakdown
                nr::vram::Scope owner(nr::vram::kUnset, config_net(config));
                return std::make_unique<Runtime>(locked, config, ControlMaskConfig{}, temporal);
            }();
#if NR_INT4
            } catch (const std::bad_alloc&) {
                throw;   // host memory: the whole build is retried later, int4 mixed still requested
            } catch (const std::exception& e) {
                if (config.int4_mixed != 1) throw;
                log("[nr] int4 mixed network failed: %s; building the default network", e.what());
                RuntimeConfig fallback = config;
                fallback.int4_mixed = 0;
                made = [&] {   // its memory under its network in NR Control's VRAM breakdown
                nr::vram::Scope owner(nr::vram::kUnset, config_net(fallback));
                return std::make_unique<Runtime>(locked, fallback, ControlMaskConfig{}, temporal);
            }();
            }
#endif
        } catch (const std::bad_alloc& e) {
            // Caught apart from everything else because it is the one failure
            // that is about the *host* and is worth retrying: operator new, or
            // Vulkan's own host-memory / mapping failures (nrvk::HostMemoryError).
            // For a plain bad_alloc `what()` is the useless "std::bad_alloc";
            // what a reader needs is how much address space was left, and the
            // build's own log lines above say what it had just asked for.
            oom = true;
            error = std::string("neural rendering unavailable: out of host memory (") + e.what() + "). " +
                    address_space_report();
        } catch (const std::exception& e) {
            error = std::string("neural rendering unavailable: ") + e.what();
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> guard(build_lock);
        built = std::move(made); build_error = error; build_seconds = seconds; build_done = true;
        build_oom = oom;
    });
    if (net_only && runtime != nullptr) { touch_running(); return true; }   // a network switch: the running one serves until the new one is in
    return false;
}

RuntimeConfig Session::Impl::make_config(uint32_t w, uint32_t h, VkFormat f) const {
    RuntimeConfig config;
    config.root = root; config.width = w; config.height = h; config.colour_format = f;
    config.motion_point = motion_point;
    config.reuse = reuse_want;
    // RuntimeConfig::network: 1 the small model, 2 CC_SMALL_FP8_EXPERIMENTAL, 0 the 310.8 one
    config.network = net_want == nr::kSmallFp8 ? 1 : net_want == nr::kSmallFp8Experimental ? 2 : 0;
#if NR_INT4
    config.int4_mixed = net_want == nr::kSilverAardwoldInt4 ? 1 : 0;
#endif
    return config;
}

HostDevice Session::Impl::make_host() const {
    HostDevice host{};
    host.instance = handles.instance; host.physical = handles.physical; host.device = handles.device;
    // A real queue and its real family. Construction submits the weight upload
    // and waits for it; the game is submitting to the same underlying VkQueue,
    // so each of those submits takes the queue lock (the build thread's).
    host.queue = access.queue; host.queue_family = access.family;
    return host;
}

// The cached plan that runs a frame of w x h: its working extent, tall enough. Null: none yet.
const Session::Impl::PlanEntry* Session::Impl::plan_for(uint32_t w, uint32_t h, VkFormat f, NetworkModel model) {
    uint32_t ww = 0, wh = 0;
    bool known = false;
    for (auto& p : plans) {
        if (p.format != f || p.net != net_want || h > p.sh) continue;
        if (!known) { working_extent(model, 1.0f, w, h, &ww, &wh); known = true; }
        if (p.ww == ww && p.wh == wh) { p.used = ++use_stamp; return &p; }
    }
    return nullptr;
}

// A frame at a size no resident runtime serves, run at once: a runtime built inside this recording from a resident
// network of its working extent (Runtime::share_for) or from a cached plan in a resident network's memory
// (Runtime::with_slot_of) - the memory least recently used first, so OptiScaler's NR before and after the upscaler
// keep a memory each. False: nothing fits; the caller builds in the background.
bool Session::Impl::inframe(uint32_t w, uint32_t h, VkFormat f, std::shared_ptr<const RuntimeCore>* tried) {
    // This configuration's resident networks, least recently used first.
    std::vector<Built*> nets;
    for (auto& e : cache)
        if (same_config(e, f) && !e.runtime->stale()) nets.push_back(&e);
    // none for this colour format: a network of another format serves it (the runtime checks it can - the format only
    // sizes its images), as a game switching its colour buffer's format at a camera cut asks
    if (nets.empty())
        for (auto& e : cache)
            if (e.format != f && same_config_but_format(e) && !e.runtime->stale()) nets.push_back(&e);
    if (nets.empty()) return false;
    // The memory this feature ran in last first, then the least recently used.
    const auto pref = feature_slot.find(current_feature);
    const void* want = pref == feature_slot.end() ? nullptr : pref->second;
    std::sort(nets.begin(), nets.end(), [want](const Built* a, const Built* b) {
        const bool pa = a->runtime->network_id() == want, pb = b->runtime->network_id() == want;
        return pa != pb ? pa : a->used < b->used;
    });
    std::shared_ptr<const RuntimeCore> core;
    Built* memory = nullptr;   // the resident network whose memory `core` runs in
    for (Built* e : nets)
        if ((core = e->runtime->share_for(w, h))) { memory = e; break; }
    if (!core)
        if (const PlanEntry* p = plan_for(w, h, f, nets.front()->runtime->network()))
            for (Built* e : nets)
                if ((core = e->runtime->with_slot_of(p->core))) { memory = e; break; }
    if (!core) return false;
    // No heap in that memory yet: a couple of runtimes may take memory of their own meanwhile; then the frames pass
    // through until the heap is made (heap_poll), rather than each new size holding its images until retired.
    if (!image_heap_fits(*core, w, h)) {
        size_t own = 0;
        for (const auto& c : cache) own += c.light && c.runtime->owns_image_memory();
        for (const auto& r : retired) own += r.runtime && r.runtime->owns_image_memory();
        if (own >= kOwnImages) {
            inframe_waiting = true;
            heap_ask = memory->runtime->network_id();
            return false;
        }
    }
    RuntimeConfig config = make_config(w, h, f);
    config.share = core;
    config.deferred = true;
    std::unique_ptr<Runtime> made;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        made = [&] {   // its memory under its network in NR Control's VRAM breakdown
                nr::vram::Scope owner(nr::vram::kUnset, config_net(config));
                return std::make_unique<Runtime>(make_host(), config, ControlMaskConfig{}, make_temporal());
            }();
    } catch (const std::exception& e) {
        // once per extent, format and reason: the frames ask again until the background build is there
        static std::set<std::string> said;
        if (said.insert(std::to_string(w) + "x" + std::to_string(h) + ":" + std::to_string(int(f)) + ":" + e.what()).second)
            log("[nr] %ux%u could not be built inside the frame (%s); building it in the background", w, h, e.what());
        *tried = core;
        return false;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    inframe_ms_max = std::max(inframe_ms_max, ms);
    if (++inframe_built <= 8 || inframe_built % 100 == 0)
        log("[nr] %ux%u: runtime built inside the frame in %.2f ms (%llu so far, slowest %.2f ms)%s", w, h, ms,
            (unsigned long long)inframe_built, inframe_ms_max, inframe_built == 8 ? "; logged every 100th from here" : "");
    Built entry;
    entry.width = w; entry.height = h; entry.format = f;
    entry.net = net_of(*made);
    entry.reuse = reuse_want;
    entry.used = ++use_stamp;
    entry.last = std::chrono::steady_clock::now();
    entry.light = true;
    entry.runtime = std::move(made);
    trim_cache(&entry);
    cache.push_back(std::move(entry));
    auto& adopted = cache.back();
    runtime = adopted.runtime.get();
    width = adopted.width; height = adopted.height; this->format = adopted.format;
    running_net = adopted.net;
    return true;
}

bool Session::Impl::start_plan(uint32_t w, uint32_t h, VkFormat f) {
    const Built* net = nullptr;
    for (const auto& e : cache)
        if (same_config(e, f) && !e.runtime->stale()) { net = &e; break; }
    if (!net) return false;
    const NetworkModel model = net->runtime->network();
    if (plan_for(w, h, f, model)) return false;   // there is one: no memory holds it (a whole build)
    RuntimeConfig config = make_config(w, h, f);
    config.plan_only = true;
    const TemporalConfig temporal = make_temporal();
    const HostDevice host = make_host();
    building = true; build_plan = true;
    build_w = w; build_h = h; build_format = f;
    build_layer = call_layer;
    build_net = net_want;
    build_started = std::chrono::steady_clock::now();
    build_thread = std::thread([this, host, config, temporal, model]() mutable {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Runtime> made;
        std::string error;
        try {
            // lowered at the tallest frame of this working extent: it runs every height of it
            config.height = plan_height(model, 1.0f, config.width, config.height);
            made = [&] {   // its memory under its network in NR Control's VRAM breakdown
                nr::vram::Scope owner(nr::vram::kUnset, config_net(config));
                return std::make_unique<Runtime>(host, config, ControlMaskConfig{}, temporal);
            }();
        } catch (const std::exception& e) {
            error = e.what();
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> guard(build_lock);
        built = std::move(made); build_error = error; build_seconds = seconds; build_done = true; build_oom = false;
    });
    return true;
}

void Session::Impl::prefetch() {
    if (prefetched_around == std::make_pair(last_w, last_h)) return;
    const Built* net = nullptr;
    for (const auto& e : cache)
        if (same_config(e, last_format) && !e.runtime->stale()) { net = &e; break; }
    if (!net || plans.size() >= kMaxPlans) return;
    // The working extents planned already for this configuration.
    std::vector<std::pair<uint32_t, uint32_t>> covered;
    for (const auto& p : plans)
        if (p.format == last_format && p.net == net_want)
            covered.push_back({p.ww, p.wh});
    for (const auto& e : cache)
        if (same_config(e, last_format)) {
            uint32_t ww = 0, wh = 0;
            if (const auto c = e.runtime->plan()) { uint32_t sh = 0; plan_extent(*c, &ww, &wh, &sh); covered.push_back({ww, wh}); }
        }
    const NetworkModel model = net->runtime->network();
    RuntimeConfig config = make_config(last_w, last_h, last_format);
    config.plan_only = true;
    const TemporalConfig temporal = make_temporal();
    const HostDevice host = make_host();
    building = true; build_plan = true;
    build_w = last_w; build_h = last_h; build_format = last_format;
    build_layer = call_layer;
    build_net = net_want;
    build_started = std::chrono::steady_clock::now();
    const std::pair<uint32_t, uint32_t> around{last_w, last_h};
    build_thread = std::thread([this, host, config, temporal, model, covered, around]() mutable {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Runtime> made;
        std::string error;
        try {
            // The nearest working extent not planned, along the frame's aspect, within half to one and a half its size.
            const float scale = 1.0f;
            const double aspect = double(around.first) / double(around.second);
            for (uint32_t step = 8; step <= around.second / 2 && !made; step += 8)
                for (int sign : {-1, 1}) {
                    const int64_t hh = int64_t(around.second) + sign * int64_t(step);
                    const uint32_t ch = uint32_t(hh), cw = uint32_t(std::lround(double(hh) * aspect));
                    if (hh < 64 || cw < 64) continue;
                    uint32_t ww = 0, wh = 0;
                    working_extent(model, scale, cw, ch, &ww, &wh);
                    if (std::find(covered.begin(), covered.end(), std::make_pair(ww, wh)) != covered.end()) continue;
                    config.width = cw;
                    config.height = plan_height(model, scale, cw, ch);
                    made = [&] {   // its memory under its network in NR Control's VRAM breakdown
                nr::vram::Scope owner(nr::vram::kUnset, config_net(config));
                return std::make_unique<Runtime>(host, config, ControlMaskConfig{}, temporal);
            }();
                    break;
                }
        } catch (const std::exception& e) {
            error = e.what();
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> guard(build_lock);
        if (!made && error.empty()) error = "nothing left to plan around this size";
        built = std::move(made); build_error = error; build_seconds = seconds; build_done = true; build_oom = false;
    });
}

// The image heap for the network memory the running runtime is in: asked for in the background when the frame size
// moves (Runtime::image_heap_wanted), handed over here when made.
void Session::Impl::heap_poll() {
    {
        std::lock_guard<std::mutex> guard(heap_lock);
        if (heap_done) {
            heap_done = false; heap_busy = false;
            let_end(heap_thread);
            for (auto& e : cache)
                if (e.runtime->network_id() == heap_for) { e.runtime->adopt_image_heap(heap_made); break; }
            if (!heap_made) log("[nr] the image heap for the changing frame size could not be made");
            heap_made.reset();
        }
        if (heap_busy) return;
    }
    // the memory a waiting frame asked for (inframe), else the running one's while the size moves
    const Runtime* target = nullptr;
    if (heap_ask) {
        for (auto& e : cache)
            if (e.runtime->network_id() == heap_ask) { target = e.runtime.get(); break; }
        heap_ask = nullptr;
    }
    if (!target && runtime && drs_active()) target = runtime;
    if (!target) return;
    const uint64_t region = target->image_heap_wanted();
    if (!region) return;
    heap_busy = true; heap_for = target->network_id();
    const HostDevice host = make_host();
    heap_thread = std::thread([this, host, region, net = running_net] {
        nr::vram::Scope owner(nr::vram::kUnset, net);
        auto made = allocate_image_heap(host, region);
        std::lock_guard<std::mutex> guard(heap_lock);
        heap_made = std::move(made); heap_done = true;
    });
}

// [Network] Resize History = 1: the feature's history from the runtime at its previous size, scaled into the one it
// runs on now, instead of starting afresh (Runtime::carry_history). False: a fresh start (Reset), as NVIDIA's DLL.
bool Session::Impl::carry(VkCommandBuffer cmd, uint64_t feature) {
    const auto now = std::chrono::steady_clock::now();
    if (now - resize_history_read > std::chrono::seconds(1)) {
        resize_history_read = now;
        std::string v = nr::ini_network_key(root + "\\dlssnr-amd.ini", "resize history");
        for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        const bool on = v == "1" || v == "true" || v == "on" || v == "yes";
        if (on != resize_history)
            log("[nr] Resize History = %d: a feature at a new size %s", on ? 1 : 0,
                on ? "keeps its history, scaled to the new size" : "starts its history afresh (as NVIDIA)");
        resize_history = on;
    }
    if (!resize_history || !runtime || !feature) return false;
    const auto it = feature_prev_runtime.find(feature);
    if (it == feature_prev_runtime.end() || !it->second || it->second == runtime) return false;
    const Runtime* prev = it->second;
    bool alive = false;   // still ours: resident, or retired and not yet freed
    for (const auto& e : cache) alive |= e.runtime.get() == prev;
    for (const auto& r : retired) alive |= r.runtime.get() == prev;
    if (!alive) return false;
    const bool ok = runtime->carry_history(cmd, *prev, feature);
    if (ok && ++carried <= 4)
        log("[nr] feature #%u: history carried over to the new size (Resize History = 1)%s", unsigned(feature),
            carried == 4 ? "; not logged again" : "");
    return ok;
}

// What can run in this run, from the files installed, this build and the game's device; a UI shows the reason
// beside a network that cannot, and the hotkey skips it. A build that falls back later says more (network_note).
void Session::Impl::check_availability() {
    if (!handles.device) return;
    availability_checked = true;
    auto exists = [&](const char* rel) {
        const DWORD a = GetFileAttributesA((root + "\\" + rel).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!exists("dlssnr-amd\\dlssnr-small.bin"))
        net_switch.unavailable(nr::kSmallFp8, "needs the model of the 310.9.16 nvngx_dlssnr.dll (install.sh --dll)");
    if (!exists("dlssnr-amd\\dlssnr.bin"))
        net_switch.unavailable(nr::kSilverAardwold, "the model file is missing (run install.sh again)");
    if (!exists("dlssnr-amd\\experimental\\shaders\\fixed-attention.txt"))
        net_switch.unavailable(nr::kSmallFp8Experimental, "not installed (run install.sh again)");
    else if (!exists("dlssnr-amd\\dlssnr-small-experimental.bin"))
        net_switch.unavailable(nr::kSmallFp8Experimental, "needs the model of the 310.9.16 nvngx_dlssnr.dll (install.sh --dll)");
#if NR_INT4
    if (!exists("dlssnr-amd\\dlssnr.bin"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "the model file is missing (run install.sh again)");
    else if (!exists("dlssnr-amd\\int4\\settings.txt"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "not installed (run install.sh again)");
    else if (!exists("dlssnr-amd\\dlssnr-int4.bin"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "its weights were not made at install (run install.sh again)");
    else if (!vkGetDeviceProcAddr(handles.device, "vkCreatePipelineBinariesKHR"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "start the game with the INT4 launch option install.sh printed");
#else
    net_switch.unavailable(nr::kSilverAardwoldInt4, "not in this build");
#endif
}

void Session::Impl::release_output() {
    if (output) { output->Release(); output = nullptr; }
    output_vk = ResourceHandle{};
}

void Session::Impl::release_invalidator() {
    if (invalidator_gpu_heap) { invalidator_gpu_heap->Release(); invalidator_gpu_heap = nullptr; }
    if (invalidator_cpu_heap) { invalidator_cpu_heap->Release(); invalidator_cpu_heap = nullptr; }
    if (invalidator) { invalidator->Release(); invalidator = nullptr; }
}

// A 2x1 R32_UINT texture and the two descriptors a ClearUAV needs. Two texels
// because the clear below asks for ONE of them: see forget_bound_pipeline.
bool Session::Impl::ensure_invalidator(ID3D12Device* device) {
    if (invalidator && invalidator_cpu_heap && invalidator_gpu_heap) return true;
    if (invalidator_failed || !device) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&invalidator));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&invalidator_cpu_heap));
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&invalidator_gpu_heap));
    if (FAILED(hr)) {
        release_invalidator();
        invalidator_failed = true;
        log("[nr] could not build the pipeline invalidator (0x%08lX); a second neural pass in one "
            "command list will leave the caller's next dispatch bound to our kernel",
            (unsigned long)hr);
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(invalidator, nullptr, &uav,
                                      invalidator_cpu_heap->GetCPUDescriptorHandleForHeapStart());
    device->CreateUnorderedAccessView(invalidator, nullptr, &uav,
                                      invalidator_gpu_heap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

// **Why a ClearUAV of two texels is the fix for multipass.** Measured with
// a D3D12 test harness, which drives our _nvngx.dll the way
// OptiScaler does, with OptiScaler's own codec shader between and after the
// evaluates, and reads every surface back:
//
//   one pass  - the caller's dispatch after the evaluate is correct.
//   two passes- the clamp (after the FIRST evaluate) is correct; every dispatch
//               after the SECOND evaluate runs our last network kernel instead
//               of the caller's shader. Its target is never written, so the
//               frame keeps whatever it held: the unedited picture. That is the
//               NR-off picture, exactly as reported in the game.
//
// The cause is in vkd3d-proton (libs/vkd3d/command.c). It tracks TWO pipelines:
// `state`/`current_pipeline`, the D3D12-level one, and `command_buffer_pipeline`,
// what it believes is really bound in the VkCommandBuffer. Our
// SetPipelineState(nullptr) below clears the first, so the caller's next
// SetPipelineState is not deduplicated at the D3D12 level - but the bind itself
// is still skipped, because the recomputed VkPipeline equals
// `command_buffer_pipeline`. The first dispatch after a recording escapes only
// because that caller pipeline had never been bound in this command buffer yet.
//
// vkd3d has exactly one mechanism for "something bound its own pipeline behind
// my back": d3d12_command_list_invalidate_current_pipeline(list, meta_shader=true),
// which sets `command_buffer_pipeline = VK_NULL_HANDLE` - "just pretend we never
// bound anything". It runs when vkd3d dispatches one of its own meta shaders, and
// (newer vkd3d-proton) in ID3D12DXVKInteropDevice1::BeginVkCommandBufferInterop with
// all the list's other bindings - the way Session::set_keep_caller_state records. ClearUnorderedAccessViewUint takes that path whenever
// its fast path cannot serve the clear, and a rect covering PART of the image is
// refused by the fast path (d3d12_command_list_clear_uav_builtin: "Accept a
// single full-subresource rect"). So: two texels, clear one.
//
// It costs one tiny meta dispatch per evaluate and touches no state the caller
// can observe - unlike ClearState, which would do the same job by also throwing
// away the caller's render targets and bindings.
void Session::Impl::forget_bound_pipeline(ID3D12Device* device, ID3D12GraphicsCommandList* list) {
    if (!ensure_invalidator(device)) return;
    const UINT zero[4] = {0, 0, 0, 0};
    const D3D12_RECT rect{0, 0, 1, 1};   // half of a 2x1 image: the fast path must refuse it
    list->ClearUnorderedAccessViewUint(invalidator_gpu_heap->GetGPUDescriptorHandleForHeapStart(),
                                       invalidator_cpu_heap->GetCPUDescriptorHandleForHeapStart(),
                                       invalidator, zero, 1, &rect);
}

bool Session::Impl::ensure_output(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT dxgi) {
    if (output) {
        const auto desc = output->GetDesc();
        if (desc.Width == w && desc.Height == h && desc.Format == dxgi) return true;
        release_output();
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = dxgi; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (test_fault("d12_resource") ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               __uuidof(ID3D12Resource),
                                               reinterpret_cast<void**>(&output)))) {
        status = "could not allocate the render-resolution output";
        return false;
    }
    output_vk = colour_handle(device, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!output_vk.usable()) {
        release_output();
        status = "the output has no Vulkan image; this is not vkd3d-proton";
        return false;
    }
    return true;
}

namespace {

// One place turns a D3D12 resource into the ColourFrame the runtime wants, so
// colour, motion and depth cannot drift apart in layout handling.
bool fill(ColourFrame* out, const ResourceHandle& handle) {
    if (!handle.usable()) return false;
    out->image = handle.image;
    out->format = handle.format;
    out->width = handle.width; out->height = handle.height;
    out->before = out->after = handle.layout;
    out->usage = handle.usage;
    return true;
}

// Narrow a guide to the part of it the game rendered into: its subrect's base and extent, which the
// runtime reads from (EngineFrame::motion_x/depth_x and the narrowed width/height) - its blits take
// the region as their source rectangle, the in-place motion sample maps uv into it. A base outside
// the texture keeps the whole allocation (logged once).
void apply_guide_subrect(ColourFrame* guide, const Session::Subrect& want, const char* what,
                         uint32_t* base_x, uint32_t* base_y) {
    if (!guide->image) return;
    const uint32_t aw = guide->width, ah = guide->height;
    const int which = what[0] == 'd' ? 0 : 1;
    if (want.x >= aw || want.y >= ah) {
        static bool outside[2] = {false, false};
        if (!outside[which]) {
            outside[which] = true;
            log("[nr] the %s guide's subrect starts at (%u,%u), outside its %ux%u texture; the whole texture is used",
                what, want.x, want.y, aw, ah);
        }
        return;
    }
    // The region the game rendered into: from the base, the given extent (or the
    // rest of the texture), clipped to the texture - NVIDIA's own reading of the
    // subrect (base and extent, nvngx_dlssnr.dll 0x180022278).
    const uint32_t w = want.width ? std::min(want.width, aw - want.x) : aw - want.x;
    const uint32_t h = want.height ? std::min(want.height, ah - want.y) : ah - want.y;
    *base_x = want.x; *base_y = want.y;
    // A depth region that is not the whole image is read through the runtime's
    // copy, not sampled in place. Motion keeps its usage: it is sampled in place
    // through the region's base and share of the allocation.
    if (which == 0 && (w < aw || h < ah)) guide->usage = 0;
    guide->width = w; guide->height = h;
    if (want.x || want.y) {
        static bool noted[2] = {false, false};
        if (!noted[which]) {
            noted[which] = true;
            log("[nr] the %s guide's region starts at (%u,%u): %ux%u of the %ux%u texture", what, want.x, want.y,
                w, h, aw, ah);
        }
    }
}

}  // namespace

bool Session::Impl::ensure_d3d12_device(ID3D12Device* device) {
    if (handles.valid() && access.valid()) return true;
    if (!handles.valid()) {
        handles = device_handles(device);
        if (!handles.valid()) {
            status = "no Vulkan device underneath; not running on vkd3d-proton";
            failed = true;
            log("[nr] %s", status.c_str());
            return false;
        }
    }
    if (!access.valid() && !d3d12_queue_access(device, &access, &owned_queue)) {
        status = "vkd3d-proton would not hand out a queue; the weights cannot be uploaded";
        failed = true;
        log("[nr] %s", status.c_str());
        return false;
    }
    return true;
}

Session::Session(std::string root) : impl_(std::make_unique<Impl>()) {
    impl_->root = root.empty() ? std::string(nr::pe::module_folder()) : std::move(root);
    // The runtime's own diagnostics - the build's phase breakdown above all -
    // went to a stdout a game does not have. Send them to the same file
    // everything else here writes to.
    nr::set_log_sink([](const char* line) { log("%s", line); });
    // 32-bit: the build logs the free address space at each phase.
    if (sizeof(void*) == 4) nr::g_memory_note = [] { return address_space_report(); };
    log("[nr] session root %s", impl_->root.c_str());
    // NR_INPUT_CHECK=1 (NR_INPUT_CHECK_DEFAULT for a build that has it on without asking):
    // score what the game hands the network and what it gets back; pictures in
    // dlssnr-amd-check\ beside the log. NR_INPUT_CHECK_PICTURES=n sets how many captures
    // are written as pictures (default 4).
    static const bool input_check = [] {
#ifdef NR_INPUT_CHECK_DEFAULT
        bool on = NR_INPUT_CHECK_DEFAULT != 0;
#else
        bool on = false;
#endif
        char v[16] = {};
        if (GetEnvironmentVariableA("NR_INPUT_CHECK", v, sizeof v) > 0) on = v[0] == '1';
        if (!on) return false;
        int pictures = 4;
        if (GetEnvironmentVariableA("NR_INPUT_CHECK_PICTURES", v, sizeof v) > 0) pictures = std::atoi(v);
        std::string folder = nr::pe::module_folder();
        folder += folder.empty() ? "dlssnr-amd-check" : "\\dlssnr-amd-check";
        if (!CreateDirectoryA(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) folder.clear();
        nr::set_input_check(true, folder, pictures);
        return true;
    }();
    (void)input_check;
}

void Session::set_motion_point(bool v) { impl_->motion_point = v; }
void Session::set_keep_caller_state(bool v) { impl_->keep_caller_state = v; }

VkCommandBuffer Session::Impl::begin_recording(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                                               Recording& rec) {
    rec.device = device;
    rec.list = list;
    if (keep_caller_state) {
        if (const VkCommandBuffer cmd = begin_interop_recording(device, list)) {
            rec.interop = true;
            return cmd;
        }
        static bool said = false;
        if (!said) {
            said = true;
            log("[nr] this vkd3d-proton has no interop begin/end (ID3D12DXVKInteropDevice1): the network's commands "
                "leave the command list's compute pipeline state and root signature nulled");
        }
    }
    return command_buffer(list);
}

void Session::Impl::leave_list(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Recording& rec) {
    if (rec.interop) return;   // vkd3d-proton re-emits the list's own bindings at its next draw or dispatch
    list->SetPipelineState(nullptr);
    list->SetComputeRootSignature(nullptr);
    forget_bound_pipeline(device, list);
}

void Session::set_call_network(int network) { impl_->call_net = network; }
void Session::reset_layer_timing(int layer) {
    Impl& s = *impl_;
    ++s.layer_gen[layer];
    s.stage_avg.erase(layer);
    s.outer_avg.erase(layer);
    s.outer_net_avg.erase(layer);
    s.outer_seen.erase(layer);
}

void Session::set_call_layer(int layer) {
    impl_->call_layer = layer;
    const auto a = impl_->stage_avg.find(layer);   // a run not timed by the RenoDX stage: the runtime's timing
    if (a != impl_->stage_avg.end()) a->second.renodx_timed = false;
}

void Session::set_history_strength(float v) {
    impl_->history_strength = v;
    for (auto& e : impl_->cache) e.runtime->set_history_strength(v);
}

void Session::log_totals() {
    static_assert(sizeof(impl_->ran_total) == 8, "counters");
    if (impl_->totals_logged) return;
    impl_->totals_logged = true;
    const uint64_t r = impl_->ran_total + impl_->ran, p = impl_->passed_total + impl_->passed;
    if (r + p)
        log("[nr] session: %llu evaluations ran the network, %llu passed the frame through", (unsigned long long)r,
            (unsigned long long)p);
}

Session::~Session() {
    log_totals();
    // A build still running owns nothing the session is about to destroy, but
    // it will hand back a runtime that must be destroyed too: wait for it.
    if (impl_->build_thread.joinable()) impl_->build_thread.join();
    if (impl_->heap_thread.joinable()) impl_->heap_thread.join();
    if (impl_->offload_thread.joinable()) impl_->offload_thread.join();
    for (std::thread& t : impl_->ending_threads) t.join();
    impl_->ending_threads.clear();
    impl_->heap_made.reset();
    impl_->built.reset();
    impl_->runtime = nullptr;
    impl_->cache.clear();
    impl_->retired.clear();
    impl_->release_output();
    impl_->release_invalidator();
    impl_->release_vk_output();
    if (impl_->stage_timing) vkDestroyQueryPool(impl_->handles.device, impl_->stage_timing, nullptr);
    if (impl_->outer_timing) vkDestroyQueryPool(impl_->handles.device, impl_->outer_timing, nullptr);
    // Screenshot readbacks: the worker may still be reading one it was handed (it finishes on its own).
    if (impl_->shot) impl_->shot_free(*impl_->shot);
    if (impl_->outside_shot) impl_->shot_free(*impl_->outside_shot);
    if (impl_->model_shot) impl_->shot_free(*impl_->model_shot);
    for (auto& d : impl_->diag)   // NR_DIAG_EXPOSURE: a slot still in flight stays allocated
        if (!d.pending || vkGetEventStatus(impl_->handles.device, d.done) == VK_EVENT_SET) impl_->diag_free(d);
    for (auto& sh : impl_->shots_out) {
        for (int i = 0; i < 500 && sh->handed && !sh->read.load(std::memory_order_acquire); ++i) Sleep(10);
        if (!sh->handed || sh->read.load(std::memory_order_acquire)) impl_->shot_free(*sh);
    }
    for (const auto& r : impl_->neutral_retired) {
        if (r.image) vkDestroyImage(impl_->handles.device, r.image, nullptr);
        if (r.memory) vkFreeMemory(impl_->handles.device, r.memory, nullptr);
    }
    {   // the wilsjo2 layers' surfaces and programs (the device is idle by now or never ran them)
        std::vector<uint64_t> features;
        for (const auto& e : impl_->w2) features.push_back(e.first);
        for (uint64_t f : features) impl_->wilsjo2_release(f);
        for (auto& r : impl_->w2_retired) impl_->w2_destroy(r.s);
        impl_->w2_retired.clear();
        impl_->w2_stage.reset();
    }
    if (impl_->neutral_mv) vkDestroyImage(impl_->handles.device, impl_->neutral_mv, nullptr);
    if (impl_->neutral_mv_memory) vkFreeMemory(impl_->handles.device, impl_->neutral_mv_memory, nullptr);
    if (impl_->pool) vkDestroyCommandPool(impl_->handles.device, impl_->pool, nullptr);
    if (impl_->raw11_pool) {   // nothing freed while the GPU may still run it (not done in 2 s: left as it is)
        if (vkWaitForFences(impl_->handles.device, Impl::kRaw11, impl_->raw11_fence, VK_TRUE, 2000000000ull) == VK_SUCCESS) {
            for (VkFence f : impl_->raw11_fence) if (f) vkDestroyFence(impl_->handles.device, f, nullptr);
            vkDestroyCommandPool(impl_->handles.device, impl_->raw11_pool, nullptr);
        }
    }
    if (impl_->owned_queue) impl_->owned_queue->Release();
    if (impl_->dxvk) impl_->dxvk->Release();
    if (impl_->own_fence) {
        // Nothing may be freed while the GPU is still reading it.
        impl_->wait_for_own_cmd();
        vkDestroyFence(impl_->handles.device, impl_->own_fence, nullptr);
    }
    impl_->release_slots();
}

bool Session::wait_idle() {
    nr::pe::watchdog::Phase phase("waiting for NR's GPU work (session wait_idle)");
    bool done = impl_->wait_for_own_cmd();
    for (auto& slot : impl_->slots) done = impl_->wait_slot(slot) && done;
    if (impl_->raw11_pool)   // the D3D10/11 raw recordings (HUD FIX and the compare tools on the ReShade route)
        done = vkWaitForFences(impl_->handles.device, Impl::kRaw11, impl_->raw11_fence, VK_TRUE, 2000000000ull) == VK_SUCCESS && done;
    return done;
}

const std::string& Session::status() const { return impl_->status; }

bool Session::building() const { return impl_->building; }
bool Session::failed() const { return impl_->failed; }

// The runtime that is actually running frames, not the one being built: during a
// rebuild the old one is still the one paying for the picture on screen.
bool Session::video_memory(uint64_t* usage, uint64_t* budget, uint64_t* heap) const {
    Impl::Vram v;
    if (!impl_->vram(v)) return false;
    *usage = v.usage; *budget = v.budget; *heap = v.heap;
    return true;
}

bool Session::capture_begin(VkCommandBuffer cmd, VkImage image, VkFormat format, uint32_t w, uint32_t h,
                            VkImageLayout layout, uint8_t hdr_mode, const char* path) {
    Impl& s = *impl_;
    s.shot_poll();
    if (s.outside_shot || s.shot || !cmd || !image || !screenshot::wants_final()) return false;
    if (!screenshot::readable(format)) {
        screenshot::fail("unsupported_present", "the frame's format cannot be read back");
        return false;
    }
    s.outside_shot = s.shot_prepare(w, h, format);
    if (!s.outside_shot) {
        screenshot::abandon("allocation_failed", "the readback buffers could not be allocated");
        return false;
    }
    Impl::Shot& sh = *s.outside_shot;
    sh.pair.hdr_mode = hdr_mode;
    sh.pair.linear_unit_nits = 80.0f;
    sh.pair.meta = std::string("path=") + (path ? path : "wilsjo2") + "\ncomparison=" +
                   (path && std::string(path) == "present" ? "final-picture-before-after-nr" : "frame-nr-runs-on-before-after-nr") +
                   "\nprocessing=wilsjo2\nwidth=" + std::to_string(w) + "\nheight=" + std::to_string(h) +
                   "\nsource_format=" + std::to_string(int(format)) + "\nhdr_mode=" + std::to_string(hdr_mode) + "\n";
    Impl::shot_copy(cmd, sh, image, layout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0);
    screenshot::recorded();
    return true;
}

void Session::capture_abort() {
    if (!impl_->outside_shot) return;
    impl_->shots_out.push_back(std::move(impl_->outside_shot));
    screenshot::abandon("run_failed", "the frame's run could not finish");
}

void Session::capture_end(VkCommandBuffer cmd, VkImage image, VkImageLayout layout) {
    Impl& s = *impl_;
    if (!s.outside_shot || !cmd || !image) return;
    std::unique_ptr<Impl::Shot> sh = std::move(s.outside_shot);
    Impl::shot_copy(cmd, *sh, image, layout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, sh->bytes);
    s.shot_recorded(cmd, std::move(sh));
}

void Session::discard_timing() { impl_->discard_timing(); }

void Session::outer_begin(VkCommandBuffer cmd) {
    Impl& s = *impl_;
    if (!s.outer_ready()) return;
    vkCmdResetQueryPool(cmd, s.outer_timing, s.outer_slot * Impl::kOuterQueries, Impl::kOuterQueries);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.outer_timing,
                        s.outer_slot * Impl::kOuterQueries);   // after the work before it
    s.outer_open = true;
    s.outer_passes = 0;
}

void Session::outer_end(VkCommandBuffer cmd) {
    Impl& s = *impl_;
    if (!s.outer_open) return;
    s.outer_open = false;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.outer_timing, s.outer_slot * Impl::kOuterQueries + 1);
    s.outer_slot_layer[s.outer_slot] = s.call_layer;
    s.outer_slot_gen[s.outer_slot] = s.layer_gen[s.call_layer];
    s.outer_slot_passes[s.outer_slot] = s.outer_passes;
    if (!s.outer_fresh(s.call_layer)) {   // runs again after a pause (or as another processing): its average anew
        s.outer_avg.erase(s.call_layer);
        s.outer_net_avg.erase(s.call_layer);
    }
    s.outer_seen[s.call_layer] = std::chrono::steady_clock::now();
    ++s.outer_recorded;
    s.outer_slot = (s.outer_slot + 1) % Impl::kOuterSlots;
    s.read_outer_timing();
}

wilsjo2::SpatialLayout Session::spatial_layout() const { return impl_->w2_spatial; }
bool Session::spatial_active() const { return impl_->w2_spatial_on; }
float Session::gpu_ms() const {
    if (const auto o = impl_->outer_avg.find(impl_->call_layer);
        o != impl_->outer_avg.end() && o->second > 0.0f && impl_->outer_fresh(impl_->call_layer))
        return o->second;
    const auto a = impl_->stage_avg.find(impl_->call_layer);
    if (a != impl_->stage_avg.end() && a->second.renodx_timed) return a->second.ms;
    if (impl_->call_layer > 0) return 0.0f;
    return impl_->runtime ? impl_->runtime->average_gpu_ms() : 0.0f;
}

bool Session::network_ms_whole_run() const {
    const auto o = impl_->outer_avg.find(impl_->call_layer);
    return o != impl_->outer_avg.end() && o->second > 0.0f && impl_->outer_fresh(impl_->call_layer) &&
           impl_->outer_net_avg.count(impl_->call_layer) != 0;
}

float Session::network_ms() const {
    // a layer timed by an outer run (wilsjo2): the sum of its network recordings in it, while its total is that run's
    if (const auto o = impl_->outer_avg.find(impl_->call_layer);
        o != impl_->outer_avg.end() && o->second > 0.0f && impl_->outer_fresh(impl_->call_layer)) {
        const auto n = impl_->outer_net_avg.find(impl_->call_layer);
        return n != impl_->outer_net_avg.end() ? n->second : 0.0f;
    }
    const auto a = impl_->stage_avg.find(impl_->call_layer);
    if (a != impl_->stage_avg.end() && a->second.renodx_timed) return a->second.net_ms;
    if (impl_->call_layer > 0) return 0.0f;
    return impl_->runtime ? impl_->runtime->average_network_ms() : 0.0f;
}

void Session::network_size(uint32_t* width, uint32_t* height) const {
    *width = impl_->runtime ? impl_->runtime->model_width() : 0;
    *height = impl_->runtime ? impl_->runtime->model_height() : 0;
}

Session::State Session::state() const {
    State s;
    const auto& m = *impl_;
    s.running = m.runtime != nullptr;
    if (m.runtime) {
        s.model_w = m.runtime->model_width(); s.model_h = m.runtime->model_height();
    }
    s.running_net = m.runtime ? m.running_net : -1;
    s.want_net = m.net_want;
    for (int n = 0; n < nr::kNetworkChoices; ++n) s.unavailable[n] = m.net_switch.reason(n);
    s.building_net = m.building && !m.build_plan ? m.build_net : -1;
    s.resizes = m.resizes;
    s.carried = m.carried;
    s.renodx_known = m.renodx_facts_known;
    s.renodx = m.renodx_facts;
    s.wilsjo2_fast_passes = m.w2_fast_passes; s.wilsjo2_fast_fallback = m.w2_fast_fallback; s.wilsjo2_motion = m.w2_motion;
    s.motion_without_depth = m.runtime ? m.runtime->motion_without_depth() : m.net_switch.file().motion_without_depth;
    s.building = m.building && !m.build_plan;   // a plan is no network to wait for
    s.passed = !m.last_ok;
    if (s.passed) s.status = m.status;
    if (m.building) {
        s.building_seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - m.build_started).count();
        s.stage = nr::g_build_stage;
        s.pipes_done = nr::g_build_pipes_done; s.pipes_total = nr::g_build_pipes_total;
    }
    return s;
}

void Session::set_network(int n) { impl_->net_switch.set(n); }

void Session::idle() {
    auto& m = *impl_;
    const auto now = std::chrono::steady_clock::now();
    if (m.last_call == std::chrono::steady_clock::time_point{} || now - m.last_call < Impl::kNrOff) return;
    if (!m.retired.empty()) {
        const size_t before = m.retired.size();
        m.retired.erase(std::remove_if(m.retired.begin(), m.retired.end(), [](const Impl::Retired& r) {
            return r.runtime->recordings_done();
        }), m.retired.end());
        if (m.retired.size() != before)
            events::note(events::kResidency, "evicted networks freed (no evaluation for %.0f s)", std::chrono::duration<double>(now - m.last_call).count());
    }
    if ((!m.cache.empty() || !m.parked.empty()) && now - m.residency_checked >= Impl::kResidencyCheck)
        m.residency(false, true);
}

uint64_t Session::create_feature() {
    const uint64_t id = impl_->next_feature++;
    log("[nr] feature #%u created", static_cast<unsigned>(id));
    return id;
}

void Session::release_feature(uint64_t feature) {
    if (!feature) return;
    // Every network this feature may have been evaluated against holds a
    // history for it. None of them is freed here; see Runtime::release_feature.
    for (auto& e : impl_->cache) e.runtime->release_feature(feature);
    impl_->feature_extent.erase(feature);
    // what it last ran against: a feature that is gone holds no network (make_room / park count these references)
    const auto forget = [&](uint64_t f) {
        impl_->feature_slot.erase(f);
        impl_->feature_runtime.erase(f);
        impl_->feature_prev_runtime.erase(f);
    };
    forget(feature);
    const auto stage = impl_->renodx.find(feature);
    if (stage != impl_->renodx.end()) {
        for (uint32_t k = 1; k < stage->second.stack; ++k) {
            impl_->release_everywhere(Impl::stack_feature(feature, k));
            forget(Impl::stack_feature(feature, k));
        }
        impl_->retire_stage(std::move(stage->second.stage));
        impl_->renodx.erase(stage);
    }
    impl_->wilsjo2_release(feature);
    log("[nr] feature #%u released", static_cast<unsigned>(feature));
}

// The pixels of the game's guides' allocation (the bigger of motion and depth), 0 without them: Impl::size_hint_px.
static uint64_t guide_pixels(const Session::EngineResources& r) {
    uint64_t px = 0;
    for (ID3D12Resource* g : {r.motion, r.depth})
        if (g) {
            const D3D12_RESOURCE_DESC d = g->GetDesc();
            px = std::max<uint64_t>(px, uint64_t(d.Width) * d.Height);
        }
    return px;
}

// A layer of a chain run through here on its own (the ReShade route's wilsjo2 layers): timed around its recording as an
// outer run (outer_begin / outer_end), so each layer shows its own time - not the runtime's average, which every layer
// on that runtime shares. Nothing when an outer run is open already (the .asi's wilsjo2 times its whole flow) or no
// layer is said (call_layer 0: the route's one layer, the runtime's own timing).
struct OwnOuterRun {
    Session& s;
    VkCommandBuffer cmd;
    bool on;
    OwnOuterRun(Session& session, VkCommandBuffer c, bool want) : s(session), cmd(c), on(want) { if (on) s.outer_begin(cmd); }
    ~OwnOuterRun() { if (on) s.outer_end(cmd); }
};

ID3D12Resource* Session::run(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                             const EngineResources& resources, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = resources.feature;   // which network memory a new frame size prefers (Impl::inframe)
    ID3D12Resource* const colour = resources.colour;
    const D3D12_RESOURCE_STATES colour_state = resources.colour_state;
    if (s.failed || !device || !list || !colour) return nullptr;
    s.status.clear();

    if (!s.ensure_d3d12_device(device)) return nullptr;
    const VkCommandBuffer cmd = command_buffer(list);
    if (!cmd) {
        s.status = "the command list exposes no Vulkan handle";
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return nullptr;
    }
    const auto colour_vk = colour_handle(device, colour, colour_state);
    if (!colour_vk.usable()) {
        s.status = describe_rejection(colour_vk, "colour");
        return nullptr;
    }
    if (resource_is_typeless(colour_vk) && !s.typeless_noted) {
        s.typeless_noted = true;
        log("[nr] the colour resource is typeless (DXGI format %u); read through vkd3d's "
            "representative format %u - if the picture is wrong, start here",
            unsigned(colour_vk.dxgi), unsigned(colour_vk.format));
    }
    const auto colour_desc = colour->GetDesc();
    if (!s.ensure_output(device, colour_vk.width, colour_vk.height, colour_desc.Format))
        return nullptr;

    // The network's extent is the *render* resolution, which changes with the
    // upscaler's quality preset. Rebuild rather than scale: this pass is only
    // cheap because it runs at exactly the size the game rendered.
    s.size_hint_px = guide_pixels(resources);
    if (!s.ensure_runtime(controls, colour_vk.width, colour_vk.height, colour_vk.format))
        return nullptr;

    // The game keeps using its own colour buffer, so the network works on our
    // copy and the upscaler is pointed at that instead.
    ColourFrame frame{};
    frame.image = s.output_vk.image;
    frame.format = colour_vk.format;
    frame.width = colour_vk.width; frame.height = colour_vk.height;
    frame.before = frame.after = VK_IMAGE_LAYOUT_GENERAL;

    EngineFrame engine{};
    engine.colour = frame;
    engine.feature = resources.feature;
    engine.reset = resources.reset;
    if (s.resized(resources.feature, frame.width, frame.height) && !s.carry(cmd, resources.feature)) engine.reset = true;
    engine.motion_scale_x = resources.motion_scale_x;
    engine.motion_scale_y = resources.motion_scale_y;
    engine.depth_inverted = resources.depth_inverted;
    const bool motion_vk = resources.motion &&
        fill(&engine.motion, resource_handle(device, resources.motion, resources.motion_state));
    if (motion_vk) {
        engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, resources.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (resources.depth &&
        fill(&engine.depth, resource_handle(device, resources.depth, resources.depth_state)))
        apply_guide_subrect(&engine.depth, resources.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);

    OwnOuterRun timing(*this, cmd, s.call_layer > 0 && !s.outer_open);
    try {
        // Seed our output with the game's colour, then enhance it in place.
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {colour_vk.width, colour_vk.height, 1};
        // The caller told us the D3D12 state; vkd3d told us the layout that
        // implies. Transition from it and put it back, or the game's next use
        // of its own buffer sees a layout it never asked for.
        VkImageMemoryBarrier in{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        in.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        in.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        in.oldLayout = colour_vk.layout; in.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        in.srcQueueFamilyIndex = in.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in.image = colour_vk.image;
        in.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier out = in;
        out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        out.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        out.oldLayout = VK_IMAGE_LAYOUT_GENERAL; out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        out.image = s.output_vk.image;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &in);
        vkCmdCopyImage(cmd, colour_vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s.output_vk.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        std::swap(in.oldLayout, in.newLayout);
        in.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        in.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &in);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &out);

        if (motion_vk) s.runtime->record_engine(cmd, engine, s.capture_controls(controls));
        else s.runtime->record(cmd, frame, s.capture_controls(controls), engine.reset);

    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return nullptr;
    }
    return s.output;
}

bool Session::run_after(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                        ID3D12Resource* output, D3D12_RESOURCE_STATES output_state,
                        const EngineResources& resources, const Controls& controls,
                        ID3D12Resource* input, D3D12_RESOURCE_STATES input_state) {
    auto& s = *impl_;
    s.current_feature = resources.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !device || !list || !output) return false;
    s.status.clear();
    if (!s.ensure_d3d12_device(device)) return false;
    Impl::Recording rec;
    const VkCommandBuffer cmd = s.begin_recording(device, list, rec);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, output, output_state);
    if (!target.usable()) { s.status = describe_rejection(target, "output"); return false; }
    s.size_hint_px = guide_pixels(resources);
    if (resources.wilsjo2) {   // a wilsjo2 layer (the ReShade route): its flow on the output, in place
        Controls net{};
        if (!s.wilsjo2_prepare(*resources.wilsjo2, resources.feature, target.width, target.height, target.format,
                               resources.motion != nullptr, controls, &net))
            return false;
        ColourFrame frame{};
        frame.image = target.image;
        frame.format = target.format;
        frame.width = target.width; frame.height = target.height;
        frame.before = frame.after = target.layout;
        frame.usage = target.usage;
        EngineFrame engine{};
        engine.colour = frame;
        engine.feature = resources.feature;
        engine.reset = resources.reset || s.resized(resources.feature, frame.width, frame.height);
        engine.motion_scale_x = resources.motion_scale_x;
        engine.motion_scale_y = resources.motion_scale_y;
        engine.depth_inverted = resources.depth_inverted;
        if (resources.motion && fill(&engine.motion, resource_handle(device, resources.motion, resources.motion_state))) {
            engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
            apply_guide_subrect(&engine.motion, resources.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
        }
        if (resources.depth && fill(&engine.depth, resource_handle(device, resources.depth, resources.depth_state)))
            apply_guide_subrect(&engine.depth, resources.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
        bool ran = false;
        try {
            OwnOuterRun timing(*this, cmd, s.call_layer > 0 && !s.outer_open);
            ran = s.record_wilsjo2(cmd, frame, engine, net, *resources.wilsjo2);
        } catch (const std::exception& e) {
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return false;
        }
        s.leave_list(device, list, rec);
        return ran;
    }
    if (!s.ensure_runtime(controls, target.width, target.height, target.format)) return false;

    // In place: the upscaler already wrote this and nobody is waiting for a
    // different resource, so there is nothing to rebind.
    ColourFrame frame{};
    frame.image = target.image;
    frame.format = target.format;
    frame.width = target.width; frame.height = target.height;
    frame.before = frame.after = target.layout;
    frame.usage = target.usage;

    EngineFrame engine{};
    engine.colour = frame;
    // A separate input: the network samples it in place and stores its answer
    // into the output, so the output needs no copy of the colour first. Only
    // the engine path with the model applied; anything else is the caller's
    // seed copy and the in-place run.
    if (input) {
        if (!resources.motion || !s.runtime || !s.runtime->takes_target(controls)) return false;
        const auto source = colour_handle(device, input, input_state);
        if (!source.usable() || source.format != target.format || source.width != target.width ||
            source.height != target.height || !(source.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
        engine.target = frame;
        engine.colour.image = source.image;
        engine.colour.before = engine.colour.after = source.layout;
        engine.colour.usage = source.usage;
    }
    engine.feature = resources.feature;
    engine.reset = resources.reset;
    engine.motion_scale_x = resources.motion_scale_x;
    engine.motion_scale_y = resources.motion_scale_y;
    engine.depth_inverted = resources.depth_inverted;
    const bool motion_vk = resources.motion &&
        fill(&engine.motion, resource_handle(device, resources.motion, resources.motion_state));
    if (motion_vk) {
        engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, resources.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (resources.depth &&
        fill(&engine.depth, resource_handle(device, resources.depth, resources.depth_state)))
        apply_guide_subrect(&engine.depth, resources.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
    if (input && !motion_vk) return false;   // the plain path works in place: the caller seeds
    if (s.resized(resources.feature, frame.width, frame.height) && !s.carry(cmd, resources.feature)) engine.reset = true;
    // Full memory barriers on either side of the network, because the caller's
    // barriers are vkd3d's translation of D3D12 resource states and know
    // nothing about ours. Its "UAV -> SRV" on the output waits for compute
    // shader writes; the network's last write into the output is a
    // vkCmdCopyImage - a TRANSFER-stage write - so a dispatch that follows
    // immediately (OptiScaler's clamp between two NR passes, its resolve) can
    // start before that copy has landed and reads what was there before: the
    // seed copy of the input. One pass survived only because other work
    // happened to sit between the two. The same in reverse for our reads.
    const auto everything = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = src_access; mb.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    try {
        OwnOuterRun timing(*this, cmd, s.call_layer > 0 && !s.outer_open);

        if (motion_vk) s.runtime->record_engine(cmd, engine, s.capture_controls(controls));
        else s.runtime->record(cmd, frame, s.capture_controls(controls), engine.reset);

    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }
    // The network was recorded as raw Vulkan into vkd3d-proton's command buffer:
    // it bound its own compute pipelines, descriptor sets and push constants.
    // vkd3d does not know, and it de-duplicates state on the D3D12 side -
    // SetPipelineState returns early when `list->state == state`, the root
    // signature likewise - so a caller that sets the SAME pipeline state
    // before and after us (OptiScaler's clamp between two NR passes, then its
    // resolve) never gets vkCmdBindPipeline re-emitted and dispatches our last
    // kernel instead of its own. NVIDIA's DLL never trips this because a cubin
    // launch (vkCmdCuLaunchKernelNVX) touches no Vulkan bind point. Setting the
    // compute bind point's tracked state to null makes the next real set differ
    // and vkd3d re-emits the pipeline, the descriptor offsets and the push
    // constants (SetComputeRootSignature -> invalidate_root_parameters). The
    // graphics bind point was not touched by us and is left alone.
    everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    // (leave_list: SetPipelineState(nullptr), SetComputeRootSignature(nullptr), and the part those two cannot
    // reach: vkd3d's memory of what is bound in the Vulkan command buffer itself. Without that, the second neural
    // pass in one command list leaves every following caller dispatch running our kernel. Through the interop
    // begin/end none of it is needed.)
    s.leave_list(device, list, rec);
    return true;
}

// The RenoDX-style option, shared by every route: the network at the frame's extent on RenoDX's proxy
// (RGBA16F, the DLL's own composition), ready or building in the background.
bool Session::Impl::renodx_runtime(const Controls& controls, uint32_t w, uint32_t h, Controls* c) {
    // The network sees RenoDX's proxy and answers into the stage's own image, one pass: what RenoDX's
    // feature 18 evaluate is. Preprocess runs inside RenoDX's processing (RenoDXFrame::preprocess), not here.
    *c = controls;
    c->preprocess = Preprocess{};
    c->passes = 1;
    // NR's switch is [RenoDX] NeuralUplift (the stage passes the frame through when it is off); the route's own
    // Enabled / Apply model do not apply.
    c->enabled = true;
    c->apply_model = true;
    const bool ok = ensure_runtime(*c, w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    return ok && runtime->takes_target(*c);   // the model not applied: the frame stays as it is
}

// The network's extent for this feature's frame (RenoDX's ComputeNrWorkingResolution + CommitNrResolution).
void Session::Impl::renodx_extent(uint64_t feature, uint32_t w, uint32_t h, uint32_t guide_w, uint32_t guide_h,
                                  const renodx::Settings& settings, uint32_t* nw, uint32_t* nh) {
    auto& e = renodx[feature];
    // Only changes in the game's input activate nearby-size prefetching. A user scale/follow change is
    // deliberate, so planning dozens of sizes around it consumes host memory without helping a later frame.
    const bool input_resize = e.scale == settings.resolution_scale && e.follow == settings.follow_input_res;
    struct Resize {
        Impl& s; const RenoDXEntry& e; uint32_t w, h; bool input_resize;
        ~Resize() { if (input_resize && w && (e.applied_w != w || e.applied_h != h))
            s.last_resize = std::chrono::steady_clock::now(); }
    } resize{*this, e, e.applied_w, e.applied_h, input_resize};
    renodx::working_resolution(settings, w, h, guide_w, guide_h, nw, nh);
    const bool settings_changed = e.scale != settings.resolution_scale || e.follow != settings.follow_input_res ||
                                  e.w != w || e.h != h;
    e.scale = settings.resolution_scale; e.follow = settings.follow_input_res;
    if (e.applied_w == 0 || settings_changed || (*nw == e.applied_w && *nh == e.applied_h)) {
        e.applied_w = *nw; e.applied_h = *nh;
        e.pending_frames = 0;
        return;
    }
    // The game's guides changed size: taken only once it has held.
    if (*nw != e.pending_w || *nh != e.pending_h) { e.pending_w = *nw; e.pending_h = *nh; e.pending_frames = 0; }
    if (++e.pending_frames >= renodx::kContractDebounceFrames) {
        e.applied_w = *nw; e.applied_h = *nh;
        e.pending_frames = 0;
        return;
    }
    *nw = e.applied_w; *nh = e.applied_h;
}

// This feature's stage at the frame's extent, the network's, format and HDR judgement (a new one, the old
// retired, when any of them changes), with this frame's settings.
renodx::Stage* Session::Impl::renodx_stage(uint64_t feature, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh,
                                           VkFormat format, const Session::RenoDXFrame& rf) {
    free_retired_stages();
    auto& e = renodx[feature];
    if (!e.stage || e.w != w || e.h != h || e.nw != nw || e.nh != nh || e.format != format || e.hdr != rf.hdr_flag ||
        e.present != rf.present) {
        retire_stage(std::move(e.stage));
        renodx::StageConfig sc;
        sc.shaders = root + "\\dlssnr-amd\\shaders\\renodx";
        sc.width = w; sc.height = h; sc.nr_width = nw; sc.nr_height = nh;
        sc.frame_format = format;
        sc.hdr_flag = rf.hdr_flag;
        sc.present = rf.present;
        sc.settings = rf.settings;   // the units it is made for
        HostDevice host{};
        host.instance = handles.instance; host.physical = handles.physical; host.device = handles.device;
        host.queue = access.queue; host.queue_family = access.family;
        try {
            e.stage = std::make_unique<renodx::Stage>(host, sc);
        } catch (const std::exception& x) {
            status = std::string("RenoDX-style processing unavailable: ") + x.what();
            log("[nr] %s", status.c_str());
            failed = true;
            return nullptr;
        }
        e.w = w; e.h = h; e.nw = nw; e.nh = nh; e.format = format; e.hdr = rf.hdr_flag; e.present = rf.present;
        log("[nr] feature #%u: RenoDX-style processing at %ux%u (network %ux%u), format %d, %s", unsigned(feature), w,
            h, nw, nh, int(format), rf.hdr_flag ? "HDR" : "not HDR");
    }
    try {
        e.stage->set(rf.settings);
    } catch (const std::exception&) {
        // Relative <-> absolute units (NRLinearUnitNits, NRSourceEncoding): a new stage next frame.
        retire_stage(std::move(e.stage));
        return nullptr;
    }
    return e.stage.get();
}

bool Session::Impl::neutral_motion(VkCommandBuffer cmd, uint32_t w, uint32_t h, ColourFrame* out) {
    ++neutral_calls;
    const auto now = std::chrono::steady_clock::now();
    for (auto i = neutral_retired.begin(); i != neutral_retired.end();) {
        if (neutral_calls - i->call < 64 || now - i->at < std::chrono::seconds(2) || !i->uses.ready()) { ++i; continue; }
        if (i->image) vkDestroyImage(handles.device, i->image, nullptr);
        if (i->memory) vkFreeMemory(handles.device, i->memory, nullptr);
        i = neutral_retired.erase(i);
    }
    if (!neutral_mv || neutral_w != w || neutral_h != h) {
        if (neutral_mv || neutral_mv_memory) neutral_retired.push_back({neutral_mv, neutral_mv_memory, neutral_uses, neutral_calls, now});
        neutral_mv = VK_NULL_HANDLE; neutral_mv_memory = VK_NULL_HANDLE; neutral_uses = {};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R16G16_SFLOAT; ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_STORAGE_BIT;
        if (vkCreateImage(handles.device, &ci, nullptr, &neutral_mv) != VK_SUCCESS) {
            status = "could not create the neutral motion vectors";
            return false;
        }
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(handles.device, neutral_mv, &req);
        VkPhysicalDeviceMemoryProperties mem{}; vkGetPhysicalDeviceMemoryProperties(handles.physical, &mem);
        uint32_t type = mem.memoryTypeCount;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i; break;
            }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        if (type == mem.memoryTypeCount || vkAllocateMemory(handles.device, &ai, nullptr, &neutral_mv_memory) != VK_SUCCESS ||
            vkBindImageMemory(handles.device, neutral_mv, neutral_mv_memory, 0) != VK_SUCCESS) {
            status = "could not back the neutral motion vectors";
            return false;
        }
        neutral_w = w; neutral_h = h; neutral_cleared = false;
    }
    neutral_uses.touch();
    if (!neutral_cleared) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.image = neutral_mv;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, neutral_mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        neutral_cleared = true;
    }
    *out = {};
    out->image = neutral_mv; out->format = VK_FORMAT_R16G16_SFLOAT; out->width = w; out->height = h;
    out->before = out->after = VK_IMAGE_LAYOUT_GENERAL;
    out->before_stage = out->after_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    out->before_access = out->after_access = VK_ACCESS_SHADER_READ_BIT;
    out->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return true;
}

// The stage around the network, `colour` read and `output` written (they may be one image). `engine` carries
// the motion and depth; its colour and target are the stage's.
bool Session::Impl::record_renodx(VkCommandBuffer cmd, renodx::Stage& stage, const ColourFrame& colour,
                                  const ColourFrame& output, EngineFrame engine, const Controls& c,
                                  const Session::RenoDXFrame& rf, const ColourFrame* exposure) {
    // The Present path without the game's motion vectors (NRPresentGuides Optional), or set to Never: neutral guides
    // for the network - zero motion, no depth - and none for the look (its Detail stability is the game's vectors').
    const bool neutral = rf.present && (rf.settings.present_guides == 2u || !engine.motion.image);
    ColourFrame look_motion = engine.motion;
    if (neutral) {
        if (!neutral_motion(cmd, colour.width, colour.height, &engine.motion)) return false;
        engine.motion_x = engine.motion_y = 0;
        engine.motion_texture_width = colour.width; engine.motion_texture_height = colour.height;
        engine.motion_scale_x = engine.motion_scale_y = 1.0f;
        engine.depth = ColourFrame{};
        look_motion = ColourFrame{};
    }
    renodx::StageFrame sf;
    sf.fast_motion_available = rf.fast_motion_available;
    sf.colour = colour;
    sf.output = output;
    sf.reset = engine.reset;
    sf.after_upscale = rf.after_upscale;
    sf.motion = look_motion;
    sf.motion_x = engine.motion_x; sf.motion_y = engine.motion_y;
    sf.motion_scale_x = engine.motion_scale_x; sf.motion_scale_y = engine.motion_scale_y;
    if (exposure) sf.exposure = *exposure;
    sf.pre_exposure = rf.pre_exposure;
    sf.exposure_scale = rf.exposure_scale;
    sf.auto_exposure = rf.auto_exposure;
    sf.create_flags_known = rf.create_flags_known;
    sf.preprocess = rf.preprocess;
    sf.present_white_nits = rf.present_white_nits;
    const renodx::Settings& rs = rf.settings;
    const uint32_t passes = renodx::stack_passes(rs);
    // A shorter stack than last frame: the passes it no longer runs give their histories back (a longer one starts
    // its new passes fresh, as RenoDX's own does).
    // Each pass on a history of its own. The ReShade routes run as feature 0, the runtime's own unnamed history,
    // which it neither saves nor restores around a named one: stacked there, pass 1 takes a name too (else passes 1
    // and 2 would read each other's history every frame).
    const uint64_t named = engine.feature ? engine.feature : kUnnamedStack;
    const auto pass_feature = [&](uint32_t k) { return engine.feature || passes > 1 ? stack_feature(named, k) : 0; };
    auto& entry = renodx[engine.feature];
    for (uint32_t k = passes; k < entry.stack; ++k) release_everywhere(stack_feature(named, k));
    if (!engine.feature && passes == 1 && entry.stack > 1) release_everywhere(named);
    entry.stack = passes;
    // RenoDX's guides: NRDepthMode overrides the game's depth direction, NRMVecScaleX / Y scale its motion vectors
    // (the network's and the look's).
    if (rs.depth_mode == 1u) engine.depth_inverted = false;
    if (rs.depth_mode == 2u) engine.depth_inverted = true;
    engine.motion_scale_x *= rs.mvec_scale_x;
    engine.motion_scale_y *= rs.mvec_scale_y;
    sf.motion_scale_x = engine.motion_scale_x; sf.motion_scale_y = engine.motion_scale_y;
    // Pass k's controls, RenoDX's (the route's own do not apply with the option on): pass 1's, or pass k's once its
    // "Same as pass 1" is off (PassModel; the intensity is always the pass's own, PassIntensity); with
    // NRSkinIndependent and the Character mask on, Structure at most 1 and a stock Skin sent as 1 (SentSkinSteering).
    const auto pass_controls = [&](uint32_t pass) {
        const uint32_t k = std::min(pass, 3u);   // passes 5.. (NRUnlockPasses) take pass 4's controls
        Controls ck = c;
        ck.per_pass.clear();
        ck.passes = 1;
        ck.history_strength = 1.f;
        const bool own = k > 0 && !rs.pass_follow[k - 1];
        ck.intensity = k == 0 ? rs.intensity : rs.pass_intensity[k - 1];
        ck.style = int(own ? rs.pass_style[k - 1] : rs.style);
        ck.local_tone = own ? rs.pass_local_tone[k - 1] : rs.local_tone;
        ck.local_structure = own ? rs.pass_local_structure[k - 1] : rs.local_structure;
        ck.skin_structure = own ? rs.pass_skin_structure[k - 1] : rs.skin_structure;
        ck.automatic_mask = own ? rs.pass_auto_mask[k - 1] : rs.auto_mask;
        ck.seed_fixed = !rs.noise_every_frame;
        if (rs.skin_independent && ck.automatic_mask) {
            ck.local_structure = std::min(ck.local_structure, 1.0f);
            if (ck.skin_structure < 0.0f) ck.skin_structure = 1.0f;
        }
        return ck;
    };
    const bool timed = stage_timing_ready();
    const uint32_t slot = stage_slot, base = slot * kStageQueries;
    uint32_t timed_passes = 0;
    // NR_DIAG_EXPOSURE: the game's exposure and the frame before NR (before the stage's own transitions).
    diag_begin(rs, rf, engine.reset);
    if (exposure)
        diag_copy(cmd, 0, exposure->image, exposure->format, 1, 1, exposure->usage,
                  exposure->before, exposure->before_stage, exposure->before_access);
    diag_copy(cmd, 1, colour.image, colour.format, colour.width, colour.height, colour.usage, colour.before,
              colour.before_stage, colour.before_access);
    // RenoDX's NR screenshot. The final picture before and after NR: the presented frame on the ReShade route,
    // OptiScaler's finished picture on its route (NR moved there for the capture, nr_pe_optifix.cpp; `clean`: no NR
    // ran on it inline). The model's input and output: the last pass's, after it ran.
    shot_poll();
    std::unique_ptr<Shot> capture;
    const bool final_frame = rf.present != 0 || rf.finished || rf.capture_at;
    if (!shot && rs.nr_enabled && screenshot::wants_final() && final_frame && rf.clean) {
        if (!screenshot::readable(colour.format)) {
            screenshot::fail("unsupported_present", "the frame's format cannot be read back");
        } else if ((colour.usage && !(colour.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
                   (output.usage && !(output.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))) {
            screenshot::fail("unsupported_present", "the frame cannot be copied (no transfer use)");
        } else if (!(capture = shot_prepare(colour.width, colour.height, colour.format))) {
            screenshot::abandon("allocation_failed", "the readback buffers could not be allocated");
        } else {
            // HDR stays HDR: PQ codes as they are, scRGB (1.0 = 80 nits) encoded to PQ; SDR as display codes. The
            // Present path knows the swap chain's encoding; OptiScaler's finished picture is the back buffer, where a
            // float format is scRGB (DXGI's only float encoding) and 10 bits are PQ when OptiScaler judged it HDR.
            const bool pq = colour.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                            colour.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            const bool linear = colour.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                colour.format == VK_FORMAT_R32G32B32A32_SFLOAT ||
                                colour.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            capture->pair.hdr_mode = rf.present ? (rf.present == 2 ? 2 : rf.present == 3 ? 1 : 0)
                                                : linear ? 1 : pq && rf.hdr_flag ? 2 : 0;
            capture->pair.linear_unit_nits = 80.0f;
            // A frame inside the game's pipeline (Vulkan / D3D11 games, the frame NR runs on): read as the stage reads
            // it. Relative linear light (before the game's tone mapping) has no nits: shown with 1.0 = 80 nits.
            const char* units = "";
            if (!rf.present && !rf.finished && rf.capture_at) {
                const renodx::FrameUnits u = renodx::frame_units(rf.settings, colour.format, rf.hdr_flag);
                capture->pair.hdr_mode = u.mode;
                if (u.mode == 1 && u.unit_nits > 0.f) capture->pair.linear_unit_nits = u.unit_nits;
                units = u.mode == 1 ? (u.unit_nits > 0.f ? "\nlinear_units=absolute" : "\nlinear_units=relative-shown-as-80-nits")
                                    : u.mode == 2 ? "\nlinear_units=pq" : "\nlinear_units=display-codes";
            }
            const bool mid = !rf.present && !rf.finished && rf.capture_at;
            capture->pair.meta = std::string("path=") + (rf.present ? "present" : rf.capture_at ? rf.capture_at : "optiscaler-finished-picture") +
                                 (mid ? "\ncomparison=frame-nr-runs-on-before-after-nr" : "\ncomparison=final-picture-before-after-nr") +
                                 units + "\nstack=" + std::to_string(passes) +
                                 "\nwidth=" + std::to_string(colour.width) + "\nheight=" + std::to_string(colour.height) +
                                 "\nsource_format=" + std::to_string(int(colour.format)) +
                                 "\nhdr_mode=" + std::to_string(capture->pair.hdr_mode) + "\n";
            shot_copy(cmd, *capture, colour.image, colour.before, colour.before_stage, colour.before_access, 0);
        }
    } else if (screenshot::armed() && !rs.nr_enabled) {
        screenshot::fail("nr_disabled", "enable NR before taking a comparison");
    }
    const bool model_capture = !shot && !capture && rs.nr_enabled && screenshot::wants_model();
    const renodx::Stage::Network network = [&](uint32_t k, const ColourFrame& proxy, const ColourFrame& neural, bool reset) {
        EngineFrame e = engine;
        e.colour = proxy;
        e.target = neural;
        e.reset = reset;
        e.feature = pass_feature(k);
        const bool time_net = timed && timed_passes < renodx::kMaxUnlockedPasses;
        Controls controls = pass_controls(k);
        if (time_net) controls.network_timestamp = [&](VkCommandBuffer c, bool begin) {
            vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stage_timing,
                                base + 2 + 2 * timed_passes + (begin ? 0u : 1u));
            if (!begin) ++timed_passes;
        };
        runtime->record_engine(cmd, e, controls);
        if (k == 0) {
            diag_copy(cmd, 2, proxy.image, proxy.format, proxy.width, proxy.height, proxy.usage, proxy.after,
                      proxy.after_stage, proxy.after_access);
            diag_copy(cmd, 3, neural.image, neural.format, neural.width, neural.height, neural.usage, neural.after,
                      neural.after_stage, neural.after_access);
        }
        // RenoDX's model diagnostic: the last pass's proxy and network output, sRGB previews (not the displayed frame).
        if (model_capture && k + 1 == passes && !shot && proxy.format == neural.format && proxy.width == neural.width &&
            proxy.height == neural.height && screenshot::readable(proxy.format)) {
            if (auto sh = shot_prepare(proxy.width, proxy.height, proxy.format)) {
                sh->pair.model_diagnostic = true;
                sh->pair.hdr_mode = 0;
                sh->pair.meta = std::string("path=") + (rf.present ? "present" : "optiscaler") +
                                "\ncomparison=model-pass-input-output\ndisplay_reference=0"
                                "\nsource=model-proxy-and-neural-output\npng=rgb8-srgb-clipped-preview"
                                "\nencoding=srgb\nunits=model-proxy-relative"
                                "\nstage=before-look-transfer-and-game-tonemap"
                                "\nexcluded=look,transfer,pedestal,game-upscale,game-tonemap,UI"
                                "\ninput_includes_previous_stack_passes=" + std::string(k ? "1" : "0") +
                                "\nstack_pass=" + std::to_string(k + 1) + "\nstack=" + std::to_string(passes) +
                                "\nwidth=" + std::to_string(proxy.width) + "\nheight=" + std::to_string(proxy.height) +
                                "\nsource_format=" + std::to_string(int(proxy.format)) + "\n";
                shot_copy(cmd, *sh, proxy.image, proxy.after, proxy.after_stage, proxy.after_access, 0);
                shot_copy(cmd, *sh, neural.image, neural.after, neural.after_stage, neural.after_access, sh->bytes);
                shot_recorded(cmd, std::move(sh));
            } else {
                screenshot::abandon("allocation_failed", "model diagnostic readback buffers could not be allocated");
            }
        }
    };
    if (timed) {
        vkCmdResetQueryPool(cmd, stage_timing, base, kStageQueries);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stage_timing, base);
    }
    try {
        stage.record(cmd, sf, network);
        renodx_facts = stage.facts();
        renodx_facts_known = true;
        diag_copy(cmd, 4, output.image, output.format, output.width, output.height, output.usage, output.after,
                  output.after_stage, output.after_access);
        diag_end(cmd);
        if (capture) {
            shot_copy(cmd, *capture, output.image, output.after, output.after_stage, output.after_access, capture->bytes);
            shot_recorded(cmd, std::move(capture));
        }
        if (timed) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stage_timing, base + 1);
            stage_slot_passes[slot] = timed_passes;
            stage_slot_layer[slot] = call_layer;
            stage_slot_gen[slot] = layer_gen[call_layer];
            ++stage_recorded;
            stage_slot = (slot + 1) % kStageSlots;
            read_stage_timing();
        }
        stage_avg[call_layer].renodx_timed = timed;
    } catch (const std::exception& x) {
        if (capture) {   // its pre-NR copy may be in the command buffer: kept until the session goes
            screenshot::abandon("failed", "RenoDX-style processing failed during the capture");
            shots_out.push_back(std::move(capture));
        }
        status = std::string("RenoDX-style processing failed: ") + x.what();
        failed = true;
        log("[nr] %s", status.c_str());
        return false;
    }
    return true;
}

// ---- wilsjo2 layers of the ReShade route (nr_wilsjo2_flow.hpp) ---------------------------------------------------------
namespace {
VkFormat w2_storage_twin(VkFormat f) {   // a format the stage can store into: an sRGB one's UNORM twin (same bytes)
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}
// The format the stage works a frame of format `f` in: `f` or its sRGB twin when the stage can store into it (sampled +
// storage); else a wider format holding every value of `f`, copied in and back by blits (`*blit`; 0.0.7's ReShade
// route went through such an intermediate with a blit, Transfer::DirectBlit) - when `f` can be blitted from and into.
// UNDEFINED: neither.
VkFormat w2_work_format(VkPhysicalDevice physical, VkFormat f, bool* blit) {
    const auto has = [physical](VkFormat x, VkFormatFeatureFlags need) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, x, &fp);
        return (fp.optimalTilingFeatures & need) == need;
    };
    const VkFormatFeatureFlags work = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    *blit = false;
    const VkFormat twin = w2_storage_twin(f);
    if (has(twin, work)) return twin;
    VkFormat wide = VK_FORMAT_R32G32B32A32_SFLOAT;   // any other colour format's values
    switch (f) {
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R16G16B16_SFLOAT:
            wide = VK_FORMAT_R16G16B16A16_SFLOAT;
            break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_A4R4G4B4_UNORM_PACK16: case VK_FORMAT_A4B4G4R4_UNORM_PACK16:
        case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_B8G8R8_UNORM:
            wide = VK_FORMAT_R8G8B8A8_UNORM;
            break;
        case VK_FORMAT_R16G16B16_UNORM:
            wide = VK_FORMAT_R16G16B16A16_UNORM;
            break;
        default:
            break;
    }
    if (!has(f, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !has(wide, work | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return VK_FORMAT_UNDEFINED;
    *blit = true;
    return wide;
}
bool w2_unorm(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_R16G16B16A16_UNORM:
            return true;
        default:
            return false;
    }
}
void w2_layout(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
void w2_everything(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
}
}  // namespace

Session::Impl::W2Surface* Session::Impl::w2_make(VkCommandBuffer cmd, VkFormat f, uint32_t w, uint32_t h) {
    if (!handles.device || !handles.physical || !f || !w || !h) return nullptr;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(handles.physical, f, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((fp.optimalTilingFeatures & need) != need) return nullptr;
    if (!w2_mem_known) { vkGetPhysicalDeviceMemoryProperties(handles.physical, &w2_mem); w2_mem_known = true; }
    auto* x = new W2Surface{};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(handles.device, &ii, nullptr, &x->image) != VK_SUCCESS) { x->image = VK_NULL_HANDLE; w2_destroy(x); return nullptr; }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(handles.device, x->image, &req);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < w2_mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (w2_mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    {
        nr::vram::Scope tag(nr::vram::kWilsjo2);
        if (type == ~0u || vkAllocateMemory(handles.device, &ai, nullptr, &x->memory) != VK_SUCCESS) {
            x->memory = VK_NULL_HANDLE;
            w2_destroy(x);
            return nullptr;
        }
    }
    if (vkBindImageMemory(handles.device, x->image, x->memory, 0) != VK_SUCCESS) { w2_destroy(x); return nullptr; }
    x->format = f; x->w = w; x->h = h;
    w2_layout(cmd, x->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return x;
}

namespace {
// The flow's route inside the session (the ReShade route's wilsjo2 layers): our surfaces GENERAL throughout, the frame
// moved to GENERAL for the run and back after, the guides read in their layouts, the stage on the session's device and
// the network through the session's runtime, network-only (Session::Impl::wilsjo2_runtime).
class SessionW2Io final : public nr::wilsjo2::FlowIo {
public:
    using Impl = Session::Impl;
    SessionW2Io(Impl& s, VkCommandBuffer cmd, const ColourFrame& target, const EngineFrame& engine, const Controls& net,
                const nr::wilsjo2::Settings& settings, uint64_t feature, bool estimate)
        : s_(s), cmd_(cmd), target_(target), engine_(engine), net_(net), settings_(settings), feature_(feature),
          estimate_(estimate) {
        target_now_ = target.before;
    }
    nr::wilsjo2::Surf target() { return {&target_}; }
    nr::wilsjo2::Surf depth() { return engine_.depth.image ? nr::wilsjo2::Surf{&engine_.depth} : nr::wilsjo2::Surf{}; }
    nr::wilsjo2::Surf motion() { return engine_.motion.image ? nr::wilsjo2::Surf{&engine_.motion} : nr::wilsjo2::Surf{}; }

    nr::wilsjo2::Surf make(nr::wilsjo2::Fmt format, uint32_t w, uint32_t h, bool) override {
        VkFormat f = target_.format;
        switch (format) {
            case nr::wilsjo2::Fmt::kFrame: break;
            case nr::wilsjo2::Fmt::kHalf: f = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            case nr::wilsjo2::Fmt::kR32F: f = VK_FORMAT_R32_SFLOAT; break;
            case nr::wilsjo2::Fmt::kRG32F: f = VK_FORMAT_R32G32_SFLOAT; break;
            case nr::wilsjo2::Fmt::kF32: f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            case nr::wilsjo2::Fmt::kRG16F: f = VK_FORMAT_R16G16_SFLOAT; break;
        }
        return {s_.w2_make(cmd_, f, w, h)};
    }
    void retire(nr::wilsjo2::Surf& x) override {
        auto* p = static_cast<Impl::W2Surface*>(x.h);
        s_.w2_retire(p);
        x = {};
    }
    void use(nr::wilsjo2::Surf x, nr::wilsjo2::Access) override {
        if (x.h == &target_ && target_now_ != VK_IMAGE_LAYOUT_GENERAL) {
            w2_layout(cmd_, target_.image, target_now_, VK_IMAGE_LAYOUT_GENERAL);
            target_now_ = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
    bool record(const Recorder& fn) override {
        if (!s_.w2_stage) return false;
        const ImageOf image_of = [&](nr::wilsjo2::Surf x) {
            nr::wilsjo2::Image i;
            if (!x) return i;
            if (x.h == &target_) {
                i.image = target_.image; i.format = target_.format; i.width = target_.width; i.height = target_.height;
                i.layout = target_now_;
            } else if (x.h == &engine_.depth || x.h == &engine_.motion) {
                const ColourFrame& g = *static_cast<const ColourFrame*>(x.h);
                i.image = g.image; i.format = g.format; i.width = g.width; i.height = g.height; i.layout = g.before;
            } else {
                const auto* p = static_cast<const Impl::W2Surface*>(x.h);
                i.image = p->image; i.format = p->format; i.width = p->w; i.height = p->h; i.layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            return i;
        };
        // one stage recording a call (its descriptor sets and views by recording: a frame with many passes needs more
        // sets than one recording's pool holds)
        bool ok = false;
        try {
            ok = fn(cmd_, *s_.w2_stage, image_of);
        } catch (const std::exception& e) {
            log("[nr] wilsjo2 (ReShade route): %s", e.what());
            ok = false;
        }
        s_.w2_stage->end_recording();
        return ok;
    }
    bool has(nr::wilsjo2::Mode mode) const override { return s_.w2_stage && s_.w2_stage->has(mode); }
    bool has_fast() const override { return !s_.w2_stage || s_.w2_stage->has_fast(); }
    bool stage_failed() const override { return !s_.w2_stage; }
    nr::wilsjo2::PassReady pass(uint32_t, const nr::wilsjo2::PassControls&, uint32_t, uint32_t) override {
        return nr::wilsjo2::PassReady::kReady;   // the network runtime is made before the recording (wilsjo2_prepare)
    }
    void release_passes(size_t from) override {
        for (uint32_t k = uint32_t(from); k < nr::wilsjo2::kMaxPasses; ++k) s_.release_everywhere(Impl::stack_feature(feature_, k));
    }
    int network(const nr::wilsjo2::NetCall& c) override {
        ColourFrame in = frame_of(c.input), out = frame_of(c.output);
        EngineFrame e = engine_;
        e.colour = in;
        e.target = out;
        e.reset = c.reset;
        e.feature = Impl::stack_feature(feature_, c.pass);
        if (c.depth.h != &engine_.depth) {   // peripheral compression: the packed guides at the model extent
            e.depth = frame_of(c.depth);
            e.motion = frame_of(c.motion);
            e.depth_x = e.depth_y = e.motion_x = e.motion_y = 0;
            e.motion_texture_width = e.motion.width; e.motion_texture_height = e.motion.height;
            e.motion_scale_x = c.motion_rect.width ? c.mv_x / float(c.motion_rect.width) : c.mv_x;
            e.motion_scale_y = c.motion_rect.height ? c.mv_y / float(c.motion_rect.height) : c.mv_y;
        }
        // Otherwise the route's own scales (normalized screen units, at any model extent), as this route ran them.
        Controls k = net_;
        k.intensity = c.controls.intensity;
        k.style = int(c.controls.style);
        k.local_structure = c.controls.local_structure;
        k.local_tone = c.controls.local_tone;
        k.skin_structure = c.controls.skin_structure;
        k.automatic_mask = c.controls.auto_mask;
        k.noise_every_frame = c.noise_every_frame;
        k.reversible_mode = int(c.proxy_curve);
        k.model_capture_allowed = c.last;
        k.history_strength = settings_.history_strength;
        try {
            if (e.motion.image) {
                s_.runtime->record_engine(cmd_, e, s_.capture_controls(k));
            } else {
                if (in.image != out.image) {
                    w2_everything(cmd_);
                    VkImageCopy region{};
                    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.extent = {in.width, in.height, 1};
                    vkCmdCopyImage(cmd_, in.image, VK_IMAGE_LAYOUT_GENERAL, out.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                    w2_everything(cmd_);
                }
                if (estimate_) {
                    TemporalFrame t{c.reset, e.feature};
                    s_.runtime->record_temporal(cmd_, out, s_.capture_controls(k), t);
                } else {
                    s_.runtime->record(cmd_, out, s_.capture_controls(k), c.reset);
                }
            }
        } catch (const std::exception& x) {
            log("[nr] wilsjo2 (ReShade route): the network: %s", x.what());
            return 1;
        }
        return 0;
    }
    void passes_begin(const nr::wilsjo2::FlowFacts& x) override {
        s_.w2_fast_passes = x.fast_passes;
        s_.w2_fast_fallback = x.fast_fallback;
    }
    void spatial_ran(const nr::wilsjo2::SpatialLayout& layout) override {
        s_.w2_spatial = layout;
        s_.w2_spatial_on = true;
    }
    void finish() override {
        if (target_now_ != target_.after) {
            w2_layout(cmd_, target_.image, target_now_, target_.after);
            target_now_ = target_.after;
        }
    }
    void say(const char* text) override { log("[nr] wilsjo2 (ReShade route): %s", text); }

private:
    ColourFrame frame_of(nr::wilsjo2::Surf x) const {
        ColourFrame f{};
        if (x.h == &target_) {
            f = target_;
            f.before = f.after = target_now_;
            return f;
        }
        if (x.h == &engine_.depth || x.h == &engine_.motion) return *static_cast<const ColourFrame*>(x.h);
        const auto* p = static_cast<const Impl::W2Surface*>(x.h);
        f.image = p->image; f.format = p->format; f.width = p->w; f.height = p->h;
        f.before = f.after = VK_IMAGE_LAYOUT_GENERAL;
        f.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return f;
    }
    Impl& s_;
    VkCommandBuffer cmd_;
    ColourFrame target_;
    VkImageLayout target_now_;
    EngineFrame engine_;
    Controls net_;
    const nr::wilsjo2::Settings& settings_;
    uint64_t feature_;
    bool estimate_;
};
}  // namespace

void Session::Impl::wilsjo2_release(uint64_t feature) {
    const auto it = w2.find(feature);
    if (it == w2.end()) return;
    nr::wilsjo2::FlowState& o = it->second.flow;
    for (W2Surface** x : {reinterpret_cast<W2Surface**>(&o.proxy.h), reinterpret_cast<W2Surface**>(&o.keep.h),
                          reinterpret_cast<W2Surface**>(&o.answer.h), reinterpret_cast<W2Surface**>(&o.scratch.h),
                          reinterpret_cast<W2Surface**>(&o.clamp.h), reinterpret_cast<W2Surface**>(&o.small.h),
                          reinterpret_cast<W2Surface**>(&o.native.h), reinterpret_cast<W2Surface**>(&o.sp_colour.h),
                          reinterpret_cast<W2Surface**>(&o.sp_depth.h), reinterpret_cast<W2Surface**>(&o.sp_motion.h),
                          reinterpret_cast<W2Surface**>(&o.sp_proxy.h), reinterpret_cast<W2Surface**>(&o.sp_answer.h),
                          reinterpret_cast<W2Surface**>(&o.sp_proxy_native.h),
                          reinterpret_cast<W2Surface**>(&o.sp_answer_native.h), reinterpret_cast<W2Surface**>(&o.meter.h),
                          reinterpret_cast<W2Surface**>(&o.exposure.h), reinterpret_cast<W2Surface**>(&o.p_light.h),
                          reinterpret_cast<W2Surface**>(&o.p_light_m.h), reinterpret_cast<W2Surface**>(&o.p_in.h),
                          reinterpret_cast<W2Surface**>(&o.p_out.h), reinterpret_cast<W2Surface**>(&o.p_scratch.h),
                          reinterpret_cast<W2Surface**>(&o.p_clamp.h), reinterpret_cast<W2Surface**>(&o.p_m.h),
                          reinterpret_cast<W2Surface**>(&o.p_state.h), reinterpret_cast<W2Surface**>(&o.small_raw.h),
                          reinterpret_cast<W2Surface**>(&o.small_keep.h), reinterpret_cast<W2Surface**>(&o.classic.h),
                          reinterpret_cast<W2Surface**>(&o.f_in.h), reinterpret_cast<W2Surface**>(&o.f_out.h),
                          reinterpret_cast<W2Surface**>(&o.f_net.h), reinterpret_cast<W2Surface**>(&o.f_ans.h)})
        w2_retire(*x);
    for (nr::wilsjo2::Surf& x : o.fast) w2_retire(*reinterpret_cast<W2Surface**>(&x.h));
    w2_retire(it->second.work);
    for (uint32_t k = 1; k < nr::wilsjo2::kMaxPasses; ++k) release_everywhere(stack_feature(feature, k));
    w2.erase(it);
}

// The network at the model's extent, network-only (as RenoDX's): the flow shows it the proxy and composes the answer.
bool Session::Impl::wilsjo2_runtime(const Controls& controls, uint32_t w, uint32_t h, VkFormat f, Controls* c) {
    *c = controls;
    c->preprocess = Preprocess{};   // the flow runs [Preprocess] itself, from the frame's light
    c->passes = 1;
    c->per_pass.clear();
    c->enabled = true;
    c->apply_model = true;
    c->debug_view = 0;
    c->show_skin_mask = false;
    const bool ok = ensure_runtime(*c, w, h, f);
    return ok && runtime->takes_target(*c);
}

bool Session::Impl::wilsjo2_prepare(const Session::Wilsjo2Frame& wf, uint64_t feature, uint32_t w, uint32_t h, VkFormat f,
                                    bool guides, const Controls& controls, Controls* net) {
    if (!wf.settings) return false;
    w2_free_retired();
    const auto& e = w2[feature];
    uint32_t mw = w, mh = h;
    bool spatial = false;
    nr::wilsjo2::flow_model_extent(*wf.settings, w, h, e.flow, guides, &mw, &mh, &spatial);
    // the network made for the format the flow will show it (its input and answer)
    namespace w2f = nr::wilsjo2;
    const bool prep = wf.preprocess.active() && (!w2_stage || w2_stage->has(w2f::kPrepBack));
    const w2f::Fmt nf = w2f::flow_network_format(*wf.settings, spatial, !w2_stage || w2_stage->has_fast(), prep);
    const VkFormat vf = nf == w2f::Fmt::kHalf ? VK_FORMAT_R16G16B16A16_SFLOAT
                      : nf == w2f::Fmt::kF32 ? VK_FORMAT_R32G32B32A32_SFLOAT : [&] {
                            bool blit = false;
                            const VkFormat x = w2_work_format(handles.physical, f, &blit);
                            return x ? x : w2_storage_twin(f);
                        }();
    return wilsjo2_runtime(controls, mw, mh, vf, net);
}

bool Session::Impl::record_wilsjo2(VkCommandBuffer cmd, const ColourFrame& colour, const EngineFrame& engine,
                                   const Controls& net, const Session::Wilsjo2Frame& wf) {
    if (!wf.settings || !runtime) return false;
    const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!w2_stage && (!w2_stage_failed_ms || now_ms - w2_stage_failed_ms >= 5000)) {
        try {
            w2_stage = std::make_unique<nr::wilsjo2::Stage>(make_host(), root + "\\dlssnr-amd\\shaders\\wilsjo2");
            if (w2_stage_failed_ms) log("[nr] wilsjo2 (ReShade route) available again");
            w2_stage_failed_ms = 0;
        } catch (const std::exception& x) {
            if (!w2_stage_said) { w2_stage_said = true; log("[nr] wilsjo2 (ReShade route) unavailable: %s (tried again every 5 s)", x.what()); }
            w2_stage_failed_ms = std::max<uint64_t>(now_ms, 1);
        }
    }
    if (!w2_stage) return false;
    w2_uses.touch();
    W2Entry& entry = w2[engine.feature];
    w2_everything(cmd);
    // The frame read and written in place when the stage can (sampled + storage, a storable format); else through an
    // image of ours (an sRGB view's bytes as UNORM), copied in before and back after.
    // A format the stage cannot store into goes through a wider one by blits (w2_work_format).
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    bool blit = false;
    const VkFormat twin = w2_work_format(handles.physical, colour.format, &blit);
    if (!twin) {
        status = "wilsjo2: the stage can neither store into the frame's format nor blit it into one it can";
        return false;
    }
    const bool in_place = twin == colour.format && (colour.usage & rw) == rw;
    ColourFrame target = colour;
    const auto copy = [&](VkImage from, VkImageLayout from_layout, VkImage to, VkImageLayout to_layout) {
        if (blit) {
            VkImageBlit b{};
            b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[1] = b.dstOffsets[1] = {int32_t(colour.width), int32_t(colour.height), 1};
            vkCmdBlitImage(cmd, from, from_layout, to, to_layout, 1, &b, VK_FILTER_NEAREST);
            return;
        }
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {colour.width, colour.height, 1};
        vkCmdCopyImage(cmd, from, from_layout, to, to_layout, 1, &region);
    };
    if (!in_place) {
        if (!(colour.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(colour.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            status = "the frame cannot be read and written in place or copied (its usage)";
            return false;
        }
        W2Surface*& work = entry.work;
        if (work && (work->format != twin || work->w != colour.width || work->h != colour.height)) w2_retire(work);
        if (!work && !(work = w2_make(cmd, twin, colour.width, colour.height))) {
            status = "wilsjo2: the frame's working copy could not be made";
            return false;
        }
        if (blit) {
            static bool said = false;
            if (!said) { said = true; log("[nr] wilsjo2 (ReShade route): the frame's format %d is worked in format %d (blits in and back)", int(colour.format), int(twin)); }
        }
        w2_layout(cmd, colour.image, colour.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy(colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, work->image, VK_IMAGE_LAYOUT_GENERAL);
        w2_layout(cmd, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colour.before);
        target.image = work->image; target.format = work->format;
        target.before = target.after = VK_IMAGE_LAYOUT_GENERAL;
        target.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    SessionW2Io io(*this, cmd, target, engine, net, *wf.settings, engine.feature, wf.estimate_motion);
    w2_fast_passes = 0.f; w2_fast_fallback = false; w2_spatial_on = false;
    w2_motion = engine.motion.image != VK_NULL_HANDLE && wf.fast_motion;
    nr::wilsjo2::FlowFrame ff;
    ff.target = io.target();
    ff.width = target.width; ff.height = target.height;
    ff.unorm = w2_unorm(target.format);
    ff.direct_ok = true;   // the stage reads and writes it in GENERAL at its extent (SessionW2Io::use)
    ff.depth = io.depth();
    ff.motion = io.motion();
    char g[160];
    std::snprintf(g, sizeof g, "%d:%ux%u:%d:%ux%u", int(engine.depth.format), engine.depth.width, engine.depth.height,
                  int(engine.motion.format), engine.motion.width, engine.motion.height);
    ff.guide_signature = g;
    ff.depth_rect = {engine.depth_x, engine.depth_y, engine.depth.width, engine.depth.height};
    ff.motion_rect = {engine.motion_x, engine.motion_y, engine.motion.width, engine.motion.height};
    ff.flags = engine.depth_inverted ? (1u << 3) : 0u;
    ff.hdr = wf.hdr;
    ff.reset = engine.reset;
    // the flow's MV scale in pixels of the motion region (as an NGX scale is), so that its peripheral compression maps
    // them to native pixels; the network is given this route's own normalized scale (SessionW2Io::network)
    ff.mv_x = engine.motion_scale_x * float(std::max(engine.motion.width, 1u));
    ff.mv_y = engine.motion_scale_y * float(std::max(engine.motion.height, 1u));
    ff.preprocess = wf.preprocess;
    ff.at_present = true;
    ff.fast_motion = wf.fast_motion;
    const bool ran = nr::wilsjo2::flow_run(io, entry.flow, ff, *wf.settings, uint64_t(target.format));
    w2_everything(cmd);
    if (!in_place) {   // the answer (or the frame as it came) back into the frame
        w2_layout(cmd, colour.image, colour.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy(entry.work->image, VK_IMAGE_LAYOUT_GENERAL, colour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        w2_layout(cmd, colour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, colour.after);
    }
    return ran;
}

bool Session::run_renodx(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                         ID3D12Resource* output, D3D12_RESOURCE_STATES output_state,
                         const EngineResources& resources, const Controls& controls,
                         ID3D12Resource* input, D3D12_RESOURCE_STATES input_state, const RenoDXFrame& rf) {
    auto& s = *impl_;
    s.current_feature = resources.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !device || !list || !output || !input) return false;
    if (!resources.motion && Impl::renodx_needs_motion(rf)) return false;
    s.status.clear();
    if (!s.ensure_d3d12_device(device)) return false;
    Impl::Recording rec;
    const VkCommandBuffer cmd = s.begin_recording(device, list, rec);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, output, output_state);
    if (!target.usable()) { s.status = describe_rejection(target, "output"); return false; }
    const auto source = input == output ? target : colour_handle(device, input, input_state);
    if (!source.usable()) { s.status = describe_rejection(source, "colour"); return false; }
    if (source.format != target.format || source.width != target.width || source.height != target.height) {
        s.status = "RenoDX-style: the colour and the output differ in format or extent";
        return false;
    }
    const uint32_t w = target.width, h = target.height;
    const auto motion_vk = resources.motion ? resource_handle(device, resources.motion, resources.motion_state)
                                            : decltype(resource_handle(device, resources.motion, resources.motion_state)){};
    uint32_t nw = w, nh = h;
    s.renodx_extent(resources.feature, w, h,
                    resources.motion_subrect.width ? resources.motion_subrect.width : motion_vk.width,
                    resources.motion_subrect.height ? resources.motion_subrect.height : motion_vk.height,
                    rf.settings, &nw, &nh);
    Controls c;
    // NR off (NeuralUplift): the stage copies the frame through and never asks for the network - none is made or
    // kept in use for it, so [Network] Residency sees NR off
    if (rf.settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &c)) return false;
    renodx::Stage* stage = s.renodx_stage(resources.feature, w, h, nw, nh, target.format, rf);
    if (!stage) return false;
    ColourFrame colour{}, out{};
    fill(&colour, source);
    fill(&out, target);
    EngineFrame engine{};
    engine.feature = resources.feature;
    engine.reset = resources.reset;
    engine.motion_scale_x = resources.motion_scale_x;
    engine.motion_scale_y = resources.motion_scale_y;
    engine.depth_inverted = resources.depth_inverted;
    if (resources.motion) {
        if (!fill(&engine.motion, motion_vk)) return false;
        engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, resources.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (resources.depth &&
        fill(&engine.depth, resource_handle(device, resources.depth, resources.depth_state)))
        apply_guide_subrect(&engine.depth, resources.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
    // As run_after: full barriers on both sides, vkd3d's tracked compute state forgotten afterwards.
    const auto everything = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = src_access; mb.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    ColourFrame exposure{};
    ResourceHandle exposure_vk = rf.exposure ? resource_handle(device, rf.exposure, rf.exposure_state) : ResourceHandle{};
    // An exposure is a float (DLSS's ExposureTexture, as OptiScaler reads it); a typeless one is read as its float
    // format. Safe for any VkFormat vkd3d-proton made it (R32_TYPELESS: R32_UINT): the stage copies its texel
    // (vkCmdCopyImage, same size class) into its own image of this format.
    bool exposure_typeless = false;
    if (exposure_vk.image && exposure_vk.format == VK_FORMAT_UNDEFINED) {
        switch (exposure_vk.dxgi) {
            case DXGI_FORMAT_R32_TYPELESS: exposure_vk.format = VK_FORMAT_R32_SFLOAT; break;
            case DXGI_FORMAT_R16_TYPELESS: exposure_vk.format = VK_FORMAT_R16_SFLOAT; break;
            case DXGI_FORMAT_R32G32B32A32_TYPELESS: exposure_vk.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            default: break;
        }
        exposure_typeless = exposure_vk.format != VK_FORMAT_UNDEFINED;
    }
    const bool have_exposure = rf.exposure && fill(&exposure, exposure_vk);
    {   // what became of the game's exposure texture, once per outcome (the stage's feed line says only "none")
        static int logged = -1;
        const int outcome = !rf.exposure ? 0 : have_exposure ? 1 : 2;
        if (outcome != logged) {
            logged = outcome;
            if (outcome == 0) log("[nr] RenoDX stage: OptiScaler handed no exposure texture with this frame");
            else if (outcome == 1) log("[nr] RenoDX stage: the game's exposure texture read (DXGI format %u, %ux%u%s)",
                                       unsigned(exposure_vk.dxgi), exposure_vk.width, exposure_vk.height,
                                       exposure_typeless ? ", typeless: read as float" : "");
            else log("[nr] RenoDX stage: the game's exposure texture could not be read (DXGI format %u, %ux%u, %s)",
                     unsigned(exposure_vk.dxgi), exposure_vk.width, exposure_vk.height,
                     exposure_vk.image ? "a format not mapped" : "no Vulkan image");
        }
    }
    everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (!s.record_renodx(cmd, *stage, colour, out, engine, c, rf, have_exposure ? &exposure : nullptr)) return false;
    everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    s.leave_list(device, list, rec);
    return true;
}

bool Session::record_raw(ID3D12Device* device, ID3D12GraphicsCommandList* list, const RawRecorder& recorder) {
    auto& s = *impl_;
    if (!device || !list || !recorder) return false;
    if (!s.ensure_d3d12_device(device)) return false;
    Impl::Recording rec;
    const VkCommandBuffer cmd = s.begin_recording(device, list, rec);
    if (!cmd) return false;
    HostDevice host{};
    host.instance = s.handles.instance; host.physical = s.handles.physical; host.device = s.handles.device;
    host.queue = s.access.queue; host.queue_family = s.access.family;
    const RawLookup lookup = [device](ID3D12Resource* r, D3D12_RESOURCE_STATES state, bool colour) {
        RawImage out{};
        if (!r) return out;
        ResourceHandle h = colour ? colour_handle(device, r, state) : resource_handle(device, r, state);
        if (!colour && h.image && h.format == VK_FORMAT_UNDEFINED) {   // as run_renodx reads an exposure texture
            switch (h.dxgi) {
                case DXGI_FORMAT_R32_TYPELESS: h.format = VK_FORMAT_R32_SFLOAT; break;
                case DXGI_FORMAT_R16_TYPELESS: h.format = VK_FORMAT_R16_SFLOAT; break;
                case DXGI_FORMAT_R32G32B32A32_TYPELESS: h.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
                default: break;
            }
        }
        out.image = h.image; out.format = h.format; out.width = h.width; out.height = h.height; out.layout = h.layout;
        return out;
    };
    const auto everything = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    everything();
    bool ok = false;
    try {
        ok = recorder(cmd, host, lookup);
    } catch (const std::exception& e) {
        log("[nr] %s", e.what());
        ok = false;
    }
    everything();
    s.leave_list(device, list, rec);
    return ok;
}

bool Session::record_raw_vk(VkCommandBuffer cmd, const RawRecorderVk& recorder) {
    auto& s = *impl_;
    if (!cmd || !recorder || !s.handles.valid() || !s.access.queue) return false;
    HostDevice host{};
    host.instance = s.handles.instance; host.physical = s.handles.physical; host.device = s.handles.device;
    host.queue = s.access.queue; host.queue_family = s.access.family;
    const auto everything = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    everything();
    bool ok = false;
    try {
        ok = recorder(cmd, host);
    } catch (const std::exception& e) {
        log("[nr] %s", e.what());
        ok = false;
    }
    everything();
    return ok;
}

bool Session::record_raw_d3d11(IUnknown* device, const RawRecorder11& recorder) {
    auto& s = *impl_;
    if (!device || !recorder || !s.dxvk || !s.handles.valid() || !s.access.queue) return false;
    if (!s.raw11_pool) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s.access.family;
        if (vkCreateCommandPool(s.handles.device, &pci, nullptr, &s.raw11_pool) != VK_SUCCESS) { s.raw11_pool = {}; return false; }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.raw11_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = Impl::kRaw11;
        bool made = vkAllocateCommandBuffers(s.handles.device, &ai, s.raw11_cmd) == VK_SUCCESS;
        for (int i = 0; made && i < Impl::kRaw11; ++i) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            made = vkCreateFence(s.handles.device, &fi, nullptr, &s.raw11_fence[i]) == VK_SUCCESS;
        }
        if (!made) {
            for (VkFence& f : s.raw11_fence) if (f) { vkDestroyFence(s.handles.device, f, nullptr); f = {}; }
            vkDestroyCommandPool(s.handles.device, s.raw11_pool, nullptr);
            s.raw11_pool = {};
            return false;
        }
    }
    const int slot = s.raw11_slot;
    if (!s.raw11_fence[slot]) {   // a failed submit's fence that could not be made again (below): made here, or no NR now
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (test_fault("raw11_fence") || vkCreateFence(s.handles.device, &fi, nullptr, &s.raw11_fence[slot]) != VK_SUCCESS) {
            s.raw11_fence[slot] = {};
            return false;
        }
    }
    if (vkWaitForFences(s.handles.device, 1, &s.raw11_fence[slot], VK_TRUE, 2000000000ull) != VK_SUCCESS) return false;
    const VkCommandBuffer cmd = s.raw11_cmd[slot];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
    auto unsubmitted = on_exit([&s] { s.discard_timing(); });
    HostDevice host{};
    host.instance = s.handles.instance; host.physical = s.handles.physical; host.device = s.handles.device;
    host.queue = s.access.queue; host.queue_family = s.access.family;
    const auto everything = [&] {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    const RawLookup11 lookup = [&](IUnknown* texture) {
        RawImage out;
        const ResourceHandle h = d3d11_image(texture, device);
        if (!h.image) return out;
        out.image = h.image; out.format = h.format; out.width = h.width; out.height = h.height; out.layout = h.layout;
        return out;
    };
    everything();
    bool ok = false;
    try {
        ok = recorder(cmd, host, lookup);
    } catch (const std::exception& e) {
        log("[nr] %s", e.what());
        ok = false;
    }
    everything();
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    if (!ok) return false;   // nothing submitted (the slot's fence stays signalled)
    if (vkResetFences(s.handles.device, 1, &s.raw11_fence[slot]) != VK_SUCCESS) return false;
    VkResult r;
    {
        QueueAccess::Held held(s.access);   // the game's recorded work goes in front (DXVK's flush in the lock)
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        r = test_fault("raw11_submit") ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkQueueSubmit(s.access.queue, 1, &si, s.raw11_fence[slot]);
    }
    if (r != VK_SUCCESS) {   // the fence stays unsignalled: made again signalled so the slot is not waited on for ever
        vkDestroyFence(s.handles.device, s.raw11_fence[slot], nullptr);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (test_fault("raw11_fence") || vkCreateFence(s.handles.device, &fi, nullptr, &s.raw11_fence[slot]) != VK_SUCCESS)
            s.raw11_fence[slot] = {};
        return false;
    }
    s.raw11_slot = (slot + 1) % Impl::kRaw11;
    unsubmitted.armed = false;
    return true;
}

bool Session::run_present(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                          ID3D12Resource* back_buffer, unsigned width, unsigned height,
                          const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = Impl::kUnnamedStack;   // legacy Present entry, private to this session
    if (s.failed || !device || !list || !back_buffer) return false;
    s.status.clear();
    if (!s.ensure_d3d12_device(device)) return false;
    const VkCommandBuffer cmd = command_buffer(list);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, back_buffer, D3D12_RESOURCE_STATE_PRESENT);
    if (!target.usable()) { s.status = describe_rejection(target, "back buffer"); return false; }
    if (!s.ensure_runtime(controls, target.width, target.height, target.format)) return false;

    // Here the network works on the game's own buffer: there is nothing to hand
    // anyone afterwards, the frame just has to come out enhanced.
    ColourFrame frame{};
    frame.image = target.image;
    frame.format = target.format;
    frame.width = target.width; frame.height = target.height;
    frame.before = frame.after = target.layout;
    try {
        TemporalFrame temporal{false, s.current_feature};
        s.runtime->record_temporal(cmd, frame, s.capture_controls(controls), temporal);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }
    return true;
}

bool Session::run_d3d11(const D3D11Frame& frame, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = frame.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !frame.device || !frame.target) return false;
    s.status.clear();

    // DXVK's interop device is the whole of this path: it is where the Vulkan
    // handles, the submission queue and the queue lock all come from.
    if (!s.dxvk) {
        log("[nr] d3d11: asking DXVK for its Vulkan handles");
        s.handles = d3d11_handles(frame.device, &s.dxvk);
        log("[nr] d3d11: instance %p physical %p device %p interop %p",
            static_cast<void*>(s.handles.instance), static_cast<void*>(s.handles.physical),
            static_cast<void*>(s.handles.device), static_cast<void*>(s.dxvk));
        if (!s.handles.valid() || !s.dxvk) {
            if (s.dxvk) { s.dxvk->Release(); s.dxvk = nullptr; }
            s.status = "no Vulkan device underneath; this D3D11 game is not on DXVK";
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return false;
        }
        if (!d3d11_queue_access(s.dxvk, &s.access)) {
            s.status = "DXVK would not hand out its submission queue";
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return false;
        }
    }

    // Said once per target, not once per frame: this pair used to be two of the
    // fifteen flushed lines every frame wrote (see first_sight in
    // nr_pe_interop.cpp). What the target is only changes when it changes.
    static VkImage traced_target = VK_NULL_HANDLE;
    const auto target = d3d11_image(frame.target, frame.device);
    if (!target) {
        s.status = "the D3D11 texture exposes no Vulkan image";
        return false;
    }
    if (target.image != traced_target) {
        traced_target = target.image;
        log("[nr] d3d11: queue family %u; target %ux%u format %u layout %u usage 0x%x",
            s.access.family, target.width, target.height, unsigned(target.format),
            unsigned(target.layout), unsigned(target.usage));
    }
    // The pass transfers into and out of whatever image it is given. DXVK tells
    // us what the image may be used for, so this is a check rather than a hope -
    // and a texture without these bits gets declined with a reason instead of
    // producing validation errors inside somebody's game.
    constexpr VkImageUsageFlags required =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((target.usage & required) != required) {
        s.status = "the game's texture cannot be copied to and from; nothing to run on";
        return false;
    }
    Controls renodx_controls{}, w2_controls{};
    renodx::Stage* renodx_stage = nullptr;
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return false;
        }
        const auto mv = frame.motion ? d3d11_image(frame.motion, frame.device) : decltype(d3d11_image(frame.motion, frame.device)){};
        uint32_t nw = target.width, nh = target.height;
        s.renodx_extent(frame.feature, target.width, target.height, mv.width, mv.height, frame.renodx->settings, &nw, &nh);
        if (frame.renodx->settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &renodx_controls)) return false;
        if (!(renodx_stage = s.renodx_stage(frame.feature, target.width, target.height, nw, nh, target.format, *frame.renodx)))
            return false;
    } else if (frame.wilsjo2) {
        if (!s.wilsjo2_prepare(*frame.wilsjo2, frame.feature, target.width, target.height, target.format,
                               frame.motion != nullptr, controls, &w2_controls))
            return false;
    } else if (!s.ensure_runtime(controls, target.width, target.height, target.format)) return false;
    if (!s.ensure_own_command_buffer()) return false;
    if (!s.wait_for_own_cmd()) return false;

    ColourFrame colour{};
    colour.image = target.image;
    colour.format = target.format;
    colour.width = target.width; colour.height = target.height;
    // Transition from the layout DXVK believes the image is in, and put it back:
    // the game's next use of its own texture must see what it left there.
    colour.before = colour.after = target.layout;
    // What DXVK says the image may be used for: with SAMPLED and STORAGE the runtime reads and
    // writes it in place at Model resolution below 100% instead of copying it out and back.
    colour.usage = target.usage;

    EngineFrame engine{};
    engine.feature = frame.feature;
    engine.colour = colour;
    engine.reset = frame.reset || s.resized(frame.feature, colour.width, colour.height);
    bool w2_ran = true;   // a wilsjo2 layer's flow ran (else the frame was left as it came; its commands still go in)
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    const bool motion = frame.motion && fill(&engine.motion, d3d11_image(frame.motion, frame.device));
    if (frame.depth) fill(&engine.depth, d3d11_image(frame.depth, frame.device));

    auto unsubmitted = on_exit([&s] { s.discard_timing(); });
    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(s.own_cmd, 0);
        vkBeginCommandBuffer(s.own_cmd, &bi);
        if (frame.wilsjo2) {
            OwnOuterRun timing(*this, s.own_cmd, s.call_layer > 0 && !s.outer_open);
            Wilsjo2Frame wf = *frame.wilsjo2;
            wf.estimate_motion = wf.estimate_motion || frame.estimate_motion;
            w2_ran = s.record_wilsjo2(s.own_cmd, colour, engine, w2_controls, wf);
        } else if (renodx_stage) {
            if (!motion && Impl::renodx_needs_motion(*frame.renodx)) { vkEndCommandBuffer(s.own_cmd); return false; }
            if (!s.record_renodx(s.own_cmd, *renodx_stage, colour, colour, engine, renodx_controls, *frame.renodx)) {
                vkEndCommandBuffer(s.own_cmd);
                return false;
            }
        } else {
            OwnOuterRun timing(*this, s.own_cmd, s.call_layer > 0 && !s.outer_open);

            if (motion) s.runtime->record_engine(s.own_cmd, engine, s.capture_controls(controls));
            else if (frame.estimate_motion) {
                TemporalFrame temporal{engine.reset, frame.feature};
                s.runtime->record_temporal(s.own_cmd, colour, s.capture_controls(controls), temporal);
            } else s.runtime->record(s.own_cmd, colour, s.capture_controls(controls), frame.reset);

        }
        vkEndCommandBuffer(s.own_cmd);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }

    // Everything the game has recorded goes in front of us - that is what the
    // flush inside the lock is for - and then our submit happens with the queue
    // held, because DXVK shares one VkQueue with everything else in the process.
    VkResult r;
    {
        QueueAccess::Held held(s.access);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &s.own_cmd;
        r = test_fault("own_submit") ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkQueueSubmit(s.access.queue, 1, &si, s.own_fence);
        if (r == VK_SUCCESS) s.own_cmd_pending = true;
    }
    if (r != VK_SUCCESS) {
        s.status = "the submit on the game's queue failed";
        return false;
    }
    unsubmitted.armed = false;
    return w2_ran;
}

bool Session::run_present_vulkan(const DeviceHandles& handles, VkQueue queue, uint32_t family,
                                 VkImage image, VkFormat format, uint32_t width, uint32_t height,
                                 uint32_t slot_index, uint32_t slot_count,
                                 const VkSemaphore* wait, uint32_t wait_count,
                                 VkSemaphore* signalled, bool temporal,
                                 const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = Impl::kUnnamedStack;   // legacy Present entry, private to this session
    if (s.failed || !image || !queue || !handles.valid() || !signalled) return false;
    s.status.clear();
    s.handles = handles;
    s.access.queue = queue;
    s.access.family = family;
    s.access.lock = nullptr;      // this queue is the game's and we are inside its present
    s.access.unlock = nullptr;

    // A game that alternates between two swapchain extents - 007 First Light
    // cycles 3840x2160 and 3840x2096 at startup and on every mode change - would
    // otherwise rebuild the whole network on each one: four gigabytes
    // reallocated, the weights re-uploaded, and the history thrown away. A
    // discarded history closes the temporal gate for that frame, which changes
    // the picture visibly, and doing it every other frame is exactly what a
    // flicker is.
    //
    // So an extent that does not match the built network does not rebuild it: it
    // is counted, the frame is left alone, and the rebuild happens only once the
    // new extent has actually settled. A real resolution change costs half a
    // second of untouched frames; an alternation costs nothing at all.
    if (s.runtime && (s.width != width || s.height != height) && !s.have_runtime(width, height)) {
        if (s.pending_w != width || s.pending_h != height) {
            s.pending_w = width; s.pending_h = height; s.pending_frames = 0;
        }
        if (++s.pending_frames < 30) return false;
        log("[nr] the swapchain has been %ux%u for %u frames; rebuilding from %ux%u",
            width, height, s.pending_frames, s.width, s.height);
    }
    s.pending_frames = 0;

    if (!s.ensure_slots(slot_count)) return false;
    if (slot_index >= s.slots.size()) return false;
    auto& slot = s.slots[slot_index];

    // Every slot, not just this one.
    //
    // The runtime's contract is explicit that the caller serialises uses of its
    // scratch arena, and the per-image slots defeat exactly that: with three
    // swapchain images, three of our command buffers can be executing at once,
    // all reading and writing one activation arena and one history image. The
    // result is frames that alternate between right and wrong, which is what a
    // flickering picture is.
    //
    // So the slots stay - a binary semaphore still needs one per image, and the
    // present depends on that - but only one pass is ever in flight. It costs a
    // pipeline bubble, which is the price of the arena being shared.
    for (auto& other : s.slots) if (!s.wait_slot(other)) return false;
    // Only after the slot is free: rebuilding the runtime destroys images this
    // slot's command buffer may still be reading.
    if (!s.ensure_runtime(controls, width, height, format)) return false;

    // A swapchain image at present time is in PRESENT_SRC. The pass transfers in
    // and out of it, and leaves it exactly as it found it: the present that
    // follows is entitled to the layout it was promised.
    ColourFrame colour{};
    colour.image = image;
    colour.format = format;
    colour.width = width; colour.height = height;
    colour.before = colour.after = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    static bool first = true;
    if (first) log("[nr] first fallback frame: %ux%u format %d on queue family %u, image %u of %u",
                   width, height, int(format), family, slot_index, slot_count);
    auto unsubmitted = on_exit([&s] { s.discard_timing(); });
    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(slot.cmd, 0);
        vkBeginCommandBuffer(slot.cmd, &bi);
        if (temporal) {
            TemporalFrame frame{false, s.current_feature};
            s.runtime->record_temporal(slot.cmd, colour, s.capture_controls(controls), frame);
        } else {
            s.runtime->record(slot.cmd, colour, s.capture_controls(controls));
        }
        vkEndCommandBuffer(slot.cmd);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }

    // One wait stage per semaphore, as the spec requires.
    std::vector<VkPipelineStageFlags> stages(wait_count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait_count;
    si.pWaitSemaphores = wait_count ? wait : nullptr;
    si.pWaitDstStageMask = wait_count ? stages.data() : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &slot.done;
    if (test_fault("slot_submit") || vkQueueSubmit(queue, 1, &si, slot.fence) != VK_SUCCESS) {
        s.status = "the submit before present failed";
        return false;
    }
    slot.pending = true;
    unsubmitted.armed = false;
    *signalled = slot.done;
    if (first) { first = false; log("[nr] first fallback frame submitted"); }
    return true;
}

void Session::set_vulkan_queue(VkQueue queue, uint32_t family) {
    if (!queue) return;
    impl_->access.queue = queue;
    impl_->access.family = family;
    // The caller records into the command buffer the upscaler handed over, which the game submits itself; only the
    // builds submit here (the weight upload, on the background thread), onto the game's own queue. Vulkan wants
    // every call on one queue serialised: on the .asi route the game's queue calls take the queue's lock
    // (vkdevice::enable_queue_locks), and ours take it too. Elsewhere (ReShade as a Vulkan layer serialises its own)
    // there is no lock to take, as before.
    if (vkdevice::queue_locks_enabled()) {
        impl_->access.lock = impl_->access.lock_quiet = [queue] { vkdevice::lock_queue(queue); };
        impl_->access.unlock = impl_->access.unlock_quiet = [queue] { vkdevice::unlock_queue(queue); };
    } else {
        impl_->access.lock = nullptr;
        impl_->access.unlock = nullptr;
    }
}

void Session::set_queue_lock(std::function<void()> lock, std::function<void()> unlock) {
    impl_->access.lock = impl_->access.lock_quiet = lock;
    impl_->access.unlock = impl_->access.unlock_quiet = unlock;
}

void Session::set_vulkan_handles(const DeviceHandles& handles) {
    if (handles.valid()) impl_->handles = handles;
}

VkImageView Session::vulkan_output_view() const { return impl_->vk_output_view; }

VkImage Session::run_vulkan(const DeviceHandles& handles, VkCommandBuffer cmd,
                            const VulkanFrame& frame, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = frame.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !cmd || !frame.colour || !handles.valid()) return VK_NULL_HANDLE;
    s.status.clear();
    s.handles = handles;

    // A Vulkan game gives us no D3D12 resource to hand back, so the output is a
    // plain VkImage of our own rather than one borrowed from a D3D12 allocation -
    // unless the caller takes the answer in its own image (VulkanFrame::in_place).
    Controls renodx_controls{}, w2_controls{};
    renodx::Stage* renodx_stage = nullptr;
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return VK_NULL_HANDLE;
        }
        uint32_t nw = frame.width, nh = frame.height;
        s.renodx_extent(frame.feature, frame.width, frame.height,
                        frame.motion_subrect.width ? frame.motion_subrect.width : frame.motion_width,
                        frame.motion_subrect.height ? frame.motion_subrect.height : frame.motion_height,
                        frame.renodx->settings, &nw, &nh);
        if (frame.renodx->settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &renodx_controls))
            return VK_NULL_HANDLE;
        if (!(renodx_stage = s.renodx_stage(frame.feature, frame.width, frame.height, nw, nh, frame.colour_format,
                                            *frame.renodx)))
            return VK_NULL_HANDLE;
    } else if (frame.wilsjo2) {
        if (!s.wilsjo2_prepare(*frame.wilsjo2, frame.feature, frame.width, frame.height, frame.colour_format,
                               frame.motion != VK_NULL_HANDLE, controls, &w2_controls))
            return VK_NULL_HANDLE;
    } else {
    if (!frame.in_place && !s.vk_output_valid(frame.width, frame.height, frame.colour_format)) {
        if (!s.ensure_vk_output(frame.width, frame.height, frame.colour_format))
            return VK_NULL_HANDLE;
    }
    s.size_hint_px = std::max(uint64_t(frame.motion ? frame.motion_width : 0) * (frame.motion ? frame.motion_height : 0),
                              uint64_t(frame.depth ? frame.depth_width : 0) * (frame.depth ? frame.depth_height : 0));
    if (!s.ensure_runtime(controls, frame.width, frame.height, frame.colour_format))
        return VK_NULL_HANDLE;
    }

    ColourFrame colour{};
    colour.image = frame.in_place ? frame.colour : s.vk_output;
    colour.format = frame.colour_format;
    colour.width = frame.width; colour.height = frame.height;
    colour.before = colour.after = frame.in_place ? frame.colour_layout : VK_IMAGE_LAYOUT_GENERAL;
    if (frame.in_place) colour.usage = frame.colour_usage;

    EngineFrame engine{};
    engine.colour = colour;
    // The caller's colour read in place and its output written directly (the engine path with
    // the model applied): the copy into vk_output, the copy into the runtime's input and the
    // write-back all go. Anything else keeps the copies below.
    const bool direct = !frame.in_place && frame.output && frame.motion && s.runtime && s.runtime->takes_target(controls) &&
                        (frame.colour_usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
                        !(std::getenv("NR_VK_COPY") && std::atoi(std::getenv("NR_VK_COPY")));
    if (direct) {
        engine.colour.image = frame.colour;
        engine.colour.before = engine.colour.after = frame.colour_layout;
        engine.colour.usage = frame.colour_usage;
        engine.target = colour;
        engine.target.image = frame.output;
        engine.target.before = engine.target.after = frame.output_layout;
        engine.target.usage = frame.output_usage;
    }
    engine.feature = frame.feature;
    engine.reset = frame.reset;
    if (s.resized(frame.feature, frame.width, frame.height) && !s.carry(cmd, frame.feature)) engine.reset = true;
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    if (frame.motion) {
        engine.motion.image = frame.motion;
        engine.motion.format = frame.motion_format;
        engine.motion.width = frame.motion_width;
        engine.motion.height = frame.motion_height;
        engine.motion.before = engine.motion.after = frame.motion_layout;
        engine.motion.usage = frame.motion_usage;
        engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, frame.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (frame.depth) {
        engine.depth.image = frame.depth;
        engine.depth.format = frame.depth_format;
        engine.depth.width = frame.depth_width;
        engine.depth.height = frame.depth_height;
        engine.depth.before = engine.depth.after = frame.depth_layout;
        engine.depth.usage = frame.depth_usage;
        apply_guide_subrect(&engine.depth, frame.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
    }

    if (frame.wilsjo2) {   // a wilsjo2 layer (the ReShade route): its flow on `colour`, in place
        ColourFrame in{};
        in.image = frame.colour; in.format = frame.colour_format; in.width = frame.width; in.height = frame.height;
        in.before = in.after = frame.colour_layout; in.usage = frame.colour_usage;
        engine.colour = in;
        bool ran = false;
        try {
            OwnOuterRun timing(*this, cmd, s.call_layer > 0 && !s.outer_open);
            ran = s.record_wilsjo2(cmd, in, engine, w2_controls, *frame.wilsjo2);
        } catch (const std::exception& e) {
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return VK_NULL_HANDLE;
        }
        return ran ? frame.colour : VK_NULL_HANDLE;
    }
    if (renodx_stage) {
        ColourFrame in{}, out{};
        in.image = frame.colour; in.format = frame.colour_format; in.width = frame.width; in.height = frame.height;
        in.before = in.after = frame.colour_layout; in.usage = frame.colour_usage;
        const bool to_output = !frame.in_place && frame.output;
        out = in;
        if (to_output) {
            out.image = frame.output; out.before = out.after = frame.output_layout; out.usage = frame.output_usage;
        }
        ColourFrame exposure{};
        const ColourFrame* ex = nullptr;
        if (frame.renodx->vk_exposure) {
            exposure.image = frame.renodx->vk_exposure; exposure.format = frame.renodx->vk_exposure_format;
            exposure.width = frame.renodx->vk_exposure_width; exposure.height = frame.renodx->vk_exposure_height;
            exposure.before = exposure.after = VK_IMAGE_LAYOUT_GENERAL;
            ex = &exposure;
        }
        if (!s.record_renodx(cmd, *renodx_stage, in, out, engine, renodx_controls, *frame.renodx, ex)) return VK_NULL_HANDLE;
        return out.image;
    }
    OwnOuterRun timing(*this, cmd, s.call_layer > 0 && !s.outer_open);
    if (direct) {
        try {

            s.runtime->record_engine(cmd, engine, s.capture_controls(controls));

        } catch (const std::exception& e) {
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return VK_NULL_HANDLE;
        }
        return frame.output;
    }
    if (frame.in_place) {
        try {

            if (frame.motion) s.runtime->record_engine(cmd, engine, s.capture_controls(controls));
            else s.runtime->record(cmd, colour, s.capture_controls(controls), engine.reset);

        } catch (const std::exception& e) {
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return VK_NULL_HANDLE;
        }
        return frame.colour;
    }
    try {
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {frame.width, frame.height, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = frame.colour_layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = frame.colour;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyImage(cmd, frame.colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s.vk_output, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        if (frame.motion) s.runtime->record_engine(cmd, engine, s.capture_controls(controls));
        else s.runtime->record(cmd, colour, s.capture_controls(controls), engine.reset);

    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return VK_NULL_HANDLE;
    }
    return s.vk_output;
}

}  // namespace nr::pe
