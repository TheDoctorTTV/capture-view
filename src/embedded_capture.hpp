#pragma once
#include "cli.hpp"
#include "v4l2_capture.hpp"
namespace cv {
int run_embedded_capture(CliOptions& options, const std::vector<CaptureConfig>& attempts);
}
