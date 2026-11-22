#include "gui_bridge.hpp"
#include "audio_devices.hpp"
#include "config.hpp"
#include "v4l2_discovery.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace cv {
namespace {
std::string json(const std::string& value) {
  std::string result = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
    else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
    else result += static_cast<char>(c);
  }
  return result + "\"";
}
}

int write_gui_info(const CliOptions& o) {
  const auto videos = list_video_devices();
  std::vector<AudioDeviceInfo> audio;
  std::string audio_error;
  try { audio = list_audio_devices(); } catch (const AppError& e) { audio_error = e.what(); }
  std::ostringstream out;
  out << "{\"config_path\":" << json(config_file_path())
      << ",\"audio_error\":" << json(audio_error) << ",\"arguments\":[";
  bool first = true;
  for (const auto& arg : viewer_arguments(o)) {
    if (!first) out << ',';
    first = false;
    out << json(arg);
  }
  out << "],\"settings\":{\"video\":" << json(o.video_device)
      << ",\"virtual_devices\":" << o.virtual_devices
      << ",\"width\":" << o.size.width << ",\"height\":" << o.size.height
      << ",\"fps\":" << o.fps << ",\"format\":" << json(to_string(o.format))
      << ",\"audio_input\":" << json(o.audio_input.empty() ? "default" : o.audio_input)
      << ",\"audio_output\":" << json(o.audio_output.empty() ? "default" : o.audio_output)
      << ",\"system_tray\":" << (o.system_tray ? "true" : "false")
      << ",\"show_preview\":" << (o.show_preview ? "true" : "false")
      << ",\"audio_monitor\":" << (o.audio_monitor ? "true" : "false")
      << ",\"renderer\":" << json(o.render_backend)
      << ",\"latency\":" << json(o.latency_mode.empty() ? "ultra" : o.latency_mode)
      << ",\"pacing\":" << json(o.frame_pacing)
      << ",\"vsync\":" << (o.vsync ? "true" : "false")
      << ",\"fullscreen\":" << (o.fullscreen ? "true" : "false")
      << ",\"borderless\":" << (o.borderless ? "true" : "false")
      << ",\"muted\":" << (o.muted ? "true" : "false")
      << ",\"volume\":" << o.volume << "},\"video_devices\":[";
  first = true;
  for (const auto& device : videos) {
    if (device.driver == "v4l2 loopback" && device.card.starts_with("Capture View ")) continue;
    if (!first) out << ',';
    first = false;
    out << "{\"path\":" << json(device.path) << ",\"name\":" << json(device.card) << ",\"modes\":[";
    bool first_mode = true;
    for (const auto& format : device.formats) {
      const auto pixel_format = pixel_format_from_v4l2(format.fourcc);
      if (pixel_format == PixelFormat::Unknown) continue;
      for (const auto& size : format.sizes) {
        for (const auto& interval : size.intervals) {
          if (interval.numerator == 0 || interval.denominator / interval.numerator == 0) continue;
          if (!first_mode) out << ',';
          first_mode = false;
          out << "{\"width\":" << size.size.width << ",\"height\":" << size.size.height
              << ",\"fps\":" << interval.denominator / interval.numerator
              << ",\"format\":" << json(to_string(pixel_format)) << '}';
        }
      }
    }
    out << "]}";
  }
  out << "],\"audio_devices\":[";
  first = true;
  for (const auto& device : audio) {
    if (!first) out << ',';
    first = false;
    out << "{\"name\":" << json(device.name) << ",\"label\":" << json(audio_device_display_name(device))
        << ",\"kind\":" << json(device.media_class) << '}';
  }
  out << "]}\n";
  std::cout << out.str();
  return 0;
}

int run_gpui_ui(const CliOptions& options) {
  const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path() / "capture-view-gui";
  auto args = viewer_arguments(options);
  args.insert(args.begin(), executable.string());
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) throw AppError("could not launch the GPUI interface");
  if (child == 0) {
    execv(executable.c_str(), argv.data());
    std::cerr << "Could not start " << executable << ": " << std::strerror(errno) << '\n';
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) throw AppError("could not wait for the GPUI interface");
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
}
