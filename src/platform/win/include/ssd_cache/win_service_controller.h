#pragma once

#include <string>

#include <windows.h>

#include "ssd_cache/config.h"

namespace ssd_cache {

constexpr const wchar_t* kServiceName = L"SsdCacheService";
constexpr const wchar_t* kDriverServiceName = L"CacheMon";
constexpr DWORD kServiceControlDisabledMode = 128;
constexpr DWORD kServiceControlMonitorMode = 129;
constexpr DWORD kServiceControlServeMode = 130;

enum class ServiceState {
    Missing,
    Stopped,
    StartPending,
    StopPending,
    Running
};

ServiceState query_service_state(const std::wstring& service_name);

ServiceState query_driver_state();

std::wstring service_state_to_wstring(ServiceState state);

class WinServiceController {
public:
    explicit WinServiceController(std::wstring service_name = kServiceName);

    bool install_service(const std::wstring& binary_path) const;

    bool start_service() const;

    bool stop_service() const;

    bool start_driver() const;

    bool stop_driver() const;

    bool send_mode(AppMode mode) const;

    ServiceState query_state() const;

private:
    std::wstring service_name_;
};

}  // namespace ssd_cache
