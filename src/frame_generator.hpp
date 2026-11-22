#pragma once

// NVIDIA DLSS Frame Generation for a captured 2D video stream.
//
// Approach (inspired by Veyra-NRVideo's Windows implementation): DLSS-G is
// designed for games, so the video frame is presented to it as a game frame
// rendered by a stationary camera. Depth is a constant plane and all motion is
// carried by per-pixel motion vectors that capture-view estimates itself with a
// coarse-to-fine GPU block matcher (shaders/flow_*.comp). DLSS-G then refines
// that motion with its own optical flow and synthesises 1-5 frames between
// each pair of captured frames.

#include "vk_util.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cv {

struct FrameGenerationCaps {
  bool available = false;
  // Highest output multiplier the driver allows: 2 means 2x only, 4 means up to
  // 4x and so on. 1 when unavailable.
  uint32_t max_multiplier = 1;
  std::string reason;
};

// Extension discovery for instance/device creation (no-ops when the build has
// no NGX support or NGX is unavailable).
void frame_generation_instance_extensions(std::vector<std::string>& out);
bool frame_generation_device_extensions(VkInstance instance, VkPhysicalDevice physical,
                                        std::vector<std::string>& out, std::string& reason);

class FrameGenerator {
public:
  explicit FrameGenerator(const vk::Gpu& gpu);
  ~FrameGenerator();

  FrameGenerator(const FrameGenerator&) = delete;
  FrameGenerator& operator=(const FrameGenerator&) = delete;

  [[nodiscard]] const FrameGenerationCaps& caps() const { return caps_; }

  // Prepares images and the DLSS-G feature for frames of this size, with room
  // for `multiplier - 1` generated frames (only those are allocated in VRAM).
  // Records one-time initialisation into `cmd`. Returns false if frame
  // generation cannot run (reason in last_error()).
  bool configure(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t multiplier);

  // Records motion estimation and `multiplier - 1` DLSS-G evaluations for the
  // real frame `current` (RGBA8, GENERAL layout). Returns the number of
  // generated frames ready for presentation (0 after a reset or on the first
  // frame). Generated frame i lies at time (i + 1) / multiplier between the
  // previous and the current real frame.
  uint32_t record(VkCommandBuffer cmd, const vk::Image& current, uint32_t multiplier, bool reset);

  [[nodiscard]] const vk::Image& generated(uint32_t index) const { return generated_[index]; }
  [[nodiscard]] const std::string& last_error() const { return last_error_; }
  // Motion vectors handed to DLSS-G for the last recorded frame (RG16F,
  // previous - current, in pixels of this texture). For tests and debugging.
  [[nodiscard]] const vk::Image& motion_vectors() const;

  // Generated frames for a source of this frame rate stop being useful past the
  // display refresh rate; exposed for the stats overlay.
  static constexpr uint32_t kMaxMultiplier = 6;

private:
  struct Impl;
  void release_size_dependent();

  const vk::Gpu& gpu_;
  FrameGenerationCaps caps_;
  std::string last_error_;
  std::unique_ptr<Impl> impl_;
  std::array<vk::Image, kMaxMultiplier - 1> generated_{};
};

// Creates a temporary headless Vulkan device, initialises NGX and reports what
// frame generation this machine supports. Used by the settings UI.
FrameGenerationCaps probe_frame_generation();

} // namespace cv
