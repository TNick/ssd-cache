#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <gdiplus.h>

#include "ssd_cache/config.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_file_logger.h"
#include "ssd_cache/win_mount_manager.h"
#include "ssd_cache/win_service_controller.h"

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT_PTR kStatusTimerId = 2;
constexpr UINT kStatusTimerIntervalMs = 2000;
constexpr WORD kAppIconId = 32512;
constexpr WORD kErrorIconId = 32513;
constexpr WORD kWarningIconId = 32515;
constexpr int kCmdStartService = 1001;
constexpr int kCmdStopService = 1002;
constexpr int kCmdDisabledMode = 1003;
constexpr int kCmdMonitorMode = 1004;
constexpr int kCmdServeMode = 1005;
constexpr int kCmdOpenConfig = 1006;
constexpr int kCmdOpenAppLog = 1007;
constexpr int kCmdOpenServiceLog = 1008;
constexpr int kCmdOpenDriverLog = 1009;
constexpr int kCmdExit = 1010;

constexpr wchar_t kWindowClassName[] = L"SsdCacheTrayWindow";

struct TrayIcons {
    HICON running = nullptr;
    HICON warning = nullptr;
    HICON missing = nullptr;
};

class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        const auto status = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
        if (status != Gdiplus::Ok) {
            token_ = 0;
        }
    }

    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

    ~GdiplusSession() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

private:
    ULONG_PTR token_ = 0;
};

UINT g_taskbar_created = 0;
std::wstring g_config_path;
std::wstring g_app_log_path;
std::wstring g_service_log_path;
ssd_cache::WinServiceController g_controller;
TrayIcons g_tray_icons;
std::unique_ptr<ssd_cache::WinFileLogger> g_logger;

bool wants_help(int argc, wchar_t** argv) {
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], L"--help") == 0 ||
            _wcsicmp(argv[index], L"-h") == 0 ||
            _wcsicmp(argv[index], L"/?") == 0) {
            return true;
        }
    }

    return false;
}

// This is a WIN32 (GUI) subsystem executable, so it owns no console. When run
// from a terminal we attach to the parent console and write there; when run
// from Explorer (no parent console) we fall back to a message box.
void show_text(const std::wstring& text, const wchar_t* title) {
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

        if (console != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteConsoleW(
                console,
                text.c_str(),
                static_cast<DWORD>(text.size()),
                &written,
                nullptr
            );
            CloseHandle(console);
        }

        FreeConsole();
        return;
    }

    MessageBoxW(nullptr, text.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

void show_help() {
    show_text(
        L"ssd-cache-tray - SSD Cache system tray application\r\n"
        L"\r\n"
        L"Usage:\r\n"
        L"  ssd-cache-tray [command]\r\n"
        L"\r\n"
        L"With no command the application starts and adds an icon to the Windows\r\n"
        L"notification area (system tray). Every entry in the tray's right-click\r\n"
        L"menu is also available as a command that performs the action and exits.\r\n"
        L"\r\n"
        L"Commands:\r\n"
        L"  --start-service     Start the SSD Cache Windows service.\r\n"
        L"  --stop-service      Stop the SSD Cache Windows service.\r\n"
        L"  --disabled-mode     Switch the service to Disabled mode.\r\n"
        L"  --monitor-mode      Switch to Monitor mode (maps the source network\r\n"
        L"                      drive).\r\n"
        L"  --serve-mode        Switch to Serve mode (unmaps the source network\r\n"
        L"                      drive).\r\n"
        L"  --open-config       Open the configuration file.\r\n"
        L"  --open-app-log      Open the tray application log.\r\n"
        L"  --open-service-log  Open the service log.\r\n"
        L"  --open-driver-log   Open the driver DbgPrintEx output in WinDbg.\r\n"
        L"  --exit              Close a running tray instance.\r\n"
        L"  --help, -h          Show this help text and exit.\r\n",
        L"ssd-cache-tray"
    );
}

std::wstring module_directory() {
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
    return std::filesystem::path(path).parent_path().wstring();
}

std::wstring tray_icon_path(const wchar_t* file_name) {
    return (
        std::filesystem::path(module_directory()) / file_name
    ).wstring();
}

void destroy_icon(HICON icon) {
    if (icon != nullptr) {
        DestroyIcon(icon);
    }
}

void destroy_tray_icons() {
    destroy_icon(g_tray_icons.running);
    destroy_icon(g_tray_icons.warning);
    destroy_icon(g_tray_icons.missing);
    g_tray_icons = {};
}

void ensure_file_exists(const std::wstring& path) {
    ssd_cache::ensure_parent_directory(path);
    std::ofstream output(std::filesystem::path(path), std::ios::app);
}

HICON load_png_icon(const std::wstring& path) {
    Gdiplus::Bitmap bitmap(path.c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }

    HICON icon = nullptr;
    const auto status = bitmap.GetHICON(&icon);
    return status == Gdiplus::Ok ? icon : nullptr;
}

void load_tray_icons() {
    destroy_tray_icons();
    g_tray_icons.running = load_png_icon(tray_icon_path(L"driver_running.png"));
    g_tray_icons.warning = load_png_icon(tray_icon_path(L"driver_warning.png"));
    g_tray_icons.missing = load_png_icon(tray_icon_path(L"driver_missing.png"));
}

void open_text_file(HWND window, const std::wstring& path) {
    ensure_file_exists(path);
    ShellExecuteW(
        window,
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

void open_driver_log(HWND window) {
    const auto debugger = windbg_path();
    if (!debugger.has_value()) {
        MessageBoxW(
            window,
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
        window,
        L"runas",
        debugger->c_str(),
        parameters.c_str(),
        nullptr,
        SW_SHOWNORMAL
    );

    if (!shell_execute_succeeded(result)) {
        MessageBoxW(
            window,
            L"Failed to start WinDbg for CacheMon DbgPrintEx output.",
            L"SSD Cache driver log",
            MB_OK | MB_ICONWARNING
        );
    }
}

const wchar_t* describe_state(ssd_cache::ServiceState state) {
    switch (state) {
        case ssd_cache::ServiceState::Missing:
            return L"missing";
        case ssd_cache::ServiceState::Stopped:
            return L"stopped";
        case ssd_cache::ServiceState::StartPending:
            return L"starting";
        case ssd_cache::ServiceState::StopPending:
            return L"stopping";
        case ssd_cache::ServiceState::Running:
            return L"running";
    }

    return L"unknown";
}

HICON tray_icon_for_driver_state(ssd_cache::ServiceState state) {
    HICON custom_icon = nullptr;
    WORD icon_id = kErrorIconId;

    switch (state) {
        case ssd_cache::ServiceState::Running:
            custom_icon = g_tray_icons.running;
            icon_id = kAppIconId;
            break;
        case ssd_cache::ServiceState::StartPending:
        case ssd_cache::ServiceState::StopPending:
        case ssd_cache::ServiceState::Stopped:
            custom_icon = g_tray_icons.warning;
            icon_id = kWarningIconId;
            break;
        case ssd_cache::ServiceState::Missing:
            custom_icon = g_tray_icons.missing;
            icon_id = kErrorIconId;
            break;
    }

    if (custom_icon != nullptr) {
        return custom_icon;
    }

    return LoadIconW(nullptr, MAKEINTRESOURCEW(icon_id));
}

std::wstring build_tray_tooltip() {
    const auto driver_state = ssd_cache::query_driver_state();
    const auto service_state = g_controller.query_state();

    std::wstring tip = L"SSD Cache";
    tip += L" | driver ";
    tip += describe_state(driver_state);
    tip += L" | service ";
    tip += describe_state(service_state);
    return tip;
}

void sync_tray_icon(HWND window, DWORD message) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = tray_icon_for_driver_state(ssd_cache::query_driver_state());

    const auto tip = build_tray_tooltip();
    wcscpy_s(data.szTip, tip.c_str());
    Shell_NotifyIconW(message, &data);
}

ssd_cache::AppConfig load_or_create_config() {
    ssd_cache::AppConfig config;
    config.sqlite_path = ssd_cache::default_sqlite_path();

    if (std::filesystem::exists(std::filesystem::path(g_config_path))) {
        config = ssd_cache::load_config_file(g_config_path);
        if (config.sqlite_path.empty()) {
            config.sqlite_path = ssd_cache::default_sqlite_path();
        }
        return config;
    }

    ssd_cache::save_config_file(g_config_path, config);
    return config;
}

bool equal_ignore_case(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }

    return true;
}

std::optional<ssd_cache::AppMode> infer_current_mode(
    const ssd_cache::AppConfig& config
) {
    const auto source_mapping = ssd_cache::network_unc_for_drive(
        config.source_presentation_letter
    );
    if (source_mapping.has_value() &&
        equal_ignore_case(*source_mapping, config.source_unc)) {
        return ssd_cache::AppMode::Monitor;
    }

    if (!source_mapping.has_value() &&
        ssd_cache::volume_name_for_drive(config.source_presentation_letter) &&
        !ssd_cache::volume_name_for_drive(config.cache_letter)) {
        return ssd_cache::AppMode::Serve;
    }

    return std::nullopt;
}

std::optional<ssd_cache::AppMode> current_mode() {
    const auto config = load_or_create_config();
    if (config.mode.has_value()) {
        return config.mode;
    }

    const auto inferred_mode = infer_current_mode(config);
    if (inferred_mode.has_value()) {
        return inferred_mode;
    }

    return ssd_cache::AppMode::Disabled;
}

bool mode_is_active(ssd_cache::AppMode mode) {
    const auto active_mode = current_mode();
    return active_mode.has_value() && *active_mode == mode;
}

void persist_mode(ssd_cache::AppMode mode) {
    auto config = load_or_create_config();
    config.mode = mode;
    ssd_cache::save_config_file(g_config_path, config);
}

void add_tray_icon(HWND window) {
    sync_tray_icon(window, NIM_ADD);
}

void remove_tray_icon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void append_service_menu_items(HMENU menu) {
    const auto service_state = g_controller.query_state();
    const bool stop_enabled =
        service_state == ssd_cache::ServiceState::Running ||
        service_state == ssd_cache::ServiceState::StartPending;

    AppendMenuW(
        menu,
        MF_STRING | (stop_enabled ? MF_GRAYED : 0),
        kCmdStartService,
        L"Start service"
    );
    AppendMenuW(
        menu,
        MF_STRING | (stop_enabled ? 0 : MF_GRAYED),
        kCmdStopService,
        L"Stop service"
    );
}

void append_mode_menu_item(
    HMENU menu,
    UINT_PTR command,
    const wchar_t* text,
    ssd_cache::AppMode mode,
    const std::optional<ssd_cache::AppMode>& active_mode
) {
    const bool checked = active_mode.has_value() && *active_mode == mode;
    AppendMenuW(
        menu,
        MF_STRING | (checked ? MF_CHECKED | MF_GRAYED : 0),
        command,
        text
    );
}

void show_menu(HWND window) {
    POINT point{};
    GetCursorPos(&point);

    HMENU menu = CreatePopupMenu();
    append_service_menu_items(menu);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const auto active_mode = current_mode();
    append_mode_menu_item(
        menu,
        kCmdDisabledMode,
        L"Disabled mode",
        ssd_cache::AppMode::Disabled,
        active_mode
    );
    append_mode_menu_item(
        menu,
        kCmdMonitorMode,
        L"Monitor mode",
        ssd_cache::AppMode::Monitor,
        active_mode
    );
    append_mode_menu_item(
        menu,
        kCmdServeMode,
        L"Serve mode",
        ssd_cache::AppMode::Serve,
        active_mode
    );
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpenConfig, L"Open config");
    AppendMenuW(menu, MF_STRING, kCmdOpenAppLog, L"Open app log");
    AppendMenuW(menu, MF_STRING, kCmdOpenServiceLog, L"Open service log");
    AppendMenuW(menu, MF_STRING, kCmdOpenDriverLog, L"Open driver log");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    SetForegroundWindow(window);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        point.x,
        point.y,
        0,
        window,
        nullptr
    );
    DestroyMenu(menu);
}

bool enter_monitor_mode() {
    if (mode_is_active(ssd_cache::AppMode::Monitor)) {
        return true;
    }

    const auto config = load_or_create_config();
    g_logger->log("Tray requested monitor mode.");
    // Switch the service first: it (as LocalSystem) moves the cache volume off
    // the presentation letter back to the cache letter, which frees that letter
    // for the user-session network mapping below. Mapping before the switch --
    // while coming from serve mode, where the cache volume still occupies the
    // presentation letter -- hits ERROR_ALREADY_ASSIGNED and silently no-ops,
    // leaving the network drive unrestored.
    const bool switched = g_controller.send_mode(ssd_cache::AppMode::Monitor);
    if (switched) {
        ssd_cache::map_network_drive(
            config.source_presentation_letter,
            config.source_unc
        );
        persist_mode(ssd_cache::AppMode::Monitor);
    }

    return switched;
}

bool enter_serve_mode() {
    if (mode_is_active(ssd_cache::AppMode::Serve)) {
        return true;
    }

    const auto config = load_or_create_config();
    g_logger->log("Tray requested serve mode.");
    ssd_cache::unmap_network_drive(config.source_presentation_letter);
    const bool switched = g_controller.send_mode(ssd_cache::AppMode::Serve);
    if (switched) {
        persist_mode(ssd_cache::AppMode::Serve);
    }

    return switched;
}

bool enter_disabled_mode() {
    if (mode_is_active(ssd_cache::AppMode::Disabled)) {
        return true;
    }

    g_logger->log("Tray requested disabled mode.");
    const bool switched = g_controller.send_mode(ssd_cache::AppMode::Disabled);
    if (switched) {
        persist_mode(ssd_cache::AppMode::Disabled);
    }

    return switched;
}

LRESULT CALLBACK window_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param
) {
    if (message == g_taskbar_created) {
        add_tray_icon(window);
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            add_tray_icon(window);
            SetTimer(window, kStatusTimerId, kStatusTimerIntervalMs, nullptr);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kStatusTimerId);
            remove_tray_icon(window);
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (w_param == kStatusTimerId) {
                sync_tray_icon(window, NIM_MODIFY);
                return 0;
            }
            break;
        case kTrayMessage:
            if (LOWORD(l_param) == WM_RBUTTONUP ||
                LOWORD(l_param) == WM_LBUTTONUP) {
                show_menu(window);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w_param)) {
                case kCmdStartService:
                    g_logger->log("Tray requested service start.");
                    g_controller.start_service();
                    sync_tray_icon(window, NIM_MODIFY);
                    return 0;
                case kCmdStopService:
                    g_logger->log("Tray requested service stop.");
                    g_controller.stop_service();
                    sync_tray_icon(window, NIM_MODIFY);
                    return 0;
                case kCmdDisabledMode:
                    enter_disabled_mode();
                    sync_tray_icon(window, NIM_MODIFY);
                    return 0;
                case kCmdMonitorMode:
                    enter_monitor_mode();
                    sync_tray_icon(window, NIM_MODIFY);
                    return 0;
                case kCmdServeMode:
                    enter_serve_mode();
                    sync_tray_icon(window, NIM_MODIFY);
                    return 0;
                case kCmdOpenConfig:
                    load_or_create_config();
                    g_logger->log("Tray opened config file.");
                    open_text_file(window, g_config_path);
                    return 0;
                case kCmdOpenAppLog:
                    g_logger->log("Tray opened app log.");
                    open_text_file(window, g_app_log_path);
                    return 0;
                case kCmdOpenServiceLog:
                    g_logger->log("Tray opened service log.");
                    open_text_file(window, g_service_log_path);
                    return 0;
                case kCmdOpenDriverLog:
                    g_logger->log("Tray opened driver log.");
                    open_driver_log(window);
                    return 0;
                case kCmdExit:
                    g_logger->log("Tray exit requested.");
                    DestroyWindow(window);
                    return 0;
            }
            break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

// Signals a running tray instance (a message-only window of kWindowClassName)
// to shut down, mirroring the "Exit" menu entry.
int exit_running_instance() {
    HWND existing = FindWindowExW(
        HWND_MESSAGE,
        nullptr,
        kWindowClassName,
        nullptr
    );

    if (existing == nullptr) {
        g_logger->log("CLI exit requested but no running tray instance found.");
        return 1;
    }

    g_logger->log("CLI requested tray exit.");
    PostMessageW(existing, WM_CLOSE, 0, 0);
    return 0;
}

// Executes a single tray menu action requested on the command line. Each
// recognized command performs its action and the process exits. Returns the
// process exit code for a recognized command, or -1 to indicate no command was
// given and the GUI (tray icon + message loop) should launch normally.
int run_cli(int argc, wchar_t** argv) {
    if (wants_help(argc, argv)) {
        show_help();
        return 0;
    }

    for (int index = 1; index < argc; ++index) {
        const wchar_t* arg = argv[index];

        if (_wcsicmp(arg, L"--start-service") == 0) {
            g_logger->log("CLI requested service start.");
            return g_controller.start_service() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--stop-service") == 0) {
            g_logger->log("CLI requested service stop.");
            return g_controller.stop_service() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--disabled-mode") == 0) {
            return enter_disabled_mode() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--monitor-mode") == 0) {
            return enter_monitor_mode() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--serve-mode") == 0) {
            return enter_serve_mode() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--open-config") == 0) {
            load_or_create_config();
            g_logger->log("CLI opened config file.");
            open_text_file(nullptr, g_config_path);
            return 0;
        }
        if (_wcsicmp(arg, L"--open-app-log") == 0) {
            g_logger->log("CLI opened app log.");
            open_text_file(nullptr, g_app_log_path);
            return 0;
        }
        if (_wcsicmp(arg, L"--open-service-log") == 0) {
            g_logger->log("CLI opened service log.");
            open_text_file(nullptr, g_service_log_path);
            return 0;
        }
        if (_wcsicmp(arg, L"--open-driver-log") == 0) {
            g_logger->log("CLI opened driver log.");
            open_driver_log(nullptr);
            return 0;
        }
        if (_wcsicmp(arg, L"--exit") == 0) {
            return exit_running_instance();
        }

        // An unrecognized switch is a usage error; anything else falls through
        // to launching the tray GUI.
        if (arg[0] == L'-' || arg[0] == L'/') {
            show_text(
                std::wstring(L"Unknown option: ") + arg + L"\r\n" +
                    L"Run with --help to see the available commands.\r\n",
                L"ssd-cache-tray"
            );
            return 2;
        }
    }

    return -1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Paths, controller and logger are shared by both the command-line commands
    // and the tray GUI, so initialize them before dispatching either.
    g_config_path = ssd_cache::default_config_path();
    g_app_log_path = ssd_cache::default_app_log_path();
    g_service_log_path = ssd_cache::default_service_log_path();
    g_logger = std::make_unique<ssd_cache::WinFileLogger>(g_app_log_path);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        const int cli_result = run_cli(argc, argv);
        LocalFree(argv);
        if (cli_result >= 0) {
            return cli_result;
        }
    }

    GdiplusSession gdiplus_session;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    load_tray_icons();
    g_logger->log("Tray application started.");

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClassName;

    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"SSD Cache",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        nullptr
    );

    if (window == nullptr) {
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_logger->log("Tray application stopped.");
    destroy_tray_icons();
    return 0;
}
