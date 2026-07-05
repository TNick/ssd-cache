/**
 * @file
 * @brief Implementation of config parsing/serialization and path/process
 *        filtering.
 */

#include "ssd_cache/config.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "ssd_cache/utf.h"

namespace ssd_cache {
namespace {

/**
 * Trims leading and trailing whitespace.
 *
 * @param value String to trim (taken by value and modified in place).
 * @return The trimmed string.
 */
std::wstring trim(std::wstring value) {
    const auto not_space = [](wchar_t ch) {
        return std::iswspace(ch) == 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), not_space).base(),
        value.end()
    );
    return value;
}

/**
 * Lowercases a string.
 *
 * @param value Input string.
 * @return A lowercased copy of @p value.
 */
std::wstring lower_copy(std::wstring_view value) {
    std::wstring lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lowered;
}

/**
 * Extracts the final component of a path.
 *
 * @param path Path to inspect (either separator style).
 * @return The substring after the last separator, or all of @p path if none.
 */
std::wstring filename_from_path(std::wstring_view path) {
    const auto last_separator = path.find_last_of(L"\\/");
    if (last_separator == std::wstring_view::npos) {
        return std::wstring(path);
    }

    return std::wstring(path.substr(last_separator + 1));
}

/**
 * Case-insensitive wildcard match supporting `*` (any run) and `?` (any one
 * character).
 *
 * @param pattern Wildcard pattern.
 * @param value Value to test against @p pattern.
 * @return True if @p value matches @p pattern.
 */
bool wildcard_match_ignore_case(
    std::wstring_view pattern,
    std::wstring_view value
) {
    const auto lowered_pattern = lower_copy(pattern);
    const auto lowered_value = lower_copy(value);
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star_index = std::wstring::npos;
    std::size_t star_match_index = 0;

    while (value_index < lowered_value.size()) {
        if (pattern_index < lowered_pattern.size() &&
            (lowered_pattern[pattern_index] == L'?' ||
             lowered_pattern[pattern_index] == lowered_value[value_index])) {
            ++pattern_index;
            ++value_index;
            continue;
        }

        if (pattern_index < lowered_pattern.size() &&
            lowered_pattern[pattern_index] == L'*') {
            star_index = pattern_index;
            star_match_index = value_index;
            ++pattern_index;
            continue;
        }

        if (star_index != std::wstring::npos) {
            pattern_index = star_index + 1;
            ++star_match_index;
            value_index = star_match_index;
            continue;
        }

        return false;
    }

    while (pattern_index < lowered_pattern.size() &&
        lowered_pattern[pattern_index] == L'*') {
        ++pattern_index;
    }

    return pattern_index == lowered_pattern.size();
}

/**
 * Reads an INI-style file into key/value pairs, skipping blank and `#`-comment
 * lines and trimming both sides of each `key=value`.
 *
 * @param path Path to the file to read.
 * @return The parsed key/value map (empty if the file cannot be opened).
 */
std::map<std::wstring, std::wstring> read_key_values(const std::wstring& path) {
    std::ifstream input{std::filesystem::path(path)};
    if (!input) {
        return {};
    }

    std::map<std::wstring, std::wstring> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        values.emplace(
            trim(utf8_to_wide(std::string_view(line).substr(0, pos))),
            trim(utf8_to_wide(std::string_view(line).substr(pos + 1)))
        );
    }

    return values;
}

/**
 * Reads a drive-letter value from a key/value map.
 *
 * @param values Parsed key/value map.
 * @param key Key to read.
 * @param fallback Letter returned when the key is absent or empty.
 * @return The upper-cased first character of the value, or @p fallback.
 */
wchar_t read_letter(
    const std::map<std::wstring, std::wstring>& values,
    const std::wstring& key,
    wchar_t fallback
) {
    const auto iter = values.find(key);
    if (iter == values.end() || iter->second.empty()) {
        return fallback;
    }

    return static_cast<wchar_t>(std::towupper(iter->second.front()));
}

/**
 * Splits a delimited pattern list on `,` or `;`, trimming and dropping empties.
 *
 * @param value The delimited list.
 * @return The individual, trimmed, non-empty patterns.
 */
std::vector<std::wstring> split_patterns(std::wstring_view value) {
    std::vector<std::wstring> patterns;
    std::size_t start = 0;

    while (start <= value.size()) {
        std::size_t end = start;
        while (end < value.size() && value[end] != L',' && value[end] != L';') {
            ++end;
        }

        auto pattern = trim(std::wstring(value.substr(start, end - start)));
        if (!pattern.empty()) {
            patterns.push_back(std::move(pattern));
        }

        if (end == value.size()) {
            break;
        }

        start = end + 1;
    }

    return patterns;
}

/**
 * Joins patterns into a single `;`-delimited string for persistence.
 *
 * @param patterns Patterns to join.
 * @return The `;`-delimited list.
 */
std::wstring join_patterns(const std::vector<std::wstring>& patterns) {
    std::wstring result;
    for (const auto& pattern : patterns) {
        if (!result.empty()) {
            result.push_back(L';');
        }

        result.append(pattern);
    }

    return result;
}

/**
 * Tests a value against a list of wildcard patterns, matching either the whole
 * value or just its final path component.
 *
 * @param patterns Patterns to test against.
 * @param value Value to test (empty never matches).
 * @return True if any pattern matches @p value or its file name.
 */
bool value_matches_any_pattern(
    const std::vector<std::wstring>& patterns,
    std::wstring_view value
) {
    if (value.empty()) {
        return false;
    }

    const auto filename = filename_from_path(value);
    for (const auto& pattern : patterns) {
        if (wildcard_match_ignore_case(pattern, value) ||
            wildcard_match_ignore_case(pattern, filename)) {
            return true;
        }
    }

    return false;
}

}  // namespace

std::wstring cache_root_from_config(const AppConfig& config) {
    std::wstring root;
    root.push_back(config.cache_letter);
    root.append(L":\\");
    return root;
}

std::wstring source_presentation_root_from_config(const AppConfig& config) {
    std::wstring root;
    root.push_back(config.source_presentation_letter);
    root.append(L":\\");
    return root;
}

bool process_matches_ignored_patterns(
    const AppConfig& config,
    std::wstring_view process_path
) {
    return value_matches_any_pattern(
        config.ignored_process_patterns,
        process_path
    );
}

bool path_matches_ignored_patterns(
    const AppConfig& config,
    std::wstring_view relative_path
) {
    return value_matches_any_pattern(config.ignored_path_patterns, relative_path);
}

std::wstring app_mode_to_wstring(AppMode mode) {
    switch (mode) {
        case AppMode::Disabled:
            return L"disabled";
        case AppMode::Monitor:
            return L"monitor";
        case AppMode::Serve:
            return L"serve";
    }

    throw std::invalid_argument("unknown app mode");
}

AppMode app_mode_from_wstring(const std::wstring& value) {
    auto lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });

    if (lowered == L"disabled") {
        return AppMode::Disabled;
    }

    if (lowered == L"monitor") {
        return AppMode::Monitor;
    }

    if (lowered == L"serve") {
        return AppMode::Serve;
    }

    throw std::invalid_argument("unknown app mode string");
}

AppConfig load_config_file(const std::wstring& path) {
    const auto values = read_key_values(path);
    AppConfig config;

    if (const auto iter = values.find(L"source_unc"); iter != values.end()) {
        config.source_unc = iter->second;
    }

    if (const auto iter = values.find(L"sqlite_path"); iter != values.end()) {
        config.sqlite_path = iter->second;
    }

    if (const auto iter = values.find(L"mode"); iter != values.end() &&
        !iter->second.empty()) {
        config.mode = app_mode_from_wstring(iter->second);
    }

    if (const auto iter = values.find(L"copy_delay_seconds"); iter != values.end()) {
        config.copy_delay = std::chrono::seconds(std::stoll(iter->second));
    }

    if (const auto iter = values.find(L"compare_hash_before_overwrite");
        iter != values.end()) {
        config.compare_hash_before_overwrite =
            iter->second == L"1" || iter->second == L"true";
    }

    if (const auto iter = values.find(L"min_free_space_mb");
        iter != values.end() && !iter->second.empty()) {
        config.min_free_bytes =
            static_cast<std::uint64_t>(std::stoull(iter->second)) *
            (1024ULL * 1024ULL);
    }

    if (const auto iter = values.find(L"ignored_process_patterns");
        iter != values.end()) {
        config.ignored_process_patterns = split_patterns(iter->second);
    }

    if (const auto iter = values.find(L"ignored_path_patterns");
        iter != values.end()) {
        config.ignored_path_patterns = split_patterns(iter->second);
    }

    config.source_presentation_letter =
        read_letter(values, L"source_presentation_letter", L'N');
    config.cache_letter = read_letter(values, L"cache_letter", L'K');
    return config;
}

void save_config_file(const std::wstring& path, const AppConfig& config) {
    const std::filesystem::path config_path(path);
    if (config_path.has_parent_path()) {
        std::filesystem::create_directories(config_path.parent_path());
    }

    std::ofstream output(config_path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write config file");
    }

    output << "source_unc=" << wide_to_utf8(config.source_unc) << "\n";
    output << "source_presentation_letter="
           << narrow_ascii(std::wstring(1, config.source_presentation_letter))
           << "\n";
    output << "cache_letter="
           << narrow_ascii(std::wstring(1, config.cache_letter))
           << "\n";
    output << "sqlite_path=" << wide_to_utf8(config.sqlite_path) << "\n";
    if (config.mode.has_value()) {
        output << "mode=" << wide_to_utf8(app_mode_to_wstring(*config.mode))
               << "\n";
    }
    output << "copy_delay_seconds=" << config.copy_delay.count() << "\n";
    output << "compare_hash_before_overwrite="
           << (config.compare_hash_before_overwrite ? "1" : "0") << "\n";
    output << "min_free_space_mb=" << (config.min_free_bytes / (1024ULL * 1024ULL))
           << "\n";
    output << "ignored_process_patterns="
           << wide_to_utf8(join_patterns(config.ignored_process_patterns))
           << "\n";
    output << "ignored_path_patterns="
           << wide_to_utf8(join_patterns(config.ignored_path_patterns))
           << "\n";
}

}  // namespace ssd_cache
