#include "embedded_capture.hpp"
#include "audio_monitor.hpp"
#include "gpu_pipeline.hpp"
#include "h264_decoder.hpp"
#include "mjpeg_decoder.hpp"
#include "preview_writer.hpp"
#include "repeat_detector.hpp"
#include "v4l2_output.hpp"
#include "log.hpp"
#include "test_pattern.hpp"
#include <algorithm>
#include <csignal>
#include <deque>
#include <memory>
#include <optional>
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

double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

Clock::duration ms(double value) {
  return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(value));
}

// Tracks the interval between distinct captured frames (33 ms for a 30 fps
// game even in a 60 Hz signal) from the driver's capture timestamps.
class FrameClock {
public:
  explicit FrameClock(uint32_t fps) : interval_ms_(1000.0 / std::max(fps, 1U)) {}

  // Returns true when the gap is so long (a stall or a scene that froze) that
  // interpolating across it would smear unrelated frames.
  bool arrived(Clock::time_point when) {
    bool stale = false;
    if (last_.time_since_epoch().count() != 0) {
      const double measured = ms_between(last_, when);
      if (measured > interval_ms_ * 0.4 && measured < interval_ms_ * 2.5) {
        interval_ms_ = interval_ms_ * 0.9 + measured * 0.1;
      } else if (measured >= interval_ms_ * 2.5 && measured > 120.0) {
        stale = true;
      } else if (measured >= interval_ms_ * 2.5) {
        // The game changed frame rate (e.g. 60 -> 30 fps): adapt quickly.
        interval_ms_ = interval_ms_ * 0.5 + measured * 0.5;
      }
    }
    last_ = when;
    return stale;
  }
  [[nodiscard]] double interval_ms() const { return interval_ms_; }

private:
  double interval_ms_;
  Clock::time_point last_{};
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
  PreviewWriter preview(options.preview_file, options.size, options.show_preview);
  auto paths = options.video_outputs;
  if (!options.video_output.empty()) paths.push_back(options.video_output);
  std::set<std::string> unique(paths.begin(), paths.end());

  // GPU path: conversion, frame generation and packing for the preview and all
  // virtual cameras happen in VRAM with a single read-back per output frame.
  std::string output_format = options.video_output_format;
  if (options.frame_generation > 1 && output_format == "mjpeg") {
    log::warning("frame generation cannot pass MJPEG through; virtual cameras use YUYV instead");
    output_format = "yuyv";
  }
  std::unique_ptr<GpuPipeline> gpu;
  try {
    GpuPipelineOptions gpu_options;
    gpu_options.size = options.size;
    gpu_options.frame_generation = options.frame_generation;
    // No preview copy at all while "Show video in viewer" is off.
    gpu_options.preview = options.show_preview;
    gpu_options.output_format = unique.empty() || output_format == "mjpeg" ? std::string{} : output_format;
    gpu = std::make_unique<GpuPipeline>(gpu_options);
  } catch (const AppError& e) {
    log::warning("GPU pipeline unavailable, using the CPU path: ", e.what());
    if (options.frame_generation > 1) log::warning("frame generation is off because the GPU pipeline is unavailable");
  }
  const uint32_t multiplier = gpu ? gpu->expected_frame_generation() : 1U;
  if (gpu && options.frame_generation > 1) {
    if (multiplier > 1) log::info("frame generation ", multiplier, "x active");
    else log::warning("frame generation off: ", gpu->frame_generation_note());
  }

  std::vector<std::unique_ptr<V4l2Output>> outputs;
  for (const auto& path : unique) {
    if (path == options.video_device) throw AppError("the capture input cannot also be a virtual output");
    outputs.push_back(std::make_unique<V4l2Output>(
        V4l2OutputConfig{path, options.size, options.fps * multiplier, output_format}));
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
            " ", options.fps, "fps virtual_outputs=", outputs.size(), gpu ? " gpu" : " cpu");
  TestPattern pattern(options.size);
  MjpegDecoder mjpeg; H264Decoder h264;
  RgbaFrame rgba;
  std::vector<std::byte> encoded;
  uint64_t frames = 0;
  uint64_t output_frames = 0;
  uint64_t dropped_generated = 0;
  uint64_t stale_resets = 0;
  auto logged = Clock::now();
  auto next_pattern = Clock::now();
  const bool compressed = options.format == PixelFormat::Mjpeg || options.format == PixelFormat::H264;
  RepeatDetector repeats(compressed ? 2.0 : 0.75);
  FrameClock frame_clock(options.fps);
  bool reset = true;

  // Frame rates shown in the GUI: input counts distinct captured frames (so a
  // 30 fps game in a 60 Hz signal reads 30), output counts every published frame.
  uint64_t distinct_frames = 0;
  uint64_t distinct_at_rate = 0;
  uint64_t output_at_rate = 0;
  auto rate_window = Clock::now();
  const auto update_rates = [&]() {
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - rate_window).count();
    if (seconds < 0.5) return;
    preview.set_rates(static_cast<double>(distinct_frames - distinct_at_rate) / seconds,
                      static_cast<double>(output_frames - output_at_rate) / seconds);
    distinct_at_rate = distinct_frames;
    output_at_rate = output_frames;
    rate_window = now;
  };
  const auto is_repeat = [&](const std::optional<FrameView>& raw_frame, const RgbaFrame* decoded_frame) {
    if (raw_frame && (raw_frame->format == PixelFormat::Yuyv || raw_frame->format == PixelFormat::Nv12)) {
      return repeats.is_repeat(
          raw_frame->bytes.data(), raw_frame_stride(*raw_frame),
          static_cast<size_t>(raw_frame->size.width) * (raw_frame->format == PixelFormat::Yuyv ? 2U : 1U),
          raw_frame->size.height);
    }
    if (decoded_frame == nullptr) return false;
    return repeats.is_repeat(decoded_frame->pixels.data(), static_cast<size_t>(decoded_frame->size.width) * 4,
                             static_cast<size_t>(decoded_frame->size.width) * 4, decoded_frame->size.height);
  };
  const auto log_stats = [&]() {
    const auto now = Clock::now();
    if (now - logged < std::chrono::seconds(2)) return;
    const auto stats = audio ? audio->stats() : AudioStats{};
    log::info("embedded frames=", frames, " output_frames=", output_frames,
              " frame_generation=", gpu ? gpu->active_frame_generation() : 1U,
              " interval_ms=", frame_clock.interval_ms(), " repeats_skipped=", repeats.repeats(),
              " generated_dropped=", dropped_generated, " stale_resets=", stale_resets,
              " gpu_ms=", gpu ? gpu->gpu_ms() : 0.0,
              " audio_input_frames=", stats.input_frames,
              " audio_output_frames=", stats.output_frames, " audio_virtual_frames=", stats.virtual_source_frames);
    logged = now;
  };

  if (gpu) {
    // ---------------------------------------------------------------- GPU
    // The GPU works on the newest captured frame while the frames of the
    // previous batch are still being published on an even cadence, so GPU time
    // never adds to the time between captured frames.
    struct Batch {
      uint32_t slot = 0;
      uint32_t count = 0;
      uint32_t next = 0;
      bool has_preview = false; // the preview copy existed when it was submitted
    };
    struct InFlight {
      uint32_t slot = 0;
      bool has_preview = false;
    };
    std::deque<InFlight> busy; // submitted, GPU still running (oldest first)
    std::optional<Batch> ready; // finished, frames being published
    Clock::time_point last_out{};
    double step_ms = frame_clock.interval_ms();
    const bool mjpeg_passthrough = output_format == "mjpeg";

    const auto publish = [&](const Batch& batch, uint32_t index) {
      if (batch.has_preview && gpu->preview_enabled()) {
        preview.publish_bgra(gpu->preview(batch.slot, index));
      }
      if (!outputs.empty() && !mjpeg_passthrough) {
        const auto bytes = gpu->output(batch.slot, index);
        for (auto& output : outputs) output->write_bytes(bytes);
      }
      ++output_frames;
    };
    // Publishes the batch's real frame right away (dropping generated frames
    // that missed their slot) so frames never go out of order.
    const auto flush_ready = [&]() {
      if (!ready) return;
      if (ready->next < ready->count) {
        dropped_generated += ready->count - 1 - std::min(ready->next, ready->count - 1);
        publish(*ready, ready->count - 1);
        last_out = Clock::now();
      }
      ready.reset();
    };
    // Publishes every frame whose time has come; returns when the next is due.
    const auto publish_due = [&]() -> std::optional<Clock::time_point> {
      while (ready && ready->next < ready->count) {
        const auto now = Clock::now();
        const uint32_t index = ready->next;
        const bool real = index + 1 == ready->count;
        // Without generated frames there is nothing to space out: show at once.
        const auto target = last_out.time_since_epoch().count() == 0 || ready->count == 1 ? now : last_out + ms(step_ms);
        if (!real && now > target + ms(step_ms)) {
          ++dropped_generated; // far behind: skip this generated frame
          ++ready->next;
          last_out = target;
          continue;
        }
        if (now < target) return target;
        publish(*ready, index);
        last_out = now - target > ms(step_ms * 0.5) ? now : target;
        ++ready->next;
      }
      ready.reset();
      return std::nullopt;
    };
    const auto free_slot = [&]() -> uint32_t {
      for (uint32_t slot = 0; slot < GpuPipeline::kSlots; ++slot) {
        const bool in_flight = std::ranges::any_of(busy, [&](const InFlight& f) { return f.slot == slot; });
        if (!in_flight && !(ready && ready->slot == slot)) return slot;
      }
      return GpuPipeline::kSlots;
    };

    while (!stopping) {
      update_rates();
      log_stats();
      // 0. Follow the GUI's "Show video in viewer" toggle: without it, no
      //    preview copy exists in VRAM or system memory.
      if (preview.wanted() != gpu->preview_enabled()) {
        gpu->set_preview(preview.wanted());
      }
      // 1. Collect a finished GPU batch.
      if (!busy.empty() && gpu->wait(busy.front().slot, std::chrono::nanoseconds(0))) {
        flush_ready();
        const uint32_t count = gpu->frame_count(busy.front().slot);
        ready = Batch{busy.front().slot, count, 0, busy.front().has_preview};
        busy.pop_front();
        // Spread this batch over one capture interval, slightly faster so
        // output never builds up a backlog behind the capture.
        step_ms = frame_clock.interval_ms() * 0.97 / static_cast<double>(count);
      }
      // 2. Publish frames that are due.
      const auto due = publish_due();
      auto limit = ms(2.0);
      if (due) limit = std::max(Clock::duration::zero(), std::min(limit, *due - Clock::now()));
      // 3. Wait for the GPU if it is full, otherwise look for a new frame.
      if (busy.size() >= 2) {
        gpu->wait(busy.front().slot, std::chrono::duration_cast<std::chrono::nanoseconds>(limit));
        continue;
      }
      std::optional<FrameView> raw;
      const RgbaFrame* decoded = nullptr;
      if (options.test_pattern) {
        if (Clock::now() < next_pattern) {
          std::this_thread::sleep_for(std::min(limit, next_pattern - Clock::now()));
          continue;
        }
        next_pattern = std::max(next_pattern + ms(1000.0 / std::max(options.fps, 1U)), Clock::now());
        rgba = pattern.next();
        decoded = &rgba;
      } else {
        const int timeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(limit).count());
        raw = capture->poll_newest(busy.empty() ? timeout : std::min(timeout, 1));
        if (!raw) continue;
      }
      const bool raw_input = raw && (raw->format == PixelFormat::Yuyv || raw->format == PixelFormat::Nv12);
      try {
        if (raw && !raw_input) {
          if (raw->format == PixelFormat::Mjpeg) mjpeg.decode(*raw, rgba);
          else if (raw->format == PixelFormat::H264) { if (!h264.decode(*raw, rgba)) continue; }
          else throw AppError("unsupported embedded capture format");
          decoded = &rgba;
        }
      } catch (const AppError& e) { log::warning("dropped preview frame: ", e.what()); continue; }

      // A 30 fps game in a 60 Hz signal repeats every frame: with frame
      // generation on, interpolate between the distinct frames only.
      const bool repeat = is_repeat(raw, decoded);
      if (!repeats.identical()) ++distinct_frames;
      if (multiplier > 1 && repeat) continue;
      ++frames;

      if (mjpeg_passthrough && !outputs.empty()) {
        if (!raw || raw->format != PixelFormat::Mjpeg)
          throw AppError("embedded MJPEG export requires an MJPEG capture source");
        for (auto& output : outputs) output->write_bytes(raw->bytes);
      }
      const auto arrival = raw && raw->captured_at.time_since_epoch().count() != 0 ? raw->captured_at : Clock::now();
      if (frame_clock.arrived(arrival)) { reset = true; ++stale_resets; }

      const uint32_t slot = free_slot();
      try {
        if (raw_input) gpu->submit(slot, *raw, reset);
        else gpu->submit(slot, *decoded, reset);
        reset = false;
        busy.push_back(InFlight{slot, gpu->preview_enabled()});
      } catch (const AppError& e) { log::warning("GPU frame failed: ", e.what()); reset = true; }
    }
    return 0;
  }

  // ------------------------------------------------------------------ CPU
  while (!stopping) {
    update_rates();
    log_stats();
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
    (void)is_repeat(raw, decoded);
    if (!repeats.identical()) ++distinct_frames;
    // Publish the local preview before fan-out; slow consumers cannot queue it.
    if (preview.wanted()) preview.publish(*decoded);
    if (!outputs.empty()) {
      std::span<const std::byte> bytes;
      const bool raw_yuyv = raw && raw->format == PixelFormat::Yuyv && output_format == "yuyv";
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
      } else if (output_format == "mjpeg" && raw && raw->format == PixelFormat::Mjpeg) bytes = raw->bytes;
      else {
        if (output_format == "yuyv") convert_rgba_to_yuyv(*decoded, encoded);
        else if (output_format == "nv12") convert_rgba_to_nv12(*decoded, encoded);
        else if (output_format == "rgba") encoded = decoded->pixels;
        else throw AppError("embedded MJPEG export requires an MJPEG capture source");
        bytes = encoded;
      }
      for (auto& output : outputs) output->write_bytes(bytes);
    }
    ++output_frames;
    ++frames;
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
