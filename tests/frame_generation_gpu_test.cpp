// GPU test for DLSS Frame Generation on video content.
//
// Feeds the frame generator a textured pattern that pans at a known speed and
// checks that (1) the estimated motion vectors match the true motion and
// (2) every generated frame matches the analytically shifted in-between image
// better than either neighbouring real frame does.
//
// Needs an NVIDIA RTX 40/50 GPU and the downloaded runtime; exits 77 (skipped)
// otherwise. Run outside sandboxes that hide /dev/nvidia*.

#include "frame_generator.hpp"
#include "log.hpp"
#include "vk_device.hpp"
#include "vk_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace cv;

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
// Pan speed in pixels per captured frame (content moves right and down).
constexpr float kDx = 12.0F;
constexpr float kDy = 6.0F;

float pattern(float x, float y) {
  // Smooth but detailed: several sinusoids plus a checker so block matching
  // has unambiguous texture everywhere.
  const float a = std::sin(x * 0.071F) * std::cos(y * 0.053F);
  const float b = std::sin((x + y) * 0.031F + 1.3F);
  const float c = (static_cast<int>(std::floor(x / 23.0F)) + static_cast<int>(std::floor(y / 19.0F))) % 2 == 0 ? 0.2F
                                                                                                            : -0.2F;
  return std::clamp(0.5F + 0.25F * a + 0.15F * b + c, 0.0F, 1.0F);
}

std::vector<uint8_t> make_frame(float offset_x, float offset_y) {
  std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const float sx = static_cast<float>(x) - offset_x;
      const float sy = static_cast<float>(y) - offset_y;
      const float v = pattern(sx, sy);
      const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
      rgba[i] = static_cast<uint8_t>(v * 255.0F);
      rgba[i + 1] = static_cast<uint8_t>(pattern(sx + 37.0F, sy) * 255.0F);
      rgba[i + 2] = static_cast<uint8_t>(pattern(sx, sy + 51.0F) * 255.0F);
      rgba[i + 3] = 255;
    }
  }
  return rgba;
}

// Mean absolute error over the interior (borders contain newly revealed
// content that no interpolator can know).
double interior_error(const uint8_t* a, const std::vector<uint8_t>& b) {
  const uint32_t margin = 64;
  double sum = 0.0;
  size_t count = 0;
  for (uint32_t y = margin; y < kHeight - margin; ++y) {
    for (uint32_t x = margin; x < kWidth - margin; ++x) {
      const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
      for (int c = 0; c < 3; ++c) {
        sum += std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]));
        ++count;
      }
    }
  }
  return sum / static_cast<double>(count);
}

float half_to_float(uint16_t h) {
  const uint32_t sign = (h >> 15U) & 1U;
  const uint32_t exponent = (h >> 10U) & 0x1FU;
  const uint32_t mantissa = h & 0x3FFU;
  float value = 0.0F;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? INFINITY : NAN;
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
  }
  return sign != 0 ? -value : value;
}

struct Runner {
  vk::Gpu gpu;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  explicit Runner(const vk::Gpu& g) : gpu(g) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = gpu.queue_family;
    vk::check(vkCreateCommandPool(gpu.device, &pool_info, nullptr, &pool), "pool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vk::check(vkAllocateCommandBuffers(gpu.device, &alloc, &cmd), "cmd");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk::check(vkCreateFence(gpu.device, &fence_info, nullptr, &fence), "fence");
  }
  ~Runner() {
    vkDestroyFence(gpu.device, fence, nullptr);
    vkDestroyCommandPool(gpu.device, pool, nullptr);
  }
  VkCommandBuffer begin() {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &info);
    return cmd;
  }
  void submit_and_wait() {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vk::check(vkQueueSubmit(gpu.queue, 1, &submit, fence), "submit");
    vk::check(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
    vk::check(vkResetFences(gpu.device, 1, &fence), "reset");
  }
};

void copy_image(VkCommandBuffer cmd, const vk::Image& image, const vk::Buffer& buffer, bool to_image) {
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {image.width, image.height, 1};
  if (to_image) {
    vkCmdCopyBufferToImage(cmd, buffer.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  } else {
    vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1, &region);
  }
  vk::full_barrier(cmd);
}

} // namespace

int main(int argc, char** argv) {
  const uint32_t multiplier = argc > 1 ? static_cast<uint32_t>(std::stoul(argv[1])) : 2;
  log::init("", false);
  vk::Device device;
  try {
    vk::DeviceRequest request;
    request.want_frame_generation = true;
    device = vk::create_device(request);
  } catch (const AppError& error) {
    std::printf("SKIP: %s\n", error.what());
    return 77;
  }
  if (!device.frame_generation_extensions) {
    std::printf("SKIP: %s\n", device.frame_generation_reason.c_str());
    vk::destroy_device(device);
    return 77;
  }

  int failures = 0;
  {
    FrameGenerator generator(device.gpu);
    if (!generator.caps().available) {
      std::printf("SKIP: %s\n", generator.caps().reason.c_str());
      return 77;
    }
    if (multiplier > generator.caps().max_multiplier) {
      std::printf("SKIP: %ux exceeds this GPU's %ux\n", multiplier, generator.caps().max_multiplier);
      return 77;
    }
    Runner runner(device.gpu);
    const vk::Gpu& gpu = device.gpu;
    vk::Image source = vk::create_image(gpu, kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    vk::Buffer upload = vk::create_host_buffer(gpu, static_cast<VkDeviceSize>(kWidth) * kHeight * 4,
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    vk::Buffer readback = vk::create_host_buffer(gpu, static_cast<VkDeviceSize>(kWidth) * kHeight * 4,
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBuffer cmd = runner.begin();
    vk::init_general_layout(cmd, source);
    if (!generator.configure(cmd, kWidth, kHeight, multiplier)) {
      std::printf("FAIL: configure: %s\n", generator.last_error().c_str());
      return 1;
    }
    runner.submit_and_wait();

    constexpr int kFrames = 8;
    std::vector<uint8_t> previous;
    for (int frame = 0; frame < kFrames; ++frame) {
      const auto pixels = make_frame(kDx * static_cast<float>(frame), kDy * static_cast<float>(frame));
      std::memcpy(upload.mapped, pixels.data(), pixels.size());
      cmd = runner.begin();
      copy_image(cmd, source, upload, true);
      const uint32_t generated = generator.record(cmd, source, multiplier, frame == 0);
      runner.submit_and_wait();

      if (frame < kFrames - 1) {
        previous = pixels;
        continue;
      }
      // Motion vectors: expect (previous - current) in half-resolution pixels.
      const vk::Image& mvec = generator.motion_vectors();
      cmd = runner.begin();
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {mvec.width, mvec.height, 1};
      vkCmdCopyImageToBuffer(cmd, mvec.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
      runner.submit_and_wait();
      const auto* halves = static_cast<const uint16_t*>(readback.mapped);
      double sum_x = 0.0;
      double sum_y = 0.0;
      size_t good = 0;
      size_t count = 0;
      const float expect_x = -kDx * static_cast<float>(mvec.width) / static_cast<float>(kWidth);
      const float expect_y = -kDy * static_cast<float>(mvec.height) / static_cast<float>(kHeight);
      for (uint32_t y = 32; y < mvec.height - 32; ++y) {
        for (uint32_t x = 32; x < mvec.width - 32; ++x) {
          const size_t i = (static_cast<size_t>(y) * mvec.width + x) * 2;
          const float vx = half_to_float(halves[i]);
          const float vy = half_to_float(halves[i + 1]);
          sum_x += vx;
          sum_y += vy;
          good += std::abs(vx - expect_x) <= 0.75F && std::abs(vy - expect_y) <= 0.75F ? 1 : 0;
          ++count;
        }
      }
      const double inlier = static_cast<double>(good) / static_cast<double>(count);
      std::printf("motion vectors: mean (%.2f, %.2f) expected (%.2f, %.2f), %.1f%% within 0.75px\n",
                  sum_x / static_cast<double>(count), sum_y / static_cast<double>(count), expect_x, expect_y,
                  inlier * 100.0);
      if (inlier < 0.9) {
        std::printf("FAIL: motion estimation is inaccurate\n");
        ++failures;
      }

      if (generated != multiplier - 1) {
        std::printf("FAIL: expected %u generated frames, got %u (%s)\n", multiplier - 1, generated,
                    generator.last_error().c_str());
        ++failures;
        break;
      }
      for (uint32_t index = 0; index < generated; ++index) {
        cmd = runner.begin();
        copy_image(cmd, generator.generated(index), readback, false);
        runner.submit_and_wait();
        const auto* out = static_cast<const uint8_t*>(readback.mapped);
        const float t = static_cast<float>(index + 1) / static_cast<float>(multiplier);
        const float base = static_cast<float>(frame - 1);
        const auto truth = make_frame(kDx * (base + t), kDy * (base + t));
        const double err_truth = interior_error(out, truth);
        const double err_prev = interior_error(out, previous);
        const double err_cur = interior_error(out, pixels);
        std::printf("generated %u/%u (t=%.2f): error vs truth %.2f, vs previous %.2f, vs current %.2f\n", index + 1,
                    generated, t, err_truth, err_prev, err_cur);
        if (!(err_truth < err_prev * 0.6 && err_truth < err_cur * 0.6)) {
          std::printf("FAIL: generated frame is not a convincing in-between frame\n");
          ++failures;
        }
      }
    }
    vk::destroy_buffer(gpu, upload);
    vk::destroy_buffer(gpu, readback);
    vk::destroy_image(gpu, source);
  }
  vk::destroy_device(device);
  std::printf(failures == 0 ? "PASS\n" : "FAILED (%d)\n", failures);
  return failures == 0 ? 0 : 1;
}
