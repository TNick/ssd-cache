#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace ssd_cache {

enum class CopyAction {
    Copied,
    SkippedSameSize,
    SkippedHashMatch,
    SourceMissing,
    Failed,
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

class ICopyEngine {
public:
    virtual ~ICopyEngine() = default;

    virtual CopyResult copy_and_hash(
        const std::wstring& source_abs,
        const std::wstring& cache_abs,
        bool compare_hash_before_overwrite
    ) = 0;
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
