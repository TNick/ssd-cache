#include "test_macros.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "ssd_cache/job_scheduler.h"
#include "ssd_cache/path_mapper.h"

namespace {

class FakeCopyEngine final : public ssd_cache::ICopyEngine {
public:
    ssd_cache::CopyResult copy_and_hash(
        const std::wstring&,
        const std::wstring&,
        bool,
        const std::atomic_bool&
    ) override {
        ++calls;
        ssd_cache::CopyResult result;
        result.action = action;
        result.source_size_bytes = 7;
        result.cached_size_bytes = 7;
        result.hash_hex = "hash";
        result.completed_at = std::chrono::system_clock::now();
        result.error_message = error_message;
        return result;
    }

    ssd_cache::RemoveResult remove_cached_file(
        const std::wstring&
    ) override {
        ++remove_calls;
        if (free_on_remove != nullptr) {
            *free_on_remove += bytes_freed_per_remove;
        }
        return remove_result;
    }

    ssd_cache::CopyAction action = ssd_cache::CopyAction::Copied;
    ssd_cache::RemoveResult remove_result;
    std::string error_message;
    int calls = 0;
    int remove_calls = 0;
    // When set, each remove_cached_file() bumps the pointed-to free-space
    // counter, letting a test model disk space recovered by eviction.
    std::uint64_t* free_on_remove = nullptr;
    std::uint64_t bytes_freed_per_remove = 0;
};

class FakeFreeSpace final : public ssd_cache::IFreeSpaceProvider {
public:
    std::optional<std::uint64_t> free_bytes(
        const std::wstring&
    ) const override {
        return free;
    }

    std::uint64_t free = 0;
};

class FakeResolver final : public ssd_cache::ICopyPathResolver {
public:
    std::wstring source_absolute(
        const std::wstring& relative_path
    ) const override {
        return ssd_cache::join_root_relative(L"\\\\nas\\share", relative_path);
    }

    std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const override {
        return ssd_cache::join_root_relative(L"K:\\", relative_path);
    }
};

std::wstring scheduler_db_path() {
    auto path = std::filesystem::temp_directory_path();
    path /= L"ssd-cache-scheduler-test.sqlite3";
    std::filesystem::remove(path);
    return path.wstring();
}

}  // namespace

TEST_CASE("scheduler copies due job and updates cache index") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    FakeCopyEngine copy_engine;
    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings
    );

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"file.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();

    scheduler.record_event(event);
    const auto copied = scheduler.run_due_once(
        std::chrono::system_clock::now() + std::chrono::seconds(1)
    );

    REQUIRE(copied == 1);
    REQUIRE(copy_engine.calls == 1);

    const auto record = index.file_record(event.relative_path);
    REQUIRE(record.has_value());
    REQUIRE(record->cache_present);
    REQUIRE(record->hash_hex == "hash");
    REQUIRE(!index.pending_job(event.relative_path).has_value());
}

TEST_CASE("scheduler removes cache entry when source disappears") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    FakeCopyEngine copy_engine;
    copy_engine.action = ssd_cache::CopyAction::SourceMissing;
    copy_engine.error_message = "source file does not exist";
    copy_engine.remove_result.removed = true;

    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings
    );

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"removed.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();

    scheduler.record_event(event);
    const auto copied = scheduler.run_due_once(
        std::chrono::system_clock::now() + std::chrono::seconds(1)
    );

    REQUIRE(copied == 0);
    REQUIRE(copy_engine.calls == 1);
    REQUIRE(copy_engine.remove_calls == 1);
    REQUIRE(!index.pending_job(event.relative_path).has_value());

    const auto record = index.file_record(event.relative_path);
    REQUIRE(record.has_value());
    REQUIRE(!record->cache_present);
}

TEST_CASE("scheduler ignores empty relative path events") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    FakeCopyEngine copy_engine;
    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings
    );

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();

    scheduler.record_event(event);
    const auto copied = scheduler.run_due_once(
        std::chrono::system_clock::now() + std::chrono::seconds(1)
    );

    REQUIRE(copied == 0);
    REQUIRE(copy_engine.calls == 0);
    REQUIRE(!index.file_record(L"").has_value());
    REQUIRE(!index.pending_job(L"").has_value());
}

namespace {

void seed_cached_file(
    ssd_cache::CacheIndex& index,
    const std::wstring& path,
    std::uint64_t size,
    std::chrono::system_clock::time_point cached_at
) {
    ssd_cache::AccessEvent event;
    event.relative_path = path;
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.size_hint = size;
    event.observed_at = cached_at;
    index.record_access(event, std::chrono::seconds(0));
    index.remove_pending_job(path);
    index.mark_cached(path, size, size, "hash", cached_at);
}

}  // namespace

TEST_CASE("scheduler evicts oldest cached files to reach free-space target") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    const auto base = std::chrono::system_clock::now();
    seed_cached_file(index, L"old.txt", 50, base + std::chrono::seconds(1));
    seed_cached_file(index, L"mid.txt", 50, base + std::chrono::seconds(2));
    seed_cached_file(index, L"new.txt", 50, base + std::chrono::seconds(3));

    FakeFreeSpace free_space;
    free_space.free = 10;  // below the 100-byte target

    FakeCopyEngine copy_engine;
    copy_engine.free_on_remove = &free_space.free;
    copy_engine.bytes_freed_per_remove = 50;

    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);
    settings.min_free_bytes = 100;
    settings.cache_root = L"K:\\";

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings,
        nullptr,
        &free_space
    );

    ssd_cache::AccessEvent target;
    target.relative_path = L"target.txt";
    target.kind = ssd_cache::AccessKind::ReadOpen;
    target.size_hint = 5;  // smaller than threshold: target == min_free_bytes
    target.observed_at = base;
    scheduler.record_event(target);

    const auto copied = scheduler.run_due_once(base + std::chrono::seconds(1));

    REQUIRE(copied == 1);
    // free: 10 -> 60 -> 110 (>= 100), so exactly the two oldest are evicted.
    REQUIRE(copy_engine.remove_calls == 2);
    REQUIRE(!index.file_record(L"old.txt")->cache_present);
    REQUIRE(!index.file_record(L"mid.txt")->cache_present);
    REQUIRE(index.file_record(L"new.txt")->cache_present);
    REQUIRE(index.file_record(L"target.txt")->cache_present);
}

TEST_CASE("scheduler does not evict when free space is sufficient") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    const auto base = std::chrono::system_clock::now();
    seed_cached_file(index, L"old.txt", 50, base + std::chrono::seconds(1));

    FakeFreeSpace free_space;
    free_space.free = 1000;  // already above target

    FakeCopyEngine copy_engine;
    copy_engine.free_on_remove = &free_space.free;
    copy_engine.bytes_freed_per_remove = 50;

    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);
    settings.min_free_bytes = 100;
    settings.cache_root = L"K:\\";

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings,
        nullptr,
        &free_space
    );

    ssd_cache::AccessEvent target;
    target.relative_path = L"target.txt";
    target.kind = ssd_cache::AccessKind::ReadOpen;
    target.size_hint = 5;
    target.observed_at = base;
    scheduler.record_event(target);

    const auto copied = scheduler.run_due_once(base + std::chrono::seconds(1));

    REQUIRE(copied == 1);
    REQUIRE(copy_engine.remove_calls == 0);
    REQUIRE(index.file_record(L"old.txt")->cache_present);
}

TEST_CASE("paused scheduler does not schedule new events") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    FakeCopyEngine copy_engine;
    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings
    );
    scheduler.set_paused(true);

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"paused.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();

    scheduler.record_event(event);
    const auto copied = scheduler.run_due_once(
        std::chrono::system_clock::now() + std::chrono::seconds(1)
    );

    REQUIRE(copied == 0);
    REQUIRE(copy_engine.calls == 0);
    REQUIRE(!index.pending_job(event.relative_path).has_value());
    REQUIRE(!index.file_record(event.relative_path).has_value());
}

TEST_CASE("paused worker holds jobs until resumed") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    FakeCopyEngine copy_engine;
    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings
    );

    // Seed a due pending job through the normal path (which creates the file
    // record the pending job references) before the worker runs, then start the
    // worker paused so the held job is deterministically untouched.
    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"held.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();
    scheduler.record_event(event);
    REQUIRE(index.pending_job(L"held.txt").has_value());

    scheduler.set_paused(true);
    scheduler.start();

    // While paused the pending job must survive untouched.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(copy_engine.calls == 0);
    REQUIRE(index.pending_job(L"held.txt").has_value());

    // Resuming must let the worker pick the held job up and copy it.
    scheduler.set_paused(false);

    bool copied = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!index.pending_job(L"held.txt").has_value()) {
            copied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    scheduler.stop();

    REQUIRE(copied);
    REQUIRE(copy_engine.calls == 1);
}

// Regression test for the silent-skip bug: the worker thread reloads each job
// from the cache index (whole-second timestamps) and compares it against the
// in-memory heap entry. If the heap entry keeps sub-second precision it never
// matches, and the copy is skipped with no error. This drives the real worker
// loop (not run_due_once directly) so that regression would fail here.
TEST_CASE("scheduler worker copies live-enqueued job") {
    ssd_cache::CacheIndex index(scheduler_db_path());
    index.open();

    FakeCopyEngine copy_engine;
    FakeResolver resolver;
    ssd_cache::SchedulerSettings settings;
    settings.copy_delay = std::chrono::seconds(0);

    ssd_cache::PendingCopyScheduler scheduler(
        index,
        copy_engine,
        resolver,
        settings
    );
    scheduler.start();

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"live.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();
    scheduler.record_event(event);

    bool copied = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!index.pending_job(event.relative_path).has_value()) {
            copied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    scheduler.stop();

    REQUIRE(copied);
    REQUIRE(copy_engine.calls == 1);

    const auto record = index.file_record(event.relative_path);
    REQUIRE(record.has_value());
    REQUIRE(record->cache_present);
}
