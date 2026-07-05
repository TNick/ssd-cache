#include "ssd_cache/win_config_paths.h"

#include <filesystem>
#include <stdexcept>

#include <shlobj.h>
#include <windows.h>

namespace ssd_cache {
namespace {

std::wstring known_folder_path(REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(folder_id, 0, nullptr, &raw_path);
    if (FAILED(hr)) {
        throw std::runtime_error("failed to resolve known folder");
    }

    std::wstring result(raw_path);
    CoTaskMemFree(raw_path);
    return result;
}

}  // namespace

std::wstring program_data_app_dir() {
    auto path = known_folder_path(FOLDERID_ProgramData);
    path.append(L"\\ssd-cache");
    std::filesystem::create_directories(path);
    return path;
}

std::wstring default_config_path() {
    return program_data_app_dir() + L"\\config.ini";
}

std::wstring default_sqlite_path() {
    return program_data_app_dir() + L"\\cache-index.sqlite3";
}

void ensure_parent_directory(const std::wstring& path) {
    const std::filesystem::path file_path(path);
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
    }
}

}  // namespace ssd_cache
