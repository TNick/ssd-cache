/**
 * @file
 * @brief ssd-cache-service: the SSD Cache Windows background service executable.
 *
 * Runs as a Windows service under the Service Control Manager (SCM) and also
 * exposes CLI switches for install/start/stop, driver control, health checks,
 * cache eviction, and opening config or log files. With no switch the process
 * registers with the SCM and runs the service host loop.
 */

#include <array>
#include <cstdint>
#include <string>
#include <filesystem>
#include <optional>

#include <windows.h>
#include <shellapi.h>

#include "ssd_cache/cache_eviction.h"
#include "ssd_cache/cache_index.h"
#include "ssd_cache/config.h"
#include "ssd_cache/path_mapper.h"
#include "ssd_cache/win_file_copy.h"
#include "ssd_cache/win_file_logger.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_service_controller.h"
#include "ssd_cache/win_service_host.h"
#include "ssd_cache/utf.h"

namespace {

/**
 * Returns the full path of this executable.
 *
 * @return Absolute path to the running module.
 */
std::wstring module_path() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size())
    );

    // Grow the buffer until GetModuleFileNameW no longer fills it entirely.
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(
            nullptr,
            path.data(),
            static_cast<DWORD>(path.size())
        );
    }

    path.resize(length);
    return path;
}

/**
 * Tests whether a command-line switch is present.
 *
 * @param argc Argument count from CommandLineToArgvW.
 * @param argv Argument vector from CommandLineToArgvW.
 * @param value Switch to look for (case-insensitive).
 * @return True if @p value appears among @p argv[1..].
 */
bool has_arg(int argc, wchar_t** argv, const wchar_t* value) {
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], value) == 0) {
            return true;
        }
    }

    return false;
}

/**
 * Ensures a text file exists and opens it in Notepad.
 *
 * @param path Absolute path to the file to open.
 */
void open_text_file(const std::wstring& path) {
    ssd_cache::ensure_parent_directory(path);

    // Create an empty file when it does not exist yet.
    if (!std::filesystem::exists(std::filesystem::path(path))) {
        HANDLE handle = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }

    ShellExecuteW(
        nullptr,
        L"open",
        L"notepad.exe",
        path.c_str(),
        nullptr,
        SW_SHOWNORMAL
    );
}

/**
 * Interprets a ShellExecuteW result as success or failure.
 *
 * @param result Value returned by ShellExecuteW.
 * @return True when the shell accepted the request (instance handle > 32).
 */
bool shell_execute_succeeded(HINSTANCE result) {
    return reinterpret_cast<INT_PTR>(result) > 32;
}

/**
 * Locates a WinDbg executable installed with the Windows SDK.
 *
 * @return Path to windbg.exe, or nullopt when none of the known locations exist.
 */
std::optional<std::wstring> windbg_path() {
    const std::array<std::filesystem::path, 3> candidates{
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\windbg.exe",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\arm64\\windbg.exe",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x86\\windbg.exe",
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate.wstring();
        }
    }

    return std::nullopt;
}

/**
 * Launches WinDbg to capture CacheMon driver DbgPrintEx output.
 */
void open_driver_log() {
    const auto debugger = windbg_path();
    if (!debugger.has_value()) {
        MessageBoxW(
            nullptr,
            L"CacheMon driver logs are emitted with DbgPrintEx, but WinDbg "
            L"was not found under the Windows Kits debugger directory.",
            L"SSD Cache driver log",
            MB_OK | MB_ICONWARNING
        );
        return;
    }

    // Elevate WinDbg and attach to the kernel log with DbgPrint enabled.
    const std::wstring parameters =
        L"-kl -c \"ed nt!Kd_IHVDRIVER_Mask 0xffffffff; !dbgprint\"";
    const auto result = ShellExecuteW(
        nullptr,
        L"runas",
        debugger->c_str(),
        parameters.c_str(),
        nullptr,
        SW_SHOWNORMAL
    );

    if (!shell_execute_succeeded(result)) {
        MessageBoxW(
            nullptr,
            L"Failed to start WinDbg for CacheMon DbgPrintEx output.",
            L"SSD Cache driver log",
            MB_OK | MB_ICONWARNING
        );
    }
}

/**
 * Tests whether the user asked for help text.
 *
 * @param argc Argument count from CommandLineToArgvW.
 * @param argv Argument vector from CommandLineToArgvW.
 * @return True when --help, -h, or /? is present.
 */
bool wants_help(int argc, wchar_t** argv) {
    return has_arg(argc, argv, L"--help") ||
           has_arg(argc, argv, L"-h") ||
           has_arg(argc, argv, L"/?");
}

/**
 * Writes text to a console or pipe handle.
 *
 * This is a WIN32 (GUI) subsystem executable, so it owns no console. When run
 * from a terminal we attach to the parent console and write there; when run
 * from Explorer (no parent console) we fall back to a message box.
 *
 * @param handle Output handle (console or pipe).
 * @param text Wide-character text to write.
 * @return True when the write succeeded.
 */
bool write_to_handle(HANDLE handle, const std::wstring& text) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Prefer WriteConsoleW when the target is an interactive console.
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        return WriteConsoleW(
            handle,
            text.c_str(),
            static_cast<DWORD>(text.size()),
            &written,
            nullptr
        ) != FALSE;
    }

    // Fall back to raw UTF-8 bytes for pipes and redirected output.
    const auto utf8 = ssd_cache::wide_to_utf8(text);
    DWORD written = 0;
    return WriteFile(
        handle,
        utf8.data(),
        static_cast<DWORD>(utf8.size()),
        &written,
        nullptr
    ) != FALSE;
}

/**
 * Shows text on stdout or in a message box when no console is available.
 *
 * @param text Body text to display.
 * @param title Caption for the fallback message box.
 */
void show_text(const std::wstring& text, const wchar_t* title) {
    if (write_to_handle(GetStdHandle(STD_OUTPUT_HANDLE), text)) {
        return;
    }

    // Attach to the parent console when launched from a terminal.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE console = CreateFileW(
            L"CONOUT$",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (write_to_handle(console, text)) {
            CloseHandle(console);
            FreeConsole();
            return;
        }

        if (console != INVALID_HANDLE_VALUE) {
            CloseHandle(console);
        }
        FreeConsole();
    }

    MessageBoxW(nullptr, text.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

/** Prints usage text for all supported CLI switches. */
void show_help() {
    show_text(
        L"ssd-cache-service - SSD Cache background service\r\n"
        L"\r\n"
        L"Usage:\r\n"
        L"  ssd-cache-service [option]\r\n"
        L"\r\n"
        L"Options:\r\n"
        L"  --install     Register the service with the Windows Service Control\r\n"
        L"                Manager (SCM).\r\n"
        L"  --start       Start the installed service.\r\n"
        L"  --stop        Stop the running service.\r\n"
        L"  --status      Print the current Windows service state.\r\n"
        L"  --driver-start Load the CacheMon minifilter driver.\r\n"
        L"  --driver-stop  Unload the CacheMon minifilter driver.\r\n"
        L"  --driver-status Print the current driver state.\r\n"
        L"  --health      Print JSON health for the service and driver.\r\n"
        L"  --free <size> Evict least-recently-used cached files to free at\r\n"
        L"                least <size> (bytes, or with a KB/MB/GB suffix, e.g.\r\n"
        L"                500MB). Prompts for confirmation when the size exceeds\r\n"
        L"                a third of the cache disk; pass --yes to skip it.\r\n"
        L"  --open-config Open the machine config file in Notepad.\r\n"
        L"  --open-app-log Open the tray app log in Notepad.\r\n"
        L"  --open-service-log Open the service log in Notepad.\r\n"
        L"  --open-driver-log Open the driver DbgPrintEx output in WinDbg.\r\n"
        L"  --console     Run the service logic in the foreground of the current\r\n"
        L"                console instead of under the SCM (useful for debugging).\r\n"
        L"  --help, -h    Show this help text and exit.\r\n"
        L"\r\n"
        L"With no option the executable runs as a Windows service and is normally\r\n"
        L"launched by the Service Control Manager rather than directly.\r\n",
        L"ssd-cache-service"
    );
}

/**
 * Builds a JSON health snapshot for the service and driver.
 *
 * @param service_state Current SCM-reported service state.
 * @param driver_state Current CacheMon driver state.
 * @return Pretty-printed JSON document as a wide string.
 */
std::wstring build_health_json(
    ssd_cache::ServiceState service_state,
    ssd_cache::ServiceState driver_state
) {
    const auto service_text = ssd_cache::service_state_to_wstring(service_state);
    const auto driver_text = ssd_cache::service_state_to_wstring(driver_state);
    const auto healthy =
        service_state == ssd_cache::ServiceState::Running &&
        driver_state == ssd_cache::ServiceState::Running;
    const auto service_healthy =
        service_state == ssd_cache::ServiceState::Running;
    const auto driver_healthy =
        driver_state == ssd_cache::ServiceState::Running;

    std::wstring json = L"{\r\n";
    json += L"  \"service\": {\r\n";
    json += L"    \"name\": \"SsdCacheService\",\r\n";
    json += L"    \"state\": \"" + service_text + L"\",\r\n";
    json += L"    \"healthy\": ";
    json += service_healthy ? L"true" : L"false";
    json += L"\r\n  },\r\n";
    json += L"  \"driver\": {\r\n";
    json += L"    \"name\": \"CacheMon\",\r\n";
    json += L"    \"state\": \"" + driver_text + L"\",\r\n";
    json += L"    \"healthy\": ";
    json += driver_healthy ? L"true" : L"false";
    json += L"\r\n  },\r\n";
    json += L"  \"overall_healthy\": ";
    json += healthy ? L"true" : L"false";
    json += L"\r\n}\r\n";
    return json;
}

/**
 * Returns the argument following a value-bearing switch.
 *
 * Used for switches like `--free 500MB`.
 *
 * @param argc Argument count from CommandLineToArgvW.
 * @param argv Argument vector from CommandLineToArgvW.
 * @param name Switch whose following token should be returned.
 * @return Pointer to the next argument, or nullptr when @p name is absent or
 *         has no following token.
 */
const wchar_t* arg_value(int argc, wchar_t** argv, const wchar_t* name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (_wcsicmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }

    return nullptr;
}

/**
 * Resolves cache-side absolute paths for eviction.
 *
 * The free command never needs source paths, so source_absolute is
 * intentionally empty.
 */
class CacheOnlyResolver final : public ssd_cache::ICopyPathResolver {
public:
    /**
     * Stores the cache root used to resolve relative cache paths.
     *
     * @param cache_root Absolute path to the cache directory.
     */
    explicit CacheOnlyResolver(std::wstring cache_root)
        : cache_root_(std::move(cache_root)) {}

    /**
     * Returns an empty source path; eviction only touches cache files.
     *
     * @param relative_path Ignored.
     * @return An empty wide string.
     */
    std::wstring source_absolute(const std::wstring&) const override {
        return {};
    }

    /**
     * Maps a relative cache path to an absolute path under the cache root.
     *
     * @param relative_path Path relative to the cache root.
     * @return Absolute cache-side path.
     */
    std::wstring cache_absolute(
        const std::wstring& relative_path
    ) const override {
        return ssd_cache::join_root_relative(cache_root_, relative_path);
    }

private:
    std::wstring cache_root_;
};

/**
 * Loads configuration from disk or falls back to defaults.
 *
 * @param config_path Absolute path to the machine config file.
 * @return Merged configuration with a default SQLite path when unset.
 */
ssd_cache::AppConfig load_config_or_defaults(const std::wstring& config_path) {
    ssd_cache::AppConfig config;
    if (std::filesystem::exists(std::filesystem::path(config_path))) {
        config = ssd_cache::load_config_file(config_path);
    }
    if (config.sqlite_path.empty()) {
        config.sqlite_path = ssd_cache::default_sqlite_path();
    }
    return config;
}

/**
 * Queries total and free space for the volume hosting the cache root.
 *
 * @param cache_root Absolute cache directory path.
 * @param total_bytes Receives total volume size in bytes on success.
 * @param free_bytes Receives available free space in bytes on success.
 * @return True when GetDiskFreeSpaceExW succeeded.
 */
bool cache_volume_sizes(
    const std::wstring& cache_root,
    std::uint64_t& total_bytes,
    std::uint64_t& free_bytes
) {
    ULARGE_INTEGER free_available{};
    ULARGE_INTEGER total{};
    if (!GetDiskFreeSpaceExW(cache_root.c_str(), &free_available, &total, nullptr)) {
        return false;
    }

    total_bytes = static_cast<std::uint64_t>(total.QuadPart);
    free_bytes = static_cast<std::uint64_t>(free_available.QuadPart);
    return true;
}

/**
 * Prompts for confirmation before a destructive operation.
 *
 * When launched from a terminal we read a line from the parent console; when
 * launched from Explorer (no console) we fall back to a Yes/No message box.
 *
 * @param message Question text shown to the user.
 * @return True only on an explicit "y" or "yes" (or Yes in the message box).
 */
bool confirm(const std::wstring& message) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        bool answered_yes = false;
        HANDLE out = CreateFileW(
            L"CONOUT$",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        HANDLE in = CreateFileW(
            L"CONIN$",
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        // Read a single non-whitespace character from the parent console.
        if (out != INVALID_HANDLE_VALUE && in != INVALID_HANDLE_VALUE) {
            const std::wstring prompt = message + L" [y/N] ";
            DWORD written = 0;
            WriteConsoleW(
                out,
                prompt.c_str(),
                static_cast<DWORD>(prompt.size()),
                &written,
                nullptr
            );

            std::array<wchar_t, 16> buffer{};
            DWORD read = 0;
            if (ReadConsoleW(
                    in,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size() - 1),
                    &read,
                    nullptr
                )) {
                for (DWORD index = 0; index < read; ++index) {
                    const wchar_t ch = buffer[index];
                    if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
                        continue;
                    }

                    answered_yes = ch == L'y' || ch == L'Y';
                    break;
                }
            }
        }

        if (out != INVALID_HANDLE_VALUE) {
            CloseHandle(out);
        }
        if (in != INVALID_HANDLE_VALUE) {
            CloseHandle(in);
        }
        FreeConsole();
        return answered_yes;
    }

    return MessageBoxW(
        nullptr,
        message.c_str(),
        L"ssd-cache-service",
        MB_YESNO | MB_ICONWARNING
    ) == IDYES;
}

/**
 * Evicts least-recently-used cache entries until the requested space is freed.
 *
 * @param size_arg Size token from the command line (e.g. "500MB").
 * @param assume_yes When true, skip the large-eviction confirmation prompt.
 * @param logger Service log sink for eviction progress.
 * @param config_path Absolute path to the machine config file.
 * @return Process exit code: 0 on success, 1 when aborted, 2 on usage errors.
 */
int run_free_command(
    const wchar_t* size_arg,
    bool assume_yes,
    ssd_cache::WinFileLogger& logger,
    const std::wstring& config_path
) {
    if (size_arg == nullptr) {
        show_text(
            L"Missing size. Usage: ssd-cache-service --free <size> "
            L"(e.g. 500MB, 2GB, 1048576).\r\n",
            L"ssd-cache-service"
        );
        return 2;
    }

    const auto requested = ssd_cache::parse_size_bytes(size_arg);
    if (!requested || *requested == 0) {
        show_text(
            L"Invalid size '" + std::wstring(size_arg) +
                L"'. Use a positive number with an optional KB/MB/GB suffix, "
                L"e.g. 500MB.\r\n",
            L"ssd-cache-service"
        );
        return 2;
    }

    auto config = load_config_or_defaults(config_path);
    const auto cache_root = ssd_cache::cache_root_from_config(config);

    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
    const bool have_sizes =
        cache_volume_sizes(cache_root, total_bytes, free_bytes);

    // Confirm when the request would free more than a third of the cache disk.
    if (have_sizes && *requested > total_bytes / 3 && !assume_yes) {
        const std::wstring message =
            L"Freeing " + ssd_cache::format_size_bytes(*requested) +
            L" is more than a third of the cache disk (" +
            ssd_cache::format_size_bytes(total_bytes) +
            L"). Continue?";
        if (!confirm(message)) {
            show_text(L"Aborted; no files were removed.\r\n", L"ssd-cache-service");
            return 1;
        }
    }

    logger.log(
        "CLI free requested: " + std::to_string(*requested) + " bytes"
    );

    // Open the index and run LRU eviction against cache-side paths only.
    ssd_cache::CacheIndex index(config.sqlite_path);
    index.open();
    ssd_cache::WinFileCopyEngine copy_engine;
    CacheOnlyResolver resolver(cache_root);

    const auto result = ssd_cache::evict_least_recently_used(
        index,
        copy_engine,
        resolver,
        *requested,
        &logger
    );

    logger.log(
        "CLI free removed " + std::to_string(result.files_removed) +
        " file(s), " + std::to_string(result.bytes_freed) + " bytes"
    );

    std::wstring report =
        L"Freed " + ssd_cache::format_size_bytes(result.bytes_freed) +
        L" by removing " + std::to_wstring(result.files_removed) +
        L" file(s) from the cache.\r\n";
    if (result.bytes_freed < *requested) {
        report +=
            L"Note: only " + ssd_cache::format_size_bytes(result.bytes_freed) +
            L" could be freed; the cache held less than requested.\r\n";
    }
    show_text(report, L"ssd-cache-service");
    return 0;
}

}  // namespace

/**
 * Entry point. Parses CLI switches for service management and cache operations,
 * or runs the service host when no switch is given.
 *
 * @param instance Module instance handle for the running process.
 * @param prev_instance Legacy previous-instance handle; always null and unused.
 * @param command_line Unused; arguments are re-parsed via GetCommandLineW.
 * @param show_command Unused; this process has no top-level visible window.
 * @return Exit code for the selected CLI command, or the service host result.
 */
int WINAPI wWinMain(
    [[maybe_unused]] HINSTANCE instance,
    [[maybe_unused]] HINSTANCE prev_instance,
    [[maybe_unused]] PWSTR command_line,
    [[maybe_unused]] int show_command
) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Help is handled before any service objects are constructed.
    if (argv != nullptr && wants_help(argc, argv)) {
        show_help();
        LocalFree(argv);
        return 0;
    }

    // Shared state for CLI commands and the default service-host path.
    const auto config_path = ssd_cache::default_config_path();
    ssd_cache::WinFileLogger logger(ssd_cache::default_service_log_path());
    ssd_cache::WinServiceHost host(ssd_cache::kServiceName, config_path);
    ssd_cache::WinServiceController controller;

    // SCM and driver management switches.
    if (argv != nullptr && has_arg(argc, argv, L"--install")) {
        logger.log("Service install requested.");
        const bool installed = controller.install_service(module_path());
        LocalFree(argv);
        return installed ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--start")) {
        logger.log("Service start requested.");
        const bool started = controller.start_service();
        LocalFree(argv);
        return started ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--stop")) {
        logger.log("Service stop requested.");
        const bool stopped = controller.stop_service();
        LocalFree(argv);
        return stopped ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--status")) {
        const auto state = controller.query_state();
        show_text(
            L"SsdCacheService: " + ssd_cache::service_state_to_wstring(state) +
                L"\r\n",
            L"ssd-cache-service"
        );
        LocalFree(argv);
        return state == ssd_cache::ServiceState::Running ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--driver-start")) {
        logger.log("Driver start requested.");
        const bool started = controller.start_driver();
        LocalFree(argv);
        return started ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--driver-stop")) {
        logger.log("Driver stop requested.");
        const bool stopped = controller.stop_driver();
        LocalFree(argv);
        return stopped ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--driver-status")) {
        const auto state = ssd_cache::query_driver_state();
        show_text(
            L"CacheMon: " + ssd_cache::service_state_to_wstring(state) +
                L"\r\n",
            L"ssd-cache-service"
        );
        LocalFree(argv);
        return state == ssd_cache::ServiceState::Running ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--health")) {
        const auto service_state = controller.query_state();
        const auto driver_state = ssd_cache::query_driver_state();
        show_text(
            build_health_json(service_state, driver_state),
            L"ssd-cache-service"
        );
        LocalFree(argv);
        return service_state == ssd_cache::ServiceState::Running &&
            driver_state == ssd_cache::ServiceState::Running ? 0 : 1;
    }

    // Open config or log files in the user's default editor.
    if (argv != nullptr && has_arg(argc, argv, L"--open-config")) {
        open_text_file(ssd_cache::default_config_path());
        LocalFree(argv);
        return 0;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--open-app-log")) {
        open_text_file(ssd_cache::default_app_log_path());
        LocalFree(argv);
        return 0;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--open-service-log")) {
        open_text_file(ssd_cache::default_service_log_path());
        LocalFree(argv);
        return 0;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--open-driver-log")) {
        open_driver_log();
        LocalFree(argv);
        return 0;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--free")) {
        const wchar_t* size_arg = arg_value(argc, argv, L"--free");
        const bool assume_yes =
            has_arg(argc, argv, L"--yes") || has_arg(argc, argv, L"-y");
        const int rc =
            run_free_command(size_arg, assume_yes, logger, config_path);
        LocalFree(argv);
        return rc;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--console")) {
        logger.log("Service console mode requested.");
        LocalFree(argv);
        return host.run_console();
    }

    if (argv != nullptr) {
        LocalFree(argv);
    }

    // No CLI switch: register with the SCM and run the service loop.
    return host.run();
}
