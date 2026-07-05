#pragma once

#include <string>

#include <windows.h>

namespace ssd_cache {

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

    std::wstring service_name_;
    std::wstring config_path_;
    SERVICE_STATUS_HANDLE status_handle_ = nullptr;
    SERVICE_STATUS status_{};
    HANDLE stop_event_ = nullptr;
};

}  // namespace ssd_cache
