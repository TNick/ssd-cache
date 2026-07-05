#include "ssd_cache/win_mount_manager.h"

#include <array>
#include <cwctype>

#include <windows.h>
#include <winnetwk.h>

#include "ssd_cache/config.h"

namespace ssd_cache {

std::wstring drive_root(wchar_t letter) {
    std::wstring root;
    root.push_back(static_cast<wchar_t>(std::towupper(letter)));
    root.append(L":\\");
    return root;
}

std::optional<std::wstring> network_unc_for_drive(wchar_t letter) {
    std::wstring local;
    local.push_back(static_cast<wchar_t>(std::towupper(letter)));
    local.push_back(L':');

    std::array<wchar_t, 1024> remote{};
    DWORD remote_size = static_cast<DWORD>(remote.size());
    const DWORD rc = WNetGetConnectionW(local.c_str(), remote.data(), &remote_size);
    if (rc != NO_ERROR) {
        return std::nullopt;
    }

    return std::wstring(remote.data());
}

bool map_network_drive(wchar_t letter, const std::wstring& unc) {
    NETRESOURCEW resource{};
    std::wstring local;
    local.push_back(static_cast<wchar_t>(std::towupper(letter)));
    local.push_back(L':');

    resource.dwType = RESOURCETYPE_DISK;
    resource.lpLocalName = local.data();
    resource.lpRemoteName = const_cast<wchar_t*>(unc.c_str());

    const DWORD rc = WNetAddConnection2W(&resource, nullptr, nullptr, 0);
    return rc == NO_ERROR || rc == ERROR_ALREADY_ASSIGNED;
}

bool unmap_network_drive(wchar_t letter) {
    std::wstring local;
    local.push_back(static_cast<wchar_t>(std::towupper(letter)));
    local.push_back(L':');

    const DWORD rc = WNetCancelConnection2W(local.c_str(), 0, TRUE);
    return rc == NO_ERROR || rc == ERROR_NOT_CONNECTED;
}

std::optional<std::wstring> volume_name_for_drive(wchar_t letter) {
    std::array<wchar_t, MAX_PATH> volume_name{};
    const auto root = drive_root(letter);
    if (!GetVolumeNameForVolumeMountPointW(
            root.c_str(),
            volume_name.data(),
            static_cast<DWORD>(volume_name.size())
        )) {
        return std::nullopt;
    }

    return std::wstring(volume_name.data());
}

bool assign_volume_to_drive(const std::wstring& volume_name, wchar_t letter) {
    const auto root = drive_root(letter);
    return SetVolumeMountPointW(root.c_str(), volume_name.c_str()) != FALSE;
}

bool remove_drive_mount(wchar_t letter) {
    const auto root = drive_root(letter);
    if (DeleteVolumeMountPointW(root.c_str())) {
        return true;
    }

    const DWORD error = GetLastError();
    return error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_PARAMETER;
}

bool WinMountPresentation::enter_monitor_mode(const AppConfig& config) {
    const auto source_volume = volume_name_for_drive(
        config.source_presentation_letter
    );

    if (source_volume && !volume_name_for_drive(config.cache_letter)) {
        remove_drive_mount(config.source_presentation_letter);
        assign_volume_to_drive(*source_volume, config.cache_letter);
    }

    return map_network_drive(
        config.source_presentation_letter,
        config.source_unc
    );
}

bool WinMountPresentation::enter_serve_mode(const AppConfig& config) {
    unmap_network_drive(config.source_presentation_letter);

    const auto cache_volume = volume_name_for_drive(config.cache_letter);
    if (!cache_volume) {
        return false;
    }

    remove_drive_mount(config.cache_letter);
    remove_drive_mount(config.source_presentation_letter);
    return assign_volume_to_drive(
        *cache_volume,
        config.source_presentation_letter
    );
}

bool WinMountPresentation::enter_disabled_mode(const AppConfig&) {
    return true;
}

}  // namespace ssd_cache
