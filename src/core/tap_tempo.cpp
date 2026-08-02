#include "loop_finder/tap_tempo.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace loop_finder {

TapTempo::TapTempo(std::size_t required_taps,
                   std::size_t maximum_intervals,
                   double inactivity_timeout_seconds)
    : required_taps_(required_taps),
      maximum_intervals_(maximum_intervals),
      inactivity_timeout_seconds_(inactivity_timeout_seconds) {
    if (required_taps_ < 3 || maximum_intervals_ < required_taps_ - 1 ||
        !std::isfinite(inactivity_timeout_seconds_) ||
        inactivity_timeout_seconds_ <= 0.0) {
        throw std::invalid_argument("Invalid tap-tempo configuration");
    }
}

TapTempoResult TapTempo::tap(double timestamp_seconds) {
    if (!std::isfinite(timestamp_seconds)) {
        reset();
        return {};
    }

    bool restarted = false;
    if (previous_tap_.has_value()) {
        const double interval = timestamp_seconds - *previous_tap_;
        if (interval <= 0.0 || interval > inactivity_timeout_seconds_) {
            reset();
            restarted = true;
        } else {
            intervals_.push_back(interval);
            while (intervals_.size() > maximum_intervals_) {
                intervals_.pop_front();
            }
        }
    }
    previous_tap_ = timestamp_seconds;

    TapTempoResult result{intervals_.size() + 1U, restarted, std::nullopt};
    if (result.tap_count < required_taps_) {
        return result;
    }

    std::vector<double> sorted(intervals_.begin(), intervals_.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t middle = sorted.size() / 2U;
    const double median = sorted.size() % 2U == 0U
        ? (sorted[middle - 1U] + sorted[middle]) / 2.0
        : sorted[middle];
    if (median > 0.0 && std::isfinite(median)) {
        result.bpm = 60.0 / median;
    }
    return result;
}

void TapTempo::reset() noexcept {
    previous_tap_.reset();
    intervals_.clear();
}

std::size_t TapTempo::tap_count() const noexcept {
    return previous_tap_.has_value() ? intervals_.size() + 1U : 0U;
}

} // namespace loop_finder
