#pragma once

#include <chrono>
#include <string>

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
    std::chrono::seconds copy_delay{60};
    bool compare_hash_before_overwrite = false;
};

std::wstring cache_root_from_config(const AppConfig& config);

std::wstring source_presentation_root_from_config(const AppConfig& config);

std::wstring app_mode_to_wstring(AppMode mode);

AppMode app_mode_from_wstring(const std::wstring& value);

AppConfig load_config_file(const std::wstring& path);

void save_config_file(const std::wstring& path, const AppConfig& config);

}  // namespace ssd_cache
