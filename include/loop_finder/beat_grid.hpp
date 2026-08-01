#pragma once

#include "loop_finder/loop_state.hpp"

#include <vector>

namespace loop_finder {

struct GridLine {
    double seconds{};
    bool is_bar{};
    std::uint32_t beat_in_bar{};
};

double beat_duration(double bpm);
double snap_to_grid(double seconds, const LoopState& state);
std::vector<GridLine> grid_lines(double from_seconds, double to_seconds,
                                 const LoopState& state);

} // namespace loop_finder

