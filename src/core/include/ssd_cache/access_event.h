#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace ssd_cache {

enum class AccessKind {
    ReadOpen,
    WriteObserved,
    WriteClosed,
    Rename,
    Delete,
};

struct AccessEvent {
    std::wstring source_root_id;
    std::wstring relative_path;
    AccessKind kind = AccessKind::ReadOpen;
    std::uint64_t size_hint = 0;
    std::uint64_t requestor_pid = 0;
    std::chrono::system_clock::time_point observed_at =
        std::chrono::system_clock::now();
};

std::string access_kind_to_string(AccessKind kind);

AccessKind access_kind_from_string(std::string_view value);

bool should_schedule_copy(AccessKind kind);

}  // namespace ssd_cache
