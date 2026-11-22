#include "vk_device.hpp"

#include "frame_generator.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace cv::vk {

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& available, const std::string& name) {
  return std::ranges::any_of(available,
                             [&](const VkExtensionProperties& ext) { return name == ext.extensionName; });
}

std::vector<VkExtensionProperties> instance_extension_list() {
  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> out(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, out.data());
  return out;
}

std::vector<VkExtensionProperties> device_extension_list(VkPhysicalDevice physical) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> out(count);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, out.data());
  return out;
}

struct Candidate {
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  int score = -1;
};

Candidate pick_device(VkInstance instance, VkSurfaceKHR surface, bool want_frame_generation) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  Candidate best;
  for (VkPhysicalDevice physical : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
      continue;
    }
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (features13.dynamicRendering != VK_TRUE) {
      continue;
    }
    const auto extensions = device_extension_list(physical);
    if (!has_extension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) ||
        (surface != VK_NULL_HANDLE && !has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
      continue;
    }
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    for (uint32_t i = 0; i < family_count; ++i) {
      const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      if ((families[i].queueFlags & need) != need) {
        continue;
      }
      if (surface != VK_NULL_HANDLE) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        if (present != VK_TRUE) {
          continue;
        }
      }
      int score = 1;
      if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 100;
      } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 50;
      }
      if (want_frame_generation && props.vendorID == 0x10DE) {
        score += 1000;
      }
      if (score > best.score) {
        best = {physical, i, score};
      }
      break;
    }
  }
  return best;
}

// Enables the optional features capture-view or NGX may use, when supported.
struct FeatureChain {
  VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

  void link(bool with13) {
    core.pNext = &v11;
    v11.pNext = &v12;
    v12.pNext = with13 ? &v13 : nullptr;
    v13.pNext = nullptr;
  }
};

FeatureChain choose_features(VkPhysicalDevice physical, bool with13) {
  FeatureChain supported;
  supported.link(with13);
  vkGetPhysicalDeviceFeatures2(physical, &supported.core);

  FeatureChain enabled;
  enabled.link(with13);
  auto& s = supported.core.features;
  auto& e = enabled.core.features;
  e.shaderStorageImageExtendedFormats = s.shaderStorageImageExtendedFormats;
  e.shaderStorageImageWriteWithoutFormat = s.shaderStorageImageWriteWithoutFormat;
  e.shaderStorageImageReadWithoutFormat = s.shaderStorageImageReadWithoutFormat;
  e.shaderInt16 = s.shaderInt16;
  e.shaderInt64 = s.shaderInt64;
  e.fragmentStoresAndAtomics = s.fragmentStoresAndAtomics;
  e.samplerAnisotropy = s.samplerAnisotropy;
  enabled.v11.storageBuffer16BitAccess = supported.v11.storageBuffer16BitAccess;
  enabled.v11.uniformAndStorageBuffer16BitAccess = supported.v11.uniformAndStorageBuffer16BitAccess;
  enabled.v12.bufferDeviceAddress = supported.v12.bufferDeviceAddress;
  enabled.v12.timelineSemaphore = supported.v12.timelineSemaphore;
  enabled.v12.shaderFloat16 = supported.v12.shaderFloat16;
  enabled.v12.shaderInt8 = supported.v12.shaderInt8;
  enabled.v12.storageBuffer8BitAccess = supported.v12.storageBuffer8BitAccess;
  enabled.v12.uniformAndStorageBuffer8BitAccess = supported.v12.uniformAndStorageBuffer8BitAccess;
  enabled.v12.descriptorIndexing = supported.v12.descriptorIndexing;
  enabled.v12.runtimeDescriptorArray = supported.v12.runtimeDescriptorArray;
  enabled.v12.descriptorBindingPartiallyBound = supported.v12.descriptorBindingPartiallyBound;
  enabled.v12.shaderSampledImageArrayNonUniformIndexing = supported.v12.shaderSampledImageArrayNonUniformIndexing;
  enabled.v12.scalarBlockLayout = supported.v12.scalarBlockLayout;
  enabled.v12.hostQueryReset = supported.v12.hostQueryReset;
  enabled.v12.vulkanMemoryModel = supported.v12.vulkanMemoryModel;
  if (with13) {
    enabled.v13.synchronization2 = supported.v13.synchronization2;
    enabled.v13.dynamicRendering = supported.v13.dynamicRendering;
    enabled.v13.maintenance4 = supported.v13.maintenance4;
    enabled.v13.shaderDemoteToHelperInvocation = supported.v13.shaderDemoteToHelperInvocation;
    enabled.v13.subgroupSizeControl = supported.v13.subgroupSizeControl;
    enabled.v13.computeFullSubgroups = supported.v13.computeFullSubgroups;
  }
  return enabled;
}

} // namespace

Device create_device(const DeviceRequest& request, SurfaceFactory make_surface, void* user) {
  Device out;

  // Instance.
  const auto available_instance = instance_extension_list();
  std::vector<std::string> instance_names = request.instance_extensions;
  if (request.want_frame_generation) {
    std::vector<std::string> ngx;
    frame_generation_instance_extensions(ngx);
    for (const auto& name : ngx) {
      if (has_extension(available_instance, name)) {
        instance_names.push_back(name);
      } else {
        log::warning("frame generation: instance extension unavailable: ", name);
      }
    }
  }
  std::set<std::string> unique(instance_names.begin(), instance_names.end());
  std::vector<const char*> instance_ptrs;
  for (const auto& name : unique) {
    instance_ptrs.push_back(name.c_str());
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "capture-view";
  app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
  app.pEngineName = "capture-view";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_ptrs.size());
  instance_info.ppEnabledExtensionNames = instance_ptrs.data();
  check(vkCreateInstance(&instance_info, nullptr, &out.gpu.instance), "vkCreateInstance");

  try {
    if (request.want_surface && make_surface != nullptr) {
      out.surface = make_surface(out.gpu.instance, user);
      if (out.surface == VK_NULL_HANDLE) {
        throw AppError("could not create a Vulkan surface for the window");
      }
    }

    const Candidate chosen = pick_device(out.gpu.instance, out.surface, request.want_frame_generation);
    if (chosen.physical == VK_NULL_HANDLE) {
      throw AppError("no Vulkan 1.3 GPU with graphics, compute, dynamic rendering and push descriptors found");
    }
    out.gpu.physical = chosen.physical;
    out.gpu.queue_family = chosen.queue_family;
    vkGetPhysicalDeviceProperties(out.gpu.physical, &out.gpu.properties);
    vkGetPhysicalDeviceMemoryProperties(out.gpu.physical, &out.gpu.memory);
    log::info("vulkan device: ", out.gpu.properties.deviceName, " api ", VK_API_VERSION_MAJOR(out.gpu.properties.apiVersion),
              ".", VK_API_VERSION_MINOR(out.gpu.properties.apiVersion));

    // Device extensions.
    const auto available_device = device_extension_list(out.gpu.physical);
    std::vector<std::string> device_names{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (out.surface != VK_NULL_HANDLE) {
      device_names.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (request.want_frame_generation) {
      std::vector<std::string> ngx;
      std::string reason;
      if (frame_generation_device_extensions(out.gpu.instance, out.gpu.physical, ngx, reason)) {
        const bool all = std::ranges::all_of(ngx, [&](const std::string& name) {
          const bool ok = has_extension(available_device, name);
          if (!ok) {
            log::warning("frame generation: device extension unavailable: ", name);
          }
          return ok;
        });
        if (all) {
          device_names.insert(device_names.end(), ngx.begin(), ngx.end());
          out.frame_generation_extensions = true;
        } else {
          out.frame_generation_reason = "the GPU driver is missing Vulkan extensions DLSS Frame Generation needs";
        }
      } else {
        out.frame_generation_reason = reason;
      }
    }
    std::set<std::string> unique_device(device_names.begin(), device_names.end());
    std::vector<const char*> device_ptrs;
    for (const auto& name : unique_device) {
      device_ptrs.push_back(name.c_str());
    }

    FeatureChain features = choose_features(out.gpu.physical, true);

    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = out.gpu.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features.core;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_ptrs.size());
    device_info.ppEnabledExtensionNames = device_ptrs.data();
    check(vkCreateDevice(out.gpu.physical, &device_info, nullptr, &out.gpu.device), "vkCreateDevice");
    vkGetDeviceQueue(out.gpu.device, out.gpu.queue_family, 0, &out.gpu.queue);
    out.gpu.push_descriptor = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(out.gpu.device, "vkCmdPushDescriptorSetKHR"));
    if (out.gpu.push_descriptor == nullptr) {
      throw AppError("vkCmdPushDescriptorSetKHR unavailable");
    }
  } catch (...) {
    destroy_device(out);
    throw;
  }
  return out;
}

void destroy_device(Device& device) {
  if (device.gpu.device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device.gpu.device);
    vkDestroyDevice(device.gpu.device, nullptr);
  }
  if (device.surface != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(device.gpu.instance, device.surface, nullptr);
  }
  if (device.gpu.instance != VK_NULL_HANDLE) {
    vkDestroyInstance(device.gpu.instance, nullptr);
  }
  device = {};
}

} // namespace cv::vk
