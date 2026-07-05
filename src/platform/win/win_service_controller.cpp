#include "ssd_cache/win_service_controller.h"

#include <cstddef>
#include <cwchar>
#include <vector>
#include <utility>

#include <fltuser.h>

namespace ssd_cache {
namespace {

constexpr const wchar_t* kLocalSystemAccount = L"LocalSystem";

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

class FilterFindHandle {
public:
    explicit FilterFindHandle(HANDLE handle) : handle_(handle) {}

    FilterFindHandle(const FilterFindHandle&) = delete;
    FilterFindHandle& operator=(const FilterFindHandle&) = delete;

    ~FilterFindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            FilterFindClose(handle_);
        }
    }

    HANDLE get() const {
        return handle_;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
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

std::wstring quoted_path(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

SC_HANDLE open_service_handle(
    SC_HANDLE manager,
    const std::wstring& service_name,
    DWORD access
) {
    return OpenServiceW(manager, service_name.c_str(), access);
}

bool verify_service_exists(
    SC_HANDLE manager,
    const std::wstring& service_name
) {
    ScHandle service(open_service_handle(
        manager,
        service_name,
        SERVICE_QUERY_STATUS
    ));
    return service.get() != nullptr;
}

bool matches_filter_name(
    const FILTER_FULL_INFORMATION* info,
    const std::wstring& filter_name
) {
    const auto length = static_cast<std::size_t>(info->FilterNameLength) /
        sizeof(wchar_t);
    return length == filter_name.size() &&
        _wcsnicmp(
            info->FilterNameBuffer,
            filter_name.c_str(),
            length
        ) == 0;
}

bool buffer_contains_filter(
    const void* buffer,
    const std::wstring& filter_name
) {
    auto* current =
        static_cast<const FILTER_FULL_INFORMATION*>(buffer);

    while (current != nullptr) {
        if (matches_filter_name(current, filter_name)) {
            return true;
        }

        if (current->NextEntryOffset == 0) {
            return false;
        }

        current = reinterpret_cast<const FILTER_FULL_INFORMATION*>(
            reinterpret_cast<const std::byte*>(current) +
            current->NextEntryOffset
        );
    }

    return false;
}

bool find_loaded_filter(const std::wstring& filter_name) {
    std::vector<std::byte> buffer(4096);
    DWORD bytes_returned = 0;
    HANDLE raw_handle = INVALID_HANDLE_VALUE;

    HRESULT hr = FilterFindFirst(
        FilterFullInformation,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_returned,
        &raw_handle
    );
    if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
        bytes_returned > buffer.size()) {
        buffer.resize(bytes_returned);
        hr = FilterFindFirst(
            FilterFullInformation,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned,
            &raw_handle
        );
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
        return false;
    }

    if (FAILED(hr)) {
        return false;
    }

    FilterFindHandle handle(raw_handle);
    if (buffer_contains_filter(buffer.data(), filter_name)) {
        return true;
    }

    while (true) {
        bytes_returned = 0;
        hr = FilterFindNext(
            handle.get(),
            FilterFullInformation,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned
        );
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
            bytes_returned > buffer.size()) {
            buffer.resize(bytes_returned);
            hr = FilterFindNext(
                handle.get(),
                FilterFullInformation,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytes_returned
            );
        }

        if (hr == HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)) {
            return false;
        }

        if (FAILED(hr)) {
            return false;
        }

        if (buffer_contains_filter(buffer.data(), filter_name)) {
            return true;
        }
    }
}

ServiceState convert_service_state(DWORD state) {
    switch (state) {
        case SERVICE_RUNNING:
            return ServiceState::Running;
        case SERVICE_START_PENDING:
            return ServiceState::StartPending;
        case SERVICE_STOP_PENDING:
            return ServiceState::StopPending;
        default:
            return ServiceState::Stopped;
    }
}

}  // namespace

std::wstring service_state_to_wstring(ServiceState state) {
    switch (state) {
        case ServiceState::Missing:
            return L"missing";
        case ServiceState::Stopped:
            return L"stopped";
        case ServiceState::StartPending:
            return L"start_pending";
        case ServiceState::StopPending:
            return L"stop_pending";
        case ServiceState::Running:
            return L"running";
    }

    return L"unknown";
}

ServiceState query_service_state(const std::wstring& service_name) {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (manager.get() == nullptr) {
        return ServiceState::Missing;
    }

    ScHandle service(OpenServiceW(
        manager.get(),
        service_name.c_str(),
        SERVICE_QUERY_STATUS
    ));
    if (service.get() == nullptr) {
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ?
            ServiceState::Missing :
            ServiceState::Stopped;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    const BOOL ok = QueryServiceStatusEx(
        service.get(),
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status),
        sizeof(status),
        &bytes_needed
    );
    if (!ok) {
        return ServiceState::Stopped;
    }

    return convert_service_state(status.dwCurrentState);
}

ServiceState query_driver_state() {
    const auto service_state = query_service_state(kDriverServiceName);
    if (service_state == ServiceState::Missing ||
        service_state == ServiceState::StartPending ||
        service_state == ServiceState::StopPending ||
        service_state == ServiceState::Running) {
        return service_state;
    }

    if (find_loaded_filter(kDriverServiceName)) {
        return ServiceState::Running;
    }

    return ServiceState::Stopped;
}

WinServiceController::WinServiceController(std::wstring service_name)
    : service_name_(std::move(service_name)) {}

bool WinServiceController::install_service(
    const std::wstring& binary_path
) const {
    ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (manager.get() == nullptr) {
        return false;
    }

    const auto service_binary = quoted_path(binary_path);
    ScHandle service(CreateServiceW(
        manager.get(),
        service_name_.c_str(),
        L"SSD Cache Service",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        service_binary.c_str(),
        nullptr,
        nullptr,
        nullptr,
        kLocalSystemAccount,
        nullptr
    ));

    if (service.get() != nullptr) {
        return verify_service_exists(manager.get(), service_name_);
    }

    if (GetLastError() != ERROR_SERVICE_EXISTS) {
        return false;
    }

    ScHandle existing_service(open_service_handle(
        manager.get(),
        service_name_,
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS
    ));
    if (existing_service.get() == nullptr) {
        return false;
    }

    const BOOL updated = ChangeServiceConfigW(
        existing_service.get(),
        SERVICE_NO_CHANGE,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        service_binary.c_str(),
        nullptr,
        nullptr,
        nullptr,
        kLocalSystemAccount,
        nullptr,
        L"SSD Cache Service"
    );
    if (!updated) {
        return false;
    }

    return verify_service_exists(manager.get(), service_name_);
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

bool WinServiceController::start_driver() const {
    const HRESULT hr = FilterLoad(kDriverServiceName);
    return SUCCEEDED(hr) ||
        hr == HRESULT_FROM_WIN32(ERROR_SERVICE_ALREADY_RUNNING);
}

bool WinServiceController::stop_driver() const {
    const HRESULT hr = FilterUnload(kDriverServiceName);
    return SUCCEEDED(hr) ||
        hr == HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
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

ServiceState WinServiceController::query_state() const {
    return query_service_state(service_name_);
}

}  // namespace ssd_cache
