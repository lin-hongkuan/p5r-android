// What vk_bridge.cpp (loader, instance, device, surfaces) and vk_swapchain.cpp (swapchains, acquire, present,
// screenshots) share. Split from vk_bridge.cpp (700-line rule).
#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "vk_gen.h"

namespace nx::vk::bridge {

VkPhysicalDevice physical();               // the GPU the game was given (vkEnumeratePhysicalDevices)
VkSurfaceKHR host_surface(uint64_t game);  // the host surface behind a game handle, re-made on a new window
// The host surface behind a game handle after re-making it if the OS replaced the window; VK_NULL_HANDLE when the
// handle is unknown or there is no window.
VkSurfaceKHR refresh_host_surface(uint64_t game);
uint64_t new_handle();  // a game-visible surface or swapchain handle (ours, never a host value)

// Two-call enumeration helper: fills the guest array from `all`.
template <typename T>
VkResult enumerate(const std::vector<T>& all, uint64_t count_ptr, uint64_t out) {
    uint32_t n = ld<uint32_t>(count_ptr);
    if (!out) {
        st<uint32_t>(count_ptr, uint32_t(all.size()));
        return VK_SUCCESS;
    }
    uint32_t w = std::min<uint32_t>(n, uint32_t(all.size()));
    std::memcpy(reinterpret_cast<void*>(out), all.data(), sizeof(T) * w);
    st<uint32_t>(count_ptr, w);
    return w < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

}  // namespace nx::vk::bridge

namespace nx::vk {

// ---- present mode (vk_swapchain.cpp) ----

const char* present_mode_name(VkPresentModeKHR m);  // for logs

// The one place a present mode is decided: vk_swapchain.cpp::adapt_to_surface (a game that makes its own swapchains)
// and nvn_vk.cpp::make_swapchain (NVN's, which is what P5R uses) both come here, so the two cannot disagree.
//
// `auto_pref`/`auto_count` are the modes the caller would have picked before the perf profile existed, best first; with
// present_mode unset (the default) that list is walked exactly as before, so the default is upstream's choice byte for
// byte. An explicit present_mode replaces it with the mode asked for and a safe fallback chain (immediate -> mailbox ->
// fifo, or mailbox -> immediate -> fifo), checked against vkGetPhysicalDeviceSurfacePresentModesKHR, FIFO last. Logs
// what it actually chose under `who`.
VkPresentModeKHR resolve_present_mode(VkPhysicalDevice pd, VkSurfaceKHR surface, const VkPresentModeKHR* auto_pref,
                                      size_t auto_count, const char* who);

}  // namespace nx::vk
