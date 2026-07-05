#include "ssd_cache/win_service_controller.h"

#include <utility>

namespace ssd_cache {
namespace {

class ScHandle {
public:
    explicit ScHandle(SC_HANDLE handle) : handle_(handle) {}

    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;

    ~ScHandle() {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
    }

    SC_HANDLE get() const {
        return handle_;
    }

private:
    SC_HANDLE handle_ = nullptr;
};

DWORD control_code_for_mode(AppMode mode) {
    switch (mode) {
        case AppMode::Disabled:
            return kServiceControlDisabledMode;
        case AppMode::Monitor:
            return kServiceControlMonitorMode;
        case AppMode::Serve:
            return kServiceControlServeMode;
    }

    return kServiceControlDisabledMode;
}

}  // namespace

WinServiceController::WinServiceController(std::wstring service_name)
    : service_name_(std::move(service_name)) {}

bool WinServiceController::install_service(
    const std::wstring& binary_path
) const {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (manager.get() == nullptr) {
        return false;
    }

    ScHandle service(CreateServiceW(
        manager.get(),
        service_name_.c_str(),
        L"SSD Cache Service",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        binary_path.c_str(),
        nullptr,
        nullptr,
        nullptr,
        L"NT AUTHORITY\\NetworkService",
        nullptr
    ));

    return service.get() != nullptr || GetLastError() == ERROR_SERVICE_EXISTS;
}

bool WinServiceController::start_service() const {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (manager.get() == nullptr) {
        return false;
    }

    ScHandle service(OpenServiceW(
        manager.get(),
        service_name_.c_str(),
        SERVICE_START | SERVICE_QUERY_STATUS
    ));
    if (service.get() == nullptr) {
        return false;
    }

    if (StartServiceW(service.get(), 0, nullptr)) {
        return true;
    }

    return GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
}

bool WinServiceController::stop_service() const {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (manager.get() == nullptr) {
        return false;
    }

    ScHandle service(OpenServiceW(
        manager.get(),
        service_name_.c_str(),
        SERVICE_STOP | SERVICE_QUERY_STATUS
    ));
    if (service.get() == nullptr) {
        return false;
    }

    SERVICE_STATUS status{};
    if (ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        return true;
    }

    return GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
}

bool WinServiceController::send_mode(AppMode mode) const {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (manager.get() == nullptr) {
        return false;
    }

    ScHandle service(OpenServiceW(
        manager.get(),
        service_name_.c_str(),
        SERVICE_USER_DEFINED_CONTROL | SERVICE_QUERY_STATUS
    ));
    if (service.get() == nullptr) {
        return false;
    }

    SERVICE_STATUS status{};
    return ControlService(
        service.get(),
        control_code_for_mode(mode),
        &status
    ) != FALSE;
}

}  // namespace ssd_cache
