#include "vk_util.hpp"

#include <string>

namespace cv::vk {

std::string result_string(VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_SUBOPTIMAL_KHR:
    return "VK_SUBOPTIMAL_KHR";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_EXTENSION_NOT_PRESENT:
    return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT:
    return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER:
    return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_SURFACE_LOST_KHR:
    return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_OUT_OF_DATE_KHR:
    return "VK_ERROR_OUT_OF_DATE_KHR";
  default:
    return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
  }
}

void check(VkResult result, const char* what) {
  if (result != VK_SUCCESS) {
    throw AppError(std::string(what) + " failed: " + result_string(result));
  }
}

uint32_t find_memory_type(const Gpu& gpu, uint32_t type_bits, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < gpu.memory.memoryTypeCount; ++i) {
    if ((type_bits & (1U << i)) != 0 && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  throw AppError("no suitable Vulkan memory type");
}

Image create_image(const Gpu& gpu, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage) {
  Image out;
  out.format = format;
  out.width = width;
  out.height = height;

  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  check(vkCreateImage(gpu.device, &info, nullptr, &out.image), "vkCreateImage");

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(gpu.device, out.image, &requirements);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = find_memory_type(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory), "vkAllocateMemory(image)");
  check(vkBindImageMemory(gpu.device, out.image, out.memory, 0), "vkBindImageMemory");

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  check(vkCreateImageView(gpu.device, &view, nullptr, &out.view), "vkCreateImageView");
  return out;
}

void destroy_image(const Gpu& gpu, Image& image) {
  if (image.view != VK_NULL_HANDLE) {
    vkDestroyImageView(gpu.device, image.view, nullptr);
  }
  if (image.image != VK_NULL_HANDLE) {
    vkDestroyImage(gpu.device, image.image, nullptr);
  }
  if (image.memory != VK_NULL_HANDLE) {
    vkFreeMemory(gpu.device, image.memory, nullptr);
  }
  image = {};
}

namespace {

Buffer create_mapped_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage, bool cached) {
  Buffer out;
  out.size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(gpu.device, &info, nullptr, &out.buffer), "vkCreateBuffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(gpu.device, out.buffer, &requirements);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = requirements.size;
  const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  try {
    alloc.memoryTypeIndex = find_memory_type(gpu, requirements.memoryTypeBits,
                                             cached ? coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : coherent);
  } catch (const AppError&) {
    alloc.memoryTypeIndex = find_memory_type(gpu, requirements.memoryTypeBits, coherent);
  }
  check(vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory), "vkAllocateMemory(buffer)");
  check(vkBindBufferMemory(gpu.device, out.buffer, out.memory, 0), "vkBindBufferMemory");
  check(vkMapMemory(gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
  return out;
}

} // namespace

Buffer create_host_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
  return create_mapped_buffer(gpu, size, usage, false);
}

Buffer create_readback_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
  return create_mapped_buffer(gpu, size, usage, true);
}

Buffer create_device_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage) {
  Buffer out;
  out.size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  check(vkCreateBuffer(gpu.device, &info, nullptr, &out.buffer), "vkCreateBuffer");
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(gpu.device, out.buffer, &requirements);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = find_memory_type(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  check(vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory), "vkAllocateMemory(device buffer)");
  check(vkBindBufferMemory(gpu.device, out.buffer, out.memory, 0), "vkBindBufferMemory");
  return out;
}

void copy_buffer(VkCommandBuffer cmd, const Buffer& src, const Buffer& dst, VkDeviceSize size, bool to_shader) {
  const VkBufferCopy region{0, 0, size};
  vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &region);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = to_shader ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       to_shader ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
}

void destroy_buffer(const Gpu& gpu, Buffer& buffer) {
  if (buffer.mapped != nullptr) {
    vkUnmapMemory(gpu.device, buffer.memory);
  }
  if (buffer.buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(gpu.device, buffer.buffer, nullptr);
  }
  if (buffer.memory != VK_NULL_HANDLE) {
    vkFreeMemory(gpu.device, buffer.memory, nullptr);
  }
  buffer = {};
}

void init_general_layout(VkCommandBuffer cmd, const Image& image) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);
}

void full_barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
}

VkShaderModule create_shader(const Gpu& gpu, std::span<const uint32_t> code) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size_bytes();
  info.pCode = code.data();
  VkShaderModule module = VK_NULL_HANDLE;
  check(vkCreateShaderModule(gpu.device, &info, nullptr, &module), "vkCreateShaderModule");
  return module;
}

ComputePipeline create_compute_pipeline(const Gpu& gpu, std::span<const uint32_t> code,
                                        std::span<const VkDescriptorType> bindings, uint32_t push_constant_size) {
  ComputePipeline out;
  std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    layout_bindings.push_back({i, bindings[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  set_info.bindingCount = static_cast<uint32_t>(layout_bindings.size());
  set_info.pBindings = layout_bindings.data();
  check(vkCreateDescriptorSetLayout(gpu.device, &set_info, nullptr, &out.set_layout), "vkCreateDescriptorSetLayout");

  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_constant_size};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &out.set_layout;
  layout_info.pushConstantRangeCount = push_constant_size > 0 ? 1U : 0U;
  layout_info.pPushConstantRanges = &range;
  check(vkCreatePipelineLayout(gpu.device, &layout_info, nullptr, &out.layout), "vkCreatePipelineLayout");

  VkShaderModule module = create_shader(gpu, code);
  VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  info.stage.module = module;
  info.stage.pName = "main";
  info.layout = out.layout;
  const VkResult result = vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &out.pipeline);
  vkDestroyShaderModule(gpu.device, module, nullptr);
  check(result, "vkCreateComputePipelines");
  return out;
}

void destroy_pipeline(const Gpu& gpu, ComputePipeline& pipeline) {
  if (pipeline.pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(gpu.device, pipeline.pipeline, nullptr);
  }
  if (pipeline.layout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(gpu.device, pipeline.layout, nullptr);
  }
  if (pipeline.set_layout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(gpu.device, pipeline.set_layout, nullptr);
  }
  pipeline = {};
}

VkWriteDescriptorSet storage_image_write(uint32_t binding, const VkDescriptorImageInfo* info) {
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstBinding = binding;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write.pImageInfo = info;
  return write;
}

VkWriteDescriptorSet storage_buffer_write(uint32_t binding, const VkDescriptorBufferInfo* info) {
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstBinding = binding;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = info;
  return write;
}

} // namespace cv::vk
