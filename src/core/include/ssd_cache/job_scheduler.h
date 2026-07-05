#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "ssd_cache/access_event.h"
#include "ssd_cache/cache_index.h"
#include "ssd_cache/copy_engine.h"
#include "ssd_cache/logger.h"

namespace ssd_cache {

struct SchedulerSettings {
    std::chrono::seconds copy_delay{60};
    bool compare_hash_before_overwrite = false;
    int due_batch_size = 32;
    // Free-space eviction target. Before caching a file the scheduler evicts the
    // least recently accessed cached files until the cache volume has at least
    // max(incoming file size, min_free_bytes) free. Zero disables eviction.
    std::uint64_t min_free_bytes = 0;
    // Volume path queried for free space (e.g. "K:\\"). Only used when a free
    // space provider is supplied and min_free_bytes is non-zero.
    std::wstring cache_root;
};

class PendingCopyScheduler {
public:
    PendingCopyScheduler(
        CacheIndex& cache_index,
        ICopyEngine& copy_engine,
        const ICopyPathResolver& path_resolver,
        SchedulerSettings settings,
        ILogger* logger = nullptr,
        const IFreeSpaceProvider* free_space = nullptr
    );

    PendingCopyScheduler(const PendingCopyScheduler&) = delete;
    PendingCopyScheduler& operator=(const PendingCopyScheduler&) = delete;

    ~PendingCopyScheduler();

    void start();

    void stop();

    // Pauses or resumes copy processing. While paused the worker leaves due
    // jobs pending in the cache index (they are retried once resumed) and
    // record_event stops scheduling new copies. Used to honor Disabled/Serve
    // mode without tearing down the scheduler.
    void set_paused(bool paused);

    void record_event(const AccessEvent& event);

    std::size_t run_due_once(std::chrono::system_clock::time_point now_utc);

private:
    struct HeapEntry {
        std::chrono::system_clock::time_point due_utc;
        std::uint64_t sequence = 0;
        std::wstring relative_path;
    };

    struct HeapEntryLater {
        bool operator()(const HeapEntry& left, const HeapEntry& right) const;
    };

    void enqueue(
        const std::wstring& relative_path,
        std::chrono::system_clock::time_point due_utc
    );

    void load_existing_jobs();

    void worker_loop();

    // Evicts the least recently accessed cached files until the cache volume
    // has at least max(incoming size for relative_path, settings_.min_free_bytes)
    // free. The file being cached is never chosen as a victim. No-op when
    // eviction is disabled or no free space provider was supplied.
    void make_room_for(const std::wstring& relative_path);

    void log(const std::string& message) const noexcept;

    CacheIndex& cache_index_;
    ICopyEngine& copy_engine_;
    const ICopyPathResolver& path_resolver_;
    SchedulerSettings settings_;
    ILogger* logger_ = nullptr;
    const IFreeSpaceProvider* free_space_ = nullptr;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::priority_queue<
        HeapEntry,
        std::vector<HeapEntry>,
        HeapEntryLater
    > heap_;
    std::thread worker_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool paused_{false};
    std::uint64_t sequence_ = 0;
};

}  // namespace ssd_cache
