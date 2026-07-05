#pragma once

/**
 * @file
 * @brief SQLite-backed index of observed files, cache state and pending copies.
 */

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

/** Full row describing a known file and its cache state. */
struct FileRecord {
    /** Path relative to the source root (primary key). */
    std::wstring relative_path;
    /** Most recent time the file was accessed. */
    std::chrono::system_clock::time_point last_seen_utc;
    /** Time the file was last closed after writing, when known. */
    std::optional<std::chrono::system_clock::time_point> last_write_close_utc;
    /** Size of the source file in bytes. */
    std::uint64_t source_size_bytes = 0;
    /** Size of the cached copy in bytes. */
    std::uint64_t cached_size_bytes = 0;
    /** Time the file was cached, when present. */
    std::optional<std::chrono::system_clock::time_point> cached_at_utc;
    /** Hash algorithm name (e.g. "sha256"). */
    std::string hash_algo;
    /** Hex digest of the cached content. */
    std::string hash_hex;
    /** Time the hash was computed, when known. */
    std::optional<std::chrono::system_clock::time_point> hash_at_utc;
    /** True when a cache copy currently exists. */
    bool cache_present = false;
    /** Detail of the last error touching this file; empty if none. */
    std::string last_error;
    /** Pid of the last process to access the file; 0 when unknown. */
    std::uint64_t last_requestor_pid = 0;
    /** Image path of the last requesting process; empty when unknown. */
    std::string last_requestor_process;
};

/** Lightweight reference to a cached file, used by eviction. */
struct CachedFileRef {
    std::wstring relative_path;       /**< Path relative to the source root. */
    std::uint64_t cached_size_bytes = 0;  /**< Size of the cached copy in bytes. */
};

/** A scheduled copy for a file, due at a given time. */
struct PendingJob {
    std::wstring relative_path;                    /**< File to copy. */
    std::chrono::system_clock::time_point due_utc; /**< When the copy is due. */
    std::string reason;                            /**< Why it was scheduled. */
    int retries = 0;                               /**< Retry attempts so far. */
    std::uint64_t requestor_pid = 0;               /**< Requesting pid; 0 if n/a. */
    std::string requestor_process;                 /**< Requesting image path. */
};

/**
 * Thread-safe SQLite store of file records and pending copy jobs. All public
 * methods lock internally; the database must be opened before use.
 */
class CacheIndex {
public:
    /**
     * @param sqlite_path Path to the SQLite database file (created on open).
     */
    explicit CacheIndex(std::wstring sqlite_path);

    CacheIndex(const CacheIndex&) = delete;
    CacheIndex& operator=(const CacheIndex&) = delete;

    ~CacheIndex();

    /** Opens the database, applying pragmas and creating/upgrading the schema. */
    void open();

    /** Closes the database if open. */
    void close();

    /**
     * Records an observed access, upserting the file row and (when the access
     * warrants it) scheduling a pending copy.
     *
     * @param event The observed access event.
     * @param copy_delay Delay added to the observed time to compute the due time.
     */
    void record_access(
        const AccessEvent& event,
        std::chrono::seconds copy_delay
    );

    /**
     * Inserts or updates a pending copy job for a path.
     *
     * @param relative_path File the job is for.
     * @param due_utc When the copy becomes due.
     * @param reason Human-readable reason recorded with the job.
     */
    void upsert_pending_job(
        const std::wstring& relative_path,
        std::chrono::system_clock::time_point due_utc,
        std::string_view reason
    );

    /**
     * Fetches the pending job for a path.
     *
     * @param relative_path File to look up.
     * @return The job, or nullopt if none is pending.
     */
    std::optional<PendingJob> pending_job(
        const std::wstring& relative_path
    );

    /**
     * @return All pending jobs ordered by due time.
     */
    std::vector<PendingJob> load_pending_jobs();

    /**
     * @return The relative paths of all known file records.
     */
    std::vector<std::wstring> load_record_paths();

    /**
     * Lists cached files (cache_present=1) ordered by last access, least
     * recently accessed first. Used by free-space eviction to pick victims.
     *
     * @return Cached-file references in eviction order.
     */
    std::vector<CachedFileRef> cached_files_by_last_access();

    /**
     * Fetches pending jobs that are due at or before a time.
     *
     * @param now_utc Cutoff time; jobs with due_utc <= this are returned.
     * @param limit Maximum number of jobs to return.
     * @return Due jobs ordered by due time, at most @p limit of them.
     */
    std::vector<PendingJob> due_jobs(
        std::chrono::system_clock::time_point now_utc,
        int limit
    );

    /**
     * Removes a pending job.
     *
     * @param relative_path File whose pending job is removed.
     */
    void remove_pending_job(const std::wstring& relative_path);

    /**
     * Deletes a file's record and any pending job entirely.
     *
     * @param relative_path File to delete from the index.
     */
    void delete_record(const std::wstring& relative_path);

    /**
     * Marks a file as cached and clears its pending state and last error.
     *
     * @param relative_path File that was cached.
     * @param source_size_bytes Size of the source file in bytes.
     * @param cached_size_bytes Size of the cached copy in bytes.
     * @param hash_hex Hex digest of the cached content.
     * @param cached_at_utc Time the file was cached.
     */
    void mark_cached(
        const std::wstring& relative_path,
        std::uint64_t source_size_bytes,
        std::uint64_t cached_size_bytes,
        std::string_view hash_hex,
        std::chrono::system_clock::time_point cached_at_utc
    );

    /**
     * Records an error for a file and increments its pending job's retry count.
     *
     * @param relative_path File the error applies to.
     * @param error_message Human-readable error detail to store.
     */
    void record_error(
        const std::wstring& relative_path,
        std::string_view error_message
    );

    /**
     * Marks a file's cache copy as gone (cache_present=0) and drops its pending
     * job, without deleting the file record.
     *
     * @param relative_path File whose cache copy was removed.
     */
    void mark_removed(const std::wstring& relative_path);

    /**
     * Fetches the full record for a file.
     *
     * @param relative_path File to look up.
     * @return The record, or nullopt if the file is unknown.
     */
    std::optional<FileRecord> file_record(const std::wstring& relative_path);

    /**
     * @return The database path this index was constructed with.
     */
    const std::wstring& sqlite_path() const;

private:
    /** Creates the schema and applies additive migrations. */
    void initialize_schema();

    /**
     * @return The open database handle.
     * @throws std::runtime_error if the index is not open.
     */
    sqlite3* require_db();

    std::wstring sqlite_path_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

}  // namespace ssd_cache
