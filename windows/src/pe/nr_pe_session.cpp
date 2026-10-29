#include "nr_pe_session.hpp"
#include "nr_pe_bridge.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_config.hpp"   // NetworkSwitch
#include "nr_pe_log.hpp"
#include "nr_pe_vkdevice.hpp"
#include "nr_pipeline_binary.hpp"
#include "nr_log.hpp"
#include "nr_screenshot.hpp"
#include <windows.h>
#include <d3d10.h>
#include <d3d11_4.h>
#include <dxgi.h>
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
#include <set>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace nr::pe {
namespace {

using nr::pe::log;

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

// What the card and the machine have left, in bytes.
//
// **Both, because on Windows the network is charged twice.** WDDM backs every
// video-memory allocation with system commit (RAM plus page file), so the 3.9
// GB arena of a 3818x1998 build takes 3.9 GB of VRAM budget *and* 3.9 GB of
// commit. A tester's machine ran out of the second with VRAM to spare (package
// test 4: 43286 of 43391 MB committed, the game holding 20.8 GB): DXVK's
// vkEndCommandBuffer failed, Dalamud wrote through a null pointer and the
// desktop lost its icons. Earlier "out of VRAM" failures - DXVK refused 108 MB
// with 2.3 GB of budget left, pipelines failed with OUT_OF_HOST_MEMORY - were
// the same thing.
struct MemoryState {
    uint64_t vram_used{}, vram_budget{}, vram_free{};   // largest device-local heap
    uint64_t commit_free{}, commit_limit{};             // this process's view of system commit
    bool vram_known{};
};

MemoryState read_memory(VkPhysicalDevice physical) {
    MemoryState m;
    if (physical) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        props.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physical, &props);
        // The largest device-local heap's free space, by budget where the driver
        // reports one and by heap size where it does not.
        const auto& mem = props.memoryProperties;
        for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
            if (!(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
            const uint64_t cap = budget.heapBudget[i] ? budget.heapBudget[i] : mem.memoryHeaps[i].size;
            const uint64_t used = budget.heapUsage[i];
            const uint64_t free = cap > used ? cap - used : 0;
            if (!m.vram_known || free > m.vram_free) {
                m.vram_free = free; m.vram_used = used; m.vram_budget = cap; m.vram_known = true;
            }
        }
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) {
        m.commit_free = memory.ullAvailPageFile;
        m.commit_limit = memory.ullTotalPageFile;
    }
    return m;
}

double mb(uint64_t bytes) { return double(bytes) / (1 << 20); }

// Headroom left for the game after a build, and the levels at which a running
// network steps aside. The gap between the two is the hysteresis that keeps a
// build from being made and dropped on alternate seconds.
constexpr uint64_t kVramReserve = 1024ull << 20, kCommitReserve = 3072ull << 20;
constexpr uint64_t kVramLow = 384ull << 20, kCommitLow = 1536ull << 20;

}  // namespace

// What the bridge needs to know about one D3D12 frame, whichever route handed it over.
struct BridgeInputs {
    ID3D12Resource* target{};
    D3D12_RESOURCE_STATES target_state{};
    // Where the colour is read from when it is not `target` itself (same format and extent).
    ID3D12Resource* source{};
    D3D12_RESOURCE_STATES source_state{};
    ID3D12Resource* motion{};
    D3D12_RESOURCE_STATES motion_state{};
    ID3D12Resource* depth{};
    D3D12_RESOURCE_STATES depth_state{};
    Session::Subrect motion_subrect{}, depth_subrect{};
    float motion_scale_x{1.0f}, motion_scale_y{1.0f};
    bool depth_inverted{true};
    bool reset{};
    uint64_t feature{};
    bool linear{};   // the colour is scene-referred linear light (encoded by the runtime)
    // The RenoDX-style option: its processing around the network instead (Session::run_renodx), with the
    // game's exposure texture when the route has one.
    const Session::RenoDXFrame* renodx{};
    ID3D12Resource* exposure{};
    D3D12_RESOURCE_STATES exposure_state{};
};

// The RenoDX-style option on the bridge: the stage recorded in the job in place of the network, on the shared
// images (in place: the copy out takes its answer).
struct BridgeRenoDX {
    renodx::Stage* stage{};
    Controls controls{};
    const Session::RenoDXFrame* frame{};
    bool exposure{};   // the shared exposure image holds the game's exposure texture
};

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
        bool linear{};
        float scale{1.0f};
        // The pass ceiling this one was built for: it sizes one history image
        // per pass, so a network built for four cannot serve a request for six.
        uint32_t passes{4};
        bool prep{};   // RuntimeConfig::preprocess
        bool native{}; // RuntimeConfig::native_compose
        int net{};     // the network it runs (nr::NetworkChoice)
        uint64_t used{};   // LRU stamp
        std::chrono::steady_clock::time_point last{};   // when it was last selected
        bool light{};      // built inside a recording (inframe): its images in its network memory's heap
    };
    // A cached network taken up again after serving nothing for this long starts its features fresh (as after a
    // Reset), so no frame is blended with a history that old; RenoDX's own look history uses the same bound.
    static constexpr std::chrono::milliseconds kStaleHistory{250};
    std::vector<Built> cache;
    uint64_t use_stamp{};
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
        VkFormat format{}; bool linear{}; float scale{}; uint32_t passes{}; bool prep{}, native{}; int net{};
        uint32_t ww{}, wh{}, sh{};   // working extent, tallest frame height
        std::shared_ptr<const RuntimeCore> core;
        uint64_t used{};
    };
    std::vector<PlanEntry> plans;
    static constexpr size_t kMaxPlans = 48;
    bool build_plan{};   // the background build is a plan (RuntimeConfig::plan_only)
    uint64_t inframe_built{}, plans_built{};
    double inframe_ms_max{};
    bool same_config_but_format(const Built& e, bool want_linear) const { return same_config(e, e.format, want_linear); }
    bool same_config(const Built& e, VkFormat f, bool want_linear) const {
        if (e.format != f || e.linear != want_linear || e.scale != model_scale || e.passes != max_passes_want ||
            e.prep != prep_want || e.native != (native_compose || renodx_native))
            return false;
        return e.net == net_want;
    }
    // The network a runtime runs (nr::NetworkChoice).
    static int net_of(const Runtime& r) {
#if NR_INT4
        if (r.int4_mixed()) return nr::kSilverAardwoldInt4;
#endif
        return r.network() == NetworkModel::Small ? nr::kSmallFp8 : nr::kSilverAardwold;
    }
    RuntimeConfig make_config(uint32_t w, uint32_t h, VkFormat f, bool want_linear) const;
    HostDevice make_host() const;
    TemporalConfig make_temporal() const {
        TemporalConfig temporal; temporal.enable = true;
        temporal.history_strength = history_strength;
        return temporal;
    }
    const PlanEntry* plan_for(uint32_t w, uint32_t h, VkFormat f, bool want_linear, NetworkModel model);
    // `tried`: the network it would have run on, when building inside the frame failed (a pipeline not made yet):
    // the background build then makes that runtime.
    bool inframe(uint32_t w, uint32_t h, VkFormat f, bool want_linear, std::shared_ptr<const RuntimeCore>* tried);
    bool start_plan(uint32_t w, uint32_t h, VkFormat f, bool want_linear);
    // While a game's dynamic resolution moves the frame size (a resize in the last kDrsActive): the plans of the
    // working extents nearest the latest size, one at a time in the background, so a sweep finds them made.
    static constexpr std::chrono::seconds kDrsActive{2};
    std::chrono::steady_clock::time_point last_resize{};
    uint32_t last_w{}, last_h{};
    VkFormat last_format{VK_FORMAT_UNDEFINED};
    bool last_linear{};
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
    void heap_poll();
    // Runtimes built inside frames with images of their own (no heap yet) kept at most this many.
    static constexpr size_t kOwnImages = 2;
    bool inframe_waiting{};
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
    // Evaluations that ran the network and that passed the frame through (building, freeing, failed): in the log
    // every kPassLog while any pass through, and for the whole session at its end.
    uint64_t ran{}, passed{}, ran_total{}, passed_total{};
    std::chrono::steady_clock::time_point pass_logged{};
    static constexpr std::chrono::seconds kPassLog{10};
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
        VkFormat format{}; bool linear{}; float scale{}; uint32_t passes{}; bool prep{}, native{}; int net{};
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
    // The parked network a background build is bringing back from RAM (reload_network writes its memory on that
    // thread): not looked at until the build is adopted.
    bool reloading(const Parked& p) const { return building && p.core == build_share; }
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
    float stage_ms_avg{}, stage_net_ms_avg{};
    bool renodx_timed{};
    void read_stage_timing() {
        if (stage_recorded <= kStageSlots) return;
        const uint32_t n = 2 + 2 * stage_slot_passes[stage_slot];
        uint64_t stamps[kStageQueries] = {};
        if (vkGetQueryPoolResults(handles.device, stage_timing, stage_slot * kStageQueries, n, n * sizeof(uint64_t), stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || stamps[1] <= stamps[0])
            return;   // still running: next frame
        uint64_t net = 0;
        for (uint32_t p = 0; p < stage_slot_passes[stage_slot]; ++p)
            if (stamps[3 + 2 * p] > stamps[2 + 2 * p]) net += stamps[3 + 2 * p] - stamps[2 + 2 * p];
        const float ms = float(double(stamps[1] - stamps[0]) * stage_timestamp_ns / 1e6);
        const float net_ms = float(double(net) * stage_timestamp_ns / 1e6);
        const bool first = stage_ms_avg <= 0.0f;
        stage_ms_avg = first ? ms : stage_ms_avg + (ms - stage_ms_avg) * 0.1f;
        stage_net_ms_avg = first ? net_ms : stage_net_ms_avg + (net_ms - stage_net_ms_avg) * 0.1f;
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
    std::vector<std::unique_ptr<Shot>> shots_out;        // with the worker, or kept
    void shot_free(Shot& sh) {
        if (sh.mapped) vkUnmapMemory(handles.device, sh.memory);
        if (sh.buffer) vkDestroyBuffer(handles.device, sh.buffer, nullptr);
        if (sh.memory) vkFreeMemory(handles.device, sh.memory, nullptr);
        if (sh.done) vkDestroyEvent(handles.device, sh.done, nullptr);
        sh.mapped = nullptr; sh.buffer = VK_NULL_HANDLE; sh.memory = VK_NULL_HANDLE; sh.done = VK_NULL_HANDLE;
    }
    // Readbacks for a w x h pair of `format`, stamped (nr_screenshot.hpp); null when anything fails.
    std::unique_ptr<Shot> shot_prepare(uint32_t w, uint32_t h, VkFormat format) {
        auto sh = std::make_unique<Shot>();
        sh->bytes = uint64_t(w) * h * screenshot::bytes_per_pixel(format);
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
            if (calls - renodx_retired[i].call > kRetireCalls && now - renodx_retired[i].at > kRetireTime &&
                !used_by_pending_job(renodx_retired[i].stage.get()))
                renodx_retired.erase(renodx_retired.begin() + std::ptrdiff_t(i));
    }
    // On the bridge, what a recorded job reads besides its owner (bridge::Device::begin): the RenoDX stage it ran,
    // the runtime a carried history came from (carry). Kept until that job has run or is given up.
    struct JobUse { const void* what; const bridge::Job* job; uint64_t generation; };
    std::vector<JobUse> job_uses;
    const Runtime* carry_source{};   // set by carry, for record_bridge_job
    bool used_by_pending_job(const void* what);
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
    // `linear`: the colour is scene-referred linear light (an upscaler's input
    // in a float format), so the runtime encodes it; false for every swapchain.
    // Builds in the background: returns false, with `status` saying so, until
    // the network is ready, and the caller leaves the frame alone meanwhile.
    // `controls` asking for a preprocess makes every runtime from then on one
    // that can run it (prep_want); see RuntimeConfig::preprocess.
    bool ensure_runtime(const Controls& controls, uint32_t w, uint32_t h, VkFormat format, bool linear);
    bool ensure_runtime_(const Controls& controls, uint32_t w, uint32_t h, VkFormat format, bool linear);
    // The preprocess meter in the log every five seconds while it runs, so NR on/off at one spot
    // can be compared (the feedback loop that made upstream drop frame metering).
    std::chrono::steady_clock::time_point meter_logged{};
    static bool is_linear_format(VkFormat f) {
        return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
               f == VK_FORMAT_R32G32B32A32_SFLOAT ||
               // Taken only through the rest-of-DXGI colour formats
               // (nr_pe_interop.cpp colour_format_fallback), so no frame that
               // ran before changes its answer here.
               f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 || f == VK_FORMAT_R32G32B32_SFLOAT;
    }
    // The background build. Everything the thread needs is copied in; it hands
    // back either a runtime or an error under `build_lock`, and the render
    // thread adopts whichever on its next call.
    // Evicted networks wait here before they are destroyed. run_after and run_present record the network into
    // the game's own command list (vkd3d-proton's command buffer), which the game submits and the GPU runs frames
    // later: a network that served the previous frame can still be read by the GPU when its replacement is
    // adopted (the [Int4Mixed] switch, a resolution change). Destroying it then frees memory under in-flight work
    // and hangs the GPU. Freed after kRetireCalls more evaluations and kRetireTime, far past any frame in flight.
    struct Evicted { std::unique_ptr<Runtime> runtime; uint64_t call; std::chrono::steady_clock::time_point at; };
    std::vector<Evicted> retired;
    uint64_t calls{};   // ensure_runtime calls (one per evaluation)
    static constexpr uint64_t kRetireCalls = 16;
    static constexpr std::chrono::seconds kRetireTime{1};
    void free_retired();
    std::thread build_thread;
    std::mutex build_lock;
    bool building{}, build_done{}, build_linear{}, build_prep{}, build_native{};
    // [Network] Model: the network the switch asks for, the one being built, and the one running (Built::net).
    NetworkSwitch net_switch;
    int net_want{}, build_net{}, running_net{-1};
    // What can run in this run, checked once the game's device is known (NetworkSwitch::unavailable).
    bool availability_checked{};
    void check_availability();
    uint32_t build_passes{kMaxPasses};
    uint32_t build_w{}, build_h{};
    VkFormat build_format{VK_FORMAT_UNDEFINED};
    std::unique_ptr<Runtime> built;
    std::string build_error;
    std::chrono::steady_clock::time_point build_started{};
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
    bool linear{};
    float white_point{1.0f};
    // Model Resolution. A change rebuilds the network, like a resolution change.
    float model_scale{1.0f}, build_scale{1.0f}, built_scale{1.0f};
    bool native_compose{};   // Session::set_native_compose
    bool motion_point{};     // Session::set_motion_point
    // The RenoDX-style option on a route without native compose (the ReShade add-on): its frames ask for the
    // DLL's own composition too, so the network RenoDX's processing wraps is the same on every route.
    bool renodx_native{};
    bool running_native{};   // Built::native of `runtime`
    uint32_t running_passes{};   // Built::passes of `runtime`
    bool running_prep{};         // Built::prep of `runtime`
    // Controls::preprocess was asked for once: from then on every runtime is
    // built able to run it, so the hotkey switches at once after the first
    // time. Never cleared; until then nothing about the runtime changes.
    bool prep_want{};
    // Every runtime is built able to run this many passes; Controls::passes
    // picks the count per frame.
    // What a network is built for unless the host asks for something else
    // (set_max_passes: one pass is built for one, more for at least four). The
    // ceiling is 16 and raising it costs one model-sized history image per
    // pass, which is why it is not simply set there.
    static constexpr uint32_t kMaxPasses = 4;
    static constexpr uint32_t kPassCeiling = 16;
    uint32_t max_passes_want{kMaxPasses};
    bool ensure_own_command_buffer();
    bool ensure_d3d12_device(ID3D12Device* device);
    // The Vulkan device underneath is vkd3d-proton's, which always enables bufferDeviceAddress.
    bool vkd3d_device = false;

    // ---- the bridge (nr_pe_bridge.hpp): the native D3D runtime, our own Vulkan device ----
    enum class Mode { Unknown, Interop, Bridge };
    Mode mode{Mode::Unknown};
    bridge::Device* bdev{};
    // The frame's colour (in place), motion and depth as both APIs see them. The D3D side of each is
    // ours; the game's resources are copied in and the result copied back.
    bridge::Image shared_colour, shared_motion, shared_depth;
    bridge::Image shared_exposure;   // the RenoDX-style option's exposure texture (the OptiScaler route's)
    bridge::DepthCopyD3D12 depth12;
    bridge::DepthCopyD3D11 depth11;
    bool depth_warned{};
    // Shared images replaced by a resize, freed once nothing can still be using them.
    struct RetiredImage {
        bridge::Image image;
        uint64_t after{};
        uint64_t mark{~0ull};   // the hand-over queue's position once no job of ours could use it
    };
    // Runtimes whose recorded frames were dropped (bridge::Device::on_drop, any thread): their temporal
    // state is reset before their next recording. Shared, so a drop after the session is gone is harmless.
    struct Drops {
        std::mutex m;
        std::set<const void*> runtimes;
    };
    std::shared_ptr<Drops> drops = std::make_shared<Drops>();
    // A bridge job on the current runtime, with the drop hook set and any pending drop applied.
    bridge::Job* begin_job(std::string* why);
    // The ReShade D3D12 route's queue: its own lists and their fence belong to that queue alone.
    ID3D12CommandQueue* own_queue{};
    std::vector<RetiredImage> retired_images;
    uint64_t bridge_runs{};
    bool decide_mode(bool interop_present);
    bool ensure_bridge(const LUID& luid);
    bool ensure_shared(bridge::Image& image, uint32_t w, uint32_t h, DXGI_FORMAT format, ID3D12Device* d12,
                       ID3D11Device* d11, const char* what, ID3D10Device* d10 = nullptr);
    void sweep_retired_images();
    // The network over the shared images, recorded on our device. Null (with `status`) when it could not.
    bridge::Job* record_bridge_job(const EngineFrame& base, bool motion, bool depth, const Controls& controls,
                                   const BridgeRenoDX* renodx = nullptr);
    // The RenoDX-style option's network and stage for a bridged w x h frame in `format` whose guides are
    // guide_w x guide_h. False with `status` set: the frame passes through.
    bool bridge_renodx(const Session::RenoDXFrame& rf, uint64_t feature, uint32_t w, uint32_t h, VkFormat format,
                       uint32_t guide_w, uint32_t guide_h, const Controls& controls, BridgeRenoDX* out);
    // The ReShade D3D12 route's own command lists on the game's queue, two per frame.
    struct OwnList {
        ID3D12CommandAllocator* allocator{};
        ID3D12GraphicsCommandList* list{};
        uint64_t done{};
    };
    std::vector<OwnList> own_lists;
    ID3D12Fence* own_lists_fence{};
    uint64_t own_lists_value{};
    ID3D12GraphicsCommandList* next_own_list(ID3D12Device* device);
    void release_bridge();
    void record_copy_in(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* colour,
                        D3D12_RESOURCE_STATES colour_state, ID3D12Resource* motion, D3D12_RESOURCE_STATES motion_state,
                        ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource* exposure = nullptr,
                        D3D12_RESOURCE_STATES exposure_state = D3D12_RESOURCE_STATE_COMMON);
    // One set of shared images per extent in use (features before and after the upscale alternate).
    struct SharedSet {
        bridge::Image colour, motion, depth;
    };
    std::vector<SharedSet> parked_sets;
    void select_shared_set(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const void* device);
    void record_copy_out(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES target_state);
    bridge::Job* bridge_prepare_d3d12(ID3D12Device* device, ID3D12GraphicsCommandList* copy_list,
                                      const BridgeInputs& in, const Controls& controls,
                                      const std::function<bool()>& before_copy);
    bool bridge_d3d11(ID3D11Device* device, const D3D11Frame& frame, const Controls& controls);
    bool fits_in_memory(uint32_t w, uint32_t h, bool quiet = false);   // quiet: no log, no status
    // Session::Impl::memory_guard. False while the network is held off.
    bool memory_guard();
    struct Retired {
        Built entry;
        unsigned frames_left{};
        std::chrono::steady_clock::time_point not_before{};
    };
    std::vector<Retired> retiring;
    std::chrono::steady_clock::time_point guard_next{}, hold_until{};
    unsigned guard_trips{};
    // Make `entry` the one the run paths use. Returns false when there is no
    // such entry.
    bool select_runtime(uint32_t w, uint32_t h, VkFormat format, bool want_linear);
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
    void drop(size_t index, const char* why = "to make room");
    // Feature ids, handed out by Session::create_feature. Monotonic, never
    // reused, so a stale id can only ever miss.
    uint64_t next_feature{1};
    // NVIDIA's resize semantics (310.9, measured on an RTX 5090): a
    // feature evaluated at another extent than its last evaluation is a new feature at that extent - the frame is
    // byte-identical to the first frame of a feature created there; its history is not read. `resized` says so for
    // this evaluation (the caller ORs it into the frame's Reset) and remembers the extent.
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> feature_extent;
    uint64_t resizes{};
    // The extent each feature last asked a network for, run or not: a size change counts for drs_active even when the
    // frame passes through (the bridge hands the first frame over only once the game's command list can be cut, so
    // `resized` may see a feature's second size first, and the frames waiting for memory record nothing).
    std::map<uint64_t, std::pair<uint32_t, uint32_t>> asked_extent;
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
    // The arena is sized from the padded model extent (Model Resolution), the
    // frame-sized images from the frame; plus weights and scratch.
    const auto model = [&](uint32_t v) {
        return model_scale >= 1.0f ? v : std::max(8u, uint32_t(double(v) * model_scale + 0.5));
    };
    const uint64_t pixels = uint64_t((model(w) + 63) / 64 * 64) * ((model(h) + 63) / 64 * 64);
    const uint64_t need = pixels * arena_bytes_per_pixel() + uint64_t(w) * h * 48 + (300ull << 20);

    const MemoryState m = read_memory(handles.physical);
    const char* short_of = nullptr;
    uint64_t have = 0, reserve = 0;
    if (m.vram_known && need + kVramReserve > m.vram_free) {
        short_of = "video memory"; have = m.vram_free; reserve = kVramReserve;
    } else if (m.commit_limit && need + kCommitReserve > m.commit_free) {
        short_of = "system memory (RAM + page file)"; have = m.commit_free; reserve = kCommitReserve;
    }
    if (quiet) return !short_of;
    if (short_of) {
        char message[320];
        std::snprintf(message, sizeof message,
                      "the network needs about %.0f MB at %ux%u (model scale %.2f) and %.0f MB of %s is "
                      "free, %.0f MB of which stays with the game; not building it. A lower Model "
                      "Resolution, a larger page file or closing other programs makes room.",
                      mb(need), w, h, model_scale, mb(have), short_of, mb(reserve));
        status = message;
        log("[nr] %s", message);
        return false;
    }
    log("[nr] %ux%u (model scale %.2f) needs about %.0f MB; free: VRAM %.0f MB, commit %.0f MB", w, h,
        model_scale, mb(need), mb(m.vram_free), mb(m.commit_free));
    return true;
}

// **The running network steps aside when the machine runs short.** Checked at
// most twice a second. The game grows after the build was admitted - loading a
// zone, a resize, another program - and at the limit it is the game that gets
// the failed allocation. So below kVramLow / kCommitLow every resident network
// is retired: taken out of use at once (frames pass through) and destroyed a
// few frames later, when nothing the GPU still has in flight can reference it
// (on the paths that record into the game's command list there is no fence of
// ours to wait on). A rebuild then has to pass fits_in_memory with its reserves,
// and not before a hold that doubles each time, so a machine that stays short
// is not rebuilt into again and again.
bool Session::Impl::memory_guard() {
    const auto now = std::chrono::steady_clock::now();
    // Retired networks, destroyed once they are surely idle.
    for (auto it = retiring.begin(); it != retiring.end();) {
        if (it->frames_left) --it->frames_left;
        // On the bridge, not while a recorded or running frame of ours still uses it.
        if (!it->frames_left && now >= it->not_before &&
            !(bdev && (bdev->busy(it->entry.runtime.get()) || used_by_pending_job(it->entry.runtime.get())))) {
            wait_for_own_cmd();
            it = retiring.erase(it);
        } else {
            ++it;
        }
    }
    if (now < guard_next) return now >= hold_until;
    guard_next = now + std::chrono::milliseconds(500);
    if (cache.empty() && parked.empty()) return now >= hold_until;
    const MemoryState m = read_memory(handles.physical);
    const bool vram_low = m.vram_known && m.vram_free < kVramLow;
    const bool commit_low = m.commit_limit && m.commit_free < kCommitLow;
    if (!vram_low && !commit_low) return now >= hold_until;

    const unsigned hold = std::min(10u << std::min(guard_trips, 4u), 160u);
    ++guard_trips;
    log("[nr] memory is running out (VRAM %.0f MB free of %.0f, commit %.0f MB free of %.0f): "
        "the network steps aside for %u s; %u network(s) released, %u parked one(s) freed",
        mb(m.vram_free), mb(m.vram_budget), mb(m.commit_free), mb(m.commit_limit), hold,
        unsigned(cache.size()), unsigned(parked.size()));
    // The networks kept for a switch back ([Network] Residency) go as well: their memory (video memory, or RAM, which
    // WDDM charges to commit too) is held by their cores and freed with the last runtime of each.
    parked.clear();
    for (auto& e : cache) {
        Retired r;
        r.entry = std::move(e);
        r.frames_left = 16;
        r.not_before = now + std::chrono::seconds(2);
        retiring.push_back(std::move(r));
    }
    cache.clear();
    runtime = nullptr; width = height = 0;
    hold_until = now + std::chrono::seconds(hold);
    return false;
}

bool Session::Impl::select_runtime(uint32_t w, uint32_t h, VkFormat format, bool want_linear) {
    for (auto& e : cache) {
        if (e.runtime->stale()) continue;   // its images belong to a later runtime (dropped by trim_cache)
        if (e.width != w || e.height != h || e.format != format || e.linear != want_linear ||
            e.scale != model_scale || e.passes != max_passes_want || e.prep != prep_want ||
            e.native != (native_compose || renodx_native))
            continue;
        if (e.net != net_want) continue;   // [Network] Model: the network the switch asks for
        e.used = ++use_stamp;
        const auto now = std::chrono::steady_clock::now();
        if (runtime != e.runtime.get()) {
            const bool stale = e.last != std::chrono::steady_clock::time_point{} && now - e.last > kStaleHistory;
            if (stale) e.runtime->forget_history();
            log("[nr] runtime cache hit: %ux%u (model %ux%u, scale %.2f%s), %u built, "
                "%u live features%s", e.width, e.height, e.runtime->model_width(),
                e.runtime->model_height(), e.scale, e.linear ? ", linear" : "",
                unsigned(cache.size()), e.runtime->live_features(), stale ? ", history started afresh" : "");
        }
        e.last = now;
        runtime = e.runtime.get();
        width = e.width; height = e.height; this->format = e.format;
        linear = e.linear; built_scale = e.scale; running_native = e.native;
        running_passes = e.passes; running_prep = e.prep;
        running_net = e.net;
        return true;
    }
    return false;
}

void Session::Impl::drop(size_t index, const char* why) {
    auto& e = cache[index];
    log("[nr] evicting the %ux%u network %s", e.width, e.height, why);
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
        if (calls - retired[i].call < kRetireCalls || now - retired[i].at < kRetireTime) continue;
        // On the bridge a recorded frame runs when its command list does, which the game decides: not
        // while one that uses this network is recorded and not yet run, or still running.
        if (bdev && (bdev->busy(retired[i].runtime.get()) || used_by_pending_job(retired[i].runtime.get()))) continue;
        { std::lock_guard<std::mutex> guard(drops->m); drops->runtimes.erase(retired[i].runtime.get()); }
        retired.erase(retired.begin() + long(i));
        log("[nr] evicted network freed");
    }
}

bool Session::Impl::vram(Vram& v) const {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    props.pNext = &budget;
    if (!handles.physical) return false;
    vkGetPhysicalDeviceMemoryProperties2(handles.physical, &props);
    const auto& mem = props.memoryProperties;
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
            p.format = e.format; p.linear = e.linear; p.scale = e.scale; p.passes = e.passes; p.prep = e.prep;
            p.native = e.native; p.net = e.net; p.width = e.width; p.height = e.height; p.last = e.last;
            parked.push_back(std::move(p));
            log("[nr] %s at %ux%u parked: its weights stay, its runtimes and images go %s", nr::network_codename(e.net),
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
    if (offload_thread.joinable()) offload_thread.join();
    if (offload_ok) {
        release_network(*offloading, std::move(offload_weights));
        offload_ok = network_offloaded(*offloading);
    }
    offload_weights.clear();
    for (const auto& p : parked)
        if (p.core == offloading)
            log("[nr] %s at %ux%u: %s", nr::network_codename(p.net), p.width, p.height,
                offload_ok ? "its weights moved to RAM, its video memory freed" : "its weights could not be moved to RAM");
    if (!offload_ok)   // not tried again: freed instead
        for (size_t i = parked.size(); i-- > 0;)
            if (parked[i].core == offloading) parked.erase(parked.begin() + long(i));
    offloading.reset();
}

void Session::Impl::residency(bool switched, bool nr_off) {
    const auto now = std::chrono::steady_clock::now();
    residency_checked = now;
    poll_offload();
    const int mode = net_switch.file().residency;   // 0 auto, 1 resident, 2 offload, 3 minimal
    // Of another kind and not used for a second: right after a switch the left network may still serve a feature whose
    // new network is not built yet (net_only, touch_running).
    auto inactive = [&](const Built& e) { return nr_off || (e.net != running_net && now - e.last > std::chrono::seconds(1)); };
    if (mode == 1) return;
    if (mode == 3) {
        for (size_t i = cache.size(); i-- > 0;)
            if (inactive(cache[i]))
                drop(i, nr_off ? "(NR off; [Network] Residency frees it)" : "(another network runs; [Network] Residency frees it)");
        if (!parked.empty() && !offloading) { parked.clear(); log("[nr] parked networks freed ([Network] Residency)"); }
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
            log("[nr] parked %s freed (RAM is short)", nr::network_codename(parked[lru].net));
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
            if (!reloading(parked[i]) && network_offloaded(*parked[i].core) && (lru == parked.size() || parked[i].last < parked[lru].last)) lru = i;
        if (lru < parked.size()) {
            log("[nr] %s freed from RAM (RAM is short)", nr::network_codename(parked[lru].net));
            parked.erase(parked.begin() + long(lru));
        }
    }
    // video memory: the least recently used parked network still in it moves to RAM - not the one NR asks for now
    // (NR back on: it is about to run again)
    size_t lru = parked.size();
    for (size_t i = 0; i < parked.size(); ++i)
        if (!reloading(parked[i]) && !network_offloaded(*parked[i].core) && parked[i].core != offloading && (nr_off || parked[i].net != net_want) &&
            (lru == parked.size() || parked[i].last < parked[lru].last))
            lru = i;
    if (lru == parked.size()) return;
    const bool old = std::chrono::duration<double>(now - parked[lru].last).count() > keep;
    if (!short_now && !old) return;
    if (ram_short()) {
        log_state();
        log("[nr] parked %s at %ux%u freed (%s; RAM is short too)", nr::network_codename(parked[lru].net),
            parked[lru].width, parked[lru].height, short_now ? "video memory is short" : "video memory is getting full");
        parked.erase(parked.begin() + long(lru));
    } else if (start_offload(parked[lru])) {
        log_state();
        log("[nr] parked %s at %ux%u: moving its weights to RAM (%s)", nr::network_codename(parked[lru].net),
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
            struct Use { uint64_t used{}; std::chrono::steady_clock::time_point last{}; };
            std::map<const void*, Use> nets;
            size_t entries = 0, oldest = cache.size();
            for (size_t i = 0; i < cache.size(); ++i) {
                if (!of_kind(cache[i])) continue;
                ++entries;
                Use& u = nets[cache[i].runtime->network_id()];
                u.used = std::max(u.used, cache[i].used); u.last = std::max(u.last, cache[i].last);
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
                    if (!id || (adopting && id == adopting->runtime->network_id())) continue;
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
                for (const auto& [id, u] : nets) if (u.used < v.used) { v = u; victim = id; }
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
    if (fits_in_memory(w, h)) return true;
    if (cache.empty()) return false;
    size_t oldest = 0;
    for (size_t i = 1; i < cache.size(); ++i)
        if (cache[i].used < cache[oldest].used) oldest = i;
    // One at a time: an evicted network keeps its memory until it is freed (see Retired), and the build is asked
    // for again once it has been.
    drop(oldest);
    status = "freeing the least recently used network to make room";
    return false;
}


bool Session::Impl::ensure_runtime(const Controls& controls, uint32_t w, uint32_t h, VkFormat format,
                                   bool want_linear) {
    {
        auto& e = asked_extent[current_feature];
        if (e.first && (e.first != w || e.second != h)) last_resize = std::chrono::steady_clock::now();
        e = {w, h};
    }
    const bool ok = ensure_runtime_(controls, w, h, format, want_linear);
    ++(ok ? ran : passed);
    last_w = w; last_h = h; last_format = format; last_linear = want_linear;
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
            if (passed)
                log("[nr] last %.0f s: %llu evaluations ran the network, %llu passed the frame through (%s)",
                    std::chrono::duration<double>(now - pass_logged).count(), (unsigned long long)ran,
                    (unsigned long long)passed, ok ? "running now" : status.c_str());
            ran_total += ran; passed_total += passed; ran = passed = 0;
            pass_logged = now;
        }
    }
    if (ok && controls.preprocess.active() && controls.preprocess.exposure == 1) {
        const auto now = std::chrono::steady_clock::now();
        const auto m = runtime->preprocess_meter();
        if (now - meter_logged > std::chrono::seconds(5) && m.first == m.first) {
            meter_logged = now;
            const float bias = controls.preprocess.bias_ev;
            log("[nr] preprocess exposure: auto %+.2f EV (this frame asks %+.2f) + ExposureBias %+.2f = %+.2f EV",
                m.first, m.second, bias, m.first + bias);
        }
    }
    return ok;
}

bool Session::Impl::ensure_runtime_(const Controls& controls, uint32_t w, uint32_t h, VkFormat format,
                                    bool want_linear) {
    ++calls;
    last_call = std::chrono::steady_clock::now();
    if (last_call - residency_checked >= kResidencyCheck) residency(false, false);
    if (!retired.empty()) free_retired();
    if (cache.size() > kMaxResident) trim_cache();
    if (controls.preprocess.active() && !prep_want) {
        prep_want = true;
        log("[nr] preprocess asked for: networks are rebuilt able to run it (once)");
    }
    if (!availability_checked) check_availability();
    net_want = net_switch.frame(root + "\\dlssnr-amd.ini");
    // Adopt a finished background build first.
    {
        std::lock_guard<std::mutex> guard(build_lock);
        if (build_done) {
            if (build_thread.joinable()) build_thread.join();
            build_done = false; building = false;
            if (build_plan) {
                // A plan (RuntimeConfig::plan_only): cached; its runtimes are built inside the frames that ask.
                build_plan = false;
                if (built && built->plan()) {
                    PlanEntry pe;
                    pe.format = build_format; pe.linear = build_linear; pe.scale = build_scale; pe.passes = build_passes;
                    pe.prep = build_prep; pe.native = build_native;
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
                entry.linear = build_linear; entry.scale = build_scale;
                entry.passes = build_passes; entry.prep = build_prep; entry.native = build_native;
                // what was built, not what was asked: a network without what it needs is another one (the runtime
                // says why); the one asked for is not asked again this run
                entry.net = net_of(*built);
                if (entry.net != build_net) {
                    net_switch.unavailable(build_net, built->network_note().empty() ? "could not be built (see dlssnr-amd.log)"
                                                                                    : built->network_note());
                    net_want = net_switch.frame(root + "\\dlssnr-amd.ini");
                }
                entry.used = ++use_stamp;
                entry.last = std::chrono::steady_clock::now();
                entry.runtime = std::move(built);
                // The pass count went back while this one built, and the running network is the one wanted again:
                // this one is not wanted; the running one keeps serving.
                const bool unwanted = entry.passes != max_passes_want && runtime && running_passes == max_passes_want &&
                                      width == entry.width && height == entry.height && this->format == entry.format;
                if (unwanted) {
                    log("[nr] the network built for %u passes is not wanted any more (the pass count changed back)",
                        entry.passes);
                    retired.push_back({std::move(entry.runtime), calls, std::chrono::steady_clock::now()});
                    build_share.reset();
                } else {
                log("[nr] network built at %ux%u (model %ux%u, scale %.2f) on queue family %u in "
                    "%.1f s%s; %u networks resident",
                    entry.width, entry.height, entry.runtime->model_width(),
                    entry.runtime->model_height(), entry.scale, access.family, build_seconds,
                    entry.linear ? "; the colour is linear light and is encoded for the network" : "",
                    unsigned(cache.size() + 1));
                // A network built without the preprocess at this extent is
                // never selected again; its memory goes now. Its last frame
                // was before this build started, a second or more ago.
                for (size_t i = cache.size(); i-- > 0;)
                    if (entry.prep && !cache[i].prep && cache[i].width == entry.width &&
                        cache[i].height == entry.height)
                        drop(i, "built without the preprocess");
                // A change of pass count: the network built for the other count served until now and keeps no
                // memory once this one is in.
                for (size_t i = cache.size(); i-- > 0;)
                    if (cache[i].passes != entry.passes && cache[i].width == entry.width &&
                        cache[i].height == entry.height && cache[i].format == entry.format &&
                        cache[i].linear == entry.linear && cache[i].scale == entry.scale &&
                        cache[i].prep == entry.prep && cache[i].native == entry.native)
                        drop(i, "(built for another pass count)");
                // Model resolution is one value for the whole session: a network built for another one can never
                // be selected again unless the setting goes back, and kept, every value tried stayed resident
                // (ReShade route, 1080p, 100% -> 50% in steps: 6 networks, 865 -> 3288 MiB).
                for (size_t i = cache.size(); i-- > 0;)
                    if (cache[i].scale != entry.scale) drop(i, "(built for another model resolution)");
                trim_cache(&entry);
                cache.push_back(std::move(entry));
                auto& adopted = cache.back();
                // a parked network in use again: its runtime holds it now
                for (size_t i = parked.size(); i-- > 0;)
                    if (core_network(*parked[i].core) == adopted.runtime->network_id()) parked.erase(parked.begin() + long(i));
                build_share.reset();   // (held only for a failed build)
                runtime = adopted.runtime.get();
                width = adopted.width; height = adopted.height; this->format = adopted.format;
                linear = adopted.linear; built_scale = adopted.scale; running_native = adopted.native;
                running_passes = adopted.passes; running_prep = adopted.prep;
                running_net = adopted.net;
                // A network switch: what happens to the networks it left ([Network] Residency)
                residency(true, false);
                if (Vram v; vram(v))
                    log("[nr] %s serving; %zu runtimes, %zu parked; this process's video memory %.0f MiB (budget %.0f MiB)",
                        nr::network_codename(running_net), cache.size(), parked.size(), double(v.usage) / (1 << 20),
                        double(v.budget) / (1 << 20));
                }
                oom_failures = 0;
                retry_at = {};
            } else {
                // a runtime from a parked network that failed: the network goes (its weights may be half back)
                for (size_t i = parked.size(); i-- > 0;)
                    if (build_share && parked[i].core == build_share) parked.erase(parked.begin() + long(i));
                build_share.reset();
                status = build_error;
                log("[nr] %s", status.c_str());
                if (build_oom && oom_failures < kOomRetries) {
                    // 4, 8, 16, 32 seconds. Long enough that the load or the
                    // resolution change that took the address space has
                    // finished, short enough that a player does not give up on
                    // the feature first.
                    const unsigned wait = 4u << oom_failures;
                    ++oom_failures;
                    retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
                    log("[nr] retrying the build in %u s (attempt %u of %u)", wait, oom_failures,
                        kOomRetries);
                } else {
                    failed = true;
                }
                return false;
            }
        }
    }
    if (!memory_guard()) {
        status = "memory is short; the network is held off and frames pass through";
        return false;
    }
    if (select_runtime(w, h, format, want_linear)) return true;
    // The resize path: a new frame size from a resident network or a cached plan, inside this recording.
    std::shared_ptr<const RuntimeCore> tried;
    inframe_waiting = false;
    if (inframe(w, h, format, want_linear, &tried)) return true;
    if (inframe_waiting) {
        status = "making memory for the game's changing frame size; frames pass through until it is ready";
        return false;
    }
    // A network switch: the running network keeps serving its frames while the other one builds, so the
    // comparison has no gap but the build itself (the first int4 build also compiles its pipelines).
    const bool net_only = runtime && running_net >= 0 && running_net != net_want && width == w && height == h &&
                           this->format == format && linear == want_linear && built_scale == model_scale &&
                           running_native == (native_compose || renodx_native);
    // A change of pass count (set_max_passes): the running network serves, at most at the passes it was built for,
    // while the one for the new count builds.
    static constexpr const char* kPassesServing =
        "building the network for the new pass count in the background; the running one serves until it is ready";
    const bool passes_only = runtime && running_passes != max_passes_want && width == w && height == h &&
                             this->format == format && linear == want_linear && built_scale == model_scale &&
                             running_native == (native_compose || renodx_native) && running_prep == prep_want
                             && running_net == net_want
        ;
    if (building) {
        if (net_only) { touch_running(); return true; }
        if (passes_only) { touch_running(); status = kPassesServing; return true; }
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
    // An evicted network still holds its memory until it is freed. If the next one does not fit beside it, wait
    // for that rather than push out another: the switch keeps the running network serving, anything else passes
    // through.
    // The resize path: a cached network of the same working extent and configuration gives this extent its network
    // (Runtime::share_for) - only the images and descriptor sets are made, about 10 ms instead of a whole build, and no
    // more arena memory.
    std::shared_ptr<const RuntimeCore> share = tried;
    for (const auto& e : cache) {
        if (share) break;
        if (!same_config(e, format, want_linear) || e.runtime->stale()) continue;
        if ((share = e.runtime->share_for(w, h)))
            log("[nr] %ux%u has the working extent of the %ux%u network: built from it", w, h, e.width, e.height);
    }
    // A parked network of this configuration (Residency auto): its memory is still there, only a runtime is made. The
    // runtime checks the core fits this extent and makes a whole network when it does not (the log says so).
    poll_offload();
    for (const auto& p : parked) {
        if (share) break;
        if (p.net != net_want || p.format != format || p.linear != want_linear || p.scale != model_scale ||
            p.passes != max_passes_want || p.prep != prep_want || p.native != (native_compose || renodx_native) ||
            !core_runs(*p.core, w, h, model_scale, native_compose || renodx_native))
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
    if (!share && start_plan(w, h, format, want_linear)) {
        if (net_only) { touch_running(); return true; }
        if (passes_only) { touch_running(); status = kPassesServing; return true; }
        status = "building the plan for this frame size in the background; frames pass through until it is ready";
        return false;
    }
    // The size must hold before a whole network is built for it (a size moving every frame asked for one per frame) -
    // unless no network of this configuration is resident (the first; the plans need one), or a plan for it exists and
    // only no resident memory holds it (a sweep past the biggest network so far).
    bool have_network = false;
    const Built* any_net = nullptr;
    for (const auto& e : cache)
        if (same_config(e, format, want_linear) && !e.runtime->stale()) { have_network = true; any_net = &e; break; }
    const bool capacity_miss = any_net && plan_for(w, h, format, want_linear, any_net->runtime->network()) != nullptr;
    if (!share && have_network && !capacity_miss && !settled(w, h)) {
        if (net_only) { touch_running(); return true; }
        if (passes_only) { touch_running(); status = kPassesServing; return true; }
        status = "the frame's size is changing; the network is built once it holds";
        return false;
    }
    if (!share && !retired.empty() && !fits_in_memory(w, h, true)) {
        if (net_only) { touch_running(); return true; }
        if (passes_only) { touch_running(); status = kPassesServing; return true; }
        status = "freeing the previous network before building another";
        return false;
    }
    // a parked network whose weights are in RAM needs room too (the weights, perhaps an arena)
    if ((!share || network_offloaded(*share)) && !make_room(w, h)) return false;
    RuntimeConfig config = make_config(w, h, format, want_linear);
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
    const bool native = config.native_compose;
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
    building = true; build_w = w; build_h = h; build_format = format; build_linear = want_linear;
    build_started = std::chrono::steady_clock::now();
    nr::g_build_stage = 1; nr::g_build_pipes_done = 0; nr::g_build_pipes_total = 0;
    building = true; build_w = w; build_h = h; build_format = format; build_linear = want_linear;
    build_scale = model_scale; build_passes = max_passes_want; build_prep = prep_want; build_native = native;
    build_net = net_want;
    status = "building the network in the background; frames pass through until it is ready";
    log("[nr] network asked for: %s", nr::network_codename(net_want));
    VkPhysicalDeviceProperties gpu{};
    vkGetPhysicalDeviceProperties(handles.physical, &gpu);
    log("[nr] building the network at %ux%u (model scale %.2f) on %s in the background%s%s", w, h, model_scale,
        gpu.deviceName,
        want_linear ? " (linear-light colour: encoding with a white point, see dlssnr-amd.ini white_point)" : "",
        nr::binary::directory().empty() ? "" : ", ACO's machine code ([Network] ACO Mode)");
    {
        static std::string said;
        const std::string why = nr::binary::disabled_why();
        if (!why.empty() && why != said) { said = why; log("[nr] [Network] ACO Mode is off for this run: %s", why.c_str()); }
    }
    build_thread = std::thread([this, host, config, temporal, access_copy] {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Runtime> made;
        std::string error;
        bool oom = false;
        // The queue lock around each submit of the build, not around the
        // build: held for the whole of it, the pipeline compile (14-24 s
        // cold at 4K under AMD's Windows compiler) blocked vkd3d's own
        // submissions and presents on the shared queue, and the game froze
        // for as long. **Quiet**: this is not the render thread, and the
        // flush half of the other lock drives DXVK's immediate context,
        // which that thread owns. See QueueAccess::lock_quiet.
        HostDevice locked = host;
        locked.queue_lock = access_copy.lock_quiet;
        locked.queue_unlock = access_copy.unlock_quiet;
        try {
            // a parked network whose weights went to RAM (Residency offload): back first
            if (config.share && network_offloaded(*config.share)) {
                const auto r0 = std::chrono::steady_clock::now();
                if (!reload_network(locked, *config.share, config.act_arena, config.act_slack))
                    throw std::runtime_error("the network's weights could not be brought back from RAM");
                log("[nr] network weights back from RAM in %.0f ms",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count());
            }
            // ACO Mode's network did not build: once more with the driver's compiler (a runtime from a parked
            // network keeps that network's machine code: no fallback there).
            const auto driver_compiler = [&](const std::exception& e, const RuntimeConfig& c) {
                if (nr::binary::directory().empty() || c.share) throw;
                nr::binary::disable(std::string("the ACO network did not build (") + e.what() + ")");
                log("[nr] [Network] ACO Mode: %s; building with the driver's compiler", nr::binary::disabled_why().c_str());
                made = std::make_unique<Runtime>(locked, c, ControlMaskConfig{}, temporal);
            };
            try {
                made = std::make_unique<Runtime>(locked, config, ControlMaskConfig{}, temporal);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& e) {
#if NR_INT4
                if (config.int4_mixed == 1) {
                    log("[nr] int4 mixed network failed: %s; building the default network", e.what());
                    RuntimeConfig fallback = config;
                    fallback.int4_mixed = 0;
                    // A driver that takes no ACO machine code fails the int4 network and then this one as well.
                    try {
                        made = std::make_unique<Runtime>(locked, fallback, ControlMaskConfig{}, temporal);
                    } catch (const std::bad_alloc&) {
                        throw;
                    } catch (const std::exception& e2) {
                        driver_compiler(e2, fallback);
                    }
                } else
#endif
                    driver_compiler(e, config);
            }
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
    if (passes_only && runtime != nullptr) {   // a change of pass count, the same
        touch_running();
        status = kPassesServing;
        return true;
    }
    return false;
}

RuntimeConfig Session::Impl::make_config(uint32_t w, uint32_t h, VkFormat f, bool want_linear) const {
    RuntimeConfig config;
    config.root = root; config.width = w; config.height = h; config.colour_format = f;
    config.linear_input = want_linear; config.white_point = white_point;
    config.model_scale = model_scale;
    const bool native = native_compose || renodx_native;
    config.max_passes = native ? 1u : max_passes_want;
    config.native_compose = native;
    config.motion_point = motion_point;
    config.preprocess = prep_want;
    config.network = net_want == nr::kSmallFp8 ? 1 : 0;   // RuntimeConfig::network: 1 the small model, 0 the 310.8 one
#if NR_INT4
    config.int4_mixed = net_want == nr::kSilverAardwoldInt4 ? 1 : 0;
#endif
    // The soft knee to undo: the linear path's own encode, or OptiScaler's
    // linear-HDR encode, which a float proxy on the OptiScaler route came
    // through (OptiScaler encodes a float colour flagged IsHDR or AutoExposure
    // and hands an SDR one over as it is, and only the format reaches us).
    config.preprocess_unknee = want_linear || (native && is_linear_format(f));
    if (native) config.model_scale = 1.0f;   // the DLL has no resample; OptiScaler scales outside
    return config;
}

HostDevice Session::Impl::make_host() const {
    HostDevice host{};
    host.instance = handles.instance; host.physical = handles.physical; host.device = handles.device;
    // A real queue and its real family. Construction submits the weight upload
    // and waits for it; the game is submitting to the same underlying VkQueue,
    // so each of those submits takes the queue lock (the build thread's).
    host.queue = access.queue; host.queue_family = access.family;
    host.buffer_device_address = vkd3d_device || mode == Mode::Bridge ||
                                 vkdevice::network_features_added(handles.device);
    return host;
}

// The cached plan that runs a frame of w x h: its working extent, tall enough. Null: none yet.
const Session::Impl::PlanEntry* Session::Impl::plan_for(uint32_t w, uint32_t h, VkFormat f, bool want_linear,
                                                        NetworkModel model) {
    const bool native = native_compose || renodx_native;
    uint32_t ww = 0, wh = 0;
    bool known = false;
    for (auto& p : plans) {
        if (p.format != f || p.linear != want_linear || p.scale != model_scale || p.passes != max_passes_want ||
            p.prep != prep_want || p.native != native || p.net != net_want || h > p.sh)
            continue;
        if (!known) { working_extent(model, native ? 1.0f : model_scale, w, h, &ww, &wh); known = true; }
        if (p.ww == ww && p.wh == wh) { p.used = ++use_stamp; return &p; }
    }
    return nullptr;
}

// A frame at a size no resident runtime serves, run at once: a runtime built inside this recording from a resident
// network of its working extent (Runtime::share_for) or from a cached plan in a resident network's memory
// (Runtime::with_slot_of) - the memory least recently used first, so OptiScaler's NR before and after the upscaler
// keep a memory each. False: nothing fits; the caller builds in the background.
bool Session::Impl::inframe(uint32_t w, uint32_t h, VkFormat f, bool want_linear,
                           std::shared_ptr<const RuntimeCore>* tried) {
    const bool native = native_compose || renodx_native;
    if (!native && model_scale != 1.0f) return false;   // Model Resolution below 100% submits at construction
    // This configuration's resident networks, least recently used first.
    std::vector<Built*> nets;
    for (auto& e : cache)
        if (same_config(e, f, want_linear) && !e.runtime->stale()) nets.push_back(&e);
    // none for this colour format: a network of another format serves it (the runtime checks it can - the format only
    // sizes its images), as a game switching its colour buffer's format at a camera cut asks
    if (nets.empty())
        for (auto& e : cache)
            if (e.format != f && same_config_but_format(e, want_linear) && !e.runtime->stale()) nets.push_back(&e);
    if (nets.empty()) return false;
    // The memory this feature ran in last first, then the least recently used.
    const auto pref = feature_slot.find(current_feature);
    const void* want = pref == feature_slot.end() ? nullptr : pref->second;
    std::sort(nets.begin(), nets.end(), [want](const Built* a, const Built* b) {
        const bool pa = a->runtime->network_id() == want, pb = b->runtime->network_id() == want;
        return pa != pb ? pa : a->used < b->used;
    });
    std::shared_ptr<const RuntimeCore> core;
    for (Built* e : nets)
        if ((core = e->runtime->share_for(w, h))) break;
    if (!core)
        if (const PlanEntry* p = plan_for(w, h, f, want_linear, nets.front()->runtime->network()))
            for (Built* e : nets)
                if ((core = e->runtime->with_slot_of(p->core))) break;
    if (!core) return false;
    // No heap in that memory yet: a couple of runtimes may take memory of their own meanwhile; then the frames pass
    // through until the heap is made (heap_poll), rather than each new size holding its images until retired.
    if (!image_heap_fits(*core, w, h)) {
        size_t own = 0;
        for (const auto& c : cache) own += c.light && c.runtime->owns_image_memory();
        for (const auto& r : retired) own += r.runtime && r.runtime->owns_image_memory();
        if (own >= kOwnImages) { inframe_waiting = true; return false; }
    }
    RuntimeConfig config = make_config(w, h, f, want_linear);
    config.share = core;
    config.deferred = true;
    std::unique_ptr<Runtime> made;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        made = std::make_unique<Runtime>(make_host(), config, ControlMaskConfig{}, make_temporal());
    } catch (const std::exception& e) {
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
    entry.width = w; entry.height = h; entry.format = f; entry.linear = want_linear; entry.scale = model_scale;
    entry.passes = max_passes_want; entry.prep = prep_want; entry.native = native;
    entry.net = net_of(*made);
    entry.used = ++use_stamp;
    entry.last = std::chrono::steady_clock::now();
    entry.light = true;
    entry.runtime = std::move(made);
    trim_cache(&entry);
    cache.push_back(std::move(entry));
    auto& adopted = cache.back();
    runtime = adopted.runtime.get();
    width = adopted.width; height = adopted.height; this->format = adopted.format;
    linear = adopted.linear; built_scale = adopted.scale; running_native = adopted.native;
    running_passes = adopted.passes; running_prep = adopted.prep;
    running_net = adopted.net;
    return true;
}

bool Session::Impl::start_plan(uint32_t w, uint32_t h, VkFormat f, bool want_linear) {
    const bool native = native_compose || renodx_native;
    if (!native && model_scale != 1.0f) return false;
    const Built* net = nullptr;
    for (const auto& e : cache)
        if (same_config(e, f, want_linear) && !e.runtime->stale()) { net = &e; break; }
    if (!net) return false;
    const NetworkModel model = net->runtime->network();
    if (plan_for(w, h, f, want_linear, model)) return false;   // there is one: no memory holds it (a whole build)
    RuntimeConfig config = make_config(w, h, f, want_linear);
    config.plan_only = true;
    const TemporalConfig temporal = make_temporal();
    const HostDevice host = make_host();
    building = true; build_plan = true;
    build_w = w; build_h = h; build_format = f; build_linear = want_linear;
    build_scale = model_scale; build_passes = max_passes_want; build_prep = prep_want; build_native = native;
    build_net = net_want;
    build_started = std::chrono::steady_clock::now();
    build_thread = std::thread([this, host, config, temporal, model]() mutable {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Runtime> made;
        std::string error;
        try {
            // lowered at the tallest frame of this working extent: it runs every height of it
            config.height = plan_height(model, config.native_compose ? 1.0f : config.model_scale, config.width,
                                        config.height);
            made = std::make_unique<Runtime>(host, config, ControlMaskConfig{}, temporal);
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
    const bool native = native_compose || renodx_native;
    if (!native && model_scale != 1.0f) return;
    if (prefetched_around == std::make_pair(last_w, last_h)) return;
    const Built* net = nullptr;
    for (const auto& e : cache)
        if (same_config(e, last_format, last_linear) && !e.runtime->stale()) { net = &e; break; }
    if (!net || plans.size() >= kMaxPlans) return;
    // The working extents planned already for this configuration.
    std::vector<std::pair<uint32_t, uint32_t>> covered;
    for (const auto& p : plans)
        if (p.format == last_format && p.linear == last_linear && p.scale == model_scale && p.passes == max_passes_want &&
            p.prep == prep_want && p.native == native && p.net == net_want)
            covered.push_back({p.ww, p.wh});
    for (const auto& e : cache)
        if (same_config(e, last_format, last_linear)) {
            uint32_t ww = 0, wh = 0;
            if (const auto c = e.runtime->plan()) { uint32_t sh = 0; plan_extent(*c, &ww, &wh, &sh); covered.push_back({ww, wh}); }
        }
    const NetworkModel model = net->runtime->network();
    RuntimeConfig config = make_config(last_w, last_h, last_format, last_linear);
    config.plan_only = true;
    const TemporalConfig temporal = make_temporal();
    const HostDevice host = make_host();
    building = true; build_plan = true;
    build_w = last_w; build_h = last_h; build_format = last_format; build_linear = last_linear;
    build_scale = model_scale; build_passes = max_passes_want; build_prep = prep_want; build_native = native;
    build_net = net_want;
    build_started = std::chrono::steady_clock::now();
    const std::pair<uint32_t, uint32_t> around{last_w, last_h};
    build_thread = std::thread([this, host, config, temporal, model, covered, around]() mutable {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<Runtime> made;
        std::string error;
        try {
            // The nearest working extent not planned, along the frame's aspect, within half to one and a half its size.
            const float scale = config.native_compose ? 1.0f : config.model_scale;
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
                    made = std::make_unique<Runtime>(host, config, ControlMaskConfig{}, temporal);
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
            if (heap_thread.joinable()) heap_thread.join();
            for (auto& e : cache)
                if (e.runtime->network_id() == heap_for) { e.runtime->adopt_image_heap(heap_made); break; }
            if (!heap_made) log("[nr] the image heap for the changing frame size could not be made");
            heap_made.reset();
        }
        if (heap_busy) return;
    }
    if (!runtime || !drs_active()) return;
    const uint64_t region = runtime->image_heap_wanted();
    if (!region) return;
    heap_busy = true; heap_for = runtime->network_id();
    const HostDevice host = make_host();
    heap_thread = std::thread([this, host, region] {
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
    for (const auto& r : retired) alive |= r.runtime.get() == prev;   // (on the bridge kept for this job: job_uses)
    if (!alive) return false;
    const bool ok = runtime->carry_history(cmd, *prev, feature);
    if (ok) carry_source = prev;
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
        net_switch.unavailable(nr::kSmallFp8, "needs the model of the 310.9.16 nvngx_dlssnr.dll (install.ps1 -Dll)");
    if (!exists("dlssnr-amd\\dlssnr.bin"))
        net_switch.unavailable(nr::kSilverAardwold, "the model file is missing (run install.bat again)");
#if NR_INT4
    if (!exists("dlssnr-amd\\dlssnr.bin"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "the model file is missing (run install.bat again)");
    else if (!exists("dlssnr-amd\\int4\\settings.txt"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "not installed (run install.bat again)");
    else if (!exists("dlssnr-amd\\dlssnr-int4.bin"))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "its weights were not made at install (run install.bat again)");
    else if (nr::binary::directory().empty())
        net_switch.unavailable(nr::kSilverAardwoldInt4, nr::binary::disabled_why().empty()
            ? "needs [Network] ACO Mode = 1 (takes effect after a game restart)"
            : "needs ACO Mode, which is off for this run: " + nr::binary::disabled_why());
    else if (!nr::binary::enabled(handles.device))
        net_switch.unavailable(nr::kSilverAardwoldInt4, "the game's device has no pipeline binaries (ACO Mode)");
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
// bound anything". It is not on any interface; it runs when vkd3d dispatches one
// of its own meta shaders. ClearUnorderedAccessViewUint takes that path whenever
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
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
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
        vkd3d_device = handles.valid();
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

// ---- the bridge -------------------------------------------------------------------------------------

bool Session::Impl::decide_mode(bool interop_present) {
    if (mode != Mode::Unknown) return mode == Mode::Bridge;
    static const bool forced = [] {
        char v[4] = {};
        const DWORD n = GetEnvironmentVariableA("NR_BRIDGE", v, sizeof v);
        return n > 0 && n < sizeof v && v[0] == '1';
    }();
    mode = interop_present && !forced ? Mode::Interop : Mode::Bridge;
    if (mode == Mode::Bridge)
        log("[nr] %s: the network runs on a Vulkan device of its own beside the game's D3D device",
            interop_present ? "NR_BRIDGE=1" : "native D3D runtime");
    return mode == Mode::Bridge;
}

bool Session::Impl::ensure_bridge(const LUID& luid) {
    if (bdev) return true;
    std::string why;
    bdev = bridge::Device::get(luid, &why);
    if (!bdev) {
        status = "neural rendering unavailable: " + why;
        failed = true;
        return false;
    }
    handles = bdev->handles();
    access.queue = bdev->queue();
    access.family = bdev->family();
    // Our queue is ours: the only other submitter is the build thread's weight upload.
    bridge::Device* d = bdev;
    access.lock = access.lock_quiet = [d] { d->lock(); };
    access.unlock = access.unlock_quiet = [d] { d->unlock(); };
    return true;
}

bool Session::Impl::ensure_shared(bridge::Image& image, uint32_t w, uint32_t h, DXGI_FORMAT format,
                                  ID3D12Device* d12, ID3D11Device* d11, const char* what, ID3D10Device* d10) {
    const void* device = d12 ? static_cast<const void*>(d12) : d11 ? static_cast<const void*>(d11) : d10;
    if (image.matches(w, h, format, device)) return true;
    if (image) {
        retired_images.push_back({image, bridge_runs + 16});
        image = bridge::Image{};
    }
    std::string why;
    const bool ok = d12 ? bdev->create_d3d12(d12, w, h, format, &image, &why)
                  : d11 ? bdev->create_d3d11(d11, w, h, format, &image, &why)
                        : bdev->create_d3d10(d10, w, h, format, &image, &why);
    if (!ok) {
        status = std::string("the shared ") + what + " image: " + why;
        thread_local std::string said;
        if (said != status) { said = status; log("[nr] bridge: %s", status.c_str()); }
        return false;
    }
    log("[nr] bridge: shared %s image %ux%u, DXGI format %u, VkFormat %u%s", what, w, h, unsigned(format),
        unsigned(image.format), image.storage ? "" : " (no storage use)");
    return true;
}

void Session::Impl::sweep_retired_images() {
    for (auto it = retired_images.begin(); it != retired_images.end();) {
        // Not before 16 more hand-overs, not while a job of ours (recorded or running) uses it, and a
        // D3D12 one not before its queue has got past the frames that copied out of it.
        if (bridge_runs < it->after || bdev->busy(it->image.image)) { ++it; continue; }
        if (it->mark == ~0ull) { it->mark = bdev->hand_over_mark(); ++it; continue; }
        ID3D12Resource* r12 = nullptr;
        const bool d3d12 = it->image.d3d && SUCCEEDED(it->image.d3d->QueryInterface(IID_PPV_ARGS(&r12))) && r12;
        if (r12) r12->Release();
        if (d3d12 && !bdev->hand_over_passed(it->mark)) { ++it; continue; }
        bdev->destroy(it->image);
        it = retired_images.erase(it);
    }
}

namespace {

void add_transition(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
    if (!r || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    v.push_back(b);
}

void flush_barriers(ID3D12GraphicsCommandList* list, std::vector<D3D12_RESOURCE_BARRIER>& v) {
    if (!v.empty()) list->ResourceBarrier(UINT(v.size()), v.data());
    v.clear();
}

ID3D12Resource* d3d12_of(const bridge::Image& image) { return static_cast<ID3D12Resource*>(image.d3d); }

ColourFrame bridge_frame(const bridge::Image& image) {
    ColourFrame f{};
    f.image = image.image;
    f.format = image.format;
    f.width = image.width;
    f.height = image.height;
    f.before = f.after = VK_IMAGE_LAYOUT_GENERAL;
    f.usage = image.usage;
    return f;
}

}  // namespace

ID3D12GraphicsCommandList* Session::Impl::next_own_list(ID3D12Device* device) {
    if (!own_lists_fence &&
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own_lists_fence))))
        return nullptr;
    const uint64_t completed = own_lists_fence->GetCompletedValue();
    if (completed == UINT64_MAX) { if (bdev) bdev->lost("the game's D3D12 device was removed"); return nullptr; }
    OwnList* pick = nullptr;
    for (auto& l : own_lists)
        if (l.done <= completed && l.done < ~0ull - 1) { pick = &l; break; }
    if (!pick && own_lists.size() >= 8) {
        // Every one is still on the GPU: wait for the oldest, at most a second.
        OwnList* oldest = &own_lists[0];
        for (auto& l : own_lists) if (l.done < oldest->done) oldest = &l;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event && SUCCEEDED(own_lists_fence->SetEventOnCompletion(oldest->done, event)))
            WaitForSingleObject(event, 1000);
        if (event) CloseHandle(event);
        if (own_lists_fence->GetCompletedValue() < oldest->done) return nullptr;
        pick = oldest;
    }
    if (pick) {
        if (FAILED(pick->allocator->Reset()) || FAILED(pick->list->Reset(pick->allocator, nullptr))) return nullptr;
    } else {
        OwnList l;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&l.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, l.allocator, nullptr,
                                             IID_PPV_ARGS(&l.list)))) {
            if (l.allocator) l.allocator->Release();
            return nullptr;
        }
        own_lists.push_back(l);
        pick = &own_lists.back();
    }
    pick->done = ~0ull;   // recording; given its value when executed
    return pick->list;
}

void Session::Impl::release_bridge() {
    if (!bdev) return;
    bdev->wait_idle();
    if (own_lists_fence) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event && SUCCEEDED(own_lists_fence->SetEventOnCompletion(own_lists_value, event)))
            WaitForSingleObject(event, 1000);
        if (event) CloseHandle(event);
    }
    for (auto& l : own_lists) {
        if (l.list) l.list->Release();
        if (l.allocator) l.allocator->Release();
    }
    own_lists.clear();
    if (own_lists_fence) { own_lists_fence->Release(); own_lists_fence = nullptr; }
    // The D3D sides are released once the game's queue is past what it was given so far (bury): a
    // command list recorded with our copies can still be executed after the session is gone.
    bdev->bury(shared_colour);
    bdev->bury(shared_motion);
    bdev->bury(shared_depth);
    bdev->bury(shared_exposure);
    for (auto& r : retired_images) bdev->bury(r.image);
    retired_images.clear();
    for (auto& p : parked_sets) { bdev->bury(p.colour); bdev->bury(p.motion); bdev->bury(p.depth); }
    parked_sets.clear();
    if (own_queue) { own_queue->Release(); own_queue = nullptr; }
}

bridge::Job* Session::Impl::begin_job(std::string* why) {
    {
        // Every network still ours whose recorded frame was not run (not only the current one: another runtime of the
        // same network memory must not take that memory as made ready - Runtime::drop_history).
        std::lock_guard<std::mutex> guard(drops->m);
        for (auto& e : cache)
            if (drops->runtimes.erase(e.runtime.get())) {
                e.runtime->drop_history();
                log("[nr] bridge: a recorded frame was not run; the network's history starts again");
            }
    }
    bridge::Job* job = bdev->begin(why, runtime);
    if (job) {
        auto box = drops;
        const void* owner = runtime;
        bdev->on_drop(job, [box, owner] {
            std::lock_guard<std::mutex> guard(box->m);
            box->runtimes.insert(owner);
        });
    }
    return job;
}

bridge::Job* Session::Impl::record_bridge_job(const EngineFrame& base, bool motion, bool depth,
                                              const Controls& controls, const BridgeRenoDX* renodx) {
    (void)depth;
    std::string why;
    bridge::Job* job = begin_job(&why);
    if (!job) {
        status = "neural rendering skipped: " + why;
        return nullptr;
    }
    std::vector<const bridge::Image*> images{&shared_colour};
    if (base.motion.image) images.push_back(&shared_motion);
    if (base.depth.image) images.push_back(&shared_depth);
    if (renodx && renodx->exposure) images.push_back(&shared_exposure);
    bdev->acquire(job, images);
    if (renodx) {
        // In place on the shared colour image; record_renodx sets `status` (and `failed`) when it cannot.
        const ColourFrame exposure = bridge_frame(shared_exposure);
        if (!record_renodx(bdev->command_buffer(job), *renodx->stage, base.colour, base.colour, base, renodx->controls,
                           *renodx->frame, renodx->exposure ? &exposure : nullptr)) {
            bdev->abort(job);
            return nullptr;
        }
        if (!bdev->end(job, &why)) {
            status = why;
            return nullptr;
        }
        job_uses.push_back({renodx->stage, job, bdev->generation(job)});
        return job;
    }
    carry_source = nullptr;
    try {
        const VkCommandBuffer cmd = bdev->command_buffer(job);
        EngineFrame frame = base;
        if (resized(base.feature, base.colour.width, base.colour.height) && !carry(cmd, base.feature)) frame.reset = true;
        if (motion) runtime->record_engine(cmd, frame, controls);
        else runtime->record(cmd, frame.colour, controls, frame.reset);
    } catch (const std::exception& e) {
        bdev->abort(job);
        status = std::string("neural rendering failed: ") + e.what();
        failed = true;
        log("[nr] %s", status.c_str());
        return nullptr;
    }
    if (!bdev->end(job, &why)) {
        status = why;
        return nullptr;
    }
    if (carry_source) job_uses.push_back({carry_source, job, bdev->generation(job)});
    return job;
}

bool Session::Impl::used_by_pending_job(const void* what) {
    if (!bdev) return false;
    bool used = false;
    for (size_t i = job_uses.size(); i-- > 0;) {
        if (!bdev->busy(job_uses[i].job, job_uses[i].generation)) { job_uses.erase(job_uses.begin() + long(i)); continue; }
        used |= job_uses[i].what == what;
    }
    return used;
}

// The game's frame, motion and depth into the shared images, recorded into `list`. Each resource goes
// back to the state it came in; the shared ones end in COMMON, as every hand-over to the other API
// wants. `depth` comes back false when the depth buffer could not be read.
void Session::Impl::record_copy_in(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                                   ID3D12Resource* colour, D3D12_RESOURCE_STATES colour_state, ID3D12Resource* motion,
                                   D3D12_RESOURCE_STATES motion_state, ID3D12Resource* depth,
                                   D3D12_RESOURCE_STATES depth_state, ID3D12Resource* exposure,
                                   D3D12_RESOURCE_STATES exposure_state) {
    auto& s = *this;
    constexpr auto kCommon = D3D12_RESOURCE_STATE_COMMON;
    constexpr auto kSrc = D3D12_RESOURCE_STATE_COPY_SOURCE, kDst = D3D12_RESOURCE_STATE_COPY_DEST;
    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* sc = d3d12_of(s.shared_colour);
    ID3D12Resource* sm = motion ? d3d12_of(s.shared_motion) : nullptr;
    ID3D12Resource* sd = depth ? d3d12_of(s.shared_depth) : nullptr;
    ID3D12Resource* se = exposure ? d3d12_of(s.shared_exposure) : nullptr;
    // A read state gains the compute-read bit; a write state (or none) is left for compute reads alone,
    // since read and write bits cannot be combined.
    const auto kWrites = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                             D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
                             D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
    const D3D12_RESOURCE_STATES depth_read =
        (depth_state & kRead) ? depth_state
        : (depth_state & kWrites) || depth_state == D3D12_RESOURCE_STATE_COMMON ? kRead
                                                                                : (depth_state | kRead);
    std::vector<D3D12_RESOURCE_BARRIER> b;
    add_transition(b, colour, colour_state, kSrc);
    add_transition(b, sc, kCommon, kDst);
    if (motion) { add_transition(b, motion, motion_state, kSrc); add_transition(b, sm, kCommon, kDst); }
    if (depth) { add_transition(b, depth, depth_state, depth_read); add_transition(b, sd, kCommon, kUav); }
    if (exposure) { add_transition(b, exposure, exposure_state, kSrc); add_transition(b, se, kCommon, kDst); }
    flush_barriers(list, b);
    list->CopyResource(sc, colour);
    if (motion) list->CopyResource(sm, motion);
    if (exposure) list->CopyResource(se, exposure);
    if (depth) {
        std::string why;
        // Checked by bridge_prepare_d3d12 already (depth12.ready); a failure here would only leave the
        // depth image as it was.
        if (!s.depth12.record(device, list, depth, sd, &why)) log("[nr] bridge: depth copy failed: %s", why.c_str());
    }
    add_transition(b, colour, kSrc, colour_state);
    add_transition(b, sc, kDst, kCommon);
    if (motion) { add_transition(b, motion, kSrc, motion_state); add_transition(b, sm, kDst, kCommon); }
    if (depth) { add_transition(b, depth, depth_read, depth_state); add_transition(b, sd, kUav, kCommon); }
    if (exposure) { add_transition(b, exposure, kSrc, exposure_state); add_transition(b, se, kDst, kCommon); }
    flush_barriers(list, b);
}

// The enhanced frame back into the game's resource.
void Session::Impl::record_copy_out(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                                    D3D12_RESOURCE_STATES target_state) {
    auto& s = *this;
    ID3D12Resource* sc = d3d12_of(s.shared_colour);
    std::vector<D3D12_RESOURCE_BARRIER> b;
    add_transition(b, sc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add_transition(b, target, target_state, D3D12_RESOURCE_STATE_COPY_DEST);
    flush_barriers(list, b);
    list->CopyResource(target, sc);
    add_transition(b, sc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    add_transition(b, target, D3D12_RESOURCE_STATE_COPY_DEST, target_state);
    flush_barriers(list, b);
}

// Everything up to and including the network's recording: the runtime, the shared images, the copy
// into them (into `copy_list`) and the job. Null with `status` set when the frame passes through.
namespace {
// A resource the bridge copies whole with CopyResource into its one-mip, one-layer, single-sample
// shared image.
bool plain_2d(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.MipLevels == 1 && d.DepthOrArraySize == 1 &&
           d.SampleDesc.Count == 1;
}
}  // namespace

void Session::Impl::select_shared_set(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const void* device) {
    if (!shared_colour || shared_colour.matches(w, h, fmt, device)) return;
    // Features at two extents (before and after the upscale) take turns: each keeps its own images
    // rather than recreating them on every switch.
    size_t found = parked_sets.size();
    for (size_t i = 0; i < parked_sets.size(); ++i)
        if (parked_sets[i].colour.matches(w, h, fmt, device)) found = i;
    SharedSet current{shared_colour, shared_motion, shared_depth};
    shared_colour = bridge::Image{}; shared_motion = bridge::Image{}; shared_depth = bridge::Image{};
    if (found < parked_sets.size()) {
        shared_colour = parked_sets[found].colour;
        shared_motion = parked_sets[found].motion;
        shared_depth = parked_sets[found].depth;
        parked_sets.erase(parked_sets.begin() + long(found));
    }
    parked_sets.push_back(current);
    if (parked_sets.size() > 3) {
        for (bridge::Image* i : {&parked_sets[0].colour, &parked_sets[0].motion, &parked_sets[0].depth})
            if (*i) retired_images.push_back({*i, bridge_runs + 16});
        parked_sets.erase(parked_sets.begin());
    }
}

bool Session::Impl::bridge_renodx(const Session::RenoDXFrame& rf, uint64_t feature, uint32_t w, uint32_t h,
                                  VkFormat format, uint32_t guide_w, uint32_t guide_h, const Controls& controls,
                                  BridgeRenoDX* out) {
    uint32_t nw = w, nh = h;
    renodx_extent(feature, w, h, guide_w, guide_h, rf.settings, &nw, &nh);
    out->frame = &rf;
    // NR off (NeuralUplift): the stage copies the frame through and never asks for the network (as the other routes)
    if (rf.settings.nr_enabled && !renodx_runtime(controls, nw, nh, &out->controls)) return false;
    out->stage = renodx_stage(feature, w, h, nw, nh, format, rf);
    return out->stage != nullptr;
}

bridge::Job* Session::Impl::bridge_prepare_d3d12(ID3D12Device* device, ID3D12GraphicsCommandList* copy_list,
                                                 const BridgeInputs& in, const Controls& controls,
                                                 const std::function<bool()>& before_copy) {
    auto& s = *this;
    ++s.bridge_runs;
    s.sweep_retired_images();
    const D3D12_RESOURCE_DESC desc = in.target->GetDesc();
    const DXGI_FORMAT fmt = bridge::shared_format(desc.Format);
    const VkFormat vk = fmt == DXGI_FORMAT_UNKNOWN ? VK_FORMAT_UNDEFINED : vulkan_colour_format_of(fmt);
    if (vk == VK_FORMAT_UNDEFINED) {
        s.status = "the frame's DXGI format " + std::to_string(unsigned(desc.Format)) + " is not one the bridge carries";
        return nullptr;
    }
    if (!plain_2d(desc)) {
        s.status = "the frame is not a single-sample 2D texture with one mip and one layer";
        return nullptr;
    }
    if (in.source) {
        const D3D12_RESOURCE_DESC sd = in.source->GetDesc();
        if (bridge::shared_format(sd.Format) != fmt || sd.Width != desc.Width || sd.Height != desc.Height ||
            !plain_2d(sd)) {
            s.status = "the input is not the output's twin";
            return nullptr;
        }
    }
    const uint32_t w = uint32_t(desc.Width), h = desc.Height;
    BridgeRenoDX renodx{};
    if (in.renodx) {
        if (!in.motion && Impl::renodx_needs_motion(*in.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return nullptr;
        }
        const D3D12_RESOURCE_DESC md = in.motion ? in.motion->GetDesc() : D3D12_RESOURCE_DESC{};
        if (!s.bridge_renodx(*in.renodx, in.feature, w, h, vk,
                             in.motion_subrect.width ? in.motion_subrect.width : uint32_t(md.Width),
                             in.motion_subrect.height ? in.motion_subrect.height : md.Height, controls, &renodx))
            return nullptr;
    } else {
        // the guides' allocation: a game with dynamic resolution renders into a corner of them (Impl::size_hint_px)
        s.size_hint_px = 0;
        for (ID3D12Resource* g : {in.motion, in.depth})
            if (g) {
                const D3D12_RESOURCE_DESC gd = g->GetDesc();
                s.size_hint_px = std::max<uint64_t>(s.size_hint_px, uint64_t(gd.Width) * gd.Height);
            }
        if (!s.ensure_runtime(controls, w, h, vk, in.linear && Session::Impl::is_linear_format(vk))) return nullptr;
    }
    s.select_shared_set(w, h, fmt, device);
    if (!s.ensure_shared(s.shared_colour, w, h, fmt, device, nullptr, "colour")) return nullptr;
    // Everything that decides what is copied is settled here, before anything is recorded anywhere.
    bool motion = false, depth = false;
    if (in.motion) {
        const D3D12_RESOURCE_DESC md = in.motion->GetDesc();
        const DXGI_FORMAT mf = bridge::shared_format(md.Format);
        motion = mf != DXGI_FORMAT_UNKNOWN && plain_2d(md) &&
                 s.ensure_shared(s.shared_motion, uint32_t(md.Width), md.Height, mf, device, nullptr, "motion");
    }
    if (in.renodx && !motion && Impl::renodx_needs_motion(*in.renodx)) {
        s.status = "the RenoDX-style option needs the motion vectors, and the bridge cannot carry them";
        return nullptr;
    }
    // RenoDX's exposure feed: the game's exposure texture (texel 0,0), when there is one the bridge can carry.
    if (in.renodx && in.exposure) {
        const D3D12_RESOURCE_DESC ed = in.exposure->GetDesc();
        const DXGI_FORMAT ef = bridge::shared_format(ed.Format);
        renodx.exposure = ef != DXGI_FORMAT_UNKNOWN && plain_2d(ed) &&
                          s.ensure_shared(s.shared_exposure, uint32_t(ed.Width), ed.Height, ef, device, nullptr,
                                          "exposure");
    }
    // [Network] Motion Without Depth (the default, as NVIDIA's DLL): the network never reads the depth, so it is not
    // copied either (the runtime leaves it alone on the other routes); no network (RenoDX with NR off): none either.
    const bool want_depth = s.runtime && !s.runtime->motion_without_depth();
    if (in.depth && want_depth) {
        const D3D12_RESOURCE_DESC dd = in.depth->GetDesc();
        std::string why;
        if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.DepthOrArraySize != 1 || dd.SampleDesc.Count != 1)
            why = "the depth buffer is not a single-sample 2D texture";
        else if (dd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
            why = "the depth buffer denies shader reads";
        else if (bridge::depth_view_format(dd.Format) == DXGI_FORMAT_UNKNOWN)
            why = "the depth buffer's DXGI format " + std::to_string(unsigned(dd.Format)) + " cannot be read";
        else if (!s.depth12.ready(device, &why)) {
        } else if (!s.ensure_shared(s.shared_depth, uint32_t(dd.Width), dd.Height, DXGI_FORMAT_R32_FLOAT, device,
                                    nullptr, "depth")) {
            why = s.status;
        } else if (!s.shared_depth.storage) {
            why = "the shared depth image cannot be written by a shader";
        } else {
            depth = true;
        }
        if (!depth && !s.depth_warned) {
            s.depth_warned = true;
            log("[nr] bridge: running without depth: %s", why.c_str());
        }
    }
    s.status.clear();
    // Everything that can still turn the frame away is asked before the network is recorded: a recording
    // advances the network's history, and one that never runs leaves that history unwritten.
    if (before_copy && !before_copy()) return nullptr;

    // The network first: a frame that cannot be recorded leaves the game's list untouched.
    EngineFrame engine{};
    engine.colour = bridge_frame(s.shared_colour);
    engine.feature = in.feature;
    engine.reset = in.reset;
    engine.motion_scale_x = in.motion_scale_x;
    engine.motion_scale_y = in.motion_scale_y;
    engine.depth_inverted = in.depth_inverted;
    if (motion) {
        engine.motion = bridge_frame(s.shared_motion);
        engine.motion_texture_width = engine.motion.width;
        engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, in.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (depth) {
        engine.depth = bridge_frame(s.shared_depth);
        apply_guide_subrect(&engine.depth, in.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
    }
    bridge::Job* job = s.record_bridge_job(engine, motion, depth, controls, in.renodx ? &renodx : nullptr);
    if (!job) return nullptr;
    record_copy_in(device, copy_list, in.source ? in.source : in.target, in.source ? in.source_state : in.target_state,
                   motion ? in.motion : nullptr, in.motion_state, depth ? in.depth : nullptr, in.depth_state,
                   renodx.exposure ? in.exposure : nullptr, in.exposure_state);
    return job;
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

void Session::set_white_point(float v) {
    impl_->white_point = v;
    for (auto& e : impl_->cache) e.runtime->set_white_point(v);
}

void Session::set_native_compose(bool v) { impl_->native_compose = v; }
void Session::set_motion_point(bool v) { impl_->motion_point = v; }

void Session::set_max_passes(uint32_t v) {
    // One pass: a network built for exactly one, whose single-pass paths (history
    // written in turn into two images, motion sampled in place, the input not
    // kept for later passes) do less work for the same answer. More than one:
    // never below four, so 2..4 share a network; above it the next frame
    // rebuilds, like a resolution change. A change of pass count keeps the
    // running network serving until the new one is built (ensure_runtime_).
    const uint32_t want = v <= 1 ? 1u
                        : v < Session::Impl::kMaxPasses ? Session::Impl::kMaxPasses
                        : (v > Session::Impl::kPassCeiling ? Session::Impl::kPassCeiling : v);
    impl_->max_passes_want = want;
}

void Session::set_model_scale(float v) {
    // Clamped to the range the runtime accepts; the rebuild happens on the
    // next frame, in the background, like a resolution change.
    impl_->model_scale = v < 0.25f ? 0.25f : (v > 1.0f ? 1.0f : v);
}

void Session::set_history_strength(float v) {
    impl_->history_strength = v;
    for (auto& e : impl_->cache) e.runtime->set_history_strength(v);
}

Session::~Session() {
    {
        const uint64_t r = impl_->ran_total + impl_->ran, p = impl_->passed_total + impl_->passed;
        if (r + p)
            log("[nr] session: %llu evaluations ran the network, %llu passed the frame through", (unsigned long long)r,
                (unsigned long long)p);
    }
    // A build still running owns nothing the session is about to destroy, but
    // it will hand back a runtime that must be destroyed too: wait for it.
    if (impl_->build_thread.joinable()) impl_->build_thread.join();
    if (impl_->heap_thread.joinable()) impl_->heap_thread.join();
    if (impl_->offload_thread.joinable()) impl_->offload_thread.join();
    impl_->heap_made.reset();
    // On the bridge: frames recorded for these networks and not yet run are given up (a command list
    // that runs later skips them), and our queue may still be running frames that use them.
    if (impl_->bdev) {
        for (auto& e : impl_->cache) impl_->bdev->cancel(e.runtime.get());
        for (auto& e : impl_->retiring) impl_->bdev->cancel(e.entry.runtime.get());
        for (auto& e : impl_->retired) impl_->bdev->cancel(e.runtime.get());
        impl_->bdev->wait_idle();
    }
    impl_->built.reset();
    impl_->runtime = nullptr;
    impl_->cache.clear();
    impl_->retiring.clear();
    impl_->retired.clear();
    impl_->release_output();
    impl_->release_invalidator();
    impl_->release_bridge();
    impl_->release_vk_output();
    if (impl_->stage_timing) vkDestroyQueryPool(impl_->handles.device, impl_->stage_timing, nullptr);
    // Screenshot readbacks: the worker may still be reading one it was handed (it finishes on its own).
    if (impl_->shot) impl_->shot_free(*impl_->shot);
    for (auto& d : impl_->diag)   // NR_DIAG_EXPOSURE: a slot still in flight stays allocated
        if (!d.pending || vkGetEventStatus(impl_->handles.device, d.done) == VK_EVENT_SET) impl_->diag_free(d);
    for (auto& sh : impl_->shots_out) {
        for (int i = 0; i < 500 && sh->handed && !sh->read.load(std::memory_order_acquire); ++i) Sleep(10);
        if (!sh->handed || sh->read.load(std::memory_order_acquire)) impl_->shot_free(*sh);
    }
    if (impl_->neutral_mv) vkDestroyImage(impl_->handles.device, impl_->neutral_mv, nullptr);
    if (impl_->neutral_mv_memory) vkFreeMemory(impl_->handles.device, impl_->neutral_mv_memory, nullptr);
    if (impl_->pool) vkDestroyCommandPool(impl_->handles.device, impl_->pool, nullptr);
    if (impl_->owned_queue) impl_->owned_queue->Release();
    if (impl_->dxvk) impl_->dxvk->Release();
    if (impl_->own_fence) {
        // Nothing may be freed while the GPU is still reading it.
        impl_->wait_for_own_cmd();
        vkDestroyFence(impl_->handles.device, impl_->own_fence, nullptr);
    }
    impl_->release_slots();
}

void Session::wait_idle() {
    impl_->wait_for_own_cmd();
    for (auto& slot : impl_->slots) impl_->wait_slot(slot);
    if (impl_->bdev) impl_->bdev->wait_idle();
}

const std::string& Session::status() const { return impl_->status; }

bool Session::building() const { return impl_->building; }
bool Session::failed() const { return impl_->failed; }

// The runtime that is actually running frames, not the one being built: during a
// rebuild the old one is still the one paying for the picture on screen.
float Session::gpu_ms() const {
    if (impl_->renodx_timed) return impl_->stage_ms_avg;
    return impl_->runtime ? impl_->runtime->average_gpu_ms() : 0.0f;
}

float Session::network_ms() const {
    if (impl_->renodx_timed) return impl_->stage_net_ms_avg;
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
        s.prep_ev = m.runtime->preprocess_meter().first;
    }
    s.running_net = m.runtime ? m.running_net : -1;
    s.want_net = m.net_want;
    for (int n = 0; n < nr::kNetworkChoices; ++n) s.unavailable[n] = m.net_switch.reason(n);
    s.building_net = m.building && !m.build_plan ? m.build_net : -1;
    s.resizes = m.resizes;
    s.carried = m.carried;
    s.renodx_known = m.renodx_facts_known;
    s.renodx = m.renodx_facts;
    s.motion_without_depth = m.runtime ? m.runtime->motion_without_depth() : m.net_switch.file().motion_without_depth;
    s.building = m.building && !m.build_plan;   // a plan is no network to wait for
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
        // on the bridge not one a recorded frame still uses (free_retired)
        const size_t before = m.retired.size();
        for (size_t i = m.retired.size(); i-- > 0;)
            if (!(m.bdev && (m.bdev->busy(m.retired[i].runtime.get()) || m.used_by_pending_job(m.retired[i].runtime.get()))))
                m.retired.erase(m.retired.begin() + long(i));
        if (m.retired.size() != before)
            log("[nr] evicted networks freed (no evaluation for %.0f s)", std::chrono::duration<double>(now - m.last_call).count());
    }
    if ((!m.cache.empty() || !m.parked.empty()) && now - m.residency_checked >= Impl::kResidencyCheck)
        m.residency(false, true);
}

std::string Session::vram_line() const {
    const MemoryState m = read_memory(impl_->handles.physical);
    char line[128];
    if (m.vram_known)
        std::snprintf(line, sizeof line, "VRAM %.0f / %.0f MB, commit %.0f MB free of %.0f",
                      mb(m.vram_used), mb(m.vram_budget), mb(m.commit_free), mb(m.commit_limit));
    else
        std::snprintf(line, sizeof line, "commit %.0f MB free of %.0f", mb(m.commit_free), mb(m.commit_limit));
    return line;
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
    impl_->asked_extent.erase(feature);
    const auto stage = impl_->renodx.find(feature);
    if (stage != impl_->renodx.end()) {
        for (uint32_t k = 1; k < stage->second.stack; ++k) impl_->release_everywhere(Impl::stack_feature(feature, k));
        impl_->retire_stage(std::move(stage->second.stage));
        impl_->renodx.erase(stage);
    }
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
    if (!s.ensure_runtime(controls, colour_vk.width, colour_vk.height, colour_vk.format,
                          !resources.colour_encoded &&
                              Session::Impl::is_linear_format(colour_vk.format)))
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

        if (motion_vk) s.runtime->record_engine(cmd, engine, controls);
        else s.runtime->record(cmd, frame, controls, engine.reset);
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
    if (bridged(device)) {
        // Native D3D12: the list is cut here (nr_pe_d3d12split.hpp) and the network runs between the
        // halves on our own device.
        if (!s.ensure_bridge(device->GetAdapterLuid())) return false;
        BridgeInputs in{};
        in.target = output; in.target_state = output_state;
        // A separate input: copied into the shared image instead of the output, which then needs no
        // seed copy (the answer is copied into it either way).
        in.source = input; in.source_state = input_state;
        in.motion = resources.motion; in.motion_state = resources.motion_state;
        in.depth = resources.depth; in.depth_state = resources.depth_state;
        in.motion_subrect = resources.motion_subrect; in.depth_subrect = resources.depth_subrect;
        in.motion_scale_x = resources.motion_scale_x; in.motion_scale_y = resources.motion_scale_y;
        in.depth_inverted = resources.depth_inverted;
        in.reset = resources.reset;
        in.feature = resources.feature;
        in.linear = !resources.colour_encoded;
        std::shared_ptr<split::Snapshot> state;
        std::string why;
        // The game's state as the evaluate found it, before anything of ours is recorded.
        const auto before_copy = [&] {
            if (!split::prepare(list, &why)) { s.status = "the command list cannot be cut yet: " + why; return false; }
            state = split::snapshot(list);
            if (!state) { s.status = "the command list's state is not known"; return false; }
            return true;
        };
        bridge::Job* job = s.bridge_prepare_d3d12(device, list, in, controls, before_copy);
        if (!job) return false;
        bridge::Device* d = s.bdev;
        const uint64_t gen = d->generation(job);
        split::Job between;
        between.run = [d, job, gen](ID3D12CommandQueue* queue) { d->run_between(queue, job, gen); };
        between.discard = [d, job, gen] { d->discard(job, gen); };
        if (!split::cut(list, std::move(between), state, &why)) {
            // The copy into the shared images is in the list; the bindings the depth copy made are
            // taken back, and the frame is left alone.
            d->discard(job, gen);
            split::restore(list, state);
            s.status = "the command list could not be cut: " + why;
            thread_local std::string said;
            if (said != s.status) { said = s.status; log("[nr] bridge: %s", s.status.c_str()); }
            return false;
        }
        // Recorded through the list, so into the continuation, after our work.
        s.record_copy_out(list, output, output_state);
        static bool first = true;
        if (first) {
            first = false;
            log("[nr] bridge: first frame cut at the evaluate and handed to the network (%ux%u)",
                unsigned(output->GetDesc().Width), output->GetDesc().Height);
        }
        return true;
    }
    if (!s.ensure_d3d12_device(device)) return false;
    const VkCommandBuffer cmd = command_buffer(list);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, output, output_state);
    if (!target.usable()) { s.status = describe_rejection(target, "output"); return false; }
    s.size_hint_px = guide_pixels(resources);
    if (!s.ensure_runtime(controls, target.width, target.height, target.format,
                          !resources.colour_encoded &&
                              Session::Impl::is_linear_format(target.format))) return false;

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
        if (motion_vk) s.runtime->record_engine(cmd, engine, controls);
        else s.runtime->record(cmd, frame, controls, engine.reset);
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
    list->SetPipelineState(nullptr);
    list->SetComputeRootSignature(nullptr);
    // ...and the part those two cannot reach: vkd3d's memory of what is bound in
    // the Vulkan command buffer itself. Without this, the second neural pass in
    // one command list leaves every following caller dispatch running our kernel.
    s.forget_bound_pipeline(device, list);
    return true;
}

bool Session::bridged(ID3D12Device* device) {
    auto& s = *impl_;
    if (s.mode != Impl::Mode::Unknown) return s.mode == Impl::Mode::Bridge;
    return s.decide_mode(device_handles(device).valid());
}

bool Session::run_d3d12_queue(const D3D12QueueFrame& f, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !f.device || !f.queue || !f.target) return false;
    s.status.clear();
    if (!bridged(f.device)) { s.status = "the queue-level hand-over is for the native D3D12 runtime"; return false; }
    if (!s.ensure_bridge(f.device->GetAdapterLuid())) return false;
    // Our lists and their completion fence follow one queue's order: frames on any other queue pass through.
    if (!s.own_queue) { s.own_queue = f.queue; f.queue->AddRef(); }
    if (f.queue != s.own_queue) { s.status = "the frame is on another D3D12 queue than the network's"; return false; }
    ID3D12GraphicsCommandList* in_list = s.next_own_list(f.device);
    if (!in_list) { s.status = "no command list of our own on the game's device"; return false; }
    BridgeInputs in{};
    in.target = f.target; in.target_state = f.target_state;
    in.motion = f.motion; in.motion_state = f.motion_state;
    in.depth = f.depth; in.depth_state = f.depth_state;
    in.motion_scale_x = f.motion_scale_x; in.motion_scale_y = f.motion_scale_y;
    in.depth_inverted = f.depth_inverted;
    in.reset = f.reset;
    in.linear = f.linear_hdr;
    in.renodx = f.renodx;
    // The list's slot is handed back as finished at once if the frame passes through.
    const auto give_back = [&](ID3D12GraphicsCommandList* l) {
        l->Close();
        for (auto& o : s.own_lists) if (o.list == l) o.done = 0;
    };
    bridge::Job* job = s.bridge_prepare_d3d12(f.device, in_list, in, controls, nullptr);
    if (!job) { give_back(in_list); return false; }
    const uint64_t gen = s.bdev->generation(job);
    ID3D12GraphicsCommandList* out_list = s.next_own_list(f.device);
    if (!out_list) {
        s.bdev->discard(job, gen);
        give_back(in_list);
        s.status = "no command list of our own on the game's device";
        return false;
    }
    s.record_copy_out(out_list, f.target, f.target_state);
    const HRESULT closed_in = in_list->Close(), closed_out = out_list->Close();
    if (FAILED(closed_in) || FAILED(closed_out)) {
        s.bdev->discard(job, gen);
        for (auto& o : s.own_lists) if (o.list == in_list || o.list == out_list) o.done = 0;
        s.status = "our command lists would not close";
        return false;
    }
    // Everything recorded before us goes to the queue first; then the copy in, the hand-over to our
    // queue and back, the copy out (only when the network ran: otherwise the frame stays as it was).
    if (f.flush) f.flush();
    ID3D12CommandList* first = in_list;
    f.queue->ExecuteCommandLists(1, &first);
    const bool ran = s.bdev->run_between(f.queue, job, gen);
    if (ran) {
        ID3D12CommandList* second = out_list;
        f.queue->ExecuteCommandLists(1, &second);
    }
    // The in-list copy is on the queue whatever happened: the slots are free once this value is reached.
    if (FAILED(f.queue->Signal(s.own_lists_fence, s.own_lists_value + 1))) {
        // No completion mark: these two lists are never handed out again.
        for (auto& o : s.own_lists) if (o.list == in_list || o.list == out_list) o.done = ~0ull - 1;
        s.status = "the D3D12 queue refused a signal";
        return false;
    }
    ++s.own_lists_value;
    for (auto& o : s.own_lists)
        if (o.list == in_list || o.list == out_list) o.done = ran || o.list == in_list ? s.own_lists_value : 0;
    if (!ran) { s.status = "the network did not run on this frame"; return false; }
    static bool first_frame = true;
    if (first_frame) { first_frame = false; log("[nr] bridge: first D3D12 frame handed over at queue level"); }
    return true;
}

namespace {
LUID adapter_luid(IUnknown* device) {
    LUID luid{};
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && dxgi && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        adapter && SUCCEEDED(adapter->GetDesc(&ad)))
        luid = ad.AdapterLuid;
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
    return luid;
}

// The keyed mutexes of the shared images, as one transaction: either every one is taken at `key`, or
// none is (those already taken are handed back at `key`).
enum class Keyed { Ok, Timeout, Abandoned, Failed };
Keyed acquire_all(const std::vector<const bridge::Image*>& images, UINT64 key, DWORD ms) {
    std::vector<IDXGIKeyedMutex*> taken;
    Keyed result = Keyed::Ok;
    for (const bridge::Image* i : images) {
        IDXGIKeyedMutex* km = nullptr;
        if (FAILED(i->d3d->QueryInterface(IID_PPV_ARGS(&km))) || !km) { result = Keyed::Failed; break; }
        const HRESULT hr = km->AcquireSync(key, ms);
        if (hr == S_OK) { taken.push_back(km); continue; }
        km->Release();
        result = hr == static_cast<HRESULT>(WAIT_ABANDONED) ? Keyed::Abandoned
               : hr == static_cast<HRESULT>(WAIT_TIMEOUT) ? Keyed::Timeout : Keyed::Failed;
        break;
    }
    for (IDXGIKeyedMutex* km : taken) {
        if (result != Keyed::Ok) km->ReleaseSync(key);
        km->Release();
    }
    return result;
}
// Every image handed over at `key`. False if any refused; the ones that refused are still ours (at the
// key they were acquired with), the others are at `key`.
bool release_all(const std::vector<const bridge::Image*>& images, UINT64 key, std::vector<bool>* released = nullptr) {
    bool ok = true;
    if (released) released->assign(images.size(), false);
    for (size_t n = 0; n < images.size(); ++n) {
        IDXGIKeyedMutex* km = nullptr;
        if (FAILED(images[n]->d3d->QueryInterface(IID_PPV_ARGS(&km))) || !km) { ok = false; continue; }
        const bool done = SUCCEEDED(km->ReleaseSync(key));
        km->Release();
        if (released) (*released)[n] = done;
        ok = ok && done;
    }
    return ok;
}
const char* keyed_text(Keyed k) {
    return k == Keyed::Timeout ? "timed out" : k == Keyed::Abandoned ? "was abandoned" : "failed";
}
}  // namespace

bool Session::run_d3d10(const D3D11Frame& frame, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !frame.device || !frame.target) return false;
    s.status.clear();
    ID3D10Device* device = nullptr;
    if (FAILED(frame.device->QueryInterface(IID_PPV_ARGS(&device))) || !device) {
        s.status = "the device is not a D3D10 device";
        return false;
    }
    struct Release { IUnknown* p; ~Release() { if (p) p->Release(); } } hold_device{device};
    s.decide_mode(false);
    if (!s.ensure_bridge(adapter_luid(device))) return false;
    if (!s.bdev->keyed_mutex()) {
        s.status = "neural rendering unavailable in D3D10: the driver has no VK_KHR_win32_keyed_mutex";
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }
    ++s.bridge_runs;
    s.sweep_retired_images();
    ID3D10Texture2D* target = nullptr;
    if (FAILED(frame.target->QueryInterface(IID_PPV_ARGS(&target))) || !target) {
        s.status = "the D3D10 frame is not a 2D texture";
        return false;
    }
    Release hold_target{target};
    D3D10_TEXTURE2D_DESC td{};
    target->GetDesc(&td);
    const DXGI_FORMAT fmt = bridge::shared_format(td.Format);
    const VkFormat vk = fmt == DXGI_FORMAT_UNKNOWN ? VK_FORMAT_UNDEFINED : vulkan_colour_format_of(fmt);
    if (vk == VK_FORMAT_UNDEFINED || td.SampleDesc.Count != 1 || td.MipLevels != 1 || td.ArraySize != 1) {
        s.status = "the frame's DXGI format " + std::to_string(unsigned(td.Format)) + " is not one the bridge carries";
        return false;
    }
    const bool linear = (frame.upscaler_input || frame.linear_hdr) && !frame.estimate_motion &&
                        Impl::is_linear_format(vk);
    BridgeRenoDX renodx{};
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return false;
        }
        D3D10_TEXTURE2D_DESC md{};
        ID3D10Texture2D* m = nullptr;
        if (frame.motion && SUCCEEDED(frame.motion->QueryInterface(IID_PPV_ARGS(&m))) && m) { m->GetDesc(&md); m->Release(); }
        if (!s.bridge_renodx(*frame.renodx, 0, td.Width, td.Height, vk, md.Width, md.Height, controls, &renodx))
            return false;
    } else if (!s.ensure_runtime(controls, td.Width, td.Height, vk, linear)) {
        return false;
    }
    if (!s.ensure_shared(s.shared_colour, td.Width, td.Height, fmt, nullptr, nullptr, "colour", device))
        return false;
    ID3D10Texture2D* motion = nullptr;
    if (frame.motion && SUCCEEDED(frame.motion->QueryInterface(IID_PPV_ARGS(&motion))) && motion) {
        D3D10_TEXTURE2D_DESC md{};
        motion->GetDesc(&md);
        const DXGI_FORMAT mf = bridge::shared_format(md.Format);
        if (mf == DXGI_FORMAT_UNKNOWN || md.SampleDesc.Count != 1 || md.MipLevels != 1 || md.ArraySize != 1 ||
            !s.ensure_shared(s.shared_motion, md.Width, md.Height, mf, nullptr, nullptr, "motion", device)) {
            motion->Release();
            motion = nullptr;
        }
    }
    Release hold_motion{motion};
    if (frame.renodx && !motion && Impl::renodx_needs_motion(*frame.renodx)) {
        s.status = "the RenoDX-style option needs the motion vectors, and the bridge cannot carry them";
        return false;
    }
    if (frame.depth && !s.depth_warned) {
        s.depth_warned = true;
        log("[nr] bridge: D3D10 has no compute shaders; the pass runs without depth");
    }
    s.status.clear();

    // The network first, so a frame that cannot be recorded leaves the game's texture alone.
    EngineFrame engine{};
    engine.colour = bridge_frame(s.shared_colour);
    engine.reset = frame.reset;
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    if (motion) engine.motion = bridge_frame(s.shared_motion);
    bridge::Job* job = nullptr;
    if (frame.renodx) {
        job = s.record_bridge_job(engine, motion != nullptr, false, controls, &renodx);
        if (!job) return false;
    } else if (frame.estimate_motion && !motion) {
        std::string why;
        job = s.begin_job(&why);
        if (!job) { s.status = "neural rendering skipped: " + why; return false; }
        s.bdev->acquire(job, {&s.shared_colour});
        try {
            TemporalFrame temporal{};
            temporal.reset = frame.reset;
            s.runtime->record_temporal(s.bdev->command_buffer(job), engine.colour, controls, temporal);
        } catch (const std::exception& e) {
            s.bdev->abort(job);
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return false;
        }
        if (!s.bdev->end(job, &why)) { s.status = why; return false; }
    } else {
        job = s.record_bridge_job(engine, motion != nullptr, false, controls);
        if (!job) return false;
    }
    const uint64_t gen = s.bdev->generation(job);

    std::vector<const bridge::Image*> images{&s.shared_colour};
    if (motion) images.push_back(&s.shared_motion);
    // Key 0: ours on the D3D10 side. A key that was abandoned (a holder died) is not used again.
    const Keyed took = acquire_all(images, 0, 1000);
    if (took != Keyed::Ok) {
        s.bdev->discard(job, gen);
        s.status = std::string("the shared textures' keyed mutex ") + keyed_text(took);
        if (took != Keyed::Timeout) { s.failed = true; log("[nr] bridge: %s", s.status.c_str()); }
        return false;
    }
    // Our copies must not be skipped by a predicate the game left set.
    ID3D10Predicate* predicate = nullptr;
    BOOL predicate_value = FALSE;
    device->GetPredication(&predicate, &predicate_value);
    if (predicate) device->SetPredication(nullptr, FALSE);
    const auto restore_predicate = [&] {
        if (predicate) { device->SetPredication(predicate, predicate_value); predicate->Release(); predicate = nullptr; }
    };
    device->CopyResource(static_cast<ID3D10Resource*>(s.shared_colour.d3d), target);
    if (motion) device->CopyResource(static_cast<ID3D10Resource*>(s.shared_motion.d3d), motion);
    // Handed to our queue at key 1; a texture that refused stays ours at 0, the rest are taken back to 0.
    std::vector<bool> released;
    if (!release_all(images, 1, &released)) {
        s.bdev->discard(job, gen);
        std::vector<const bridge::Image*> back;
        for (size_t n = 0; n < images.size(); ++n) if (released[n]) back.push_back(images[n]);
        const bool recovered = acquire_all(back, 1, 1000) == Keyed::Ok && release_all(images, 0);
        restore_predicate();
        s.status = "a shared texture's keyed mutex could not be handed over";
        if (!recovered) s.failed = true;
        log("[nr] bridge: %s%s", s.status.c_str(), recovered ? "" : "; the textures are lost to us");
        return false;
    }
    device->Flush();
    if (!s.bdev->submit_keyed(job, gen, images, 1, 2)) {
        // Take the textures back at the key we left them at, so the next frame starts from 0.
        const bool recovered = acquire_all(images, 1, 1000) == Keyed::Ok && release_all(images, 0);
        restore_predicate();
        s.status = "the bridge submit failed";
        if (!recovered) { s.failed = true; log("[nr] bridge: %s; the shared textures are lost to us", s.status.c_str()); }
        return false;
    }
    // Key 2: the network has finished. Waiting here is the only order D3D10 offers (it has no fences).
    const Keyed back = acquire_all(images, 2, 1000);
    if (back != Keyed::Ok) {
        restore_predicate();
        s.status = std::string("the network did not hand the frame back (") + keyed_text(back) + ")";
        s.failed = true;
        log("[nr] bridge: %s", s.status.c_str());
        return false;
    }
    device->CopyResource(target, static_cast<ID3D10Resource*>(s.shared_colour.d3d));
    restore_predicate();
    if (!release_all(images, 0)) {
        s.status = "a shared texture's keyed mutex was not handed back after the frame";
        s.failed = true;
        log("[nr] bridge: %s", s.status.c_str());
    }
    return true;
}

bool Session::Impl::bridge_d3d11(ID3D11Device* device, const D3D11Frame& frame, const Controls& controls) {
    auto& s = *this;
    const LUID luid = adapter_luid(device);
    if (!s.ensure_bridge(luid)) return false;
    ++s.bridge_runs;
    s.sweep_retired_images();
    ID3D11Texture2D* target = nullptr;
    if (FAILED(frame.target->QueryInterface(IID_PPV_ARGS(&target))) || !target) {
        s.status = "the D3D11 frame is not a 2D texture";
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    target->GetDesc(&td);
    const DXGI_FORMAT fmt = bridge::shared_format(td.Format);
    const VkFormat vk = fmt == DXGI_FORMAT_UNKNOWN ? VK_FORMAT_UNDEFINED : vulkan_colour_format_of(fmt);
    if (vk == VK_FORMAT_UNDEFINED || td.SampleDesc.Count != 1 || td.MipLevels != 1 || td.ArraySize != 1) {
        target->Release();
        s.status = "the frame's DXGI format " + std::to_string(unsigned(td.Format)) + " is not one the bridge carries";
        return false;
    }
    const bool linear = (frame.upscaler_input || frame.linear_hdr) && !frame.estimate_motion && is_linear_format(vk);
    BridgeRenoDX renodx{};
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            target->Release();
            s.status = "the RenoDX-style option needs the motion vectors";
            return false;
        }
        D3D11_TEXTURE2D_DESC md{};
        ID3D11Texture2D* m = nullptr;
        if (frame.motion && SUCCEEDED(frame.motion->QueryInterface(IID_PPV_ARGS(&m))) && m) { m->GetDesc(&md); m->Release(); }
        if (!s.bridge_renodx(*frame.renodx, 0, td.Width, td.Height, vk, md.Width, md.Height, controls, &renodx)) {
            target->Release();
            return false;
        }
    } else if (!s.ensure_runtime(controls, td.Width, td.Height, vk, linear)) {
        target->Release();
        return false;
    }
    if (!s.ensure_shared(s.shared_colour, td.Width, td.Height, fmt, nullptr, device, "colour")) {
        target->Release();
        return false;
    }
    ID3D11Resource* motion = nullptr;
    ID3D11Resource* depth = nullptr;
    if (frame.motion) {
        ID3D11Texture2D* m = nullptr;
        D3D11_TEXTURE2D_DESC md{};
        if (SUCCEEDED(frame.motion->QueryInterface(IID_PPV_ARGS(&m))) && m) {
            m->GetDesc(&md);
            const DXGI_FORMAT mf = bridge::shared_format(md.Format);
            if (mf != DXGI_FORMAT_UNKNOWN && md.SampleDesc.Count == 1 && md.MipLevels == 1 && md.ArraySize == 1 &&
                s.ensure_shared(s.shared_motion, md.Width, md.Height, mf, nullptr, device, "motion"))
                motion = m;
            else
                m->Release();
        }
    }
    // Motion Without Depth (the default): the network never reads the depth, so it is not copied (as bridge_prepare_d3d12)
    if (frame.depth && s.runtime && !s.runtime->motion_without_depth()) {
        ID3D11Texture2D* d = nullptr;
        D3D11_TEXTURE2D_DESC dd{};
        if (SUCCEEDED(frame.depth->QueryInterface(IID_PPV_ARGS(&d))) && d) {
            d->GetDesc(&dd);
            if (bridge::depth_view_format(dd.Format) != DXGI_FORMAT_UNKNOWN && dd.SampleDesc.Count == 1 &&
                s.ensure_shared(s.shared_depth, dd.Width, dd.Height, DXGI_FORMAT_R32_FLOAT, nullptr, device, "depth"))
                depth = d;
            else
                d->Release();
        }
    }
    if (frame.renodx && !motion && Impl::renodx_needs_motion(*frame.renodx)) {
        if (depth) depth->Release();
        target->Release();
        s.status = "the RenoDX-style option needs the motion vectors, and the bridge cannot carry them";
        return false;
    }
    s.status.clear();
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    // Our copies and the depth pass must not be skipped by a predicate the game left set; it is put back.
    ID3D11Predicate* predicate = nullptr;
    BOOL predicate_value = FALSE;
    context->GetPredication(&predicate, &predicate_value);
    if (predicate) context->SetPredication(nullptr, FALSE);
    context->CopyResource(static_cast<ID3D11Resource*>(s.shared_colour.d3d), target);
    if (motion) context->CopyResource(static_cast<ID3D11Resource*>(s.shared_motion.d3d), motion);
    if (depth) {
        std::string why;
        if (!s.depth11.run(device, context, depth, static_cast<ID3D11Texture2D*>(s.shared_depth.d3d), &why)) {
            if (!s.depth_warned) { s.depth_warned = true; log("[nr] bridge: running without depth: %s", why.c_str()); }
            depth->Release();
            depth = nullptr;
        }
    }

    EngineFrame engine{};
    engine.colour = bridge_frame(s.shared_colour);
    engine.reset = frame.reset;
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    if (motion) engine.motion = bridge_frame(s.shared_motion);
    if (depth) engine.depth = bridge_frame(s.shared_depth);
    bridge::Job* job = nullptr;
    if (frame.renodx) {
        job = s.record_bridge_job(engine, motion != nullptr, depth != nullptr, controls, &renodx);
    } else if (frame.estimate_motion && !motion) {
        // The Present fallback: the estimator's temporal path, as run_d3d11 records it on DXVK.
        std::string why;
        job = s.begin_job(&why);
        if (!job) s.status = "neural rendering skipped: " + why;
        if (job) {
            s.bdev->acquire(job, {&s.shared_colour});
            try {
                TemporalFrame temporal{};
                temporal.reset = frame.reset;
                s.runtime->record_temporal(s.bdev->command_buffer(job), engine.colour, controls, temporal);
            } catch (const std::exception& e) {
                s.bdev->abort(job);
                job = nullptr;
                s.status = std::string("neural rendering failed: ") + e.what();
                s.failed = true;
                log("[nr] %s", s.status.c_str());
            }
            if (job && !s.bdev->end(job, &why)) { job = nullptr; s.status = why; }
        }
    } else {
        job = s.record_bridge_job(engine, motion != nullptr, depth != nullptr, controls);
    }
    bool ran = false;
    if (job && s.bdev->run_between(context, job, s.bdev->generation(job))) {
        context->CopyResource(target, static_cast<ID3D11Resource*>(s.shared_colour.d3d));
        ran = true;
        static bool first = true;
        if (first) { first = false; log("[nr] bridge: first D3D11 frame handed over through the shared fence"); }
    }
    if (predicate) { context->SetPredication(predicate, predicate_value); predicate->Release(); }
    context->Release();
    if (motion) motion->Release();
    if (depth) depth->Release();
    target->Release();
    return ran;
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
    renodx_native = true;
    const bool ok = ensure_runtime(*c, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    renodx_native = false;
    return ok && runtime->takes_target(*c);   // the model not applied: the frame stays as it is
}

// The network's extent for this feature's frame (RenoDX's ComputeNrWorkingResolution + CommitNrResolution).
void Session::Impl::renodx_extent(uint64_t feature, uint32_t w, uint32_t h, uint32_t guide_w, uint32_t guide_h,
                                  const renodx::Settings& settings, uint32_t* nw, uint32_t* nh) {
    auto& e = renodx[feature];
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
    if (!neutral_mv || neutral_w != w || neutral_h != h) {
        if (neutral_mv) { vkDestroyImage(handles.device, neutral_mv, nullptr); neutral_mv = VK_NULL_HANDLE; }
        if (neutral_mv_memory) { vkFreeMemory(handles.device, neutral_mv_memory, nullptr); neutral_mv_memory = VK_NULL_HANDLE; }
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
        const bool own = k > 0 && !rs.pass_follow[k - 1];
        ck.intensity = k == 0 ? rs.intensity : rs.pass_intensity[k - 1];
        ck.style = int(own ? rs.pass_style[k - 1] : rs.style);
        ck.local_tone = own ? rs.pass_local_tone[k - 1] : rs.local_tone;
        ck.local_structure = own ? rs.pass_local_structure[k - 1] : rs.local_structure;
        ck.skin_structure = own ? rs.pass_skin_structure[k - 1] : rs.skin_structure;
        ck.automatic_mask = own ? rs.pass_auto_mask[k - 1] : rs.auto_mask;
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
    const bool final_frame = rf.present != 0 || rf.finished;
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
            capture->pair.meta = std::string("path=") + (rf.present ? "present" : "optiscaler-finished-picture") +
                                 "\ncomparison=final-picture-before-after-nr\nstack=" + std::to_string(passes) +
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
        if (time_net) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stage_timing, base + 2 + 2 * timed_passes);
        runtime->record_engine(cmd, e, pass_controls(k));
        if (time_net) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stage_timing, base + 3 + 2 * timed_passes++);
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
            ++stage_recorded;
            stage_slot = (slot + 1) % kStageSlots;
            read_stage_timing();
        }
        renodx_timed = timed;
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

bool Session::run_renodx(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                         ID3D12Resource* output, D3D12_RESOURCE_STATES output_state,
                         const EngineResources& resources, const Controls& controls,
                         ID3D12Resource* input, D3D12_RESOURCE_STATES input_state, const RenoDXFrame& rf) {
    auto& s = *impl_;
    s.current_feature = resources.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !device || !list || !output || !input) return false;
    if (!resources.motion && Impl::renodx_needs_motion(rf)) return false;
    s.status.clear();
    if (bridged(device)) {
        // Native D3D12: as run_after's bridge path - the list is cut at the evaluate, the input, guides and exposure
        // copied into the shared images, RenoDX's processing and the network run between the halves on our device,
        // the answer copied into the output.
        if (!s.ensure_bridge(device->GetAdapterLuid())) return false;
        BridgeInputs in{};
        in.target = output; in.target_state = output_state;
        if (input != output) { in.source = input; in.source_state = input_state; }
        in.motion = resources.motion; in.motion_state = resources.motion_state;
        in.depth = resources.depth; in.depth_state = resources.depth_state;
        in.motion_subrect = resources.motion_subrect; in.depth_subrect = resources.depth_subrect;
        in.motion_scale_x = resources.motion_scale_x; in.motion_scale_y = resources.motion_scale_y;
        in.depth_inverted = resources.depth_inverted;
        in.reset = resources.reset;
        in.feature = resources.feature;
        in.renodx = &rf;
        in.exposure = rf.exposure; in.exposure_state = rf.exposure_state;
        std::shared_ptr<split::Snapshot> state;
        std::string why;
        const auto before_copy = [&] {
            if (!split::prepare(list, &why)) { s.status = "the command list cannot be cut yet: " + why; return false; }
            state = split::snapshot(list);
            if (!state) { s.status = "the command list's state is not known"; return false; }
            return true;
        };
        bridge::Job* job = s.bridge_prepare_d3d12(device, list, in, controls, before_copy);
        if (!job) return false;
        bridge::Device* d = s.bdev;
        const uint64_t gen = d->generation(job);
        split::Job between;
        between.run = [d, job, gen](ID3D12CommandQueue* queue) { d->run_between(queue, job, gen); };
        between.discard = [d, job, gen] { d->discard(job, gen); };
        if (!split::cut(list, std::move(between), state, &why)) {
            d->discard(job, gen);
            split::restore(list, state);
            s.status = "the command list could not be cut: " + why;
            thread_local std::string said;
            if (said != s.status) { said = s.status; log("[nr] bridge: %s", s.status.c_str()); }
            return false;
        }
        s.record_copy_out(list, output, output_state);
        static bool first = true;
        if (first) {
            first = false;
            log("[nr] bridge: first RenoDX-style frame cut at the evaluate and handed to the network (%ux%u)",
                unsigned(output->GetDesc().Width), output->GetDesc().Height);
        }
        return true;
    }
    if (!s.ensure_d3d12_device(device)) return false;
    const VkCommandBuffer cmd = command_buffer(list);
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
    list->SetPipelineState(nullptr);
    list->SetComputeRootSignature(nullptr);
    s.forget_bound_pipeline(device, list);
    return true;
}

bool Session::run_present(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                          ID3D12Resource* back_buffer, unsigned width, unsigned height,
                          const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !device || !list || !back_buffer) return false;
    s.status.clear();
    if (!s.ensure_d3d12_device(device)) return false;
    const VkCommandBuffer cmd = command_buffer(list);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, back_buffer, D3D12_RESOURCE_STATE_PRESENT);
    if (!target.usable()) { s.status = describe_rejection(target, "back buffer"); return false; }
    if (!s.ensure_runtime(controls, target.width, target.height, target.format, false)) return false;

    // Here the network works on the game's own buffer: there is nothing to hand
    // anyone afterwards, the frame just has to come out enhanced.
    ColourFrame frame{};
    frame.image = target.image;
    frame.format = target.format;
    frame.width = target.width; frame.height = target.height;
    frame.before = frame.after = target.layout;
    try {
        TemporalFrame temporal{};
        s.runtime->record_temporal(cmd, frame, controls, temporal);
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
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !frame.device || !frame.target) return false;
    s.status.clear();

    // The native D3D11 runtime: the bridge, on a Vulkan device of our own.
    if (!s.dxvk && s.mode != Impl::Mode::Interop) {
        IDXGIVkInteropDevice* probe = nullptr;
        const bool interop = d3d11_handles(frame.device, &probe).valid();
        if (probe) probe->Release();
        if (s.decide_mode(interop)) {
            ID3D11Device* device = nullptr;
            if (FAILED(frame.device->QueryInterface(IID_PPV_ARGS(&device))) || !device) {
                s.status = "the D3D11 device is not a D3D11 device";
                return false;
            }
            const bool ran = s.bridge_d3d11(device, frame, controls);
            device->Release();
            return ran;
        }
    }
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
    Controls renodx_controls{};
    renodx::Stage* renodx_stage = nullptr;
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return false;
        }
        const auto mv = frame.motion ? d3d11_image(frame.motion, frame.device) : decltype(d3d11_image(frame.motion, frame.device)){};
        uint32_t nw = target.width, nh = target.height;
        s.renodx_extent(0, target.width, target.height, mv.width, mv.height, frame.renodx->settings, &nw, &nh);
        if (frame.renodx->settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &renodx_controls)) return false;
        if (!(renodx_stage = s.renodx_stage(0, target.width, target.height, nw, nh, target.format, *frame.renodx)))
            return false;
    } else if (!s.ensure_runtime(controls, target.width, target.height, target.format,
                                 (frame.upscaler_input || frame.linear_hdr) && !frame.estimate_motion &&
                                 Session::Impl::is_linear_format(target.format))) return false;
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
    engine.colour = colour;
    engine.reset = frame.reset || s.resized(0, colour.width, colour.height);
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    const bool motion = frame.motion && fill(&engine.motion, d3d11_image(frame.motion, frame.device));
    if (frame.depth) fill(&engine.depth, d3d11_image(frame.depth, frame.device));

    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(s.own_cmd, 0);
        vkBeginCommandBuffer(s.own_cmd, &bi);
        if (renodx_stage) {
            if (!motion && Impl::renodx_needs_motion(*frame.renodx)) { vkEndCommandBuffer(s.own_cmd); return false; }
            if (!s.record_renodx(s.own_cmd, *renodx_stage, colour, colour, engine, renodx_controls, *frame.renodx)) {
                vkEndCommandBuffer(s.own_cmd);
                return false;
            }
        } else if (motion) s.runtime->record_engine(s.own_cmd, engine, controls);
        else if (frame.estimate_motion) {
            TemporalFrame temporal{}; temporal.reset = frame.reset;
            s.runtime->record_temporal(s.own_cmd, colour, controls, temporal);
        } else {
            s.runtime->record(s.own_cmd, colour, controls, frame.reset);
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
        r = vkQueueSubmit(s.access.queue, 1, &si, s.own_fence);
        if (r == VK_SUCCESS) s.own_cmd_pending = true;
    }
    if (r != VK_SUCCESS) {
        s.status = "the submit on the game's queue failed";
        return false;
    }
    return true;
}

bool Session::run_present_vulkan(const DeviceHandles& handles, VkQueue queue, uint32_t family,
                                 VkImage image, VkFormat format, uint32_t width, uint32_t height,
                                 uint32_t slot_index, uint32_t slot_count,
                                 const VkSemaphore* wait, uint32_t wait_count,
                                 VkSemaphore* signalled, bool temporal,
                                 const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
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
    if (!s.ensure_runtime(controls, width, height, format, false)) return false;

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
    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(slot.cmd, 0);
        vkBeginCommandBuffer(slot.cmd, &bi);
        if (temporal) {
            TemporalFrame frame{};
            s.runtime->record_temporal(slot.cmd, colour, controls, frame);
        } else {
            s.runtime->record(slot.cmd, colour, controls);
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
    if (vkQueueSubmit(queue, 1, &si, slot.fence) != VK_SUCCESS) {
        s.status = "the submit before present failed";
        return false;
    }
    slot.pending = true;
    *signalled = slot.done;
    if (first) { first = false; log("[nr] first fallback frame submitted"); }
    return true;
}

void Session::set_vulkan_queue(VkQueue queue, uint32_t family) {
    if (!queue) return;
    impl_->access.queue = queue;
    impl_->access.family = family;
    // No lock: this queue belongs to the game and nothing else in this process
    // shares it through an interop layer, so there is no mutex to take. The
    // caller records into the command buffer the upscaler handed over, which the
    // game submits itself; only the one-off weight upload submits here.
    impl_->access.lock = nullptr;
    impl_->access.unlock = nullptr;
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
    Controls renodx_controls{};
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
    } else {
    if (!frame.in_place && !s.vk_output_valid(frame.width, frame.height, frame.colour_format)) {
        if (!s.ensure_vk_output(frame.width, frame.height, frame.colour_format))
            return VK_NULL_HANDLE;
    }
    s.size_hint_px = std::max(uint64_t(frame.motion ? frame.motion_width : 0) * (frame.motion ? frame.motion_height : 0),
                              uint64_t(frame.depth ? frame.depth_width : 0) * (frame.depth ? frame.depth_height : 0));
    if (!s.ensure_runtime(controls, frame.width, frame.height, frame.colour_format,
                          (frame.upscaler_input || frame.linear_hdr) &&
                              Session::Impl::is_linear_format(frame.colour_format)))
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
    if (direct) {
        try {
            s.runtime->record_engine(cmd, engine, controls);
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
            if (frame.motion) s.runtime->record_engine(cmd, engine, controls);
            else s.runtime->record(cmd, colour, controls, engine.reset);
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
        if (frame.motion) s.runtime->record_engine(cmd, engine, controls);
        else s.runtime->record(cmd, colour, controls, engine.reset);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return VK_NULL_HANDLE;
    }
    return s.vk_output;
}

}  // namespace nr::pe
