#include "nr_vram_ledger.hpp"
#include "nr_ini_network.hpp"

#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d10.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

// The generated entry-point slots (linux/build/build_optiscaler_nr.sh, build_package.sh): a tail jump through each.
extern "C" void* nr_vk_p_vkAllocateMemory;
extern "C" void* nr_vk_p_vkFreeMemory;

namespace nr::vram {
namespace {
struct Entry {
    uint64_t bytes;
    uint32_t type;   // Vulkan memory type; ~0u: a D3D12 default-heap resource
    Tag tag;
};
std::mutex g_lock;
std::unordered_map<uint64_t, Entry> g_vk;    // by VkDeviceMemory
std::unordered_map<uint64_t, Entry> g_d3d;   // by our own key
std::unordered_map<uint64_t, Entry> g_ext;   // add_external's, by the caller's key
uint64_t g_next_key = 1;
uint32_t g_device_local = ~0u;               // memory types (bits) that are device-local; all until known
PFN_vkAllocateMemory g_real_allocate = nullptr;
PFN_vkFreeMemory g_real_free = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                        const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    const VkResult r = g_real_allocate(device, info, callbacks, memory);
    if (r == VK_SUCCESS && info && memory && *memory) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_vk[uint64_t(*memory)] = Entry{info->allocationSize, info->memoryTypeIndex, t_tag};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL release(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
    if (memory) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_vk.erase(uint64_t(memory));
    }
    g_real_free(device, memory, callbacks);
}

// IUnknown's id (not from uuid.lib, which not every link of this file takes)
constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// Held by the resource as private data: D3D12 releases it when the resource is destroyed, and the entry goes.
struct Tracker final : IUnknown {
    std::atomic<ULONG> refs{1};
    uint64_t key = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(id, kIUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
        if (!n) {
            {
                std::lock_guard<std::mutex> guard(g_lock);
                g_d3d.erase(key);
            }
            delete this;
        }
        return n;
    }
};
// {6E8C2F4A-3B1D-4F57-9A0E-5C2D7B81A944}
constexpr GUID kTrackerGuid = {0x6e8c2f4a, 0x3b1d, 0x4f57, {0x9a, 0x0e, 0x5c, 0x2d, 0x7b, 0x81, 0xa9, 0x44}};

int owner_index(int owner) { return owner >= 0 && owner < kNetworkChoices ? owner : kOwners - 1; }
}  // namespace

void install_vulkan() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (nr_vk_p_vkAllocateMemory != reinterpret_cast<void*>(&allocate)) {
        g_real_allocate = reinterpret_cast<PFN_vkAllocateMemory>(nr_vk_p_vkAllocateMemory);
        nr_vk_p_vkAllocateMemory = reinterpret_cast<void*>(&allocate);
    }
    if (nr_vk_p_vkFreeMemory != reinterpret_cast<void*>(&release)) {
        g_real_free = reinterpret_cast<PFN_vkFreeMemory>(nr_vk_p_vkFreeMemory);
        nr_vk_p_vkFreeMemory = reinterpret_cast<void*>(&release);
    }
}

void memory_types(const VkPhysicalDeviceMemoryProperties& props) {
    uint32_t bits = 0;
    for (uint32_t i = 0; i < props.memoryTypeCount && i < 32; ++i)
        if (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) bits |= 1u << i;
    std::lock_guard<std::mutex> guard(g_lock);
    g_device_local = bits;
}

void track_d3d12(ID3D12Device* device, ID3D12Resource* resource, Kind kind) {
    if (!device || !resource) return;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(resource->GetHeapProperties(&heap, &flags)) || heap.Type != D3D12_HEAP_TYPE_DEFAULT) return;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const uint64_t bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    if (!bytes || bytes == UINT64_MAX) return;
    auto* tracker = new Tracker;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        tracker->key = g_next_key++;
        Tag tag = t_tag;
        tag.kind = kind;
        g_d3d[tracker->key] = Entry{bytes, ~0u, tag};
    }
    if (FAILED(resource->SetPrivateDataInterface(kTrackerGuid, tracker))) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_d3d.erase(tracker->key);
    }
    tracker->Release();   // the resource holds it now (or nobody: then it is gone)
}

namespace {
// The bytes a texel of a DXGI format holds (0: not known here).
uint32_t texel_bytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS: return 8;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_TYPELESS: return 4;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R8G8_UNORM: return 2;
        case DXGI_FORMAT_R8_UNORM: return 1;
        default: return 0;
    }
}
// `bytes` under `kind` (the thread's network) while `child` lives (its private data holds the tracker).
template <class Child> void track_child(Child* child, uint64_t bytes, Kind kind) {
    if (!child || !bytes) return;
    auto* tracker = new Tracker;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        tracker->key = g_next_key++;
        Tag tag = t_tag;
        tag.kind = kind;
        g_d3d[tracker->key] = Entry{bytes, ~0u, tag};
    }
    if (FAILED(child->SetPrivateDataInterface(kTrackerGuid, tracker))) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_d3d.erase(tracker->key);
    }
    tracker->Release();
}
}  // namespace

void track_d3d11(ID3D11Resource* resource, Kind kind) {
    if (!resource) return;
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    uint64_t bytes = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            t->Release();
            if (d.Usage != D3D11_USAGE_DEFAULT) return;
            bytes = uint64_t(d.Width) * d.Height * d.ArraySize * texel_bytes(d.Format);
        }
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        ID3D11Buffer* b = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&b))) && b) {
            D3D11_BUFFER_DESC d{};
            b->GetDesc(&d);
            b->Release();
            if (d.Usage != D3D11_USAGE_DEFAULT) return;
            bytes = d.ByteWidth;
        }
    }
    track_child(resource, bytes, kind);
}

void track_d3d10(ID3D10Resource* resource, Kind kind) {
    if (!resource) return;
    D3D10_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    uint64_t bytes = 0;
    if (dim == D3D10_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D10Texture2D* t = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&t))) && t) {
            D3D10_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            t->Release();
            if (d.Usage != D3D10_USAGE_DEFAULT) return;
            bytes = uint64_t(d.Width) * d.Height * d.ArraySize * texel_bytes(d.Format);
        }
    } else if (dim == D3D10_RESOURCE_DIMENSION_BUFFER) {
        ID3D10Buffer* b = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D10Buffer), reinterpret_cast<void**>(&b))) && b) {
            D3D10_BUFFER_DESC d{};
            b->GetDesc(&d);
            b->Release();
            if (d.Usage != D3D10_USAGE_DEFAULT) return;
            bytes = d.ByteWidth;
        }
    }
    track_child(resource, bytes, kind);
}

void add_external(uint64_t key, uint64_t bytes, Kind kind) {
    if (!key || !bytes) return;
    std::lock_guard<std::mutex> guard(g_lock);
    Tag tag = t_tag;
    tag.kind = kind;
    g_ext[key] = Entry{bytes, ~0u, tag};
}

void forget_external(uint64_t key) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_ext.erase(key);
}

void totals(uint64_t out[kOwners][kKinds]) {
    std::memset(out, 0, sizeof(uint64_t) * kOwners * kKinds);
    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto& [memory, e] : g_vk)
        if (e.type < 32 && (g_device_local >> e.type) & 1u) out[owner_index(e.tag.owner)][e.tag.kind] += e.bytes;
    for (const auto& [key, e] : g_d3d) out[owner_index(e.tag.owner)][e.tag.kind] += e.bytes;
    for (const auto& [key, e] : g_ext) out[owner_index(e.tag.owner)][e.tag.kind] += e.bytes;
}
}  // namespace nr::vram
