#include "nvidia_runtime.hpp"

#include "common.hpp"
#include "log.hpp"
#include "sha256.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace cv {

namespace {

constexpr const char* kDlssCommit = "9981c6350dc58c0ce21f024fd65e31e851c31689";

std::string raw_url(const std::string& path) {
  return std::string("https://raw.githubusercontent.com/NVIDIA/DLSS/") + kDlssCommit + "/" + path;
}

struct DownloadState {
  std::FILE* file = nullptr;
  Sha256 hash;
  uint64_t written = 0;
  uint64_t expected = 0;
  uint64_t base = 0;
  uint64_t total = 0;
  const std::function<void(uint64_t, uint64_t)>* progress = nullptr;
};

size_t write_chunk(char* data, size_t size, size_t count, void* user) {
  auto* state = static_cast<DownloadState*>(user);
  const size_t bytes = size * count;
  if (state->written + bytes > state->expected) {
    return 0; // larger than the pinned file: abort
  }
  if (std::fwrite(data, 1, bytes, state->file) != bytes) {
    return 0;
  }
  state->hash.update(std::as_bytes(std::span(data, bytes)));
  state->written += bytes;
  if (state->progress != nullptr && *state->progress) {
    (*state->progress)(state->base + state->written, state->total);
  }
  return bytes;
}

void download_file(const NvidiaRuntimeFile& entry, const std::filesystem::path& destination, uint64_t base,
                   uint64_t total, const std::function<void(uint64_t, uint64_t)>& progress) {
  const std::filesystem::path partial = destination.string() + ".part";
  DownloadState state;
  state.expected = entry.size;
  state.base = base;
  state.total = total;
  state.progress = &progress;
  state.file = std::fopen(partial.c_str(), "wb");
  if (state.file == nullptr) {
    throw AppError("could not write " + partial.string());
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    std::fclose(state.file);
    std::filesystem::remove(partial);
    throw AppError("could not start the download (libcurl unavailable)");
  }
  char error_buffer[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(curl, CURLOPT_URL, entry.url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "capture-view");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_chunk);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  const CURLcode code = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  const bool flushed = std::fclose(state.file) == 0;

  std::string failure;
  if (code != CURLE_OK) {
    failure = std::string("download of ") + entry.name + " failed: " +
              (error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code));
  } else if (!flushed) {
    failure = "could not finish writing " + partial.string();
  } else if (state.written != entry.size) {
    failure = entry.name + " has an unexpected size (" + std::to_string(state.written) + " bytes, expected " +
              std::to_string(entry.size) + ")";
  } else if (const std::string digest = state.hash.hex_digest(); digest != entry.sha256) {
    failure = entry.name + " failed verification (SHA-256 " + digest + " does not match the pinned release)";
  }
  if (!failure.empty()) {
    std::error_code ignored;
    std::filesystem::remove(partial, ignored);
    throw AppError(failure);
  }
  std::filesystem::rename(partial, destination);
}

} // namespace

const std::vector<NvidiaRuntimeFile>& nvidia_runtime_files() {
  static const std::vector<NvidiaRuntimeFile> files{
      {"libnvidia-ngx-dlssg.so.310.9.16", raw_url("lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16"), 7391232,
       "574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7"},
      {"NVIDIA-RTX-SDK-LICENSE.txt", raw_url("LICENSE.txt"), 26620,
       "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"},
  };
  return files;
}

std::filesystem::path nvidia_runtime_dir() {
  if (const char* custom = std::getenv("CAPTURE_VIEW_NVIDIA_DIR"); custom != nullptr && custom[0] != '\0') {
    return custom;
  }
  if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] == '/') {
    return std::filesystem::path(data) / "capture-view" / "nvidia";
  }
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home != nullptr ? home : ".") / ".local" / "share" / "capture-view" / "nvidia";
}

NvidiaRuntimeStatus nvidia_runtime_status(bool verify_hash) {
  NvidiaRuntimeStatus status;
  status.directory = nvidia_runtime_dir();
  for (const auto& entry : nvidia_runtime_files()) {
    const auto path = status.directory / entry.name;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
      status.detail = entry.name + " is not downloaded";
      return status;
    }
    if (size != entry.size) {
      status.detail = entry.name + " is incomplete or a different version";
      return status;
    }
    if (verify_hash) {
      const auto digest = sha256_file(path);
      if (!digest || *digest != entry.sha256) {
        status.detail = entry.name + " failed verification";
        return status;
      }
    }
  }
  status.installed = true;
  status.detail = std::string("DLSS Frame Generation runtime ") + kNvidiaDlssSdkVersion + " installed";
  return status;
}

void install_nvidia_runtime(const std::function<void(uint64_t, uint64_t)>& progress) {
  const auto directory = nvidia_runtime_dir();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw AppError("could not create " + directory.string() + ": " + error.message());
  }
  uint64_t total = 0;
  for (const auto& entry : nvidia_runtime_files()) {
    total += entry.size;
  }
  static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  if (!curl_ready) {
    throw AppError("could not initialise libcurl");
  }
  uint64_t done = 0;
  for (const auto& entry : nvidia_runtime_files()) {
    const auto destination = directory / entry.name;
    const auto existing = sha256_file(destination);
    if (!existing || *existing != entry.sha256) {
      log::info("downloading ", entry.url);
      download_file(entry, destination, done, total, progress);
    }
    done += entry.size;
    if (progress) {
      progress(done, total);
    }
  }
  log::info("NVIDIA runtime installed in ", directory.string());
}

void remove_nvidia_runtime() {
  const auto directory = nvidia_runtime_dir();
  for (const auto& entry : nvidia_runtime_files()) {
    std::error_code error;
    std::filesystem::remove(directory / entry.name, error);
  }
}

} // namespace cv
