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
    event.requestor_pid = 42;
    event.requestor_process = "C:\\Tools\\reader.exe";
    event.observed_at = std::chrono::system_clock::now();

    index.record_access(event, std::chrono::seconds(60));

    const auto record = index.file_record(event.relative_path);
    REQUIRE(record.has_value());
    REQUIRE(record->source_size_bytes == 123);
    REQUIRE(record->last_requestor_pid == 42);
    REQUIRE(record->last_requestor_process == "C:\\Tools\\reader.exe");

    const auto pending = index.pending_job(event.relative_path);
    REQUIRE(pending.has_value());
    REQUIRE(pending->reason == "read_open");
    REQUIRE(pending->requestor_pid == 42);
    REQUIRE(pending->requestor_process == "C:\\Tools\\reader.exe");
}

TEST_CASE("cache index lists cached files least recently accessed first") {
    ssd_cache::CacheIndex index(test_db_path(L"ssd-cache-index-age.sqlite3"));
    index.open();

    const auto base = std::chrono::system_clock::now();
    const auto seed = [&](const std::wstring& path,
                          std::uint64_t size,
                          std::chrono::system_clock::time_point accessed_at) {
        ssd_cache::AccessEvent event;
        event.relative_path = path;
        event.kind = ssd_cache::AccessKind::ReadOpen;
        event.size_hint = size;
        event.observed_at = accessed_at;
        index.record_access(event, std::chrono::seconds(0));
        index.remove_pending_job(path);
        index.mark_cached(path, size, size, "hash", accessed_at);
    };

    seed(L"mid.txt", 20, base + std::chrono::seconds(2));
    seed(L"old.txt", 10, base + std::chrono::seconds(1));
    seed(L"new.txt", 30, base + std::chrono::seconds(3));

    // A recorded-but-not-yet-cached file must be excluded.
    ssd_cache::AccessEvent pending_only;
    pending_only.relative_path = L"pending.txt";
    pending_only.kind = ssd_cache::AccessKind::ReadOpen;
    pending_only.observed_at = base;
    index.record_access(pending_only, std::chrono::seconds(0));

    auto files = index.cached_files_by_last_access();
    REQUIRE(files.size() == 3);
    REQUIRE(files[0].relative_path == L"old.txt");
    REQUIRE(files[0].cached_size_bytes == 10);
    REQUIRE(files[1].relative_path == L"mid.txt");
    REQUIRE(files[2].relative_path == L"new.txt");

    // Re-accessing the oldest-cached file makes it most recently used, so it
    // must move to the end of the eviction order (ordering is by access, not
    // cache date).
    ssd_cache::AccessEvent reaccess;
    reaccess.relative_path = L"old.txt";
    reaccess.kind = ssd_cache::AccessKind::ReadOpen;
    reaccess.observed_at = base + std::chrono::seconds(4);
    index.record_access(reaccess, std::chrono::seconds(0));

    files = index.cached_files_by_last_access();
    REQUIRE(files.size() == 3);
    REQUIRE(files[0].relative_path == L"mid.txt");
    REQUIRE(files[1].relative_path == L"new.txt");
    REQUIRE(files[2].relative_path == L"old.txt");
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

TEST_CASE("cache index ignores empty relative paths") {
    ssd_cache::CacheIndex index(test_db_path(L"ssd-cache-index-empty.sqlite3"));
    index.open();

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();

    index.record_access(event, std::chrono::seconds(60));
    index.upsert_pending_job(L"", event.observed_at, "read_open");

    REQUIRE(!index.file_record(L"").has_value());
    REQUIRE(!index.pending_job(L"").has_value());
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

TEST_CASE("cache index deletes file record and pending job") {
    ssd_cache::CacheIndex index(test_db_path(L"ssd-cache-index-delete.sqlite3"));
    index.open();

    ssd_cache::AccessEvent event;
    event.source_root_id = L"\\\\nas\\share";
    event.relative_path = L"folder";
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.observed_at = std::chrono::system_clock::now();
    index.record_access(event, std::chrono::seconds(60));

    REQUIRE(index.file_record(event.relative_path).has_value());
    REQUIRE(index.pending_job(event.relative_path).has_value());
    REQUIRE(index.load_record_paths().size() == 1);

    index.delete_record(event.relative_path);

    REQUIRE(!index.file_record(event.relative_path).has_value());
    REQUIRE(!index.pending_job(event.relative_path).has_value());
    REQUIRE(index.load_record_paths().empty());
}
