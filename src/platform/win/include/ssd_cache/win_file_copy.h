#pragma once

/**
 * @file
 * @brief Windows copy engine (buffered copy + SHA-256) and free-space provider.
 */

#include <chrono>
#include <cstddef>

#include "ssd_cache/copy_engine.h"

namespace ssd_cache {

/**
 * Copies files into the cache in throttled chunks while computing a SHA-256
 * hash, writing via a temp file and atomic rename. Runs at low I/O priority so
 * caching does not disturb foreground work.
 */
class WinFileCopyEngine final : public ICopyEngine {
public:
    WinFileCopyEngine();

    /**
     * Copies and hashes a source file into the cache. (Contract documented on
     * ICopyEngine::copy_and_hash.)
     *
     * @param source_abs Absolute source path.
     * @param cache_abs Absolute cache destination path.
     * @param compare_hash_before_overwrite Skip an identical existing copy by
     *        comparing hashes when sizes match.
     * @param cancelled Polled to abort a long copy without leaving a partial file.
     * @return The copy outcome.
     */
    CopyResult copy_and_hash(
        const std::wstring& source_abs,
        const std::wstring& cache_abs,
        bool compare_hash_before_overwrite,
        const std::atomic_bool& cancelled
    ) override;

    /**
     * Removes a cache file. (Contract documented on
     * ICopyEngine::remove_cached_file.)
     *
     * @param cache_abs Absolute cache path to remove.
     * @return Whether a file was removed, was absent, or the error.
     */
    RemoveResult remove_cached_file(const std::wstring& cache_abs) override;

    /**
     * Sets the per-chunk copy size.
     *
     * @param chunk_size Chunk size in bytes.
     */
    void set_chunk_size(std::size_t chunk_size);

    /**
     * Sets the delay inserted between chunks to throttle copy throughput.
     *
     * @param delay Delay between chunks.
     */
    void set_sleep_between_chunks(std::chrono::milliseconds delay);

private:
    std::size_t chunk_size_;
    std::chrono::milliseconds sleep_between_chunks_;
};

/** IFreeSpaceProvider backed by GetDiskFreeSpaceExW. */
class WinFreeSpaceProvider final : public IFreeSpaceProvider {
public:
    /**
     * @param path Any path on the volume of interest.
     * @return Bytes available on that volume, or nullopt on failure.
     */
    std::optional<std::uint64_t> free_bytes(
        const std::wstring& path
    ) const override;
};

}  // namespace ssd_cache
