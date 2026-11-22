#pragma once
#include "cli.hpp"
namespace cv {
int write_gui_info(const CliOptions& options);
int run_gpui_ui(const CliOptions& options);
// Prints DLSS Frame Generation support and runtime state as one JSON line.
int write_frame_generation_info();
// Downloads the NVIDIA runtime, printing "progress DONE TOTAL" lines and a
// final "ok" or "error MESSAGE" line for the settings UI.
int run_install_nvidia_runtime();
int run_remove_nvidia_runtime();
}
