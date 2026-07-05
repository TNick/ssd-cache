/**
 * @file
 * @brief Implementation of ModeManager: dispatches a mode to the presentation.
 */

#include "ssd_cache/mode_manager.h"

namespace ssd_cache {

ModeManager::ModeManager(IMountPresentation& presentation)
    : presentation_(presentation) {}

bool ModeManager::switch_to(AppMode mode, const AppConfig& config) {
    switch (mode) {
        case AppMode::Disabled:
            return presentation_.enter_disabled_mode(config);
        case AppMode::Monitor:
            return presentation_.enter_monitor_mode(config);
        case AppMode::Serve:
            return presentation_.enter_serve_mode(config);
    }

    return false;
}

}  // namespace ssd_cache
