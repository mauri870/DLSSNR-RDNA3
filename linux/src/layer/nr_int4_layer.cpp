// The int4 mixed network's Linux Vulkan layer (installed only with int4 mixed): adds VK_KHR_pipeline_binary and
// VK_KHR_maintenance5 (with their features) to the game's device when the driver offers them - the int4 pipelines
// rewrite their own binaries (nrvk.hpp create_iu4). It exists because on the OptiScaler route our PE runtime is loaded
// after the game (vkd3d/DXVK under Proton) has created its device; this layer sits under winevulkan and is in place
// from the start, and winevulkan asks the host device which functions exist, so the extensions reach the Windows side
// too. Lives in <game>/dlssnr-amd/int4/layer and is enabled from the launch options (VK_ADD_LAYER_PATH +
// VK_INSTANCE_LAYERS): nothing is installed outside the game.
// It only adds them when the int4 network may be used in this run: DLSSNR_INT4=1, or no DLSSNR_INT4 and, in the game's
// dlssnr-amd.ini, [Int4Mixed] Enabled = 1 or a Hotkey to switch to it in game; otherwise every device is created
// exactly as asked and every call passes straight to the next layer.
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include "nr_ini_int4.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "nr_device_features.hpp"

namespace {
std::mutex lock;
struct Inst { VkInstance handle; PFN_vkGetInstanceProcAddr gipa; };
std::unordered_map<void*, Inst> instances;              // loader dispatch key -> instance
std::unordered_map<void*, PFN_vkGetDeviceProcAddr> devices;
void* key(const void* h) { return *reinterpret_cast<void* const*>(h); }

template <class T> T* find_link(const void* chain, VkStructureType type, VkLayerFunction fn) {
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext)
        if (p->sType == type && reinterpret_cast<const T*>(p)->function == fn) return const_cast<T*>(reinterpret_cast<const T*>(p));
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    auto* link = find_link<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(ci, a, out);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> g(lock); instances[key(*out)] = {*out, gipa}; }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance i, const VkAllocationCallbacks* a) {
    PFN_vkGetInstanceProcAddr gipa;
    { std::lock_guard<std::mutex> g(lock); auto it = instances.find(key(i)); if (it == instances.end()) return; gipa = it->second.gipa; instances.erase(it); }
    reinterpret_cast<PFN_vkDestroyInstance>(gipa(i, "vkDestroyInstance"))(i, a);
}

bool int4_wanted() {
    if (const char* e = std::getenv("DLSSNR_INT4")) return std::string(e) == "1";
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&int4_wanted), &info) || !info.dli_fname) return false;
    // <game>/dlssnr-amd/int4/layer/libVkLayer_dlssnr_int4.so
    const auto game = std::filesystem::path(info.dli_fname).parent_path().parent_path().parent_path().parent_path();
    const nr::Int4Ini ini = nr::ini_int4((game / "dlssnr-amd.ini").string());
    return ini.enabled != 0 || !ini.hotkey.empty();   // no Enabled: on (nr_ini_int4.hpp)
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    auto* link = find_link<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkInstance inst = VK_NULL_HANDLE;
    { std::lock_guard<std::mutex> g(lock); auto it = instances.find(key(phys)); if (it != instances.end()) inst = it->second.handle; }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
    VkDeviceCreateInfo patched = *ci; std::unique_ptr<nr::DeviceFeatures> features; std::vector<const char*> ext;
    if (int4_wanted()) try {
        auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(inst, "vkEnumerateDeviceExtensionProperties"));
        auto feats2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(inst, "vkGetPhysicalDeviceFeatures2"));
        uint32_t n = 0; enumerate(phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n); enumerate(phys, nullptr, &n, have.data());
        auto offered = [&](const char* e) { for (auto& h : have) if (!std::strcmp(h.extensionName, e)) return true; return false; };
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &m5};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pb};
        if (offered(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) && offered(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
            feats2(phys, &f2);
            if (pb.pipelineBinaries && m5.maintenance5) {
                features = std::make_unique<nr::DeviceFeatures>(ci->pNext);
                // the loader's own link structures stay in the copied chain: the next layer/driver finds them there
                features->get<VkPhysicalDevicePipelineBinaryFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR)->pipelineBinaries = VK_TRUE;
                if (auto* v14 = features->find<VkPhysicalDeviceVulkan14Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES))
                    v14->maintenance5 = VK_TRUE;
                else
                    features->get<VkPhysicalDeviceMaintenance5FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR)->maintenance5 = VK_TRUE;
                for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) ext.push_back(ci->ppEnabledExtensionNames[i]);
                for (const char* e : {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME}) {
                    bool on = false; for (auto x : ext) on |= !std::strcmp(x, e);
                    if (!on) ext.push_back(e);
                }
                patched.pNext = features->head; patched.enabledExtensionCount = uint32_t(ext.size()); patched.ppEnabledExtensionNames = ext.data();
            }
        }
    } catch (...) { patched = *ci; }
    // One downstream create: the loader chain was consumed above.
    VkResult r = create(phys, &patched, a, out);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> g(lock); devices[key(*out)] = gdpa; }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice d, const VkAllocationCallbacks* a) {
    PFN_vkGetDeviceProcAddr gdpa;
    { std::lock_guard<std::mutex> g(lock); auto it = devices.find(key(d)); if (it == devices.end()) return; gdpa = it->second; devices.erase(it); }
    reinterpret_cast<PFN_vkDestroyDevice>(gdpa(d, "vkDestroyDevice"))(d, a);
}
}  // namespace

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_int4_GetDeviceProcAddr(VkDevice d, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_int4_GetInstanceProcAddr(VkInstance i, const char* name) {
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&nr_int4_GetInstanceProcAddr);
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&nr_int4_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
    if (!std::strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice);
    if (!i) return nullptr;
    PFN_vkGetInstanceProcAddr gipa;
    { std::lock_guard<std::mutex> g(lock); auto it = instances.find(key(i)); if (it == instances.end()) return nullptr; gipa = it->second.gipa; }
    return gipa(i, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nr_int4_GetDeviceProcAddr(VkDevice d, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&nr_int4_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice);
    PFN_vkGetDeviceProcAddr gdpa;
    { std::lock_guard<std::mutex> g(lock); auto it = devices.find(key(d)); if (it == devices.end()) return nullptr; gdpa = it->second; }
    return gdpa(d, name);
}
}
