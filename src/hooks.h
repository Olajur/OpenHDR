#pragma once

#include "common.h"

namespace openhdr {

// Hooks swap chain creation on the DXGI factory vtable (shared by every factory the process creates).
// Swap chains created before this call are left alone.
void InstallDxgiHooks();

}  // namespace openhdr
