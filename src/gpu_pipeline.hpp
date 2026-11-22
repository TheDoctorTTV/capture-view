#pragma once

// Headless GPU video pipeline used when capture-view runs behind the settings
// GUI. One captured frame goes in; everything else stays in VRAM:
//
//   upload -> colour conversion -> [DLSS Frame Generation] -> pack
//
// The pack step writes every output frame (generated frames first, the real
// frame last) directly into host-readable buffers, once in BGRA for the GUI
// preview and once in the virtual cameras' format. That single read-back is
// shared by the preview and all virtual cameras.
//
// Work is double-buffered: while the CPU is still publishing the frames of one
// batch (spaced over the capture interval), the GPU already processes the next
// captured frame. Each submit() returns a slot; the caller waits for it with
// finish() and reads its frames until it submits into that slot again.

#include "frame.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace cv {

struct GpuPipelineOptions {
  Size size;
  uint32_t frame_generation = 1; // 1 = off
  bool preview = true;
  // "yuyv", "nv12", "rgba" or "" for no virtual camera output.
  std::string output_format;
};

class GpuPipeline {
public:
  // One batch being published, up to two queued on the GPU.
  static constexpr uint32_t kSlots = 3;

  explicit GpuPipeline(const GpuPipelineOptions& options);
  ~GpuPipeline();

  GpuPipeline(const GpuPipeline&) = delete;
  GpuPipeline& operator=(const GpuPipeline&) = delete;

  // Copies the frame and starts GPU work in `slot` without waiting. The slot's
  // previous results must no longer be needed.
  void submit(uint32_t slot, const RgbaFrame& frame, bool reset);
  void submit(uint32_t slot, FrameView frame, bool reset);
  // Waits up to `timeout` for the slot's GPU work. Returns false on timeout.
  bool wait(uint32_t slot, std::chrono::nanoseconds timeout);
  // After wait() succeeded: number of output frames (generated + 1 real).
  [[nodiscard]] uint32_t frame_count(uint32_t slot) const;

  // Synchronous convenience: submit into slot 0 and wait.
  uint32_t process(const RgbaFrame& frame, bool reset);
  uint32_t process(FrameView frame, bool reset);

  // BGRA, width * height * 4 bytes. Valid until the slot is submitted again.
  [[nodiscard]] std::span<const std::byte> preview(uint32_t slot, uint32_t index) const;
  // Virtual camera bytes in the configured format.
  [[nodiscard]] std::span<const std::byte> output(uint32_t slot, uint32_t index) const;
  [[nodiscard]] std::span<const std::byte> preview(uint32_t index) const { return preview(0, index); }
  [[nodiscard]] std::span<const std::byte> output(uint32_t index) const { return output(0, index); }

  // Turns the BGRA preview copy on or off. Off frees its VRAM and read-back
  // buffers; preview() then returns empty spans. Waits for in-flight GPU work.
  void set_preview(bool enabled);
  [[nodiscard]] bool preview_enabled() const;

  // Effective multiplier (1 when frame generation is off or unavailable) and a
  // short explanation when it is not what was asked for.
  [[nodiscard]] uint32_t active_frame_generation() const;
  // Multiplier that will be used once frames flow (known after construction).
  [[nodiscard]] uint32_t expected_frame_generation() const;
  [[nodiscard]] const std::string& frame_generation_note() const;
  // GPU execution time of the last finished batch, measured with timestamps.
  [[nodiscard]] double gpu_ms() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cv
