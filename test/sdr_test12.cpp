// Minimal D3D12 SDR renderer that exercises OpenHDR's injected mode without a real game.
//
//   sdr_test12 [--frames N] [--resize-test] [--srgb-rtv] [--null-rtv] [--unknown-rtv] [--load-library DLL] [--crash-after N] [--fake-fg]
//
// Flip-model R8G8B8A8_UNORM swap chain with 3 buffers, drawing the same pattern as sdr_test.
// --srgb-rtv: _SRGB render target views of the UNORM back buffers (what engines in linear colour
// space do). --null-rtv: render target views created without a description (format from the
// resource). --unknown-rtv: descriptions with DXGI_FORMAT_UNKNOWN, as Unreal Engine creates them.
// With OpenHDR active, the printed descriptions must still show the SDR format.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "g_PatternPS50.h"
#include "g_PatternVS50.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kBufferCount = 3;
bool g_resized = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            g_resized = true;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Fail(const char* what, HRESULT hr) {
    printf("FAILED: %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    return 1;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

}  // namespace

// --fake-fg: imitates a frame generation layer such as XeFG through OptiScaler. Hooked into the DXGI
// factory before OpenHDR loads, it creates the swap chain it presents to from inside the game's
// CreateSwapChainForHwnd (a nested call through the factory) and hands that one back to the game.
namespace fakefg {

using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
CreateHwndFn g_original = nullptr;
thread_local bool t_inside = false;
int g_nestedCreations = 0;

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* d, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    if (t_inside) return g_original(f, d, hwnd, desc, fs, o, out);  // our own nested call, when nothing else hooks
    t_inside = true;
    ++g_nestedCreations;
    // Through the vtable again, as frame generation libraries do with a factory of their own.
    const HRESULT hr = f->CreateSwapChainForHwnd(d, hwnd, desc, fs, o, out);
    t_inside = false;
    return hr;
}

void Install() {
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return;
    void** vtable = *reinterpret_cast<void***>(factory.Get());
    DWORD old;
    VirtualProtect(&vtable[15], sizeof(void*), PAGE_READWRITE, &old);
    g_original = reinterpret_cast<CreateHwndFn>(vtable[15]);
    vtable[15] = reinterpret_cast<void*>(&CreateSwapChainForHwnd);
    VirtualProtect(&vtable[15], sizeof(void*), old, &old);
    factory.Detach();  // the vtable belongs to dxgi.dll; keep it loaded
}

}  // namespace fakefg

// Animation clock. QPC, not GetTickCount64: that advances in ~15.6 ms steps, so at high frame rates
// the pattern would move only every few frames and stutter on its own.
float SecondsSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return float(double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart));
}

int main(int argc, char** argv) {
    bool resizeTest = false, srgbRtv = false, nullRtv = false, unknownRtv = false;
    int maxFrames = 0;
    int crashAfter = 0;  // --crash-after N: an access violation after N frames (OpenHDR's crash log)
    bool fakeFg = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--resize-test")) resizeTest = true;
        else if (!strcmp(argv[i], "--srgb-rtv")) srgbRtv = true;
        else if (!strcmp(argv[i], "--null-rtv")) nullRtv = true;
        else if (!strcmp(argv[i], "--unknown-rtv")) unknownRtv = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--crash-after") && i + 1 < argc) crashAfter = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fake-fg")) fakeFg = true;
        // Loads a DLL before anything else, as a mod would (e.g. libxess_fg.dll for OpenHDR's XeFG detection).
        else if (!strcmp(argv[i], "--load-library") && i + 1 < argc) {
            if (!LoadLibraryA(argv[++i])) printf("LoadLibrary(%s) failed: %lu\n", argv[i], GetLastError());
        }
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // a crash must end the test, not open a dialog
    if (fakeFg) fakefg::Install();  // before the window: OpenHDR activates when it is created

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpenHDRSdrTest12";
    RegisterClassW(&wc);
    RECT rect{0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    // On the primary monitor: with several displays the default position may be on another one.
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"OpenHDR SDR test (D3D12)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 64,
                              64, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Fail("CreateDXGIFactory2", hr);
    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) return Fail("D3D12CreateDevice", hr);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return Fail("CreateCommandQueue", hr);

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc = {1, 0};
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = kBufferCount;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) return Fail("CreateSwapChainForHwnd", hr);
    ComPtr<IDXGISwapChain3> swapChain;
    sc1.As(&swapChain);
    DXGI_SWAP_CHAIN_DESC1 reported{};
    swapChain->GetDesc1(&reported);
    printf("Swap chain as seen by the app: %ux%u format %d, swap effect %d, %u buffer(s)\n", reported.Width, reported.Height,
           reported.Format, reported.SwapEffect, reported.BufferCount);

    // Pattern pipeline: root constants b0 = resolution, time.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 4;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errors;
    hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) return Fail("D3D12SerializeRootSignature", hr);
    ComPtr<ID3D12RootSignature> rootSignature;
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature.Get();
    pso.VS = {g_PatternVS50, sizeof(g_PatternVS50)};
    pso.PS = {g_PatternPS50, sizeof(g_PatternPS50)};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = srgbRtv ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pipeline;
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) return Fail("CreateGraphicsPipelineState", hr);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ComPtr<ID3D12CommandAllocator> allocators[kBufferCount];
    for (auto& a : allocators) device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), pipeline.Get(), IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;
    UINT64 frameFence[kBufferCount] = {};
    auto waitFor = [&](UINT64 value) {
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    };
    auto waitIdle = [&] {
        queue->Signal(fence.Get(), ++fenceValue);
        waitFor(fenceValue);
    };

    ComPtr<ID3D12Resource> backBuffers[kBufferCount];
    UINT width = 0, height = 0;
    auto createTargets = [&]() -> HRESULT {
        for (UINT i = 0; i < kBufferCount; ++i) {
            HRESULT r = swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i]));
            if (FAILED(r)) return r;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(i) * rtvStride;
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = unknownRtv ? DXGI_FORMAT_UNKNOWN : srgbRtv ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(backBuffers[i].Get(), nullRtv ? nullptr : &rd, handle);
        }
        const D3D12_RESOURCE_DESC d = backBuffers[0]->GetDesc();
        width = static_cast<UINT>(d.Width);
        height = d.Height;
        printf("Back buffer as seen by the app: %ux%u format %d\n", width, height, d.Format);
        return S_OK;
    };
    hr = createTargets();
    if (FAILED(hr)) return Fail("back buffers", hr);
    g_resized = false;

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    int frame = 0;
    MSG msg{};
    for (;;) {
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (g_resized) {
            g_resized = false;
            waitIdle();
            for (auto& b : backBuffers) b.Reset();
            hr = swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) return Fail("ResizeBuffers", hr);
            hr = createTargets();
            if (FAILED(hr)) return Fail("back buffers after resize", hr);
        }
        if (width == 0 || height == 0) continue;

        const UINT index = swapChain->GetCurrentBackBufferIndex();
        waitFor(frameFence[index]);
        allocators[index]->Reset();
        list->Reset(allocators[index].Get(), pipeline.Get());
        D3D12_RESOURCE_BARRIER toTarget = Transition(backBuffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toTarget);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(index) * rtvStride;
        const float constants[4] = {float(width), float(height), SecondsSince(start), 0.0f};
        const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
        list->SetGraphicsRootSignature(rootSignature.Get());
        list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER toPresent = Transition(backBuffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &toPresent);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        hr = swapChain->Present(1, 0);
        if (FAILED(hr)) return Fail("Present", hr);
        queue->Signal(fence.Get(), ++fenceValue);
        frameFence[index] = fenceValue;

        ++frame;
        if (resizeTest && frame == 30) {
            RECT r{0, 0, 1600, 900};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        if (maxFrames > 0 && frame >= maxFrames) break;
        if (crashAfter > 0 && frame >= crashAfter) *static_cast<volatile int*>(nullptr) = frame;
    }
    waitIdle();
    if (fakeFg) printf("Fake frame generation: %d nested swap chain creation(s)\n", fakefg::g_nestedCreations);
    printf("Rendered %d frame(s), exiting cleanly\n", frame);
    return 0;
}
