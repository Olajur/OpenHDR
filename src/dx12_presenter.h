#pragma once

#include "presenter.h"

#include <d3d12.h>

#include <memory>
#include <vector>

namespace openhdr {

// D3D12: one proxy per swap chain buffer (the game renders into proxy N while the real chain is on
// buffer N). The conversion is our own command list, executed on the game's queue right before
// Present, with its own allocators and fence, so nothing of the game's command state is touched.
class Dx12Presenter final : public Presenter {
public:
    static HRESULT Create(ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, const GameBufferDesc& game,
                          const DisplayInfo& display, HWND hwnd, OutputMode output,
                          std::unique_ptr<Presenter>& out);
    ~Dx12Presenter() override;

    HRESULT GetProxyBuffer(UINT index, REFIID riid, void** out) override;
    void ReleaseBuffers() override;
    void Render(IDXGISwapChain* swapChain) override;

private:
    using Presenter::Presenter;

    struct Frame {
        // What the game renders into; left in PRESENT (COMMON) by the game. The real buffers are fetched
        // per frame and never kept: other layers (frame generation) may resize the swap chain behind
        // our back, which fails while anyone still holds a buffer.
        ComPtr<ID3D12Resource> proxy;
        ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
    };

    HRESULT CreatePipeline();
    HRESULT CreateBuffers(IDXGISwapChain* swapChain) override;
    void WaitFor(uint64_t value);
    void WaitIdle();
    void UploadPanel(bool changed);
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(UINT index) const;
    void WritePanelDescriptors();
    void DumpFrame(UINT index, ID3D12Resource* real);

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;

    // Shader-visible heap: per frame, [proxy SRV, panel SRV]. RTV heap: one per real buffer.
    ComPtr<ID3D12DescriptorHeap> srvHeap_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    UINT srvStride_ = 0;
    UINT rtvStride_ = 0;
    std::vector<Frame> frames_;

    ComPtr<ID3D12Resource> panelTexture_;
    ComPtr<ID3D12Resource> panelUpload_;
    D3D12_RESOURCE_STATES panelState_ = D3D12_RESOURCE_STATE_COPY_DEST;

    // Scene analysis: a kSceneGridWidth x kSceneGridHeight float target (RTV after the real buffers'),
    // copied each frame into a ring of read-back buffers that are read once their frame's fence passed.
    void RecordSceneAnalysis(UINT index, const Config& cfg);
    void CollectSceneAnalysis();
    ComPtr<ID3D12PipelineState> scenePipeline_;
    ComPtr<ID3D12Resource> sceneTarget_;
    ComPtr<ID3D12Resource> sceneReadback_[kSceneReadbackLatency];
    uint64_t sceneFence_[kSceneReadbackLatency] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sceneFootprint_{};
    uint64_t sceneWritten_ = 0;
};

}  // namespace openhdr
