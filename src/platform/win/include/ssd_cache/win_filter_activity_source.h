#pragma once

/**
 * @file
 * @brief Activity source backed by the CacheMon minifilter communication port.
 */

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>

#include "ssd_cache/access_event.h"
#include "ssd_cache/interfaces.h"

namespace ssd_cache {

/**
 * Connects to the CacheMon minifilter's communication port and delivers each
 * driver event to a callback on a background reader thread. Implements
 * IActivitySource so the service can consume events without knowing the
 * transport.
 */
class WinFilterActivitySource final : public IActivitySource {
public:
    /** Callback invoked for each observed access event. */
    using EventCallback = std::function<void(const AccessEvent&)>;

    /**
     * @param source_unc Source UNC used to map observed paths to the source root.
     * @param callback Invoked on the reader thread for each event; must be safe
     *        to call from that thread.
     */
    WinFilterActivitySource(std::wstring source_unc, EventCallback callback);

    WinFilterActivitySource(const WinFilterActivitySource&) = delete;
    WinFilterActivitySource& operator=(const WinFilterActivitySource&) = delete;

    ~WinFilterActivitySource() override;

    /** Connects to the port, registers with the driver and starts reading. */
    void start() override;

    /** Stops the reader thread and closes the port. */
    void stop() override;

private:
    /** Reader-thread loop: pulls messages from the port and dispatches them. */
    void reader_loop();

    std::wstring source_unc_;
    EventCallback callback_;
    HANDLE port_ = INVALID_HANDLE_VALUE;
    std::thread reader_;
    std::atomic_bool stop_requested_{false};
};

}  // namespace ssd_cache
