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

// Incrementally reduces decoded PCM while bounding temporary analysis memory.
// The final bins describe the full decoded stream and are independent of any
// drawing size.
class StreamingWaveformReducer {
public:
    explicit StreamingWaveformReducer(std::size_t requested_bins);

    void append(std::span<const float> interleaved_pcm, std::size_t channels);
    [[nodiscard]] std::vector<WaveformBin> finish();
    [[nodiscard]] std::size_t frame_count() const noexcept;

private:
    struct Aggregate {
        float minimum = 1.0F;
        float maximum = -1.0F;
        double square_sum = 0.0;
        std::size_t frame_count = 0;
    };

    void flush_pending();
    void compact();

    std::size_t requested_bins_;
    std::size_t block_frames_ = 1;
    std::size_t total_frames_ = 0;
    Aggregate pending_;
    std::vector<Aggregate> aggregates_;
};

// Selects a normalized [begin, end] view and resamples it to display columns.
// A non-empty source produces exactly requested_bins output bins, including
// when the display is wider than the cached analysis.
std::vector<WaveformBin> resample_waveform(
    std::span<const WaveformBin> source,
    double begin,
    double end,
    std::size_t requested_bins);

} // namespace loop_finder
