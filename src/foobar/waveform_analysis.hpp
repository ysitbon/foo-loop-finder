#pragma once

#ifdef _WIN32

#include <foobar2000/SDK/foobar2000.h>

#include "loop_finder/waveform.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace loop_finder::foobar {

inline constexpr std::uint32_t kWaveformAnalysisFormatVersion = 1;

struct WaveformSnapshot {
    std::vector<WaveformBin> bins;
    double duration_seconds = 0.0;
    std::uint64_t decoded_frames = 0;
};

using WaveformSnapshotPtr = std::shared_ptr<const WaveformSnapshot>;

std::string make_track_identity(const metadb_handle_ptr& track);

class WaveformCache {
public:
    explicit WaveformCache(std::size_t maximum_entries = 8);

    WaveformSnapshotPtr find(const std::string& identity);
    void store(std::string identity, WaveformSnapshotPtr snapshot);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Entry {
        WaveformSnapshotPtr snapshot;
        std::list<std::string>::iterator recency;
    };

    const std::size_t maximum_entries_;
    std::list<std::string> recency_;
    std::unordered_map<std::string, Entry> entries_;
};

class WaveformAnalysis final {
public:
    using Completion = std::function<void(std::uint64_t generation,
                                          const std::string& identity,
                                          WaveformSnapshotPtr snapshot,
                                          const std::string& error)>;

    explicit WaveformAnalysis(Completion completion);
    ~WaveformAnalysis();

    WaveformAnalysis(const WaveformAnalysis&) = delete;
    WaveformAnalysis& operator=(const WaveformAnalysis&) = delete;

    void request(std::uint64_t generation,
                 std::string identity,
                 metadb_handle_ptr track);
    void cancel();

private:
    struct Job {
        std::uint64_t generation = 0;
        std::string identity;
        metadb_handle_ptr track;
    };

    struct DeliveryState {
        bool enabled = true;
        Completion completion;
    };

    void worker_loop();
    static WaveformSnapshotPtr decode(const metadb_handle_ptr& track,
                                      abort_callback& aborter);
    static void deliver(const std::shared_ptr<DeliveryState>& state,
                        Job job,
                        WaveformSnapshotPtr snapshot,
                        std::string error,
                        abort_callback& aborter);

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::optional<Job> pending_;
    std::shared_ptr<abort_callback_impl> active_aborter_;
    std::shared_ptr<DeliveryState> delivery_;
    std::thread worker_;
};

} // namespace loop_finder::foobar

#endif
