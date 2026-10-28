#pragma once

#include "common.h"

namespace openhdr {

struct DisplayInfo {
    bool found = false;
    bool hdr = false;
    float maxNits = 0.0f;
    float maxFullFrameNits = 0.0f;
};

// HDR state and luminance of the display showing `hwnd`.
DisplayInfo QueryDisplay(IDXGIFactory* factory, HWND hwnd);

}  // namespace openhdr
