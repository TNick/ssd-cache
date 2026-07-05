#include "ssd_cache/job_scheduler.h"

#include <algorithm>
#include <exception>

namespace ssd_cache {

bool PendingCopyScheduler::HeapEntryLater::operator()(
    const HeapEntry& left,
    const HeapEntry& right
) const {
    if (left.due_utc == right.due_utc) {
        return left.sequence > right.sequence;
    }

    return left.due_utc > right.due_utc;
}

PendingCopyScheduler::PendingCopyScheduler(
    CacheIndex& cache_index,
    ICopyEngine& copy_engine,
    const ICopyPathResolver& path_resolver,
    SchedulerSettings settings
)
    : cache_index_(cache_index),
      copy_engine_(copy_engine),
      path_resolver_(path_resolver),
      settings_(settings) {}

PendingCopyScheduler::~PendingCopyScheduler() {
    stop();
}

void PendingCopyScheduler::start() {
    bool expected = false;
    if (!stop_requested_.compare_exchange_strong(expected, false)) {
        stop_requested_ = false;
    }

    if (worker_.joinable()) {
        return;
    }

    load_existing_jobs();
    worker_ = std::thread([this]() {
        worker_loop();
    });
}

void PendingCopyScheduler::stop() {
    stop_requested_ = true;
    wake_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

void PendingCopyScheduler::record_event(const AccessEvent& event) {
    if (event.kind == AccessKind::Delete) {
        cache_index_.mark_removed(event.relative_path);
        return;
    }

    cache_index_.record_access(event, settings_.copy_delay);

    if (should_schedule_copy(event.kind)) {
        enqueue(event.relative_path, event.observed_at + settings_.copy_delay);
    }
}

std::size_t PendingCopyScheduler::run_due_once(
    std::chrono::system_clock::time_point now_utc
) {
    auto jobs = cache_index_.due_jobs(now_utc, settings_.due_batch_size);
    std::size_t completed = 0;

    for (const auto& job : jobs) {
        const auto source = path_resolver_.source_absolute(job.relative_path);
        const auto cache = path_resolver_.cache_absolute(job.relative_path);

        const auto result = copy_engine_.copy_and_hash(
            source,
            cache,
            settings_.compare_hash_before_overwrite
        );

        if (result.action == CopyAction::Failed ||
            result.action == CopyAction::SourceMissing) {
            cache_index_.record_error(job.relative_path, result.error_message);

            const auto retry_seconds = std::min(300, 30 * (job.retries + 1));
            const auto retry_due = now_utc + std::chrono::seconds(retry_seconds);
            cache_index_.upsert_pending_job(
                job.relative_path,
                retry_due,
                "retry"
            );
            enqueue(job.relative_path, retry_due);
            continue;
        }

        cache_index_.mark_cached(
            job.relative_path,
            result.source_size_bytes,
            result.cached_size_bytes,
            result.hash_hex,
            result.completed_at
        );
        cache_index_.remove_pending_job(job.relative_path);
        ++completed;
    }

    return completed;
}

void PendingCopyScheduler::enqueue(
    const std::wstring& relative_path,
    std::chrono::system_clock::time_point due_utc
) {
    {
        std::lock_guard lock(mutex_);
        heap_.push(HeapEntry{due_utc, ++sequence_, relative_path});
    }

    wake_.notify_all();
}

void PendingCopyScheduler::load_existing_jobs() {
    const auto jobs = cache_index_.load_pending_jobs();
    std::lock_guard lock(mutex_);
    for (const auto& job : jobs) {
        heap_.push(HeapEntry{job.due_utc, ++sequence_, job.relative_path});
    }
}

void PendingCopyScheduler::worker_loop() {
    while (!stop_requested_) {
        std::unique_lock lock(mutex_);
        if (heap_.empty()) {
            wake_.wait(lock, [this]() {
                return stop_requested_.load() || !heap_.empty();
            });
            continue;
        }

        const auto next = heap_.top();
        const auto now = std::chrono::system_clock::now();
        if (next.due_utc > now) {
            wake_.wait_until(lock, next.due_utc);
            continue;
        }

        heap_.pop();
        lock.unlock();

        const auto current_job = cache_index_.pending_job(next.relative_path);
        if (!current_job || current_job->due_utc != next.due_utc) {
            continue;
        }

        try {
            run_due_once(std::chrono::system_clock::now());
        } catch (const std::exception& error) {
            cache_index_.record_error(next.relative_path, error.what());
        }
    }
}

}  // namespace ssd_cache
