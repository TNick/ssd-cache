#pragma once

/**
 * @file
 * @brief Abstract interfaces the core depends on, implemented by platform code.
 */

#include "ssd_cache/config.h"

namespace ssd_cache {

/**
 * Source of file-access activity. Concrete implementations connect to the
 * driver (or a test fake) and deliver events; start/stop bracket the delivery.
 */
class IActivitySource {
public:
    virtual ~IActivitySource() = default;

    /** Begins delivering activity events. */
    virtual void start() = 0;

    /** Stops delivering activity events and releases the underlying source. */
    virtual void stop() = 0;
};

/**
 * Enacts a mode by rearranging how the source and cache are presented as drives.
 * The platform implementation performs the actual network mapping and volume
 * mount-point changes.
 */
class IMountPresentation {
public:
    virtual ~IMountPresentation() = default;

    /**
     * Presents Monitor mode: source share mapped, cache on the cache letter.
     *
     * @param config Source UNC and drive letters to apply.
     * @return True on success.
     */
    virtual bool enter_monitor_mode(const AppConfig& config) = 0;

    /**
     * Presents Serve mode: the cache volume is moved onto the source letter to
     * serve cached files offline.
     *
     * @param config Source UNC and drive letters to apply.
     * @return True on success.
     */
    virtual bool enter_serve_mode(const AppConfig& config) = 0;

    /**
     * Presents Disabled mode: no active caching.
     *
     * @param config Source UNC and drive letters to apply.
     * @return True on success.
     */
    virtual bool enter_disabled_mode(const AppConfig& config) = 0;
};

}  // namespace ssd_cache
