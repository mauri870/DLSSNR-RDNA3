// HUD FIX's capture on the .asi route, D3D12 (nr_asi_hudcap.hpp; the rules: nr_hud_capture.hpp).
// What ReShade's events give the add-on, from hooks of our own (MinHook, the functions of the game's D3D12 objects,
// each hooked once and never removed; a detour calls the original first or last as the call needs):
//   the device: its render-target views (a descriptor -> the resource it views: which bind is of a back buffer, copied
//               descriptors followed) and its pipelines' depth test and blending (CreateGraphicsPipelineState,
//               CreatePipelineState, a pipeline library's LoadGraphicsPipeline / LoadPipeline) - from the game's swap
//               chain on, so its pipelines are known when the capture is armed later;
//   the lists:  the render targets, viewport, scissor and pipeline bound, each draw, clear, copy and resolve on a back
//               buffer, render passes, Reset (from the first armed frame on);
//   the queue:  ExecuteCommandLists - the lists' writes in the GPU's order.
// The copy goes into the game's list right before the first overlay after a scene write (every such place: lists are
// recorded on many threads, not in the GPU's order; nr::hudcap::kMaxCopies a frame at most), with the back buffer moved
// from the state that write has it in to COPY_SOURCE and back. Not followed: bundles (a bundle's write on the back buffer
// makes the frame not captured), copies inside a render pass (none can be made there).
// D3D11 (further down): the immediate context's functions hooked (its vtable, from the device's GetImmediateContext); its
// bound states read at a draw on the back buffer (their GetDesc), the copy made by that context, at the last scene ->
// overlay place of the last frame (one copy a frame: the context runs in its own order). A deferred context's commands
// are not seen.
#include "nr_asi_hudcap.hpp"
#include "nr_hud_capture.hpp"
#include "nr_pe_log.hpp"

#include <MinHook.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nr::asi::hudcap {
namespace {

using nr::pe::log;
using nr::hudcap::Rec;
using nr::hudcap::kLost;
using nr::hudcap::kScene;
using nr::hudcap::kOverlay;
using nr::hudcap::kCopy;
using nr::hudcap::kUnknown;

// D3D12 pieces this MinGW's headers do not have (ID3D12GraphicsCommandList4's render passes, ID3D12Device2's pipeline
// state streams): only what is read.
struct RenderPassTarget {   // D3D12_RENDER_PASS_RENDER_TARGET_DESC's head (the first one is read)
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    UINT begin_type;        // D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE: 2 CLEAR
};
struct StreamDesc {         // D3D12_PIPELINE_STATE_STREAM_DESC
    SIZE_T size;
    void* stream;
};

thread_local int t_own = 0;

struct PsoFacts {
    int8_t depth = -1, blend = -1;
};
struct List {
    uint64_t back = 0;                // the back buffer bound as a render target (else 0)
    bool dsv = false;
    ID3D12PipelineState* pso = nullptr;
    D3D12_VIEWPORT vp{};
    D3D12_RECT scissor{};
    bool in_pass = false;
    uint8_t last = 0;
    std::vector<Rec> recs;
    void reset() {
        back = 0; dsv = false; pso = nullptr; vp = {}; scissor = {}; in_pass = false; last = 0;
        recs.clear();
    }
};

struct State {
    std::atomic<bool> armed{false};
    std::atomic<ID3D12Device*> device{nullptr};
    std::atomic<uint64_t> backs[8];
    std::atomic<uint32_t> back_w{0}, back_h{0};
    std::atomic<ID3D12Resource*> hudless{nullptr};
    std::atomic<uint32_t> copies{0};
    std::atomic<bool> foreign{false};
    UINT rtv_step = 0;
    std::shared_mutex views_m;
    std::unordered_map<uint64_t, uint64_t> rtvs;           // RTV descriptor -> the resource it views
    std::shared_mutex pipes_m;
    std::unordered_map<ID3D12PipelineState*, PsoFacts> pipes;
    std::mutex m;                                          // lists' map, frame, retired
    std::unordered_map<void*, List> lists;                 // never erased (threads keep pointers to the entries)
    std::vector<Rec> frame;                                // the executed lists' writes, in the GPU's order
    struct Retired { ID3D12Resource* r; uint64_t at; };
    std::vector<Retired> retired;                          // earlier copies' images, released 64 Presents later
    uint64_t presents = 0;
    bool device_hooked = false, lists_hooked = false, late = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
} g;

bool is_back(uint64_t h) {
    if (!h) return false;
    for (const auto& b : g.backs)
        if (b.load(std::memory_order_relaxed) == h) return true;
    return false;
}

List* list_of(void* cl) {
    thread_local void* last_cl = nullptr;
    thread_local List* last_state = nullptr;
    if (cl == last_cl) return last_state;
    std::lock_guard<std::mutex> lk(g.m);
    List* s = &g.lists[cl];
    last_cl = cl;
    last_state = s;
    return s;
}

PsoFacts facts_of(ID3D12PipelineState* p) {
    if (!p) return {};
    std::shared_lock<std::shared_mutex> lk(g.pipes_m);
    const auto it = g.pipes.find(p);
    if (it != g.pipes.end()) return it->second;
    PsoFacts unknown;   // made before the capture followed the device: not known
    unknown.depth = unknown.blend = -2;
    return unknown;
}

void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

// A write of `kind` on back buffer `b` in `cl` (`state`: what the game has it in for that write): first the copy, where
// an overlay follows a scene write or opens the list.
void write(ID3D12GraphicsCommandList* cl, List& s, uint64_t b, uint8_t kind, D3D12_RESOURCE_STATES state) {
    if (kind == kOverlay && s.last != kOverlay) {
        ID3D12Resource* h = g.hudless.load(std::memory_order_acquire);
        if (h && !s.in_pass && g.copies.fetch_add(1, std::memory_order_relaxed) < nr::hudcap::kMaxCopies) {
            ++t_own;
            ID3D12Resource* back = reinterpret_cast<ID3D12Resource*>(b);
            transition(cl, back, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(cl, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(h, back);
            transition(cl, h, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition(cl, back, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            --t_own;
            s.recs.push_back({kCopy, b, reinterpret_cast<uint64_t>(h)});
        }
    }
    if (s.recs.size() >= nr::hudcap::kMaxRecs) {   // never executed: kept short, the frame not trusted
        s.recs.clear();
        s.recs.push_back({kLost, b});
    }
    s.recs.push_back({kind, b});
    s.last = kind;
}

bool full_box(const D3D12_BOX* box) {
    return !box || (box->left == 0 && box->top == 0 && box->right >= g.back_w.load(std::memory_order_relaxed) &&
                    box->bottom >= g.back_h.load(std::memory_order_relaxed));
}

uint8_t draw_kind(const List& s, uint32_t vertices, uint32_t instances, bool indirect) {
    const PsoFacts f = facts_of(s.pso);
    nr::hudcap::Draw d;
    d.dsv = s.dsv;
    d.depth = f.depth;
    d.blend = f.blend;
    d.scissor_on = true;   // D3D12 always cuts by its scissor
    d.vp[0] = s.vp.TopLeftX; d.vp[1] = s.vp.TopLeftY; d.vp[2] = s.vp.Width; d.vp[3] = s.vp.Height;
    d.sc[0] = s.scissor.left; d.sc[1] = s.scissor.top; d.sc[2] = s.scissor.right; d.sc[3] = s.scissor.bottom;
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, g.back_w.load(std::memory_order_relaxed), g.back_h.load(std::memory_order_relaxed));
}

bool on() { return g.armed.load(std::memory_order_relaxed) && !t_own; }

// ---- the detours --------------------------------------------------------------------------------------------------
using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_CopyTextureRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT,
                                                       UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PFN_Resolve = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
using PFN_Viewports = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using PFN_Scissors = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using PFN_SetPipeline = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_ExecuteBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using PFN_SetTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_ClearTarget = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT,
                                                 const D3D12_RECT*);
using PFN_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*,
                                                     UINT64, ID3D12Resource*, UINT64);
using PFN_BeginPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const RenderPassTarget*, const void*, UINT);
using PFN_EndPass = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateRtv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                               D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                                     const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CreateGraphics = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateStream = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const StreamDesc*, REFIID, void**);
using PFN_LoadGraphics = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                     REFIID, void**);
using PFN_LoadStream = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, const StreamDesc*, REFIID, void**);

PFN_Reset real_reset;
PFN_Draw real_draw;
PFN_DrawIndexed real_draw_indexed;
PFN_CopyTextureRegion real_copy_region;
PFN_CopyResource real_copy_resource;
PFN_Resolve real_resolve;
PFN_Viewports real_viewports;
PFN_Scissors real_scissors;
PFN_SetPipeline real_set_pipeline;
PFN_ExecuteBundle real_execute_bundle;
PFN_SetTargets real_set_targets;
PFN_ClearTarget real_clear_target;
PFN_ExecuteIndirect real_execute_indirect;
PFN_BeginPass real_begin_pass;
PFN_EndPass real_end_pass;
PFN_Execute real_execute;
PFN_CreateRtv real_create_rtv;
PFN_CopyDescriptors real_copy_descriptors;
PFN_CopyDescriptorsSimple real_copy_descriptors_simple;
PFN_CreateGraphics real_create_graphics;
PFN_CreateStream real_create_stream;
PFN_LoadGraphics real_load_graphics;
PFN_LoadStream real_load_stream;

void remember(void** out, const PsoFacts& f) {
    if (!out || !*out) return;
    std::unique_lock<std::shared_mutex> lk(g.pipes_m);
    g.pipes[static_cast<ID3D12PipelineState*>(*out)] = f;
}
PsoFacts facts_of_desc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    PsoFacts f;
    if (!d) return f;
    f.depth = d->DepthStencilState.DepthEnable && d->DSVFormat != DXGI_FORMAT_UNKNOWN ? 1 : 0;
    f.blend = d->BlendState.RenderTarget[0].BlendEnable ? 1 : 0;
    return f;
}
// A pipeline state stream's depth test and blending (D3D12_PIPELINE_STATE_SUBOBJECT_TYPE; each subobject a type word
// and its payload, both at pointer alignment). A subobject this reader does not know ends the reading (the rest stays
// not known).
PsoFacts facts_of_stream(const StreamDesc* d) {
    PsoFacts f;
    if (!d || !d->stream) return f;
    const uint8_t* p = static_cast<const uint8_t*>(d->stream);
    const uint8_t* end = p + d->size;
    // payload sizes by type (x64), 0: not known
    static const uint32_t kPayload[30] = {8, 16, 16, 16, 16, 16, 16, 32, 328, 4, 44, 52, 16, 4, 4, 36, 4, 8, 4, 16, 4,
                                          56, 24, 0, 16, 16, 60, 44, 40, 16};
    static const uint32_t kAlign[30] = {8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
                                        4, 8, 0, 8, 8, 4, 4, 4, 8};
    bool depth_set = false, dsv_known = false, dsv_format = false, whole = true;
    int depth_enable = -1;
    while (p + sizeof(UINT) <= end) {
        UINT type;
        std::memcpy(&type, p, sizeof type);
        if (type >= 30 || !kPayload[type]) { whole = false; break; }
        const size_t at = (sizeof(UINT) + kAlign[type] - 1) / kAlign[type] * kAlign[type];
        const uint8_t* payload = p + at;
        if (payload + kPayload[type] > end) { whole = false; break; }
        if (type == 8) {   // BLEND: D3D12_BLEND_DESC, RenderTarget[0].BlendEnable after two BOOLs
            BOOL b;
            std::memcpy(&b, payload + 8, sizeof b);
            f.blend = b ? 1 : 0;
        } else if (type == 11 || type == 21 || type == 26) {   // DEPTH_STENCIL / 1 / 2: DepthEnable first
            BOOL b;
            std::memcpy(&b, payload, sizeof b);
            depth_enable = b ? 1 : 0;
            depth_set = true;
        } else if (type == 16) {   // DEPTH_STENCIL_FORMAT
            DXGI_FORMAT fm;
            std::memcpy(&fm, payload, sizeof fm);
            dsv_known = true;
            dsv_format = fm != DXGI_FORMAT_UNKNOWN;
        }
        p += (at + kPayload[type] + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
    }
    if (depth_set) f.depth = depth_enable && (!dsv_known || dsv_format) ? 1 : 0;
    else if (dsv_known && whole) f.depth = dsv_format ? 1 : 0;   // the default: depth on, where a depth format is set
    else if (!whole) f.depth = -2;                               // a stream read in part: not known
    if (f.blend < 0) f.blend = whole ? 0 : -2;                   // the default: off; read in part: not known
    return f;
}

HRESULT STDMETHODCALLTYPE hk_create_graphics(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid,
                                             void** out) {
    const HRESULT hr = real_create_graphics(d, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_desc(desc));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_create_stream(ID3D12Device* d, const StreamDesc* desc, REFIID iid, void** out) {
    const HRESULT hr = real_create_stream(d, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_stream(desc));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_load_graphics(IUnknown* lib, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                           REFIID iid, void** out) {
    const HRESULT hr = real_load_graphics(lib, name, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_desc(desc));
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_load_stream(IUnknown* lib, LPCWSTR name, const StreamDesc* desc, REFIID iid,
                                         void** out) {
    const HRESULT hr = real_load_stream(lib, name, desc, iid, out);
    if (SUCCEEDED(hr)) remember(out, facts_of_stream(desc));
    return hr;
}
void STDMETHODCALLTYPE hk_create_rtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                     D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_create_rtv(d, r, desc, h);
    std::unique_lock<std::shared_mutex> lk(g.views_m);
    if (r) g.rtvs[h.ptr] = reinterpret_cast<uint64_t>(r);
    else g.rtvs.erase(h.ptr);
}
void copy_views(SIZE_T dst, SIZE_T src, UINT n) {
    if (!g.rtv_step) return;
    std::unique_lock<std::shared_mutex> lk(g.views_m);
    for (UINT i = 0; i < n; ++i) {
        const auto it = g.rtvs.find(src + SIZE_T(i) * g.rtv_step);
        if (it != g.rtvs.end()) g.rtvs[dst + SIZE_T(i) * g.rtv_step] = it->second;
        else g.rtvs.erase(dst + SIZE_T(i) * g.rtv_step);
    }
}
void STDMETHODCALLTYPE hk_copy_descriptors_simple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    real_copy_descriptors_simple(d, n, dst, src, type);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) copy_views(dst.ptr, src.ptr, n);
}
void STDMETHODCALLTYPE hk_copy_descriptors(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dsizes,
                                           UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* ssizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE type) {
    real_copy_descriptors(d, nd, dst, dsizes, ns, src, ssizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV || !dst || !src) return;
    // the ranges walked together, descriptor by descriptor (a null size array: ranges of one)
    UINT di = 0, si = 0, doff = 0, soff = 0;
    while (di < nd && si < ns) {
        const UINT dn = dsizes ? dsizes[di] : 1, sn = ssizes ? ssizes[si] : 1;
        const UINT n = std::min(dn - doff, sn - soff);
        copy_views(dst[di].ptr + SIZE_T(doff) * g.rtv_step, src[si].ptr + SIZE_T(soff) * g.rtv_step, n);
        doff += n; soff += n;
        if (doff == dn) { ++di; doff = 0; }
        if (soff == sn) { ++si; soff = 0; }
    }
}

HRESULT STDMETHODCALLTYPE hk_reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    const HRESULT hr = real_reset(cl, a, p);
    if (SUCCEEDED(hr)) nr::gpu::reset(cl);
    if (g.lists_hooked && !t_own) {
        List* s = list_of(cl);
        s->reset();
        s->pso = p;
    }
    return hr;
}
void STDMETHODCALLTYPE hk_set_targets(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (on()) {
        List* s = list_of(cl);
        s->back = 0;
        s->dsv = dsv != nullptr;
        if (rtvs && n) {
            std::shared_lock<std::shared_mutex> lk(g.views_m);
            for (UINT i = 0; i < n; ++i) {
                const SIZE_T at = single ? rtvs[0].ptr + SIZE_T(i) * g.rtv_step : rtvs[i].ptr;
                const auto it = g.rtvs.find(at);
                if (it != g.rtvs.end() && is_back(it->second)) { s->back = it->second; break; }
            }
        }
    }
    real_set_targets(cl, n, rtvs, single, dsv);
}
void STDMETHODCALLTYPE hk_viewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* v) {
    if (on() && n && v) list_of(cl)->vp = v[0];
    real_viewports(cl, n, v);
}
void STDMETHODCALLTYPE hk_scissors(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_RECT* r) {
    if (on() && n && r) list_of(cl)->scissor = r[0];
    real_scissors(cl, n, r);
}
void STDMETHODCALLTYPE hk_set_pipeline(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p) {
    if (on()) list_of(cl)->pso = p;
    real_set_pipeline(cl, p);
}
void STDMETHODCALLTYPE hk_draw(ID3D12GraphicsCommandList* cl, UINT v, UINT i, UINT fv, UINT fi) {
    if (on()) {
        List* s = list_of(cl);
        if (s->back) write(cl, *s, s->back, draw_kind(*s, v, i, false), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    real_draw(cl, v, i, fv, fi);
}
void STDMETHODCALLTYPE hk_draw_indexed(ID3D12GraphicsCommandList* cl, UINT n, UINT i, UINT fi, INT vo, UINT fin) {
    if (on()) {
        List* s = list_of(cl);
        if (s->back) write(cl, *s, s->back, draw_kind(*s, n, i, false), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    real_draw_indexed(cl, n, i, fi, vo, fin);
}
void STDMETHODCALLTYPE hk_execute_indirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args,
                                           UINT64 off, ID3D12Resource* count, UINT64 coff) {
    if (on()) {
        // a dispatch's signature is not told apart from a draw's: indirect work with the back buffer bound is not judged
        // (the frame is not trusted), and no copy is made before it (the back buffer may be in another state)
        List* s = list_of(cl);
        if (s->back) {
            if (s->recs.size() >= nr::hudcap::kMaxRecs) { s->recs.clear(); s->recs.push_back({kLost, s->back}); }
            s->recs.push_back({kUnknown, s->back});
            s->last = kOverlay;
        }
    }
    real_execute_indirect(cl, sig, max, args, off, count, coff);
}
void STDMETHODCALLTYPE hk_clear_target(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT* c, UINT n,
                                       const D3D12_RECT* r) {
    if (on()) {
        uint64_t res = 0;
        {
            std::shared_lock<std::shared_mutex> lk(g.views_m);
            const auto it = g.rtvs.find(rtv.ptr);
            if (it != g.rtvs.end()) res = it->second;
        }
        if (is_back(res)) write(cl, *list_of(cl), res, n ? kOverlay : kScene, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    real_clear_target(cl, rtv, c, n, r);
}
void STDMETHODCALLTYPE hk_copy_region(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                      const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (on() && dst && is_back(reinterpret_cast<uint64_t>(dst->pResource))) {
        // the extent copied: the box, else the whole source (a texture's top level, or a buffer's footprint)
        uint64_t cw = 0, ch = 0;
        if (box) { cw = box->right - box->left; ch = box->bottom - box->top; }
        else if (src && src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            cw = src->PlacedFootprint.Footprint.Width; ch = src->PlacedFootprint.Footprint.Height;
        } else if (src && src->pResource) {
            const D3D12_RESOURCE_DESC sd = src->pResource->GetDesc();
            cw = sd.Width; ch = sd.Height;
        }
        const bool whole = x == 0 && y == 0 && cw >= g.back_w.load() && ch >= g.back_h.load();
        write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst->pResource), whole ? kScene : kOverlay,
              D3D12_RESOURCE_STATE_COPY_DEST);
    }
    real_copy_region(cl, dst, x, y, z, src, box);
}
void STDMETHODCALLTYPE hk_copy_resource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    if (on() && is_back(reinterpret_cast<uint64_t>(dst)))
        write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst), kScene, D3D12_RESOURCE_STATE_COPY_DEST);
    real_copy_resource(cl, dst, src);
}
void STDMETHODCALLTYPE hk_resolve(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, UINT ds, ID3D12Resource* src, UINT ss,
                                  DXGI_FORMAT f) {
    if (on() && is_back(reinterpret_cast<uint64_t>(dst)))
        write(cl, *list_of(cl), reinterpret_cast<uint64_t>(dst), kScene, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    real_resolve(cl, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE hk_execute_bundle(ID3D12GraphicsCommandList* cl, ID3D12GraphicsCommandList* bundle) {
    // the bundle's draws see no back buffer of their own (its targets are the list's): a bound back buffer makes the
    // frame not trusted
    if (on()) {
        List* s = list_of(cl);
        if (s->back) { s->recs.push_back({kUnknown, s->back}); s->last = kOverlay; }
    }
    real_execute_bundle(cl, bundle);
}
void STDMETHODCALLTYPE hk_begin_pass(ID3D12GraphicsCommandList* cl, UINT n, const RenderPassTarget* rts, const void* ds, UINT flags) {
    if (on()) {
        List* s = list_of(cl);
        s->back = 0;
        s->dsv = ds != nullptr;
        s->in_pass = true;
        if (n && rts) {
            uint64_t res = 0;
            {
                std::shared_lock<std::shared_mutex> lk(g.views_m);
                const auto it = g.rtvs.find(rts[0].cpu.ptr);
                if (it != g.rtvs.end()) res = it->second;
            }
            if (is_back(res)) {
                s->back = res;
                if (rts[0].begin_type == 2) write(cl, *s, res, kScene, D3D12_RESOURCE_STATE_RENDER_TARGET);   // a clear
            }
        }
    }
    real_begin_pass(cl, n, rts, ds, flags);
}
void STDMETHODCALLTYPE hk_end_pass(ID3D12GraphicsCommandList* cl) {
    if (on()) {
        List* s = list_of(cl);
        s->in_pass = false;
        s->back = 0;
    }
    real_end_pass(cl);
}
void executed(UINT n, ID3D12CommandList* const* lists) {
    if (!g.lists_hooked || t_own || !lists) return;
    std::lock_guard<std::mutex> lk(g.m);
    for (UINT i = 0; i < n; ++i) {
        const auto it = g.lists.find(static_cast<void*>(lists[i]));
        if (it == g.lists.end() || it->second.recs.empty()) continue;
        // its records stay until the list is reset: executed again, its copies run again
        if (g.frame.size() < 16384) g.frame.insert(g.frame.end(), it->second.recs.begin(), it->second.recs.end());
    }
}
// Only where NR Control's queue hook is not there (it calls d3d12_executed after the call, through the .asi)
void STDMETHODCALLTYPE hk_execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    executed(n, lists);
    real_execute(q, n, lists);
}

// `object`'s vtable slot hooked with `detour` (once: a function hooked already is left as it is).
bool hook(void* object, int slot, void* detour, void** real, const char* name) {
    void* fn = (*reinterpret_cast<void***>(object))[slot];
    MH_STATUS st = MH_CreateHook(fn, detour, real);
    if (st == MH_OK) st = MH_EnableHook(fn);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        log("[nr] HUD FIX capture: %s could not be hooked (%s)", name, MH_StatusToString(st));
        return false;
    }
    return true;
}
template <class F> void* as(F f) { return reinterpret_cast<void*>(f); }

void hook_device(ID3D12Device* device) {
    if (g.device_hooked) return;
    g.device_hooked = true;
    MH_Initialize();
    g.rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    bool ok = hook(device, 10, as(&hk_create_graphics), reinterpret_cast<void**>(&real_create_graphics), "CreateGraphicsPipelineState") &&
              hook(device, 20, as(&hk_create_rtv), reinterpret_cast<void**>(&real_create_rtv), "CreateRenderTargetView") &&
              hook(device, 23, as(&hk_copy_descriptors), reinterpret_cast<void**>(&real_copy_descriptors), "CopyDescriptors") &&
              hook(device, 24, as(&hk_copy_descriptors_simple), reinterpret_cast<void**>(&real_copy_descriptors_simple),
                   "CopyDescriptorsSimple");
    // ID3D12Device2's CreatePipelineState (slot 47) and a pipeline library's loads (ID3D12Device1::CreatePipelineLibrary,
    // slot 44: one made empty for its vtable; LoadGraphicsPipeline 9, ID3D12PipelineLibrary1::LoadPipeline 13)
    static const GUID kDevice2 = {0x30baa41e, 0xb15b, 0x475c, {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};
    static const GUID kDevice1 = {0x77acce80, 0x638e, 0x4e65, {0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e}};
    static const GUID kLibrary = {0xc64226a8, 0x9201, 0x46af, {0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f}};
    static const GUID kLibrary1 = {0x80eabf42, 0x2568, 0x4e5e, {0xbd, 0x82, 0xc3, 0x7f, 0x86, 0x96, 0x1d, 0xc3}};
    IUnknown* d2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(kDevice2, reinterpret_cast<void**>(&d2))) && d2) {
        ok = hook(d2, 47, as(&hk_create_stream), reinterpret_cast<void**>(&real_create_stream), "CreatePipelineState") && ok;
        d2->Release();
    }
    IUnknown* d1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(kDevice1, reinterpret_cast<void**>(&d1))) && d1) {
        using PFN_CreateLibrary = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, const void*, SIZE_T, REFIID, void**);
        auto create = reinterpret_cast<PFN_CreateLibrary>((*reinterpret_cast<void***>(d1))[44]);
        IUnknown* lib = nullptr;
        if (SUCCEEDED(create(d1, nullptr, 0, kLibrary, reinterpret_cast<void**>(&lib))) && lib) {
            ok = hook(lib, 9, as(&hk_load_graphics), reinterpret_cast<void**>(&real_load_graphics), "LoadGraphicsPipeline") && ok;
            IUnknown* lib1 = nullptr;
            if (SUCCEEDED(lib->QueryInterface(kLibrary1, reinterpret_cast<void**>(&lib1))) && lib1) {
                ok = hook(lib1, 13, as(&hk_load_stream), reinterpret_cast<void**>(&real_load_stream), "LoadPipeline") && ok;
                lib1->Release();
            }
            lib->Release();
        }
        d1->Release();
    }
    log("[nr] HUD FIX capture: the device's render-target views and pipelines are followed%s", ok ? "" : " (in part: above)");
}

bool hook_lists(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g.lists_hooked) return true;
    static bool tried = false;   // once: a list it could not hook stays so (said once)
    if (tried) return false;
    tried = true;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) || !alloc) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&l))) || !l) {
        alloc->Release();
        return false;
    }
    bool ok = hook(l, 10, as(&hk_reset), reinterpret_cast<void**>(&real_reset), "Reset") &&
              hook(l, 12, as(&hk_draw), reinterpret_cast<void**>(&real_draw), "DrawInstanced") &&
              hook(l, 13, as(&hk_draw_indexed), reinterpret_cast<void**>(&real_draw_indexed), "DrawIndexedInstanced") &&
              hook(l, 16, as(&hk_copy_region), reinterpret_cast<void**>(&real_copy_region), "CopyTextureRegion") &&
              hook(l, 17, as(&hk_copy_resource), reinterpret_cast<void**>(&real_copy_resource), "CopyResource") &&
              hook(l, 19, as(&hk_resolve), reinterpret_cast<void**>(&real_resolve), "ResolveSubresource") &&
              hook(l, 21, as(&hk_viewports), reinterpret_cast<void**>(&real_viewports), "RSSetViewports") &&
              hook(l, 22, as(&hk_scissors), reinterpret_cast<void**>(&real_scissors), "RSSetScissorRects") &&
              hook(l, 25, as(&hk_set_pipeline), reinterpret_cast<void**>(&real_set_pipeline), "SetPipelineState") &&
              hook(l, 27, as(&hk_execute_bundle), reinterpret_cast<void**>(&real_execute_bundle), "ExecuteBundle") &&
              hook(l, 46, as(&hk_set_targets), reinterpret_cast<void**>(&real_set_targets), "OMSetRenderTargets") &&
              hook(l, 48, as(&hk_clear_target), reinterpret_cast<void**>(&real_clear_target), "ClearRenderTargetView") &&
              hook(l, 59, as(&hk_execute_indirect), reinterpret_cast<void**>(&real_execute_indirect), "ExecuteIndirect");
    static const GUID kList4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
    IUnknown* l4 = nullptr;
    if (SUCCEEDED(l->QueryInterface(kList4, reinterpret_cast<void**>(&l4))) && l4) {
        ok = hook(l4, 68, as(&hk_begin_pass), reinterpret_cast<void**>(&real_begin_pass), "BeginRenderPass") &&
             hook(l4, 69, as(&hk_end_pass), reinterpret_cast<void**>(&real_end_pass), "EndRenderPass") && ok;
        l4->Release();
    }
    if (queue) {   // MH_ERROR_ALREADY_CREATED (NR Control's hook, the usual case): its observer reports to us
        ok = hook(queue, 10, as(&hk_execute), reinterpret_cast<void**>(&real_execute), "ExecuteCommandLists") && ok;
    } else {
        ok = false;
        log("[nr] HUD FIX capture: the swap chain's queue was not found");
    }
    l->Release();
    alloc->Release();
    g.lists_hooked = ok;
    log("[nr] HUD FIX capture: the game's command lists are followed%s", ok ? "" : " - NOT: it does not run");
    return ok;
}

bool ensure_hudless(ID3D12Device* device, DXGI_FORMAT format, uint32_t w, uint32_t h) {
    ID3D12Resource* cur = g.hudless.load();
    if (cur) {
        const D3D12_RESOURCE_DESC d = cur->GetDesc();
        ID3D12Device* of = nullptr;
        const bool same = SUCCEEDED(cur->GetDevice(IID_PPV_ARGS(&of))) && of == device;
        if (of) of->Release();
        if (same && d.Width == w && d.Height == h && d.Format == format) return true;
        g.hudless.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> lk(g.m);
        g.retired.push_back({cur, g.presents});
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = format; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&r))) || !r) {
        log("[nr] HUD FIX capture: its image (format %u, %ux%u) could not be made", unsigned(format), w, h);
        return false;
    }
    g.hudless.store(r, std::memory_order_release);
    return true;
}


// ---- frame generation's HUD-less frame (D3D12) ----------------------------------------------------------------------
// The FidelityFX API's frame-generation configure (ffx_framegeneration.h, ffx_api.h, ffx_api_types.h: FidelityFX SDK,
// MIT), XeSS-FG's resource tag (xefg_swapchain_d3d12.h, Intel, MIT) and Streamline's tags (sl_core_types.h, NVIDIA,
// MIT): only the members read, in their layout.
struct FfxHeader { uint64_t type; FfxHeader* next; };
struct FfxResource {
    void* resource;
    uint32_t type, format, width, height, depth, mips, flags, usage;
    uint32_t state;
};
struct FfxConfigureFg {   // ffxConfigureDescFrameGeneration
    FfxHeader header;
    void* swap_chain;
    void* present_callback;
    void* present_context;
    void* fg_callback;
    void* fg_context;
    bool enabled, async;
    FfxResource hudless;
};
constexpr uint64_t kFfxConfigureFg = 0x00020002u;   // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION
struct XefgResource {      // xefg_swapchain_d3d12_resource_data_t
    uint32_t type, validity;   // type 0 HUDLESS_COLOR; validity 0 UNTIL_NEXT_PRESENT, 1 ONLY_NOW
    uint32_t base[2], size[2];
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state;
};
struct SlBase { void* next; uint8_t type[16]; size_t version; };
struct SlResource { SlBase base; char type; void* native; void* memory; void* view; uint32_t state; };
struct SlTag { SlBase base; SlResource* resource; uint32_t type; int lifecycle; uint32_t extent[4]; };   // lifecycle 0 only now

D3D12_RESOURCE_STATES d3d12_state_of_ffx(uint32_t s) {
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (s & (1u << 1)) r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & (1u << 2)) r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & (1u << 3)) r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & (1u << 4)) r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & (1u << 5)) r |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & (1u << 6)) r |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (s & (1u << 8)) r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return r;   // PRESENT and COMMON: 0
}
// Formats CopyResource takes as a pair: the same typeless group.
int format_group(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 2;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 3;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 4;
    case DXGI_FORMAT_R11G11B10_FLOAT: return 5;
    default: return 100 + int(f);
    }
}

struct FgState {
    std::atomic<bool> armed{false};
    std::atomic<int> in_present{0};
    std::mutex m;
    ID3D12Resource* copy = nullptr;   // ours
    uint32_t w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // the last hand-over
    uint64_t at = ~0ull;              // g.presents then (this frame's: the presents so far)
    ID3D12Resource* game = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool copied = false, until_present = false, substitute = false;
    const char* source = "";
    std::string refused;              // the last one not taken: why
    bool hooked_ffx[3] = {}, hooked_xefg = false, hooked_sl = false;
    uint64_t looked = 0;
} gf;

// A hand-over of `r` (in `state`; `list`: the game's command list it was made on, null none) by `source`.
void fg_note(ID3D12Resource* r, D3D12_RESOURCE_STATES state, ID3D12GraphicsCommandList* list, bool until_present,
             bool substitute, const char* source) {
    if (!r || !gf.armed.load(std::memory_order_relaxed) || t_own) return;
    std::lock_guard<std::mutex> lk(gf.m);
    if (gf.in_present.load()) { gf.refused = std::string(source) + "'s is handed over during the Present (OptiScaler's own frame generation): the capture serves it"; return; }
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    ID3D12Device* rd = nullptr, *cd = nullptr;
    if (gf.copy) { r->GetDevice(IID_PPV_ARGS(&rd)); gf.copy->GetDevice(IID_PPV_ARGS(&cd)); }
    const bool same_device = rd && rd == cd;
    if (rd) rd->Release();
    if (cd) cd->Release();
    if (!same_device) { gf.refused = std::string(source) + "'s is of another device"; return; }
    if (d.Width != gf.w || d.Height != gf.h || format_group(d.Format) != format_group(gf.format) || !gf.copy) {
        gf.refused = std::string(source) + "'s is not of the back buffer's size and format";
        return;
    }
    gf.at = g.presents;
    gf.game = r;
    gf.state = state;
    gf.source = source;
    gf.until_present = until_present;
    gf.substitute = substitute && until_present;
    gf.copied = false;
    gf.refused.clear();
    if (list && reinterpret_cast<uintptr_t>(list) > 0xFFFF) {   // copied now, on the game's list (XeSS-FG's fake list: 1)
        ++t_own;
        transition(list, r, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, gf.copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(gf.copy, r);
        transition(list, gf.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(list, r, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        --t_own;
        gf.copied = true;
    } else if (!until_present) {
        gf.at = ~0ull;
        gf.refused = std::string(source) + "'s is valid only when handed over, without a command list to copy it on";
    }
}

using PFN_FfxConfigure = int32_t (*)(void*, const FfxHeader*);
PFN_FfxConfigure real_ffx_configure[3];
template <int I> int32_t ffx_configure(void* ctx, const FfxHeader* desc) {
    for (const FfxHeader* h = desc; h; h = h->next)
        if (h->type == kFfxConfigureFg) {
            const auto* c = reinterpret_cast<const FfxConfigureFg*>(h);
            if (c->enabled && c->hudless.resource)
                fg_note(static_cast<ID3D12Resource*>(c->hudless.resource), d3d12_state_of_ffx(c->hudless.state), nullptr, true, true,
                        "FSR frame generation");
            break;
        }
    return real_ffx_configure[I](ctx, desc);
}
using PFN_XefgTag = int32_t (*)(void*, ID3D12CommandList*, uint32_t, const XefgResource*);
PFN_XefgTag real_xefg_tag;
int32_t xefg_tag(void* sc, ID3D12CommandList* list, uint32_t id, const XefgResource* r) {
    if (r && r->type == 0 && r->resource) {
        ID3D12GraphicsCommandList* gl = nullptr;
        if (list && reinterpret_cast<uintptr_t>(list) > 0xFFFF && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&gl))) && gl) gl->Release();
        fg_note(r->resource, r->state, gl, r->validity == 0, r->validity == 0, "XeSS frame generation");
    }
    return real_xefg_tag(sc, list, id, r);
}
void sl_tags(const SlTag* tags, uint32_t n, void* cmd) {
    for (uint32_t i = 0; tags && i < n; ++i) {
        const SlTag& t = tags[i];
        if (t.type != 2 || !t.resource || !t.resource->native) continue;   // kBufferTypeHUDLessColor
        ID3D12GraphicsCommandList* gl = nullptr;
        IUnknown* u = static_cast<IUnknown*>(cmd);
        if (u && reinterpret_cast<uintptr_t>(u) > 0xFFFF && SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&gl))) && gl) gl->Release();
        // lifecycle 0 valid only now, 1 until the present, 2 until its evaluate: only 1 is read at the Present; the others
        // are copied on their command list (refused without one)
        fg_note(static_cast<ID3D12Resource*>(t.resource->native), D3D12_RESOURCE_STATES(t.resource->state), gl, t.lifecycle == 1,
                t.lifecycle == 1, "DLSS frame generation (Streamline)");
    }
}
using PFN_SlSetTag = int (*)(const void*, const SlTag*, uint32_t, void*);
using PFN_SlSetTagForFrame = int (*)(const void*, const void*, const SlTag*, uint32_t, void*);
PFN_SlSetTag real_sl_set_tag;
PFN_SlSetTagForFrame real_sl_set_tag_frame;
int sl_set_tag(const void* vp, const SlTag* tags, uint32_t n, void* cmd) { sl_tags(tags, n, cmd); return real_sl_set_tag(vp, tags, n, cmd); }
int sl_set_tag_frame(const void* f, const void* vp, const SlTag* tags, uint32_t n, void* cmd) {
    sl_tags(tags, n, cmd);
    return real_sl_set_tag_frame(f, vp, tags, n, cmd);
}

bool hook_export(HMODULE m, const char* name, void* detour, void** real) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!fn) return false;
    MH_STATUS st = MH_CreateHook(fn, detour, real);
    if (st == MH_OK) st = MH_EnableHook(fn);
    if (st != MH_OK) { log("[nr] HUD FIX (frame generation): %s could not be hooked (%s)", name, MH_StatusToString(st)); return false; }
    return true;
}
// The frame-generation libraries as they come (looked for once a second of Presents): each hooked once.
void fg_hook_libraries() {
    if (g.presents - gf.looked < 60 && gf.looked) return;
    gf.looked = g.presents;
    MH_Initialize();
    static const wchar_t* const ffx[3] = {L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
                                          L"amd_fidelityfx_framegeneration_dx12.dll"};
    static void* const ffx_detour[3] = {as(&ffx_configure<0>), as(&ffx_configure<1>), as(&ffx_configure<2>)};
    for (int i = 0; i < 3; ++i)
        if (!gf.hooked_ffx[i])
            if (HMODULE m = GetModuleHandleW(ffx[i])) {
                gf.hooked_ffx[i] = true;
                if (hook_export(m, "ffxConfigure", ffx_detour[i], reinterpret_cast<void**>(&real_ffx_configure[i])))
                    log("[nr] HUD FIX (frame generation): %ls's ffxConfigure followed", ffx[i]);
            }
    if (!gf.hooked_xefg)
        if (HMODULE m = GetModuleHandleW(L"libxess_fg.dll")) {
            gf.hooked_xefg = true;
            if (hook_export(m, "xefgSwapChainD3D12TagFrameResource", as(&xefg_tag), reinterpret_cast<void**>(&real_xefg_tag)))
                log("[nr] HUD FIX (frame generation): libxess_fg.dll's resource tags followed");
        }
    if (!gf.hooked_sl)
        if (HMODULE m = GetModuleHandleW(L"sl.interposer.dll")) {
            gf.hooked_sl = true;
            const bool a = hook_export(m, "slSetTag", as(&sl_set_tag), reinterpret_cast<void**>(&real_sl_set_tag));
            const bool b = hook_export(m, "slSetTagForFrame", as(&sl_set_tag_frame), reinterpret_cast<void**>(&real_sl_set_tag_frame));
            if (a || b) log("[nr] HUD FIX (frame generation): Streamline's resource tags followed");
        }
}

}  // namespace

// The private object follows Release as well as Reset. Keeping a COM reference to the command list itself
// would prevent its release and could retain every recording forever.
nr::gpu::Ticket d3d12_recording(ID3D12GraphicsCommandList* list) {
    static const GUID key = {0x709ae383, 0x15a8, 0x4e89, {0xb5, 0x8c, 0x3a, 0x6a, 0xde, 0x2d, 0xa1, 0x7f}};
    if (auto t = nr::gpu::find(list)) return t;
    if (!hook(list, 10, as(&hk_reset), reinterpret_cast<void**>(&real_reset), "Reset")) return {};
    struct Lifetime final : IUnknown {
        std::atomic<ULONG> refs{1}; const void* key; nr::gpu::Ticket ticket;
        Lifetime(const void* k, nr::gpu::Ticket t) : key(k), ticket(std::move(t)) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override {
            if (!p) return E_POINTER;
            *p = nullptr;
            if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
            *p = this; AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs; if (!n) delete this; return n; }
        ~Lifetime() { nr::gpu::forget(key, ticket); }
    };
    auto ticket = nr::gpu::recording(list);
    auto* owned = new Lifetime(list, ticket);
    const HRESULT hr = list->SetPrivateDataInterface(key, owned);
    owned->Release();
    if (FAILED(hr)) return {};
    return ticket;
}

void d3d12_executed(unsigned count, ID3D12CommandList* const* lists) { executed(count, lists); }

Own::Own() { ++t_own; }
Own::~Own() { --t_own; }
PresentCall::PresentCall() { ++gf.in_present; }
PresentCall::~PresentCall() { --gf.in_present; }

void fg_note_dlssg(ID3D12GraphicsCommandList* list, ID3D12Resource* hudless) {
    if (hudless && list) fg_note(hudless, D3D12_RESOURCE_STATE_COPY_DEST, list, false, false, "DLSS frame generation (NGX)");
}

void d3d12_device(ID3D12Device* device, bool armed_now) {
    (void)armed_now;   // the device's creations are followed at once either way (cheap: they come at load and resize)
    if (!device) return;
    g.device.store(device);
    hook_device(device);
}

Frame d3d12_present(ID3D12Device* device, IDXGISwapChain* sc, ID3D12CommandQueue* queue, ID3D12Resource* back,
                    DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed, bool fg_armed) {
    Frame f;
    {   // frame generation's HUD-less frame: this frame's hand-over (since the last Present), and its copy made ready
        std::lock_guard<std::mutex> lk(gf.m);
        if (fg_armed && device) {
            if (gf.at == g.presents) {
                f.fg.seen = true;
                f.fg.copy = gf.copy; f.fg.copied = gf.copied; f.fg.game = gf.game; f.fg.state = gf.state;
                f.fg.substitute = gf.substitute; f.fg.source = gf.source;
            } else {
                f.fg.why = !gf.refused.empty() ? gf.refused : "the game hands its frame generation no HUD-less frame (none this frame)";
            }
            gf.refused.clear();
            bool other_device = false;
            if (gf.copy) {
                ID3D12Device* of = nullptr;
                other_device = FAILED(gf.copy->GetDevice(IID_PPV_ARGS(&of))) || of != device;
                if (of) of->Release();
            }
            if (!gf.copy || other_device || gf.w != w || gf.h != h || gf.format != format) {
                if (gf.copy) { std::lock_guard<std::mutex> lk2(g.m); g.retired.push_back({gf.copy, g.presents}); gf.copy = nullptr; }
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.Format = format; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           nullptr, IID_PPV_ARGS(&gf.copy)))) gf.copy = nullptr;
                gf.w = w; gf.h = h; gf.format = format;
                if (f.fg.seen) { f.fg.seen = false; f.fg.why = "its image is being made"; }
            }
        } else if (gf.armed.load()) {
            gf.at = ~0ull;
        }
        gf.armed.store(fg_armed && device);
    }
    if (fg_armed && device) fg_hook_libraries();
    ++g.presents;
    // earlier copies' images (g.retired) are kept: a list the game recorded with one may still run (it is not freed
    // without a proof that none can; each resize retires one or two)
    const bool was = g.armed.load();
    if (!armed) {
        if (was) {
            g.armed.store(false);
            std::lock_guard<std::mutex> lk(g.m);
            g.frame.clear();
        }
        return f;
    }
    if (!g.device_hooked) { g.late = true; hook_device(device); }
    if (!hook_lists(device, queue)) {
        f.result = Result::kMissed;
        f.why = "the game's command lists could not be followed (see the log)";
        f.broken = true;
        return f;
    }
    {   // the swap chain's back buffers
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            for (UINT i = 0; i < 8; ++i) {
                ID3D12Resource* b = nullptr;
                uint64_t v = 0;
                if (i < d.BufferCount && SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&b))) && b) { v = reinterpret_cast<uint64_t>(b); b->Release(); }
                g.backs[i].store(v);
            }
        }
        g.back_w.store(w);
        g.back_h.store(h);
    }
    const bool image = ensure_hudless(device, format, w, h);
    g.armed.store(true);
    std::vector<Rec> recs;
    {
        std::lock_guard<std::mutex> lk(g.m);
        recs.swap(g.frame);
    }
    g.copies.store(0);
    const bool foreign = g.foreign.exchange(false);
    if (!was) {
        f.result = Result::kMissed;
        f.why = "it starts with the next frame";
        return f;
    }
    const nr::hudcap::Verdict v = nr::hudcap::verdict(recs, reinterpret_cast<uint64_t>(back), foreign,
                                                      image ? reinterpret_cast<uint64_t>(g.hudless.load()) : 0, f.why);
    f.result = v == nr::hudcap::Verdict::kCaptured ? Result::kCaptured
               : v == nr::hudcap::Verdict::kClean ? Result::kClean : Result::kMissed;
    if (f.result == Result::kCaptured) f.hudless = g.hudless.load();
    if (g.late && f.result != Result::kCaptured)
        f.why += " (HUD FIX was switched on after the game made its pipelines and views: restart the game with it on)";
    return f;
}


// ---- D3D11 ----------------------------------------------------------------------------------------------------------
namespace {

struct State11 {
    std::atomic<bool> armed{false};
    ID3D11DeviceContext* imm = nullptr;
    uint64_t back_buffer = 0;   // the swap chain's buffer
    uint32_t w = 0, h = 0;
    ID3D11Texture2D* hudless = nullptr;
    bool hooked = false;
    // the immediate context's bindings and this frame's writes (the context's thread)
    uint64_t back = 0;
    bool dsv = false;
    ID3D11DepthStencilState* ds = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    D3D11_VIEWPORT vp{};
    D3D11_RECT sc{};
    uint8_t last = 0;
    std::vector<Rec> recs;
    uint32_t places = 0, last_place = 0;
    bool hooks_ok = false;
    void unbind() { back = 0; dsv = false; ds = nullptr; bs = nullptr; rs = nullptr; vp = {}; sc = {}; }
} g11;

bool on11(ID3D11DeviceContext* ctx) { return g11.armed.load(std::memory_order_relaxed) && !t_own && ctx == g11.imm; }

void write11(ID3D11DeviceContext* ctx, uint8_t kind) {
    if (kind == kOverlay && g11.last == kScene) {
        const uint32_t place = g11.places++;
        if (place >= g11.last_place && g11.hudless) {   // the last frame's last place: one copy a frame
            ++t_own;
            ctx->CopyResource(g11.hudless, reinterpret_cast<ID3D11Resource*>(g11.back_buffer));
            --t_own;
            g11.recs.push_back({kCopy, g11.back_buffer, reinterpret_cast<uint64_t>(g11.hudless)});
        }
    }
    if (g11.recs.size() >= nr::hudcap::kMaxRecs) {
        g11.recs.clear();
        g11.recs.push_back({kLost, g11.back_buffer});
    }
    g11.recs.push_back({kind, g11.back_buffer});
    g11.last = kind;
}

uint8_t draw_kind11(uint32_t vertices, uint32_t instances, bool indirect) {
    nr::hudcap::Draw d;
    d.dsv = g11.dsv;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;   // a null state: the default (depth on)
    if (g11.ds) g11.ds->GetDesc(&dd);
    d.depth = dd.DepthEnable ? 1 : 0;
    D3D11_BLEND_DESC bd{};
    if (g11.bs) g11.bs->GetDesc(&bd);
    d.blend = bd.RenderTarget[0].BlendEnable ? 1 : 0;
    D3D11_RASTERIZER_DESC rd{};
    if (g11.rs) g11.rs->GetDesc(&rd);
    d.scissor_on = rd.ScissorEnable != FALSE;
    d.vp[0] = g11.vp.TopLeftX; d.vp[1] = g11.vp.TopLeftY; d.vp[2] = g11.vp.Width; d.vp[3] = g11.vp.Height;
    d.sc[0] = g11.sc.left; d.sc[1] = g11.sc.top; d.sc[2] = g11.sc.right; d.sc[3] = g11.sc.bottom;
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, g11.w, g11.h);
}
void draw11(ID3D11DeviceContext* ctx, uint32_t vertices, uint32_t instances, bool indirect) {
    if (on11(ctx) && g11.back) write11(ctx, draw_kind11(vertices, instances, indirect));
}
bool is_back11(ID3D11Resource* r) { return r && reinterpret_cast<uint64_t>(r) == g11.back_buffer; }
uint64_t back_of(ID3D11RenderTargetView* v) {
    if (!v) return 0;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return 0;
    const uint64_t at = reinterpret_cast<uint64_t>(r);
    r->Release();
    return at == g11.back_buffer ? at : 0;
}

using PFN11_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN11_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN11_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN11_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN11_SetTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN11_SetTargetsUav = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                     UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PFN11_Blend = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT);
using PFN11_Depth = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using PFN11_DrawAuto = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN11_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN11_Raster = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
using PFN11_Viewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN11_Scissors = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using PFN11_CopyRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT,
                                                  const D3D11_BOX*);
using PFN11_Copy = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PFN11_Update = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using PFN11_Clear = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
using PFN11_Resolve = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using PFN11_Execute = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN11_ClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);

PFN11_DrawIndexed r11_draw_indexed;
PFN11_Draw r11_draw;
PFN11_DrawIndexedInstanced r11_draw_indexed_instanced;
PFN11_DrawInstanced r11_draw_instanced;
PFN11_SetTargets r11_set_targets;
PFN11_SetTargetsUav r11_set_targets_uav;
PFN11_Blend r11_blend;
PFN11_Depth r11_depth;
PFN11_DrawAuto r11_draw_auto;
PFN11_DrawIndirect r11_draw_indexed_indirect, r11_draw_indirect;
PFN11_Raster r11_raster;
PFN11_Viewports r11_viewports;
PFN11_Scissors r11_scissors;
PFN11_CopyRegion r11_copy_region;
PFN11_Copy r11_copy;
PFN11_Update r11_update;
PFN11_Clear r11_clear;
PFN11_Resolve r11_resolve;
PFN11_Execute r11_execute;
PFN11_ClearState r11_clear_state;

void STDMETHODCALLTYPE k11_draw_indexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b) { draw11(c, n, 1, false); r11_draw_indexed(c, n, s, b); }
void STDMETHODCALLTYPE k11_draw(ID3D11DeviceContext* c, UINT n, UINT s) { draw11(c, n, 1, false); r11_draw(c, n, s); }
void STDMETHODCALLTYPE k11_draw_indexed_instanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, INT b, UINT si) {
    draw11(c, n, i, false);
    r11_draw_indexed_instanced(c, n, i, s, b, si);
}
void STDMETHODCALLTYPE k11_draw_instanced(ID3D11DeviceContext* c, UINT n, UINT i, UINT s, UINT si) {
    draw11(c, n, i, false);
    r11_draw_instanced(c, n, i, s, si);
}
void STDMETHODCALLTYPE k11_draw_auto(ID3D11DeviceContext* c) { draw11(c, 0, 0, true); r11_draw_auto(c); }
void STDMETHODCALLTYPE k11_draw_indexed_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    draw11(c, 0, 0, true);
    r11_draw_indexed_indirect(c, b, o);
}
void STDMETHODCALLTYPE k11_draw_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    draw11(c, 0, 0, true);
    r11_draw_indirect(c, b, o);
}
void targets11(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!on11(c)) return;
    g11.back = 0;
    g11.dsv = dsv != nullptr;
    for (UINT i = 0; rtvs && i < n && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (const uint64_t b = back_of(rtvs[i])) { g11.back = b; break; }
}
void STDMETHODCALLTYPE k11_set_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    targets11(c, n, rtvs, dsv);
    r11_set_targets(c, n, rtvs, dsv);
}
void STDMETHODCALLTYPE k11_set_targets_uav(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                           UINT us, UINT un, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) targets11(c, n, rtvs, dsv);
    r11_set_targets_uav(c, n, rtvs, dsv, us, un, uavs, counts);
}
void STDMETHODCALLTYPE k11_blend(ID3D11DeviceContext* c, ID3D11BlendState* s, const FLOAT* f, UINT m) {
    if (on11(c)) g11.bs = s;
    r11_blend(c, s, f, m);
}
void STDMETHODCALLTYPE k11_depth(ID3D11DeviceContext* c, ID3D11DepthStencilState* s, UINT r) {
    if (on11(c)) g11.ds = s;
    r11_depth(c, s, r);
}
void STDMETHODCALLTYPE k11_raster(ID3D11DeviceContext* c, ID3D11RasterizerState* s) {
    if (on11(c)) g11.rs = s;
    r11_raster(c, s);
}
void STDMETHODCALLTYPE k11_viewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (on11(c) && n && v) g11.vp = v[0];
    r11_viewports(c, n, v);
}
void STDMETHODCALLTYPE k11_scissors(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (on11(c) && n && r) g11.sc = r[0];
    r11_scissors(c, n, r);
}
bool whole11(UINT x, UINT y, ID3D11Resource* src, const D3D11_BOX* box) {
    if (x || y) return false;
    if (box) return box->right - box->left >= g11.w && box->bottom - box->top >= g11.h;
    ID3D11Texture2D* t = nullptr;
    if (!src || FAILED(src->QueryInterface(IID_PPV_ARGS(&t))) || !t) return false;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    t->Release();
    return d.Width >= g11.w && d.Height >= g11.h;
}
void STDMETHODCALLTYPE k11_copy_region(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src,
                                       UINT ss, const D3D11_BOX* box) {
    if (on11(c) && is_back11(dst)) write11(c, whole11(x, y, src, box) ? kScene : kOverlay);
    r11_copy_region(c, dst, ds, x, y, z, src, ss, box);
}
void STDMETHODCALLTYPE k11_copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (on11(c) && is_back11(dst)) write11(c, kScene);
    r11_copy(c, dst, src);
}
void STDMETHODCALLTYPE k11_update(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data,
                                  UINT pitch, UINT depth) {
    if (on11(c) && is_back11(dst))
        write11(c, !box || (box->left == 0 && box->top == 0 && box->right >= g11.w && box->bottom >= g11.h) ? kScene : kOverlay);
    r11_update(c, dst, sub, box, data, pitch, depth);
}
void STDMETHODCALLTYPE k11_clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* colour) {
    if (on11(c) && back_of(v)) write11(c, kScene);
    r11_clear(c, v, colour);
}
void STDMETHODCALLTYPE k11_resolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (on11(c) && is_back11(dst)) write11(c, kScene);
    r11_resolve(c, dst, ds, src, ss, f);
}
void STDMETHODCALLTYPE k11_execute(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL restore) {
    r11_execute(c, l, restore);
    if (on11(c)) {
        // a deferred context's work: its writes on the back buffer are not seen - after it the frame is not trusted
        g11.recs.push_back({kUnknown, g11.back_buffer});
        g11.last = kOverlay;
        if (!restore) g11.unbind();   // the context's state cleared after it
    }
}
void STDMETHODCALLTYPE k11_clear_state(ID3D11DeviceContext* c) {
    r11_clear_state(c);
    if (on11(c)) g11.unbind();
}

bool hook11(ID3D11DeviceContext* ctx) {
    if (g11.hooked) return g11.hooks_ok;
    g11.hooked = true;
    MH_Initialize();
    const struct { int slot; void* detour; void** real; const char* name; } hooks[] = {
        {12, as(&k11_draw_indexed), reinterpret_cast<void**>(&r11_draw_indexed), "DrawIndexed"},
        {13, as(&k11_draw), reinterpret_cast<void**>(&r11_draw), "Draw"},
        {20, as(&k11_draw_indexed_instanced), reinterpret_cast<void**>(&r11_draw_indexed_instanced), "DrawIndexedInstanced"},
        {21, as(&k11_draw_instanced), reinterpret_cast<void**>(&r11_draw_instanced), "DrawInstanced"},
        {33, as(&k11_set_targets), reinterpret_cast<void**>(&r11_set_targets), "OMSetRenderTargets"},
        {34, as(&k11_set_targets_uav), reinterpret_cast<void**>(&r11_set_targets_uav), "OMSetRenderTargetsAndUnorderedAccessViews"},
        {35, as(&k11_blend), reinterpret_cast<void**>(&r11_blend), "OMSetBlendState"},
        {36, as(&k11_depth), reinterpret_cast<void**>(&r11_depth), "OMSetDepthStencilState"},
        {38, as(&k11_draw_auto), reinterpret_cast<void**>(&r11_draw_auto), "DrawAuto"},
        {39, as(&k11_draw_indexed_indirect), reinterpret_cast<void**>(&r11_draw_indexed_indirect), "DrawIndexedInstancedIndirect"},
        {40, as(&k11_draw_indirect), reinterpret_cast<void**>(&r11_draw_indirect), "DrawInstancedIndirect"},
        {43, as(&k11_raster), reinterpret_cast<void**>(&r11_raster), "RSSetState"},
        {44, as(&k11_viewports), reinterpret_cast<void**>(&r11_viewports), "RSSetViewports"},
        {45, as(&k11_scissors), reinterpret_cast<void**>(&r11_scissors), "RSSetScissorRects"},
        {46, as(&k11_copy_region), reinterpret_cast<void**>(&r11_copy_region), "CopySubresourceRegion"},
        {47, as(&k11_copy), reinterpret_cast<void**>(&r11_copy), "CopyResource"},
        {48, as(&k11_update), reinterpret_cast<void**>(&r11_update), "UpdateSubresource"},
        {50, as(&k11_clear), reinterpret_cast<void**>(&r11_clear), "ClearRenderTargetView"},
        {57, as(&k11_resolve), reinterpret_cast<void**>(&r11_resolve), "ResolveSubresource"},
        {58, as(&k11_execute), reinterpret_cast<void**>(&r11_execute), "ExecuteCommandList"},
        {110, as(&k11_clear_state), reinterpret_cast<void**>(&r11_clear_state), "ClearState"},
    };
    bool ok = true;
    for (const auto& k : hooks) ok = hook(ctx, k.slot, k.detour, k.real, k.name) && ok;
    log("[nr] HUD FIX capture (D3D11): the immediate context is followed%s", ok ? "" : " - NOT in full (above): it does not run");
    g11.hooks_ok = ok;
    return ok;
}

}  // namespace

Frame11 d3d11_present(ID3D11Device* device, ID3D11Texture2D* back, DXGI_FORMAT format, uint32_t w, uint32_t h, bool armed) {
    Frame11 f;
    const bool was = g11.armed.load();
    if (!armed || !device || !back) {
        if (was) { g11.armed.store(false); g11.recs.clear(); g11.last = 0; g11.places = 0; }
        return f;
    }
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) { f.result = Result::kMissed; f.why = "the game's context was not found"; return f; }
    ctx->Release();   // the device holds it
    if (g11.imm != ctx) {   // another device: its own copy image
        g11.imm = ctx;
        g11.unbind();
        if (g11.hudless) { g11.hudless->Release(); g11.hudless = nullptr; }
    }
    if (!hook11(ctx)) {
        f.result = Result::kMissed;
        f.why = "the game's D3D11 context could not be followed (see the log)";
        f.broken = true;
        return f;
    }
    g11.back_buffer = reinterpret_cast<uint64_t>(static_cast<ID3D11Resource*>(back));
    g11.w = w; g11.h = h;
    bool image = false;
    if (g11.hudless) {
        D3D11_TEXTURE2D_DESC d{};
        g11.hudless->GetDesc(&d);
        if (d.Width == w && d.Height == h && d.Format == format) image = true;
        else { g11.hudless->Release(); g11.hudless = nullptr; }   // D3D11 keeps it while the GPU uses it
    }
    if (!image) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = format; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        image = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g11.hudless)) && g11.hudless;
        if (!image) { g11.hudless = nullptr; log("[nr] HUD FIX capture (D3D11): its image (format %u) could not be made", unsigned(format)); }
    }
    g11.armed.store(true);
    std::vector<Rec> recs;
    recs.swap(g11.recs);
    g11.last = 0;
    g11.last_place = g11.places ? g11.places - 1 : 0;
    g11.places = 0;
    if (!was) { f.result = Result::kMissed; f.why = "it starts with the next frame"; return f; }
    const nr::hudcap::Verdict v =
        nr::hudcap::verdict(recs, g11.back_buffer, false, image ? reinterpret_cast<uint64_t>(g11.hudless) : 0, f.why);
    f.result = v == nr::hudcap::Verdict::kCaptured ? Result::kCaptured
               : v == nr::hudcap::Verdict::kClean ? Result::kClean : Result::kMissed;
    if (f.result == Result::kCaptured) f.hudless = g11.hudless;
    return f;
}


// ---- Vulkan ---------------------------------------------------------------------------------------------------------
// Every hooked function twice where winevulkan hands two addresses (its export, and what its vkGetDeviceProcAddr gives
// when that is another one), as the .asi's queue hooks: a detour instance each, its own original.
namespace {

struct RpInfo {   // a render pass: its first subpass's colour attachments, each one's load op and initial layout
    std::vector<uint32_t> colour;
    std::vector<VkAttachmentLoadOp> load;
    std::vector<VkImageLayout> initial;
    bool depth = false;
};
struct FbInfo {
    VkRenderPass rp = VK_NULL_HANDLE;
    std::vector<VkImageView> views;   // empty: imageless (the views come with vkCmdBeginRenderPass)
};
struct VkPipeFacts {
    int8_t depth = -1;   // -2: dynamic (vkCmdSetDepthTestEnable)
    int8_t blend = -1;
    bool dyn_vp = true, dyn_sc = true;
    VkViewport vp{};
    VkRect2D sc{};
};
struct VkList {
    uint64_t back = 0;
    bool dsv = false, in_pass = false;
    VkPipeline pipe = VK_NULL_HANDLE;
    int8_t dyn_depth = -1;
    VkViewport vp{};
    VkRect2D sc{};
    uint8_t last = 0;
    std::vector<Rec> recs;
    void reset() { back = 0; dsv = in_pass = false; pipe = VK_NULL_HANDLE; dyn_depth = -1; vp = {}; sc = {}; last = 0; recs.clear(); }
};
struct StateVk {
    std::atomic<bool> armed{false};
    std::atomic<VkDevice> device{VK_NULL_HANDLE};
    std::atomic<uint64_t> backs[16];
    std::atomic<uint32_t> w{0}, h{0};
    std::atomic<uint64_t> hudless{0};
    std::atomic<uint32_t> copies{0};
    std::atomic<bool> foreign{false};   // a secondary command buffer or a next subpass on a back buffer: not followed
    bool created_hooked = false, cmds_hooked = false;
    std::shared_mutex m_obj;   // views, framebuffers, render passes, pipelines
    std::unordered_map<uint64_t, uint64_t> views;   // view -> image
    std::unordered_map<uint64_t, VkImageUsageFlags> usage;   // image -> its usage
    std::unordered_set<uint64_t> reordered;                  // views whose channels are not in their own order
    std::unordered_map<uint64_t, FbInfo> fbs;
    std::unordered_map<uint64_t, RpInfo> rps;
    std::unordered_map<uint64_t, VkPipeFacts> pipes;
    std::mutex m;   // lists' map, frame
    std::unordered_map<VkCommandBuffer, VkList> lists;   // never erased
    std::vector<Rec> frame;
} gv;

bool is_back_vk(uint64_t img) {
    if (!img) return false;
    for (const auto& b : gv.backs)
        if (b.load(std::memory_order_relaxed) == img) return true;
    return false;
}
VkList* vlist(VkCommandBuffer cb) {
    thread_local VkCommandBuffer last_cb = VK_NULL_HANDLE;
    thread_local VkList* last_state = nullptr;
    if (cb == last_cb) return last_state;
    std::lock_guard<std::mutex> lk(gv.m);
    VkList* s = &gv.lists[cb];
    last_cb = cb;
    last_state = s;
    return s;
}
bool on_vk() { return gv.armed.load(std::memory_order_relaxed) && !t_own; }
uint64_t image_of_view(VkImageView v) {
    std::shared_lock<std::shared_mutex> lk(gv.m_obj);
    const auto it = gv.views.find(reinterpret_cast<uint64_t>(v));
    return it == gv.views.end() ? 0 : it->second;
}

void vk_write(VkCommandBuffer cb, VkList& s, uint64_t b, uint8_t kind) {
    if (s.recs.size() >= nr::hudcap::kMaxRecs) {
        s.recs.clear();
        s.recs.push_back({kLost, b});
    }
    s.recs.push_back({kind, b});
    s.last = kind;
    (void)cb;
}
// The copy before a render pass on back buffer `b`, which is in `layout` then (outside any pass).
void vk_copy_before(VkCommandBuffer cb, VkList& s, uint64_t b, VkImageLayout layout) {
    const VkImage hl = reinterpret_cast<VkImage>(gv.hudless.load(std::memory_order_acquire));
    if (!hl || layout == VK_IMAGE_LAYOUT_UNDEFINED || s.last == kOverlay ||
        gv.copies.fetch_add(1, std::memory_order_relaxed) >= nr::hudcap::kMaxCopies)
        return;
    ++t_own;
    const VkImage back = reinterpret_cast<VkImage>(b);
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier in[2]{};
    for (auto& x : in) { x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; x.subresourceRange = all; }
    in[0].image = back; in[0].oldLayout = layout; in[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    in[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; in[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    in[1].image = hl; in[1].oldLayout = in[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    in[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; in[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, in);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {gv.w.load(), gv.h.load(), 1};
    vkCmdCopyImage(cb, back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hl, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    VkImageMemoryBarrier out[2] = {in[0], in[1]};
    out[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; out[0].newLayout = layout;
    out[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; out[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    out[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; out[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, out);
    --t_own;
    s.recs.push_back({kCopy, b, reinterpret_cast<uint64_t>(hl)});
}
uint8_t vk_draw_kind(const VkList& s, uint32_t vertices, uint32_t instances, bool indirect) {
    VkPipeFacts f;
    {
        std::shared_lock<std::shared_mutex> lk(gv.m_obj);
        const auto it = gv.pipes.find(reinterpret_cast<uint64_t>(s.pipe));
        if (it != gv.pipes.end()) f = it->second;
    }
    nr::hudcap::Draw d;
    d.dsv = s.dsv;
    d.depth = f.depth == -2 ? s.dyn_depth : f.depth;
    d.blend = f.blend;
    d.scissor_on = true;   // Vulkan always cuts by its scissor
    VkViewport vp = f.dyn_vp ? s.vp : f.vp;
    if (vp.height < 0) { vp.y += vp.height; vp.height = -vp.height; }   // a flipped viewport
    const VkRect2D sc = f.dyn_sc ? s.sc : f.sc;
    d.vp[0] = vp.x; d.vp[1] = vp.y; d.vp[2] = vp.width; d.vp[3] = vp.height;
    d.sc[0] = sc.offset.x; d.sc[1] = sc.offset.y; d.sc[2] = sc.offset.x + int32_t(sc.extent.width);
    d.sc[3] = sc.offset.y + int32_t(sc.extent.height);
    d.vertices = vertices; d.instances = instances; d.indirect = indirect;
    return nr::hudcap::draw_kind(d, gv.w.load(std::memory_order_relaxed), gv.h.load(std::memory_order_relaxed));
}
void vk_draw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, bool indirect) {
    if (!on_vk()) return;
    VkList* s = vlist(cb);
    if (s->back) vk_write(cb, *s, s->back, vk_draw_kind(*s, vertices, instances, indirect));
}
// A transfer into back buffer `img` covering `x, y, w x h` of it: a scene write when whole.
void vk_transfer(VkCommandBuffer cb, VkImage img, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!on_vk() || !is_back_vk(reinterpret_cast<uint64_t>(img))) return;
    const bool whole = x == 0 && y == 0 && w >= gv.w.load() && h >= gv.h.load();
    VkList* s = vlist(cb);
    vk_write(cb, *s, reinterpret_cast<uint64_t>(img), whole ? kScene : kOverlay);
}

// creation
PFN_vkCreateImageView rv_create_view[2];
PFN_vkCreateFramebuffer rv_create_fb[2];
PFN_vkCreateRenderPass rv_create_rp[2];
PFN_vkCreateRenderPass2 rv_create_rp2[2], rv_create_rp2_khr[2];
PFN_vkCreateGraphicsPipelines rv_create_pipes[2];
PFN_vkCreateImage rv_create_image[2];
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_view(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a,
                                                             VkImageView* out) {
    const VkResult r = rv_create_view[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        const auto own = [](VkComponentSwizzle s, VkComponentSwizzle self) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == self; };
        const bool reordered = !own(ci->components.r, VK_COMPONENT_SWIZZLE_R) || !own(ci->components.g, VK_COMPONENT_SWIZZLE_G) ||
                               !own(ci->components.b, VK_COMPONENT_SWIZZLE_B) || !own(ci->components.a, VK_COMPONENT_SWIZZLE_A);
        const uint64_t v = reinterpret_cast<uint64_t>(*out);
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.views[v] = reinterpret_cast<uint64_t>(ci->image);
        if (reordered) gv.reordered.insert(v); else gv.reordered.erase(v);
    }
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_image(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a,
                                                              VkImage* out) {
    const VkResult r = rv_create_image[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.usage[reinterpret_cast<uint64_t>(*out)] = ci->usage;   // a handle made again is written again
    }
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_fb(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkFramebuffer* out) {
    const VkResult r = rv_create_fb[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        FbInfo f;
        f.rp = ci->renderPass;
        if (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->pAttachments)
            f.views.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.fbs[reinterpret_cast<uint64_t>(*out)] = std::move(f);
    }
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_rp(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a,
                                                           VkRenderPass* out) {
    const VkResult r = rv_create_rp[I](d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && d == gv.device.load()) {
        RpInfo p;
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
            p.load.push_back(ci->pAttachments[i].loadOp);
            p.initial.push_back(ci->pAttachments[i].initialLayout);
        }
        if (ci->subpassCount) {
            const VkSubpassDescription& sp = ci->pSubpasses[0];
            for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) p.colour.push_back(sp.pColorAttachments[i].attachment);
            p.depth = sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;
        }
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.rps[reinterpret_cast<uint64_t>(*out)] = std::move(p);
    }
    return r;
}
void remember_rp2(VkDevice d, const VkRenderPassCreateInfo2* ci, VkRenderPass* out) {
    if (!ci || !out || d != gv.device.load()) return;
    RpInfo p;
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
        p.load.push_back(ci->pAttachments[i].loadOp);
        p.initial.push_back(ci->pAttachments[i].initialLayout);
    }
    if (ci->subpassCount) {
        const VkSubpassDescription2& sp = ci->pSubpasses[0];
        for (uint32_t i = 0; i < sp.colorAttachmentCount; ++i) p.colour.push_back(sp.pColorAttachments[i].attachment);
        p.depth = sp.pDepthStencilAttachment && sp.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;
    }
    std::unique_lock<std::shared_mutex> lk(gv.m_obj);
    gv.rps[reinterpret_cast<uint64_t>(*out)] = std::move(p);
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_rp2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a,
                                                            VkRenderPass* out) {
    const VkResult r = rv_create_rp2[I](d, ci, a, out);
    if (r == VK_SUCCESS) remember_rp2(d, ci, out);
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_rp2_khr(VkDevice d, const VkRenderPassCreateInfo2* ci,
                                                                const VkAllocationCallbacks* a, VkRenderPass* out) {
    const VkResult r = rv_create_rp2_khr[I](d, ci, a, out);
    if (r == VK_SUCCESS) remember_rp2(d, ci, out);
    return r;
}
template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_create_pipes(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                              const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a,
                                                              VkPipeline* out) {
    const VkResult r = rv_create_pipes[I](d, cache, n, ci, a, out);
    if ((r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED) && ci && out && d == gv.device.load()) {
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        for (uint32_t i = 0; i < n; ++i) {
            if (!out[i]) continue;
            const VkGraphicsPipelineCreateInfo& c = ci[i];
            VkPipeFacts f;
            bool dyn_depth = false, dyn_blend = false;
            f.dyn_vp = f.dyn_sc = false;
            if (c.pDynamicState)
                for (uint32_t k = 0; k < c.pDynamicState->dynamicStateCount; ++k) {
                    const VkDynamicState ds = c.pDynamicState->pDynamicStates[k];
                    if (ds == VK_DYNAMIC_STATE_VIEWPORT || ds == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) f.dyn_vp = true;
                    if (ds == VK_DYNAMIC_STATE_SCISSOR || ds == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) f.dyn_sc = true;
                    if (ds == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) dyn_depth = true;
                    if (ds == VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT) dyn_blend = true;
                }
            f.depth = dyn_depth ? -2 : (c.pDepthStencilState ? (c.pDepthStencilState->depthTestEnable ? 1 : 0) : 0);
            f.blend = dyn_blend || !c.pColorBlendState || !c.pColorBlendState->attachmentCount
                          ? int8_t(dyn_blend ? -1 : 0) : int8_t(c.pColorBlendState->pAttachments[0].blendEnable ? 1 : 0);
            if (c.pViewportState) {
                if (!f.dyn_vp && c.pViewportState->pViewports && c.pViewportState->viewportCount) f.vp = c.pViewportState->pViewports[0];
                if (!f.dyn_sc && c.pViewportState->pScissors && c.pViewportState->scissorCount) f.sc = c.pViewportState->pScissors[0];
            }
            gv.pipes[reinterpret_cast<uint64_t>(out[i])] = f;
        }
    }
    return r;
}

// commands
PFN_vkBeginCommandBuffer rv_begin_cb[2];
PFN_vkCmdBeginRenderPass rv_begin_rp[2];
PFN_vkCmdBeginRenderPass2 rv_begin_rp2[2], rv_begin_rp2_khr[2];
PFN_vkCmdEndRenderPass rv_end_rp[2];
PFN_vkCmdEndRenderPass2 rv_end_rp2[2], rv_end_rp2_khr[2];
PFN_vkCmdBeginRendering rv_begin_r[2], rv_begin_r_khr[2];
PFN_vkCmdEndRendering rv_end_r[2], rv_end_r_khr[2];
PFN_vkCmdBindPipeline rv_bind_pipe[2];
PFN_vkCmdSetViewport rv_set_vp[2];
PFN_vkCmdSetScissor rv_set_sc[2];
PFN_vkCmdSetDepthTestEnable rv_set_depth[2], rv_set_depth_ext[2];
PFN_vkCmdDraw rv_draw[2];
PFN_vkCmdDrawIndexed rv_draw_indexed[2];
PFN_vkCmdDrawIndirect rv_draw_indirect[2];
PFN_vkCmdDrawIndexedIndirect rv_draw_indexed_indirect[2];
PFN_vkCmdDrawIndirectCount rv_draw_count[2], rv_draw_indexed_count[2];
PFN_vkCmdCopyImage rv_copy_image[2];
PFN_vkCmdBlitImage rv_blit_image[2];
PFN_vkCmdCopyBufferToImage rv_copy_buffer[2];
PFN_vkCmdClearColorImage rv_clear_colour[2];
PFN_vkCmdResolveImage rv_resolve[2];
PFN_vkCmdExecuteCommands rv_execute_commands[2];
PFN_vkCmdClearAttachments rv_clear_attachments[2];
PFN_vkCmdNextSubpass rv_next_subpass[2];
PFN_vkCmdNextSubpass2 rv_next_subpass2[2], rv_next_subpass2_khr[2];

template <int I> VKAPI_ATTR VkResult VKAPI_CALL kv_begin_cb(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    const VkResult result = rv_begin_cb[I](cb, info);
    if (result == VK_SUCCESS) {
        nr::gpu::reset(cb);
        if (gv.cmds_hooked && !t_own) vlist(cb)->reset();
    }
    return result;
}
// A render pass begins on `views` with `rp`'s attachments (or, dynamic rendering, each attachment's own load op and
// layout): the copy before it where it keeps a back buffer's contents.
void vk_pass(VkCommandBuffer cb, uint64_t img, VkAttachmentLoadOp load, VkImageLayout layout, bool dsv) {
    VkList* s = vlist(cb);
    s->back = 0;
    s->dsv = dsv;
    if (!is_back_vk(img)) { s->in_pass = true; return; }
    if (load == VK_ATTACHMENT_LOAD_OP_LOAD) vk_copy_before(cb, *s, img, layout);
    s->in_pass = true;
    s->back = img;
    if (load != VK_ATTACHMENT_LOAD_OP_LOAD) vk_write(cb, *s, img, kScene);   // cleared or left undefined: a scene start
}
void vk_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo* info) {
    if (!on_vk() || !info) return;
    FbInfo fb;
    RpInfo rp;
    {
        std::shared_lock<std::shared_mutex> lk(gv.m_obj);
        const auto f = gv.fbs.find(reinterpret_cast<uint64_t>(info->framebuffer));
        const auto p = gv.rps.find(reinterpret_cast<uint64_t>(info->renderPass));
        if (f == gv.fbs.end() || p == gv.rps.end()) { VkList* s = vlist(cb); s->back = 0; s->in_pass = true; return; }
        fb = f->second;
        rp = p->second;
    }
    std::vector<VkImageView> views = fb.views;
    if (views.empty())   // imageless: VkRenderPassAttachmentBeginInfo
        for (const VkBaseInStructure* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                const auto* a = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(n);
                views.assign(a->pAttachments, a->pAttachments + a->attachmentCount);
            }
    for (uint32_t c : rp.colour) {
        if (c == VK_ATTACHMENT_UNUSED || c >= views.size() || c >= rp.load.size()) continue;
        const uint64_t img = image_of_view(views[c]);
        if (!is_back_vk(img)) continue;
        vk_pass(cb, img, rp.load[c], rp.initial[c], rp.depth);
        return;
    }
    vk_pass(cb, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_UNDEFINED, rp.depth);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_rp(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents c) {
    vk_begin_rp(cb, info);
    rv_begin_rp[I](cb, info, c);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_rp2(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* sb) {
    vk_begin_rp(cb, info);
    rv_begin_rp2[I](cb, info, sb);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_rp2_khr(VkCommandBuffer cb, const VkRenderPassBeginInfo* info,
                                                             const VkSubpassBeginInfo* sb) {
    vk_begin_rp(cb, info);
    rv_begin_rp2_khr[I](cb, info, sb);
}
void vk_end(VkCommandBuffer cb) {
    if (!on_vk()) return;
    VkList* s = vlist(cb);
    s->in_pass = false;
    s->back = 0;
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_rp(VkCommandBuffer cb) { vk_end(cb); rv_end_rp[I](cb); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_rp2(VkCommandBuffer cb, const VkSubpassEndInfo* e) { vk_end(cb); rv_end_rp2[I](cb, e); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_rp2_khr(VkCommandBuffer cb, const VkSubpassEndInfo* e) { vk_end(cb); rv_end_rp2_khr[I](cb, e); }
void vk_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    if (!on_vk() || !info) return;
    const bool dsv = info->pDepthAttachment && info->pDepthAttachment->imageView;
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo& a = info->pColorAttachments[i];
        const uint64_t img = image_of_view(a.imageView);
        if (!is_back_vk(img)) continue;
        vk_pass(cb, img, a.loadOp, a.imageLayout, dsv);
        return;
    }
    vk_pass(cb, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_UNDEFINED, dsv);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_r(VkCommandBuffer cb, const VkRenderingInfo* info) { vk_begin_rendering(cb, info); rv_begin_r[I](cb, info); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_begin_r_khr(VkCommandBuffer cb, const VkRenderingInfo* info) { vk_begin_rendering(cb, info); rv_begin_r_khr[I](cb, info); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_r(VkCommandBuffer cb) { vk_end(cb); rv_end_r[I](cb); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_end_r_khr(VkCommandBuffer cb) { vk_end(cb); rv_end_r_khr[I](cb); }
template <int I> VKAPI_ATTR void VKAPI_CALL kv_bind_pipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (on_vk() && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) vlist(cb)->pipe = p;
    rv_bind_pipe[I](cb, bp, p);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_vp(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    if (on_vk() && first == 0 && n && v) vlist(cb)->vp = v[0];
    rv_set_vp[I](cb, first, n, v);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_sc(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    if (on_vk() && first == 0 && n && r) vlist(cb)->sc = r[0];
    rv_set_sc[I](cb, first, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_depth(VkCommandBuffer cb, VkBool32 on) {
    if (on_vk()) vlist(cb)->dyn_depth = on ? 1 : 0;
    rv_set_depth[I](cb, on);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_set_depth_ext(VkCommandBuffer cb, VkBool32 on) {
    if (on_vk()) vlist(cb)->dyn_depth = on ? 1 : 0;
    rv_set_depth_ext[I](cb, on);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) {
    vk_draw(cb, v, i, false);
    rv_draw[I](cb, v, i, fv, fi);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fi, int32_t vo, uint32_t fin) {
    vk_draw(cb, n, i, false);
    rv_draw_indexed[I](cb, n, i, fi, vo, fin);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_indirect[I](cb, b, o, n, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indexed_indirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_indexed_indirect[I](cb, b, o, n, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_count(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                          uint32_t m, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_count[I](cb, b, o, cbuf, co, m, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_draw_indexed_count(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                                  VkDeviceSize co, uint32_t m, uint32_t s) {
    vk_draw(cb, 0, 0, true);
    rv_draw_indexed_count[I](cb, b, o, cbuf, co, m, s);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_copy_image(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                          uint32_t n, const VkImageCopy* r) {
    for (uint32_t k = 0; k < n && r; ++k)
        vk_transfer(cb, dst, r[k].dstOffset.x, r[k].dstOffset.y, r[k].extent.width, r[k].extent.height);
    rv_copy_image[I](cb, src, sl, dst, dl, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_blit_image(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                          uint32_t n, const VkImageBlit* r, VkFilter f) {
    for (uint32_t k = 0; k < n && r; ++k) {
        const VkOffset3D a = r[k].dstOffsets[0], b = r[k].dstOffsets[1];
        vk_transfer(cb, dst, std::min(a.x, b.x), std::min(a.y, b.y), uint32_t(std::abs(b.x - a.x)), uint32_t(std::abs(b.y - a.y)));
    }
    rv_blit_image[I](cb, src, sl, dst, dl, n, r, f);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_copy_buffer(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                                           const VkBufferImageCopy* r) {
    for (uint32_t k = 0; k < n && r; ++k)
        vk_transfer(cb, dst, r[k].imageOffset.x, r[k].imageOffset.y, r[k].imageExtent.width, r[k].imageExtent.height);
    rv_copy_buffer[I](cb, src, dst, dl, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_clear_colour(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c,
                                                            uint32_t n, const VkImageSubresourceRange* r) {
    vk_transfer(cb, img, 0, 0, gv.w.load(), gv.h.load());
    rv_clear_colour[I](cb, img, l, c, n, r);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_resolve(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                       uint32_t n, const VkImageResolve* r) {
    for (uint32_t k = 0; k < n && r; ++k)
        vk_transfer(cb, dst, r[k].dstOffset.x, r[k].dstOffset.y, r[k].extent.width, r[k].extent.height);
    rv_resolve[I](cb, src, sl, dst, dl, n, r);
}

// Attachments cleared inside a pass on a back buffer: a scene write when one rect covers it all, else an overlay (a UI
// panel's background).
template <int I> VKAPI_ATTR void VKAPI_CALL kv_clear_attachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a,
                                                                 uint32_t nr, const VkClearRect* r) {
    if (on_vk()) {
        VkList* s = vlist(cb);
        bool colour = false, whole = false;
        for (uint32_t i = 0; a && i < na; ++i) colour |= (a[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
        for (uint32_t i = 0; r && i < nr; ++i)
            whole |= r[i].rect.offset.x <= 0 && r[i].rect.offset.y <= 0 && r[i].rect.extent.width >= gv.w.load() &&
                     r[i].rect.extent.height >= gv.h.load();
        if (s->back && colour) vk_write(cb, *s, s->back, whole ? kScene : kOverlay);
    }
    rv_clear_attachments[I](cb, na, a, nr, r);
}

// Work this capture does not follow inside a pass on a back buffer: the frame is not trusted.
void vk_unfollowed(VkCommandBuffer cb) {
    if (!on_vk()) return;
    VkList* s = vlist(cb);
    if (s->back) vk_write(cb, *s, s->back, kUnknown);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_execute_commands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* b) {
    for (uint32_t i = 0; i < n; ++i) nr::gpu::inherit(cb, b[i]);
    vk_unfollowed(cb);
    rv_execute_commands[I](cb, n, b);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_next_subpass(VkCommandBuffer cb, VkSubpassContents c) {
    vk_unfollowed(cb);
    rv_next_subpass[I](cb, c);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_next_subpass2(VkCommandBuffer cb, const VkSubpassBeginInfo* b, const VkSubpassEndInfo* e) {
    vk_unfollowed(cb);
    rv_next_subpass2[I](cb, b, e);
}
template <int I> VKAPI_ATTR void VKAPI_CALL kv_next_subpass2_khr(VkCommandBuffer cb, const VkSubpassBeginInfo* b,
                                                                 const VkSubpassEndInfo* e) {
    vk_unfollowed(cb);
    rv_next_subpass2_khr[I](cb, b, e);
}

std::mutex lifetime_mutex;
std::unordered_map<VkCommandBuffer, VkCommandPool> lifetime_pools;
PFN_vkAllocateCommandBuffers rv_allocate_cb[2]{};
PFN_vkFreeCommandBuffers rv_free_cb[2]{};
PFN_vkResetCommandBuffer rv_reset_cb[2]{};
PFN_vkResetCommandPool rv_reset_pool[2]{};
PFN_vkDestroyCommandPool rv_destroy_pool[2]{};
template<int I> VKAPI_ATTR VkResult VKAPI_CALL kv_allocate_cb(VkDevice d, const VkCommandBufferAllocateInfo* a, VkCommandBuffer* cb) {
    const VkResult r = rv_allocate_cb[I](d, a, cb);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> guard(lifetime_mutex);
        for (uint32_t i = 0; i < a->commandBufferCount; ++i) lifetime_pools[cb[i]] = a->commandPool; }
    return r;
}
template<int I> VKAPI_ATTR void VKAPI_CALL kv_free_cb(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer* cb) {
    rv_free_cb[I](d, p, n, cb);
    std::lock_guard<std::mutex> guard(lifetime_mutex);
    for (uint32_t i = 0; i < n; ++i) { nr::gpu::reset(cb[i]); lifetime_pools.erase(cb[i]); }
}
template<int I> VKAPI_ATTR VkResult VKAPI_CALL kv_reset_cb(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    const VkResult r = rv_reset_cb[I](cb, f); if (r == VK_SUCCESS) nr::gpu::reset(cb); return r;
}
void reset_pool_recordings(VkCommandPool pool, bool destroy) {
    std::lock_guard<std::mutex> guard(lifetime_mutex);
    for (auto i = lifetime_pools.begin(); i != lifetime_pools.end();) {
        if (i->second != pool) { ++i; continue; }
        nr::gpu::reset(i->first);
        if (destroy) i = lifetime_pools.erase(i); else ++i;
    }
}
template<int I> VKAPI_ATTR VkResult VKAPI_CALL kv_reset_pool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    const VkResult r = rv_reset_pool[I](d, p, f); if (r == VK_SUCCESS) reset_pool_recordings(p, false); return r;
}
template<int I> VKAPI_ATTR void VKAPI_CALL kv_destroy_pool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks* a) {
    rv_destroy_pool[I](d, p, a); reset_pool_recordings(p, true);
}

// `name` in winevulkan hooked with detours [0] (its export) and [1] (its vkGetDeviceProcAddr's, when another).
bool hook_vk(VkDevice device, const char* name, void* d0, void** r0, void* d1, void** r1) {
    HMODULE m = GetModuleHandleW(L"winevulkan.dll");
    if (!m) return false;
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m, "vkGetDeviceProcAddr")));
    void* exported = reinterpret_cast<void*>(GetProcAddress(m, name));
    void* asked = gdpa && device ? reinterpret_cast<void*>(gdpa(device, name)) : nullptr;
    void* targets[2] = {exported, asked && asked != exported ? asked : nullptr};
    void* detours[2] = {d0, d1};
    void** reals[2] = {r0, r1};
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        if (!targets[i]) continue;
        MH_STATUS st = MH_CreateHook(targets[i], detours[i], reals[i]);
        if (st == MH_OK || (st == MH_ERROR_ALREADY_CREATED && *reals[i])) st = MH_EnableHook(targets[i]);
        if (st == MH_OK || st == MH_ERROR_ENABLED) any = true;
        else log("[nr] HUD FIX capture (Vulkan): %s could not be hooked (%s)", name, MH_StatusToString(st));
    }
    return any;
}
#define NR_VK_HOOK(device, name, k, r) \
    hook_vk(device, name, as(&k<0>), reinterpret_cast<void**>(&r[0]), as(&k<1>), reinterpret_cast<void**>(&r[1]))

}  // namespace

bool vk_lifetime(VkDevice device) {
    bool ok = NR_VK_HOOK(device, "vkBeginCommandBuffer", kv_begin_cb, rv_begin_cb);
    ok = NR_VK_HOOK(device, "vkAllocateCommandBuffers", kv_allocate_cb, rv_allocate_cb) && ok;
    ok = NR_VK_HOOK(device, "vkFreeCommandBuffers", kv_free_cb, rv_free_cb) && ok;
    ok = NR_VK_HOOK(device, "vkResetCommandBuffer", kv_reset_cb, rv_reset_cb) && ok;
    ok = NR_VK_HOOK(device, "vkResetCommandPool", kv_reset_pool, rv_reset_pool) && ok;
    ok = NR_VK_HOOK(device, "vkDestroyCommandPool", kv_destroy_pool, rv_destroy_pool) && ok;
    ok = NR_VK_HOOK(device, "vkCmdExecuteCommands", kv_execute_commands, rv_execute_commands) && ok;
    return ok;
}

void vk_device(VkDevice device, bool translator) {
    if (device) vk_lifetime(device);
    if (translator || !device) return;
    if (gv.created_hooked) {   // another native device: its creations from now on (the hooks are winevulkan's own)
        std::unique_lock<std::shared_mutex> lk(gv.m_obj);
        gv.views.clear(); gv.fbs.clear(); gv.rps.clear(); gv.pipes.clear(); gv.usage.clear(); gv.reordered.clear();
        gv.device.store(device);
        return;
    }
    gv.created_hooked = true;
    gv.device.store(device);
    MH_Initialize();
    bool ok = NR_VK_HOOK(device, "vkCreateImageView", kv_create_view, rv_create_view);
    ok = NR_VK_HOOK(device, "vkCreateImage", kv_create_image, rv_create_image) && ok;
    ok = NR_VK_HOOK(device, "vkCreateFramebuffer", kv_create_fb, rv_create_fb) && ok;
    ok = NR_VK_HOOK(device, "vkCreateRenderPass", kv_create_rp, rv_create_rp) && ok;
    NR_VK_HOOK(device, "vkCreateRenderPass2", kv_create_rp2, rv_create_rp2);
    NR_VK_HOOK(device, "vkCreateRenderPass2KHR", kv_create_rp2_khr, rv_create_rp2_khr);
    ok = NR_VK_HOOK(device, "vkCreateGraphicsPipelines", kv_create_pipes, rv_create_pipes) && ok;
    log("[nr] HUD FIX capture (Vulkan): the device's images, image views, framebuffers, render passes and pipelines are "
        "followed%s", ok ? "" : " (in part: above)");
}

bool vk_image_usage(VkImage image, VkImageUsageFlags* usage) {
    std::shared_lock<std::shared_mutex> lk(gv.m_obj);
    const auto it = gv.usage.find(reinterpret_cast<uint64_t>(image));
    if (it == gv.usage.end()) return false;
    *usage = it->second;
    return true;
}
bool vk_view_reordered(VkImageView view) {
    std::shared_lock<std::shared_mutex> lk(gv.m_obj);
    return gv.reordered.count(reinterpret_cast<uint64_t>(view)) != 0;
}

void vk_submitted(uint32_t n, const VkCommandBuffer* buffers) {
    if (!gv.cmds_hooked || t_own || !buffers) return;
    std::lock_guard<std::mutex> lk(gv.m);
    for (uint32_t i = 0; i < n; ++i) {
        const auto it = gv.lists.find(buffers[i]);
        if (it == gv.lists.end() || it->second.recs.empty()) continue;
        // its records stay until the buffer is begun again: submitted again, its copies run again
        if (gv.frame.size() < 16384) gv.frame.insert(gv.frame.end(), it->second.recs.begin(), it->second.recs.end());
    }
}
void vk_submitted2(uint32_t n, const VkSubmitInfo2* submits) {
    if (!gv.cmds_hooked || t_own || !submits) return;
    std::vector<VkCommandBuffer> cbs;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k) cbs.push_back(submits[i].pCommandBufferInfos[k].commandBuffer);
    vk_submitted(uint32_t(cbs.size()), cbs.data());
}

FrameVk vk_present(VkDevice device, const std::vector<VkImage>& images, VkImage image, VkImage hudless, uint32_t w, uint32_t h,
                   bool armed) {
    FrameVk f;
    const bool was = gv.armed.load();
    if (!armed || !device) {
        if (was) {
            gv.armed.store(false);
            std::lock_guard<std::mutex> lk(gv.m);
            gv.frame.clear();
        }
        return f;
    }
    if (!gv.created_hooked || gv.device.load() != device) {
        f.result = Result::kMissed;
        f.why = "the game's Vulkan device was not seen being made";
        f.broken = true;
        return f;
    }
    if (!gv.cmds_hooked) {
        gv.cmds_hooked = true;
        bool ok = NR_VK_HOOK(device, "vkBeginCommandBuffer", kv_begin_cb, rv_begin_cb);
        ok = NR_VK_HOOK(device, "vkCmdBeginRenderPass", kv_begin_rp, rv_begin_rp) && ok;
        NR_VK_HOOK(device, "vkCmdBeginRenderPass2", kv_begin_rp2, rv_begin_rp2);
        NR_VK_HOOK(device, "vkCmdBeginRenderPass2KHR", kv_begin_rp2_khr, rv_begin_rp2_khr);
        ok = NR_VK_HOOK(device, "vkCmdEndRenderPass", kv_end_rp, rv_end_rp) && ok;
        NR_VK_HOOK(device, "vkCmdEndRenderPass2", kv_end_rp2, rv_end_rp2);
        NR_VK_HOOK(device, "vkCmdEndRenderPass2KHR", kv_end_rp2_khr, rv_end_rp2_khr);
        NR_VK_HOOK(device, "vkCmdBeginRendering", kv_begin_r, rv_begin_r);
        NR_VK_HOOK(device, "vkCmdBeginRenderingKHR", kv_begin_r_khr, rv_begin_r_khr);
        NR_VK_HOOK(device, "vkCmdEndRendering", kv_end_r, rv_end_r);
        NR_VK_HOOK(device, "vkCmdEndRenderingKHR", kv_end_r_khr, rv_end_r_khr);
        ok = NR_VK_HOOK(device, "vkCmdBindPipeline", kv_bind_pipe, rv_bind_pipe) && ok;
        ok = NR_VK_HOOK(device, "vkCmdSetViewport", kv_set_vp, rv_set_vp) && ok;
        ok = NR_VK_HOOK(device, "vkCmdSetScissor", kv_set_sc, rv_set_sc) && ok;
        NR_VK_HOOK(device, "vkCmdSetDepthTestEnable", kv_set_depth, rv_set_depth);
        NR_VK_HOOK(device, "vkCmdSetDepthTestEnableEXT", kv_set_depth_ext, rv_set_depth_ext);
        ok = NR_VK_HOOK(device, "vkCmdDraw", kv_draw, rv_draw) && ok;
        ok = NR_VK_HOOK(device, "vkCmdDrawIndexed", kv_draw_indexed, rv_draw_indexed) && ok;
        NR_VK_HOOK(device, "vkCmdDrawIndirect", kv_draw_indirect, rv_draw_indirect);
        NR_VK_HOOK(device, "vkCmdDrawIndexedIndirect", kv_draw_indexed_indirect, rv_draw_indexed_indirect);
        NR_VK_HOOK(device, "vkCmdDrawIndirectCount", kv_draw_count, rv_draw_count);
        NR_VK_HOOK(device, "vkCmdDrawIndexedIndirectCount", kv_draw_indexed_count, rv_draw_indexed_count);
        NR_VK_HOOK(device, "vkCmdCopyImage", kv_copy_image, rv_copy_image);
        NR_VK_HOOK(device, "vkCmdBlitImage", kv_blit_image, rv_blit_image);
        NR_VK_HOOK(device, "vkCmdCopyBufferToImage", kv_copy_buffer, rv_copy_buffer);
        NR_VK_HOOK(device, "vkCmdClearColorImage", kv_clear_colour, rv_clear_colour);
        NR_VK_HOOK(device, "vkCmdResolveImage", kv_resolve, rv_resolve);
        NR_VK_HOOK(device, "vkCmdExecuteCommands", kv_execute_commands, rv_execute_commands);
        NR_VK_HOOK(device, "vkCmdClearAttachments", kv_clear_attachments, rv_clear_attachments);
        NR_VK_HOOK(device, "vkCmdNextSubpass", kv_next_subpass, rv_next_subpass);
        NR_VK_HOOK(device, "vkCmdNextSubpass2", kv_next_subpass2, rv_next_subpass2);
        NR_VK_HOOK(device, "vkCmdNextSubpass2KHR", kv_next_subpass2_khr, rv_next_subpass2_khr);
        log("[nr] HUD FIX capture (Vulkan): the game's command buffers are followed%s", ok ? "" : " (in part: above)");
    }
    for (size_t i = 0; i < 16; ++i) gv.backs[i].store(i < images.size() ? reinterpret_cast<uint64_t>(images[i]) : 0);
    gv.w.store(w);
    gv.h.store(h);
    gv.hudless.store(reinterpret_cast<uint64_t>(hudless), std::memory_order_release);
    gv.armed.store(true);
    std::vector<Rec> recs;
    {
        std::lock_guard<std::mutex> lk(gv.m);
        recs.swap(gv.frame);
    }
    gv.copies.store(0);
    if (!was) { f.result = Result::kMissed; f.why = "it starts with the next frame"; return f; }
    const nr::hudcap::Verdict v = nr::hudcap::verdict(recs, reinterpret_cast<uint64_t>(image), gv.foreign.exchange(false),
                                                      reinterpret_cast<uint64_t>(hudless), f.why);
    f.result = v == nr::hudcap::Verdict::kCaptured ? Result::kCaptured
               : v == nr::hudcap::Verdict::kClean ? Result::kClean : Result::kMissed;
    if (f.result == Result::kMissed && f.why == "the scene was written again after the copy")
        f.why = "the HUD is drawn in the scene's own render pass (inside which no copy can be made)";
    return f;
}

}  // namespace nr::asi::hudcap
