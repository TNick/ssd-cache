#include <filesystem>
#include <string>

#include <windows.h>
#include <shellapi.h>

#include "ssd_cache/config.h"
#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_mount_manager.h"
#include "ssd_cache/win_service_controller.h"

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr int kCmdStartService = 1001;
constexpr int kCmdStopService = 1002;
constexpr int kCmdDisabledMode = 1003;
constexpr int kCmdMonitorMode = 1004;
constexpr int kCmdServeMode = 1005;
constexpr int kCmdOpenConfig = 1006;
constexpr int kCmdExit = 1007;

UINT g_taskbar_created = 0;
std::wstring g_config_path;
ssd_cache::WinServiceController g_controller;

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

void add_tray_icon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(data.szTip, L"SSD Cache");
    Shell_NotifyIconW(NIM_ADD, &data);
}

void remove_tray_icon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void show_menu(HWND window) {
    POINT point{};
    GetCursorPos(&point);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdStartService, L"Start service");
    AppendMenuW(menu, MF_STRING, kCmdStopService, L"Stop service");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDisabledMode, L"Disabled mode");
    AppendMenuW(menu, MF_STRING, kCmdMonitorMode, L"Monitor mode");
    AppendMenuW(menu, MF_STRING, kCmdServeMode, L"Serve mode");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdOpenConfig, L"Open config");
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

void enter_monitor_mode() {
    const auto config = load_or_create_config();
    ssd_cache::map_network_drive(
        config.source_presentation_letter,
        config.source_unc
    );
    g_controller.send_mode(ssd_cache::AppMode::Monitor);
}

void enter_serve_mode() {
    const auto config = load_or_create_config();
    ssd_cache::unmap_network_drive(config.source_presentation_letter);
    g_controller.send_mode(ssd_cache::AppMode::Serve);
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
            return 0;
        case WM_DESTROY:
            remove_tray_icon(window);
            PostQuitMessage(0);
            return 0;
        case kTrayMessage:
            if (LOWORD(l_param) == WM_RBUTTONUP ||
                LOWORD(l_param) == WM_LBUTTONUP) {
                show_menu(window);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w_param)) {
                case kCmdStartService:
                    g_controller.start_service();
                    return 0;
                case kCmdStopService:
                    g_controller.stop_service();
                    return 0;
                case kCmdDisabledMode:
                    g_controller.send_mode(ssd_cache::AppMode::Disabled);
                    return 0;
                case kCmdMonitorMode:
                    enter_monitor_mode();
                    return 0;
                case kCmdServeMode:
                    enter_serve_mode();
                    return 0;
                case kCmdOpenConfig:
                    load_or_create_config();
                    ShellExecuteW(
                        window,
                        L"open",
                        L"notepad.exe",
                        g_config_path.c_str(),
                        nullptr,
                        SW_SHOWNORMAL
                    );
                    return 0;
                case kCmdExit:
                    DestroyWindow(window);
                    return 0;
            }
            break;
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_config_path = ssd_cache::default_config_path();
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"SsdCacheTrayWindow";

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

    return 0;
}
