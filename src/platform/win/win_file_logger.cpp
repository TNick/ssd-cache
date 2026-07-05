#include "ssd_cache/win_file_logger.h"

#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <windows.h>

#include "ssd_cache/utf.h"
#include "ssd_cache/win_config_paths.h"

namespace ssd_cache {
namespace {

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string timestamp_prefix() {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    std::ostringstream stream;
    stream
        << '['
        << now.wYear << '-'
        << (now.wMonth < 10 ? "0" : "") << now.wMonth << '-'
        << (now.wDay < 10 ? "0" : "") << now.wDay << ' '
        << (now.wHour < 10 ? "0" : "") << now.wHour << ':'
        << (now.wMinute < 10 ? "0" : "") << now.wMinute << ':'
        << (now.wSecond < 10 ? "0" : "") << now.wSecond
        << "] ";
    return stream.str();
}

}  // namespace

WinFileLogger::WinFileLogger(std::wstring path) : path_(std::move(path)) {}

void WinFileLogger::log(const std::string& message) noexcept {
    HANDLE handle = INVALID_HANDLE_VALUE;

    try {
        const auto line = timestamp_prefix() + message + '\n';
        ensure_parent_directory(path_);
        if (std::filesystem::exists(std::filesystem::path(path_))) {
            try {
                ensure_network_service_access(path_, false);
            } catch (...) {
            }
        }

        const std::lock_guard<std::mutex> lock(log_mutex());
        handle = CreateFileW(
            path_.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (handle == INVALID_HANDLE_VALUE) {
            const auto debug_message = "Failed to open log file: " +
                wide_to_utf8(path_) + "\n";
            OutputDebugStringA(debug_message.c_str());
            return;
        }

        try {
            ensure_network_service_access(path_, false);
        } catch (...) {
        }

        DWORD bytes_written = 0;
        const BOOL ok = WriteFile(
            handle,
            line.data(),
            static_cast<DWORD>(line.size()),
            &bytes_written,
            nullptr
        );
        if (!ok) {
            const auto debug_message = "Failed to write log file: " +
                wide_to_utf8(path_) + "\n";
            OutputDebugStringA(debug_message.c_str());
        }

        CloseHandle(handle);
    } catch (...) {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        const auto debug_message = "Logger threw while writing: " +
            wide_to_utf8(path_) + "\n";
        OutputDebugStringA(debug_message.c_str());
        return;
    }
}

void WinFileLogger::log(const std::wstring& message) noexcept {
    log(wide_to_utf8(message));
}

const std::wstring& WinFileLogger::path() const {
    return path_;
}

}  // namespace ssd_cache
