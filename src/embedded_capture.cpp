#include "embedded_capture.hpp"
#include "audio_monitor.hpp"
#include "h264_decoder.hpp"
#include "mjpeg_decoder.hpp"
#include "preview_writer.hpp"
#include "v4l2_output.hpp"
#include "log.hpp"
#include "test_pattern.hpp"
#include <algorithm>
#include <csignal>
#include <memory>
#include <set>
#include <thread>

namespace cv {
namespace {
volatile std::sig_atomic_t stopping = 0;
void request_stop(int) { stopping = 1; }
struct SignalScope {
  using Handler = void (*)(int);
  Handler term, interrupt;
  SignalScope() : term(std::signal(SIGTERM, request_stop)), interrupt(std::signal(SIGINT, request_stop)) { stopping = 0; }
  ~SignalScope() { std::signal(SIGTERM, term); std::signal(SIGINT, interrupt); }
};
}
int run_embedded_capture(CliOptions& options, const std::vector<CaptureConfig>& attempts) {
  SignalScope signals;
  std::unique_ptr<V4l2Capture> capture;
  if (!options.test_pattern) {
    std::string error;
    for (const auto& config : attempts) {
      try {
        auto candidate = std::make_unique<V4l2Capture>(config);
        candidate->start();
        options.video_device = config.device; options.size = candidate->size();
        options.fps = candidate->fps(); options.format = candidate->format();
        capture = std::move(candidate); break;
      } catch (const AppError& e) { error = e.what(); log::warning("capture mode unavailable: ", error); }
    }
    if (!capture) throw AppError("could not start embedded capture: " + error);
  }
  PreviewWriter preview(options.preview_file, options.size);
  auto paths = options.video_outputs;
  if (!options.video_output.empty()) paths.push_back(options.video_output);
  std::set<std::string> unique(paths.begin(), paths.end());
  std::vector<std::unique_ptr<V4l2Output>> outputs;
  for (const auto& path : unique) {
    if (path == options.video_device) throw AppError("the capture input cannot also be a virtual output");
    outputs.push_back(std::make_unique<V4l2Output>(V4l2OutputConfig{path, options.size, options.fps, options.video_output_format}));
  }
  std::unique_ptr<AudioMonitor> audio;
  if (options.audio_monitor || !options.audio_virtual_source.empty()) {
    AudioConfig config;
    config.input_name = options.audio_input == "default" ? std::string{} : options.audio_input;
    config.output_name = options.audio_output == "default" ? std::string{} : options.audio_output;
    config.virtual_source_name = options.audio_virtual_source;
    config.buffer_ms = options.audio_buffer_ms; config.quantum_frames = options.audio_quantum;
    config.delay_ms = options.audio_delay_ms; config.volume = options.volume;
    config.muted = options.muted; config.test_tone = options.audio_test_tone;
    config.playback = options.audio_monitor;
    audio = std::make_unique<AudioMonitor>(config);
    // Exporting silent audio without telling the user would hide a broken source.
    audio->start();
  }
  log::info("embedded viewer started: ", options.size.width, "x", options.size.height,
            " ", options.fps, "fps virtual_outputs=", outputs.size());
  TestPattern pattern(options.size);
  MjpegDecoder mjpeg; H264Decoder h264;
  RgbaFrame rgba;
  std::vector<std::byte> encoded;
  uint64_t frames = 0;
  auto logged = Clock::now();
  auto next_pattern = Clock::now();
  while (!stopping) {
    std::optional<FrameView> raw;
    const RgbaFrame* decoded = nullptr;
    if (options.test_pattern) { rgba = pattern.next(); decoded = &rgba; }
    else {
      raw = capture->poll_newest(2);
      if (!raw) continue;
      try {
        if (raw->format == PixelFormat::Yuyv) convert_yuyv_to_rgba(*raw, rgba);
        else if (raw->format == PixelFormat::Nv12) convert_nv12_to_rgba(*raw, rgba);
        else if (raw->format == PixelFormat::Mjpeg) mjpeg.decode(*raw, rgba);
        else if (raw->format == PixelFormat::H264) { if (!h264.decode(*raw, rgba)) continue; }
        else throw AppError("unsupported embedded capture format");
        decoded = &rgba;
      } catch (const AppError& e) { log::warning("dropped preview frame: ", e.what()); continue; }
    }
    // Publish the local preview before fan-out; slow consumers cannot queue it.
    preview.publish(*decoded);
    if (!outputs.empty()) {
      std::span<const std::byte> bytes;
      const bool raw_yuyv = raw && raw->format == PixelFormat::Yuyv && options.video_output_format == "yuyv";
      if (raw_yuyv) {
        const size_t row = static_cast<size_t>(options.size.width) * 2;
        const size_t stride = raw_frame_stride(*raw);
        if (stride == row) bytes = raw->bytes.first(row * options.size.height);
        else {
          encoded.resize(row * options.size.height);
          for (uint32_t y = 0; y < options.size.height; ++y)
            std::copy_n(raw->bytes.data() + y * stride, row, encoded.data() + y * row);
          bytes = encoded;
        }
      } else if (options.video_output_format == "mjpeg" && raw && raw->format == PixelFormat::Mjpeg) bytes = raw->bytes;
      else {
        if (options.video_output_format == "yuyv") convert_rgba_to_yuyv(*decoded, encoded);
        else if (options.video_output_format == "nv12") convert_rgba_to_nv12(*decoded, encoded);
        else if (options.video_output_format == "rgba") encoded = decoded->pixels;
        else throw AppError("embedded MJPEG export requires an MJPEG capture source");
        bytes = encoded;
      }
      for (auto& output : outputs) output->write_bytes(bytes);
    }
    ++frames;
    const auto now = Clock::now();
    if (now - logged >= std::chrono::seconds(2)) {
      const auto stats = audio ? audio->stats() : AudioStats{};
      log::info("embedded frames=", frames, " audio_input_frames=", stats.input_frames,
                " audio_output_frames=", stats.output_frames, " audio_virtual_frames=", stats.virtual_source_frames);
      logged = now;
    }
    if (options.test_pattern || options.frame_pacing == "sleep" || options.frame_pacing == "adaptive") {
      const auto interval = std::chrono::nanoseconds(1'000'000'000 / std::max(options.fps, 1U));
      next_pattern = std::max(next_pattern + interval, Clock::now());
      std::this_thread::sleep_until(next_pattern);
    } else if (options.frame_pacing == "yield") {
      std::this_thread::yield();
    }
  }
  return 0;
}
}
