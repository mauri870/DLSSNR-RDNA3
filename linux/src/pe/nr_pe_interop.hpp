#pragma once
// Getting at the Vulkan objects underneath a D3D12 (or D3D11) game on Proton.
//
// A D3D12 game under Proton is already a Vulkan program: vkd3d-proton records
// its command lists into VkCommandBuffers and its resources are VkImages. Both
// are reachable through vkd3d-proton's own interop interfaces, so the network -
// which is SPIR-V - records straight into the game's command stream, in order,
// at the point the upscaler was about to run. No private device, no resource
// copy, no queue-boundary contortion.
//
// These declarations are transcribed from vkd3d-proton's IDL
// (include/vkd3d_command_list_vkd3d_ext.idl, and the device interop interface
// as OptiScaler declares it in misc/IdentifyGpu.h). They are ABI, not guesses:
// the UUIDs are the ones vkd3d-proton publishes.
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>
#include <functional>
#include <string>

// vkd3d-proton: the command list's Vulkan handle.
MIDL_INTERFACE("77a86b09-2bea-4801-b89a-37648e104af1")
ID3D12GraphicsCommandListExt : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandle(VkCommandBuffer * pVkCommandBuffer) = 0;
    // LaunchCubinShader follows in the real interface; we never call it, but the
    // vtable slot has to be accounted for if anything is ever added above it.
    virtual HRESULT STDMETHODCALLTYPE LaunchCubinShader(void* handle, UINT32 x, UINT32 y, UINT32 z,
                                                        const void* params, UINT32 param_size) = 0;
};

// vkd3d-proton: device, queue and resource handles.
MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
ID3D12DXVKInteropDevice : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** ppvObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT* pExtensionCount, const char** ppExtensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT* pExtensionCount, const char** ppExtensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** ppFeatures) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pVkInstance, VkPhysicalDevice* pVkPhysicalDevice,
                                                       VkDevice* pVkDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue* pCommandQueue, VkQueue* pVkQueue,
                                                         UINT32* pVkQueueFamily) = 0;
    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource* pResource, D3D12_RESOURCE_STATES State,
                                                        VkImageLayout* pVkLayout) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource* pResource, UINT64* pVkHandle,
                                                            UINT64* pBufferOffset) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue* pCommandQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue* pCommandQueue) = 0;
};

// vkd3d-proton 2.13 and later (include/vkd3d_device_vkd3d_ext.idl): the same
// resource query, plus the VkFormat vkd3d created the image with
// (libs/vkd3d/device_vkd3d_ext.c: resource->format->vk_format). Only the first
// method is called; the rest are listed so the vtable is the real one.
MIDL_INTERFACE("902d8115-59eb-4406-9518-fe00f991ee65")
ID3D12DXVKInteropDevice1 : public ID3D12DXVKInteropDevice
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo1(ID3D12Resource* pResource, UINT64* pVkHandle,
                                                             UINT64* pBufferOffset, VkFormat* pFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc,
                                                                UINT32 vk_queue_family_index,
                                                                ID3D12CommandQueue** ppQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                                    UINT32 vk_queue_family_index,
                                                                    ID3D12CommandAllocator** ppAllocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginVkCommandBufferInterop(ID3D12CommandList* pCmdList,
                                                                  VkCommandBuffer* pCommandBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndVkCommandBufferInterop(ID3D12CommandList* pCmdList) = 0;
};

// DXVK, for D3D11 games. Transcribed from dxvk/src/dxgi/dxgi_interfaces.h, not
// from memory: a wrong vtable order here is a crash inside someone's game.
//
// D3D11 has no command list to record into - its immediate context is not a
// recordable object - so this path cannot do what the D3D12 one does. What DXVK
// offers instead is the documented interop dance: flush its pending work, take
// the submission queue lock, submit our own command buffer on the same queue,
// release. Ordering holds because the flush puts everything the game has
// recorded so far in front of us.
struct IDXGIVkInteropDevice;

MIDL_INTERFACE("5546cf8c-77e7-4341-b05d-8d4d5000e77d")
IDXGIVkInteropSurface : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDevice(IDXGIVkInteropDevice** ppDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout,
                                                          VkImageCreateInfo* pInfo) = 0;
};

MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
IDXGIVkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev,
                                                    VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(IDXGIVkInteropSurface* pSurface,
                                                            const VkImageSubresourceRange* pSubresources,
                                                            VkImageLayout OldLayout,
                                                            VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

// mingw resolves __uuidof through a template specialisation rather than
// __declspec(uuid), so the GUIDs have to be declared for it explicitly. These
// are the same values as the MIDL_INTERFACE attributes above; if one is edited
// the other must be too, which is why they sit together.
#ifdef __MINGW32__
__CRT_UUID_DECL(ID3D12GraphicsCommandListExt, 0x77a86b09, 0x2bea, 0x4801,
                0xb8, 0x9a, 0x37, 0x64, 0x8e, 0x10, 0x4a, 0xf1)
__CRT_UUID_DECL(ID3D12DXVKInteropDevice, 0x39da4e09, 0xbd1c, 0x4198,
                0x9f, 0xae, 0x86, 0xbb, 0xe3, 0xbe, 0x41, 0xfd)
__CRT_UUID_DECL(ID3D12DXVKInteropDevice1, 0x902d8115, 0x59eb, 0x4406,
                0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65)
__CRT_UUID_DECL(IDXGIVkInteropSurface, 0x5546cf8c, 0x77e7, 0x4341,
                0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d)
__CRT_UUID_DECL(IDXGIVkInteropDevice, 0xe2ef5fa5, 0xdc21, 0x4af7,
                0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23)
#endif

namespace nr::pe {

// Everything the network needs about the game's Vulkan device, resolved once.
struct DeviceHandles {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    bool valid() const { return instance && physical && device; }
};

// The DXGI -> Vulkan format table this module accepts. ReShade numbers its
// formats exactly like DXGI, so its Vulkan backend's descriptions go through
// here too.
VkFormat vulkan_format_of(DXGI_FORMAT format);
// The same for a colour image (the frame, an output, a back buffer), which also
// takes the rest of DXGI's RGB colour formats - see colour_format_fallback() in
// the definition. Motion and depth keep vulkan_format_of.
VkFormat vulkan_colour_format_of(DXGI_FORMAT format);
// NR_FORMAT_FALLBACK=0 turns the rest-of-DXGI colour formats off everywhere, so a
// frame in one of them is declined as it was before they were taken.
bool format_fallback_enabled();

// Resolve the game's Vulkan device from its D3D12 device. Returns an invalid
// set when the runtime underneath is not vkd3d-proton - which is what happens
// on Windows, where this whole path does not apply.
DeviceHandles device_handles(ID3D12Device* device);

// The VkCommandBuffer the game's command list is recording into.
VkCommandBuffer command_buffer(ID3D12GraphicsCommandList* list);

// The same command buffer for raw Vulkan commands with the list's D3D12 state kept: vkd3d-proton's
// BeginVkCommandBufferInterop (ID3D12DXVKInteropDevice1), which forgets what vkd3d bound in the command buffer
// and re-emits the list's own pipeline, root signature, root arguments and descriptor heaps at its next draw or
// dispatch. Null where this vkd3d-proton has no ID3D12DXVKInteropDevice1. end_interop_recording after the
// commands.
VkCommandBuffer begin_interop_recording(ID3D12Device* device, ID3D12GraphicsCommandList* list);
void end_interop_recording(ID3D12Device* device, ID3D12GraphicsCommandList* list);

// The VkImage behind a D3D12 texture, and the layout vkd3d believes it is in
// for a given D3D12 resource state. The layout matters: we barrier from it and
// must put it back, or the game's next use sees the wrong layout.
struct ResourceHandle {
    VkImage image{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t width{}, height{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    // What the image may be used for. Only DXVK tells us (its create info comes
    // back with the handle); vkd3d does not, so this is zero there and callers
    // must not read it as "no usage". Checked before the network is pointed at a
    // game's own texture: the pass transfers in and out of it, and an image
    // without those bits is a validation error, not a picture.
    VkImageUsageFlags usage{};
    // The D3D12 format the resource was created with, kept so a rejection can
    // say which format it was. Zero (UNKNOWN) on the DXVK path.
    DXGI_FORMAT dxgi{DXGI_FORMAT_UNKNOWN};
    // Set by colour_handle when the format is one of the rest-of-DXGI colour
    // formats but this GPU cannot blit it: the VkFormat, for the rejection.
    VkFormat unblittable{VK_FORMAT_UNDEFINED};
    // The image exists. Says nothing about `format`: a resource in a format the
    // pass does not handle still comes back with its VkImage and
    // format == VK_FORMAT_UNDEFINED, so the caller can log what it was.
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
    bool usable() const { return image != VK_NULL_HANDLE && format != VK_FORMAT_UNDEFINED; }
};
ResourceHandle resource_handle(ID3D12Device* device, ID3D12Resource* resource,
                               D3D12_RESOURCE_STATES state);
// resource_handle for a colour image. Identical whenever resource_handle already
// knows the format; only a format it declines is looked at again (see the
// definition).
ResourceHandle colour_handle(ID3D12Device* device, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES state);
// Why a handle cannot be used, with the DXGI format number in it. `what` names
// the resource ("colour", "output", "back buffer") for the log line.
std::string describe_rejection(const ResourceHandle& handle, const char* what);
// True when the D3D12 resource was created typeless and `format` is the
// family's representative rather than the game's own view.
bool resource_is_typeless(const ResourceHandle& handle);

// The D3D11 equivalents, through DXVK. `interop` is borrowed, not owned: the
// caller keeps it for the flush/lock/submit sequence and releases it after.
DeviceHandles d3d11_handles(IUnknown* device, IDXGIVkInteropDevice** interop);
// `device` is the game's real D3D11 device (not a proxy): the call has to know
// it to see through ReShade's GetDevice hook, see the definition.
ResourceHandle d3d11_image(IUnknown* texture, IUnknown* device);

// A VkQueue the network can submit on, and the lock that makes submitting on it
// safe.
//
// The network is not only a recorder: building it uploads the weights, which is
// a real submit on a real queue. Both runtimes share one VkQueue between several
// API-level queues - vkd3d-proton's LockCommandQueue takes the underlying
// vkd3d_queue's mutex (libs/vkd3d/device_vkd3d_ext.c, read, not assumed) and
// DXVK's LockSubmissionQueue does the same - so the lock is not optional and
// there is no "our own private queue" to escape to.
// The Vulkan entry points (nr_vk_load, once) and the VRAM ledger's wrap of our allocations, for a caller that has no
// D3D object to hand over (the .asi on a Vulkan game).
void vulkan_entry_points();

struct QueueAccess {
    VkQueue queue{};
    uint32_t family{};
    std::function<void()> lock, unlock;
    // **The same lock without the flush, and it is not a micro-optimisation.**
    //
    // On D3D11 `lock` is `FlushRenderingCommands()` followed by
    // `LockSubmissionQueue()`. The first of those drives DXVK's *immediate
    // context*, which is not thread-safe and belongs to the game's render
    // thread; the second is a mutex over the submission queue and is meant to be
    // taken from anywhere - that is what it exists for. Calling the pair from
    // the background build thread is therefore a data race against whatever the
    // game is doing in D3D11 at that instant, and it crashed Tomb Raider 2013
    // three times out of three at `0xC0000005` inside `d3d11.dll`, always in the
    // same instruction, always within a frame of "building the network in the
    // background" (logs/dlssnr-amd.log, 2026-09-18).
    //
    // Nothing of the game's has to be ordered in front of a weight upload, so
    // the build thread takes this one and never touches the immediate context.
    // On D3D12 there is no flush in either, and both are the same call.
    std::function<void()> lock_quiet, unlock_quiet;
    bool valid() const { return queue != VK_NULL_HANDLE; }
    // RAII around whichever of the two locks applies.
    struct Held {
        const QueueAccess* access;
        explicit Held(const QueueAccess& a) : access(&a) { if (a.lock) a.lock(); }
        ~Held() { if (access->unlock) access->unlock(); }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
    };
    // For any thread that is not the game's render thread.
    struct HeldQuiet {
        const QueueAccess* access;
        explicit HeldQuiet(const QueueAccess& a) : access(&a) { if (a.lock_quiet) a.lock_quiet(); }
        ~HeldQuiet() { if (access->unlock_quiet) access->unlock_quiet(); }
        HeldQuiet(const HeldQuiet&) = delete;
        HeldQuiet& operator=(const HeldQuiet&) = delete;
    };
};

// Resolve a submittable queue from the game's D3D12 device. This creates one
// ID3D12CommandQueue of our own - vkd3d hands out a VkQueue per API queue but
// maps them onto the same shared vkd3d_queue, so ours locks against the game's -
// and hands it back through `owned` for the caller to release at shutdown.
bool d3d12_queue_access(ID3D12Device* device, QueueAccess* out, ID3D12CommandQueue** owned);

// The same, from DXVK's interop device. `interop` must outlive the access: the
// lock closures call into it.
bool d3d11_queue_access(IDXGIVkInteropDevice* interop, QueueAccess* out);

}  // namespace nr::pe
