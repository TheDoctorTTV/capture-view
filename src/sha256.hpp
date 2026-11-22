#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace cv {

class Sha256 {
public:
  Sha256();
  void update(std::span<const std::byte> data);
  [[nodiscard]] std::string hex_digest();

private:
  void transform(const uint8_t* block);

  uint32_t state_[8]{};
  uint8_t buffer_[64]{};
  uint64_t length_ = 0;
  size_t buffered_ = 0;
};

[[nodiscard]] std::string sha256_hex(std::span<const std::byte> data);
[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& path);

} // namespace cv
