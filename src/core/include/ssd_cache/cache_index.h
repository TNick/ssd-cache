#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ssd_cache/access_event.h"

struct sqlite3;

namespace ssd_cache {

struct FileRecord {
    std::wstring relative_path;
    std::chrono::system_clock::time_point last_seen_utc;
    std::optional<std::chrono::system_clock::time_point> last_write_close_utc;
    std::uint64_t source_size_bytes = 0;
    std::uint64_t cached_size_bytes = 0;
    std::optional<std::chrono::system_clock::time_point> cached_at_utc;
    std::string hash_algo;
    std::string hash_hex;
    std::optional<std::chrono::system_clock::time_point> hash_at_utc;
    bool cache_present = false;
    std::string last_error;
    std::uint64_t last_requestor_pid = 0;
    std::string last_requestor_process;
};

struct CachedFileRef {
    std::wstring relative_path;
    std::uint64_t cached_size_bytes = 0;
};

struct PendingJob {
    std::wstring relative_path;
    std::chrono::system_clock::time_point due_utc;
    std::string reason;
    int retries = 0;
    std::uint64_t requestor_pid = 0;
    std::string requestor_process;
};

class CacheIndex {
public:
    explicit CacheIndex(std::wstring sqlite_path);

    CacheIndex(const CacheIndex&) = delete;
    CacheIndex& operator=(const CacheIndex&) = delete;

    ~CacheIndex();

    void open();

    void close();

    void record_access(
        const AccessEvent& event,
        std::chrono::seconds copy_delay
    );

    void upsert_pending_job(
        const std::wstring& relative_path,
        std::chrono::system_clock::time_point due_utc,
        std::string_view reason
    );

    std::optional<PendingJob> pending_job(
        const std::wstring& relative_path
    );

    std::vector<PendingJob> load_pending_jobs();

    std::vector<std::wstring> load_record_paths();

    // Cached files (cache_present=1) ordered by last access, least recently
    // accessed first. Used by free-space eviction to pick victims.
    std::vector<CachedFileRef> cached_files_by_last_access();

    std::vector<PendingJob> due_jobs(
        std::chrono::system_clock::time_point now_utc,
        int limit
    );

    void remove_pending_job(const std::wstring& relative_path);

    void delete_record(const std::wstring& relative_path);

    void mark_cached(
        const std::wstring& relative_path,
        std::uint64_t source_size_bytes,
        std::uint64_t cached_size_bytes,
        std::string_view hash_hex,
        std::chrono::system_clock::time_point cached_at_utc
    );

    void record_error(
        const std::wstring& relative_path,
        std::string_view error_message
    );

    void mark_removed(const std::wstring& relative_path);

    std::optional<FileRecord> file_record(const std::wstring& relative_path);

    const std::wstring& sqlite_path() const;

private:
    void initialize_schema();

    sqlite3* require_db();

    std::wstring sqlite_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace ssd_cache
