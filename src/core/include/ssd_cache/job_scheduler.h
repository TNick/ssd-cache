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

namespace ssd_cache {

struct SchedulerSettings {
    std::chrono::seconds copy_delay{60};
    bool compare_hash_before_overwrite = false;
    int due_batch_size = 32;
};

class PendingCopyScheduler {
public:
    PendingCopyScheduler(
        CacheIndex& cache_index,
        ICopyEngine& copy_engine,
        const ICopyPathResolver& path_resolver,
        SchedulerSettings settings
    );

    PendingCopyScheduler(const PendingCopyScheduler&) = delete;
    PendingCopyScheduler& operator=(const PendingCopyScheduler&) = delete;

    ~PendingCopyScheduler();

    void start();

    void stop();

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

    CacheIndex& cache_index_;
    ICopyEngine& copy_engine_;
    const ICopyPathResolver& path_resolver_;
    SchedulerSettings settings_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::priority_queue<
        HeapEntry,
        std::vector<HeapEntry>,
        HeapEntryLater
    > heap_;
    std::thread worker_;
    std::atomic_bool stop_requested_{false};
    std::uint64_t sequence_ = 0;
};

}  // namespace ssd_cache
