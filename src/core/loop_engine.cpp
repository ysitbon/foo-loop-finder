#include "loop_finder/loop_engine.hpp"

#include "loop_finder/beat_grid.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace loop_finder {

namespace {
ValidationResult ok() { return {true, {}}; }
ValidationResult error(std::string message) { return {false, std::move(message)}; }
} // namespace

LoopEngine::LoopEngine(LoopState state) : state_(state) {
    if (!validate(state_).valid) {
        state_ = LoopState{};
    }
    // Safety invariant: looping never becomes active merely by constructing or
    // restoring an engine. The user must explicitly enable it.
    state_.enabled = false;
}

const LoopState& LoopEngine::state() const noexcept { return state_; }

ValidationResult LoopEngine::validate(const LoopState& candidate) const {
    if (!std::isfinite(candidate.bpm) || candidate.bpm < 20.0 || candidate.bpm > 300.0)
        return error("BPM must be between 20 and 300");
    if (candidate.beats_per_bar == 0 || candidate.beats_per_bar > 32)
        return error("Beats per bar must be between 1 and 32");
    if (candidate.subdivisions_per_beat == 0 || candidate.subdivisions_per_beat > 16)
        return error("Subdivision must be between 1 and 16");
    if (!std::isfinite(candidate.grid_offset_seconds))
        return error("Grid offset must be finite");
    if (!std::isfinite(candidate.in_seconds) || candidate.in_seconds < 0.0)
        return error("IN must be a non-negative finite time");
    if (!std::isfinite(candidate.out_seconds) || candidate.out_seconds <= candidate.in_seconds)
        return error("OUT must be after IN");
    return ok();
}

double LoopEngine::maybe_snap(double seconds) const {
    return state_.snap_enabled ? snap_to_grid(seconds, state_) : seconds;
}

ValidationResult LoopEngine::set_bpm(double bpm) {
    auto next = state_; next.bpm = bpm;
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_grid_offset(double seconds) {
    if (!std::isfinite(seconds)) return error("Grid offset must be finite");
    state_.grid_offset_seconds = seconds; return ok();
}

ValidationResult LoopEngine::set_beats_per_bar(std::uint32_t beats) {
    auto next = state_; next.beats_per_bar = beats;
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_subdivision(std::uint32_t subdivisions) {
    auto next = state_; next.subdivisions_per_beat = subdivisions;
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_snapping(SnapMode mode) {
    switch (mode) {
    case SnapMode::off:
    case SnapMode::beat:
    case SnapMode::half_beat:
    case SnapMode::quarter_beat:
    case SnapMode::eighth_beat:
    case SnapMode::sixteenth_beat:
        break;
    default:
        return error("Unsupported snapping mode");
    }

    auto next = state_;
    next.snap_enabled = mode != SnapMode::off;
    next.subdivisions_per_beat = snap_subdivisions(mode);
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_in(double seconds) {
    auto next = state_; next.in_seconds = maybe_snap(seconds);
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_out(double seconds) {
    auto next = state_; next.out_seconds = maybe_snap(seconds);
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_marker(double seconds,
                                        double track_duration_seconds,
                                        bool is_in) {
    if (!std::isfinite(track_duration_seconds) || track_duration_seconds <= 0.0)
        return error("Track duration must be positive and finite");
    if (!std::isfinite(seconds))
        return error(is_in ? "IN must be a finite time" : "OUT must be a finite time");

    const double clamped = std::clamp(seconds, 0.0, track_duration_seconds);
    const double snapped = std::clamp(maybe_snap(clamped),
                                      0.0,
                                      track_duration_seconds);
    auto next = state_;
    if (is_in) next.in_seconds = snapped;
    else next.out_seconds = snapped;
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_in_clamped(double seconds,
                                            double track_duration_seconds) {
    return set_marker(seconds, track_duration_seconds, true);
}

ValidationResult LoopEngine::set_out_clamped(double seconds,
                                             double track_duration_seconds) {
    return set_marker(seconds, track_duration_seconds, false);
}

ValidationResult LoopEngine::set_markers(double in_seconds, double out_seconds) {
    auto next = state_;
    next.in_seconds = in_seconds;
    next.out_seconds = out_seconds;
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

ValidationResult LoopEngine::set_markers_clamped(
    double in_seconds,
    double out_seconds,
    double track_duration_seconds) {
    if (!std::isfinite(track_duration_seconds) || track_duration_seconds <= 0.0)
        return error("Track duration must be positive and finite");
    if (!std::isfinite(in_seconds) || !std::isfinite(out_seconds))
        return error("Markers must be finite");
    return set_markers(std::clamp(in_seconds, 0.0, track_duration_seconds),
                       std::clamp(out_seconds, 0.0, track_duration_seconds));
}

ValidationResult LoopEngine::set_enabled(bool enabled) {
    auto next = state_; next.enabled = enabled;
    if (auto result = validate(next); !result.valid) return result;
    state_ = next; return ok();
}

std::optional<double> LoopEngine::seek_target(double playback_seconds,
                                              bool is_seeking) const {
    if (!state_.enabled || is_seeking || !std::isfinite(playback_seconds))
        return std::nullopt;
    if (playback_seconds >= state_.out_seconds)
        return state_.in_seconds;
    return std::nullopt;
}

double LoopEngine::loop_length_seconds() const noexcept {
    return state_.out_seconds - state_.in_seconds;
}

double LoopEngine::loop_length_beats() const noexcept {
    return loop_length_seconds() / (60.0 / state_.bpm);
}

std::optional<double> LoopEngine::loop_length_bars() const noexcept {
    const double bars = loop_length_beats() /
                        static_cast<double>(state_.beats_per_bar);
    const double whole_bars = std::round(bars);
    if (whole_bars > 0.0 && std::abs(bars - whole_bars) <= 1e-6) {
        return whole_bars;
    }
    return std::nullopt;
}

} // namespace loop_finder
