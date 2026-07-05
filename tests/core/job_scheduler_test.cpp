#include "test_macros.h"

#include <filesystem>

#include "ssd_cache/job_scheduler.h"
#include "ssd_cache/path_mapper.h"

namespace {

class FakeCopyEngine final : public ssd_cache::ICopyEngine {
public:
    ssd_cache::CopyResult copy_and_hash(
        const std::wstring&,
        const std::wstring&,
        bool
    ) override {
        ++calls;
        ssd_cache::CopyResult result;
        result.action = ssd_cache::CopyAction::Copied;
        result.source_size_bytes = 7;
        result.cached_size_bytes = 7;
        result.hash_hex = "hash";
        result.completed_at = std::chrono::system_clock::now();
        return result;
    }

    int calls = 0;
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
