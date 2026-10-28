// Minimal D3D11 SDR renderer that exercises OpenHDR without a real game.
//
//   sdr_test [--flip] [--frames N] [--resize-test] [--gpu-load N]
//
// Default: legacy blt-model swap chain (DXGI_SWAP_EFFECT_DISCARD, R8G8B8A8_UNORM, 1 buffer), the
// case OpenHDR has to convert to flip model. --flip: CreateSwapChainForHwnd with B8G8R8A8 flip model.
// With OpenHDR.exe running and sdr_test.exe in its game list, the printed swap chain description
// must still show the SDR format: the game side never sees the FP16 buffers.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "g_PatternPS.h"
#include "g_PatternVS.h"

using Microsoft::WRL::ComPtr;

namespace {

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

struct FrameConstants {
    float resolution[2];
    float time;
    float pad;
};

int Fail(const char* what, HRESULT hr) {
    printf("FAILED: %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    return 1;
}

}  // namespace

// --hostile-hooks: imitates overlays such as Steam's, which patch DXGI vtables and re-patch whenever
// they find their hook replaced, saving whatever was there as their "original". Two hookers that both
// do this on one slot call each other forever unless one of them breaks the cycle.
namespace hostile {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
PresentFn g_present = nullptr;
CreateFn g_create = nullptr;
CreateHwndFn g_createHwnd = nullptr;
int g_presentCalls = 0;
IDXGIFactory2* g_factory = nullptr;  // any factory: they all share one vtable

template <class Fn>
void Patch(void* object, int index, Fn hook, Fn* orig) {
    void** vtable = *static_cast<void***>(object);
    if (vtable[index] == reinterpret_cast<void*>(hook)) return;
    DWORD old;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old);
    *orig = reinterpret_cast<Fn>(vtable[index]);
    vtable[index] = reinterpret_cast<void*>(hook);
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ++g_presentCalls;
    PresentFn next = g_present;
    Patch(sc, 8, &Present, &g_present);
    return next(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

void Rehook() {
    Patch(g_factory, 10, &CreateSwapChain, &g_create);
    Patch(g_factory, 15, &CreateSwapChainForHwnd, &g_createHwnd);
}

void InstallFactoryHooks(IDXGIFactory2* factory) {
    g_factory = factory;
    g_factory->AddRef();  // kept for the process lifetime
    Rehook();
}

HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory* f, IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    HRESULT hr = g_create(f, d, desc, out);
    Rehook();  // re-hook if someone replaced us
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* d, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = g_createHwnd(f, d, hwnd, desc, fs, o, out);
    Rehook();
    return hr;
}

void HookSwapChain(IDXGISwapChain* sc) { Patch(sc, 8, &Present, &g_present); }

}  // namespace hostile

// Animation clock. QPC, not GetTickCount64: that advances in ~15.6 ms steps, so at high frame rates
// the pattern would move only every few frames and stutter on its own.
float SecondsSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return float(double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart));
}

int main(int argc, char** argv) {
    bool flip = false;
    bool resizeTest = false;
    bool unitySequence = false;
    // V Rising (Unity 2022, exclusive fullscreen): a second, blt-model _SRGB chain is created on the
    // same window while the first flip-model chain stays alive, then made fullscreen and presented.
    bool vrisingSequence = false;
    bool hostileHooks = false;
    bool sceneTest = false;  // alternate a dark and a bright scene every 1.5 s (scene adaptation)
    bool srgbRtv = false;  // like Unity in linear colour space: an _SRGB view of the UNORM back buffer
    int maxFrames = 0;
    int gpuLoad = 1;  // draws per frame: keeps the GPU busy like a game (capture-mode pacing tests)
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--flip")) flip = true;
        else if (!strcmp(argv[i], "--resize-test")) resizeTest = true;
        else if (!strcmp(argv[i], "--unity")) unitySequence = flip = true;
        else if (!strcmp(argv[i], "--vrising")) vrisingSequence = unitySequence = flip = true;
        else if (!strcmp(argv[i], "--hostile-hooks")) hostileHooks = true;
        else if (!strcmp(argv[i], "--scene-test")) sceneTest = true;
        else if (!strcmp(argv[i], "--srgb-rtv")) srgbRtv = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-load") && i + 1 < argc) gpuLoad = (std::max)(atoi(argv[++i]), 1);
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // a crash must end the test, not open a dialog

    if (hostileHooks) {
        // Like an overlay injected at process start: hooks before OpenHDR (which activates on window creation).
        ComPtr<IDXGIFactory2> early;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&early)))) return Fail("CreateDXGIFactory1", E_FAIL);
        hostile::InstallFactoryHooks(early.Get());
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpenHDRSdrTest";
    RegisterClassW(&wc);
    RECT rect{0, 0, 1280, 720};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    // On the primary monitor: with several displays the default position may be on another one.
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"OpenHDR SDR test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 64,
                              64, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, &context);
    if (FAILED(hr)) return Fail("D3D11CreateDevice", hr);

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Fail("GetParent(IDXGIFactory2)", hr);

    ComPtr<IDXGISwapChain> swapChain;
    // fullscreen: legacy blt-model chain created in exclusive fullscreen at the desktop resolution.
    auto createSwapChain = [&](bool flipModel, bool fullscreen) -> HRESULT {
        HRESULT r;
        if (flipModel) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Format = unitySequence ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = {1, 0};
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> sc1;
            r = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &sc1);
            swapChain = sc1;
        } else {
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (fullscreen) {
                MONITORINFO mi{sizeof(mi)};
                GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
                desc.BufferDesc.Width = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
                desc.BufferDesc.Height = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
            }
            desc.SampleDesc = {1, 0};
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = fullscreen ? 2 : 1;
            desc.OutputWindow = hwnd;
            desc.Windowed = fullscreen ? FALSE : TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            r = factory->CreateSwapChain(device.Get(), &desc, &swapChain);
        }
        if (FAILED(r)) return r;
        if (hostileHooks) hostile::HookSwapChain(swapChain.Get());
        DXGI_SWAP_CHAIN_DESC reported{};
        swapChain->GetDesc(&reported);
        printf("Swap chain as seen by the app: %ux%u format %d, swap effect %d, %u buffer(s), windowed %d\n",
               reported.BufferDesc.Width, reported.BufferDesc.Height, reported.BufferDesc.Format, reported.SwapEffect,
               reported.BufferCount, reported.Windowed);
        return S_OK;
    };
    hr = createSwapChain(flip, false);
    if (FAILED(hr)) return Fail("CreateSwapChain", hr);

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    device->CreateVertexShader(g_PatternVS, sizeof(g_PatternVS), nullptr, &vs);
    device->CreatePixelShader(g_PatternPS, sizeof(g_PatternPS), nullptr, &ps);
    D3D11_BUFFER_DESC cbDesc{sizeof(FrameConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    device->CreateBuffer(&cbDesc, nullptr, &cb);

    ComPtr<ID3D11RenderTargetView> rtv;
    UINT width = 0, height = 0;
    auto createTarget = [&]() -> HRESULT {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT r = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(r)) return r;
        D3D11_TEXTURE2D_DESC td;
        backBuffer->GetDesc(&td);
        width = td.Width;
        height = td.Height;
        printf("Back buffer as seen by the app: %ux%u format %d\n", td.Width, td.Height, td.Format);
        if (!srgbRtv) return device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = flip && !unitySequence ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        return device->CreateRenderTargetView(backBuffer.Get(), &rd, &rtv);
    };
    hr = createTarget();
    if (FAILED(hr)) return Fail("back buffer RTV", hr);
    g_resized = false;

    ComPtr<IDXGISwapChain> vrisingFirst;
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
            rtv.Reset();
            context->ClearState();
            hr = swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) return Fail("ResizeBuffers", hr);
            hr = createTarget();
            if (FAILED(hr)) return Fail("back buffer RTV after resize", hr);
        }
        if (width == 0 || height == 0) continue;  // minimized

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const float seconds = SecondsSince(start);
            const float mode = sceneTest ? (int(seconds / 1.5f) % 2 == 0 ? 1.0f : 2.0f) : 0.0f;
            FrameConstants fc{{float(width), float(height)}, seconds, mode};
            memcpy(mapped.pData, &fc, sizeof(fc));
            context->Unmap(cb.Get(), 0);
        }
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        for (int i = 0; i < gpuLoad; ++i) context->Draw(3, 0);

        hr = swapChain->Present(1, 0);
        if (FAILED(hr)) return Fail("Present", hr);
        ++frame;
        if (resizeTest && frame == 30) {
            RECT r{0, 0, 1600, 900};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
        if (vrisingSequence && frame == 30) {
            ComPtr<IDXGISwapChain> first = swapChain;  // stays alive, as in V Rising
            rtv.Reset();
            context->ClearState();
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
            desc.Height = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc.SampleDesc = {1, 0};
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 1;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            ComPtr<IDXGISwapChain1> second;
            hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, &second);
            if (FAILED(hr)) return Fail("second swap chain", hr);
            hr = second->SetFullscreenState(TRUE, nullptr);
            printf("Second chain: SetFullscreenState(TRUE) = 0x%08lX\n", static_cast<unsigned long>(hr));
            hr = second->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);  // what games do after going fullscreen
            if (FAILED(hr)) return Fail("second chain ResizeBuffers", hr);
            swapChain = second;
            hr = createTarget();
            if (FAILED(hr)) return Fail("back buffer RTV (second chain)", hr);
            g_resized = false;
            vrisingFirst = first;
        } else if (unitySequence && frame == 30) {
            // What Unity does for SetResolution(..., ExclusiveFullScreen): drop the windowed flip-model
            // chain and create a blt-model chain in exclusive fullscreen on the same window.
            rtv.Reset();
            context->ClearState();
            context->Flush();
            swapChain.Reset();
            hr = createSwapChain(false, true);
            if (FAILED(hr)) return Fail("CreateSwapChain (exclusive fullscreen)", hr);
            hr = createTarget();
            if (FAILED(hr)) return Fail("back buffer RTV (exclusive fullscreen)", hr);
            g_resized = false;
        }
        if (maxFrames > 0 && frame >= maxFrames) break;
    }
    if (hostileHooks) printf("Hostile overlay Present hook ran %d time(s)\n", hostile::g_presentCalls);
    BOOL fullscreen = FALSE;
    swapChain->GetFullscreenState(&fullscreen, nullptr);
    if (fullscreen) swapChain->SetFullscreenState(FALSE, nullptr);  // required before releasing a fullscreen chain
    printf("Rendered %d frame(s), exiting cleanly\n", frame);
    return 0;
}
