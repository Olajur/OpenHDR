#include "itm_params.h"

#include "scene.h"

#include <algorithm>
#include <cmath>

namespace openhdr {
namespace {

// Must match itm.hlsl.
constexpr uint32_t kInputGamma22 = 0;
constexpr uint32_t kInputSrgb = 1;
constexpr uint32_t kInputPq = 2;
constexpr uint32_t kFlagProcess = 1;
constexpr uint32_t kFlagCompare = 2;
constexpr uint32_t kFlagSrvIsSrgb = 4;
constexpr uint32_t kFlagOsd = 8;
constexpr uint32_t kFlagOutputPq = 16;

constexpr float kScRgbNits = 80.0f;
constexpr float kOsdMaxNits = 200.0f;  // panel white, kept moderate so it doesn't glare in dark scenes
constexpr float kOsdOpacity = 0.9f;

bool IsSrgbFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

}  // namespace

ShaderParams BuildShaderParams(const Config& cfg, const PassInfo& pass) {
    const float peakNits = PeakNits(cfg, pass.autoPeakNits);
    const float paperNits = PaperWhiteNits(cfg, peakNits);
    SceneAdjustment scene{peakNits, cfg.shoulderStart};
    if (pass.scene) scene = AdaptToScene(cfg, *pass.scene, paperNits, peakNits, pass.fullFrameNits);

    ShaderParams p{};
    p.paperWhite = paperNits / kScRgbNits;
    p.peakWhite = scene.peakNits / kScRgbNits;
    p.shoulderStart = scene.shoulderStart;
    p.expansionPower = cfg.expansionPower;
    // Slider mapping (levels -100..+100): contrast is the SDR decoding gamma, 2.0 + 0.008 x level, so
    // 0 / +25 / +50 decode like gamma 2.0 / 2.2 / 2.4; saturation is 1.05 + 0.002 x level, so 0 is a 5%
    // boost and -25 is neutral.
    p.saturation = 1.05f + 0.002f * cfg.saturationLevel;
    p.decodeGamma = ContrastGamma(cfg.contrastLevel);
    p.highlightSaturation = cfg.highlightSaturation;
    p.ditherAmount = cfg.dither;
    p.inputMode = pass.inputIsHdr10 ? kInputPq : cfg.inputTransfer == InputTransfer::Srgb ? kInputSrgb : kInputGamma22;
    p.flags = (cfg.enabled && cfg.conversion ? kFlagProcess : 0) | (pass.compare ? kFlagCompare : 0) |
              (IsSrgbFormat(pass.inputFormat) ? kFlagSrvIsSrgb : 0) | (pass.osdShown ? kFlagOsd : 0) | (pass.outputPq ? kFlagOutputPq : 0);
    p.frameIndex = static_cast<uint32_t>(pass.frameIndex);
    p.splitX = cfg.compareSplit * pass.width;
    p.inputLsb = pass.inputFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? 1.0f / 1023.0f : 1.0f / 255.0f;
    // Debanding: neighbours within 2 (low) or 4 (high) code values count as one flat area; the search
    // radius grows with the image so it covers similar detail at any resolution.
    p.debandThreshold = cfg.debanding == 0 ? 0.0f : (cfg.debanding == 1 ? 2.0f : 4.0f) * p.inputLsb;
    p.debandRadius = (cfg.debanding == 2 ? 24.0f : 16.0f) * std::max(pass.height / 1080.0f, 0.5f);
    p.osdWhite = std::min(paperNits, kOsdMaxNits) / kScRgbNits;
    p.osdOpacity = kOsdOpacity;
    if (pass.osdShown) {
        // Top-right corner, inset by 3% of the height.
        const float margin = std::round(pass.height * 0.03f);
        p.osdRect[0] = std::max(0.0f, pass.width - margin - pass.osdWidth);
        p.osdRect[1] = margin;
        p.osdRect[2] = static_cast<float>(pass.osdWidth);
        p.osdRect[3] = static_cast<float>(pass.osdHeight);
    }
    p.inputOffset[0] = static_cast<float>(pass.inputOffsetX);
    p.inputOffset[1] = static_cast<float>(pass.inputOffsetY);
    return p;
}

}  // namespace openhdr
