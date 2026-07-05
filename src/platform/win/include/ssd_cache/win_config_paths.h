#pragma once

#include <string>

namespace ssd_cache {

std::wstring program_data_app_dir();

std::wstring default_config_path();

std::wstring default_sqlite_path();

std::wstring default_app_log_path();

std::wstring default_service_log_path();

std::wstring default_driver_log_path();

std::wstring nt_path_from_win32_path(const std::wstring& path);

void ensure_network_service_access(
    const std::wstring& path,
    bool is_directory
);

void ensure_parent_directory(const std::wstring& path);

}  // namespace ssd_cache
