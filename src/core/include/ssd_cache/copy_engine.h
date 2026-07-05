#pragma once

/**
 * @file
 * @brief Interfaces and result types for copying files into the cache.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ssd_cache {

/** Outcome of a single copy attempt. */
enum class CopyAction {
    Copied,           /**< The file was copied into the cache. */
    SkippedSameSize,  /**< Skipped: cache copy already matches by size. */
    SkippedHashMatch, /**< Skipped: cache copy already matches by hash. */
    SourceMissing,    /**< The source file did not exist / disappeared. */
    Failed,           /**< The copy failed; see error_message. */
    Cancelled,        /**< The copy was aborted mid-flight (e.g. shutdown). */
};

/** Result of a copy-and-hash operation. */
struct CopyResult {
    /** What happened during the attempt. */
    CopyAction action = CopyAction::Failed;
    /** Size of the source file in bytes. */
    std::uint64_t source_size_bytes = 0;
    /** Size of the resulting cache file in bytes. */
    std::uint64_t cached_size_bytes = 0;
    /** Hex SHA-256 of the copied content, when computed. */
    std::string hash_hex;
    /** When the operation completed. */
    std::chrono::system_clock::time_point completed_at =
        std::chrono::system_clock::now();
    /** Human-readable error detail; empty on success. */
    std::string error_message;
};

/** Result of removing a cache file. */
struct RemoveResult {
    /** True if a file was actually deleted. */
    bool removed = false;
    /** True if there was nothing to delete (already absent). */
    bool missing = false;
    /** Human-readable error detail; empty on success. */
    std::string error_message;
};

/** Copies files into the cache and removes them, computing content hashes. */
class ICopyEngine {
public:
    virtual ~ICopyEngine() = default;

    /**
     * Copies a source file into the cache and hashes its content.
     *
     * @param source_abs Absolute path of the source file.
     * @param cache_abs Absolute destination path in the cache.
     * @param compare_hash_before_overwrite When true and an equally sized cache
     *        file exists, hashes are compared to skip an identical copy.
     * @param cancelled Polled during long-running copies so the engine can abort
     *        promptly (e.g. when the service is shutting down) instead of running
     *        a multi-gigabyte copy to completion. On abort the engine reports
     *        CopyAction::Cancelled and leaves no partial file behind.
     * @return The outcome, including sizes, hash and any error detail.
     */
    virtual CopyResult copy_and_hash(
        const std::wstring& source_abs,
        const std::wstring& cache_abs,
        bool compare_hash_before_overwrite,
        const std::atomic_bool& cancelled
    ) = 0;

    /**
     * Removes a file from the cache.
     *
     * @param cache_abs Absolute path of the cache file to remove.
     * @return Whether a file was removed, was already absent, or the error.
     */
    virtual RemoveResult remove_cached_file(
        const std::wstring& cache_abs
    ) = 0;
};

/** Reports free space on the volume that hosts a given path. */
class IFreeSpaceProvider {
public:
    virtual ~IFreeSpaceProvider() = default;

    /**
     * Queries free space for the volume hosting a path.
     *
     * @param path Any path on the volume of interest.
     * @return Bytes available to the caller on that volume, or nullopt if it
     *         cannot be determined.
     */
    virtual std::optional<std::uint64_t> free_bytes(
        const std::wstring& path
    ) const = 0;
};

/** Resolves cache-relative paths to absolute source and cache paths. */
class ICopyPathResolver {
public:
    virtual ~ICopyPathResolver() = default;

    /**
     * @param relative_path Path relative to the source root.
     * @return The absolute path of that file on the source.
     */
    virtual std::wstring source_absolute(
        const std::wstring& relative_path
    ) const = 0;

    /**
     * @param relative_path Path relative to the cache root.
     * @return The absolute path of that file in the cache.
     */
    virtual std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const = 0;
};

}  // namespace ssd_cache
