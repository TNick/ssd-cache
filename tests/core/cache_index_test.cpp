#include "test_macros.h"

#include <filesystem>

#include "ssd_cache/cache_index.h"

namespace {

std::wstring test_db_path(const wchar_t* name) {
    auto path = std::filesystem::temp_directory_path();
    path /= name;
    std::filesystem::remove(path);
    return path.wstring();
}

}  // namespace

TEST_CASE("cache index records access and pending job") {
    ssd_cache::CacheIndex index(test_db_path(L"ssd-cache-index-test.sqlite3"));
    index.open();

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"a\\b.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.size_hint = 123;
    event.observed_at = std::chrono::system_clock::now();

    index.record_access(event, std::chrono::seconds(60));

    const auto record = index.file_record(event.relative_path);
    REQUIRE(record.has_value());
    REQUIRE(record->source_size_bytes == 123);

    const auto pending = index.pending_job(event.relative_path);
    REQUIRE(pending.has_value());
    REQUIRE(pending->reason == "read_open");
}

TEST_CASE("cache index does not schedule copy on write observed") {
    ssd_cache::CacheIndex index(test_db_path(L"ssd-cache-index-write.sqlite3"));
    index.open();

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"dirty.txt";
    event.kind = ssd_cache::AccessKind::WriteObserved;
    event.observed_at = std::chrono::system_clock::now();

    index.record_access(event, std::chrono::seconds(60));

    REQUIRE(!index.pending_job(event.relative_path).has_value());
}

TEST_CASE("cache index marks cached file and clears pending job") {
    ssd_cache::CacheIndex index(test_db_path(L"ssd-cache-index-cached.sqlite3"));
    index.open();

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"cached.txt";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();
    index.record_access(event, std::chrono::seconds(0));

    index.mark_cached(
        event.relative_path,
        10,
        10,
        "abc123",
        std::chrono::system_clock::now()
    );
    index.remove_pending_job(event.relative_path);

    const auto record = index.file_record(event.relative_path);
    REQUIRE(record.has_value());
    REQUIRE(record->cache_present);
    REQUIRE(record->hash_algo == "sha256");
    REQUIRE(record->hash_hex == "abc123");
    REQUIRE(!index.pending_job(event.relative_path).has_value());
}
