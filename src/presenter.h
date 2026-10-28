#pragma once

#include "common.h"
#include "config.h"
#include "display.h"
#include "itm_params.h"
#include "osd.h"
#include "scene.h"

namespace openhdr {

// The real swap chain's format: FP16 scRGB, or HDR10 (10-bit PQ, BT.2020) where something else in the
// game wraps the swap chain and only accepts 8/10-bit buffers (Intel XeFG frame generation, e.g. via
// OptiScaler). Chosen per swap chain when it is created.
enum class OutputMode { ScRgb, Hdr10 };
inline DXGI_FORMAT OutputFormat(OutputMode mode) {
    return mode == OutputMode::Hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
}
inline DXGI_COLOR_SPACE_TYPE OutputColorSpace(OutputMode mode) {
    return mode == OutputMode::Hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}
inline const char* OutputName(OutputMode mode) { return mode == OutputMode::Hdr10 ? "HDR10" : "FP16 scRGB"; }

// SDR back buffer formats OpenHDR converts.
bool IsConvertibleFormat(DXGI_FORMAT format);
// The typeless member of a convertible format's family (R8G8B8A8_UNORM -> R8G8B8A8_TYPELESS...).
DXGI_FORMAT TypelessOf(DXGI_FORMAT format);

// Private data on proxy back buffers: the game's back buffer format (a DXGI_FORMAT). Proxies are
// created typeless so that both UNORM and _SRGB views work, as on real swap chain buffers; the
// view hooks use this to give format-less views the game's format.
// {4F1B7C2D-8E3A-4C5B-9D60-2A7E1F3B9C48}
constexpr GUID kProxyFormatGuid = {0x4f1b7c2d, 0x8e3a, 0x4c5b, {0x9d, 0x60, 0x2a, 0x7e, 0x1f, 0x3b, 0x9c, 0x48}};

// What the game asked for when it created (or last resized) the swap chain.
struct GameBufferDesc {
    DXGI_FORMAT format;
    DXGI_SAMPLE_DESC sample;
    UINT bufferCount;
    DXGI_SWAP_EFFECT swapEffect;
    DXGI_USAGE usage;
};

// One converted swap chain: the SDR proxy back buffers the game renders into, and the pass that
// converts them to scRGB in the real FP16 back buffers (plus the settings panel) before every Present.
// This base holds what doesn't depend on the graphics API: the game's view of the swap chain,
// settings, hotkeys, the panel and shader parameters. Dx11Presenter and Dx12Presenter do the GPU work.
// Owned by the SwapChainWrapper; `swapChain` arguments are always the real DXGI swap chain.
class Presenter {
public:
    virtual ~Presenter() = default;

    // False once the game switched the swap chain to a format we pass through untouched.
    bool Active() const { return active_; }
    DXGI_FORMAT GameFormat() const { return game_.format; }
    OutputMode Output() const { return output_; }
    void PatchDesc(DXGI_SWAP_CHAIN_DESC& desc) const;
    void PatchDesc1(DXGI_SWAP_CHAIN_DESC1& desc) const;
    void OnGameColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);

    virtual HRESULT GetProxyBuffer(UINT index, REFIID riid, void** out) = 0;

    // ResizeBuffers support: release before the real resize, recreate after it.
    virtual void ReleaseBuffers() = 0;
    HRESULT RecreateBuffers(IDXGISwapChain* swapChain, DXGI_FORMAT gameFormat, UINT gameBufferCount);
    void Deactivate(DXGI_FORMAT gameFormat);

    // Converts the current proxy into the current real back buffer. Called right before Present.
    virtual void Render(IDXGISwapChain* swapChain) = 0;

protected:
    Presenter(const GameBufferDesc& game, const DisplayInfo& display, HWND hwnd, OutputMode output);

    // Creates the proxies for the real swap chain's current size; sets width_, height_ and active_.
    virtual HRESULT CreateBuffers(IDXGISwapChain* swapChain) = 0;

    // Per-frame bookkeeping before drawing: settings reload, hotkeys, panel input. Returns the settings.
    Config BeginFrame();
    // Re-renders the panel's pixels if it is open; true when they changed and must be uploaded.
    bool UpdatePanel(const Config& cfg);
    // Shader parameters for this frame; also logs setting changes and triggers DumpFrame requests.
    ShaderParams FrameParams(const Config& cfg, bool panelShown);
    // Scene analysis: the measurement pass's parameters for this frame, and its (late) results.
    SceneShaderParams SceneParams(const Config& cfg) const;
    void OnSceneMeasured(const SceneStats& measured) { sceneAdapter_.Measured(measured); }

    GameBufferDesc game_{};
    OutputMode output_ = OutputMode::ScRgb;
    DisplayInfo display_;
    float autoPeakNits_ = 0.0f;
    HWND hwnd_ = nullptr;
    DXGI_COLOR_SPACE_TYPE gameColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT width_ = 0;
    UINT height_ = 0;
    bool active_ = false;
    bool dumpPending_ = false;
    uint64_t frameIndex_ = 0;
    Osd osd_{ProcessConfig()};

private:
    bool Focused() const;
    bool PollHotkeys(bool focused, const Config& cfg);

    bool compare_ = false;
    HotkeyWatcher hotkeys_;
    uint32_t loggedConfigVersion_ = 0;
    SceneAdapter sceneAdapter_;
    SceneLog sceneLog_;
};

}  // namespace openhdr
