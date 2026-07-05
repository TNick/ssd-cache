#pragma once

#include <string>

namespace ssd_cache {

// Minimal, platform-agnostic logging sink used by the core library.
//
// Core components (e.g. the pending-copy scheduler) have no knowledge of how or
// where log lines are persisted; they only depend on this interface. Platform
// code supplies a concrete implementation (on Windows, WinFileLogger) and wires
// it in. The pointer handed to core components may be null, in which case no
// logging is performed, so every call site must tolerate a null logger.
class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(const std::string& message) noexcept = 0;
};

}  // namespace ssd_cache
