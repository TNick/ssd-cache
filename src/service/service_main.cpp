#include <array>
#include <string>
#include <filesystem>
#include <optional>

#include <windows.h>
#include <shellapi.h>

#include "ssd_cache/win_file_logger.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_service_controller.h"
#include "ssd_cache/win_service_host.h"
#include "ssd_cache/utf.h"

namespace {

std::wstring module_path() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size())
    );

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

bool has_arg(int argc, wchar_t** argv, const wchar_t* value) {
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], value) == 0) {
            return true;
        }
    }

    return false;
}

void open_text_file(const std::wstring& path) {
    ssd_cache::ensure_parent_directory(path);

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

bool shell_execute_succeeded(HINSTANCE result) {
    return reinterpret_cast<INT_PTR>(result) > 32;
}

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

bool wants_help(int argc, wchar_t** argv) {
    return has_arg(argc, argv, L"--help") ||
           has_arg(argc, argv, L"-h") ||
           has_arg(argc, argv, L"/?");
}

// This is a WIN32 (GUI) subsystem executable, so it owns no console. When run
// from a terminal we attach to the parent console and write there; when run
// from Explorer (no parent console) we fall back to a message box.
bool write_to_handle(HANDLE handle, const std::wstring& text) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

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

void show_text(const std::wstring& text, const wchar_t* title) {
    if (write_to_handle(GetStdHandle(STD_OUTPUT_HANDLE), text)) {
        return;
    }

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

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv != nullptr && wants_help(argc, argv)) {
        show_help();
        LocalFree(argv);
        return 0;
    }

    const auto config_path = ssd_cache::default_config_path();
    ssd_cache::WinFileLogger logger(ssd_cache::default_service_log_path());
    ssd_cache::WinServiceHost host(ssd_cache::kServiceName, config_path);
    ssd_cache::WinServiceController controller;

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

    if (argv != nullptr && has_arg(argc, argv, L"--console")) {
        logger.log("Service console mode requested.");
        LocalFree(argv);
        return host.run_console();
    }

    if (argv != nullptr) {
        LocalFree(argv);
    }

    return host.run();
}
