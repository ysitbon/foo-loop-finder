#include "loop_finder/beat_grid.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loop_finder {

double beat_duration(double bpm) {
    if (!std::isfinite(bpm) || bpm <= 0.0) {
        throw std::invalid_argument("BPM must be finite and positive");
    }
    return 60.0 / bpm;
}

double snap_to_grid(double seconds, const LoopState& state) {
    const double step = beat_duration(state.bpm) /
                        static_cast<double>(std::max(1u, state.subdivisions_per_beat));
    const double index = std::round((seconds - state.grid_offset_seconds) / step);
    return std::max(0.0, state.grid_offset_seconds + index * step);
}

std::vector<GridLine> grid_lines(double from_seconds, double to_seconds,
                                 const LoopState& state) {
    std::vector<GridLine> result;
    if (to_seconds < from_seconds || state.beats_per_bar == 0 ||
        state.subdivisions_per_beat == 0) {
        return result;
    }

    const double step = beat_duration(state.bpm) / state.subdivisions_per_beat;
    const auto first = static_cast<long long>(
        std::ceil((from_seconds - state.grid_offset_seconds) / step));
    const auto last = static_cast<long long>(
        std::floor((to_seconds - state.grid_offset_seconds) / step));
    const auto subdivisions_per_bar = static_cast<long long>(
        state.beats_per_bar * state.subdivisions_per_beat);

    result.reserve(last >= first ? static_cast<std::size_t>(last - first + 1) : 0);
    for (long long index = first; index <= last; ++index) {
        const auto normalized = ((index % subdivisions_per_bar) + subdivisions_per_bar) %
                                subdivisions_per_bar;
        const bool is_beat = normalized % state.subdivisions_per_beat == 0;
        const auto beat = static_cast<std::uint32_t>(
            normalized / state.subdivisions_per_beat);
        result.push_back({state.grid_offset_seconds + index * step,
                          normalized == 0,
                          is_beat ? beat : state.beats_per_bar});
    }
    return result;
}

} // namespace loop_finder

