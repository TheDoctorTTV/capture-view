#pragma once
#include "frame.hpp"
#include "v4l2_util.hpp"
#include <atomic>
#include <span>
#include <string>

namespace cv {
// Fixed little-endian IPC layout, also described in gui/src/preview.rs.
// Publication is last, so consumers can discard an overwritten slot without queuing.
class PreviewWriter {
public:
  // `wanted`: whether the GUI shows the preview initially (--show-preview).
  PreviewWriter(const std::string& path, Size size, bool wanted = true);
  ~PreviewWriter();
  PreviewWriter(const PreviewWriter&) = delete;
  PreviewWriter& operator=(const PreviewWriter&) = delete;
  void publish(const RgbaFrame& frame);
  // Publishes a frame that is already BGRA (e.g. packed on the GPU).
  void publish_bgra(std::span<const std::byte> pixels);
  // Input = distinct game frames per second (repeats of a 30 fps game in a
  // 60 Hz signal do not count); output = frames shown, including generated.
  void set_rates(double input_fps, double output_fps);
  // False while the GUI has "Show video in viewer" off; then no preview frames
  // should be produced at all.
  [[nodiscard]] bool wanted() const;
private:
  UniqueFd fd_;
  std::byte* memory_ = nullptr;
  size_t mapped_size_ = 0;
  size_t capacity_ = 0;
  Size size_;
  uint64_t sequence_ = 0;
};
}
