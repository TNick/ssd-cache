/**
 * @file
 * @brief Implementation of human-size parsing/formatting and LRU eviction.
 */

#include "ssd_cache/cache_eviction.h"

#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string>

#include "ssd_cache/utf.h"

namespace ssd_cache {
namespace {

/**
 * Trims leading and trailing whitespace.
 *
 * @param value String to trim.
 * @return The trimmed substring.
 */
std::wstring trim(const std::wstring& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::iswspace(value[begin]) != 0) {
        ++begin;
    }
    while (end > begin && std::iswspace(value[end - 1]) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

/**
 * Uppercases a string (used to normalize unit suffixes).
 *
 * @param value Input string.
 * @return An uppercased copy of @p value.
 */
std::wstring to_upper(const std::wstring& value) {
    std::wstring result(value);
    for (auto& ch : result) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    return result;
}

/**
 * Maps an uppercased unit suffix to its byte multiplier.
 *
 * @param suffix Uppercased suffix ("", "B", "K"/"KB", "M"/"MB", "G"/"GB",
 *        "T"/"TB").
 * @return The 1024-based multiplier, or nullopt for an unrecognized suffix.
 */
std::optional<std::uint64_t> unit_multiplier(const std::wstring& suffix) {
    if (suffix.empty() || suffix == L"B") {
        return 1ULL;
    }
    if (suffix == L"K" || suffix == L"KB") {
        return 1024ULL;
    }
    if (suffix == L"M" || suffix == L"MB") {
        return 1024ULL * 1024ULL;
    }
    if (suffix == L"G" || suffix == L"GB") {
        return 1024ULL * 1024ULL * 1024ULL;
    }
    if (suffix == L"T" || suffix == L"TB") {
        return 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::uint64_t> parse_size_bytes(const std::wstring& text) {
    const auto trimmed = trim(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    wchar_t* end = nullptr;
    errno = 0;
    const double value = std::wcstod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || value < 0.0 || !std::isfinite(value)) {
        return std::nullopt;
    }

    const auto multiplier = unit_multiplier(to_upper(trim(std::wstring(end))));
    if (!multiplier) {
        return std::nullopt;
    }

    const double bytes = value * static_cast<double>(*multiplier);
    // 2^64 as a double; reject anything that would not fit in a uint64_t.
    if (bytes < 0.0 || bytes >= 18446744073709551616.0) {
        return std::nullopt;
    }

    return static_cast<std::uint64_t>(bytes);
}

std::wstring format_size_bytes(std::uint64_t bytes) {
    static constexpr std::array<const wchar_t*, 5> kUnits{
        L"B", L"KB", L"MB", L"GB", L"TB"
    };

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }

    wchar_t buffer[64];
    if (unit == 0) {
        std::swprintf(buffer, 64, L"%llu B",
                      static_cast<unsigned long long>(bytes));
    } else {
        std::swprintf(buffer, 64, L"%.2f %ls", value, kUnits[unit]);
    }
    return buffer;
}

EvictionResult evict_least_recently_used(
    CacheIndex& cache_index,
    ICopyEngine& copy_engine,
    const ICopyPathResolver& path_resolver,
    std::uint64_t target_bytes,
    ILogger* logger,
    const std::wstring& exclude
) {
    EvictionResult result;
    if (target_bytes == 0) {
        return result;
    }

    const auto log = [logger](const std::string& message) {
        if (logger != nullptr) {
            logger->log(message);
        }
    };

    for (const auto& victim : cache_index.cached_files_by_last_access()) {
        if (result.bytes_freed >= target_bytes) {
            break;
        }
        if (!exclude.empty() && victim.relative_path == exclude) {
            continue;
        }

        const auto cache_abs = path_resolver.cache_absolute(victim.relative_path);
        const auto remove_result = copy_engine.remove_cached_file(cache_abs);
        if (!remove_result.error_message.empty()) {
            log("eviction failed for '" + wide_to_utf8(victim.relative_path) +
                "': " + remove_result.error_message);
            continue;
        }

        cache_index.mark_removed(victim.relative_path);
        result.bytes_freed += victim.cached_size_bytes;
        ++result.files_removed;
        log("evicted '" + wide_to_utf8(victim.relative_path) + "' (" +
            std::to_string(victim.cached_size_bytes) + " bytes)");
    }

    return result;
}

}  // namespace ssd_cache
