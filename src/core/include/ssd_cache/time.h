#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace ssd_cache {

std::string format_utc(std::chrono::system_clock::time_point time_point);

std::optional<std::chrono::system_clock::time_point> parse_utc(
    std::string_view value
);

}  // namespace ssd_cache
