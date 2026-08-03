#include "loop_finder/loop_transport.hpp"

#include <algorithm>
#include <cmath>

namespace loop_finder {

namespace {
constexpr double kAutomaticSeekTargetToleranceSeconds = 0.05;
}

LoopTransportState LoopTransport::state() const noexcept { return state_; }

LoopTransportResetReason LoopTransport::reset_reason() const noexcept {
    return reset_reason_;
}

std::uint64_t LoopTransport::generation() const noexcept {
    return generation_;
}

bool LoopTransport::enabled() const noexcept { return enabled_; }

bool LoopTransport::is_pending(
    const LoopSeekRequest& request) const noexcept {
    return enabled_ &&
           state_ == LoopTransportState::automatic_seek_pending &&
           pending_request_.has_value() &&
           pending_request_->generation == request.generation &&
           pending_request_->request_id == request.request_id &&
           pending_request_->target_seconds == request.target_seconds;
}

const std::optional<LoopTimingDiagnostics>&
LoopTransport::diagnostics() const noexcept {
    return diagnostics_;
}

bool LoopTransport::valid_region(LoopRegion region) noexcept {
    return std::isfinite(region.in_seconds) && region.in_seconds >= 0.0 &&
           std::isfinite(region.out_seconds) &&
           region.out_seconds > region.in_seconds;
}

bool LoopTransport::inside(double position_seconds) const noexcept {
    return std::isfinite(position_seconds) &&
           position_seconds >= region_.in_seconds &&
           position_seconds < region_.out_seconds;
}

void LoopTransport::arm_from(
    std::optional<double> current_position) noexcept {
    state_ = LoopTransportState::armed;
    previous_position_.reset();
    pending_request_.reset();
    request_monotonic_seconds_.reset();

    if (current_position.has_value() &&
        std::isfinite(*current_position) &&
        *current_position < region_.out_seconds) {
        previous_position_ = *current_position;
    }
}

bool LoopTransport::enable(LoopRegion region,
                           bool seekable,
                           std::optional<double> current_position) {
    ++generation_;
    diagnostics_.reset();
    paused_ = false;
    if (!valid_region(region)) {
        enabled_ = false;
        state_ = LoopTransportState::disabled;
        reset_reason_ = LoopTransportResetReason::invalid_markers;
        previous_position_.reset();
        pending_request_.reset();
        request_monotonic_seconds_.reset();
        return false;
    }
    if (!seekable) {
        enabled_ = false;
        state_ = LoopTransportState::disabled;
        reset_reason_ = LoopTransportResetReason::unseekable;
        previous_position_.reset();
        pending_request_.reset();
        request_monotonic_seconds_.reset();
        return false;
    }

    region_ = region;
    enabled_ = true;
    reset_reason_ = LoopTransportResetReason::none;
    arm_from(current_position);
    return true;
}

bool LoopTransport::update_markers(
    LoopRegion region,
    std::optional<double> current_position) {
    ++generation_;
    diagnostics_.reset();
    if (!valid_region(region)) {
        enabled_ = false;
        state_ = LoopTransportState::disabled;
        reset_reason_ = LoopTransportResetReason::invalid_markers;
        previous_position_.reset();
        pending_request_.reset();
        request_monotonic_seconds_.reset();
        return false;
    }

    region_ = region;
    if (enabled_) {
        reset_reason_ = LoopTransportResetReason::none;
        arm_from(current_position);
    }
    return true;
}

void LoopTransport::set_paused(bool paused) noexcept { paused_ = paused; }

void LoopTransport::reset(LoopTransportResetReason reason) noexcept {
    ++generation_;
    enabled_ = false;
    paused_ = false;
    state_ = LoopTransportState::disabled;
    reset_reason_ = reason;
    previous_position_.reset();
    pending_request_.reset();
    request_monotonic_seconds_.reset();
    diagnostics_.reset();
}

std::optional<LoopSeekRequest> LoopTransport::observe_position(
    double position_seconds,
    double monotonic_seconds,
    std::uint64_t event_generation) {
    if (event_generation != generation_ || !enabled_ || paused_ ||
        !std::isfinite(position_seconds)) {
        return std::nullopt;
    }
    if (!valid_region(region_)) {
        reset(LoopTransportResetReason::invalid_markers);
        return std::nullopt;
    }

    if (state_ == LoopTransportState::automatic_seek_pending) {
        // Some hosts/sources may expose the new position before (or without)
        // the matching seek notification. Treat an observed position inside
        // the region as completion of the outstanding intent; positions still
        // beyond OUT remain suppressed.
        if (inside(position_seconds)) {
            state_ = LoopTransportState::armed;
            previous_position_ = position_seconds;
            if (diagnostics_.has_value()) {
                diagnostics_->return_position_seconds = position_seconds;
                if (request_monotonic_seconds_.has_value() &&
                    std::isfinite(monotonic_seconds)) {
                    diagnostics_->return_observed_after_seconds =
                        (std::max)(0.0,
                                   monotonic_seconds -
                                       *request_monotonic_seconds_);
                }
            }
            pending_request_.reset();
            request_monotonic_seconds_.reset();
        }
        return std::nullopt;
    }

    if (state_ == LoopTransportState::waiting_for_loop_in) {
        if (inside(position_seconds)) {
            state_ = LoopTransportState::armed;
            previous_position_ = position_seconds;
            if (diagnostics_.has_value()) {
                diagnostics_->return_position_seconds = position_seconds;
                if (request_monotonic_seconds_.has_value() &&
                    std::isfinite(monotonic_seconds)) {
                    diagnostics_->return_observed_after_seconds =
                        (std::max)(0.0,
                                   monotonic_seconds -
                                       *request_monotonic_seconds_);
                }
            }
            pending_request_.reset();
            request_monotonic_seconds_.reset();
        }
        return std::nullopt;
    }

    if (state_ == LoopTransportState::manually_disarmed) {
        if (inside(position_seconds)) {
            state_ = LoopTransportState::armed;
            previous_position_ = position_seconds;
        }
        return std::nullopt;
    }

    if (state_ != LoopTransportState::armed) {
        return std::nullopt;
    }

    if (previous_position_.has_value() &&
        *previous_position_ < region_.out_seconds &&
        position_seconds >= region_.out_seconds &&
        position_seconds >= *previous_position_) {
        LoopSeekRequest request;
        request.target_seconds = region_.in_seconds;
        request.crossing_position_seconds = position_seconds;
        request.boundary_overshoot_seconds =
            (std::max)(0.0, position_seconds - region_.out_seconds);
        request.generation = generation_;
        request.request_id = next_request_id_++;

        state_ = LoopTransportState::automatic_seek_pending;
        previous_position_.reset();
        pending_request_ = request;
        request_monotonic_seconds_ =
            std::isfinite(monotonic_seconds)
                ? std::optional<double>(monotonic_seconds)
                : std::nullopt;
        diagnostics_ = LoopTimingDiagnostics{
            request.crossing_position_seconds,
            request.target_seconds,
            request.boundary_overshoot_seconds,
            request.request_id,
            std::nullopt,
            std::nullopt};
        return request;
    }

    previous_position_ = position_seconds;
    return std::nullopt;
}

SeekNotificationKind LoopTransport::observe_seek(
    double position_seconds,
    double,
    std::uint64_t event_generation) {
    if (event_generation != generation_ || !enabled_ ||
        !std::isfinite(position_seconds)) {
        return SeekNotificationKind::ignored;
    }

    if ((state_ == LoopTransportState::automatic_seek_pending ||
         state_ == LoopTransportState::waiting_for_loop_in) &&
        pending_request_.has_value() &&
        pending_request_->generation == generation_ &&
        std::abs(position_seconds - pending_request_->target_seconds) <=
            kAutomaticSeekTargetToleranceSeconds) {
        state_ = LoopTransportState::waiting_for_loop_in;
        previous_position_.reset();
        return SeekNotificationKind::automatic_seek;
    }

    pending_request_.reset();
    request_monotonic_seconds_.reset();
    if (inside(position_seconds)) {
        state_ = LoopTransportState::armed;
        previous_position_ = position_seconds;
        return SeekNotificationKind::manual_seek_inside;
    }

    state_ = LoopTransportState::manually_disarmed;
    previous_position_.reset();
    return SeekNotificationKind::manual_seek_outside;
}

} // namespace loop_finder
