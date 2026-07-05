#pragma once

#include <string>

#include <windows.h>

#include "ssd_cache/config.h"

namespace ssd_cache {

constexpr const wchar_t* kServiceName = L"SsdCacheService";
constexpr DWORD kServiceControlDisabledMode = 128;
constexpr DWORD kServiceControlMonitorMode = 129;
constexpr DWORD kServiceControlServeMode = 130;

class WinServiceController {
public:
    explicit WinServiceController(std::wstring service_name = kServiceName);

    bool install_service(const std::wstring& binary_path) const;

    bool start_service() const;

    bool stop_service() const;

    bool send_mode(AppMode mode) const;

private:
    std::wstring service_name_;
};

}  // namespace ssd_cache
