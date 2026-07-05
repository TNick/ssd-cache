#pragma once

#include <string>

namespace ssd_cache {

std::wstring program_data_app_dir();

std::wstring default_config_path();

std::wstring default_sqlite_path();

void ensure_parent_directory(const std::wstring& path);

}  // namespace ssd_cache
