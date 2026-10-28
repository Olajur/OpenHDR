#include "capture.h"

#include "config.h"
#include "display.h"
#include "frame_dump.h"
#include "itm_params.h"
#include "osd.h"
#include "scene.h"

#include "g_ItmPS.h"
#include "g_ItmVS.h"

#include <avrt.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <thread>

namespace openhdr {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace {

constexpr wchar_t kOverlayClass[] = L"OpenHDR.CaptureOverlay";

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}


template <class T>
ComPtr<T> SurfaceInterface(const wgd::Direct3D11::IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<T> result;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&result)));
    return result;
}

}  // namespace

struct CaptureSession::Impl {
    Impl(HWND game, const std::wstring& exe) : game(game), config(exe), osd(config) {}
    ~Impl() {
        CloseHandle(frameEvent);
        CloseHandle(initDone);
    }

    void Run();
    bool Init();
    bool CreateOverlay();
    void Loop();
    bool TakeFrame(const wgc::Direct3D11CaptureFrame& frame);
    void Render();
    bool PollInput();
    void Dump(ID3D11Texture2D* output, UINT width, UINT height, int offsetX, int offsetY);
    void Cleanup();

    const HWND game;
    ConfigStore config;
    Osd osd;
    OsdTexture osdTexture;
    SceneAnalyzer11 sceneAnalyzer;
    SceneAdapter sceneAdapter;
    SceneLog sceneLog;

    std::thread thread;
    HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE initDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> initOk{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> alive{true};
    std::atomic<bool> closed{false};

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11Texture2D> input;  // latest captured frame, whole window (the capture surface itself, or a copy)
    ComPtr<ID3D11ShaderResourceView> inputSrv;
    UINT inputWidth = 0;
    UINT inputHeight = 0;
    // Reading the capture surface directly saves a full-frame copy per frame (and the time it takes to
    // reach the screen). The frame stays open while it is shown; views are cached per pool texture.
    wgc::Direct3D11CaptureFrame heldFrame{nullptr};
    std::vector<std::pair<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>>> surfaceViews;
    bool directRead = true;  // cleared if the capture surfaces can't be read by shaders
    // OPENHDR_CAPTURE_LEGACY=1: the earlier behaviour (copy every frame, normal priorities), for comparisons.
    const bool legacy = [] {
        wchar_t flag[4] = {};
        return GetEnvironmentVariableW(L"OPENHDR_CAPTURE_LEGACY", flag, 4) && flag[0] == L'1';
    }();
    static constexpr int kPoolBuffers = 3;  // one shown, one being captured, one spare
    ComPtr<ID3D11Texture2D> copyTexture;  // used when frames are copied
    ComPtr<ID3D11ShaderResourceView> copySrv;

    HWND overlay = nullptr;
    bool overlayShown = false;
    RECT overlayRect{};
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;

    wgd::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool framePool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize{};

    DisplayInfo display;
    float autoPeakNits = 1000.0f;
    bool compare = false;
    bool dumpPending = false;
    HotkeyWatcher hotkeys;
    uint64_t frameIndex = 0;
    // Diagnostics, logged every 5 seconds: frames received and shown; added latency (from the time the
    // captured frame was due on screen to the time our converted frame actually reached the screen, per
    // the swap chain's frame statistics), also in refreshes; spacing of our presents.
    void ReportPacing();
    // GPU time of our work per frame (scene analysis + conversion), from timestamp queries read a few
    // frames later without waiting.
    struct GpuTimer {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    } gpuTimers[4];
    uint32_t gpuTimerIndex = 0;
    GpuTimer* BeginGpuTimer();
    void EndGpuTimer(GpuTimer* timer);
    struct Pacing {
        uint32_t captured = 0, shown = 0, intervals = 0, measured = 0;
        uint32_t refreshes[3] = {};  // added latency of up to 1, 2, more refreshes
        // The game's own pacing, from the capture timestamps (when Windows showed each game frame):
        // how many refreshes each frame stayed on screen, and frames we never showed (a newer one came).
        uint32_t gameIntervals = 0, skipped = 0;
        uint32_t gameRefreshes[3] = {};  // on screen for 1, 2, 3+ refreshes
        double gameIntervalSum = 0, gameIntervalSqSum = 0, gameIntervalMax = 0;
        uint32_t gpuSamples = 0;
        double gpuSum = 0, gpuMax = 0;
        double latencySum = 0, latencyMax = 0, refreshMs = 0;
        double lastPresentMs = 0, intervalSum = 0, intervalSqSum = 0, intervalMax = 0;
        ULONGLONG since = 0;
    } pacing;
    int64_t presentCaptureTimes[16] = {};  // capture time per present count (modulo 16)
    UINT lastMeasuredPresent = 0;
    int64_t captureTime100ns = 0;  // when Windows composed the frame being shown (SystemRelativeTime)
    int64_t lastGameFrameTime100ns = 0;
    void CountGameFrame(int64_t time100ns);
    uint32_t loggedConfigVersion = 0;
};

void CaptureSession::Impl::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // Each frame has to make it from capture to the next composition (a few ms): don't let other work
    // on the system delay this thread. MMCSS "Games" is what games' own render threads use.
    DWORD mmcssTask = 0;
    HANDLE mmcss = legacy ? nullptr : AvSetMmThreadCharacteristicsW(L"Games", &mmcssTask);
    if (!legacy) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    try {
        initOk = Init();
        SetEvent(initDone);
        if (initOk) Loop();
    } catch (const winrt::hresult_error& e) {
        Log("Capture error 0x%08lX: %ls", static_cast<unsigned long>(e.code()), e.message().c_str());
    }
    SetEvent(initDone);
    alive = false;
    Cleanup();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    winrt::uninit_apartment();
}

bool CaptureSession::Impl::Init() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        Log("Capture: D3D11CreateDevice failed 0x%08lX", hr);
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    // Our GPU work is small (well under a millisecond) but on the critical path, and runs while the game
    // keeps the GPU busy: schedule it ahead of normal work (best effort). Queue at most one frame.
    const HRESULT priority = legacy ? S_FALSE : dxgiDevice->SetGPUThreadPriority(7);
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (!legacy && SUCCEEDED(device.As(&dxgiDevice1))) dxgiDevice1->SetMaximumFrameLatency(1);
    Log("Capture: GPU thread priority %s (0x%08lX)", SUCCEEDED(priority) ? "raised" : "not raised", priority);

    const Config cfg = config.Get();
    display = QueryDisplay(factory.Get(), game);
    if (!display.hdr && !cfg.forceHdr) {
        Log("Capture: Windows HDR is off on this display (ForceHDR=1 overrides)");
        return false;
    }
    autoPeakNits = AutoPeakNits(display.maxNits);

    winrt::check_hresult(device->CreateVertexShader(g_ItmVS, sizeof(g_ItmVS), nullptr, &vs));
    winrt::check_hresult(device->CreatePixelShader(g_ItmPS, sizeof(g_ItmPS), nullptr, &ps));
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ShaderParams);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    winrt::check_hresult(device->CreateBuffer(&cb, nullptr, &params));
    winrt::check_hresult(sceneAnalyzer.Init(device.Get()));

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(game, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    winrtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    if (legacy) directRead = false;
    poolSize = item.Size();
    framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolBuffers, poolSize);
    framePool.FrameArrived([this](auto&&, auto&&) { SetEvent(frameEvent); });
    item.Closed([this](auto&&, auto&&) {
        closed = true;
        SetEvent(frameEvent);
    });
    session = framePool.CreateCaptureSession(item);
    // The real mouse cursor is drawn above our window anyway; a yellow capture border would show on
    // the game. Both properties need newer Windows builds, hence the try.
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}
    // By default Windows delivers only every few compositor frames (48 of 144 Hz here); ask for every
    // frame the game shows. Windows 11 24H2 and later; older builds keep their default rate.
    try {
        session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{std::chrono::milliseconds(1)});
    } catch (...) {
        Log("Capture: MinUpdateInterval not supported by this Windows build");
    }

    if (!CreateOverlay()) return false;
    session.StartCapture();
    Log("Capture started for window %p (%dx%d)", game, poolSize.Width, poolSize.Height);
    return true;
}

bool CaptureSession::Impl::CreateOverlay() {
    static const ATOM overlayClass = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kOverlayClass;
        return RegisterClassW(&wc);
    }();
    // Topmost, never activated, and transparent to the mouse: clicks and keys go to the game.
    overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED |
                                  WS_EX_NOREDIRECTIONBITMAP,
                              MAKEINTATOM(overlayClass), L"OpenHDR", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!overlay) {
        Log("Capture: overlay window creation failed (%lu)", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&swapChain))) {
        Log("Capture: swap chain creation failed 0x%08lX", hr);
        return false;
    }
    swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

    ComPtr<IDXGIDevice> dxgiDevice;
    device.As(&dxgiDevice);
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp));
    if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(overlay, TRUE, &target);
    if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&visual);
    if (SUCCEEDED(hr)) hr = visual->SetContent(swapChain.Get());
    if (SUCCEEDED(hr)) hr = target->SetRoot(visual.Get());
    if (SUCCEEDED(hr)) hr = dcomp->Commit();
    if (FAILED(hr)) {
        Log("Capture: DirectComposition setup failed 0x%08lX", hr);
        return false;
    }
    return true;
}

void CaptureSession::Impl::Loop() {
    while (!stop) {
        // A short timeout: hotkeys are polled here, also while the window shows a still image (no new
        // frames), and a quick tap must not fall between two polls.
        MsgWaitForMultipleObjects(1, &frameEvent, FALSE, 16, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (stop) break;
        if (closed || !IsWindow(game)) {
            Log("Capture: game window closed");
            break;
        }
        // Only the newest frame matters.
        wgc::Direct3D11CaptureFrame frame{nullptr};
        for (auto next = framePool.TryGetNextFrame(); next; next = framePool.TryGetNextFrame()) {
            CountGameFrame(next.SystemRelativeTime().count());
            if (frame) {
                frame.Close();
                ++pacing.skipped;
            }
            frame = next;
        }
        if (frame) {
            captureTime100ns = frame.SystemRelativeTime().count();
            if (TakeFrame(frame)) {
                if (heldFrame) heldFrame.Close();  // the previous frame is no longer shown
                heldFrame = frame;
            } else {
                frame.Close();  // copied
            }
        }
        const bool inputChanged = PollInput();
        if (!input || (!frame && !inputChanged && !osd.Visible())) {
            continue;  // nothing new to show; with the panel open, redraw so it reacts to keys
        }
        Render();
    }
}

void CaptureSession::Impl::CountGameFrame(int64_t time100ns) {
    Pacing& p = pacing;
    ++p.captured;
    const double interval = (time100ns - lastGameFrameTime100ns) / 10000.0;
    lastGameFrameTime100ns = time100ns;
    if (interval <= 0 || interval > 250) return;  // first frame, or the game paused
    ++p.gameIntervals;
    p.gameIntervalSum += interval;
    p.gameIntervalSqSum += interval * interval;
    p.gameIntervalMax = std::max(p.gameIntervalMax, interval);
    if (p.refreshMs > 0) ++p.gameRefreshes[interval <= p.refreshMs * 1.5 ? 0 : interval <= p.refreshMs * 2.5 ? 1 : 2];
}

// Makes the frame the conversion's input. Returns true if it reads the capture surface directly (the
// caller keeps the frame open while it is shown), false if it was copied.
bool CaptureSession::Impl::TakeFrame(const wgc::Direct3D11CaptureFrame& frame) {
    const auto size = frame.ContentSize();
    ComPtr<ID3D11Texture2D> surface = SurfaceInterface<ID3D11Texture2D>(frame.Surface());
    const UINT w = static_cast<UINT>(std::max(size.Width, 1)), h = static_cast<UINT>(std::max(size.Height, 1));
    D3D11_TEXTURE2D_DESC sd;
    surface->GetDesc(&sd);

    if (size.Width != poolSize.Width || size.Height != poolSize.Height) {
        // The window was resized: new pool textures from now on. This frame is copied (below).
        poolSize = size;
        if (heldFrame) heldFrame.Close();
        heldFrame = nullptr;
        surfaceViews.clear();
        framePool.Recreate(winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolBuffers, poolSize);
    } else if (directRead) {
        ComPtr<ID3D11ShaderResourceView> view;
        for (const auto& cached : surfaceViews) {
            if (cached.first == surface.Get()) view = cached.second;
        }
        if (!view && (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || FAILED(device->CreateShaderResourceView(surface.Get(), nullptr, &view)))) {
            directRead = false;
            Log("Capture: frames can't be read directly (bind flags 0x%X); copying them instead", sd.BindFlags);
        }
        if (view) {
            if (surfaceViews.size() < 8) surfaceViews.emplace_back(surface.Get(), view);
            input = surface;
            inputSrv = view;
            inputWidth = std::min(w, sd.Width);
            inputHeight = std::min(h, sd.Height);
            return true;
        }
    }

    D3D11_TEXTURE2D_DESC cd{};
    if (copyTexture) copyTexture->GetDesc(&cd);
    if (!copyTexture || cd.Width != w || cd.Height != h) {
        copySrv.Reset();
        copyTexture.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        winrt::check_hresult(device->CreateTexture2D(&td, nullptr, &copyTexture));
        winrt::check_hresult(device->CreateShaderResourceView(copyTexture.Get(), nullptr, &copySrv));
    }
    input = copyTexture;
    inputSrv = copySrv;
    inputWidth = w;
    inputHeight = h;
    surface->GetDesc(&sd);
    const D3D11_BOX box{0, 0, 0, std::min(w, sd.Width), std::min(h, sd.Height), 1};
    context->CopySubresourceRegion(input.Get(), 0, 0, 0, 0, surface.Get(), 0, &box);
    return false;
}

// Hotkeys and the panel's keys, once per loop pass. True when something shown changed (redraw needed).
bool CaptureSession::Impl::PollInput() {
    HWND foreground = GetForegroundWindow();
    const bool focused = foreground == game || foreground == GetAncestor(game, GA_ROOT);
    const uint32_t pressed = hotkeys.Poll(config.Get().hotkeys);
    auto fired = [&](HotkeyAction a) { return focused && ((pressed >> static_cast<int>(a)) & 1); };
    if (fired(HotkeyAction::Conversion)) config.SetGameValue(L"Conversion", config.Get().conversion ? 0.0f : 1.0f);
    if (fired(HotkeyAction::Compare)) compare = !compare;
    if (fired(HotkeyAction::Dump)) dumpPending = true;
    const bool wasVisible = osd.Visible();
    osd.Update(focused, fired(HotkeyAction::Panel), autoPeakNits, compare);
    return (focused && pressed) || wasVisible != osd.Visible();
}

void CaptureSession::Impl::Render() {
    // Where the game's client area is on screen, and where it sits inside the captured image
    // (which covers the window's visible frame, title bar included for windowed games).
    RECT client{};
    GetClientRect(game, &client);
    POINT origin{0, 0};
    ClientToScreen(game, &origin);
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(game, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) GetWindowRect(game, &bounds);
    const UINT width = static_cast<UINT>(std::max<LONG>(client.right, 0));
    const UINT height = static_cast<UINT>(std::max<LONG>(client.bottom, 0));
    if (width < 16 || height < 16 || IsIconic(game)) {
        if (overlayShown) ShowWindow(overlay, SW_HIDE);
        overlayShown = false;
        return;
    }
    const RECT rect{origin.x, origin.y, origin.x + static_cast<LONG>(width), origin.y + static_cast<LONG>(height)};
    if (!EqualRect(&rect, &overlayRect)) {
        overlayRect = rect;
        SetWindowPos(overlay, HWND_TOPMOST, rect.left, rect.top, static_cast<int>(width), static_cast<int>(height), SWP_NOACTIVATE);
        winrt::check_hresult(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
        swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        Log("Capture: client area %ux%u at (%ld, %ld)", width, height, rect.left, rect.top);
    }

    ++frameIndex;
    if (frameIndex % 30 == 1) config.ReloadIfChanged();
    const Config cfg = config.Get();
    if (cfg.dumpFrame > 0 && frameIndex == static_cast<uint64_t>(cfg.dumpFrame)) dumpPending = true;
    if (osd.Visible()) osdTexture.Upload(device.Get(), context.Get(), osd, osd.Rasterize(height, cfg, autoPeakNits, compare));

    PassInfo pass;
    pass.autoPeakNits = autoPeakNits;
    pass.width = width;
    pass.height = height;
    pass.frameIndex = frameIndex;
    pass.inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    pass.compare = compare;
    pass.osdShown = osd.Visible() && osdTexture.srv;
    pass.osdWidth = osd.Width();
    pass.osdHeight = osd.Height();
    pass.inputOffsetX = std::max<LONG>(origin.x - bounds.left, 0);
    pass.inputOffsetY = std::max<LONG>(origin.y - bounds.top, 0);

    GpuTimer* gpuTimer = BeginGpuTimer();
    // Scene analysis of the client area (results arrive a few frames later).
    if (cfg.sceneAdaptation > 0.0f) {
        SceneShaderParams sp{};
        sp.inputOffset[0] = static_cast<float>(pass.inputOffsetX);
        sp.inputOffset[1] = static_cast<float>(pass.inputOffsetY);
        sp.inputSize[0] = static_cast<float>(std::min(width, inputWidth));
        sp.inputSize[1] = static_cast<float>(std::min(height, inputHeight));
        sp.gridSize[0] = static_cast<float>(kSceneGridWidth);
        sp.gridSize[1] = static_cast<float>(kSceneGridHeight);
        sp.decodeGamma = ContrastGamma(cfg.contrastLevel);
        sp.brightThreshold = cfg.shoulderStart;
        SceneStats measured;
        if (sceneAnalyzer.Measure(context.Get(), inputSrv.Get(), sp, measured)) sceneAdapter.Measured(measured);
        sceneAdapter.Advance();
        if (sceneAdapter.Valid()) pass.scene = &sceneAdapter.Smoothed();
    }
    pass.fullFrameNits = display.maxFullFrameNits;
    const ShaderParams p = BuildShaderParams(cfg, pass);
    if (pass.scene) sceneLog.Report(cfg, *pass.scene, p.peakWhite * 80.0f, p.shoulderStart);
    if (cfg.version != loggedConfigVersion) {
        loggedConfigVersion = cfg.version;
        Log("Capture output: peak %.0f nits, paper white %.0f nits, conversion %s", p.peakWhite * 80.0f, p.paperWhite * 80.0f,
            (p.flags & 1) ? "on" : "off");
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &p, sizeof(p));
        context->Unmap(params.Get(), 0);
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    winrt::check_hresult(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    ComPtr<ID3D11RenderTargetView> rtv;
    winrt::check_hresult(device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv));
    ID3D11ShaderResourceView* views[2] = {inputSrv.Get(), pass.osdShown ? osdTexture.srv.Get() : nullptr};
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, params.GetAddressOf());
    context->PSSetShaderResources(0, 2, views);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    EndGpuTimer(gpuTimer);
    ID3D11ShaderResourceView* nullViews[2] = {};
    context->PSSetShaderResources(0, 2, nullViews);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    if (dumpPending) {
        dumpPending = false;
        Dump(backBuffer.Get(), width, height, pass.inputOffsetX, pass.inputOffsetY);
    }
    swapChain->Present(0, 0);
    ReportPacing();
    if (!overlayShown) {
        ShowWindow(overlay, SW_SHOWNOACTIVATE);  // only once there is a converted frame to show
        overlayShown = true;
    }
}

CaptureSession::Impl::GpuTimer* CaptureSession::Impl::BeginGpuTimer() {
    GpuTimer& t = gpuTimers[gpuTimerIndex++ % std::size(gpuTimers)];
    if (!t.disjoint) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(device->CreateQuery(&qd, &t.disjoint))) return nullptr;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device->CreateQuery(&qd, &t.begin)) || FAILED(device->CreateQuery(&qd, &t.end))) return nullptr;
    }
    if (t.pending) {  // the result from a few frames ago
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 b, e;
        const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        if (context->GetData(t.disjoint.Get(), &dj, sizeof(dj), flags) != S_OK || context->GetData(t.begin.Get(), &b, sizeof(b), flags) != S_OK ||
            context->GetData(t.end.Get(), &e, sizeof(e), flags) != S_OK)
            return nullptr;  // not done yet: skip timing this frame
        t.pending = false;
        if (!dj.Disjoint && dj.Frequency && e > b) {
            const double ms = 1000.0 * double(e - b) / double(dj.Frequency);
            ++pacing.gpuSamples;
            pacing.gpuSum += ms;
            pacing.gpuMax = std::max(pacing.gpuMax, ms);
        }
    }
    context->Begin(t.disjoint.Get());
    context->End(t.begin.Get());
    return &t;
}

void CaptureSession::Impl::EndGpuTimer(GpuTimer* timer) {
    if (!timer) return;
    context->End(timer->end.Get());
    context->End(timer->disjoint.Get());
    timer->pending = true;
}

void CaptureSession::Impl::ReportPacing() {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double nowMs = 1000.0 * double(now.QuadPart) / double(frequency.QuadPart);
    Pacing& p = pacing;
    ++p.shown;
    UINT presentCount = 0;
    swapChain->GetLastPresentCount(&presentCount);
    presentCaptureTimes[presentCount % 16] = captureTime100ns;
    DXGI_FRAME_STATISTICS stats{};
    if (SUCCEEDED(swapChain->GetFrameStatistics(&stats)) && stats.PresentCount != lastMeasuredPresent &&
        presentCount - stats.PresentCount < 16) {
        // The latest present that reached the screen, and when (QPC). The captured frame's own time is
        // when Windows meant to show the game's frame.
        lastMeasuredPresent = stats.PresentCount;
        const double latency = 1000.0 * double(stats.SyncQPCTime.QuadPart) / double(frequency.QuadPart) -
                               presentCaptureTimes[stats.PresentCount % 16] / 10000.0;
        DCOMPOSITION_FRAME_STATISTICS composition{};
        if (SUCCEEDED(dcomp->GetFrameStatistics(&composition)) && composition.currentCompositionRate.Numerator)
            p.refreshMs = 1000.0 * composition.currentCompositionRate.Denominator / composition.currentCompositionRate.Numerator;
        if (latency >= 0 && latency < 1000) {
            ++p.measured;
            p.latencySum += latency;
            p.latencyMax = std::max(p.latencyMax, latency);
            if (p.refreshMs > 0) ++p.refreshes[latency <= p.refreshMs * 1.5 ? 0 : latency <= p.refreshMs * 2.5 ? 1 : 2];
        }
    }
    if (p.lastPresentMs > 0) {
        const double interval = nowMs - p.lastPresentMs;
        ++p.intervals;
        p.intervalSum += interval;
        p.intervalSqSum += interval * interval;
        p.intervalMax = std::max(p.intervalMax, interval);
    }
    p.lastPresentMs = nowMs;

    const ULONGLONG tick = GetTickCount64();
    if (!p.since) p.since = tick;
    if (tick - p.since < 5000) return;
    const double seconds = (tick - p.since) / 1000.0;
    const double mean = p.intervals ? p.intervalSum / p.intervals : 0;
    const double jitter = p.intervals ? std::sqrt(std::max(p.intervalSqSum / p.intervals - mean * mean, 0.0)) : 0;
    const double n = std::max(p.measured, 1u);
    Log("Capture: %.0f frames/s captured, %.0f shown; added latency %.1f ms avg (%.1f max, %u samples), refreshes 1/2/3+: %.0f/%.0f/%.0f%%; "
        "frame interval %.2f ms avg, %.2f ms jitter, %.1f ms max",
        p.captured / seconds, p.shown / seconds, p.latencySum / n, p.latencyMax, p.measured, 100 * p.refreshes[0] / n,
        100 * p.refreshes[1] / n, 100 * p.refreshes[2] / n, mean, jitter, p.intervalMax);
    Log("Capture: our GPU work %.2f ms avg (%.2f max) per frame", p.gpuSamples ? p.gpuSum / p.gpuSamples : 0.0, p.gpuMax);
    const double g = std::max(p.gameIntervals, 1u);
    const double gameMean = p.gameIntervalSum / g;
    Log("Capture: game frames %.2f ms avg, %.2f ms jitter, %.1f ms max; on screen for 1/2/3+ refreshes: %.0f/%.0f/%.0f%%; "
        "%u not shown (a newer one arrived first)",
        gameMean, std::sqrt(std::max(p.gameIntervalSqSum / g - gameMean * gameMean, 0.0)), p.gameIntervalMax,
        100 * p.gameRefreshes[0] / g, 100 * p.gameRefreshes[1] / g, 100 * p.gameRefreshes[2] / g, p.skipped);
    const double last = p.lastPresentMs, refresh = p.refreshMs;
    p = Pacing{};
    p.lastPresentMs = last;
    p.refreshMs = refresh;
    p.since = tick;
}

void CaptureSession::Impl::Dump(ID3D11Texture2D* output, UINT width, UINT height, int offsetX, int offsetY) {
    const std::wstring dir = DataDir() + L"\\dumps\\";
    DumpTexturePfm(device.Get(), context.Get(), output, dir + L"output.pfm");
    // The input cropped to the client area, so it lines up with the output pixel for pixel.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> cropped;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &cropped))) return;
    const D3D11_BOX box{static_cast<UINT>(offsetX), static_cast<UINT>(offsetY), 0, std::min(offsetX + width, inputWidth),
                        std::min(offsetY + height, inputHeight), 1};
    context->CopySubresourceRegion(cropped.Get(), 0, 0, 0, 0, input.Get(), 0, &box);
    DumpTexturePfm(device.Get(), context.Get(), cropped.Get(), dir + L"input.pfm");
}

void CaptureSession::Impl::Cleanup() {
    try {
        if (heldFrame) heldFrame.Close();
        heldFrame = nullptr;
        surfaceViews.clear();
        inputSrv.Reset();
        input.Reset();
        if (session) session.Close();
        if (framePool) framePool.Close();
    } catch (...) {
    }
    session = nullptr;
    framePool = nullptr;
    item = nullptr;
    winrtDevice = nullptr;
    visual.Reset();
    target.Reset();
    dcomp.Reset();
    swapChain.Reset();
    if (overlay) DestroyWindow(overlay);
    overlay = nullptr;
    Log("Capture stopped for window %p", game);
}

std::unique_ptr<CaptureSession> CaptureSession::Start(HWND game, const std::wstring& gameExe) {
    auto impl = std::make_unique<Impl>(game, gameExe);
    Impl* raw = impl.get();
    raw->thread = std::thread([raw] { raw->Run(); });
    WaitForSingleObject(raw->initDone, 5000);
    if (!raw->initOk) {
        raw->stop = true;
        SetEvent(raw->frameEvent);
        raw->thread.join();
        return nullptr;
    }
    return std::unique_ptr<CaptureSession>(new CaptureSession(std::move(impl)));
}

CaptureSession::CaptureSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CaptureSession::~CaptureSession() {
    impl_->stop = true;
    SetEvent(impl_->frameEvent);
    if (impl_->thread.joinable()) impl_->thread.join();
}

HWND CaptureSession::Game() const { return impl_->game; }

bool CaptureSession::Alive() const { return impl_->alive; }

}  // namespace openhdr
