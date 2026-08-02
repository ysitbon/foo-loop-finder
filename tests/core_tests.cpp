#include "loop_finder/beat_grid.hpp"
#include "loop_finder/loop_engine.hpp"
#include "loop_finder/waveform.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
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
    check(engine.set_bpm(120.0).valid, "valid BPM accepted");
    check(!engine.set_bpm(301.0).valid, "invalid BPM rejected");
    check(engine.set_in(1.1).valid && near(engine.state().in_seconds, 1.0),
          "IN snaps to nearest beat");
    check(engine.set_out(3.1).valid && near(engine.state().out_seconds, 3.0),
          "OUT snaps to nearest beat");
    check(near(engine.loop_length_beats(), 4.0), "loop beat length calculated");
    check(!engine.seek_target(3.0).has_value(), "disabled loop never seeks");
    check(engine.set_enabled(true).valid, "loop enables explicitly");
    check(near(*engine.seek_target(3.0), 1.0), "OUT seeks to IN");
    check(!engine.seek_target(2.99).has_value(), "no early seek");
    check(!engine.seek_target(3.1, true).has_value(), "user seek is not intercepted");

    LoopState grid_state;
    grid_state.bpm = 120.0;
    auto lines = grid_lines(0.0, 2.0, grid_state);
    check(lines.size() == 5, "grid contains inclusive beat lines");
    check(lines.front().is_bar && lines.back().is_bar, "bar lines identified");

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
