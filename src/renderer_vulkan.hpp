#pragma once

// Native Vulkan presentation for Linux: SDL only provides the window and
// input. Raw YUYV/NV12 (and decoded RGBA) frames are converted to RGB on the
// GPU, optionally passed through NVIDIA DLSS Frame Generation, and presented
// with even spacing between real and generated frames.

#include "frame.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <vector>

namespace cv {

struct RenderStats {
  double upload_ms = 0.0;
  double present_ms = 0.0;
  // Frames shown per second including generated ones.
  double output_fps = 0.0;
  uint64_t generated_presented = 0;
  uint64_t generated_skipped = 0;
};

struct RendererOptions {
  std::string title;
  Size size;
  bool fullscreen = false;
  bool borderless = false;
  bool vsync = false;
  // 1 = off, 2..6 = output frames per captured frame.
  uint32_t frame_generation = 1;
  uint32_t source_fps = 60;
};

class VulkanRenderer {
public:
  explicit VulkanRenderer(const RendererOptions& options);
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  bool handle_events(bool& restart_requested, bool& audio_restart_requested, bool& mute_requested, float& volume_delta);
  void render(const RgbaFrame& frame, const std::string& stats_text, Clock::time_point captured_at = {});
  void render(FrameView frame, const std::string& stats_text);
  void set_gui_lines(std::vector<std::string> lines);
  void set_vsync(bool enabled);
  // Restarts the generator history, e.g. after the capture was restarted.
  void reset_frame_generation();

  [[nodiscard]] bool show_stats() const;
  [[nodiscard]] bool show_gui() const;
  [[nodiscard]] bool vsync() const;
  [[nodiscard]] bool fullscreen() const;
  [[nodiscard]] bool borderless() const;
  [[nodiscard]] RenderStats last_stats() const;
  // Requested and currently effective multipliers (1 = off).
  [[nodiscard]] uint32_t frame_generation() const;
  [[nodiscard]] uint32_t active_frame_generation() const;
  // Short human-readable state, e.g. "4x" or "off (runtime not downloaded)".
  [[nodiscard]] std::string frame_generation_status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cv
