#include "gpu_pipeline.hpp"

#include "frame_generator.hpp"
#include "log.hpp"
#include "vk_device.hpp"
#include "vk_util.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace cv {

namespace {

constexpr uint32_t kConvert[] = {
#include "convert.comp.spv.inc"
};
constexpr uint32_t kPack[] = {
#include "pack.comp.spv.inc"
};

constexpr uint32_t kMaxOutputs = FrameGenerator::kMaxMultiplier;

uint32_t output_format_code(const std::string& format) {
  if (format == "yuyv") return 1;
  if (format == "nv12") return 2;
  if (format == "rgba") return 3;
  return 0;
}

size_t output_bytes(uint32_t code, Size size) {
  const size_t pixels = static_cast<size_t>(size.width) * size.height;
  switch (code) {
  case 1:
    return pixels * 2;
  case 2:
    return pixels + pixels / 2;
  case 3:
    return pixels * 4;
  default:
    return 0;
  }
}

} // namespace

struct GpuPipeline::Impl {
  Size size;
  bool want_preview = true;
  uint32_t out_code = 0;
  uint32_t fg_requested = 1;
  uint32_t fg_active = 1;
  std::string fg_note;
  double gpu_ms = 0.0;
  double timestamp_period_ns = 0.0;

  vk::Device device;
  vk::Gpu& gpu() { return device.gpu; }
  VkCommandPool pool = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE; // two timestamps per slot
  vk::ComputePipeline convert;
  vk::ComputePipeline pack;
  vk::Buffer upload_vram; // VRAM copy of the captured frame
  vk::Image source;
  // Pack results live in VRAM; one bulk copy per frame moves them to the
  // slot's host-readable buffers. Shaders never touch system memory directly.
  // GPU work is ordered on one queue, so the VRAM intermediates are shared.
  std::array<vk::Buffer, kMaxOutputs> preview_vram{};
  std::array<vk::Buffer, kMaxOutputs> output_vram{};
  size_t preview_size = 0;
  size_t output_size = 0;
  std::unique_ptr<FrameGenerator> generator;
  bool initialised = false;

  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    vk::Buffer upload; // system memory, CPU writes the captured frame
    std::array<vk::Buffer, kMaxOutputs> preview{};
    std::array<vk::Buffer, kMaxOutputs> output{};
    uint32_t count = 0;
    bool submitted = false;
  };
  std::array<Slot, kSlots> slots{};

  void create(const GpuPipelineOptions& options);
  void destroy();
  void submit(uint32_t slot, const void* bytes, size_t length, uint32_t format, uint32_t stride, uint32_t uv_offset,
              bool reset);
  bool wait(uint32_t slot, std::chrono::nanoseconds timeout);
  void allocate_preview();
  void free_preview();
  void record_pack(VkCommandBuffer cmd, const vk::Image& image, uint32_t index);
};

void GpuPipeline::Impl::create(const GpuPipelineOptions& options) {
  size = options.size;
  want_preview = options.preview;
  out_code = output_format_code(options.output_format);
  fg_requested = std::clamp<uint32_t>(options.frame_generation, 1, FrameGenerator::kMaxMultiplier);
  if (size.width == 0 || size.height == 0 || size.width % 4 != 0 || size.height % 2 != 0) {
    throw AppError("GPU pipeline needs a width divisible by 4 and an even height");
  }

  vk::DeviceRequest request;
  request.want_frame_generation = fg_requested > 1;
  device = vk::create_device(request);
  log::info("gpu pipeline: ", gpu().properties.deviceName, " ", size.width, "x", size.height);

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = gpu().queue_family;
  vk::check(vkCreateCommandPool(gpu().device, &pool_info, nullptr, &pool), "vkCreateCommandPool");

  timestamp_period_ns = gpu().properties.limits.timestampPeriod;
  if (gpu().properties.limits.timestampComputeAndGraphics == VK_TRUE) {
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = kSlots * 2;
    vk::check(vkCreateQueryPool(gpu().device, &query_info, nullptr, &queries), "vkCreateQueryPool");
  }

  const VkDescriptorType convert_bindings[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  convert = vk::create_compute_pipeline(gpu(), kConvert, convert_bindings, 5 * sizeof(uint32_t));
  const VkDescriptorType pack_bindings[] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
  pack = vk::create_compute_pipeline(gpu(), kPack, pack_bindings, 4 * sizeof(uint32_t));

  source = vk::create_image(gpu(), size.width, size.height, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  output_size = output_bytes(out_code, size);
  const uint32_t outputs = fg_requested; // generated + real
  const VkBufferUsageFlags vram_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  for (uint32_t i = 0; i < outputs; ++i) {
    // Unused buffers still need a valid binding; keep them tiny.
    output_vram[i] = vk::create_device_buffer(gpu(), std::max<size_t>(output_size, 4), vram_usage);
  }
  for (auto& slot : slots) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vk::check(vkAllocateCommandBuffers(gpu().device, &alloc, &slot.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vk::check(vkCreateFence(gpu().device, &fence_info, nullptr, &slot.fence), "vkCreateFence");
    for (uint32_t i = 0; i < outputs; ++i) {
      if (output_size > 0) {
        slot.output[i] = vk::create_readback_buffer(gpu(), output_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      }
    }
  }

  allocate_preview();

  if (fg_requested > 1) {
    if (!device.frame_generation_extensions) {
      fg_note = device.frame_generation_reason.empty() ? "unavailable" : device.frame_generation_reason;
    } else {
      generator = std::make_unique<FrameGenerator>(gpu());
      if (!generator->caps().available) {
        fg_note = generator->caps().reason;
        generator.reset();
      } else if (generator->caps().max_multiplier < fg_requested) {
        fg_note = "this GPU supports up to " + std::to_string(generator->caps().max_multiplier) + "x";
      }
    }
    if (!fg_note.empty()) {
      log::warning("frame generation: ", fg_note);
    }
  }
}

// The preview copy (VRAM pack target + per-slot read-back) exists only while
// the GUI shows the video; without it the preview binding is a 4-byte stub.
void GpuPipeline::Impl::allocate_preview() {
  const VkBufferUsageFlags vram_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  preview_size = want_preview ? static_cast<size_t>(size.width) * size.height * 4 : 0;
  for (uint32_t i = 0; i < fg_requested; ++i) {
    vk::destroy_buffer(gpu(), preview_vram[i]);
    preview_vram[i] = vk::create_device_buffer(gpu(), std::max<size_t>(preview_size, 4), vram_usage);
    for (auto& slot : slots) {
      vk::destroy_buffer(gpu(), slot.preview[i]);
      if (preview_size > 0) {
        slot.preview[i] = vk::create_readback_buffer(gpu(), preview_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      }
    }
  }
}

void GpuPipeline::Impl::free_preview() {
  want_preview = false;
  allocate_preview();
}

void GpuPipeline::Impl::destroy() {
  if (gpu().device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(gpu().device);
    generator.reset();
    for (auto& slot : slots) {
      for (auto& buffer : slot.preview) vk::destroy_buffer(gpu(), buffer);
      for (auto& buffer : slot.output) vk::destroy_buffer(gpu(), buffer);
      vk::destroy_buffer(gpu(), slot.upload);
      if (slot.fence != VK_NULL_HANDLE) vkDestroyFence(gpu().device, slot.fence, nullptr);
    }
    for (auto& buffer : preview_vram) vk::destroy_buffer(gpu(), buffer);
    for (auto& buffer : output_vram) vk::destroy_buffer(gpu(), buffer);
    vk::destroy_buffer(gpu(), upload_vram);
    vk::destroy_image(gpu(), source);
    vk::destroy_pipeline(gpu(), convert);
    vk::destroy_pipeline(gpu(), pack);
    if (queries != VK_NULL_HANDLE) vkDestroyQueryPool(gpu().device, queries, nullptr);
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(gpu().device, pool, nullptr);
  }
  vk::destroy_device(device);
}

void GpuPipeline::Impl::record_pack(VkCommandBuffer cmd, const vk::Image& image, uint32_t index) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack.pipeline);
  VkDescriptorImageInfo image_info{VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo preview_info{preview_vram[index].buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo output_info{output_vram[index].buffer, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[3] = {vk::storage_image_write(0, &image_info), vk::storage_buffer_write(1, &preview_info),
                                    vk::storage_buffer_write(2, &output_info)};
  gpu().push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack.layout, 0, 3, writes);
  const uint32_t push[4] = {size.width, size.height, want_preview ? 1U : 0U, out_code};
  vkCmdPushConstants(cmd, pack.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  vkCmdDispatch(cmd, vk::group_count(size.width / 4, 8), vk::group_count(size.height / 2, 8), 1);
}

void GpuPipeline::Impl::submit(uint32_t index, const void* bytes, size_t length, uint32_t format, uint32_t stride,
                               uint32_t uv_offset, bool reset) {
  Slot& slot = slots.at(index);
  // The slot's previous batch must be finished before its buffers are reused.
  vk::check(vkWaitForFences(gpu().device, 1, &slot.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

  const VkDeviceSize needed = (length + 3) & ~size_t{3};
  if (slot.upload.buffer == VK_NULL_HANDLE || slot.upload.size < needed) {
    vk::destroy_buffer(gpu(), slot.upload);
    slot.upload = vk::create_host_buffer(gpu(), needed, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  }
  if (upload_vram.buffer == VK_NULL_HANDLE || upload_vram.size < needed) {
    vkDeviceWaitIdle(gpu().device);
    vk::destroy_buffer(gpu(), upload_vram);
    upload_vram = vk::create_device_buffer(gpu(), needed,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  std::memcpy(slot.upload.mapped, bytes, length);
  vk::check(vkResetFences(gpu().device, 1, &slot.fence), "vkResetFences");

  VkCommandBuffer cmd = slot.cmd;
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  if (queries != VK_NULL_HANDLE) {
    vkCmdResetQueryPool(cmd, queries, index * 2, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, index * 2);
  }
  if (!initialised) {
    vk::init_general_layout(cmd, source);
    initialised = true;
  }
  // Orders this batch after everything the other slot submitted earlier.
  vk::full_barrier(cmd);
  // One bulk DMA copy of the captured frame into VRAM.
  vk::copy_buffer(cmd, slot.upload, upload_vram, needed, true);

  // Colour conversion, reading only VRAM.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, convert.pipeline);
  VkDescriptorBufferInfo buffer_info{upload_vram.buffer, 0, needed};
  VkDescriptorImageInfo image_info{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[2] = {vk::storage_buffer_write(0, &buffer_info), vk::storage_image_write(1, &image_info)};
  gpu().push_descriptor(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, convert.layout, 0, 2, writes);
  const uint32_t push[5] = {size.width, size.height, stride, format, uv_offset};
  vkCmdPushConstants(cmd, convert.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  vkCmdDispatch(cmd, vk::group_count(size.width, 16), vk::group_count(size.height, 16), 1);
  vk::full_barrier(cmd);

  // Frame generation.
  uint32_t generated = 0;
  fg_active = 1;
  if (generator) {
    if (generator->configure(cmd, size.width, size.height, fg_requested)) {
      fg_active = std::min(fg_requested, generator->caps().max_multiplier);
      generated = generator->record(cmd, source, fg_active, reset);
    } else {
      fg_note = generator->last_error();
      log::warning("frame generation disabled: ", fg_note);
      generator.reset();
    }
  }

  // Pack every output frame, then bulk-copy them to this slot's host buffers.
  for (uint32_t i = 0; i < generated; ++i) {
    record_pack(cmd, generator->generated(i), i);
  }
  record_pack(cmd, source, generated);
  VkMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_transfer, 0,
                       nullptr, 0, nullptr);
  for (uint32_t i = 0; i <= generated; ++i) {
    if (preview_size > 0) {
      const VkBufferCopy region{0, 0, preview_size};
      vkCmdCopyBuffer(cmd, preview_vram[i].buffer, slot.preview[i].buffer, 1, &region);
    }
    if (output_size > 0) {
      const VkBufferCopy region{0, 0, output_size};
      vkCmdCopyBuffer(cmd, output_vram[i].buffer, slot.output[i].buffer, 1, &region);
    }
  }
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0,
                       nullptr);
  if (queries != VK_NULL_HANDLE) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, index * 2 + 1);
  }
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vk::check(vkQueueSubmit(gpu().queue, 1, &submit, slot.fence), "vkQueueSubmit");
  slot.count = generated + 1;
  slot.submitted = true;
}

bool GpuPipeline::Impl::wait(uint32_t index, std::chrono::nanoseconds timeout) {
  Slot& slot = slots.at(index);
  const auto ns = static_cast<uint64_t>(std::max<int64_t>(timeout.count(), 0));
  const VkResult result = vkWaitForFences(gpu().device, 1, &slot.fence, VK_TRUE, ns);
  if (result == VK_TIMEOUT) {
    return false;
  }
  vk::check(result, "vkWaitForFences");
  if (slot.submitted && queries != VK_NULL_HANDLE) {
    uint64_t stamps[2] = {};
    if (vkGetQueryPoolResults(gpu().device, queries, index * 2, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        stamps[1] > stamps[0]) {
      gpu_ms = static_cast<double>(stamps[1] - stamps[0]) * timestamp_period_ns / 1e6;
    }
  }
  slot.submitted = false;
  return true;
}

GpuPipeline::GpuPipeline(const GpuPipelineOptions& options) : impl_(std::make_unique<Impl>()) {
  try {
    impl_->create(options);
  } catch (...) {
    impl_->destroy();
    throw;
  }
}

GpuPipeline::~GpuPipeline() { impl_->destroy(); }

void GpuPipeline::submit(uint32_t slot, const RgbaFrame& frame, bool reset) {
  if (frame.size.width != impl_->size.width || frame.size.height != impl_->size.height) {
    throw AppError("GPU pipeline frame size changed");
  }
  impl_->submit(slot, frame.pixels.data(), frame.pixels.size(), 0, frame.size.width * 4, 0, reset);
}

void GpuPipeline::submit(uint32_t slot, FrameView frame, bool reset) {
  if (frame.size.width != impl_->size.width || frame.size.height != impl_->size.height) {
    throw AppError("GPU pipeline frame size changed");
  }
  if (frame.format != PixelFormat::Yuyv && frame.format != PixelFormat::Nv12) {
    throw AppError("GPU pipeline takes raw YUYV or NV12 frames");
  }
  const size_t stride = raw_frame_stride(frame);
  const bool yuyv = frame.format == PixelFormat::Yuyv;
  const size_t rows = static_cast<size_t>(frame.size.height) + (yuyv ? 0U : frame.size.height / 2U);
  const size_t length = std::min(frame.bytes.size(), rows * stride);
  impl_->submit(slot, frame.bytes.data(), length, yuyv ? 1U : 2U, static_cast<uint32_t>(stride),
                static_cast<uint32_t>(stride * frame.size.height), reset);
}

bool GpuPipeline::wait(uint32_t slot, std::chrono::nanoseconds timeout) { return impl_->wait(slot, timeout); }

uint32_t GpuPipeline::frame_count(uint32_t slot) const { return impl_->slots.at(slot).count; }

uint32_t GpuPipeline::process(const RgbaFrame& frame, bool reset) {
  submit(0, frame, reset);
  wait(0, std::chrono::nanoseconds::max());
  return frame_count(0);
}

uint32_t GpuPipeline::process(FrameView frame, bool reset) {
  submit(0, frame, reset);
  wait(0, std::chrono::nanoseconds::max());
  return frame_count(0);
}

std::span<const std::byte> GpuPipeline::preview(uint32_t slot, uint32_t index) const {
  const auto& buffer = impl_->slots.at(slot).preview[index];
  return {static_cast<const std::byte*>(buffer.mapped), buffer.mapped != nullptr ? impl_->preview_size : 0};
}

std::span<const std::byte> GpuPipeline::output(uint32_t slot, uint32_t index) const {
  const auto& buffer = impl_->slots.at(slot).output[index];
  return {static_cast<const std::byte*>(buffer.mapped), buffer.mapped != nullptr ? impl_->output_size : 0};
}

void GpuPipeline::set_preview(bool enabled) {
  if (enabled == impl_->want_preview) return;
  vkDeviceWaitIdle(impl_->gpu().device);
  for (auto& slot : impl_->slots) slot.submitted = false;
  if (enabled) {
    impl_->want_preview = true;
    impl_->allocate_preview();
  } else {
    impl_->free_preview();
  }
  log::info("preview copy ", enabled ? "enabled" : "disabled (no VRAM or read-back used)");
}

bool GpuPipeline::preview_enabled() const { return impl_->want_preview; }

uint32_t GpuPipeline::active_frame_generation() const { return impl_->fg_active; }
uint32_t GpuPipeline::expected_frame_generation() const {
  return impl_->generator ? std::min(impl_->fg_requested, impl_->generator->caps().max_multiplier) : 1U;
}
const std::string& GpuPipeline::frame_generation_note() const { return impl_->fg_note; }
double GpuPipeline::gpu_ms() const { return impl_->gpu_ms; }

} // namespace cv
