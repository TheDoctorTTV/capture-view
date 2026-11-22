#pragma once

// Management of the NVIDIA DLSS Frame Generation runtime library, which is not
// shipped with capture-view. Users download it straight from NVIDIA's official
// DLSS SDK repository into their own data directory (the AppImage itself is
// read-only). Every file is pinned to one SDK release and verified by size and
// SHA-256 before it is used.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cv {

struct NvidiaRuntimeFile {
  std::string name;
  std::string url;
  uint64_t size = 0;
  std::string sha256;
};

inline constexpr const char* kNvidiaDlssSdkVersion = "310.9.16";
inline constexpr const char* kNvidiaDlssLicenseUrl =
    "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt";

[[nodiscard]] const std::vector<NvidiaRuntimeFile>& nvidia_runtime_files();

// $CAPTURE_VIEW_NVIDIA_DIR, else $XDG_DATA_HOME/capture-view/nvidia, else
// ~/.local/share/capture-view/nvidia.
[[nodiscard]] std::filesystem::path nvidia_runtime_dir();

struct NvidiaRuntimeStatus {
  bool installed = false;
  std::filesystem::path directory;
  std::string detail;
};

[[nodiscard]] NvidiaRuntimeStatus nvidia_runtime_status(bool verify_hash = true);

// Downloads and verifies all runtime files. `progress` receives bytes done and
// total. Throws AppError on any failure and leaves no partial file behind.
void install_nvidia_runtime(const std::function<void(uint64_t done, uint64_t total)>& progress);

// Deletes the downloaded runtime files.
void remove_nvidia_runtime();

} // namespace cv
