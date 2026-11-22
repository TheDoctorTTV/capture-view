#pragma once

// Vulkan instance/device creation shared by the window renderer and the
// headless frame-generation probe used by the settings UI.

#include "vk_util.hpp"

#include <string>
#include <vector>

namespace cv::vk {

struct DeviceRequest {
  std::vector<std::string> instance_extensions; // e.g. from SDL_Vulkan_GetInstanceExtensions
  // Optional: the device must be able to present to this surface. The
  // surface is created by the caller from `instance` via a callback because it
  // needs the instance first.
  bool want_surface = false;
  bool want_frame_generation = false;
};

struct Device {
  Gpu gpu;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  // True when the device was created with everything NVIDIA DLSS Frame
  // Generation asked for (extensions/features). NGX itself decides the rest.
  bool frame_generation_extensions = false;
  std::string frame_generation_reason;
};

using SurfaceFactory = VkSurfaceKHR (*)(VkInstance instance, void* user);

Device create_device(const DeviceRequest& request, SurfaceFactory make_surface = nullptr, void* user = nullptr);
void destroy_device(Device& device);

} // namespace cv::vk
