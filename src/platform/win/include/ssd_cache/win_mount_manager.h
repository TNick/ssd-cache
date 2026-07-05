#pragma once

#include <optional>
#include <string>

#include "ssd_cache/interfaces.h"

namespace ssd_cache {

std::wstring drive_root(wchar_t letter);

std::optional<std::wstring> network_unc_for_drive(wchar_t letter);

bool map_network_drive(wchar_t letter, const std::wstring& unc);

bool unmap_network_drive(wchar_t letter);

std::optional<std::wstring> volume_name_for_drive(wchar_t letter);

bool assign_volume_to_drive(const std::wstring& volume_name, wchar_t letter);

bool remove_drive_mount(wchar_t letter);

class WinMountPresentation final : public IMountPresentation {
public:
    bool enter_monitor_mode(const AppConfig& config) override;

    bool enter_serve_mode(const AppConfig& config) override;

    bool enter_disabled_mode(const AppConfig& config) override;
};

}  // namespace ssd_cache
