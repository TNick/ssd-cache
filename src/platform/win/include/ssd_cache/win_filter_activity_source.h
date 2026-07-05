#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>

#include "ssd_cache/access_event.h"
#include "ssd_cache/interfaces.h"

namespace ssd_cache {

class WinFilterActivitySource final : public IActivitySource {
public:
    using EventCallback = std::function<void(const AccessEvent&)>;

    WinFilterActivitySource(std::wstring source_unc, EventCallback callback);

    WinFilterActivitySource(const WinFilterActivitySource&) = delete;
    WinFilterActivitySource& operator=(const WinFilterActivitySource&) = delete;

    ~WinFilterActivitySource() override;

    void start() override;

    void stop() override;

private:
    void reader_loop();

    std::wstring source_unc_;
    EventCallback callback_;
    HANDLE port_ = INVALID_HANDLE_VALUE;
    std::thread reader_;
    std::atomic_bool stop_requested_{false};
};

}  // namespace ssd_cache
