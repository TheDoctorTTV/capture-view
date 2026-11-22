#pragma once
#include "frame.hpp"
#include "v4l2_util.hpp"
#include <atomic>
#include <string>

namespace cv {
// Fixed little-endian IPC layout, also described in gui/src/preview.rs.
// Publication is last, so consumers can discard an overwritten slot without queuing.
class PreviewWriter {
public:
  PreviewWriter(const std::string& path, Size size);
  ~PreviewWriter();
  PreviewWriter(const PreviewWriter&) = delete;
  PreviewWriter& operator=(const PreviewWriter&) = delete;
  void publish(const RgbaFrame& frame);
private:
  UniqueFd fd_;
  std::byte* memory_ = nullptr;
  size_t mapped_size_ = 0;
  size_t capacity_ = 0;
  Size size_;
  uint64_t sequence_ = 0;
};
}
