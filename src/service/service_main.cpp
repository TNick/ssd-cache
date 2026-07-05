#include <string>

#include <windows.h>
#include <shellapi.h>

#include "ssd_cache/win_config_paths.h"
#include "ssd_cache/win_service_controller.h"
#include "ssd_cache/win_service_host.h"

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

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    const auto config_path = ssd_cache::default_config_path();
    ssd_cache::WinServiceHost host(ssd_cache::kServiceName, config_path);
    ssd_cache::WinServiceController controller;

    if (argv != nullptr && has_arg(argc, argv, L"--install")) {
        const bool installed = controller.install_service(module_path());
        LocalFree(argv);
        return installed ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--start")) {
        const bool started = controller.start_service();
        LocalFree(argv);
        return started ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--stop")) {
        const bool stopped = controller.stop_service();
        LocalFree(argv);
        return stopped ? 0 : 1;
    }

    if (argv != nullptr && has_arg(argc, argv, L"--console")) {
        LocalFree(argv);
        return host.run_console();
    }

    if (argv != nullptr) {
        LocalFree(argv);
    }

    return host.run();
}
