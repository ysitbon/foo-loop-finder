#include "loop_finder/beat_grid.hpp"
#include "loop_finder/loop_engine.hpp"
#include "loop_finder/loop_transport.hpp"
#include "loop_finder/tap_tempo.hpp"
#include "loop_finder/waveform.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool near(double a, double b, double epsilon = 1e-9) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    using namespace loop_finder;

    LoopEngine engine;
    check(!engine.state().enabled, "loop is disabled by default");
    check(engine.set_bpm(20.0).valid && near(engine.state().bpm, 20.0),
          "lower BPM boundary accepted");
    check(engine.set_bpm(300.0).valid && near(engine.state().bpm, 300.0),
          "upper BPM boundary accepted");
    check(engine.set_bpm(92.5).valid && near(engine.state().bpm, 92.5),
          "decimal BPM accepted");
    const double valid_bpm = engine.state().bpm;
    check(!engine.set_bpm(19.999).valid && near(engine.state().bpm, valid_bpm),
          "BPM below range is rejected without mutation");
    check(!engine.set_bpm(300.001).valid && near(engine.state().bpm, valid_bpm),
          "BPM above range is rejected without mutation");
    check(!engine.set_bpm(std::nan("")).valid && near(engine.state().bpm, valid_bpm),
          "NaN BPM is rejected without mutation");
    check(!engine.set_bpm(std::numeric_limits<double>::infinity()).valid &&
              near(engine.state().bpm, valid_bpm),
          "infinite BPM is rejected without mutation");
    check(engine.set_bpm(120.0).valid, "valid BPM accepted");

    check(engine.set_grid_offset(-0.125).valid &&
              near(engine.state().grid_offset_seconds, -0.125),
          "negative finite grid offset accepted");
    const double valid_offset = engine.state().grid_offset_seconds;
    check(!engine.set_grid_offset(std::nan("")).valid &&
              near(engine.state().grid_offset_seconds, valid_offset),
          "invalid grid offset is rejected without mutation");
    check(engine.set_grid_offset(0.0).valid, "grid offset reset");
    check(engine.set_in(1.1).valid && near(engine.state().in_seconds, 1.0),
          "IN snaps to nearest beat");
    check(engine.set_out(3.1).valid && near(engine.state().out_seconds, 3.0),
          "OUT snaps to nearest beat");
    check(near(engine.loop_length_beats(), 4.0), "loop beat length calculated");
    check(engine.set_enabled(true).valid, "loop enables explicitly");

    LoopEngine snapping;
    check(snapping.set_markers(1.13, 3.13).valid,
          "exact persisted markers accepted");
    for (const auto mode : {SnapMode::off,
                            SnapMode::beat,
                            SnapMode::half_beat,
                            SnapMode::quarter_beat,
                            SnapMode::eighth_beat,
                            SnapMode::sixteenth_beat}) {
        const auto before = snapping.state();
        check(snapping.set_snapping(mode).valid, "supported snapping mode accepted");
        check(near(snapping.state().in_seconds, before.in_seconds) &&
                  near(snapping.state().out_seconds, before.out_seconds),
              "changing snapping does not move markers");
    }
    check(snapping.set_snapping(SnapMode::off).valid &&
              snapping.set_in(1.2345).valid &&
              near(snapping.state().in_seconds, 1.2345),
          "free placement preserves exact marker time");
    check(snapping.set_snapping(SnapMode::quarter_beat).valid &&
              snapping.set_in(1.26).valid &&
              near(snapping.state().in_seconds, 1.25),
          "quarter-beat placement snaps");
    const double marker_before_bpm = snapping.state().in_seconds;
    check(snapping.set_bpm(100.0).valid &&
              near(snapping.state().in_seconds, marker_before_bpm),
          "changing BPM does not move markers");

    LoopEngine divisions;
    check(divisions.set_markers(0.1, 3.5).valid,
          "snapping division test setup accepted");
    const struct {
        SnapMode mode;
        double expected;
    } division_cases[] = {
        {SnapMode::off, 1.17},
        {SnapMode::beat, 1.0},
        {SnapMode::half_beat, 1.25},
        {SnapMode::quarter_beat, 1.125},
        {SnapMode::eighth_beat, 1.1875},
        {SnapMode::sixteenth_beat, 1.15625},
    };
    for (const auto& test : division_cases) {
        check(divisions.set_snapping(test.mode).valid &&
                  divisions.set_in(1.17).valid &&
                  near(divisions.state().in_seconds, test.expected),
              "each supported snapping division places markers correctly");
    }

    LoopEngine ordering;
    check(!ordering.set_in(4.0).valid && near(ordering.state().in_seconds, 0.0),
          "IN cannot reach OUT");
    check(!ordering.set_out(0.0).valid && near(ordering.state().out_seconds, 4.0),
          "OUT must remain after IN");
    check(ordering.set_snapping(SnapMode::off).valid, "free clamping setup");
    check(ordering.set_in_clamped(-10.0, 3.0).valid &&
              near(ordering.state().in_seconds, 0.0),
          "IN clamps to track start");
    check(ordering.set_out_clamped(10.0, 3.0).valid &&
              near(ordering.state().out_seconds, 3.0),
          "OUT clamps to track duration");
    const auto ordered = ordering.state();
    check(!ordering.set_in_clamped(10.0, 3.0).valid &&
              near(ordering.state().in_seconds, ordered.in_seconds) &&
              near(ordering.state().out_seconds, ordered.out_seconds),
          "rejected clamped marker preserves prior state");

    LoopEngine duration;
    check(duration.set_markers(1.0, 5.0).valid,
          "duration marker setup accepted");
    check(near(duration.loop_length_seconds(), 4.0) &&
              near(duration.loop_length_beats(), 8.0),
          "duration is reported in seconds and beats");
    check(duration.loop_length_bars().has_value() &&
              near(*duration.loop_length_bars(), 2.0),
          "whole-bar duration is reported");
    check(duration.set_out(4.5).valid && !duration.loop_length_bars().has_value(),
          "non-bar duration does not claim a bar count");

    LoopState restored_state;
    restored_state.enabled = true;
    restored_state.bpm = 92.5;
    restored_state.grid_offset_seconds = -0.012;
    restored_state.in_seconds = 1.1;
    restored_state.out_seconds = 2.2;
    LoopEngine restored(restored_state);
    check(!restored.state().enabled && near(restored.state().bpm, 92.5) &&
              near(restored.state().in_seconds, 1.1),
          "restoration preserves editor metadata but forces Loop off");

    LoopTransport initial_transport;
    check(!initial_transport.enabled() &&
              initial_transport.state() == LoopTransportState::disabled,
          "transport is disabled initially");
    check(!initial_transport.observe_position(
               4.0, 0.0, initial_transport.generation()).has_value(),
          "disabled transport never requests a seek");

    LoopTransport crossing;
    check(crossing.enable({1.0, 3.0}, true, 1.25),
          "valid seekable loop arms transport");
    const auto crossing_generation = crossing.generation();
    check(!crossing.observe_position(2.9, 10.0, crossing_generation).has_value(),
          "positions below OUT do not seek");
    const auto first_seek =
        crossing.observe_position(3.2, 10.1, crossing_generation);
    check(first_seek.has_value() && near(first_seek->target_seconds, 1.0) &&
              near(first_seek->crossing_position_seconds, 3.2) &&
              near(first_seek->boundary_overshoot_seconds, 0.2) &&
              crossing.is_pending(*first_seek),
          "crossing OUT requests IN and records overshoot");
    check(crossing.state() == LoopTransportState::automatic_seek_pending &&
              !crossing.observe_position(
                   3.3, 10.2, crossing_generation).has_value(),
          "callbacks beyond OUT do not duplicate a pending seek");
    check(crossing.observe_seek(1.0, 10.25, crossing_generation) ==
              SeekNotificationKind::automatic_seek &&
              crossing.state() == LoopTransportState::waiting_for_loop_in &&
              !crossing.is_pending(*first_seek),
          "automatic seek callback is consumed explicitly");
    check(!crossing.observe_position(
               1.03, 10.3, crossing_generation).has_value() &&
              crossing.state() == LoopTransportState::armed,
          "observing playback near IN re-arms transport");
    check(crossing.diagnostics().has_value() &&
              crossing.diagnostics()->return_observed_after_seconds.has_value() &&
              near(*crossing.diagnostics()->return_observed_after_seconds, 0.2),
          "automatic seek return timing is recorded");

    LoopTransport position_acknowledged;
    check(position_acknowledged.enable({1.0, 2.0}, true, 1.8),
          "position-only automatic seek setup arms");
    const auto position_ack_generation = position_acknowledged.generation();
    check(position_acknowledged.observe_position(
              2.1, 10.0, position_ack_generation).has_value() &&
              !position_acknowledged.observe_position(
                   1.05, 10.1, position_ack_generation).has_value() &&
              position_acknowledged.state() == LoopTransportState::armed,
          "position observed inside can complete an automatic seek safely");

    LoopTransport overshoot;
    check(overshoot.enable({1.0, 2.0}, true, 0.25),
          "coarse crossing setup arms");
    const auto overshoot_generation = overshoot.generation();
    const auto coarse_seek =
        overshoot.observe_position(2.75, 1.0, overshoot_generation);
    check(coarse_seek.has_value() && near(coarse_seek->target_seconds, 1.0) &&
              near(coarse_seek->boundary_overshoot_seconds, 0.75),
          "coarse callback interval still detects an OUT crossing");

    LoopTransport manual_inside;
    check(manual_inside.enable({1.0, 3.0}, true, 1.2),
          "inside manual seek setup arms");
    const auto manual_inside_generation = manual_inside.generation();
    check(manual_inside.observe_seek(2.0, 2.0, manual_inside_generation) ==
              SeekNotificationKind::manual_seek_inside &&
              manual_inside.state() == LoopTransportState::armed,
          "manual seek inside the loop re-arms without seeking");
    check(!manual_inside.observe_position(
               2.5, 2.1, manual_inside_generation).has_value(),
          "manual seek callback does not itself trigger an automatic seek");
    check(manual_inside.observe_position(
              3.0, 2.2, manual_inside_generation).has_value(),
          "normal playback after inside manual seek may cross OUT");

    LoopTransport manual_outside;
    check(manual_outside.enable({1.0, 3.0}, true, 1.5),
          "outside manual seek setup arms");
    const auto manual_outside_generation = manual_outside.generation();
    check(manual_outside.observe_seek(4.0, 3.0, manual_outside_generation) ==
              SeekNotificationKind::manual_seek_outside &&
              manual_outside.state() == LoopTransportState::manually_disarmed,
          "manual seek outside is accepted and disarms transport");
    check(!manual_outside.observe_position(
               4.5, 3.1, manual_outside_generation).has_value() &&
              !manual_outside.observe_position(
                   0.5, 3.2, manual_outside_generation).has_value(),
          "outside manual seek never snaps back to IN");
    check(!manual_outside.observe_position(
               1.5, 3.3, manual_outside_generation).has_value() &&
              manual_outside.state() == LoopTransportState::armed,
          "playback observed inside after outside seek re-arms");

    LoopTransport cancelled;
    check(cancelled.enable({1.0, 2.0}, true, 1.5),
          "pending cancellation setup arms");
    auto cancelled_generation = cancelled.generation();
    check(cancelled.observe_position(
              2.0, 4.0, cancelled_generation).has_value(),
          "pending cancellation setup requests seek");
    cancelled.reset(LoopTransportResetReason::loop_disabled);
    check(cancelled.state() == LoopTransportState::disabled &&
              cancelled.observe_seek(1.0, 4.1, cancelled_generation) ==
                  SeekNotificationKind::ignored,
          "toggle-off clears pending automatic-seek state");

    LoopTransport paused_transport;
    check(paused_transport.enable({1.0, 2.0}, true, 1.9),
          "pause setup arms");
    const auto paused_generation = paused_transport.generation();
    paused_transport.set_paused(true);
    check(!paused_transport.observe_position(
               2.1, 5.0, paused_generation).has_value(),
          "paused transport never seeks");
    paused_transport.set_paused(false);
    check(paused_transport.observe_position(
              2.1, 5.1, paused_generation).has_value(),
          "resume safely observes the pending forward crossing");
    paused_transport.reset(LoopTransportResetReason::stopped);
    check(!paused_transport.enabled() &&
              paused_transport.reset_reason() ==
                  LoopTransportResetReason::stopped,
          "stop disables and resets transport");
    check(paused_transport.enable({1.0, 2.0}, true, 1.2),
          "track reset setup re-arms");
    paused_transport.reset(LoopTransportResetReason::track_changed);
    check(!paused_transport.enabled() &&
              paused_transport.reset_reason() ==
                  LoopTransportResetReason::track_changed,
          "track change disables and resets transport");

    LoopTransport invalid_transport;
    check(!invalid_transport.enable({2.0, 2.0}, true, 2.0) &&
              invalid_transport.reset_reason() ==
                  LoopTransportResetReason::invalid_markers &&
              !invalid_transport.observe_position(
                   3.0, 0.0, invalid_transport.generation()).has_value(),
          "invalid markers safely disable transport");
    check(!invalid_transport.enable({1.0, 2.0}, false, 1.5) &&
              invalid_transport.reset_reason() ==
                  LoopTransportResetReason::unseekable,
          "unseekable source safely disables transport");

    LoopTransport marker_generation;
    check(marker_generation.enable({1.0, 2.0}, true, 1.5),
          "marker generation setup arms");
    const auto stale_generation = marker_generation.generation();
    check(marker_generation.update_markers({1.25, 2.5}, 1.5),
          "marker change reconfigures transport");
    check(!marker_generation.observe_position(
               3.0, 6.0, stale_generation).has_value(),
          "marker changes invalidate stale position events");
    const auto current_marker_generation = marker_generation.generation();
    check(marker_generation.observe_position(
              2.75, 6.1, current_marker_generation).has_value(),
          "new marker generation detects its own forward crossing");

    LoopTransport short_loop;
    check(short_loop.enable({1.0, 1.001}, true, 0.999),
          "very short valid loop arms");
    const auto short_generation = short_loop.generation();
    check(short_loop.observe_position(
              1.002, 7.0, short_generation).has_value(),
          "very short loop requests one seek at crossing");
    check(!short_loop.observe_position(
               1.003, 7.0, short_generation).has_value() &&
              short_loop.observe_seek(1.0, 7.0, short_generation) ==
                  SeekNotificationKind::automatic_seek &&
              !short_loop.observe_position(
                   1.002, 7.0, short_generation).has_value(),
          "very short loop cannot recursively request before IN is observed");

    TapTempo tap;
    check(!tap.tap(10.0).bpm.has_value() &&
              !tap.tap(10.5).bpm.has_value() &&
              !tap.tap(11.0).bpm.has_value(),
          "tap tempo waits for enough taps");
    const auto normal_tap = tap.tap(11.5);
    check(normal_tap.bpm.has_value() && near(*normal_tap.bpm, 120.0),
          "normal tap tempo resolves BPM");

    TapTempo jittered;
    jittered.tap(0.0);
    jittered.tap(0.49);
    jittered.tap(1.01);
    jittered.tap(1.50);
    jittered.tap(2.70); // one deliberately late tap
    jittered.tap(3.00);
    const auto robust_tap = jittered.tap(3.50);
    check(robust_tap.bpm.has_value() && *robust_tap.bpm > 115.0 &&
              *robust_tap.bpm < 125.0,
          "median tap tempo resists jitter and one poor tap");

    TapTempo timed_out;
    timed_out.tap(1.0);
    timed_out.tap(1.5);
    const auto restart = timed_out.tap(6.0);
    check(restart.sequence_restarted && restart.tap_count == 1 &&
              !restart.bpm.has_value(),
          "tap tempo resets after inactivity timeout");
    check(timed_out.tap_count() == 1, "tap reset starts a new sequence");

    LoopState grid_state;
    grid_state.bpm = 120.0;
    auto lines = grid_lines(0.0, 2.0, grid_state);
    check(lines.size() == 5, "grid contains inclusive beat lines");
    check(lines.front().is_bar && lines.back().is_bar, "bar lines identified");
    grid_state.subdivisions_per_beat = 16;
    const auto bounded_lines = grid_lines(0.0, 60.0 * 60.0, grid_state, 100);
    check(bounded_lines.size() <= 100,
          "large visible ranges produce a bounded grid");

    const std::vector<float> stereo{-1.0F, -1.0F, 0.5F, 0.5F,
                                     1.0F, 1.0F, -0.5F, -0.5F};
    auto waveform = reduce_waveform(stereo, 2, 2);
    check(waveform.size() == 2, "waveform reduced to requested bins");
    check(near(waveform[0].minimum, -1.0) && near(waveform[0].maximum, 0.5),
          "first waveform extrema retained");

    check(reduce_waveform({}, 1, 10).empty(), "empty PCM produces no bins");
    check(reduce_waveform({stereo.data(), stereo.size()}, 2, 20).size() == 4,
          "short PCM does not invent source bins");

    const std::vector<float> mono{-1.0F, -0.5F, 0.5F, 1.0F};
    auto mono_waveform = reduce_waveform(mono, 1, 4);
    check(mono_waveform.size() == 4 &&
              near(mono_waveform.front().minimum, -1.0),
          "mono PCM is reduced");

    StreamingWaveformReducer streaming(2);
    streaming.append(std::span<const float>(stereo.data(), 4), 2);
    streaming.append(std::span<const float>(stereo.data() + 4, 4), 2);
    auto streamed = streaming.finish();
    check(streaming.frame_count() == 4 && streamed.size() == 2,
          "stereo PCM chunks stream into bounded waveform bins");
    check(near(streamed.front().minimum, -1.0) &&
              near(streamed.back().maximum, 1.0),
          "streaming reduction preserves extrema");

    StreamingWaveformReducer empty_stream(8);
    check(empty_stream.finish().empty(), "empty streamed PCM stays empty");

    auto expanded = resample_waveform(streamed, 0.0, 1.0, 8);
    check(expanded.size() == 8,
          "waveform resampling supports widths larger than cached bins");
    auto selected = resample_waveform(streamed, 0.5, 1.0, 3);
    check(selected.size() == 3 && near(selected.front().maximum, 1.0),
          "waveform view selection resamples the requested range");
    check(resample_waveform(streamed, 0.8, 0.2, 3).empty(),
          "invalid waveform view does not mutate or produce output");

    std::cout << "All loop finder core tests passed\n";
}
