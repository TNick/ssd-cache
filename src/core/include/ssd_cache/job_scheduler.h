#pragma once

/**
 * @file
 * @brief Background scheduler that copies accessed files into the cache.
 */

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

/** Tunables controlling copy timing, batching and free-space eviction. */
struct SchedulerSettings {
    /** Delay between observing an access and copying the file. */
    std::chrono::seconds copy_delay{60};
    /** Compare hashes before overwriting an equally sized cache file. */
    bool compare_hash_before_overwrite = false;
    /** Maximum number of due jobs processed per pass. */
    int due_batch_size = 32;
    /**
     * Free-space eviction target. Before caching a file the scheduler evicts the
     * least recently accessed cached files until the cache volume has at least
     * max(incoming file size, min_free_bytes) free. Zero disables eviction.
     */
    std::uint64_t min_free_bytes = 0;
    /**
     * Volume path queried for free space (e.g. "K:\\"). Only used when a free
     * space provider is supplied and min_free_bytes is non-zero.
     */
    std::wstring cache_root;
};

/**
 * Owns a worker thread that copies files into the cache when their scheduled
 * time arrives, persisting the queue in the cache index so it survives restarts.
 */
class PendingCopyScheduler {
public:
    /**
     * @param cache_index Index of files and pending jobs (source of truth).
     * @param copy_engine Engine used to copy and remove cache files.
     * @param path_resolver Resolves relative paths to absolute source/cache paths.
     * @param settings Timing, batching and eviction tunables.
     * @param logger Optional log sink; may be null.
     * @param free_space Optional free-space provider enabling eviction; null
     *        disables eviction regardless of settings.
     */
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

    /** Loads persisted jobs and starts the worker thread (idempotent). */
    void start();

    /** Signals the worker to stop and joins it (idempotent). */
    void stop();

    /**
     * Pauses or resumes copy processing. While paused the worker leaves due jobs
     * pending in the cache index (they are retried once resumed) and
     * record_event stops scheduling new copies. Used to honor Disabled/Serve
     * mode without tearing down the scheduler.
     *
     * @param paused True to pause, false to resume.
     */
    void set_paused(bool paused);

    /**
     * Records an observed access, persisting it and (unless paused) scheduling a
     * copy.
     *
     * @param event The observed access event.
     */
    void record_event(const AccessEvent& event);

    /**
     * Processes the jobs due at a given time, copying each into the cache. Public
     * for direct/testing use; the worker thread calls it internally.
     *
     * @param now_utc The current time used to select due jobs.
     * @return The number of files successfully cached in this pass.
     */
    std::size_t run_due_once(std::chrono::system_clock::time_point now_utc);

private:
    /** One entry in the due-time priority queue. */
    struct HeapEntry {
        std::chrono::system_clock::time_point due_utc;
        std::uint64_t sequence = 0;
        std::wstring relative_path;
    };

    /** Orders heap entries by due time, then insertion sequence (min-heap). */
    struct HeapEntryLater {
        /**
         * @param left First entry.
         * @param right Second entry.
         * @return True if @p left should be ordered after @p right.
         */
        bool operator()(const HeapEntry& left, const HeapEntry& right) const;
    };

    /**
     * Pushes a due-time entry onto the heap and wakes the worker.
     *
     * @param relative_path File to copy.
     * @param due_utc When the copy becomes due.
     */
    void enqueue(
        const std::wstring& relative_path,
        std::chrono::system_clock::time_point due_utc
    );

    /** Loads pending jobs from the cache index into the in-memory heap. */
    void load_existing_jobs();

    /** Worker thread loop: waits for due jobs and copies them until stopped. */
    void worker_loop();

    /**
     * Evicts the least recently accessed cached files until the cache volume has
     * at least max(incoming size for @p relative_path, settings_.min_free_bytes)
     * free. The file being cached is never chosen as a victim. No-op when
     * eviction is disabled or no free space provider was supplied.
     *
     * @param relative_path File about to be cached (its size sets the target and
     *        it is excluded from eviction).
     */
    void make_room_for(const std::wstring& relative_path);

    /**
     * Logs a message if a logger was supplied.
     *
     * @param message The message to log.
     */
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
