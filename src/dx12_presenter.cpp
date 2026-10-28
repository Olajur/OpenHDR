#include "dx12_presenter.h"

#include "frame_dump.h"

#include "g_ItmPS50.h"
#include "g_ItmVS50.h"
#include "g_ScenePS50.h"
#include "g_SceneVS50.h"

#include <new>

namespace openhdr {
namespace {

constexpr UINT kParamCount = sizeof(ShaderParams) / 4;  // root constants, register b0

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_DESC Texture2DDesc(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    desc.Flags = flags;
    return desc;
}

}  // namespace

HRESULT Dx12Presenter::Create(ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, const GameBufferDesc& game,
                              const DisplayInfo& display, HWND hwnd, OutputMode output, std::unique_ptr<Presenter>& out) {
    std::unique_ptr<Dx12Presenter> p(new (std::nothrow) Dx12Presenter(game, display, hwnd, output));
    if (!p) return E_OUTOFMEMORY;
    p->queue_ = queue;
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&p->device_));
    if (SUCCEEDED(hr)) hr = p->CreatePipeline();
    if (SUCCEEDED(hr)) hr = p->CreateBuffers(swapChain);
    if (FAILED(hr)) return hr;
    out = std::move(p);
    return S_OK;
}

Dx12Presenter::~Dx12Presenter() {
    if (fence_) WaitIdle();
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

HRESULT Dx12Presenter::CreatePipeline() {
    // d3d12.dll is loaded in every D3D12 game; resolving it here keeps it out of our import table.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
                           : nullptr;
    if (!serialize) return E_NOINTERFACE;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;  // t0 game image, t1 settings panel
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kParamCount;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) return hr;
    hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_));
    if (FAILED(hr)) return hr;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_.Get();
    pso.VS = {g_ItmVS50, sizeof(g_ItmVS50)};
    pso.PS = {g_ItmPS50, sizeof(g_ItmPS50)};
    pso.BlendState.RenderTarget[0] = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                                      D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = OutputFormat(output_);
    pso.SampleDesc = {1, 0};
    hr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_));
    if (FAILED(hr)) return hr;

    // Scene analysis: same root signature (its constants are the first 12 of the 24).
    pso.VS = {g_SceneVS50, sizeof(g_SceneVS50)};
    pso.PS = {g_ScenePS50, sizeof(g_ScenePS50)};
    pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    hr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&scenePipeline_));
    if (FAILED(hr)) return hr;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC sceneDesc =
        Texture2DDesc(kSceneGridWidth, kSceneGridHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &sceneDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                          IID_PPV_ARGS(&sceneTarget_));
    if (FAILED(hr)) return hr;
    UINT64 readbackSize = 0;
    device_->GetCopyableFootprints(&sceneDesc, 0, 1, 0, &sceneFootprint_, nullptr, nullptr, &readbackSize);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = readbackSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = {1, 0};
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& readback : sceneReadback_) {
        hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return hr;
    }

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) return hr;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return S_OK;
}

HRESULT Dx12Presenter::CreateBuffers(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC real{};
    HRESULT hr = swapChain->GetDesc(&real);
    if (FAILED(hr)) return hr;
    width_ = real.BufferDesc.Width;
    height_ = real.BufferDesc.Height;
    const UINT count = real.BufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = count * 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_)))) return hr;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = count + 1;  // + the scene analysis target
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_)))) return hr;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (game_.usage & DXGI_USAGE_UNORDERED_ACCESS) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Typeless like a real swap chain buffer, so the game can create UNORM and _SRGB views of it.
    const D3D12_RESOURCE_DESC proxyDesc = Texture2DDesc(width_, height_, TypelessOf(game_.format), flags);

    frames_.resize(count);
    for (UINT i = 0; i < count; ++i) {
        Frame& f = frames_[i];
        if (FAILED(hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &proxyDesc, D3D12_RESOURCE_STATE_COMMON,
                                                         nullptr, IID_PPV_ARGS(&f.proxy)))) {
            return hr;
        }
        f.proxy->SetPrivateData(kProxyFormatGuid, sizeof(game_.format), &game_.format);
        f.proxy->SetName(L"OpenHDR proxy back buffer");
        if (!f.allocator && FAILED(hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) {
            return hr;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = game_.format;  // an _SRGB format decodes on read; the shader re-encodes (FLAG_SRV_IS_SRGB)
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(f.proxy.Get(), &srv, SrvCpu(i * 2));
    }
    WritePanelDescriptors();
    device_->CreateRenderTargetView(sceneTarget_.Get(), nullptr, RtvCpu(count));
    if (!list_) {
        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list_));
        if (FAILED(hr)) return hr;
        list_->Close();  // Render resets it each frame
    }
    active_ = true;
    return S_OK;
}

HRESULT Dx12Presenter::GetProxyBuffer(UINT index, REFIID riid, void** out) {
    if (index >= frames_.size() || !frames_[index].proxy) return DXGI_ERROR_INVALID_CALL;
    return frames_[index].proxy->QueryInterface(riid, out);
}

void Dx12Presenter::ReleaseBuffers() {
    // ResizeBuffers needs every reference to the real buffers gone, and the GPU done with them.
    WaitIdle();
    for (Frame& f : frames_) {
        f.proxy.Reset();
    }
}

void Dx12Presenter::WaitFor(uint64_t value) {
    if (fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, fenceEvent_);
    WaitForSingleObject(fenceEvent_, 5000);
}

void Dx12Presenter::WaitIdle() {
    queue_->Signal(fence_.Get(), ++fenceValue_);
    WaitFor(fenceValue_);
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Presenter::SrvCpu(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(index) * srvStride_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Presenter::SrvGpu(UINT index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * srvStride_;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Presenter::RtvCpu(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(index) * rtvStride_;
    return h;
}

void Dx12Presenter::WritePanelDescriptors() {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < frames_.size(); ++i) {
        device_->CreateShaderResourceView(panelTexture_.Get(), &srv, SrvCpu(i * 2 + 1));  // null view until the panel exists
    }
}

// Records the panel's new pixels into list_. Panel changes are rare (key presses), so the upload
// buffer is simply reused after waiting for the GPU.
void Dx12Presenter::UploadPanel(bool changed) {
    if (osd_.Pixels().empty() || (!changed && panelTexture_)) return;
    const UINT w = osd_.Width(), h = osd_.Height();
    WaitIdle();

    D3D12_RESOURCE_DESC desc = panelTexture_ ? panelTexture_->GetDesc() : D3D12_RESOURCE_DESC{};
    if (!panelTexture_ || desc.Width != w || desc.Height != h) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc = Texture2DDesc(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        panelTexture_.Reset();
        panelUpload_.Reset();
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&panelTexture_)))) {
            return;
        }
        panelState_ = D3D12_RESOURCE_STATE_COPY_DEST;
        UINT64 uploadSize = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = uploadSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc = {1, 0};
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&panelUpload_)))) {
            panelTexture_.Reset();
            return;
        }
        WritePanelDescriptors();
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    uint8_t* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(panelUpload_->Map(0, &none, reinterpret_cast<void**>(&mapped)))) return;
    const uint32_t* src = osd_.Pixels().data();
    for (UINT y = 0; y < h; ++y) memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, src + size_t(y) * w, size_t(w) * 4);
    panelUpload_->Unmap(0, nullptr);

    if (panelState_ != D3D12_RESOURCE_STATE_COPY_DEST) {
        const D3D12_RESOURCE_BARRIER b = Transition(panelTexture_.Get(), panelState_, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{panelTexture_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION srcLoc{panelUpload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    srcLoc.PlacedFootprint = footprint;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    const D3D12_RESOURCE_BARRIER b = Transition(panelTexture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &b);
    panelState_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void Dx12Presenter::Render(IDXGISwapChain* swapChain) {
    const Config cfg = BeginFrame();
    if (!active_ || frames_.empty()) return;
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    if (index >= frames_.size() || !frames_[index].proxy) return;
    Frame& f = frames_[index];
    ComPtr<ID3D12Resource> real;
    if (FAILED(sc3->GetBuffer(index, IID_PPV_ARGS(&real)))) return;

    WaitFor(f.fenceValue);
    if (cfg.sceneAdaptation > 0.0f) CollectSceneAnalysis();
    if (FAILED(f.allocator->Reset()) || FAILED(list_->Reset(f.allocator.Get(), pipeline_.Get()))) return;
    if (osd_.Visible()) UploadPanel(UpdatePanel(cfg));
    const ShaderParams p = FrameParams(cfg, panelTexture_ != nullptr);

    // The game leaves its back buffer in PRESENT (== COMMON) before calling Present.
    D3D12_RESOURCE_BARRIER before[2] = {
        Transition(f.proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(real.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list_->ResourceBarrier(2, before);
    if (cfg.sceneAdaptation > 0.0f) RecordSceneAnalysis(index, cfg);

    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    // RTV descriptors are read when OMSetRenderTargets is recorded, so rewriting the slot every frame is safe.
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvCpu(index);
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = OutputFormat(output_);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(real.Get(), &rtvDesc, rtv);
    list_->SetPipelineState(pipeline_.Get());
    list_->SetGraphicsRootSignature(rootSignature_.Get());
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRoot32BitConstants(0, kParamCount, &p, 0);
    list_->SetGraphicsRootDescriptorTable(1, SrvGpu(index * 2));
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER after[2] = {
        Transition(f.proxy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(real.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT)};
    list_->ResourceBarrier(2, after);
    if (FAILED(list_->Close())) return;
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fenceValue_);
    f.fenceValue = fenceValue_;

    if (dumpPending_) {
        dumpPending_ = false;
        DumpFrame(index, real.Get());
    }
}

void Dx12Presenter::RecordSceneAnalysis(UINT index, const Config& cfg) {
    const SceneShaderParams sp = SceneParams(cfg);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(kSceneGridWidth), float(kSceneGridHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(kSceneGridWidth), LONG(kSceneGridHeight)};
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvCpu(static_cast<UINT>(frames_.size()));
    list_->SetPipelineState(scenePipeline_.Get());
    list_->SetGraphicsRootSignature(rootSignature_.Get());
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRoot32BitConstants(0, sizeof(sp) / 4, &sp, 0);
    list_->SetGraphicsRootDescriptorTable(1, SrvGpu(index * 2));
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list_->RSSetViewports(1, &viewport);
    list_->RSSetScissorRects(1, &scissor);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);

    const UINT slot = static_cast<UINT>(sceneWritten_ % kSceneReadbackLatency);
    D3D12_RESOURCE_BARRIER b = Transition(sceneTarget_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{sceneReadback_[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = sceneFootprint_;
    D3D12_TEXTURE_COPY_LOCATION src{sceneTarget_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = Transition(sceneTarget_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &b);
    sceneFence_[slot] = fenceValue_ + 1;  // the value Render signals after executing this list
    ++sceneWritten_;
}

// Reads the oldest slot of the ring if the GPU has finished it (it is about to be reused); never waits.
void Dx12Presenter::CollectSceneAnalysis() {
    if (sceneWritten_ < kSceneReadbackLatency) return;
    const UINT slot = static_cast<UINT>(sceneWritten_ % kSceneReadbackLatency);
    if (fence_->GetCompletedValue() < sceneFence_[slot]) return;
    void* data = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(sceneFootprint_.Offset + sceneFootprint_.Footprint.RowPitch * kSceneGridHeight)};
    if (FAILED(sceneReadback_[slot]->Map(0, &range, &data))) return;
    OnSceneMeasured(ReduceSceneGrid(reinterpret_cast<const float*>(static_cast<uint8_t*>(data) + sceneFootprint_.Offset),
                                    sceneFootprint_.Footprint.RowPitch / sizeof(float)));
    const D3D12_RANGE none{0, 0};
    sceneReadback_[slot]->Unmap(0, &none);
}

void Dx12Presenter::DumpFrame(UINT index, ID3D12Resource* real) {
    WaitIdle();
    const std::wstring dir = DataDir() + L"\\dumps\\";
    DumpTexturePfm(device_.Get(), queue_.Get(), real, D3D12_RESOURCE_STATE_PRESENT, dir + L"output.pfm");
    DumpTexturePfm(device_.Get(), queue_.Get(), frames_[index].proxy.Get(), D3D12_RESOURCE_STATE_COMMON, dir + L"input.pfm");
}

}  // namespace openhdr
