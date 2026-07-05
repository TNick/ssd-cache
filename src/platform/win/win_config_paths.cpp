/**
 * @file
 * @brief Implementation of the well-known paths and ACL/directory helpers.
 */

#include "ssd_cache/win_config_paths.h"

#include <filesystem>
#include <stdexcept>

#include <aclapi.h>
#include <shlobj.h>
#include <windows.h>

namespace ssd_cache {
namespace {

/**
 * Resolves a known-folder path (e.g. %ProgramData%).
 *
 * @param folder_id The KNOWNFOLDERID to resolve.
 * @return The folder's path.
 * @throws std::runtime_error if resolution fails.
 */
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

/**
 * Throws a runtime_error with a fixed message (the Win32 error is accepted for
 * call-site clarity but not embedded in the message).
 *
 * @param message Message for the exception.
 * @param error The Win32 error code (unused in the message).
 */
void throw_last_error(const char* message, DWORD error) {
    (void)error;
    throw std::runtime_error(message);
}

}  // namespace

std::wstring program_data_app_dir() {
    // Resolve %ProgramData%\ssd-cache and make sure it exists.
    auto path = known_folder_path(FOLDERID_ProgramData);
    path.append(L"\\ssd-cache");
    std::filesystem::create_directories(path);

    // Grant the service account access best-effort; failures here are ignored
    // (e.g. when running without permission to change the ACL).
    try {
        ensure_network_service_access(path, true);
    } catch (...) {
    }
    return path;
}

std::wstring default_config_path() {
    return program_data_app_dir() + L"\\config.ini";
}

std::wstring default_sqlite_path() {
    return program_data_app_dir() + L"\\cache-index.sqlite3";
}

std::wstring default_app_log_path() {
    return program_data_app_dir() + L"\\app.log";
}

std::wstring default_service_log_path() {
    return program_data_app_dir() + L"\\service.log";
}

std::wstring default_driver_log_path() {
    return program_data_app_dir() + L"\\driver.log";
}

std::wstring nt_path_from_win32_path(const std::wstring& path) {
    return L"\\??\\" + path;
}

void ensure_network_service_access(
    const std::wstring& path,
    bool is_directory
) {
    BYTE sid_buffer[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid_buffer);
    PACL existing_dacl = nullptr;
    PACL updated_dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    // Build the well-known NETWORK SERVICE SID that the grant targets.
    if (!CreateWellKnownSid(
        WinNetworkServiceSid,
        nullptr,
        sid_buffer,
        &sid_size
    )) {
        throw_last_error(
            "failed to create NetworkService SID",
            GetLastError()
        );
    }

    // Describe a read/write/execute grant to that SID, inherited by children
    // when the target is a directory.
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = is_directory ?
        SUB_CONTAINERS_AND_OBJECTS_INHERIT :
        NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(
        reinterpret_cast<wchar_t*>(sid_buffer)
    );

    // Read the object's current DACL.
    DWORD status = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &existing_dacl,
        nullptr,
        &descriptor
    );
    if (status != ERROR_SUCCESS) {
        throw_last_error("failed to read file security info", status);
    }

    // Merge the new grant into a fresh DACL.
    status = SetEntriesInAclW(1, &access, existing_dacl, &updated_dacl);
    if (status != ERROR_SUCCESS) {
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
        throw_last_error("failed to update ACL entries", status);
    }

    // Write the updated DACL back onto the object.
    status = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        updated_dacl,
        nullptr
    );

    // Free the DACL/descriptor buffers regardless of outcome.
    if (updated_dacl != nullptr) {
        LocalFree(updated_dacl);
    }
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }

    if (status != ERROR_SUCCESS) {
        throw_last_error("failed to write file security info", status);
    }
}

void ensure_parent_directory(const std::wstring& path) {
    const std::filesystem::path file_path(path);
    if (file_path.has_parent_path()) {
        // Create the parent chain, then grant the service account access
        // best-effort (ignoring ACL failures).
        const auto parent = file_path.parent_path().wstring();
        std::filesystem::create_directories(parent);
        try {
            ensure_network_service_access(parent, true);
        } catch (...) {
        }
    }
}

}  // namespace ssd_cache
