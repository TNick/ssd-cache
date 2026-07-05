/**
 * @file
 * @brief SQLite implementation of the cache index, plus statement helpers.
 */

#include "ssd_cache/cache_index.h"

#include <sqlite3.h>

#include <stdexcept>

#include "ssd_cache/time.h"
#include "ssd_cache/utf.h"

namespace ssd_cache {
namespace {

/**
 * RAII wrapper around a prepared SQLite statement. Preparation happens in the
 * constructor (throwing on error) and finalization in the destructor; the bind
 * and step helpers translate SQLite failures into exceptions.
 */
class Statement {
public:
    /**
     * Prepares a statement.
     *
     * @param db Open database handle.
     * @param sql SQL text to prepare.
     * @throws std::runtime_error if preparation fails.
     */
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    ~Statement() {
        sqlite3_finalize(stmt_);
    }

    /** @return The underlying prepared-statement handle. */
    sqlite3_stmt* get() {
        return stmt_;
    }

    /**
     * Binds a text parameter.
     *
     * @param index 1-based parameter index.
     * @param value Text value (copied by SQLite).
     * @throws std::runtime_error on bind failure.
     */
    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(
                stmt_,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT
            ) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    /**
     * Binds a wide-string parameter, stored as UTF-8.
     *
     * @param index 1-based parameter index.
     * @param value Wide value to convert and bind.
     */
    void bind_wide(int index, const std::wstring& value) {
        bind_text(index, wide_to_utf8(value));
    }

    /**
     * Binds a 64-bit integer parameter.
     *
     * @param index 1-based parameter index.
     * @param value Value to bind.
     * @throws std::runtime_error on bind failure.
     */
    void bind_int64(int index, std::uint64_t value) {
        if (sqlite3_bind_int64(
                stmt_,
                index,
                static_cast<sqlite3_int64>(value)
            ) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    /**
     * Binds a 32-bit integer parameter.
     *
     * @param index 1-based parameter index.
     * @param value Value to bind.
     * @throws std::runtime_error on bind failure.
     */
    void bind_int(int index, int value) {
        if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    /**
     * Binds a time point as a whole-second UTC string.
     *
     * @param index 1-based parameter index.
     * @param value Time point to format and bind.
     */
    void bind_time(int index, std::chrono::system_clock::time_point value) {
        bind_text(index, format_utc(value));
    }

    /**
     * Steps the statement expecting a row.
     *
     * @return True if a row is available (SQLITE_ROW), false at end (SQLITE_DONE).
     * @throws std::runtime_error on any other result.
     */
    bool step_row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) {
            return true;
        }

        if (rc == SQLITE_DONE) {
            return false;
        }

        throw std::runtime_error(sqlite3_errmsg(db_));
    }

    /**
     * Steps a statement expected to produce no rows (INSERT/UPDATE/DELETE).
     *
     * @throws std::runtime_error if the result is not SQLITE_DONE.
     */
    void step_done() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

/**
 * Reads a text column as a std::string.
 *
 * @param stmt Statement positioned on a row.
 * @param column 0-based column index.
 * @return The column text, or an empty string when NULL.
 */
std::string column_text(sqlite3_stmt* stmt, int column) {
    const auto* text = sqlite3_column_text(stmt, column);
    if (text == nullptr) {
        return {};
    }

    return reinterpret_cast<const char*>(text);
}

/**
 * Reads a UTC-timestamp column.
 *
 * @param stmt Statement positioned on a row.
 * @param column 0-based column index.
 * @return The parsed time point, or nullopt when the column is NULL.
 */
std::optional<std::chrono::system_clock::time_point> column_time(
    sqlite3_stmt* stmt,
    int column
) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
        return std::nullopt;
    }

    return parse_utc(column_text(stmt, column));
}

/**
 * Reads a PendingJob from the standard pending-job column order.
 *
 * @param stmt Statement positioned on a pending-job row.
 * @return The populated PendingJob.
 */
PendingJob read_pending_job(sqlite3_stmt* stmt) {
    PendingJob job;
    job.relative_path = utf8_to_wide(column_text(stmt, 0));
    job.due_utc = parse_utc(column_text(stmt, 1)).value_or(
        std::chrono::system_clock::now()
    );
    job.reason = column_text(stmt, 2);
    job.retries = sqlite3_column_int(stmt, 3);
    job.requestor_pid = static_cast<std::uint64_t>(
        sqlite3_column_int64(stmt, 4)
    );
    job.requestor_process = column_text(stmt, 5);
    return job;
}

/**
 * Executes one or more SQL statements with no result rows.
 *
 * @param db Open database handle.
 * @param sql SQL text to execute.
 * @throws std::runtime_error on failure (with SQLite's message).
 */
void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error == nullptr ? sqlite3_errmsg(db) : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

/**
 * Reports whether a table has a given column (used to guard migrations).
 *
 * @param db Open database handle.
 * @param table_name Table to inspect.
 * @param column_name Column to look for.
 * @return True if the column exists.
 */
bool column_exists(sqlite3* db, const char* table_name, const char* column_name) {
    Statement statement(db, "SELECT name FROM pragma_table_info(?1)");
    statement.bind_text(1, table_name);
    while (statement.step_row()) {
        if (column_text(statement.get(), 0) == column_name) {
            return true;
        }
    }

    return false;
}

/**
 * Runs an ALTER TABLE only when a column is missing, making migrations
 * idempotent.
 *
 * @param db Open database handle.
 * @param table_name Table to check.
 * @param column_name Column whose absence triggers the migration.
 * @param sql The ALTER TABLE statement to run when the column is missing.
 */
void add_column_if_missing(
    sqlite3* db,
    const char* table_name,
    const char* column_name,
    const char* sql
) {
    if (!column_exists(db, table_name, column_name)) {
        exec(db, sql);
    }
}

}  // namespace

CacheIndex::CacheIndex(std::wstring sqlite_path)
    : sqlite_path_(std::move(sqlite_path)) {}

CacheIndex::~CacheIndex() {
    close();
}

void CacheIndex::open() {
    std::lock_guard lock(mutex_);
    if (db_ != nullptr) {
        return;
    }

    const auto path = wide_to_utf8(sqlite_path_);
    const int rc = sqlite3_open_v2(
        path.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (rc != SQLITE_OK) {
        std::string message = db_ == nullptr ? "failed to open sqlite" :
            sqlite3_errmsg(db_);
        close();
        throw std::runtime_error(message);
    }

    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_, "PRAGMA synchronous=NORMAL;");
    exec(db_, "PRAGMA busy_timeout=5000;");
    exec(db_, "PRAGMA foreign_keys=ON;");
    initialize_schema();
}

void CacheIndex::close() {
    std::lock_guard lock(mutex_);
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void CacheIndex::record_access(
    const AccessEvent& event,
    std::chrono::seconds copy_delay
) {
    if (event.relative_path.empty()) {
        return;
    }

    std::lock_guard lock(mutex_);
    auto* db = require_db();
    exec(db, "BEGIN IMMEDIATE;");

    try {
        Statement statement(
            db,
            "INSERT INTO files("
            "relative_path, last_seen_utc, last_write_close_utc, "
            "source_size_bytes, cache_present, last_requestor_pid, "
            "last_requestor_process"
            ") VALUES(?1, ?2, ?3, ?4, 0, ?5, ?6) "
            "ON CONFLICT(relative_path) DO UPDATE SET "
            "last_seen_utc=excluded.last_seen_utc, "
            "last_write_close_utc=COALESCE("
            "excluded.last_write_close_utc, files.last_write_close_utc), "
            "source_size_bytes=excluded.source_size_bytes, "
            "last_requestor_pid=excluded.last_requestor_pid, "
            "last_requestor_process=excluded.last_requestor_process"
        );
        statement.bind_wide(1, event.relative_path);
        statement.bind_time(2, event.observed_at);

        if (event.kind == AccessKind::WriteClosed) {
            statement.bind_time(3, event.observed_at);
        } else {
            sqlite3_bind_null(statement.get(), 3);
        }

        statement.bind_int64(4, event.size_hint);
        statement.bind_int64(5, event.requestor_pid);
        statement.bind_text(6, event.requestor_process);
        statement.step_done();

        if (should_schedule_copy(event.kind)) {
            const auto due = event.observed_at + copy_delay;
            Statement pending(
                db,
                "INSERT INTO pending_jobs("
                "relative_path, due_utc, reason, requestor_pid, "
                "requestor_process"
                ") VALUES(?1, ?2, ?3, ?4, ?5) "
                "ON CONFLICT(relative_path) DO UPDATE SET "
                "due_utc=excluded.due_utc, "
                "reason=excluded.reason, "
                "requestor_pid=excluded.requestor_pid, "
                "requestor_process=excluded.requestor_process"
            );
            pending.bind_wide(1, event.relative_path);
            pending.bind_time(2, due);
            pending.bind_text(3, access_kind_to_string(event.kind));
            pending.bind_int64(4, event.requestor_pid);
            pending.bind_text(5, event.requestor_process);
            pending.step_done();
        }

        exec(db, "COMMIT;");
    } catch (...) {
        exec(db, "ROLLBACK;");
        throw;
    }
}

void CacheIndex::upsert_pending_job(
    const std::wstring& relative_path,
    std::chrono::system_clock::time_point due_utc,
    std::string_view reason
) {
    if (relative_path.empty()) {
        return;
    }

    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "INSERT INTO pending_jobs(relative_path, due_utc, reason) "
        "VALUES(?1, ?2, ?3) "
        "ON CONFLICT(relative_path) DO UPDATE SET "
        "due_utc=excluded.due_utc, reason=excluded.reason"
    );
    statement.bind_wide(1, relative_path);
    statement.bind_time(2, due_utc);
    statement.bind_text(3, reason);
    statement.step_done();
}

std::optional<PendingJob> CacheIndex::pending_job(
    const std::wstring& relative_path
) {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "SELECT relative_path, due_utc, reason, retries, "
        "requestor_pid, requestor_process "
        "FROM pending_jobs WHERE relative_path=?1"
    );
    statement.bind_wide(1, relative_path);
    if (!statement.step_row()) {
        return std::nullopt;
    }

    return read_pending_job(statement.get());
}

std::vector<PendingJob> CacheIndex::load_pending_jobs() {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "SELECT relative_path, due_utc, reason, retries, "
        "requestor_pid, requestor_process "
        "FROM pending_jobs ORDER BY due_utc"
    );

    std::vector<PendingJob> jobs;
    while (statement.step_row()) {
        jobs.push_back(read_pending_job(statement.get()));
    }

    return jobs;
}

std::vector<std::wstring> CacheIndex::load_record_paths() {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "SELECT relative_path FROM files ORDER BY relative_path"
    );

    std::vector<std::wstring> paths;
    while (statement.step_row()) {
        paths.push_back(utf8_to_wide(column_text(statement.get(), 0)));
    }

    return paths;
}

std::vector<CachedFileRef> CacheIndex::cached_files_by_last_access() {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "SELECT relative_path, cached_size_bytes FROM files "
        "WHERE cache_present=1 "
        "ORDER BY last_seen_utc ASC, relative_path ASC"
    );

    std::vector<CachedFileRef> files;
    while (statement.step_row()) {
        CachedFileRef ref;
        ref.relative_path = utf8_to_wide(column_text(statement.get(), 0));
        ref.cached_size_bytes = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement.get(), 1)
        );
        files.push_back(std::move(ref));
    }

    return files;
}

std::vector<PendingJob> CacheIndex::due_jobs(
    std::chrono::system_clock::time_point now_utc,
    int limit
) {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "SELECT relative_path, due_utc, reason, retries, "
        "requestor_pid, requestor_process "
        "FROM pending_jobs WHERE due_utc <= ?1 "
        "ORDER BY due_utc LIMIT ?2"
    );
    statement.bind_time(1, now_utc);
    statement.bind_int(2, limit);

    std::vector<PendingJob> jobs;
    while (statement.step_row()) {
        jobs.push_back(read_pending_job(statement.get()));
    }

    return jobs;
}

void CacheIndex::remove_pending_job(const std::wstring& relative_path) {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "DELETE FROM pending_jobs WHERE relative_path=?1"
    );
    statement.bind_wide(1, relative_path);
    statement.step_done();
}

void CacheIndex::delete_record(const std::wstring& relative_path) {
    std::lock_guard lock(mutex_);
    auto* db = require_db();
    exec(db, "BEGIN IMMEDIATE;");

    try {
        Statement pending(
            db,
            "DELETE FROM pending_jobs WHERE relative_path=?1"
        );
        pending.bind_wide(1, relative_path);
        pending.step_done();

        Statement files(
            db,
            "DELETE FROM files WHERE relative_path=?1"
        );
        files.bind_wide(1, relative_path);
        files.step_done();
        exec(db, "COMMIT;");
    } catch (...) {
        exec(db, "ROLLBACK;");
        throw;
    }
}

void CacheIndex::mark_cached(
    const std::wstring& relative_path,
    std::uint64_t source_size_bytes,
    std::uint64_t cached_size_bytes,
    std::string_view hash_hex,
    std::chrono::system_clock::time_point cached_at_utc
) {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "UPDATE files SET "
        "source_size_bytes=?2, "
        "cached_size_bytes=?3, "
        "cached_at_utc=?4, "
        "hash_algo='sha256', "
        "hash_hex=?5, "
        "hash_at_utc=?4, "
        "cache_present=1, "
        "last_error=NULL "
        "WHERE relative_path=?1"
    );
    statement.bind_wide(1, relative_path);
    statement.bind_int64(2, source_size_bytes);
    statement.bind_int64(3, cached_size_bytes);
    statement.bind_time(4, cached_at_utc);
    statement.bind_text(5, hash_hex);
    statement.step_done();
}

void CacheIndex::record_error(
    const std::wstring& relative_path,
    std::string_view error_message
) {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "UPDATE files SET last_error=?2 WHERE relative_path=?1"
    );
    statement.bind_wide(1, relative_path);
    statement.bind_text(2, error_message);
    statement.step_done();

    Statement retry(
        require_db(),
        "UPDATE pending_jobs SET retries=retries + 1 "
        "WHERE relative_path=?1"
    );
    retry.bind_wide(1, relative_path);
    retry.step_done();
}

void CacheIndex::mark_removed(const std::wstring& relative_path) {
    std::lock_guard lock(mutex_);
    auto* db = require_db();
    exec(db, "BEGIN IMMEDIATE;");

    try {
        Statement pending(
            db,
            "DELETE FROM pending_jobs WHERE relative_path=?1"
        );
        pending.bind_wide(1, relative_path);
        pending.step_done();

        Statement files(
            db,
            "UPDATE files SET cache_present=0, last_error=NULL "
            "WHERE relative_path=?1"
        );
        files.bind_wide(1, relative_path);
        files.step_done();
        exec(db, "COMMIT;");
    } catch (...) {
        exec(db, "ROLLBACK;");
        throw;
    }
}

std::optional<FileRecord> CacheIndex::file_record(
    const std::wstring& relative_path
) {
    std::lock_guard lock(mutex_);
    Statement statement(
        require_db(),
        "SELECT relative_path, last_seen_utc, last_write_close_utc, "
        "source_size_bytes, cached_size_bytes, cached_at_utc, "
        "hash_algo, hash_hex, hash_at_utc, cache_present, last_error, "
        "last_requestor_pid, last_requestor_process "
        "FROM files WHERE relative_path=?1"
    );
    statement.bind_wide(1, relative_path);

    if (!statement.step_row()) {
        return std::nullopt;
    }

    FileRecord record;
    record.relative_path = utf8_to_wide(column_text(statement.get(), 0));
    record.last_seen_utc = parse_utc(column_text(statement.get(), 1)).value_or(
        std::chrono::system_clock::now()
    );
    record.last_write_close_utc = column_time(statement.get(), 2);
    record.source_size_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement.get(), 3)
    );
    record.cached_size_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement.get(), 4)
    );
    record.cached_at_utc = column_time(statement.get(), 5);
    record.hash_algo = column_text(statement.get(), 6);
    record.hash_hex = column_text(statement.get(), 7);
    record.hash_at_utc = column_time(statement.get(), 8);
    record.cache_present = sqlite3_column_int(statement.get(), 9) != 0;
    record.last_error = column_text(statement.get(), 10);
    record.last_requestor_pid = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement.get(), 11)
    );
    record.last_requestor_process = column_text(statement.get(), 12);
    return record;
}

const std::wstring& CacheIndex::sqlite_path() const {
    return sqlite_path_;
}

void CacheIndex::initialize_schema() {
    exec(
        db_,
        "CREATE TABLE IF NOT EXISTS files ("
        "relative_path TEXT PRIMARY KEY,"
        "last_seen_utc TEXT NOT NULL,"
        "last_write_close_utc TEXT,"
        "source_size_bytes INTEGER,"
        "cached_size_bytes INTEGER,"
        "cached_at_utc TEXT,"
        "hash_algo TEXT,"
        "hash_hex TEXT,"
        "hash_at_utc TEXT,"
        "cache_present INTEGER NOT NULL DEFAULT 0,"
        "last_error TEXT,"
        "last_requestor_pid INTEGER NOT NULL DEFAULT 0,"
        "last_requestor_process TEXT NOT NULL DEFAULT ''"
        ");"
    );
    exec(
        db_,
        "CREATE TABLE IF NOT EXISTS pending_jobs ("
        "relative_path TEXT PRIMARY KEY,"
        "due_utc TEXT NOT NULL,"
        "reason TEXT NOT NULL,"
        "retries INTEGER NOT NULL DEFAULT 0,"
        "requestor_pid INTEGER NOT NULL DEFAULT 0,"
        "requestor_process TEXT NOT NULL DEFAULT '',"
        "FOREIGN KEY(relative_path) REFERENCES files(relative_path)"
        ");"
    );
    add_column_if_missing(
        db_,
        "files",
        "last_requestor_pid",
        "ALTER TABLE files ADD COLUMN "
        "last_requestor_pid INTEGER NOT NULL DEFAULT 0;"
    );
    add_column_if_missing(
        db_,
        "files",
        "last_requestor_process",
        "ALTER TABLE files ADD COLUMN "
        "last_requestor_process TEXT NOT NULL DEFAULT '';"
    );
    add_column_if_missing(
        db_,
        "pending_jobs",
        "requestor_pid",
        "ALTER TABLE pending_jobs ADD COLUMN "
        "requestor_pid INTEGER NOT NULL DEFAULT 0;"
    );
    add_column_if_missing(
        db_,
        "pending_jobs",
        "requestor_process",
        "ALTER TABLE pending_jobs ADD COLUMN "
        "requestor_process TEXT NOT NULL DEFAULT '';"
    );
    exec(
        db_,
        "CREATE INDEX IF NOT EXISTS pending_jobs_due_utc_idx "
        "ON pending_jobs(due_utc);"
    );
}

sqlite3* CacheIndex::require_db() {
    if (db_ == nullptr) {
        throw std::runtime_error("cache index is not open");
    }

    return db_;
}

}  // namespace ssd_cache
