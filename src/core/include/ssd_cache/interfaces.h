#pragma once

#include "ssd_cache/config.h"

namespace ssd_cache {

class IActivitySource {
public:
    virtual ~IActivitySource() = default;

    virtual void start() = 0;

    virtual void stop() = 0;
};

class IMountPresentation {
public:
    virtual ~IMountPresentation() = default;

    virtual bool enter_monitor_mode(const AppConfig& config) = 0;

    virtual bool enter_serve_mode(const AppConfig& config) = 0;

    virtual bool enter_disabled_mode(const AppConfig& config) = 0;
};

}  // namespace ssd_cache
