#pragma once

/**
 * @file
 * @brief UTC timestamp formatting and parsing for the cache index.
 *
 * The cache index persists timestamps as fixed, whole-second UTC strings; these
 * helpers are the single point that defines that textual representation.
 */

#include <chrono>
#include <optional>
#include <string>

namespace ssd_cache {

/**
 * Formats a time point as a whole-second UTC string.
 *
 * @param time_point The time point to format (interpreted as UTC).
 * @return The UTC timestamp string (sub-second precision is dropped).
 */
std::string format_utc(std::chrono::system_clock::time_point time_point);

/**
 * Parses a UTC timestamp string produced by format_utc.
 *
 * @param value The timestamp string to parse.
 * @return The parsed time point, or nullopt if @p value is not a valid
 *         timestamp.
 */
std::optional<std::chrono::system_clock::time_point> parse_utc(
    std::string_view value
);

}  // namespace ssd_cache
