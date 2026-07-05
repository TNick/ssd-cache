#pragma once

/**
 * @file
 * @brief Human-size parsing/formatting and least-recently-used cache eviction.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ssd_cache/cache_index.h"
#include "ssd_cache/copy_engine.h"
#include "ssd_cache/logger.h"

namespace ssd_cache {

/** Summary of a cache eviction pass. */
struct EvictionResult {
    std::uint64_t bytes_freed = 0;   /**< Total bytes freed (summed cache sizes). */
    std::size_t files_removed = 0;   /**< Number of cache files removed. */
};

/**
 * Parses a human-readable size such as "500", "500KB", "2gb" or "1.5 GB" into a
 * byte count. Suffixes K/KB, M/MB, G/GB, T/TB (case-insensitive, trailing 'B'
 * optional) use 1024-based units; a bare number is bytes.
 *
 * @param text The size text to parse.
 * @return The parsed byte count, or nullopt for empty, malformed, negative or
 *         overflowing input.
 */
std::optional<std::uint64_t> parse_size_bytes(const std::wstring& text);

/**
 * Formats a byte count compactly, e.g. "512 B", "1.50 GB".
 *
 * @param bytes The byte count to format.
 * @return A compact human-readable string using 1024-based units.
 */
std::wstring format_size_bytes(std::uint64_t bytes);

/**
 * Evicts least-recently-accessed cached files until at least @p target_bytes
 * have been freed (summed by cached size) or no evictable cached file remains.
 *
 * @param cache_index Index queried for cached files and updated on removal.
 * @param copy_engine Engine used to delete cache files.
 * @param path_resolver Resolves cache-relative paths to absolute cache paths.
 * @param target_bytes Amount of space to free; zero is a no-op.
 * @param logger Optional log sink; may be null.
 * @param exclude Path never chosen as a victim (e.g. the file being cached);
 *        empty to exclude nothing.
 * @return How much was freed and how many files were removed.
 */
EvictionResult evict_least_recently_used(
    CacheIndex& cache_index,
    ICopyEngine& copy_engine,
    const ICopyPathResolver& path_resolver,
    std::uint64_t target_bytes,
    ILogger* logger = nullptr,
    const std::wstring& exclude = std::wstring()
);

}  // namespace ssd_cache
