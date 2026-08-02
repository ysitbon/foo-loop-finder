#pragma once

#include "loop_finder/loop_state.hpp"

#include <cstddef>
#include <vector>

namespace loop_finder {

struct GridLine {
    double seconds{};
    bool is_bar{};
    bool is_beat{};
    std::uint32_t beat_in_bar{};
};

double beat_duration(double bpm);
double snap_to_grid(double seconds, const LoopState& state);
std::vector<GridLine> grid_lines(double from_seconds, double to_seconds,
                                 const LoopState& state,
                                 std::size_t maximum_lines = 4096);

} // namespace loop_finder
