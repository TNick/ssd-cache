#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssd_cache {

enum class AppMode {
    Disabled,
    Monitor,
    Serve,
};

struct AppConfig {
    std::wstring source_unc;
    wchar_t source_presentation_letter = L'N';
    wchar_t cache_letter = L'K';
    std::wstring sqlite_path;
    std::optional<AppMode> mode;
    std::chrono::seconds copy_delay{60};
    bool compare_hash_before_overwrite = false;
    // Minimum free space to keep on the cache volume. Before caching a file the
    // service evicts the least recently accessed cached files until free space
    // reaches at least max(incoming file size, this threshold). Default 6 GiB.
    std::uint64_t min_free_bytes = 6ULL * 1024 * 1024 * 1024;
    std::vector<std::wstring> ignored_process_patterns{L"explorer.exe"};
    std::vector<std::wstring> ignored_path_patterns{
        L"~$*",
        L"*-wal",
        L"*-shm",
        L"*-journal",
    };
};

std::wstring cache_root_from_config(const AppConfig& config);

std::wstring source_presentation_root_from_config(const AppConfig& config);

bool process_matches_ignored_patterns(
    const AppConfig& config,
    std::wstring_view process_path
);

bool path_matches_ignored_patterns(
    const AppConfig& config,
    std::wstring_view relative_path
);

std::wstring app_mode_to_wstring(AppMode mode);

AppMode app_mode_from_wstring(const std::wstring& value);

AppConfig load_config_file(const std::wstring& path);

void save_config_file(const std::wstring& path, const AppConfig& config);

}  // namespace ssd_cache
