#include "ssd_cache/win_filter_activity_source.h"

#include <algorithm>
#include <cwchar>
#include <stdexcept>
#include <utility>

#include <fltuser.h>

#include "cachemon_messages.h"
#include "ssd_cache/path_mapper.h"

namespace ssd_cache {
namespace {

struct CachemonFilterMessage {
    FILTER_MESSAGE_HEADER header;
    CACHEMON_EVENT event;
};

std::wstring bounded_wstring(const wchar_t* value, std::size_t limit) {
    const std::size_t length = wcsnlen_s(value, limit);
    return std::wstring(value, value + length);
}

AccessKind convert_kind(std::uint32_t kind) {
    switch (kind) {
        case CachemonAccessReadOpen:
            return AccessKind::ReadOpen;
        case CachemonAccessWriteObserved:
            return AccessKind::WriteObserved;
        case CachemonAccessWriteClosed:
            return AccessKind::WriteClosed;
        case CachemonAccessRename:
            return AccessKind::Rename;
        case CachemonAccessDelete:
            return AccessKind::Delete;
        default:
            return AccessKind::ReadOpen;
    }
}

void copy_bounded_wide(
    wchar_t* destination,
    std::size_t destination_count,
    const std::wstring& source
) {
    if (destination_count == 0) {
        return;
    }

    const auto count = std::min(destination_count - 1, source.size());
    std::wmemcpy(destination, source.data(), count);
    destination[count] = L'\0';
}

}  // namespace

WinFilterActivitySource::WinFilterActivitySource(
    std::wstring source_unc,
    EventCallback callback
)
    : source_unc_(std::move(source_unc)), callback_(std::move(callback)) {}

WinFilterActivitySource::~WinFilterActivitySource() {
    stop();
}

void WinFilterActivitySource::start() {
    if (reader_.joinable()) {
        return;
    }

    HRESULT hr = FilterConnectCommunicationPort(
        CACHEMON_PORT_NAME,
        0,
        nullptr,
        0,
        nullptr,
        &port_
    );
    if (FAILED(hr)) {
        throw std::runtime_error("failed to connect to CacheMon minifilter");
    }

    CACHEMON_COMMAND command{};
    command.version = CACHEMON_COMMAND_VERSION;
    command.command = CachemonCommandRegisterService;
    command.service_pid = GetCurrentProcessId();
    copy_bounded_wide(
        command.source_unc,
        CACHEMON_MAX_ROOT_CHARS,
        source_unc_
    );

    DWORD bytes_returned = 0;
    hr = FilterSendMessage(
        port_,
        &command,
        sizeof(command),
        nullptr,
        0,
        &bytes_returned
    );
    if (FAILED(hr)) {
        CloseHandle(port_);
        port_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("failed to register service with minifilter");
    }

    stop_requested_ = false;
    reader_ = std::thread([this]() {
        reader_loop();
    });
}

void WinFilterActivitySource::stop() {
    stop_requested_ = true;

    if (reader_.joinable()) {
        CancelSynchronousIo(reader_.native_handle());
    }

    if (port_ != INVALID_HANDLE_VALUE) {
        CloseHandle(port_);
        port_ = INVALID_HANDLE_VALUE;
    }

    if (reader_.joinable()) {
        reader_.join();
    }
}

void WinFilterActivitySource::reader_loop() {
    while (!stop_requested_) {
        CachemonFilterMessage message{};
        const HRESULT hr = FilterGetMessage(
            port_,
            &message.header,
            sizeof(message),
            nullptr
        );

        if (FAILED(hr)) {
            if (stop_requested_) {
                return;
            }

            Sleep(250);
            continue;
        }

        if (message.event.version != CACHEMON_EVENT_VERSION) {
            continue;
        }

        AccessEvent event;
        event.source_root_id = bounded_wstring(
            message.event.source_root_id,
            CACHEMON_MAX_ROOT_CHARS
        );
        event.relative_path = bounded_wstring(
            message.event.relative_path,
            CACHEMON_MAX_RELATIVE_CHARS
        );

        const auto mapped = map_observed_source_path(
            source_unc_,
            event.source_root_id.empty() ? event.relative_path :
                join_root_relative(event.source_root_id, event.relative_path)
        );
        if (mapped) {
            event.source_root_id = mapped->source_root_id;
            event.relative_path = mapped->relative_path;
        } else if (event.source_root_id.empty()) {
            continue;
        }

        event.kind = convert_kind(message.event.kind);
        event.size_hint = message.event.size_hint;
        event.requestor_pid = message.event.requestor_pid;
        event.observed_at = std::chrono::system_clock::now();

        callback_(event);
    }
}

}  // namespace ssd_cache
