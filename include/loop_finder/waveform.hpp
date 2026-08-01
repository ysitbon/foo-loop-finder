#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace loop_finder {

struct WaveformBin {
    float minimum{};
    float maximum{};
    float rms{};
};

// Reduces interleaved PCM to display-ready mono peak/RMS bins.
std::vector<WaveformBin> reduce_waveform(std::span<const float> interleaved_pcm,
                                        std::size_t channels,
                                        std::size_t requested_bins);

} // namespace loop_finder

