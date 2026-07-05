#pragma once

/**
 * @file
 * @brief Hosts the SSD Cache service: SCM integration and the worker pipeline.
 */

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

/**
 * Runs the service either under the SCM or in a foreground console, owning the
 * worker that wires the driver activity source to the copy scheduler and
 * applies mode changes delivered as custom SCM controls.
 */
class WinServiceHost {
public:
    /**
     * @param service_name SCM name to register under.
     * @param config_path Path to the machine configuration file.
     */
    WinServiceHost(std::wstring service_name, std::wstring config_path);

    /**
     * Runs under the SCM via StartServiceCtrlDispatcher.
     *
     * @return 0 on clean dispatch, otherwise a Win32 error code.
     */
    int run();

    /**
     * Runs the worker in the foreground console (for debugging).
     *
     * @return 0 on clean shutdown, non-zero on worker failure.
     */
    int run_console();

    /**
     * SCM ServiceMain entry: registers the control handler and runs the worker.
     *
     * @param argc Service argument count (unused).
     * @param argv Service argument vector (unused).
     */
    void service_main(DWORD argc, wchar_t** argv);

    /**
     * SCM control handler for stop/shutdown and the custom mode controls.
     *
     * @param control The control code received.
     * @param event_type Control-specific event type (unused).
     * @param event_data Control-specific event data (unused).
     * @param context Registered handler context (unused).
     * @return NO_ERROR when handled, or an error code for a failed mode switch.
     */
    DWORD handle_control(
        DWORD control,
        DWORD event_type,
        void* event_data,
        void* context
    );

private:
    /** Worker body: builds the pipeline and blocks until stop is signaled. */
    void run_worker();

    /**
     * Reports a new service state to the SCM.
     *
     * @param state The SERVICE_* state to report.
     * @param win32_exit_code Exit code to report (defaults to NO_ERROR).
     */
    void set_status(DWORD state, DWORD win32_exit_code = NO_ERROR);

    /**
     * Applies a mode to the live pipeline: caching runs only in Monitor mode, so
     * Disabled and Serve pause the scheduler and stop new events from being
     * scheduled. Safe to call from the SCM control thread.
     *
     * @param mode The mode to apply (nullopt is treated as caching-enabled).
     */
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
