#pragma once

// Small Vulkan helpers shared by the renderer and the frame generator.

#include "common.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cv::vk {

[[nodiscard]] std::string result_string(VkResult result);
void check(VkResult result, const char* what);

// Everything the renderer and the frame generator need to share.
struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory{};
  PFN_vkCmdPushDescriptorSetKHR push_descriptor = nullptr;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;
};

uint32_t find_memory_type(const Gpu& gpu, uint32_t type_bits, VkMemoryPropertyFlags flags);

// Images are created and always kept in VK_IMAGE_LAYOUT_GENERAL; that keeps
// sharing with NGX simple and costs nothing measurable on desktop GPUs.
Image create_image(const Gpu& gpu, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
void destroy_image(const Gpu& gpu, Image& image);

Buffer create_host_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage);
// Host-visible buffer the GPU writes and the CPU reads; uses cached memory when
// available so CPU reads are fast.
Buffer create_readback_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage);
// VRAM-only buffer (not mappable). Shaders should only ever touch VRAM; data
// moves between system memory and VRAM with vkCmdCopyBuffer (bulk DMA).
Buffer create_device_buffer(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage);
void destroy_buffer(const Gpu& gpu, Buffer& buffer);
// Records a whole-buffer copy followed by a barrier that makes it visible to
// compute shaders (to_shader) or to the CPU (!to_shader).
void copy_buffer(VkCommandBuffer cmd, const Buffer& src, const Buffer& dst, VkDeviceSize size, bool to_shader);

// Transitions a freshly created image from UNDEFINED to GENERAL.
void init_general_layout(VkCommandBuffer cmd, const Image& image);
// Full memory dependency between GPU work recorded before and after.
void full_barrier(VkCommandBuffer cmd);

VkShaderModule create_shader(const Gpu& gpu, std::span<const uint32_t> code);

// Compute pipeline that takes all of its resources as push descriptors.
struct ComputePipeline {
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
};

ComputePipeline create_compute_pipeline(const Gpu& gpu, std::span<const uint32_t> code,
                                        std::span<const VkDescriptorType> bindings, uint32_t push_constant_size);
void destroy_pipeline(const Gpu& gpu, ComputePipeline& pipeline);

// Builders for push-descriptor writes.
VkWriteDescriptorSet storage_image_write(uint32_t binding, const VkDescriptorImageInfo* info);
VkWriteDescriptorSet storage_buffer_write(uint32_t binding, const VkDescriptorBufferInfo* info);

[[nodiscard]] inline uint32_t group_count(uint32_t size, uint32_t local) { return (size + local - 1) / local; }

} // namespace cv::vk
