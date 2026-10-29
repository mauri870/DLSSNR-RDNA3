#include "nr_pe_vkdevice.hpp"

#include "nr_pe_log.hpp"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace nr::pe::vkdevice {
namespace {

using nr::pe::log;

std::mutex lock;
DeviceHandles seen{};

struct Queue { VkDevice device; uint32_t family; VkQueue queue; };
std::vector<Queue> queues;
// The queue families the game asked for in its last vkCreateDevice.
std::vector<uint32_t> requested_families;
void (*device_callback)(VkDevice) = nullptr;

using PFN_CreateInstance = VkResult(VKAPI_PTR*)(const VkInstanceCreateInfo*,
                                                const VkAllocationCallbacks*, VkInstance*);
using PFN_CreateDevice = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                                              const VkAllocationCallbacks*, VkDevice*);
using PFN_GetDeviceQueue = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
// The Vulkan 1.1 form. A game that asks for its queues this way left every one
// of them unrecorded, and the present queue's family unknown - which is exactly
// what stood the fallback down in a real game.
using PFN_GetDeviceQueue2 = void(VKAPI_PTR*)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
using PFN_EnumeratePhysicalDevices = VkResult(VKAPI_PTR*)(VkInstance, uint32_t*, VkPhysicalDevice*);

PFN_CreateInstance real_create_instance = nullptr;
PFN_CreateDevice real_create_device = nullptr;
PFN_GetDeviceQueue real_get_device_queue = nullptr;
PFN_GetDeviceQueue2 real_get_device_queue2 = nullptr;
PFN_EnumeratePhysicalDevices real_enumerate_physical_devices = nullptr;

// What install() hooked, for uninstall().
std::vector<void*> hooked_targets;

VkResult VKAPI_PTR hooked_create_instance(const VkInstanceCreateInfo* info,
                                          const VkAllocationCallbacks* allocator,
                                          VkInstance* out) {
    const VkResult r = real_create_instance(info, allocator, out);
    if (r == VK_SUCCESS && out && *out) {
        std::lock_guard<std::mutex> guard(lock);
        seen.instance = *out;
    }
    return r;
}

// A hook that is installed while the game's vkCreateInstance is still on the
// stack - which is when ReShade's Vulkan layer loads its add-ons - never sees
// that instance. Every app enumerates physical devices right after, and the
// instance is the first argument.
VkResult VKAPI_PTR hooked_enumerate_physical_devices(VkInstance instance, uint32_t* count,
                                                     VkPhysicalDevice* out) {
    const VkResult r = real_enumerate_physical_devices(instance, count, out);
    if (instance) {
        std::lock_guard<std::mutex> guard(lock);
        if (!seen.instance) log("[nr] Vulkan instance learned from vkEnumeratePhysicalDevices");
        seen.instance = instance;
    }
    return r;
}

VkResult VKAPI_PTR hooked_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                        const VkAllocationCallbacks* allocator, VkDevice* out) {
    // The physical device is known before the call; ReShade's Vulkan layer
    // fires its device events from inside this very call, so anything that
    // runs then must already find it.
    {
        std::lock_guard<std::mutex> guard(lock);
        seen.physical = physical;
        requested_families.clear();
        if (info)
            for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
                requested_families.push_back(info->pQueueCreateInfos[i].queueFamilyIndex);
    }
    const VkResult r = real_create_device(physical, info, allocator, out);
    if (r == VK_SUCCESS && out && *out) {
        {
            std::lock_guard<std::mutex> guard(lock);
            seen.physical = physical;
            seen.device = *out;
        }
        log("[nr] Vulkan device created by the game");
        if (device_callback) device_callback(*out);
    }
    return r;
}

void VKAPI_PTR hooked_get_device_queue(VkDevice device, uint32_t family, uint32_t index,
                                       VkQueue* out) {
    real_get_device_queue(device, family, index, out);
    if (!out || !*out) return;
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& q : queues)
        if (q.queue == *out) return;
    queues.push_back({device, family, *out});
}

void VKAPI_PTR hooked_get_device_queue2(VkDevice device, const VkDeviceQueueInfo2* info,
                                        VkQueue* out) {
    real_get_device_queue2(device, info, out);
    if (!out || !*out || !info) return;
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& q : queues)
        if (q.queue == *out) return;
    queues.push_back({device, info->queueFamilyIndex, *out});
}

}  // namespace

bool install() {
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (!vulkan) vulkan = LoadLibraryW(L"vulkan-1.dll");
    if (!vulkan) return false;   // not a Vulkan game; nothing to watch
    struct { const char* name; void* detour; void** original; } entries[] = {
        {"vkCreateInstance", reinterpret_cast<void*>(&hooked_create_instance),
         reinterpret_cast<void**>(&real_create_instance)},
        {"vkCreateDevice", reinterpret_cast<void*>(&hooked_create_device),
         reinterpret_cast<void**>(&real_create_device)},
        {"vkGetDeviceQueue", reinterpret_cast<void*>(&hooked_get_device_queue),
         reinterpret_cast<void**>(&real_get_device_queue)},
        {"vkEnumeratePhysicalDevices", reinterpret_cast<void*>(&hooked_enumerate_physical_devices),
         reinterpret_cast<void**>(&real_enumerate_physical_devices)},
        {"vkGetDeviceQueue2", reinterpret_cast<void*>(&hooked_get_device_queue2),
         reinterpret_cast<void**>(&real_get_device_queue2)},
    };
    bool all = true;
    for (const auto& entry : entries) {
        void* target = reinterpret_cast<void*>(GetProcAddress(vulkan, entry.name));
        if (!target || MH_CreateHook(target, entry.detour, entry.original) != MH_OK) {
            all = false;
            continue;
        }
        hooked_targets.push_back(target);
        if (MH_EnableHook(target) != MH_OK) all = false;
    }
    log("[nr] Vulkan device watch %s", all ? "installed" : "incomplete");
    return all;
}

void uninstall() {
    if (hooked_targets.empty()) return;
    for (void* target : hooked_targets) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
    hooked_targets.clear();
    log("[nr] Vulkan device watch removed");
}

DeviceHandles handles() {
    std::lock_guard<std::mutex> guard(lock);
    return seen;
}

void note_handles(VkInstance instance, VkPhysicalDevice physical, VkDevice device) {
    std::lock_guard<std::mutex> guard(lock);
    if (instance) seen.instance = instance;
    if (physical) seen.physical = physical;
    if (device) seen.device = device;
}

void note_device(VkInstance instance, VkPhysicalDevice physical, VkDevice device, const std::vector<uint32_t>& families) {
    std::lock_guard<std::mutex> guard(lock);
    if (instance) seen.instance = instance;
    seen.physical = physical;
    seen.device = device;
    requested_families = families;
}

void forget_device(VkDevice device) {
    if (!device) return;
    std::lock_guard<std::mutex> guard(lock);
    queues.erase(std::remove_if(queues.begin(), queues.end(), [device](const Queue& q) { return q.device == device; }),
                 queues.end());
    if (seen.device == device) { seen.device = VK_NULL_HANDLE; requested_families.clear(); }
}

namespace {
std::atomic<bool> g_queue_locks{false};
std::mutex g_queue_locks_map;
std::map<VkQueue, std::unique_ptr<std::recursive_mutex>> g_queue_mutexes;
std::recursive_mutex* queue_mutex(VkQueue queue) {
    std::lock_guard<std::mutex> guard(g_queue_locks_map);
    auto& m = g_queue_mutexes[queue];
    if (!m) m = std::make_unique<std::recursive_mutex>();
    return m.get();
}
}  // namespace

void enable_queue_locks() { g_queue_locks = true; }
bool queue_locks_enabled() { return g_queue_locks.load(); }
void lock_queue(VkQueue queue) { if (g_queue_locks.load() && queue) queue_mutex(queue)->lock(); }
void unlock_queue(VkQueue queue) { if (g_queue_locks.load() && queue) queue_mutex(queue)->unlock(); }
bool lock_queue_held(VkQueue queue) {
    if (!g_queue_locks.load() || !queue) return false;
    queue_mutex(queue)->lock();
    return true;
}
void unlock_queue_held(VkQueue queue, bool held) { if (held) queue_mutex(queue)->unlock(); }

bool family_of(VkQueue queue, uint32_t* family) {
    std::lock_guard<std::mutex> guard(lock);
    if (!queue || !family) return false;
    for (const auto& q : queues)
        if (q.queue == queue) { *family = q.family; return true; }
    return false;
}

void set_device_callback(void (*callback)(VkDevice)) {
    std::lock_guard<std::mutex> guard(lock);
    device_callback = callback;
}

bool queue_for(VkDevice device, VkQueue* out, uint32_t* family) {
    if (!device || !out || !family) return false;
    VkPhysicalDevice physical{};
    std::vector<uint32_t> families;
    {
        std::lock_guard<std::mutex> guard(lock);
        physical = seen.physical;
        for (const auto& q : queues) if (q.device == device) families.push_back(q.family);
        if (!physical) return false;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props.data());
    auto usable = [&](uint32_t f) {
        return f < count && (props[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && (props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    };
    {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& q : queues)
            if (q.device == device && usable(q.family)) { *out = q.queue; *family = q.family; return true; }
        families = requested_families;
    }
    for (uint32_t f : families) {
        if (!usable(f)) continue;
        VkQueue q = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, f, 0, &q);   // through the export, so the hook records it
        if (q) { *out = q; *family = f; return true; }
    }
    return false;
}

bool queue(VkQueue* out, uint32_t* family) {
    std::lock_guard<std::mutex> guard(lock);
    if (!out || !family || !seen.device || !seen.physical) return false;
    // The network computes and transfers, and the colour conversion path also
    // needs graphics, so the family has to carry all of it. Asking the physical
    // device rather than assuming family 0 is the difference between working on
    // one driver and working on the machine in front of us.
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(seen.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(seen.physical, &count, props.data());
    for (const auto& q : queues) {
        if (q.device != seen.device || q.family >= count) continue;
        const VkQueueFlags flags = props[q.family].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) && (flags & VK_QUEUE_GRAPHICS_BIT)) {
            *out = q.queue; *family = q.family;
            return true;
        }
    }
    return false;
}

}  // namespace nr::pe::vkdevice
