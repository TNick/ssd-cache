#pragma once

/**
 * @file
 * @brief The file-access event reported by the driver and consumed by the core.
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace ssd_cache {

/** Kind of file access observed on the monitored source. */
enum class AccessKind {
    ReadOpen,       /**< A file was opened for reading. */
    WriteObserved,  /**< A write to a file was observed (still open). */
    WriteClosed,    /**< A file previously written was closed. */
    Rename,         /**< A file was renamed. */
    Delete,         /**< A file was deleted. */
};

/** A single access event describing what happened, to which file, and by whom. */
struct AccessEvent {
    /** Identifier of the source root (e.g. the UNC) the path is relative to. */
    std::wstring source_root_id;
    /** Path of the affected file, relative to source_root_id. */
    std::wstring relative_path;
    /** The kind of access observed. */
    AccessKind kind = AccessKind::ReadOpen;
    /** Best-effort file size from the driver; 0 when unknown. */
    std::uint64_t size_hint = 0;
    /** Process id that triggered the access; 0 when unknown. */
    std::uint64_t requestor_pid = 0;
    /** Image path of the requesting process; empty when unknown. */
    std::string requestor_process;
    /** When the event was observed (defaults to construction time). */
    std::chrono::system_clock::time_point observed_at =
        std::chrono::system_clock::now();
};

/**
 * Converts an access kind to its stable string form (used for persistence and
 * logging).
 *
 * @param kind The kind to convert.
 * @return The canonical lowercase name, e.g. "read_open".
 */
std::string access_kind_to_string(AccessKind kind);

/**
 * Parses an access kind from the string form produced by access_kind_to_string.
 *
 * @param value The string to parse.
 * @return The matching AccessKind (falling back to ReadOpen for unknown input).
 */
AccessKind access_kind_from_string(std::string_view value);

/**
 * Reports whether an access kind should schedule a cache copy.
 *
 * @param kind The kind to test.
 * @return True for kinds that warrant caching the file (e.g. reads/opens),
 *         false for kinds that do not (e.g. write-in-progress, delete).
 */
bool should_schedule_copy(AccessKind kind);

}  // namespace ssd_cache
