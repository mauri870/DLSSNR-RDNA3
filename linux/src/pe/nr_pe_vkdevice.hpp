#pragma once
// Watching a Vulkan game create its device, so the three Vulkan paths do not
// each have to ask their upscaler for handles the upscaler may not carry.
//
// The two D3D runtimes have an interop object that hands over the instance, the
// physical device, the device and a submittable queue. A Vulkan game has nothing
// of the sort: NGX's init passes the handles but no queue, the ffx-api's Vulkan
// backend passes them only at context creation, and XeSS passes none of them at
// dispatch. All three can be served from one place - the game's own
// `vkCreateDevice` and `vkGetDeviceQueue` - and then no Vulkan path depends on
// which upscaler happened to fire.
//
// The queue matters and is not a detail: building the network uploads its
// weights, which is a submit. Without a queue the Vulkan paths can record
// nothing at all.
#include "nr_pe_interop.hpp"

namespace nr::pe::vkdevice {

// Patch vkCreateInstance / vkCreateDevice / vkGetDeviceQueue in vulkan-1.dll.
// Safe to call in a game that never touches Vulkan: the module is simply absent
// and this reports false.
bool install();

// Take the hooks out again. ReShade unloads its add-ons whenever its last
// device goes away - a game that makes a probe device and destroys it does
// that mid-run - and vulkan-1.dll stays loaded under DXVK/vkd3d-proton, so a
// hook left behind jumps into unmapped code at the next vkCreateInstance.
void uninstall();

// The game's handles, once it has made a device. Invalid until then.
DeviceHandles handles();

// A queue on a family that can run compute and graphics, which is what the
// network's upload and its passes need. Zero family with a null queue means the
// game has not created a usable one (yet).
bool queue(VkQueue* out, uint32_t* family);

// The family a queue the game created belongs to. Submitting a command buffer
// allocated from one family's pool onto a queue from another is invalid, and the
// present queue is not necessarily the queue found above - so whenever we submit
// onto a queue the game handed us, its own family is the one that matters.
// Returns false for a queue we never saw created.
bool family_of(VkQueue queue, uint32_t* family);

// A queue for THIS device, for a caller that may be running inside the
// game's own vkCreateDevice (ReShade's Vulkan layer fires its device event
// from there, before the game has asked for any queue). Falls back to asking
// the device for the first queue of a graphics+compute family the game
// requested, which is legal once vkCreateDevice's chain has returned.
bool queue_for(VkDevice device, VkQueue* out, uint32_t* family);

// Called after the game's vkCreateDevice has returned - the loader's
// dispatch is set on the handle by then, which it is not while ReShade's
// device events run inside the call. One callback, set once.
void set_device_callback(void (*callback)(VkDevice));

// Some upscalers hand us the device before we have seen it created - NGX's
// Vulkan init does. Recording it here keeps one source of truth.
void note_handles(VkInstance instance, VkPhysicalDevice physical, VkDevice device);

// A device the game made, seen by someone other than the watcher (the .asi's vkCreateDevice wrap): its handles and
// the queue families it asked for, so queue_for() can find a queue of one of them.
void note_device(VkInstance instance, VkPhysicalDevice physical, VkDevice device, const std::vector<uint32_t>& families);

// The game destroys `device`: its queues and handles are forgotten (a later device may get the same handle).
void forget_device(VkDevice device);

// Submits of ours onto a queue the game also submits to, serialised with the game's own: once enabled (the .asi on a
// Vulkan game, which wraps the game's queue calls with the same per-queue lock), lock_queue / unlock_queue take the
// queue's recursive lock; before that they do nothing (the ReShade routes: ReShade serialises its own queues).
void enable_queue_locks();
bool queue_locks_enabled();
void lock_queue(VkQueue queue);
void unlock_queue(VkQueue queue);
// The same for a call that may begin before enable_queue_locks and end after it (a hook installed earlier): the lock
// is released only if it was taken.
bool lock_queue_held(VkQueue queue);
void unlock_queue_held(VkQueue queue, bool held);

}  // namespace nr::pe::vkdevice
