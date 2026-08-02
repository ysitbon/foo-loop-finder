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
    const double phase = std::fmod(state.grid_offset_seconds, step);
    const double index = std::round((seconds - phase) / step);
    return std::max(0.0, phase + index * step);
}

std::vector<GridLine> grid_lines(double from_seconds, double to_seconds,
                                 const LoopState& state,
                                 std::size_t maximum_lines) {
    std::vector<GridLine> result;
    if (to_seconds < from_seconds || state.beats_per_bar == 0 ||
        state.subdivisions_per_beat == 0 || maximum_lines == 0) {
        return result;
    }

    const double step = beat_duration(state.bpm) / state.subdivisions_per_beat;
    const double bar_duration = step * state.subdivisions_per_beat *
                                state.beats_per_bar;
    const double phase = std::fmod(state.grid_offset_seconds, bar_duration);
    const auto first = static_cast<long long>(
        std::ceil((from_seconds - phase) / step));
    const auto last = static_cast<long long>(
        std::floor((to_seconds - phase) / step));
    const auto subdivisions_per_bar = static_cast<long long>(
        state.beats_per_bar * state.subdivisions_per_beat);

    if (last < first) {
        return result;
    }

    const auto count = static_cast<std::uint64_t>(last - first) + 1U;
    long long stride = 1;
    if (count > maximum_lines) {
        stride = static_cast<long long>(state.subdivisions_per_beat);
        const auto beat_count = count / static_cast<std::uint64_t>(stride) + 2U;
        if (beat_count > maximum_lines) {
            stride = subdivisions_per_bar;
            const auto bar_count = count / static_cast<std::uint64_t>(stride) + 2U;
            if (bar_count > maximum_lines) {
                stride *= static_cast<long long>(
                    (bar_count + maximum_lines - 1U) / maximum_lines);
            }
        }
    }

    const auto first_aligned = first +
        ((stride - ((first % stride) + stride) % stride) % stride);
    result.reserve((std::min)(maximum_lines,
                              static_cast<std::size_t>(count)));
    for (long long index = first_aligned; index <= last; index += stride) {
        const auto normalized = ((index % subdivisions_per_bar) + subdivisions_per_bar) %
                                subdivisions_per_bar;
        const bool is_beat = normalized % state.subdivisions_per_beat == 0;
        const auto beat = static_cast<std::uint32_t>(
            normalized / state.subdivisions_per_beat);
        result.push_back({phase + index * step,
                          normalized == 0,
                          is_beat,
                          is_beat ? beat : state.beats_per_bar});
    }
    return result;
}

} // namespace loop_finder
