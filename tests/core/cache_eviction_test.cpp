#include "test_macros.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "ssd_cache/cache_eviction.h"
#include "ssd_cache/cache_index.h"
#include "ssd_cache/path_mapper.h"

namespace {

class FakeEngine final : public ssd_cache::ICopyEngine {
public:
    ssd_cache::CopyResult copy_and_hash(
        const std::wstring&,
        const std::wstring&,
        bool,
        const std::atomic_bool&
    ) override {
        return {};
    }

    ssd_cache::RemoveResult remove_cached_file(
        const std::wstring&
    ) override {
        ++remove_calls;
        return remove_result;
    }

    ssd_cache::RemoveResult remove_result;
    int remove_calls = 0;
};

class FakeResolver final : public ssd_cache::ICopyPathResolver {
public:
    std::wstring source_absolute(const std::wstring&) const override {
        return {};
    }

    std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const override {
        return ssd_cache::join_root_relative(L"K:\\", relative_path);
    }
};

std::wstring eviction_db_path() {
    auto path = std::filesystem::temp_directory_path();
    path /= L"ssd-cache-eviction-test.sqlite3";
    std::filesystem::remove(path);
    return path.wstring();
}

void seed_cached_file(
    ssd_cache::CacheIndex& index,
    const std::wstring& path,
    std::uint64_t size,
    std::chrono::system_clock::time_point accessed_at
) {
    ssd_cache::AccessEvent event;
    event.relative_path = path;
    event.kind = ssd_cache::AccessKind::ReadOpen;
    event.size_hint = size;
    event.observed_at = accessed_at;
    index.record_access(event, std::chrono::seconds(0));
    index.remove_pending_job(path);
    index.mark_cached(path, size, size, "hash", accessed_at);
}

}  // namespace

TEST_CASE("parse_size_bytes handles suffixes and bare bytes") {
    REQUIRE(ssd_cache::parse_size_bytes(L"1024") == 1024ULL);
    REQUIRE(ssd_cache::parse_size_bytes(L"500") == 500ULL);
    REQUIRE(ssd_cache::parse_size_bytes(L"1KB") == 1024ULL);
    REQUIRE(ssd_cache::parse_size_bytes(L"1k") == 1024ULL);
    REQUIRE(ssd_cache::parse_size_bytes(L"2MB") == 2ULL * 1024 * 1024);
    REQUIRE(ssd_cache::parse_size_bytes(L"3gb") == 3ULL * 1024 * 1024 * 1024);
    REQUIRE(ssd_cache::parse_size_bytes(L"1.5GB") ==
            static_cast<std::uint64_t>(1.5 * 1024 * 1024 * 1024));
    REQUIRE(ssd_cache::parse_size_bytes(L"  10 MB ") == 10ULL * 1024 * 1024);
}

TEST_CASE("parse_size_bytes rejects malformed input") {
    REQUIRE(!ssd_cache::parse_size_bytes(L"").has_value());
    REQUIRE(!ssd_cache::parse_size_bytes(L"abc").has_value());
    REQUIRE(!ssd_cache::parse_size_bytes(L"10XB").has_value());
    REQUIRE(!ssd_cache::parse_size_bytes(L"-5MB").has_value());
    REQUIRE(!ssd_cache::parse_size_bytes(L"1e40").has_value());
}

TEST_CASE("format_size_bytes renders compact units") {
    REQUIRE(ssd_cache::format_size_bytes(512) == L"512 B");
    REQUIRE(ssd_cache::format_size_bytes(1024) == L"1.00 KB");
    REQUIRE(ssd_cache::format_size_bytes(1536) == L"1.50 KB");
    REQUIRE(ssd_cache::format_size_bytes(3ULL * 1024 * 1024 * 1024) ==
            L"3.00 GB");
}

TEST_CASE("evict_least_recently_used frees oldest first up to target") {
    ssd_cache::CacheIndex index(eviction_db_path());
    index.open();

    const auto base = std::chrono::system_clock::now();
    seed_cached_file(index, L"old.txt", 50, base + std::chrono::seconds(1));
    seed_cached_file(index, L"mid.txt", 50, base + std::chrono::seconds(2));
    seed_cached_file(index, L"new.txt", 50, base + std::chrono::seconds(3));

    FakeEngine engine;
    FakeResolver resolver;

    const auto result = ssd_cache::evict_least_recently_used(
        index,
        engine,
        resolver,
        80  // needs two files (50 + 50 >= 80)
    );

    REQUIRE(result.files_removed == 2);
    REQUIRE(result.bytes_freed == 100);
    REQUIRE(engine.remove_calls == 2);
    REQUIRE(!index.file_record(L"old.txt")->cache_present);
    REQUIRE(!index.file_record(L"mid.txt")->cache_present);
    REQUIRE(index.file_record(L"new.txt")->cache_present);
}

TEST_CASE("evict_least_recently_used skips the excluded path") {
    ssd_cache::CacheIndex index(eviction_db_path());
    index.open();

    const auto base = std::chrono::system_clock::now();
    seed_cached_file(index, L"old.txt", 50, base + std::chrono::seconds(1));
    seed_cached_file(index, L"mid.txt", 50, base + std::chrono::seconds(2));

    FakeEngine engine;
    FakeResolver resolver;

    const auto result = ssd_cache::evict_least_recently_used(
        index,
        engine,
        resolver,
        1000,          // more than the cache holds
        nullptr,
        L"old.txt"     // must be preserved
    );

    REQUIRE(result.files_removed == 1);
    REQUIRE(result.bytes_freed == 50);
    REQUIRE(index.file_record(L"old.txt")->cache_present);
    REQUIRE(!index.file_record(L"mid.txt")->cache_present);
}

TEST_CASE("evict_least_recently_used stops when cache is exhausted") {
    ssd_cache::CacheIndex index(eviction_db_path());
    index.open();

    const auto base = std::chrono::system_clock::now();
    seed_cached_file(index, L"only.txt", 50, base + std::chrono::seconds(1));

    FakeEngine engine;
    FakeResolver resolver;

    const auto result = ssd_cache::evict_least_recently_used(
        index,
        engine,
        resolver,
        1000
    );

    REQUIRE(result.files_removed == 1);
    REQUIRE(result.bytes_freed == 50);  // less than requested, but all there was
}
