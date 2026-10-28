#pragma once

#include "presenter.h"

#include <memory>

namespace openhdr {

// D3D11: one proxy texture (D3D11 flip-model chains only expose buffer 0), converted on the game's
// immediate context inside our own ID3DDeviceContextState so the game's pipeline state is untouched.
class Dx11Presenter final : public Presenter {
public:
    static HRESULT Create(ID3D11Device* device, IDXGISwapChain* swapChain, const GameBufferDesc& game,
                          const DisplayInfo& display, HWND hwnd, OutputMode output,
                          std::unique_ptr<Presenter>& out);

    HRESULT GetProxyBuffer(UINT index, REFIID riid, void** out) override;
    void ReleaseBuffers() override;
    void Render(IDXGISwapChain* swapChain) override;

private:
    using Presenter::Presenter;

    HRESULT CreatePipeline();
    HRESULT CreateBuffers(IDXGISwapChain* swapChain) override;
    void DumpFrame(ID3D11Texture2D* output);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<ID3DDeviceContextState> state_;  // our own pipeline state, swapped in so the game's is untouched
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> params_;

    ComPtr<ID3D11Texture2D> proxy_;    // what the game renders into (its format and sample count)
    ComPtr<ID3D11Texture2D> resolve_;  // single-sample copy of an MSAA proxy
    ComPtr<ID3D11ShaderResourceView> srv_;
    OsdTexture osdTexture_;
    SceneAnalyzer11 sceneAnalyzer_;
};

}  // namespace openhdr
