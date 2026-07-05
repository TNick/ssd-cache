#pragma once

/**
 * @file
 * @brief Applies an operating mode to the machine's drive presentation.
 */

#include "ssd_cache/config.h"
#include "ssd_cache/interfaces.h"

namespace ssd_cache {

/**
 * Drives mode transitions by delegating the concrete drive/mount changes to an
 * IMountPresentation. This keeps the transition policy platform-agnostic and
 * testable, with the Windows-specific mount work injected as a dependency.
 */
class ModeManager {
public:
    /**
     * @param presentation Mount presentation used to enact each mode; must
     *        outlive this manager.
     */
    explicit ModeManager(IMountPresentation& presentation);

    /**
     * Switches to the requested mode.
     *
     * @param mode Target mode to enter.
     * @param config Configuration providing the source UNC and drive letters
     *        the presentation acts on.
     * @return True if the presentation successfully entered @p mode.
     */
    bool switch_to(AppMode mode, const AppConfig& config);

private:
    IMountPresentation& presentation_;
};

}  // namespace ssd_cache
