#include "app.hpp"
#include "cli.hpp"
#include "common.hpp"
#include "config.hpp"
#include "log.hpp"

#include <SDL3/SDL_main.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

// The engine the GUI starts logs to stderr, which only the GUI's Console page
// sees. Also keep the latest run in a file for troubleshooting:
// $XDG_STATE_HOME/capture-view/engine.log (default ~/.local/state/...).
std::string default_engine_log() {
  std::filesystem::path dir;
  if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && state[0] == '/') {
    dir = std::filesystem::path(state) / "capture-view";
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    dir = std::filesystem::path(home) / ".local" / "state" / "capture-view";
  } else {
    return {};
  }
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  if (error) {
    return {};
  }
  const auto path = dir / "engine.log";
  // Keep the previous run as engine.log.1 so two runs can be compared.
  std::filesystem::rename(path, dir / "engine.log.1", error);
  std::ofstream truncate(path, std::ios::trunc); // one run per file
  return truncate ? path.string() : std::string{};
}

} // namespace

int main(int argc, char** argv) {
  try {
    const cv::CliOptions cli_only = cv::parse_cli(argc, argv);
    cv::CliOptions merged;
    merged.no_config = cli_only.no_config;
    merged.no_load_config = cli_only.no_load_config;
    merged.profile = cli_only.profile;
    merged.config_file = cli_only.config_file;
    cv::set_config_location(merged);
    cv::load_config(merged);
    merged = cv::parse_cli(argc, argv, merged);
    if (merged.log_file.empty() && !merged.preview_file.empty()) {
      merged.log_file = default_engine_log();
    }
    cv::log::init(merged.log_file, merged.verbose);
    cv::log::info("capture-view start version=0.2.1");
    const int result = cv::run_app(merged);
    if (!merged.no_config && !merged.gtk_ui && !merged.gui_info && !merged.frame_generation_info &&
        !merged.install_nvidia_runtime && !merged.remove_nvidia_runtime &&
        (merged.save_config || merged.save_only ||
         (!merged.diagnostic_bundle && !merged.list_profiles && !merged.init_profiles && !merged.doctor &&
          !merged.list_devices && !merged.list_audio && !merged.test_pattern))) {
      cv::save_config(merged);
      cv::log::info("config saved: ", cv::config_file_path());
    }
    return result;
  } catch (const cv::AppError& error) {
    cv::log::error(error.what());
    std::cerr << "error: " << error.what() << "\n\n";
    cv::print_usage(argv[0]);
    return 2;
  } catch (const std::exception& error) {
    cv::log::error("fatal: ", error.what());
    std::cerr << "fatal: " << error.what() << "\n";
    return 1;
  }
}
