#pragma once

// Detects captured frames that repeat the previous one. Consoles running a
// game at 30 fps still send a 60 Hz signal, so every game frame arrives twice.
// Frame generation must interpolate between *different* frames, so the second
// copy is dropped and the generator spaces its output over the real 33 ms gap.
//
// Cheap: compares a sparse grid of 32-bit words, no GPU round trip. At most
// one frame in a row is reported as a repeat, so a static picture (menus,
// pauses) still produces a steady stream.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cv {

class RepeatDetector {
public:
  // `tolerance` is the mean absolute byte difference still counted as equal:
  // near zero for raw YUYV/NV12, a little higher for decoded MJPEG/H.264.
  explicit RepeatDetector(double tolerance = 0.75) : tolerance_(tolerance) {}

  // `bytes` holds `rows` rows of `row_bytes` valid bytes, `stride` apart.
  bool is_repeat(const std::byte* bytes, size_t stride, size_t row_bytes, size_t rows);
  void reset();

  [[nodiscard]] uint64_t repeats() const { return repeats_; }
  // Whether the last frame matched the previous one, ignoring the "at most one
  // in a row" rule. Used to measure the real game frame rate.
  [[nodiscard]] bool identical() const { return identical_; }

private:
  double tolerance_;
  std::vector<uint32_t> previous_;
  std::vector<uint32_t> current_;
  bool last_was_repeat_ = false;
  bool identical_ = false;
  uint64_t repeats_ = 0;
};

} // namespace cv
