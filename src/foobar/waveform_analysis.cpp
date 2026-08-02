#ifdef _WIN32

#include "waveform_analysis.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace loop_finder::foobar {

namespace {

constexpr std::size_t kAnalysisBins = 16'384;

} // namespace

std::string make_track_identity(const metadb_handle_ptr& track) {
    if (track.is_empty()) {
        return {};
    }

    std::ostringstream identity;
    identity << "waveform-v" << kWaveformAnalysisFormatVersion << '|'
             << track->get_path() << '|' << track->get_subsong_index() << '|'
             << track->get_filesize() << '|' << track->get_filetimestamp();
    return identity.str();
}

WaveformCache::WaveformCache(std::size_t maximum_entries)
    : maximum_entries_(maximum_entries) {}

WaveformSnapshotPtr WaveformCache::find(const std::string& identity) {
    const auto found = entries_.find(identity);
    if (found == entries_.end()) {
        return {};
    }
    recency_.splice(recency_.begin(), recency_, found->second.recency);
    return found->second.snapshot;
}

void WaveformCache::store(std::string identity, WaveformSnapshotPtr snapshot) {
    if (maximum_entries_ == 0 || identity.empty() || !snapshot) {
        return;
    }

    const auto existing = entries_.find(identity);
    if (existing != entries_.end()) {
        existing->second.snapshot = std::move(snapshot);
        recency_.splice(recency_.begin(), recency_, existing->second.recency);
        return;
    }

    recency_.push_front(identity);
    entries_.emplace(recency_.front(), Entry{std::move(snapshot), recency_.begin()});
    while (entries_.size() > maximum_entries_) {
        entries_.erase(recency_.back());
        recency_.pop_back();
    }
}

std::size_t WaveformCache::size() const noexcept {
    return entries_.size();
}

WaveformAnalysis::WaveformAnalysis(Completion completion)
    : delivery_(std::make_shared<DeliveryState>(
          DeliveryState{true, std::move(completion)})),
      worker_([this] { worker_loop(); }) {}

WaveformAnalysis::~WaveformAnalysis() {
    core_api::ensure_main_thread();
    delivery_->enabled = false;
    delivery_->completion = {};
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
        if (active_aborter_) {
            active_aborter_->set();
        }
    }
    wake_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void WaveformAnalysis::request(std::uint64_t generation,
                               std::string identity,
                               metadb_handle_ptr track) {
    core_api::ensure_main_thread();
    {
        std::lock_guard lock(mutex_);
        if (active_aborter_) {
            active_aborter_->set();
        }
        pending_ = Job{generation, std::move(identity), std::move(track)};
    }
    wake_.notify_one();
}

void WaveformAnalysis::cancel() {
    core_api::ensure_main_thread();
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        if (active_aborter_) {
            active_aborter_->set();
        }
    }
    wake_.notify_one();
}

void WaveformAnalysis::worker_loop() {
    for (;;) {
        Job job;
        auto aborter = std::make_shared<abort_callback_impl>();
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) {
                return;
            }
            job = std::move(*pending_);
            pending_.reset();
            active_aborter_ = aborter;
        }

        WaveformSnapshotPtr snapshot;
        std::string error;
        try {
            snapshot = decode(job.track, *aborter);
        } catch (const exception_aborted&) {
            // Cancellation is an expected state transition. A newer generation
            // or stop callback already owns what the panel should display.
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown decoder failure";
        }

        const bool aborted = aborter->is_set();
        {
            std::lock_guard lock(mutex_);
            if (active_aborter_ == aborter) {
                active_aborter_.reset();
            }
        }
        if (!aborted) {
            deliver(delivery_,
                    std::move(job),
                    std::move(snapshot),
                    std::move(error),
                    *aborter);
        }
    }
}

WaveformSnapshotPtr WaveformAnalysis::decode(const metadb_handle_ptr& track,
                                             abort_callback& aborter) {
    aborter.check();
    if (track.is_empty()) {
        throw std::runtime_error("No track is available for analysis");
    }

    const std::string path = track->get_path();
    if (filesystem::g_is_remote_or_unrecognized(path.c_str())) {
        throw std::runtime_error("Remote or unsupported source");
    }

    const auto read_lock =
        file_lock_manager::get()->acquire_read(path.c_str(), aborter);
    input_decoder::ptr decoder;
    input_entry::g_open_for_decoding(decoder, nullptr, path.c_str(), aborter);

    file_info_impl info;
    const auto subsong = track->get_subsong_index();
    decoder->get_info(subsong, info, aborter);
    const double duration = info.get_length();
    if (!std::isfinite(duration) || duration <= 0.0) {
        throw std::runtime_error("Track length is zero or unknown");
    }

    decoder->initialize(subsong, input_flag_simpledecode, aborter);
    StreamingWaveformReducer reducer(kAnalysisBins);
    audio_chunk_impl chunk;
    std::vector<float> pcm;
    while (decoder->run(chunk, aborter)) {
        aborter.check();
        if (chunk.is_empty()) {
            continue;
        }
        pcm.resize(chunk.get_used_size());
        for (std::size_t index = 0; index < pcm.size(); ++index) {
            pcm[index] = static_cast<float>(chunk.get_data()[index]);
        }
        reducer.append(
            pcm,
            chunk.get_channels());
    }
    aborter.check();

    auto bins = reducer.finish();
    if (bins.empty()) {
        throw std::runtime_error("Decoder produced no PCM audio");
    }

    auto snapshot = std::make_shared<WaveformSnapshot>();
    snapshot->bins = std::move(bins);
    snapshot->duration_seconds = duration;
    snapshot->decoded_frames = reducer.frame_count();
    return snapshot;
}

void WaveformAnalysis::deliver(const std::shared_ptr<DeliveryState>& state,
                               Job job,
                               WaveformSnapshotPtr snapshot,
                               std::string error,
                               abort_callback& aborter) {
    fb2k::inMainThread(
        [state,
         generation = job.generation,
         identity = std::move(job.identity),
         snapshot = std::move(snapshot),
         error = std::move(error)]() mutable {
            if (!state->enabled || !state->completion) {
                return;
            }
            state->completion(generation,
                              identity,
                              std::move(snapshot),
                              error);
        },
        aborter);
}

} // namespace loop_finder::foobar

#endif
