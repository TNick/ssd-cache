#pragma once

#include <string>

#include "ssd_cache/logger.h"

namespace ssd_cache {

class WinFileLogger : public ILogger {
public:
    explicit WinFileLogger(std::wstring path);

    void log(const std::string& message) noexcept override;

    void log(const std::wstring& message) noexcept;

    const std::wstring& path() const;

private:
    std::wstring path_;
};

}  // namespace ssd_cache
