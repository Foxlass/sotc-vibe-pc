#pragma once

#include "ps2_runtime.h"

namespace sotc
{
    // SOTC_FRAME_LOCK: 60 (default), 30, or 0 (the game's own pacing).
    void installFramePacing(PS2Runtime &runtime);
}
