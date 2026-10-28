#pragma once

#include "common.h"
#include "config.h"

namespace openhdr {

// Constant buffer of shaders/itm.hlsl; layout must match.
struct alignas(16) ShaderParams {
    float paperWhite;  // scRGB units (1.0 = 80 nits)
    float peakWhite;   // scRGB units
    float shoulderStart;
    float expansionPower;
    float saturation;   // factor
    float decodeGamma;  // contrast
    float highlightSaturation;
    float ditherAmount;
    uint32_t inputMode;
    uint32_t flags;
    uint32_t frameIndex;
    float splitX;
    float inputLsb;
    float osdWhite;  // scRGB units
    float osdOpacity;
    float debandThreshold;  // code values (0..1); 0 = off
    float osdRect[4];      // x, y, width, height in pixels
    float inputOffset[2];  // input texel read for output pixel (0, 0)
    float debandRadius;    // pixels
    float pad1;
};
static_assert(sizeof(ShaderParams) == 96);

// What the conversion pass is drawing this frame, besides the settings.
struct PassInfo {
    float autoPeakNits = 1000.0f;
    UINT width = 0;  // output size in pixels
    UINT height = 0;
    uint64_t frameIndex = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool inputIsHdr10 = false;  // the game already outputs HDR10 (PQ): pass it through
    bool compare = false;
    bool outputPq = false;      // write HDR10 (PQ, BT.2020) instead of scRGB
    bool osdShown = false;
    UINT osdWidth = 0;
    UINT osdHeight = 0;
    int inputOffsetX = 0;
    int inputOffsetY = 0;
    const struct SceneStats* scene = nullptr;  // smoothed scene statistics, if measured yet
    float fullFrameNits = 0.0f;                // the display's sustained full-frame luminance (0 = unknown)
};

ShaderParams BuildShaderParams(const Config& cfg, const PassInfo& pass);

// The contrast slider as an SDR decoding gamma: 0 = 2.0, +25 = 2.2, +50 = 2.4 (see itm_params.cpp).
inline float ContrastGamma(int contrastLevel) { return 2.0f + 0.008f * contrastLevel; }

}  // namespace openhdr
