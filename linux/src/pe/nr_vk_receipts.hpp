#pragma once
// Fence generations outlive their handles. A game may reset its fence before a retired object next polls it.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <memory>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nr::pe::receipts {
struct Receipt {
    VkDevice device{};
    VkFence fence{};
    bool owned = false, complete = false;
};
using Token = std::shared_ptr<Receipt>;
inline std::recursive_mutex mutex;
inline std::vector<Token> pending;
inline std::unordered_map<VkDevice, std::vector<VkFence>> pool;
inline std::map<std::pair<VkDevice, VkFence>, std::weak_ptr<Receipt>> borrowed;
struct Wait {
    VkDevice device{};
    Token last;
    bool unused = true, retired = false;
};
inline std::map<std::pair<VkDevice, VkSemaphore>, Wait> waits;

inline bool done(const Token& r) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (!r) return false;
    if (r->complete) return true;
    if (!r->fence || vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, 0) != VK_SUCCESS) return false;
    if (r->owned) pool[r->device].push_back(r->fence);
    else borrowed.erase({r->device, r->fence});
    r->fence = VK_NULL_HANDLE;
    r->complete = true;
    return true;
}
inline void collect() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    pending.erase(std::remove_if(pending.begin(), pending.end(), [](const Token& r) {
        return done(r) || !r->fence;
    }), pending.end());
    for (auto i = waits.begin(); i != waits.end();) {
        if (i->second.retired && (i->second.unused || done(i->second.last))) {
            vkDestroySemaphore(i->second.device, i->first.second, nullptr);
            i = waits.erase(i);
        } else ++i;
    }
}
inline Token acquire(VkDevice device, VkFence game = VK_NULL_HANDLE) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    collect();
    auto r = std::make_shared<Receipt>();
    r->device = device;
    r->fence = game;
    r->owned = !game;
    if (!game) {
        auto& free = pool[device];
        if (!free.empty()) {
            r->fence = free.back(); free.pop_back();
            if (vkResetFences(device, 1, &r->fence) != VK_SUCCESS) {
                vkDestroyFence(device, r->fence, nullptr); r->fence = VK_NULL_HANDLE;
            }
        }
        if (!r->fence) {
            VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateFence(device, &ci, nullptr, &r->fence) != VK_SUCCESS) return {};
        }
    }
    return r;
}
inline void submitted(const Token& r) {
    if (!r) return;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    pending.push_back(r);
    if (!r->owned) borrowed[{r->device, r->fence}] = r;
}
inline void cancel(const Token& r) {
    if (!r || !r->owned || !r->fence) return;
    std::lock_guard<std::recursive_mutex> guard(mutex);
    pool[r->device].push_back(r->fence); r->fence = VK_NULL_HANDLE;
}
// Called before every covered ResetFences / DestroyFence. An unproved generation stays incomplete forever.
inline void invalidate(VkDevice device, VkFence fence) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    const auto i = borrowed.find({device, fence});
    if (i == borrowed.end()) return;
    const Token r = i->second.lock();
    if (r && r->device == device) {
        if (!done(r)) { r->fence = VK_NULL_HANDLE; borrowed.erase({device, fence}); }
    } else borrowed.erase(i);
}
inline void watch(VkDevice device, VkSemaphore sem) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    waits.emplace(std::make_pair(device, sem), Wait{device});
}
inline bool watched(VkDevice device, uint32_t count, const VkSemaphore* sems) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (uint32_t i = 0; i < count; ++i) if (waits.count({device, sems[i]})) return true;
    return false;
}
inline void used(VkDevice device, VkSemaphore sem, const Token& r) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (const auto i = waits.find({device, sem}); i != waits.end()) { i->second.last = r; i->second.unused = false; }
}
inline void retire(VkDevice device, VkSemaphore sem) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    const auto i = waits.find({device, sem});
    if (i != waits.end() && i->second.device == device) i->second.retired = true;
    collect();
}
inline void end(VkDevice device) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    for (auto i = pending.begin(); i != pending.end();) {
        const Token r = *i;
        if (r->device != device) { ++i; continue; }
        if (r->owned && r->fence) vkDestroyFence(device, r->fence, nullptr);
        else if (r->fence) borrowed.erase({r->device, r->fence});
        r->fence = VK_NULL_HANDLE; r->complete = true;
        i = pending.erase(i);
    }
    if (const auto i = pool.find(device); i != pool.end()) {
        for (auto f : i->second) vkDestroyFence(device, f, nullptr);
        pool.erase(i);
    }
    for (auto i = waits.begin(); i != waits.end();) {
        if (i->second.device == device) { vkDestroySemaphore(device, i->first.second, nullptr); i = waits.erase(i); }
        else ++i;
    }
}
} // namespace nr::pe::receipts
