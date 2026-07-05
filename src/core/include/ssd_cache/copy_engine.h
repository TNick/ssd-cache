#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ssd_cache {

enum class CopyAction {
    Copied,
    SkippedSameSize,
    SkippedHashMatch,
    SourceMissing,
    Failed,
    Cancelled,
};

struct CopyResult {
    CopyAction action = CopyAction::Failed;
    std::uint64_t source_size_bytes = 0;
    std::uint64_t cached_size_bytes = 0;
    std::string hash_hex;
    std::chrono::system_clock::time_point completed_at =
        std::chrono::system_clock::now();
    std::string error_message;
};

struct RemoveResult {
    bool removed = false;
    bool missing = false;
    std::string error_message;
};

class ICopyEngine {
public:
    virtual ~ICopyEngine() = default;

    // `cancelled` is polled during long-running copies so the engine can abort
    // promptly (e.g. when the service is shutting down) instead of running a
    // multi-gigabyte copy to completion. On abort the engine reports
    // CopyAction::Cancelled and leaves no partial file behind.
    virtual CopyResult copy_and_hash(
        const std::wstring& source_abs,
        const std::wstring& cache_abs,
        bool compare_hash_before_overwrite,
        const std::atomic_bool& cancelled
    ) = 0;

    virtual RemoveResult remove_cached_file(
        const std::wstring& cache_abs
    ) = 0;
};

class IFreeSpaceProvider {
public:
    virtual ~IFreeSpaceProvider() = default;

    // Bytes available to the caller on the volume hosting `path`, or nullopt if
    // it cannot be determined.
    virtual std::optional<std::uint64_t> free_bytes(
        const std::wstring& path
    ) const = 0;
};

class ICopyPathResolver {
public:
    virtual ~ICopyPathResolver() = default;

    virtual std::wstring source_absolute(
        const std::wstring& relative_path
    ) const = 0;

    virtual std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const = 0;
};

}  // namespace ssd_cache
