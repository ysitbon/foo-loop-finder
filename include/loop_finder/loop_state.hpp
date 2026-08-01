#pragma once

#include <cstdint>

namespace loop_finder {

struct LoopState {
    double bpm = 120.0;
    double grid_offset_seconds = 0.0;
    std::uint32_t beats_per_bar = 4;
    std::uint32_t subdivisions_per_beat = 1;
    double in_seconds = 0.0;
    double out_seconds = 4.0;
    bool enabled = false;
    bool snap_enabled = true;
};

} // namespace loop_finder

