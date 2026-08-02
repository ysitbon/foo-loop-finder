#pragma once

#include <cstdint>

namespace loop_finder {

enum class SnapMode : std::uint8_t {
    off = 0,
    beat = 1,
    half_beat = 2,
    quarter_beat = 4,
    eighth_beat = 8,
    sixteenth_beat = 16,
};

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

[[nodiscard]] constexpr std::uint32_t snap_subdivisions(SnapMode mode) noexcept {
    return mode == SnapMode::off ? 1U : static_cast<std::uint32_t>(mode);
}

[[nodiscard]] constexpr SnapMode snap_mode(const LoopState& state) noexcept {
    if (!state.snap_enabled) {
        return SnapMode::off;
    }
    switch (state.subdivisions_per_beat) {
    case 1: return SnapMode::beat;
    case 2: return SnapMode::half_beat;
    case 4: return SnapMode::quarter_beat;
    case 8: return SnapMode::eighth_beat;
    case 16: return SnapMode::sixteenth_beat;
    default: return SnapMode::off;
    }
}

} // namespace loop_finder
