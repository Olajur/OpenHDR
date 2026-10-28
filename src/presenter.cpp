#include "presenter.h"

namespace openhdr {
namespace {

}  // namespace

bool IsConvertibleFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return true;
        default:
            return false;
    }
}

DXGI_FORMAT TypelessOf(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        default: return format;
    }
}

Presenter::Presenter(const GameBufferDesc& game, const DisplayInfo& display, HWND hwnd, OutputMode output)
    : game_(game), output_(output), display_(display), autoPeakNits_(AutoPeakNits(display.maxNits)), hwnd_(hwnd) {}

void Presenter::PatchDesc(DXGI_SWAP_CHAIN_DESC& desc) const {
    desc.BufferDesc.Format = game_.format;
    desc.SampleDesc = game_.sample;
    desc.BufferCount = game_.bufferCount;
    desc.SwapEffect = game_.swapEffect;
    desc.BufferUsage = game_.usage;
}

void Presenter::PatchDesc1(DXGI_SWAP_CHAIN_DESC1& desc) const {
    desc.Format = game_.format;
    desc.SampleDesc = game_.sample;
    desc.BufferCount = game_.bufferCount;
    desc.SwapEffect = game_.swapEffect;
    desc.BufferUsage = game_.usage;
}

void Presenter::OnGameColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (colorSpace != gameColorSpace_) Log("Game set color space %d", colorSpace);
    gameColorSpace_ = colorSpace;
}

HRESULT Presenter::RecreateBuffers(IDXGISwapChain* swapChain, DXGI_FORMAT gameFormat, UINT gameBufferCount) {
    game_.format = gameFormat;
    if (gameBufferCount != 0) game_.bufferCount = gameBufferCount;
    return CreateBuffers(swapChain);
}

void Presenter::Deactivate(DXGI_FORMAT gameFormat) {
    ReleaseBuffers();
    game_.format = gameFormat;
    active_ = false;
}

bool Presenter::Focused() const {
    HWND foreground = GetForegroundWindow();
    return foreground && (foreground == hwnd_ || foreground == GetAncestor(hwnd_, GA_ROOT));
}

// Returns whether the panel hotkey was pressed (the panel handles that itself).
bool Presenter::PollHotkeys(bool focused, const Config& cfg) {
    const uint32_t pressed = hotkeys_.Poll(cfg.hotkeys);
    if (!pressed || !focused) return false;
    auto fired = [&](HotkeyAction a) { return (pressed >> static_cast<int>(a)) & 1; };

    if (fired(HotkeyAction::Conversion)) {
        const bool on = !GetConfig().conversion;
        ProcessConfig().SetGameValue(L"Conversion", on ? 1.0f : 0.0f);
        Log("HDR conversion %s", on ? "on" : "off");
    }
    if (fired(HotkeyAction::Compare)) {
        compare_ = !compare_;
        Log("Compare view %s", compare_ ? "on" : "off");
    }
    if (fired(HotkeyAction::Dump)) dumpPending_ = true;
    return fired(HotkeyAction::Panel);
}

Config Presenter::BeginFrame() {
    ++frameIndex_;
    if (frameIndex_ % 30 == 1) ReloadConfigIfChanged();
    const bool focused = Focused();
    const bool panel = PollHotkeys(focused, GetConfig());
    osd_.Update(focused, panel, autoPeakNits_, compare_);
    const Config cfg = GetConfig();
    if (cfg.dumpFrame > 0 && frameIndex_ == static_cast<uint64_t>(cfg.dumpFrame)) dumpPending_ = true;
    return cfg;
}

bool Presenter::UpdatePanel(const Config& cfg) {
    return osd_.Visible() && osd_.Rasterize(height_, cfg, autoPeakNits_, compare_);
}

SceneShaderParams Presenter::SceneParams(const Config& cfg) const {
    SceneShaderParams s{};
    s.inputSize[0] = static_cast<float>(width_);
    s.inputSize[1] = static_cast<float>(height_);
    s.gridSize[0] = static_cast<float>(kSceneGridWidth);
    s.gridSize[1] = static_cast<float>(kSceneGridHeight);
    s.decodeGamma = ContrastGamma(cfg.contrastLevel);
    s.brightThreshold = cfg.shoulderStart;
    s.flags = (game_.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || game_.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? 4u : 0u;
    return s;
}

ShaderParams Presenter::FrameParams(const Config& cfg, bool panelShown) {
    PassInfo pass;
    pass.autoPeakNits = autoPeakNits_;
    pass.width = width_;
    pass.height = height_;
    pass.frameIndex = frameIndex_;
    pass.inputFormat = game_.format;
    pass.inputIsHdr10 = game_.format == DXGI_FORMAT_R10G10B10A2_UNORM &&
                        gameColorSpace_ == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    pass.compare = compare_;
    pass.outputPq = output_ == OutputMode::Hdr10;
    pass.osdShown = osd_.Visible() && panelShown;
    pass.osdWidth = osd_.Width();
    pass.osdHeight = osd_.Height();
    sceneAdapter_.Advance();
    pass.scene = sceneAdapter_.Valid() && cfg.sceneAdaptation > 0.0f ? &sceneAdapter_.Smoothed() : nullptr;
    pass.fullFrameNits = display_.maxFullFrameNits;
    const ShaderParams p = BuildShaderParams(cfg, pass);
    if (pass.scene) sceneLog_.Report(cfg, *pass.scene, p.peakWhite * 80.0f, p.shoulderStart);
    if (cfg.version != loggedConfigVersion_) {
        loggedConfigVersion_ = cfg.version;
        Log("Output: peak %.0f nits, paper white %.0f nits, input mode %u, conversion %s", p.peakWhite * 80.0f,
            p.paperWhite * 80.0f, p.inputMode, (p.flags & 1) ? "on" : "off");
    }
    return p;
}

}  // namespace openhdr
