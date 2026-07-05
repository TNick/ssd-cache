#pragma once

#include <chrono>
#include <cstddef>

#include "ssd_cache/copy_engine.h"

namespace ssd_cache {

class WinFileCopyEngine final : public ICopyEngine {
public:
    WinFileCopyEngine();

    CopyResult copy_and_hash(
        const std::wstring& source_abs,
        const std::wstring& cache_abs,
        bool compare_hash_before_overwrite
    ) override;

    void set_chunk_size(std::size_t chunk_size);

    void set_sleep_between_chunks(std::chrono::milliseconds delay);

private:
    std::size_t chunk_size_;
    std::chrono::milliseconds sleep_between_chunks_;
};

}  // namespace ssd_cache
