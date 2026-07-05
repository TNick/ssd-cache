#pragma once

/**
 * @file
 * @brief Well-known Windows paths and filesystem/ACL helpers for the app.
 */

#include <string>

namespace ssd_cache {

/**
 * @return The application's per-machine data directory under %ProgramData%.
 */
std::wstring program_data_app_dir();

/**
 * @return The default path of the machine configuration file.
 */
std::wstring default_config_path();

/**
 * @return The default path of the SQLite cache-index database.
 */
std::wstring default_sqlite_path();

/**
 * @return The default path of the tray application log file.
 */
std::wstring default_app_log_path();

/**
 * @return The default path of the service log file.
 */
std::wstring default_service_log_path();

/**
 * @return The default path used for the driver's log output.
 */
std::wstring default_driver_log_path();

/**
 * Converts a Win32 path to its NT (\\Device\\...) form, as the driver expects.
 *
 * @param path A Win32 path.
 * @return The equivalent NT path.
 */
std::wstring nt_path_from_win32_path(const std::wstring& path);

/**
 * Grants the NETWORK SERVICE account access to a path so the service (running
 * under that account) can use it.
 *
 * @param path The file or directory to adjust.
 * @param is_directory True if @p path is a directory (affects inheritance).
 */
void ensure_network_service_access(
    const std::wstring& path,
    bool is_directory
);

/**
 * Creates the parent directory of a path if it does not already exist.
 *
 * @param path The file path whose parent directory is ensured.
 */
void ensure_parent_directory(const std::wstring& path);

}  // namespace ssd_cache
