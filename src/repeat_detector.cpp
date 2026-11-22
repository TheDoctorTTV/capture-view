#include "repeat_detector.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace cv {

namespace {
constexpr size_t kColumns = 96;
constexpr size_t kRows = 54;
} // namespace

bool RepeatDetector::is_repeat(const std::byte* bytes, size_t stride, size_t row_bytes, size_t rows) {
  if (bytes == nullptr || row_bytes < 4 || rows == 0) {
    reset();
    return false;
  }
  current_.resize(kColumns * kRows);
  for (size_t r = 0; r < kRows; ++r) {
    const size_t y = (r * 2 + 1) * rows / (kRows * 2);
    const std::byte* line = bytes + y * stride;
    for (size_t c = 0; c < kColumns; ++c) {
      size_t x = (c * 2 + 1) * row_bytes / (kColumns * 2);
      x = std::min(x & ~size_t{3}, row_bytes - 4);
      uint32_t word = 0;
      std::memcpy(&word, line + x, sizeof(word));
      current_[r * kColumns + c] = word;
    }
  }

  bool identical = false;
  if (previous_.size() == current_.size()) {
    uint64_t difference = 0;
    for (size_t i = 0; i < current_.size(); ++i) {
      const uint32_t a = current_[i];
      const uint32_t b = previous_[i];
      for (int shift = 0; shift < 32; shift += 8) {
        difference += static_cast<uint64_t>(std::abs(static_cast<int>((a >> shift) & 0xFFU) -
                                                     static_cast<int>((b >> shift) & 0xFFU)));
      }
    }
    const double mean = static_cast<double>(difference) / static_cast<double>(current_.size() * 4);
    identical = mean <= tolerance_;
  }
  identical_ = identical;
  const bool repeat = identical && !last_was_repeat_;
  last_was_repeat_ = repeat;
  if (repeat) {
    ++repeats_;
  } else {
    previous_.swap(current_);
  }
  return repeat;
}

void RepeatDetector::reset() {
  previous_.clear();
  last_was_repeat_ = false;
  identical_ = false;
}

} // namespace cv
