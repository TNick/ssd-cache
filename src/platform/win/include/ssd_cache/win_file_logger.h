#pragma once

/**
 * @file
 * @brief Windows file-backed implementation of the ILogger sink.
 */

#include <string>

#include "ssd_cache/logger.h"

namespace ssd_cache {

/**
 * Appends timestamped log lines to a file. Implements ILogger so core
 * components can log through it, and adds a wide-string overload for callers
 * that already hold UTF-16 text.
 */
class WinFileLogger : public ILogger {
public:
    /**
     * @param path Path of the log file to append to (created as needed).
     */
    explicit WinFileLogger(std::wstring path);

    /**
     * Appends a UTF-8 message as one log line. Never throws.
     *
     * @param message The message to log.
     */
    void log(const std::string& message) noexcept override;

    /**
     * Appends a wide-string message as one log line. Never throws.
     *
     * @param message The message to log.
     */
    void log(const std::wstring& message) noexcept;

    /**
     * @return The log file path this logger writes to.
     */
    const std::wstring& path() const;

private:
    std::wstring path_;
};

}  // namespace ssd_cache
