#pragma once
// NR's own video memory as it is allocated and freed, for NR Control's VRAM breakdown: every Vulkan allocation of ours
// (our vkAllocateMemory / vkFreeMemory entry points wrapped) and the D3D12 resources the .asi makes (tagged at creation,
// counted out when D3D12 destroys them), each under the tag of the thread that made it (nr_vram_tag.hpp). The game's
// own memory, and what vkd3d-proton allocates for it, is not seen here: the process's whole use comes from
// VK_EXT_memory_budget (Session::video_memory).
#include "nr_ini_network.hpp"
#include "nr_vram_tag.hpp"
#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D11Resource;
struct ID3D10Resource;
struct VkPhysicalDeviceMemoryProperties;

namespace nr::vram {
// After the Vulkan entry points were (re)loaded (nr_vk_load): our allocation entry points wrapped. Safe to call again.
void install_vulkan();
// Which memory types are device-local (from the device the session runs on); until then every allocation counts.
void memory_types(const VkPhysicalDeviceMemoryProperties& props);
// A D3D12 resource of ours in a default heap, under `kind` and the thread's network.
void track_d3d12(ID3D12Device* device, ID3D12Resource* resource, Kind kind);
// The same for a D3D11 / D3D10 resource (the ReShade add-on's, through reshade::api on DXVK): D3D11 / D3D10 tell no
// allocation size, so its texels' bytes (format x extent, or a buffer's width) are counted; nothing for a format not
// known here. Counted out when the resource is destroyed (private data, as for D3D12).
void track_d3d11(ID3D11Resource* resource, Kind kind);
void track_d3d10(ID3D10Resource* resource, Kind kind);
// A resource the ledger cannot hold private data on (a Vulkan image made through reshade::api): `bytes` under `kind`
// by `key` until forget_external(key); forgetting a key not counted does nothing.
void add_external(uint64_t key, uint64_t bytes, Kind kind);
void forget_external(uint64_t key);
// Device-local bytes now, by owner (the network, nr::NetworkChoice; the last: none) and kind.
static constexpr int kOwners = nr::kNetworkChoices + 1;
void totals(uint64_t out[kOwners][kKinds]);
}  // namespace nr::vram
