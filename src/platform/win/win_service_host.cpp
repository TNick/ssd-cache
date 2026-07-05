#include "ssd_cache/win_service_host.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ssd_cache/cache_index.h"
#include "ssd_cache/config.h"
#include "ssd_cache/job_scheduler.h"
#include "ssd_cache/path_mapper.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_file_copy.h"
#include "ssd_cache/win_filter_activity_source.h"
#include "ssd_cache/win_mount_manager.h"
#include "ssd_cache/win_service_controller.h"

namespace ssd_cache {
namespace {

WinServiceHost* g_service = nullptr;

class RootedPathResolver final : public ICopyPathResolver {
public:
    explicit RootedPathResolver(AppConfig config) : config_(std::move(config)) {}

    std::wstring source_absolute(
        const std::wstring& relative_path
    ) const override {
        return join_root_relative(config_.source_unc, relative_path);
    }

    std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const override {
        return join_root_relative(cache_root_from_config(config_), relative_path);
    }

private:
    AppConfig config_;
};

void WINAPI service_main_thunk(DWORD argc, wchar_t** argv) {
    if (g_service != nullptr) {
        g_service->service_main(argc, argv);
    }
}

DWORD WINAPI handler_thunk(
    DWORD control,
    DWORD event_type,
    void* event_data,
    void* context
) {
    if (g_service == nullptr) {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    return g_service->handle_control(
        control,
        event_type,
        event_data,
        context
    );
}

AppConfig load_or_create_config(const std::wstring& path) {
    AppConfig config;
    config.sqlite_path = default_sqlite_path();

    if (std::filesystem::exists(std::filesystem::path(path))) {
        config = load_config_file(path);
        if (config.sqlite_path.empty()) {
            config.sqlite_path = default_sqlite_path();
        }
        return config;
    }

    save_config_file(path, config);
    return config;
}

bool move_cache_to_monitor_letter(const AppConfig& config) {
    const auto source_volume = volume_name_for_drive(
        config.source_presentation_letter
    );

    if (!source_volume || volume_name_for_drive(config.cache_letter)) {
        return true;
    }

    remove_drive_mount(config.source_presentation_letter);
    return assign_volume_to_drive(*source_volume, config.cache_letter);
}

bool move_cache_to_source_letter(const AppConfig& config) {
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

}  // namespace

WinServiceHost::WinServiceHost(
    std::wstring service_name,
    std::wstring config_path
)
    : service_name_(std::move(service_name)),
      config_path_(std::move(config_path)) {}

int WinServiceHost::run() {
    g_service = this;

    SERVICE_TABLE_ENTRYW table[] = {
        {service_name_.data(), service_main_thunk},
        {nullptr, nullptr},
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        return static_cast<int>(GetLastError());
    }

    return 0;
}

int WinServiceHost::run_console() {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
        return static_cast<int>(GetLastError());
    }

    try {
        run_worker();
    } catch (...) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
        return 1;
    }

    CloseHandle(stop_event_);
    stop_event_ = nullptr;
    return 0;
}

void WinServiceHost::service_main(DWORD, wchar_t**) {
    status_handle_ = RegisterServiceCtrlHandlerExW(
        service_name_.c_str(),
        handler_thunk,
        nullptr
    );
    if (status_handle_ == nullptr) {
        return;
    }

    status_.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status_.dwControlsAccepted =
        SERVICE_ACCEPT_STOP |
        SERVICE_ACCEPT_SHUTDOWN |
        SERVICE_ACCEPT_PARAMCHANGE;

    set_status(SERVICE_START_PENDING);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
        set_status(SERVICE_STOPPED, GetLastError());
        return;
    }

    try {
        set_status(SERVICE_RUNNING);
        run_worker();
        set_status(SERVICE_STOPPED);
    } catch (...) {
        set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }

    CloseHandle(stop_event_);
    stop_event_ = nullptr;
}

DWORD WinServiceHost::handle_control(
    DWORD control,
    DWORD,
    void*,
    void*
) {
    if (control == SERVICE_CONTROL_STOP ||
        control == SERVICE_CONTROL_SHUTDOWN) {
        set_status(SERVICE_STOP_PENDING);
        SetEvent(stop_event_);
        return NO_ERROR;
    }

    if (control == SERVICE_CONTROL_PARAMCHANGE) {
        return NO_ERROR;
    }

    const auto config = load_or_create_config(config_path_);
    if (control == kServiceControlDisabledMode) {
        return NO_ERROR;
    }

    if (control == kServiceControlMonitorMode) {
        return move_cache_to_monitor_letter(config) ? NO_ERROR :
            ERROR_SERVICE_SPECIFIC_ERROR;
    }

    if (control == kServiceControlServeMode) {
        return move_cache_to_source_letter(config) ? NO_ERROR :
            ERROR_SERVICE_SPECIFIC_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WinServiceHost::run_worker() {
    const auto config = load_or_create_config(config_path_);
    ensure_parent_directory(config.sqlite_path);

    CacheIndex cache_index(config.sqlite_path);
    cache_index.open();

    WinFileCopyEngine copy_engine;
    RootedPathResolver path_resolver(config);
    SchedulerSettings settings;
    settings.copy_delay = config.copy_delay;
    settings.compare_hash_before_overwrite =
        config.compare_hash_before_overwrite;
    PendingCopyScheduler scheduler(
        cache_index,
        copy_engine,
        path_resolver,
        settings
    );
    scheduler.start();

    std::unique_ptr<WinFilterActivitySource> activity_source;
    if (!config.source_unc.empty()) {
        activity_source = std::make_unique<WinFilterActivitySource>(
            config.source_unc,
            [&scheduler](const AccessEvent& event) {
                scheduler.record_event(event);
            }
        );
        activity_source->start();
    }

    WaitForSingleObject(stop_event_, INFINITE);

    if (activity_source) {
        activity_source->stop();
    }
    scheduler.stop();
}

void WinServiceHost::set_status(DWORD state, DWORD win32_exit_code) {
    status_.dwCurrentState = state;
    status_.dwWin32ExitCode = win32_exit_code;
    status_.dwCheckPoint =
        state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ?
            status_.dwCheckPoint + 1 :
            0;
    status_.dwWaitHint =
        state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ?
            3000 :
            0;

    if (status_handle_ != nullptr) {
        SetServiceStatus(status_handle_, &status_);
    }
}

}  // namespace ssd_cache
