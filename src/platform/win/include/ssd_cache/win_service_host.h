#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <windows.h>

#include "ssd_cache/config.h"
#include "ssd_cache/win_file_logger.h"

namespace ssd_cache {

class PendingCopyScheduler;

class WinServiceHost {
public:
    WinServiceHost(std::wstring service_name, std::wstring config_path);

    int run();

    int run_console();

    void service_main(DWORD argc, wchar_t** argv);

    DWORD handle_control(
        DWORD control,
        DWORD event_type,
        void* event_data,
        void* context
    );

private:
    void run_worker();

    void set_status(DWORD state, DWORD win32_exit_code = NO_ERROR);

    // Applies a mode to the live pipeline: caching runs only in Monitor mode,
    // so Disabled and Serve pause the scheduler and stop new events from being
    // scheduled. Safe to call from the SCM control thread.
    void apply_caching_mode(const std::optional<AppMode>& mode);

    std::wstring service_name_;
    std::wstring config_path_;
    std::unique_ptr<WinFileLogger> logger_;
    SERVICE_STATUS_HANDLE status_handle_ = nullptr;
    SERVICE_STATUS status_{};
    HANDLE stop_event_ = nullptr;

    // Guards scheduler_ against the SCM control thread racing with run_worker
    // tearing the stack-owned scheduler down.
    std::mutex scheduler_mutex_;
    PendingCopyScheduler* scheduler_ = nullptr;
    std::atomic<bool> caching_active_{false};
};

}  // namespace ssd_cache
