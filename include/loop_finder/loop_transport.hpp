#pragma once

#include <cstdint>
#include <optional>

namespace loop_finder {

struct LoopRegion {
    double in_seconds{};
    double out_seconds{};
};

enum class LoopTransportState : std::uint8_t {
    disabled,
    armed,
    automatic_seek_pending,
    waiting_for_loop_in,
    manually_disarmed,
};

enum class LoopTransportResetReason : std::uint8_t {
    none,
    loop_disabled,
    stopped,
    track_changed,
    invalid_markers,
    unseekable,
    shutdown,
};

enum class SeekNotificationKind : std::uint8_t {
    ignored,
    automatic_seek,
    manual_seek_inside,
    manual_seek_outside,
};

struct LoopSeekRequest {
    double target_seconds{};
    double crossing_position_seconds{};
    double boundary_overshoot_seconds{};
    std::uint64_t generation{};
    std::uint64_t request_id{};
};

struct LoopTimingDiagnostics {
    double crossing_position_seconds{};
    double requested_target_seconds{};
    double boundary_overshoot_seconds{};
    std::uint64_t request_id{};
    std::optional<double> return_position_seconds;
    std::optional<double> return_observed_after_seconds;
};

class LoopTransport {
public:
    LoopTransport() = default;

    [[nodiscard]] LoopTransportState state() const noexcept;
    [[nodiscard]] LoopTransportResetReason reset_reason() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool is_pending(
        const LoopSeekRequest& request) const noexcept;
    [[nodiscard]] const std::optional<LoopTimingDiagnostics>&
        diagnostics() const noexcept;

    bool enable(LoopRegion region,
                bool seekable,
                std::optional<double> current_position = std::nullopt);
    bool update_markers(LoopRegion region,
                        std::optional<double> current_position = std::nullopt);
    void set_paused(bool paused) noexcept;
    void reset(LoopTransportResetReason reason) noexcept;

    [[nodiscard]] std::optional<LoopSeekRequest> observe_position(
        double position_seconds,
        double monotonic_seconds,
        std::uint64_t event_generation);
    [[nodiscard]] SeekNotificationKind observe_seek(
        double position_seconds,
        double monotonic_seconds,
        std::uint64_t event_generation);

private:
    [[nodiscard]] static bool valid_region(LoopRegion region) noexcept;
    [[nodiscard]] bool inside(double position_seconds) const noexcept;
    void arm_from(std::optional<double> current_position) noexcept;

    LoopTransportState state_ = LoopTransportState::disabled;
    LoopTransportResetReason reset_reason_ =
        LoopTransportResetReason::loop_disabled;
    LoopRegion region_{};
    bool enabled_ = false;
    bool paused_ = false;
    std::uint64_t generation_ = 1;
    std::uint64_t next_request_id_ = 1;
    std::optional<double> previous_position_;
    std::optional<LoopSeekRequest> pending_request_;
    std::optional<double> request_monotonic_seconds_;
    std::optional<LoopTimingDiagnostics> diagnostics_;
};

} // namespace loop_finder
