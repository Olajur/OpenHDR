#include "dx11_presenter.h"

#include "frame_dump.h"

#include "g_ItmPS.h"
#include "g_ItmVS.h"

#include <new>

namespace openhdr {

HRESULT Dx11Presenter::Create(ID3D11Device* device, IDXGISwapChain* swapChain, const GameBufferDesc& game,
                              const DisplayInfo& display, HWND hwnd, OutputMode output, std::unique_ptr<Presenter>& out) {
    std::unique_ptr<Dx11Presenter> p(new (std::nothrow) Dx11Presenter(game, display, hwnd, output));
    if (!p) return E_OUTOFMEMORY;
    p->device_ = device;
    HRESULT hr = p->CreatePipeline();
    if (SUCCEEDED(hr)) hr = p->CreateBuffers(swapChain);
    if (FAILED(hr)) return hr;
    out = std::move(p);
    return S_OK;
}

HRESULT Dx11Presenter::CreatePipeline() {
    device_->GetImmediateContext(&context_);
    HRESULT hr = context_.As(&context1_);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D11Device1> device1;
    hr = device_.As(&device1);
    if (FAILED(hr)) return hr;

    D3D_FEATURE_LEVEL level = device_->GetFeatureLevel();
    const UINT flags = (device_->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                           ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                           : 0;
    hr = device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &state_);
    if (FAILED(hr)) return hr;

    hr = device_->CreateVertexShader(g_ItmVS, sizeof(g_ItmVS), nullptr, &vs_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(g_ItmPS, sizeof(g_ItmPS), nullptr, &ps_);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ShaderParams);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&cb, nullptr, &params_);
    if (FAILED(hr)) return hr;
    return sceneAnalyzer_.Init(device_.Get());
}

HRESULT Dx11Presenter::CreateBuffers(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC real{};
    HRESULT hr = swapChain->GetDesc(&real);
    if (FAILED(hr)) return hr;
    width_ = real.BufferDesc.Width;
    height_ = real.BufferDesc.Height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    // Typeless like a real swap chain buffer, so the game can create UNORM and _SRGB views of it.
    td.Format = TypelessOf(game_.format);
    td.SampleDesc = game_.sample;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const bool wantUav = (game_.usage & DXGI_USAGE_UNORDERED_ACCESS) && game_.sample.Count == 1;
    if (wantUav) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    hr = device_->CreateTexture2D(&td, nullptr, &proxy_);
    if (FAILED(hr) && wantUav) {
        td.BindFlags &= ~D3D11_BIND_UNORDERED_ACCESS;
        hr = device_->CreateTexture2D(&td, nullptr, &proxy_);
    }
    if (FAILED(hr)) return hr;
    proxy_->SetPrivateData(kProxyFormatGuid, sizeof(game_.format), &game_.format);

    ID3D11Texture2D* source = proxy_.Get();
    if (game_.sample.Count > 1) {
        td.SampleDesc = {1, 0};
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&td, nullptr, &resolve_);
        if (FAILED(hr)) return hr;
        source = resolve_.Get();
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = game_.format;  // an _SRGB format decodes on read; the shader re-encodes (FLAG_SRV_IS_SRGB)
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(source, &sd, &srv_);
    if (FAILED(hr)) return hr;
    active_ = true;
    return S_OK;
}

HRESULT Dx11Presenter::GetProxyBuffer(UINT, REFIID riid, void** out) {
    if (!proxy_) return DXGI_ERROR_INVALID_CALL;
    return proxy_->QueryInterface(riid, out);
}

void Dx11Presenter::ReleaseBuffers() {
    srv_.Reset();
    resolve_.Reset();
    proxy_.Reset();  // the game may still hold references; that only keeps the old texture alive
}

void Dx11Presenter::Render(IDXGISwapChain* swapChain) {
    const Config cfg = BeginFrame();
    if (!active_ || !srv_) return;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return;
    // Created per frame: holding a back buffer reference across frames would block ResizeBuffers.
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv))) return;

    if (resolve_) context_->ResolveSubresource(resolve_.Get(), 0, proxy_.Get(), 0, game_.format);
    if (osd_.Visible()) osdTexture_.Upload(device_.Get(), context_.Get(), osd_, UpdatePanel(cfg));

    ComPtr<ID3DDeviceContextState> gameState;
    context1_->SwapDeviceContextState(state_.Get(), &gameState);

    // Scene analysis first (results arrive a few frames later), then this frame's parameters.
    SceneStats measured;
    if (cfg.sceneAdaptation > 0.0f && sceneAnalyzer_.Measure(context1_.Get(), srv_.Get(), SceneParams(cfg), measured)) {
        OnSceneMeasured(measured);
    }
    const ShaderParams p = FrameParams(cfg, osdTexture_.srv != nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context1_->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &p, sizeof(p));
        context1_->Unmap(params_.Get(), 0);
    }

    ID3D11ShaderResourceView* views[2] = {srv_.Get(), osd_.Visible() ? osdTexture_.srv.Get() : nullptr};
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    context1_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context1_->VSSetShader(vs_.Get(), nullptr, 0);
    context1_->PSSetShader(ps_.Get(), nullptr, 0);
    context1_->PSSetConstantBuffers(0, 1, params_.GetAddressOf());
    context1_->PSSetShaderResources(0, 2, views);
    context1_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context1_->RSSetViewports(1, &viewport);
    context1_->Draw(3, 0);

    // Unbind so the game can render into the proxy again without hazards.
    ID3D11ShaderResourceView* nullViews[2] = {};
    context1_->PSSetShaderResources(0, 2, nullViews);
    context1_->OMSetRenderTargets(0, nullptr, nullptr);
    context1_->SwapDeviceContextState(gameState.Get(), nullptr);

    if (dumpPending_) {
        dumpPending_ = false;
        DumpFrame(backBuffer.Get());
    }
}

void Dx11Presenter::DumpFrame(ID3D11Texture2D* output) {
    const std::wstring dir = DataDir() + L"\\dumps\\";
    DumpTexturePfm(device_.Get(), context_.Get(), output, dir + L"output.pfm");
    DumpTexturePfm(device_.Get(), context_.Get(), resolve_ ? resolve_.Get() : proxy_.Get(), dir + L"input.pfm");
}

}  // namespace openhdr
