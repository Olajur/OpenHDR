#include "swapchain.h"

#include <algorithm>

namespace openhdr {

void ApplyOutputColorSpace(IDXGISwapChain4* real, OutputMode mode) {
    HRESULT hr = real->SetColorSpace1(OutputColorSpace(mode));
    if (FAILED(hr)) Log("SetColorSpace1(%s) failed: 0x%08lX", OutputName(mode), hr);
}

SwapChainWrapper::SwapChainWrapper(ComPtr<IDXGISwapChain4> real, std::unique_ptr<Presenter> presenter)
    : real_(std::move(real)), presenter_(std::move(presenter)) {}

SwapChainWrapper::~SwapChainWrapper() { Log("Swap chain %p released", real_.Get()); }

// ---- IUnknown -----------------------------------------------------------------

STDMETHODIMP SwapChainWrapper::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
        *out = static_cast<IDXGISwapChain4*>(this);
        AddRef();
        return S_OK;
    }
    // Anything else (e.g. IDXGISwapChainMedia, or another layer's private interface) is answered by the
    // real swap chain. Logged once per interface: such queries show which layers talk to the swap chain.
    const HRESULT hr = real_->QueryInterface(riid, out);
    static GUID logged[16];
    static LONG loggedCount = 0;
    bool known = false;
    for (LONG i = 0; i < loggedCount; ++i) known = known || IsEqualGUID(logged[i], riid);
    if (!known && loggedCount < LONG(std::size(logged))) {
        logged[loggedCount++] = riid;
        Log("Swap chain queried for {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}: 0x%08lX", riid.Data1, riid.Data2,
            riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6],
            riid.Data4[7], hr);
    }
    return hr;
}

STDMETHODIMP_(ULONG) SwapChainWrapper::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

STDMETHODIMP_(ULONG) SwapChainWrapper::Release() {
    const LONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return static_cast<ULONG>(refs);
}

// ---- intercepted --------------------------------------------------------------

STDMETHODIMP SwapChainWrapper::Present(UINT syncInterval, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) presenter_->Render(real_.Get());
    if (presents_ == 0) Log("First Present (sync %u, flags 0x%X)", syncInterval, flags);
    const HRESULT hr = real_->Present(syncInterval, flags);
    if (presents_++ == 0) Log("First Present returned 0x%08lX", hr);
    return hr;
}

STDMETHODIMP SwapChainWrapper::Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) presenter_->Render(real_.Get());
    if (presents_ == 0) Log("First Present1 (sync %u, flags 0x%X)", syncInterval, flags);
    const HRESULT hr = real_->Present1(syncInterval, flags, params);
    if (presents_++ == 0) Log("First Present1 returned 0x%08lX", hr);
    return hr;
}

STDMETHODIMP SwapChainWrapper::GetBuffer(UINT index, REFIID riid, void** surface) {
    if (presenter_->Active()) return presenter_->GetProxyBuffer(index, riid, surface);
    return real_->GetBuffer(index, riid, surface);
}

STDMETHODIMP SwapChainWrapper::GetDesc(DXGI_SWAP_CHAIN_DESC* desc) {
    HRESULT hr = real_->GetDesc(desc);
    if (SUCCEEDED(hr) && desc && presenter_->Active()) presenter_->PatchDesc(*desc);
    return hr;
}

STDMETHODIMP SwapChainWrapper::GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) {
    HRESULT hr = real_->GetDesc1(desc);
    if (SUCCEEDED(hr) && desc && presenter_->Active()) presenter_->PatchDesc1(*desc);
    return hr;
}

STDMETHODIMP SwapChainWrapper::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    return Resize(count, width, height, format, flags, nullptr, nullptr, false);
}

STDMETHODIMP SwapChainWrapper::ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                              const UINT* nodeMasks, IUnknown* const* queues) {
    return Resize(count, width, height, format, flags, nodeMasks, queues, true);
}

HRESULT SwapChainWrapper::Resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                 const UINT* nodeMasks, IUnknown* const* queues, bool resize1) {
    auto realResize = [&](UINT c, DXGI_FORMAT f, UINT fl) {
        return resize1 ? real_->ResizeBuffers1(c, width, height, f, fl, nodeMasks, queues)
                       : real_->ResizeBuffers(c, width, height, f, fl);
    };
    presenter_->ReleaseBuffers();
    const DXGI_FORMAT gameFormat = format == DXGI_FORMAT_UNKNOWN ? presenter_->GameFormat() : format;
    if (!IsConvertibleFormat(gameFormat)) {
        // The game switched to a format we don't convert (e.g. its own HDR output): get out of the way.
        Log("ResizeBuffers to format %d: conversion disabled for this swap chain", gameFormat);
        presenter_->Deactivate(gameFormat);
        return realResize(count, gameFormat, flags);
    }

    HRESULT hr = realResize(count == 0 ? 0 : std::max(2u, count), OutputFormat(presenter_->Output()), flags & ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
    if (FAILED(hr)) {
        Log("ResizeBuffers(%u x %u) failed: 0x%08lX", width, height, hr);
        return hr;
    }
    HRESULT proxyHr = presenter_->RecreateBuffers(real_.Get(), gameFormat, count);
    if (FAILED(proxyHr)) Log("Recreating proxy buffers failed: 0x%08lX", proxyHr);
    ApplyOutputColorSpace(real_.Get(), presenter_->Output());
    return hr;
}

STDMETHODIMP SwapChainWrapper::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE colorSpace, UINT* support) {
    if (!presenter_->Active()) return real_->CheckColorSpaceSupport(colorSpace, support);
    // Answer as an SDR swap chain would, so the game doesn't switch to an HDR path of its own.
    if (!support) return E_INVALIDARG;
    *support = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
    return S_OK;
}

STDMETHODIMP SwapChainWrapper::SetColorSpace1(DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (!presenter_->Active()) return real_->SetColorSpace1(colorSpace);
    // The real chain stays scRGB; the game's choice only tells us how to decode its output.
    presenter_->OnGameColorSpace(colorSpace);
    return S_OK;
}

STDMETHODIMP SwapChainWrapper::SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void* metadata) {
    if (!presenter_->Active()) return real_->SetHDRMetaData(type, size, metadata);
    return S_OK;
}

// ---- forwarded ----------------------------------------------------------------

STDMETHODIMP SwapChainWrapper::SetPrivateData(REFGUID name, UINT size, const void* data) {
    return real_->SetPrivateData(name, size, data);
}
STDMETHODIMP SwapChainWrapper::SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) {
    return real_->SetPrivateDataInterface(name, unknown);
}
STDMETHODIMP SwapChainWrapper::GetPrivateData(REFGUID name, UINT* size, void* data) {
    return real_->GetPrivateData(name, size, data);
}
STDMETHODIMP SwapChainWrapper::GetParent(REFIID riid, void** parent) { return real_->GetParent(riid, parent); }
STDMETHODIMP SwapChainWrapper::GetDevice(REFIID riid, void** device) { return real_->GetDevice(riid, device); }
STDMETHODIMP SwapChainWrapper::SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) {
    return real_->SetFullscreenState(fullscreen, target);
}
STDMETHODIMP SwapChainWrapper::GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) {
    return real_->GetFullscreenState(fullscreen, target);
}
STDMETHODIMP SwapChainWrapper::ResizeTarget(const DXGI_MODE_DESC* params) { return real_->ResizeTarget(params); }
STDMETHODIMP SwapChainWrapper::GetContainingOutput(IDXGIOutput** output) { return real_->GetContainingOutput(output); }
STDMETHODIMP SwapChainWrapper::GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) { return real_->GetFrameStatistics(stats); }
STDMETHODIMP SwapChainWrapper::GetLastPresentCount(UINT* count) { return real_->GetLastPresentCount(count); }
STDMETHODIMP SwapChainWrapper::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) { return real_->GetFullscreenDesc(desc); }
STDMETHODIMP SwapChainWrapper::GetHwnd(HWND* hwnd) { return real_->GetHwnd(hwnd); }
STDMETHODIMP SwapChainWrapper::GetCoreWindow(REFIID riid, void** window) { return real_->GetCoreWindow(riid, window); }
STDMETHODIMP_(BOOL) SwapChainWrapper::IsTemporaryMonoSupported() { return real_->IsTemporaryMonoSupported(); }
STDMETHODIMP SwapChainWrapper::GetRestrictToOutput(IDXGIOutput** output) { return real_->GetRestrictToOutput(output); }
STDMETHODIMP SwapChainWrapper::SetBackgroundColor(const DXGI_RGBA* color) { return real_->SetBackgroundColor(color); }
STDMETHODIMP SwapChainWrapper::GetBackgroundColor(DXGI_RGBA* color) { return real_->GetBackgroundColor(color); }
STDMETHODIMP SwapChainWrapper::SetRotation(DXGI_MODE_ROTATION rotation) { return real_->SetRotation(rotation); }
STDMETHODIMP SwapChainWrapper::GetRotation(DXGI_MODE_ROTATION* rotation) { return real_->GetRotation(rotation); }
STDMETHODIMP SwapChainWrapper::SetSourceSize(UINT width, UINT height) { return real_->SetSourceSize(width, height); }
STDMETHODIMP SwapChainWrapper::GetSourceSize(UINT* width, UINT* height) { return real_->GetSourceSize(width, height); }
STDMETHODIMP SwapChainWrapper::SetMaximumFrameLatency(UINT latency) { return real_->SetMaximumFrameLatency(latency); }
STDMETHODIMP SwapChainWrapper::GetMaximumFrameLatency(UINT* latency) { return real_->GetMaximumFrameLatency(latency); }
STDMETHODIMP_(HANDLE) SwapChainWrapper::GetFrameLatencyWaitableObject() { return real_->GetFrameLatencyWaitableObject(); }
STDMETHODIMP SwapChainWrapper::SetMatrixTransform(const DXGI_MATRIX_3X2_F* matrix) { return real_->SetMatrixTransform(matrix); }
STDMETHODIMP SwapChainWrapper::GetMatrixTransform(DXGI_MATRIX_3X2_F* matrix) { return real_->GetMatrixTransform(matrix); }
STDMETHODIMP_(UINT) SwapChainWrapper::GetCurrentBackBufferIndex() { return real_->GetCurrentBackBufferIndex(); }

}  // namespace openhdr
