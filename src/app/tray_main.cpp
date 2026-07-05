/**
 * @file
 * @brief ssd-cache-tray: the SSD Cache system tray application.
 *
 * This is the user-facing front end for the SSD Cache system. It runs in the
 * interactive desktop session and does two jobs from a single executable:
 *
 *   - GUI mode (no command-line arguments): registers a notification-area
 *     (system tray) icon whose picture and tooltip reflect the live driver and
 *     service state, and whose right-click menu drives the service (start/stop,
 *     mode switching, opening config and logs).
 *   - CLI mode (a recognized switch): performs the equivalent of one menu action
 *     and exits, so the same operations are scriptable. See run_cli / show_help.
 *
 * Responsibilities are deliberately split with the Windows service: this process
 * runs as the logged-in user and therefore owns the per-session network drive
 * mapping, while the service (running as LocalSystem) owns machine-global volume
 * mount points. Mode switches here coordinate both sides -- see enter_*_mode.
 */

// Standard library.
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Win32: base API, COM interfaces (pulled in by GDI+), the shell (tray icon,
// ShellExecute) and GDI+ (decoding the PNG tray icons).
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <gdiplus.h>

// SSD Cache: config model, well-known paths, file logger, drive mount helpers
// and the service/driver controller queried for state and used to switch modes.
#include "ssd_cache/config.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_file_logger.h"
#include "ssd_cache/win_mount_manager.h"
#include "ssd_cache/win_service_controller.h"

namespace {

/**
 * Notification-area wiring. kTrayMessage is the private window message the shell
 * posts to us for mouse activity on the icon; kTrayIconId identifies our single
 * icon. The status timer re-polls driver/service state every couple of seconds
 * so the icon and tooltip stay current without any explicit refresh.
 */
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT_PTR kStatusTimerId = 2;
constexpr UINT kStatusTimerIntervalMs = 2000;

/** Stock shell icon ids, used as fallbacks when a custom PNG icon fails to load. */
constexpr WORD kAppIconId = 32512;
constexpr WORD kErrorIconId = 32513;
constexpr WORD kWarningIconId = 32515;

/**
 * WM_COMMAND identifiers for the right-click menu entries. Each maps one-to-one
 * to a case in window_proc and (except Exit) to a CLI switch in run_cli.
 */
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

/**
 * Window class of the hidden message-only window. Also used by --exit to locate
 * an already-running instance (see exit_running_instance).
 */
constexpr wchar_t kWindowClassName[] = L"SsdCacheTrayWindow";

/**
 * The three custom tray icons, decoded once from PNG at startup and reused for
 * every icon update. A null member means that PNG failed to load and a stock
 * icon is substituted.
 */
struct TrayIcons {
    HICON running = nullptr;
    HICON warning = nullptr;
    HICON missing = nullptr;
};

/**
 * RAII wrapper that starts GDI+ for the lifetime of the object and shuts it down
 * on destruction. GDI+ is needed to decode the PNG tray icons; a zero token
 * records a failed startup so the destructor skips the matching shutdown.
 */
class GdiplusSession {
public:
    /** Starts a GDI+ session, recording a zero token if startup fails. */
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        const auto status = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
        if (status != Gdiplus::Ok) {
            token_ = 0;
        }
    }

    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

    /** Shuts down the GDI+ session if it started successfully. */
    ~GdiplusSession() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

private:
    ULONG_PTR token_ = 0;
};

/**
 * Process-wide state. wWinMain fills the paths and logger before anything else
 * runs, and both the CLI and GUI paths read them. g_taskbar_created holds the
 * registered "TaskbarCreated" message id so the icon is re-added if Explorer
 * restarts. The controller is stateless and safe to share.
 */
UINT g_taskbar_created = 0;
std::wstring g_config_path;
std::wstring g_app_log_path;
std::wstring g_service_log_path;
ssd_cache::WinServiceController g_controller;
TrayIcons g_tray_icons;
std::unique_ptr<ssd_cache::WinFileLogger> g_logger;

/**
 * Reports whether the command line requests help.
 *
 * @param argc Number of arguments in @p argv.
 * @param argv Argument vector (argv[0] is the executable path).
 * @return True if any argument is a help switch (--help, -h or /?).
 */
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

/**
 * Presents a block of text to the user. This is a WIN32 (GUI) subsystem
 * executable, so it owns no console. When run from a terminal we attach to the
 * parent console and write there; when run from Explorer (no parent console) we
 * fall back to a message box.
 *
 * @param text Message to display.
 * @param title Message-box caption; used only on the Explorer/dialog fallback.
 */
void show_text(const std::wstring& text, const wchar_t* title) {
    // Terminal case: borrow the parent's console, write to it, then detach so we
    // leave the caller's console exactly as we found it.
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

    // No parent console (launched from Explorer): fall back to a dialog.
    MessageBoxW(nullptr, text.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

/** Prints the CLI usage/help text, mirroring the tray menu actions. */
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

/**
 * Returns the directory containing this executable. The tray icon PNGs are
 * deployed alongside the binary, so their paths are resolved relative to it.
 *
 * @return Absolute path of the directory holding this executable.
 */
std::wstring module_directory() {
    // Grow the buffer until the full path fits (GetModuleFileNameW truncates and
    // returns the buffer size when the name is longer).
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

/**
 * Builds the absolute path to a tray icon PNG deployed next to the binary.
 *
 * @param file_name Icon file name (e.g. L"driver_running.png").
 * @return Absolute path to @p file_name in the executable's directory.
 */
std::wstring tray_icon_path(const wchar_t* file_name) {
    return (
        std::filesystem::path(module_directory()) / file_name
    ).wstring();
}

/**
 * Destroys an icon handle, tolerating null.
 *
 * @param icon Icon to destroy; ignored when null.
 */
void destroy_icon(HICON icon) {
    if (icon != nullptr) {
        DestroyIcon(icon);
    }
}

/** Frees all loaded tray icons and resets the cache to empty handles. */
void destroy_tray_icons() {
    destroy_icon(g_tray_icons.running);
    destroy_icon(g_tray_icons.warning);
    destroy_icon(g_tray_icons.missing);
    g_tray_icons = {};
}

/**
 * Ensures a file (and its parent directory) exists so it can be opened in an
 * editor. Opening in append mode creates an empty file without truncating one
 * that is already there.
 *
 * @param path File to create if missing.
 */
void ensure_file_exists(const std::wstring& path) {
    ssd_cache::ensure_parent_directory(path);
    std::ofstream output(std::filesystem::path(path), std::ios::app);
}

/**
 * Decodes a PNG into an icon via GDI+.
 *
 * @param path Path to the PNG file.
 * @return The decoded icon, or null on any failure (missing file, decode error).
 */
HICON load_png_icon(const std::wstring& path) {
    Gdiplus::Bitmap bitmap(path.c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }

    HICON icon = nullptr;
    const auto status = bitmap.GetHICON(&icon);
    return status == Gdiplus::Ok ? icon : nullptr;
}

/**
 * (Re)loads the three custom tray icons from disk. Any icon that fails to load
 * stays null and is later substituted with a stock icon by
 * tray_icon_for_driver_state.
 */
void load_tray_icons() {
    destroy_tray_icons();
    g_tray_icons.running = load_png_icon(tray_icon_path(L"driver_running.png"));
    g_tray_icons.warning = load_png_icon(tray_icon_path(L"driver_warning.png"));
    g_tray_icons.missing = load_png_icon(tray_icon_path(L"driver_missing.png"));
}

/**
 * Opens a text file in Notepad, creating it first if necessary.
 *
 * @param window Owner window for any shell UI; may be null.
 * @param path File to open (created empty if it does not exist).
 */
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

/**
 * Interprets a ShellExecuteW return value. The API returns a pseudo-HINSTANCE
 * that is greater than 32 on success and an error code otherwise.
 *
 * @param result The value returned by ShellExecuteW.
 * @return True if the shell operation succeeded.
 */
bool shell_execute_succeeded(HINSTANCE result) {
    return reinterpret_cast<INT_PTR>(result) > 32;
}

/**
 * Locates windbg.exe under the installed Windows Kits, trying the x64, arm64 and
 * x86 debugger directories in turn.
 *
 * @return Path to the first windbg.exe found, or nullopt when none is present.
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
 * Opens the CacheMon driver's debug output. The minifilter logs via DbgPrintEx,
 * so there is no log file to open; instead this launches WinDbg (elevated) in
 * local-kernel mode with the driver's debug mask enabled and dumps the buffer.
 * Shows a warning dialog if WinDbg is missing or fails to start.
 *
 * @param window Owner window for the warning dialogs; may be null.
 */
void open_driver_log(HWND window) {
    // Without WinDbg there is nothing to show; explain that to the user.
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

    // Launch elevated (runas): local-kernel debugging requires it. The command
    // widens the driver's debug-print mask, then dumps the DbgPrint buffer.
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

/**
 * Maps a service/driver state to a short lowercase word for the tooltip.
 *
 * @param state State to describe.
 * @return A static, lowercase word such as L"running" or L"missing".
 */
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

/**
 * Chooses the tray icon for a driver state: the running icon when running, a
 * warning icon while transitioning or stopped, and an error icon when missing.
 *
 * @param state Driver state to represent.
 * @return The matching custom icon, or a stock shell icon if that PNG failed to
 *         load. The handle is owned elsewhere and must not be destroyed here.
 */
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

/**
 * Builds the hover tooltip summarizing current driver and service state.
 *
 * @return Tooltip text of the form "SSD Cache | driver <state> | service
 *         <state>".
 */
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

/**
 * Adds or updates the tray icon, refreshing both its picture and tooltip from
 * live state.
 *
 * @param window Window that receives the tray callback messages.
 * @param message Shell_NotifyIconW action, typically NIM_ADD or NIM_MODIFY.
 */
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

/**
 * Loads the machine config, creating it with defaults on first run. The SQLite
 * path is always filled in (from the default) so callers never see it empty.
 *
 * @return The loaded (or newly created) configuration.
 */
ssd_cache::AppConfig load_or_create_config() {
    ssd_cache::AppConfig config;
    config.sqlite_path = ssd_cache::default_sqlite_path();

    // Existing config: load it, then backfill a missing SQLite path.
    if (std::filesystem::exists(std::filesystem::path(g_config_path))) {
        config = ssd_cache::load_config_file(g_config_path);
        if (config.sqlite_path.empty()) {
            config.sqlite_path = ssd_cache::default_sqlite_path();
        }
        return config;
    }

    // First run: persist the defaults so the file exists for later edits.
    ssd_cache::save_config_file(g_config_path, config);
    return config;
}

/**
 * Case-insensitive comparison of two wide strings.
 *
 * @param left First string.
 * @param right Second string.
 * @return True if the strings are equal ignoring case.
 */
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

/**
 * Infers the active mode from the current drive layout when the config does not
 * record one explicitly. Monitor is detected by the source letter being mapped
 * to the configured UNC; Serve by the source letter holding a local volume while
 * the cache letter is unmounted.
 *
 * @param config Configuration providing the source UNC and drive letters.
 * @return The inferred mode, or nullopt when neither pattern matches.
 */
std::optional<ssd_cache::AppMode> infer_current_mode(
    const ssd_cache::AppConfig& config
) {
    // Monitor: the presentation letter is a network drive pointing at the source.
    const auto source_mapping = ssd_cache::network_unc_for_drive(
        config.source_presentation_letter
    );
    if (source_mapping.has_value() &&
        equal_ignore_case(*source_mapping, config.source_unc)) {
        return ssd_cache::AppMode::Monitor;
    }

    // Serve: the presentation letter is a local volume (the cache moved onto it)
    // and the cache letter is no longer mounted.
    if (!source_mapping.has_value() &&
        ssd_cache::volume_name_for_drive(config.source_presentation_letter) &&
        !ssd_cache::volume_name_for_drive(config.cache_letter)) {
        return ssd_cache::AppMode::Serve;
    }

    return std::nullopt;
}

/**
 * Determines the active mode, preferring the value persisted in the config, then
 * an inference from the live drive layout, and finally defaulting to Disabled.
 *
 * @return The active mode (always engaged; never nullopt in practice).
 */
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

/**
 * Reports whether a mode is the currently active one.
 *
 * @param mode Mode to test.
 * @return True if @p mode is the active mode.
 */
bool mode_is_active(ssd_cache::AppMode mode) {
    const auto active_mode = current_mode();
    return active_mode.has_value() && *active_mode == mode;
}

/**
 * Writes a mode into the config file, preserving all other settings.
 *
 * @param mode Mode to persist.
 */
void persist_mode(ssd_cache::AppMode mode) {
    auto config = load_or_create_config();
    config.mode = mode;
    ssd_cache::save_config_file(g_config_path, config);
}

/**
 * Adds the tray icon to the notification area.
 *
 * @param window Window that receives the tray callback messages.
 */
void add_tray_icon(HWND window) {
    sync_tray_icon(window, NIM_ADD);
}

/**
 * Removes the tray icon from the notification area.
 *
 * @param window Window the icon was registered against.
 */
void remove_tray_icon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

/**
 * Appends the Start/Stop service entries, graying out whichever one does not
 * apply to the current service state.
 *
 * @param menu Popup menu to append the two entries to.
 */
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

/**
 * Appends one mode entry, check-marking and disabling it when it is the active
 * mode so the user cannot re-select the mode they are already in.
 *
 * @param menu Popup menu to append the entry to.
 * @param command WM_COMMAND id posted when the entry is chosen.
 * @param text Entry label.
 * @param mode Mode this entry represents.
 * @param active_mode Currently active mode, used to decide the check mark.
 */
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

/**
 * Builds and shows the right-click context menu at the cursor, then blocks until
 * a selection is made or the menu is dismissed.
 *
 * @param window Window that owns the menu and receives its WM_COMMAND messages.
 */
void show_menu(HWND window) {
    POINT point{};
    GetCursorPos(&point);

    // Service controls, then a separator.
    HMENU menu = CreatePopupMenu();
    append_service_menu_items(menu);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // The three mutually exclusive modes, with the active one checked.
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

    // Config/log shortcuts and Exit.
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpenConfig, L"Open config");
    AppendMenuW(menu, MF_STRING, kCmdOpenAppLog, L"Open app log");
    AppendMenuW(menu, MF_STRING, kCmdOpenServiceLog, L"Open service log");
    AppendMenuW(menu, MF_STRING, kCmdOpenDriverLog, L"Open driver log");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    // SetForegroundWindow is required so the menu dismisses on outside clicks.
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

/**
 * Switches to Monitor mode: the source share is presented at the source letter
 * and the cache lives on the cache letter.
 *
 * Ordering matters. The service is switched first: it (as LocalSystem) moves the
 * cache volume off the presentation letter back to the cache letter, which frees
 * that letter for the user-session network mapping done afterwards.
 *
 * @return True on success, or if already in Monitor mode; false if the service
 *         refused the mode switch.
 */
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

/**
 * Switches to Serve mode: the network drive is unmapped and the service moves
 * the cache volume onto the presentation letter so cached files are served
 * offline.
 *
 * @return True on success, or if already in Serve mode; false if the service
 *         refused the mode switch.
 */
bool enter_serve_mode() {
    if (mode_is_active(ssd_cache::AppMode::Serve)) {
        return true;
    }

    const auto config = load_or_create_config();
    g_logger->log("Tray requested serve mode.");
    // Unmap the network drive first so the presentation letter is free for the
    // service to remount the cache volume there.
    ssd_cache::unmap_network_drive(config.source_presentation_letter);
    const bool switched = g_controller.send_mode(ssd_cache::AppMode::Serve);
    if (switched) {
        persist_mode(ssd_cache::AppMode::Serve);
    }

    return switched;
}

/**
 * Switches to Disabled mode: the service stops caching. Drive letters are left
 * as-is.
 *
 * @return True on success, or if already Disabled; false if the service refused
 *         the mode switch.
 */
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

/**
 * Window procedure for the hidden message-only window. Handles the shell's
 * TaskbarCreated broadcast (re-adds the icon when Explorer restarts), lifecycle
 * messages, the status-refresh timer, tray mouse callbacks and menu commands.
 *
 * @param window Handle of the window receiving the message.
 * @param message Message id.
 * @param w_param Message-specific WPARAM (e.g. the timer id or command id).
 * @param l_param Message-specific LPARAM (e.g. the tray mouse event).
 * @return Zero for messages handled here; otherwise the DefWindowProcW result.
 */
LRESULT CALLBACK window_proc(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param
) {
    // Explorer restarted: our icon was lost, so re-add it.
    if (message == g_taskbar_created) {
        add_tray_icon(window);
        return 0;
    }

    switch (message) {
        // Window lifecycle: add the icon and start the refresh timer on create;
        // tear both down and quit on destroy.
        case WM_CREATE:
            add_tray_icon(window);
            SetTimer(window, kStatusTimerId, kStatusTimerIntervalMs, nullptr);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kStatusTimerId);
            remove_tray_icon(window);
            PostQuitMessage(0);
            return 0;

        // Periodic refresh: repaint the icon/tooltip from live state.
        case WM_TIMER:
            if (w_param == kStatusTimerId) {
                sync_tray_icon(window, NIM_MODIFY);
                return 0;
            }
            break;

        // Mouse activity on the tray icon: open the menu on either button up.
        case kTrayMessage:
            if (LOWORD(l_param) == WM_RBUTTONUP ||
                LOWORD(l_param) == WM_LBUTTONUP) {
                show_menu(window);
            }
            return 0;

        // Menu selections. Each action is logged, performed, and (where it can
        // change state) followed by an icon refresh.
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

/**
 * Signals a running tray instance (a message-only window of kWindowClassName) to
 * shut down, mirroring the "Exit" menu entry.
 *
 * @return 0 if a running instance was found and signaled; 1 if none exists.
 */
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

/**
 * Executes a single tray menu action requested on the command line. Each
 * recognized command performs its action and the process exits.
 *
 * @param argc Number of arguments in @p argv.
 * @param argv Argument vector (argv[0] is the executable path).
 * @return The process exit code for a recognized command (0 success, 1 failure,
 *         2 usage error), or -1 to indicate no command was given and the GUI
 *         (tray icon + message loop) should launch normally.
 */
int run_cli(int argc, wchar_t** argv) {
    if (wants_help(argc, argv)) {
        show_help();
        return 0;
    }

    for (int index = 1; index < argc; ++index) {
        const wchar_t* arg = argv[index];

        // Service control.
        if (_wcsicmp(arg, L"--start-service") == 0) {
            g_logger->log("CLI requested service start.");
            return g_controller.start_service() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--stop-service") == 0) {
            g_logger->log("CLI requested service stop.");
            return g_controller.stop_service() ? 0 : 1;
        }

        // Mode switching (shares the same helpers as the menu commands).
        if (_wcsicmp(arg, L"--disabled-mode") == 0) {
            return enter_disabled_mode() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--monitor-mode") == 0) {
            return enter_monitor_mode() ? 0 : 1;
        }
        if (_wcsicmp(arg, L"--serve-mode") == 0) {
            return enter_serve_mode() ? 0 : 1;
        }

        // Open config / logs.
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

        // Stop a running instance.
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

/**
 * Entry point. Initializes shared state, dispatches a CLI command if one was
 * given, and otherwise runs the tray GUI: a hidden message-only window plus the
 * standard Win32 message loop.
 *
 * @param instance Module instance handle for the running process.
 * @param prev_instance Legacy previous-instance handle; always null and unused.
 * @param command_line Unused; arguments are re-parsed via GetCommandLineW.
 * @param show_command Unused; this process has no top-level visible window.
 * @return The CLI command's exit code, or 0 after the GUI message loop ends
 *         (1 if the message-only window could not be created).
 */
int WINAPI wWinMain(
    HINSTANCE instance,
    [[maybe_unused]] HINSTANCE prev_instance,
    [[maybe_unused]] PWSTR command_line,
    [[maybe_unused]] int show_command
) {
    // Paths, controller and logger are shared by both the command-line commands
    // and the tray GUI, so initialize them before dispatching either.
    g_config_path = ssd_cache::default_config_path();
    g_app_log_path = ssd_cache::default_app_log_path();
    g_service_log_path = ssd_cache::default_service_log_path();
    g_logger = std::make_unique<ssd_cache::WinFileLogger>(g_app_log_path);

    // CLI dispatch: a non-negative result means a command ran and we exit with
    // its code; -1 means no command was given, so fall through to the GUI.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        const int cli_result = run_cli(argc, argv);
        LocalFree(argv);
        if (cli_result >= 0) {
            return cli_result;
        }
    }

    // GUI startup: bring up GDI+, learn the Explorer-restart message, and load
    // the icons the window will display.
    GdiplusSession gdiplus_session;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    load_tray_icons();
    g_logger->log("Tray application started.");

    // Register and create the hidden message-only window that hosts the icon.
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

    // Standard message loop; runs until WM_DESTROY posts WM_QUIT.
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_logger->log("Tray application stopped.");
    destroy_tray_icons();
    return 0;
}
