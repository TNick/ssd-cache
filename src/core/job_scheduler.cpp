#include "ssd_cache/job_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>

#include "ssd_cache/time.h"
#include "ssd_cache/utf.h"

namespace ssd_cache {
namespace {

// The cache index persists timestamps at whole-second resolution (see
// format_utc). Heap entries, however, are built from system_clock::now() which
// carries sub-second precision, so an in-memory due time would never compare
// equal to the same job reloaded from the database. Flooring every due time to
// seconds before it enters the heap keeps the two representations identical,
// which the worker relies on to tell a live job apart from a stale heap entry.
std::chrono::system_clock::time_point floor_to_seconds(
    std::chrono::system_clock::time_point value
) {
    return std::chrono::time_point_cast<std::chrono::seconds>(value);
}

const char* copy_action_name(CopyAction action) {
    switch (action) {
        case CopyAction::Copied:
            return "copied";
        case CopyAction::SkippedSameSize:
            return "skipped (same size)";
        case CopyAction::SkippedHashMatch:
            return "skipped (hash match)";
        case CopyAction::SourceMissing:
            return "source missing";
        case CopyAction::Failed:
            return "failed";
        case CopyAction::Cancelled:
            return "cancelled";
    }

    return "unknown";
}

std::string narrow_path(const std::wstring& relative_path) {
    return wide_to_utf8(relative_path);
}

}  // namespace

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
    SchedulerSettings settings,
    ILogger* logger,
    const IFreeSpaceProvider* free_space
)
    : cache_index_(cache_index),
      copy_engine_(copy_engine),
      path_resolver_(path_resolver),
      settings_(settings),
      logger_(logger),
      free_space_(free_space) {}

void PendingCopyScheduler::log(const std::string& message) const noexcept {
    if (logger_ != nullptr) {
        logger_->log(message);
    }
}

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

void PendingCopyScheduler::set_paused(bool paused) {
    const bool was_paused = paused_.exchange(paused);
    if (was_paused == paused) {
        return;
    }

    log(paused ? "scheduler paused" : "scheduler resumed");
    // Wake the worker so a resume re-evaluates due jobs immediately and a pause
    // stops it from blocking on a now-irrelevant wait_until.
    wake_.notify_all();
}

void PendingCopyScheduler::record_event(const AccessEvent& event) {
    if (event.relative_path.empty()) {
        log("event ignored: empty relative path");
        return;
    }

    if (paused_) {
        log("event ignored: scheduler paused ('" +
            narrow_path(event.relative_path) + "')");
        return;
    }

    if (event.kind == AccessKind::Delete) {
        log("event delete: '" + narrow_path(event.relative_path) +
            "' -> marking removed from cache index");
        cache_index_.mark_removed(event.relative_path);
        return;
    }

    cache_index_.record_access(event, settings_.copy_delay);

    if (should_schedule_copy(event.kind)) {
        const auto due = event.observed_at + settings_.copy_delay;
        log("event " + access_kind_to_string(event.kind) + ": '" +
            narrow_path(event.relative_path) + "' recorded; copy due " +
            format_utc(due));
        enqueue(event.relative_path, due);
    } else {
        log("event " + access_kind_to_string(event.kind) + ": '" +
            narrow_path(event.relative_path) +
            "' recorded; no copy scheduled");
    }
}

void PendingCopyScheduler::make_room_for(const std::wstring& relative_path) {
    if (free_space_ == nullptr || settings_.min_free_bytes == 0) {
        return;
    }

    std::uint64_t incoming_size = 0;
    if (const auto record = cache_index_.file_record(relative_path)) {
        incoming_size = record->source_size_bytes;
    }
    const std::uint64_t required =
        std::max(incoming_size, settings_.min_free_bytes);

    auto free = free_space_->free_bytes(settings_.cache_root);
    if (!free) {
        log("free-space check skipped: could not read free space on '" +
            wide_to_utf8(settings_.cache_root) + "'");
        return;
    }
    if (*free >= required) {
        return;
    }

    log("free-space low on '" + wide_to_utf8(settings_.cache_root) + "': " +
        std::to_string(*free) + " free < required " + std::to_string(required) +
        " bytes; evicting oldest cached files");

    for (const auto& victim : cache_index_.cached_files_by_last_access()) {
        if (*free >= required) {
            break;
        }
        if (victim.relative_path == relative_path) {
            // Never evict the file we are about to (re)cache.
            continue;
        }

        const auto victim_cache =
            path_resolver_.cache_absolute(victim.relative_path);
        const auto remove_result = copy_engine_.remove_cached_file(victim_cache);
        if (!remove_result.error_message.empty()) {
            log("eviction failed for '" + narrow_path(victim.relative_path) +
                "': " + remove_result.error_message);
            continue;
        }

        cache_index_.mark_removed(victim.relative_path);
        log("evicted '" + narrow_path(victim.relative_path) + "' (" +
            std::to_string(victim.cached_size_bytes) + " bytes)");

        // Re-read the real free space after each deletion; fall back to a
        // size estimate only if the volume query fails mid-eviction.
        if (const auto updated = free_space_->free_bytes(settings_.cache_root)) {
            free = updated;
        } else {
            *free += victim.cached_size_bytes;
        }
    }

    if (*free < required) {
        log("free-space still below target after evicting all candidates: " +
            std::to_string(*free) + " free < " + std::to_string(required) +
            " bytes");
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
        const auto path_text = narrow_path(job.relative_path);

        make_room_for(job.relative_path);

        log("copy start: '" + path_text + "' (" + job.reason + ") " +
            wide_to_utf8(source) + " -> " + wide_to_utf8(cache));

        const auto result = copy_engine_.copy_and_hash(
            source,
            cache,
            settings_.compare_hash_before_overwrite,
            stop_requested_
        );

        if (result.action == CopyAction::Cancelled) {
            // Shutting down mid-copy. Leave the pending job in place so it is
            // retried on the next start, and stop processing this batch.
            log("copy cancelled: '" + path_text + "' (service stopping)");
            break;
        }

        if (result.action == CopyAction::SourceMissing) {
            const auto remove_result = copy_engine_.remove_cached_file(cache);
            if (remove_result.error_message.empty()) {
                cache_index_.mark_removed(job.relative_path);

                std::string remove_text = "cache file removed";
                if (remove_result.missing) {
                    remove_text = "cache file already missing";
                }

                log("copy source missing: '" + path_text + "': " +
                    result.error_message + "; " + remove_text);
                continue;
            }

            cache_index_.record_error(
                job.relative_path,
                remove_result.error_message
            );

            const auto retry_seconds = std::min(300, 30 * (job.retries + 1));
            const auto retry_due = now_utc + std::chrono::seconds(retry_seconds);
            cache_index_.upsert_pending_job(
                job.relative_path,
                retry_due,
                "remove stale cache"
            );
            enqueue(job.relative_path, retry_due);
            log("copy source missing: '" + path_text + "': " +
                result.error_message + "; failed to remove cache file: " +
                remove_result.error_message + "; cleanup retry " +
                std::to_string(job.retries + 1) + " due " +
                format_utc(retry_due));
            continue;
        }

        if (result.action == CopyAction::Failed) {
            cache_index_.record_error(job.relative_path, result.error_message);

            const auto retry_seconds = std::min(300, 30 * (job.retries + 1));
            const auto retry_due = now_utc + std::chrono::seconds(retry_seconds);
            cache_index_.upsert_pending_job(
                job.relative_path,
                retry_due,
                "retry"
            );
            enqueue(job.relative_path, retry_due);
            log("copy " + std::string(copy_action_name(result.action)) +
                ": '" + path_text + "': " + result.error_message +
                "; retry " + std::to_string(job.retries + 1) + " due " +
                format_utc(retry_due));
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
        log("copy " + std::string(copy_action_name(result.action)) + ": '" +
            path_text + "' (" + std::to_string(result.cached_size_bytes) +
            " bytes)");
    }

    return completed;
}

void PendingCopyScheduler::enqueue(
    const std::wstring& relative_path,
    std::chrono::system_clock::time_point due_utc
) {
    // Match the whole-second resolution the cache index persists, so the entry
    // popped from the heap compares equal to the same job reloaded from the
    // database in worker_loop.
    const auto due_seconds = floor_to_seconds(due_utc);
    {
        std::lock_guard lock(mutex_);
        heap_.push(HeapEntry{due_seconds, ++sequence_, relative_path});
    }

    wake_.notify_all();
}

void PendingCopyScheduler::load_existing_jobs() {
    const auto jobs = cache_index_.load_pending_jobs();
    std::lock_guard lock(mutex_);
    for (const auto& job : jobs) {
        heap_.push(
            HeapEntry{floor_to_seconds(job.due_utc), ++sequence_,
                      job.relative_path}
        );
    }

    log("loaded " + std::to_string(jobs.size()) +
        " pending job(s) from cache index");
}

void PendingCopyScheduler::worker_loop() {
    while (!stop_requested_) {
        std::unique_lock lock(mutex_);
        if (paused_) {
            // Leave any due jobs pending in the cache index until resumed.
            wake_.wait(lock, [this]() {
                return stop_requested_.load() || !paused_.load();
            });
            continue;
        }

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

        const auto path_text = narrow_path(next.relative_path);

        if (paused_) {
            // A pause landed between popping and running. Put the entry back so
            // the job is not lost, and let the top-of-loop pause wait catch it.
            enqueue(next.relative_path, next.due_utc);
            continue;
        }

        const auto current_job = cache_index_.pending_job(next.relative_path);
        if (!current_job) {
            log("skip '" + path_text +
                "': no pending job in cache index (already handled or removed)");
            continue;
        }
        if (current_job->due_utc != next.due_utc) {
            // A newer access rescheduled this path to a later due time; the
            // current heap entry is stale. The authoritative entry is still in
            // the heap and will be picked up when it comes due.
            log("skip '" + path_text + "': stale heap entry (heap due " +
                format_utc(next.due_utc) + ", index due " +
                format_utc(current_job->due_utc) + ")");
            continue;
        }

        log("due '" + path_text + "': running copy");
        try {
            run_due_once(std::chrono::system_clock::now());
        } catch (const std::exception& error) {
            log("copy threw for '" + path_text + "': " + error.what());
            cache_index_.record_error(next.relative_path, error.what());
        }
    }
}

}  // namespace ssd_cache
