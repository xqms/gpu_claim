// Collect information from GPUs using NVIDIA NVML
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef GPU_INFO_H
#define GPU_INFO_H

#include <span>

#include "../protocol.h"

namespace gpu_info
{

std::optional<std::vector<Card>> init();

void update(Card& card, const std::chrono::steady_clock::time_point& now = std::chrono::steady_clock::now());

void shutdown();

}

#endif
