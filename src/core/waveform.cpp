#include "loop_finder/waveform.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loop_finder {

std::vector<WaveformBin> reduce_waveform(std::span<const float> pcm,
                                        std::size_t channels,
                                        std::size_t requested_bins) {
    if (channels == 0 || requested_bins == 0 || pcm.empty()) return {};
    if (pcm.size() % channels != 0)
        throw std::invalid_argument("PCM sample count must be divisible by channels");

    const std::size_t frames = pcm.size() / channels;
    const std::size_t bin_count = std::min(requested_bins, frames);
    std::vector<WaveformBin> bins;
    bins.reserve(bin_count);

    for (std::size_t bin = 0; bin < bin_count; ++bin) {
        const std::size_t begin = bin * frames / bin_count;
        const std::size_t end = (bin + 1) * frames / bin_count;
        float minimum = 1.0F, maximum = -1.0F;
        double square_sum = 0.0;
        std::size_t count = 0;
        for (std::size_t frame = begin; frame < end; ++frame) {
            float mono = 0.0F;
            for (std::size_t channel = 0; channel < channels; ++channel)
                mono += pcm[frame * channels + channel];
            mono /= static_cast<float>(channels);
            minimum = std::min(minimum, mono);
            maximum = std::max(maximum, mono);
            square_sum += static_cast<double>(mono) * mono;
            ++count;
        }
        bins.push_back({minimum, maximum,
                        static_cast<float>(std::sqrt(square_sum / count))});
    }
    return bins;
}

} // namespace loop_finder

