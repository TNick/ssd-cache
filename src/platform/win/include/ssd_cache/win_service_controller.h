#pragma once

/**
 * @file
 * @brief Service/driver state queries and SCM control (install/start/stop/mode).
 */

#include <string>

#include <windows.h>

#include "ssd_cache/config.h"

namespace ssd_cache {

/** SCM name of the user-mode service. */
constexpr const wchar_t* kServiceName = L"SsdCacheService";
/** Filter/service name of the CacheMon minifilter driver. */
constexpr const wchar_t* kDriverServiceName = L"CacheMon";
/** Custom SCM control code that switches the service to Disabled mode. */
constexpr DWORD kServiceControlDisabledMode = 128;
/** Custom SCM control code that switches the service to Monitor mode. */
constexpr DWORD kServiceControlMonitorMode = 129;
/** Custom SCM control code that switches the service to Serve mode. */
constexpr DWORD kServiceControlServeMode = 130;

/** High-level state of a Windows service or the driver. */
enum class ServiceState {
    Missing,       /**< Not installed / does not exist. */
    Stopped,       /**< Installed but not running. */
    StartPending,  /**< In the process of starting. */
    StopPending,   /**< In the process of stopping. */
    Running        /**< Running. */
};

/**
 * Queries the state of a named service via the SCM.
 *
 * @param service_name SCM name of the service.
 * @return The service's state (Missing if it does not exist).
 */
ServiceState query_service_state(const std::wstring& service_name);

/**
 * Queries the CacheMon driver state, treating a loaded minifilter as running
 * even when it is not registered as a demand-start service.
 *
 * @return The driver's state.
 */
ServiceState query_driver_state();

/**
 * Converts a service state to a stable lowercase string.
 *
 * @param state The state to convert.
 * @return The state name, e.g. L"running".
 */
std::wstring service_state_to_wstring(ServiceState state);

/** Installs, starts, stops and sends mode-change controls to the service. */
class WinServiceController {
public:
    /**
     * @param service_name SCM name of the service to control (defaults to the
     *        SSD Cache service).
     */
    explicit WinServiceController(std::wstring service_name = kServiceName);

    /**
     * Installs (or reconfigures) the service in the SCM.
     *
     * @param binary_path Full path to the service executable.
     * @return True on success.
     */
    bool install_service(const std::wstring& binary_path) const;

    /**
     * Starts the service.
     *
     * @return True on success or if it is already running.
     */
    bool start_service() const;

    /**
     * Stops the service.
     *
     * @return True on success or if it is already stopped.
     */
    bool stop_service() const;

    /**
     * Loads the CacheMon minifilter driver.
     *
     * @return True on success or if it is already loaded.
     */
    bool start_driver() const;

    /**
     * Unloads the CacheMon minifilter driver.
     *
     * @return True on success or if it is not loaded.
     */
    bool stop_driver() const;

    /**
     * Sends the custom SCM control corresponding to a mode.
     *
     * @param mode The mode to switch to.
     * @return True if the control was accepted.
     */
    bool send_mode(AppMode mode) const;

    /**
     * @return The current state of the controlled service.
     */
    ServiceState query_state() const;

private:
    std::wstring service_name_;
};

}  // namespace ssd_cache
