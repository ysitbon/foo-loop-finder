#pragma once

#include "loop_finder/loop_state.hpp"

#include <optional>
#include <string>

namespace loop_finder {

struct ValidationResult {
    bool valid{};
    std::string message;
};

class LoopEngine {
public:
    explicit LoopEngine(LoopState state = {});

    [[nodiscard]] const LoopState& state() const noexcept;
    ValidationResult set_bpm(double bpm);
    ValidationResult set_grid_offset(double seconds);
    ValidationResult set_subdivision(std::uint32_t subdivisions);
    ValidationResult set_in(double seconds);
    ValidationResult set_out(double seconds);
    ValidationResult set_enabled(bool enabled);

    // Returns the seek target when playback has reached the loop boundary.
    [[nodiscard]] std::optional<double> seek_target(double playback_seconds,
                                                    bool is_seeking = false) const;
    [[nodiscard]] double loop_length_seconds() const noexcept;
    [[nodiscard]] double loop_length_beats() const noexcept;

private:
    ValidationResult validate(const LoopState& candidate) const;
    double maybe_snap(double seconds) const;
    LoopState state_;
};

} // namespace loop_finder

