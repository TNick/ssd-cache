#pragma once

#include "ssd_cache/config.h"
#include "ssd_cache/interfaces.h"

namespace ssd_cache {

class ModeManager {
public:
    explicit ModeManager(IMountPresentation& presentation);

    bool switch_to(AppMode mode, const AppConfig& config);

private:
    IMountPresentation& presentation_;
};

}  // namespace ssd_cache
