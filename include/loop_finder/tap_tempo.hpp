#pragma once

#include <cstddef>
#include <deque>
#include <optional>

namespace loop_finder {

struct TapTempoResult {
    std::size_t tap_count{};
    bool sequence_restarted{};
    std::optional<double> bpm;
};

class TapTempo {
public:
    explicit TapTempo(std::size_t required_taps = 4,
                      std::size_t maximum_intervals = 7,
                      double inactivity_timeout_seconds = 4.0);

    TapTempoResult tap(double timestamp_seconds);
    void reset() noexcept;
    [[nodiscard]] std::size_t tap_count() const noexcept;

private:
    std::size_t required_taps_;
    std::size_t maximum_intervals_;
    double inactivity_timeout_seconds_;
    std::optional<double> previous_tap_;
    std::deque<double> intervals_;
};

} // namespace loop_finder
