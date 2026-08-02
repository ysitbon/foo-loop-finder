#include "loop_finder/waveform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

StreamingWaveformReducer::StreamingWaveformReducer(
    std::size_t requested_bins)
    : requested_bins_(requested_bins) {}

void StreamingWaveformReducer::append(std::span<const float> pcm,
                                      std::size_t channels) {
    if (channels == 0) {
        throw std::invalid_argument("Channel count must be positive");
    }
    if (pcm.size() % channels != 0) {
        throw std::invalid_argument(
            "PCM sample count must be divisible by channels");
    }
    if (requested_bins_ == 0 || pcm.empty()) {
        return;
    }

    const std::size_t frames = pcm.size() / channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float mono = 0.0F;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            mono += pcm[frame * channels + channel];
        }
        mono /= static_cast<float>(channels);
        pending_.minimum = (std::min)(pending_.minimum, mono);
        pending_.maximum = (std::max)(pending_.maximum, mono);
        pending_.square_sum += static_cast<double>(mono) * mono;
        ++pending_.frame_count;
        ++total_frames_;
        if (pending_.frame_count == block_frames_) {
            flush_pending();
        }
    }
}

std::vector<WaveformBin> StreamingWaveformReducer::finish() {
    flush_pending();
    if (requested_bins_ == 0 || aggregates_.empty()) {
        return {};
    }

    const std::size_t output_count =
        (std::min)(requested_bins_, aggregates_.size());
    std::vector<WaveformBin> output;
    output.reserve(output_count);
    for (std::size_t bin = 0; bin < output_count; ++bin) {
        const std::size_t begin = bin * aggregates_.size() / output_count;
        const std::size_t end =
            (bin + 1) * aggregates_.size() / output_count;
        Aggregate combined;
        for (std::size_t index = begin; index < end; ++index) {
            const auto& source = aggregates_[index];
            combined.minimum =
                (std::min)(combined.minimum, source.minimum);
            combined.maximum =
                (std::max)(combined.maximum, source.maximum);
            combined.square_sum += source.square_sum;
            combined.frame_count += source.frame_count;
        }
        output.push_back(
            {combined.minimum,
             combined.maximum,
             static_cast<float>(std::sqrt(
                 combined.square_sum /
                 static_cast<double>(combined.frame_count)))});
    }
    return output;
}

std::size_t StreamingWaveformReducer::frame_count() const noexcept {
    return total_frames_;
}

void StreamingWaveformReducer::flush_pending() {
    if (pending_.frame_count == 0) {
        return;
    }
    aggregates_.push_back(pending_);
    pending_ = {};
    if (aggregates_.size() > requested_bins_ * 2) {
        compact();
    }
}

void StreamingWaveformReducer::compact() {
    std::vector<Aggregate> compacted;
    compacted.reserve((aggregates_.size() + 1) / 2);
    for (std::size_t index = 0; index < aggregates_.size(); index += 2) {
        Aggregate combined = aggregates_[index];
        if (index + 1 < aggregates_.size()) {
            const auto& next = aggregates_[index + 1];
            combined.minimum = (std::min)(combined.minimum, next.minimum);
            combined.maximum = (std::max)(combined.maximum, next.maximum);
            combined.square_sum += next.square_sum;
            combined.frame_count += next.frame_count;
        }
        compacted.push_back(combined);
    }
    aggregates_ = std::move(compacted);
    block_frames_ *= 2;
}

std::vector<WaveformBin> resample_waveform(
    std::span<const WaveformBin> source,
    double begin,
    double end,
    std::size_t requested_bins) {
    if (source.empty() || requested_bins == 0 || !std::isfinite(begin) ||
        !std::isfinite(end) || begin < 0.0 || end > 1.0 || begin >= end) {
        return {};
    }

    const double source_begin = begin * static_cast<double>(source.size());
    const double source_end = end * static_cast<double>(source.size());
    const double source_width = source_end - source_begin;
    std::vector<WaveformBin> output;
    output.reserve(requested_bins);

    for (std::size_t bin = 0; bin < requested_bins; ++bin) {
        const double range_begin =
            source_begin + source_width * static_cast<double>(bin) /
                               static_cast<double>(requested_bins);
        const double range_end =
            source_begin + source_width * static_cast<double>(bin + 1) /
                               static_cast<double>(requested_bins);
        const std::size_t first = (std::min)(
            static_cast<std::size_t>(range_begin), source.size() - 1);
        const std::size_t last = (std::min)(
            static_cast<std::size_t>(std::ceil(range_end)), source.size());

        float minimum = std::numeric_limits<float>::max();
        float maximum = std::numeric_limits<float>::lowest();
        double square_sum = 0.0;
        std::size_t count = 0;
        for (std::size_t index = first; index < (std::max)(first + 1, last);
             ++index) {
            const auto& value = source[(std::min)(index, source.size() - 1)];
            minimum = (std::min)(minimum, value.minimum);
            maximum = (std::max)(maximum, value.maximum);
            square_sum += static_cast<double>(value.rms) * value.rms;
            ++count;
        }
        output.push_back(
            {minimum,
             maximum,
             static_cast<float>(std::sqrt(square_sum /
                                          static_cast<double>(count)))});
    }
    return output;
}

} // namespace loop_finder
