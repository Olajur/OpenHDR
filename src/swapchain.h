#pragma once

#include "common.h"
#include "presenter.h"

#include <memory>

namespace openhdr {

// The swap chain object OpenHDR hands to the game in place of the real FP16 one.
//
// Wrapping instead of patching DXGI's swap chain vtable keeps OpenHDR out of the way of overlays
// (Steam, Discord, RTSS...) that hook the real swap chain: they see an untouched DXGI object, and
// our Present simply calls into whatever they installed.
class SwapChainWrapper final : public IDXGISwapChain4 {
public:
    SwapChainWrapper(ComPtr<IDXGISwapChain4> real, std::unique_ptr<Presenter> presenter);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDXGIObject
    STDMETHODIMP SetPrivateData(REFGUID name, UINT size, const void* data) override;
    STDMETHODIMP SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override;
    STDMETHODIMP GetPrivateData(REFGUID name, UINT* size, void* data) override;
    STDMETHODIMP GetParent(REFIID riid, void** parent) override;

    // IDXGIDeviceSubObject
    STDMETHODIMP GetDevice(REFIID riid, void** device) override;

    // IDXGISwapChain
    STDMETHODIMP Present(UINT syncInterval, UINT flags) override;
    STDMETHODIMP GetBuffer(UINT index, REFIID riid, void** surface) override;
    STDMETHODIMP SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override;
    STDMETHODIMP GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override;
    STDMETHODIMP GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override;
    STDMETHODIMP ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override;
    STDMETHODIMP ResizeTarget(const DXGI_MODE_DESC* params) override;
    STDMETHODIMP GetContainingOutput(IDXGIOutput** output) override;
    STDMETHODIMP GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override;
    STDMETHODIMP GetLastPresentCount(UINT* count) override;

    // IDXGISwapChain1
    STDMETHODIMP GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override;
    STDMETHODIMP GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override;
    STDMETHODIMP GetHwnd(HWND* hwnd) override;
    STDMETHODIMP GetCoreWindow(REFIID riid, void** window) override;
    STDMETHODIMP Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) override;
    STDMETHODIMP_(BOOL) IsTemporaryMonoSupported() override;
    STDMETHODIMP GetRestrictToOutput(IDXGIOutput** output) override;
    STDMETHODIMP SetBackgroundColor(const DXGI_RGBA* color) override;
    STDMETHODIMP GetBackgroundColor(DXGI_RGBA* color) override;
    STDMETHODIMP SetRotation(DXGI_MODE_ROTATION rotation) override;
    STDMETHODIMP GetRotation(DXGI_MODE_ROTATION* rotation) override;

    // IDXGISwapChain2
    STDMETHODIMP SetSourceSize(UINT width, UINT height) override;
    STDMETHODIMP GetSourceSize(UINT* width, UINT* height) override;
    STDMETHODIMP SetMaximumFrameLatency(UINT latency) override;
    STDMETHODIMP GetMaximumFrameLatency(UINT* latency) override;
    STDMETHODIMP_(HANDLE) GetFrameLatencyWaitableObject() override;
    STDMETHODIMP SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) override;
    STDMETHODIMP GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) override;

    // IDXGISwapChain3
    STDMETHODIMP_(UINT) GetCurrentBackBufferIndex() override;
    STDMETHODIMP CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colorSpace, UINT* support) override;
    STDMETHODIMP SetColorSpace1(DXGI_COLOR_SPACE_TYPE colorSpace) override;
    STDMETHODIMP ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                const UINT* nodeMasks, IUnknown* const* queues) override;

    // IDXGISwapChain4
    STDMETHODIMP SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) override;

private:
    ~SwapChainWrapper();

    HRESULT Resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT* nodeMasks,
                   IUnknown* const* queues, bool resize1);

    LONG refs_ = 1;
    uint64_t presents_ = 0;  // for logging the first Present
    ComPtr<IDXGISwapChain4> real_;
    std::unique_ptr<Presenter> presenter_;
};

// Sets the real swap chain to scRGB (linear BT.709).
void ApplyOutputColorSpace(IDXGISwapChain4* real, OutputMode mode);

}  // namespace openhdr
