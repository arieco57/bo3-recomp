#pragma once
#include "volk.h"   // Vulkan functions loaded at runtime (Android: 1.3 entry points are not in libvulkan.so)
#include <string>
#include <vector>

// Text lines of the performance overlay (empty = hidden). Render thread.
void OverlaySetText(const std::vector<std::string>& lines);
// Copies the overlay into the swapchain image `target` (layout TRANSFER_DST) at the top left.
void OverlayRecord(VkCommandBuffer cmd, VkImage target, VkExtent2D extent);
// On-screen "OPCIONES" button (touch UI): drawn top right; its rectangle as a fraction of the
// screen for hit tests (x0, y0, x1, y1).
void OverlayButtonRecord(VkCommandBuffer cmd, VkImage target, VkExtent2D extent);
void OverlayButtonRect(float r[4]);
