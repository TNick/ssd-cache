/**
 * @file
 * @brief Implementation of the service host: SCM plumbing and worker pipeline.
 */

#include "ssd_cache/win_service_host.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ssd_cache/access_event.h"
#include "ssd_cache/cache_index.h"
#include "ssd_cache/config.h"
#include "ssd_cache/job_scheduler.h"
#include "ssd_cache/path_mapper.h"
#include "ssd_cache/utf.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_file_copy.h"
#include "ssd_cache/win_filter_activity_source.h"
#include "ssd_cache/win_mount_manager.h"
#include "ssd_cache/win_service_controller.h"

namespace ssd_cache {
namespace {

/** The single live host, used by the SCM thunks to reach the instance. */
WinServiceHost* g_service = nullptr;

/** RAII wrapper closing a Win32 HANDLE (tolerates null/invalid) on destruction. */
class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE handle) : handle_(handle) {}

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    ~OwnedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

/** Resolves relative paths against the configured source UNC and cache root. */
class RootedPathResolver final : public ICopyPathResolver {
public:
    explicit RootedPathResolver(AppConfig config) : config_(std::move(config)) {}

    /**
     * @param relative_path Path relative to the source root.
     * @return The absolute source path.
     */
    std::wstring source_absolute(
        const std::wstring& relative_path
    ) const override {
        return join_root_relative(config_.source_unc, relative_path);
    }

    /**
     * @param relative_path Path relative to the cache root.
     * @return The absolute cache path.
     */
    std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const override {
        return join_root_relative(cache_root_from_config(config_), relative_path);
    }

private:
    AppConfig config_;
};

/**
 * SCM ServiceMain trampoline that forwards to the live host instance.
 *
 * @param argc Service argument count.
 * @param argv Service argument vector.
 */
void WINAPI service_main_thunk(DWORD argc, wchar_t** argv) {
    if (g_service != nullptr) {
        g_service->service_main(argc, argv);
    }
}

/**
 * SCM control-handler trampoline that forwards to the live host instance.
 *
 * @param control Control code.
 * @param event_type Control-specific event type.
 * @param event_data Control-specific event data.
 * @param context Registered handler context.
 * @return The host's handler result, or ERROR_CALL_NOT_IMPLEMENTED if no host.
 */
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

/**
 * Decides whether a mode should have caching active.
 *
 * @param mode The mode to test (nullopt means unset).
 * @return True for Monitor mode or an unset mode (which preserves the historical
 *         default of caching whenever a source is configured); false for
 *         Disabled and Serve.
 */
bool mode_enables_caching(const std::optional<AppMode>& mode) {
    return !mode.has_value() || *mode == AppMode::Monitor;
}

/**
 * Loads the config, creating it with defaults (and a default SQLite path) when
 * the file does not yet exist.
 *
 * @param path Path to the config file.
 * @return The loaded or newly created configuration.
 */
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

/**
 * Moves the cache volume off the presentation letter back to the cache letter
 * (the machine-global half of a switch into Monitor mode).
 *
 * @param config Source/cache drive letters.
 * @return True on success or when no move is needed.
 */
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

/**
 * Moves the cache volume onto the presentation letter (the machine-global half
 * of a switch into Serve mode).
 *
 * @param config Source/cache drive letters.
 * @return True on success, false if there is no cache volume to move.
 */
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

/**
 * Prefixes a path with the extended-length (\\?\) form for the Win32 file APIs.
 *
 * @param path A normal Win32 path.
 * @return The \\?\-prefixed equivalent (already-prefixed paths pass through).
 */
std::wstring extended_path(std::wstring_view path) {
    std::wstring value(path);
    if (value.starts_with(L"\\\\?\\")) {
        return value;
    }

    if (value.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + value.substr(2);
    }

    return L"\\\\?\\" + value;
}

/**
 * Tests whether a relative path names a directory on the source.
 *
 * @param config Configuration providing the source UNC.
 * @param relative_path Path relative to the source root.
 * @return True if the path exists on the source and is a directory.
 */
bool source_path_is_directory(
    const AppConfig& config,
    const std::wstring& relative_path
) {
    if (relative_path.empty() || config.source_unc.empty()) {
        return false;
    }

    const auto source_path = join_root_relative(
        config.source_unc,
        relative_path
    );
    const DWORD attributes = GetFileAttributesW(
        extended_path(source_path).c_str()
    );
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * Purges stale/invalid records at startup: the empty-path sentinel, paths that
 * now match ignore patterns, and paths that are actually directories.
 *
 * @param cache_index Index to clean.
 * @param config Configuration providing filters and the source UNC.
 * @param logger Optional log sink; may be null.
 */
void cleanup_invalid_cache_records(
    CacheIndex& cache_index,
    const AppConfig& config,
    ILogger* logger
) {
    if (cache_index.pending_job(L"").has_value() ||
        cache_index.file_record(L"").has_value()) {
        cache_index.delete_record(L"");
        if (logger != nullptr) {
            logger->log("cleanup removed invalid empty relative path");
        }
    }

    auto relative_paths = cache_index.load_record_paths();
    for (const auto& job : cache_index.load_pending_jobs()) {
        if (std::find(
            relative_paths.begin(),
            relative_paths.end(),
            job.relative_path
        ) == relative_paths.end()) {
            relative_paths.push_back(job.relative_path);
        }
    }

    for (const auto& relative_path : relative_paths) {
        if (path_matches_ignored_patterns(config, relative_path)) {
            cache_index.delete_record(relative_path);
            if (logger != nullptr) {
                logger->log(
                    "cleanup removed ignored path cache record: '" +
                    wide_to_utf8(relative_path) + "'"
                );
            }
            continue;
        }

        if (source_path_is_directory(config, relative_path)) {
            cache_index.delete_record(relative_path);
            if (logger != nullptr) {
                logger->log(
                    "cleanup removed directory cache record: '" +
                    wide_to_utf8(relative_path) + "'"
                );
            }
        }
    }
}

/**
 * Resolves a process id to its image path.
 *
 * @param pid Process id (0 or out-of-range yields an empty result).
 * @return The process image path, or an empty string if it cannot be obtained.
 */
std::string requestor_process_path(std::uint64_t pid) {
    if (pid == 0 || pid > std::numeric_limits<DWORD>::max()) {
        return {};
    }

    OwnedHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(pid)
    ));
    if (process.get() == nullptr) {
        return {};
    }

    std::vector<wchar_t> image_path(32768);
    DWORD image_path_size = static_cast<DWORD>(image_path.size());
    const BOOL ok = QueryFullProcessImageNameW(
        process.get(),
        0,
        image_path.data(),
        &image_path_size
    );
    if (!ok || image_path_size == 0) {
        return {};
    }

    return wide_to_utf8(std::wstring(image_path.data(), image_path_size));
}

/**
 * Returns a copy of an event with its requestor_process image path filled in.
 *
 * @param event The event to enrich.
 * @return The enriched copy.
 */
AccessEvent with_requestor_process(const AccessEvent& event) {
    auto enriched = event;
    enriched.requestor_process = requestor_process_path(event.requestor_pid);
    return enriched;
}

/**
 * Formats the requestor (pid and, if known, process image) for a log line.
 *
 * @param event The event whose requestor is described.
 * @return A label such as "pid=1234 process='C:\\...\\app.exe'".
 */
std::string requestor_process_label(const AccessEvent& event) {
    std::string label = "pid=" + std::to_string(event.requestor_pid);
    if (!event.requestor_process.empty()) {
        label += " process='" + event.requestor_process + "'";
    }

    return label;
}

/**
 * Formats a one-line description of an activity event for logging.
 *
 * @param event The event to describe.
 * @return The formatted log message.
 */
std::string activity_event_message(const AccessEvent& event) {
    return "driver event " + access_kind_to_string(event.kind) + ": '" +
        wide_to_utf8(event.relative_path) + "' from " +
        requestor_process_label(event);
}

/**
 * Decides whether an activity event should be dropped rather than scheduled,
 * logging the reason when it is.
 *
 * @param config Configuration providing the filters and source UNC.
 * @param event The enriched activity event.
 * @param logger Optional log sink; may be null.
 * @return True if the event should be ignored (empty path, filtered path or
 *         process, or a directory).
 */
bool should_ignore_activity_event(
    const AppConfig& config,
    const AccessEvent& event,
    ILogger* logger
) {
    if (event.relative_path.empty()) {
        if (logger != nullptr) {
            logger->log(
                activity_event_message(event) +
                " ignored: empty relative path"
            );
        }
        return true;
    }

    if (path_matches_ignored_patterns(config, event.relative_path)) {
        if (logger != nullptr) {
            logger->log(
                activity_event_message(event) +
                " ignored: path filter"
            );
        }
        return true;
    }

    if (process_matches_ignored_patterns(
        config,
        utf8_to_wide(event.requestor_process)
    )) {
        if (logger != nullptr) {
            logger->log(
                activity_event_message(event) +
                " ignored: process filter"
            );
        }
        return true;
    }

    if (source_path_is_directory(config, event.relative_path)) {
        if (logger != nullptr) {
            logger->log(activity_event_message(event) + " ignored: directory");
        }
        return true;
    }

    return false;
}

}  // namespace

WinServiceHost::WinServiceHost(
    std::wstring service_name,
    std::wstring config_path
)
    : service_name_(std::move(service_name)),
      config_path_(std::move(config_path)),
      logger_(std::make_unique<WinFileLogger>(default_service_log_path())) {}

int WinServiceHost::run() {
    g_service = this;
    logger_->log("Service process entered SCM mode.");

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

    logger_->log("Service process entered console mode.");

    try {
        run_worker();
        logger_->log("Service console worker stopped cleanly.");
    } catch (const std::exception& ex) {
        logger_->log(std::string("Service console worker failed: ") + ex.what());
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
        return 1;
    } catch (...) {
        logger_->log("Service console worker failed with unknown exception.");
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
    logger_->log("Service received start from SCM.");
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
        logger_->log("Service failed to create stop event.");
        set_status(SERVICE_STOPPED, GetLastError());
        return;
    }

    try {
        set_status(SERVICE_RUNNING);
        logger_->log("Service entered running state.");
        run_worker();
        logger_->log("Service worker stopped cleanly.");
        set_status(SERVICE_STOPPED);
    } catch (const std::exception& ex) {
        logger_->log(std::string("Service worker failed: ") + ex.what());
        set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    } catch (...) {
        logger_->log("Service worker failed with unknown exception.");
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
        logger_->log("Service received stop or shutdown control.");
        set_status(SERVICE_STOP_PENDING);
        SetEvent(stop_event_);
        return NO_ERROR;
    }

    if (control == SERVICE_CONTROL_PARAMCHANGE) {
        logger_->log("Service received parameter change control.");
        return NO_ERROR;
    }

    // The remaining custom controls switch modes: each performs any drive-letter
    // move, persists the new mode, and applies it to the live pipeline.
    auto config = load_or_create_config(config_path_);
    if (control == kServiceControlDisabledMode) {
        logger_->log("Service switched to disabled mode.");
        config.mode = AppMode::Disabled;
        save_config_file(config_path_, config);
        apply_caching_mode(config.mode);
        return NO_ERROR;
    }

    if (control == kServiceControlMonitorMode) {
        logger_->log("Service switched to monitor mode.");
        if (!move_cache_to_monitor_letter(config)) {
            return ERROR_SERVICE_SPECIFIC_ERROR;
        }

        config.mode = AppMode::Monitor;
        save_config_file(config_path_, config);
        apply_caching_mode(config.mode);
        return NO_ERROR;
    }

    if (control == kServiceControlServeMode) {
        logger_->log("Service switched to serve mode.");
        if (!move_cache_to_source_letter(config)) {
            return ERROR_SERVICE_SPECIFIC_ERROR;
        }

        config.mode = AppMode::Serve;
        save_config_file(config_path_, config);
        apply_caching_mode(config.mode);
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WinServiceHost::run_worker() {
    // Load configuration and open the cache index, pruning stale records.
    const auto config = load_or_create_config(config_path_);
    ensure_parent_directory(config.sqlite_path);
    logger_->log("Service loaded configuration.");

    CacheIndex cache_index(config.sqlite_path);
    cache_index.open();
    logger_->log("Service opened cache index.");
    cleanup_invalid_cache_records(cache_index, config, logger_.get());

    // Build the copy scheduler from the copy engine, path resolver, free-space
    // provider and the settings derived from config.
    WinFileCopyEngine copy_engine;
    WinFreeSpaceProvider free_space;
    RootedPathResolver path_resolver(config);
    SchedulerSettings settings;
    settings.copy_delay = config.copy_delay;
    settings.compare_hash_before_overwrite =
        config.compare_hash_before_overwrite;
    settings.min_free_bytes = config.min_free_bytes;
    settings.cache_root = cache_root_from_config(config);
    PendingCopyScheduler scheduler(
        cache_index,
        copy_engine,
        path_resolver,
        settings,
        logger_.get(),
        &free_space
    );
    scheduler.start();

    // Publish the scheduler for the control thread and apply the startup mode
    // (pausing it immediately unless we start in Monitor mode).

    {
        std::lock_guard lock(scheduler_mutex_);
        scheduler_ = &scheduler;
    }
    apply_caching_mode(config.mode);

    std::unique_ptr<WinFilterActivitySource> activity_source;
    if (!config.source_unc.empty()) {
        auto* logger = logger_.get();
        activity_source = std::make_unique<WinFilterActivitySource>(
            config.source_unc,
            [this, config, logger, &scheduler](const AccessEvent& event) {
                if (!caching_active_.load()) {
                    // Disabled/Serve mode: don't cache accessed files.
                    return;
                }

                const auto enriched_event = with_requestor_process(event);
                if (should_ignore_activity_event(
                    config,
                    enriched_event,
                    logger
                )) {
                    return;
                }

                if (logger != nullptr) {
                    logger->log(activity_event_message(enriched_event));
                }
                scheduler.record_event(enriched_event);
            }
        );
        activity_source->start();
        logger_->log("Service connected to driver activity source.");
    }

    WaitForSingleObject(stop_event_, INFINITE);

    if (activity_source) {
        activity_source->stop();
        logger_->log("Service disconnected from driver activity source.");
    }

    {
        std::lock_guard lock(scheduler_mutex_);
        scheduler_ = nullptr;
    }
    scheduler.stop();
    logger_->log("Service scheduler stopped.");
}

void WinServiceHost::apply_caching_mode(const std::optional<AppMode>& mode) {
    const bool active = mode_enables_caching(mode);
    caching_active_.store(active);

    std::lock_guard lock(scheduler_mutex_);
    if (scheduler_ != nullptr) {
        scheduler_->set_paused(!active);
    }
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
