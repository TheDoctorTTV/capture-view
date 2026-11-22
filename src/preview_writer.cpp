#include "preview_writer.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

namespace cv {
namespace {
constexpr size_t kHeader = 64;
constexpr size_t kSlotHeader = 64;
constexpr size_t kSlots = 3;
struct alignas(64) Header {
  uint32_t magic = 0x31505643; // CVP1
  uint32_t version = 1;
  uint32_t width = 0, height = 0, stride = 0, slots = kSlots;
  uint64_t capacity = 0;
  std::atomic<uint64_t> published{0};
  // Frame rates in hundredths of a frame per second, refreshed about twice a
  // second. Older readers ignore them (they were reserved zero bytes).
  std::atomic<uint32_t> input_fps_centi{0};   // distinct captured frames
  std::atomic<uint32_t> output_fps_centi{0};  // frames published, incl. generated
  // 1 while the GUI wants preview frames ("Show video in viewer"). The GUI
  // writes it; the engine stops producing the preview copy while it is 0.
  std::atomic<uint32_t> wanted{1};
  std::byte reserved[12]{};
};
static_assert(sizeof(Header) == kHeader);
static_assert(offsetof(Header, published) == 32);
static_assert(offsetof(Header, input_fps_centi) == 40);
static_assert(offsetof(Header, output_fps_centi) == 44);
static_assert(offsetof(Header, wanted) == 48);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::endian::native == std::endian::little);
}
PreviewWriter::PreviewWriter(const std::string& path, Size size, bool wanted) : size_(size) {
  if (size.width == 0 || size.height == 0 || size.width > 16384 || size.height > 16384)
    throw AppError("invalid embedded preview dimensions");
  const uint64_t bytes = static_cast<uint64_t>(size.width) * size.height * 4;
  if (bytes > 128ULL * 1024 * 1024) throw AppError("embedded preview frame is too large");
  capacity_ = (static_cast<size_t>(bytes) + 63) & ~size_t{63};
  mapped_size_ = kHeader + kSlots * (kSlotHeader + capacity_);
  fd_.reset(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (!fd_) throw AppError(errno_message("could not create embedded preview"));
  const int allocated = posix_fallocate(fd_.get(), 0, static_cast<off_t>(mapped_size_));
  if (allocated != 0) {
    ::unlink(path.c_str());
    throw AppError("could not reserve preview memory: " + std::string(std::strerror(allocated)));
  }
  auto* mapping = ::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
  if (mapping == MAP_FAILED) { ::unlink(path.c_str()); throw AppError(errno_message("could not map embedded preview")); }
  memory_ = static_cast<std::byte*>(mapping);
  auto* header = std::construct_at(reinterpret_cast<Header*>(memory_));
  header->width = size.width; header->height = size.height; header->stride = size.width * 4;
  header->capacity = capacity_;
  header->wanted.store(wanted ? 1U : 0U, std::memory_order_relaxed);
  for (size_t i = 0; i < kSlots; ++i)
    std::construct_at(reinterpret_cast<std::atomic<uint64_t>*>(memory_ + kHeader + i * (kSlotHeader + capacity_)), uint64_t{0});
}
PreviewWriter::~PreviewWriter() { if (memory_) ::munmap(memory_, mapped_size_); }
void PreviewWriter::publish(const RgbaFrame& frame) {
  if (frame.size.width != size_.width || frame.size.height != size_.height ||
      frame.pixels.size() != static_cast<size_t>(size_.width) * size_.height * 4)
    throw AppError("embedded preview dimensions changed");
  const uint64_t sequence = ++sequence_;
  auto* slot = memory_ + kHeader + (sequence % kSlots) * (kSlotHeader + capacity_);
  auto* guard = reinterpret_cast<std::atomic<uint64_t>*>(slot);
  guard->store(sequence * 2 + 1, std::memory_order_seq_cst);
  auto* pixels = slot + kSlotHeader;
  // GPUI consumes BGRA. Convert directly into the reusable shared-memory slot.
  for (size_t i = 0; i < frame.pixels.size(); i += 4) {
    pixels[i] = frame.pixels[i + 2]; pixels[i + 1] = frame.pixels[i + 1];
    pixels[i + 2] = frame.pixels[i]; pixels[i + 3] = frame.pixels[i + 3];
  }
  guard->store(sequence * 2, std::memory_order_release);
  reinterpret_cast<Header*>(memory_)->published.store(sequence, std::memory_order_release);
}
}

namespace cv {
void PreviewWriter::publish_bgra(std::span<const std::byte> pixels) {
  if (pixels.size() != static_cast<size_t>(size_.width) * size_.height * 4)
    throw AppError("embedded preview dimensions changed");
  const uint64_t sequence = ++sequence_;
  auto* slot = memory_ + kHeader + (sequence % kSlots) * (kSlotHeader + capacity_);
  auto* guard = reinterpret_cast<std::atomic<uint64_t>*>(slot);
  guard->store(sequence * 2 + 1, std::memory_order_seq_cst);
  std::memcpy(slot + kSlotHeader, pixels.data(), pixels.size());
  guard->store(sequence * 2, std::memory_order_release);
  reinterpret_cast<Header*>(memory_)->published.store(sequence, std::memory_order_release);
}
}

namespace cv {
void PreviewWriter::set_rates(double input_fps, double output_fps) {
  auto* header = reinterpret_cast<Header*>(memory_);
  const auto centi = [](double fps) { return static_cast<uint32_t>(std::clamp(fps, 0.0, 10000.0) * 100.0 + 0.5); };
  header->input_fps_centi.store(centi(input_fps), std::memory_order_relaxed);
  header->output_fps_centi.store(centi(output_fps), std::memory_order_relaxed);
}
}

namespace cv {
bool PreviewWriter::wanted() const {
  return reinterpret_cast<const Header*>(memory_)->wanted.load(std::memory_order_relaxed) != 0;
}
}
