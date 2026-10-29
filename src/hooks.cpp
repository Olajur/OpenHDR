// Swap chain creation hooks on the DXGI factory vtable.
//
// Upgraded swap chains are created as R16G16B16A16_FLOAT flip-model chains in scRGB (or HDR10, see
// ChooseOutputMode) and handed to the game inside a SwapChainWrapper, so the real swap chain's vtable is never touched.
//
// The two factory slots are patched once per vtable and never re-patched. Overlays such as Steam's
// patch DXGI vtables too, and re-patch when they find their hook replaced, saving ours as their
// "original". When that makes a call come back into our hook on the same thread, we call DXGI's own
// implementation (the slot's value in dxgi.dll on disk) instead of the saved original, which ends
// the cycle instead of recursing until the stack overflows.
#include "hooks.h"

#include "config.h"
#include "display.h"
#include "dx11_presenter.h"
#include "dx12_presenter.h"
#include "swapchain.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace openhdr {
namespace {

// Vtable slots. IUnknown is 0-2, IDXGIObject 3-6.
constexpr int kFactory_CreateSwapChain = 10;
constexpr int kFactory2_CreateSwapChainForHwnd = 15;

using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

// The value a vtable slot had when its module was loaded: the pointer stored in the file on disk,
// rebased to where the module is loaded now. Null if it can't be determined.
void* PristineSlot(void** slot) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(slot), &module)) {
        return nullptr;
    }
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return nullptr;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    std::vector<uint8_t> image;
    DWORD read = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        image.resize(static_cast<size_t>(size.QuadPart));
        if (!ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &read, nullptr) || read != image.size()) image.clear();
    }
    CloseHandle(file);
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return nullptr;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size()) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;

    const uint64_t base = reinterpret_cast<uint64_t>(module);
    const uint64_t rva = reinterpret_cast<uint64_t>(slot) - base;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (rva < section->VirtualAddress || rva + 8 > uint64_t(section->VirtualAddress) + section->SizeOfRawData) continue;
        const uint64_t offset = section->PointerToRawData + (rva - section->VirtualAddress);
        if (offset + 8 > image.size()) return nullptr;
        uint64_t value;
        memcpy(&value, image.data() + offset, 8);
        const uint64_t rebased = value - nt->OptionalHeader.ImageBase + base;
        // Must point back into the same module, or it isn't a code pointer we understand.
        return rebased >= base && rebased < base + nt->OptionalHeader.SizeOfImage ? reinterpret_cast<void*>(rebased) : nullptr;
    }
    return nullptr;
}

// One hooked vtable slot. Different DXGI classes (e.g. debug-layer factories) can have their own
// vtables, so originals are tracked per vtable and looked up from the called object.
struct SlotHook {
    struct Entry {
        void** vtable;
        void* original;  // what the slot held when we patched it (may be another overlay's hook)
        void* pristine;  // DXGI's own function, used when a call loops back into us
    };
    static constexpr int kMax = 8;
    Entry entries[kMax] = {};
    std::atomic<int> count{0};

    const Entry* Find(void* object) const {
        void** vtable = *static_cast<void***>(object);
        const int n = count.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) {
            if (entries[i].vtable == vtable) return &entries[i];
        }
        return nullptr;
    }
};

SRWLOCK g_hookLock = SRWLOCK_INIT;
SlotHook s_CreateSwapChain, s_CreateSwapChainForHwnd;

void Install(SlotHook& slot, void* object, int index, void* hook) {
    AcquireSRWLockExclusive(&g_hookLock);
    void** vtable = *static_cast<void***>(object);
    const int n = slot.count.load(std::memory_order_relaxed);
    // Once per vtable, ever: re-patching a slot someone else hooked after us is what creates cycles.
    const bool known = slot.Find(object) != nullptr;
    if (!known && n < SlotHook::kMax && vtable[index] != hook) {
        void* pristine = PristineSlot(&vtable[index]);
        DWORD oldProtect;
        if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            slot.entries[n] = {vtable, vtable[index], pristine ? pristine : vtable[index]};
            slot.count.store(n + 1, std::memory_order_release);  // publish before the hook can run
            InterlockedExchangePointer(&vtable[index], hook);
            VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
            if (slot.entries[n].original != slot.entries[n].pristine) {
                Log("Vtable slot %d was already hooked by another module; chaining to it", index);
            }
        } else {
            Log("VirtualProtect failed for vtable slot %d (error %lu)", index, GetLastError());
        }
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}

// DXGI calls its own public methods internally, and other hooks may call back into ours: only the
// outermost call on a thread is handled, nested ones go straight to DXGI.
thread_local int t_createDepth = 0;
thread_local int t_viewDepth = 0;

struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
};

// The function to call after our hook: the slot's previous occupant, or the module's own function
// when this is a nested call (depth > 0).
template <class Fn>
Fn Next(const SlotHook& slot, void* object, int depth) {
    const SlotHook::Entry* entry = slot.Find(object);
    if (!entry) return nullptr;
    return reinterpret_cast<Fn>(depth > 0 ? entry->pristine : entry->original);
}

// ---- D3D11 view hooks ---------------------------------------------------------
//
// Proxy back buffers are typeless. A view created on one without a format (or with the typeless
// format the proxy reports from GetDesc) gets the game's back buffer format, as it would on a real
// swap chain buffer. Every other resource passes through untouched.

constexpr int kDevice_CreateShaderResourceView = 7;
constexpr int kDevice_CreateUnorderedAccessView = 8;
constexpr int kDevice_CreateRenderTargetView = 9;

using PFN_CreateSRV = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
using PFN_CreateUAV = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**);
using PFN_CreateRTV = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);

SlotHook s_CreateSRV, s_CreateUAV, s_CreateRTV;

// Diagnostics: one log line the first time a view of a proxy gets its format filled in.
void LogViewFixup(const char* api, DXGI_FORMAT format) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log("%s: filled in format %d for a view of a proxy back buffer", api, format);
}

bool IsTypeless(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
}

// Whether a view of a proxy needs the game's format: no description, DXGI_FORMAT_UNKNOWN ("the
// resource's format", e.g. Unreal Engine's back buffer view; on a typeless proxy that is invalid),
// or the typeless format the proxy reports.
bool NeedsProxyFormat(const DXGI_FORMAT* requested) {
    return !requested || *requested == DXGI_FORMAT_UNKNOWN || IsTypeless(*requested);
}

// The game's back buffer format if `resource` is a proxy back buffer and the view needs it filled in.
DXGI_FORMAT ProxyViewFormat(ID3D11Resource* resource, const DXGI_FORMAT* requested, bool& multisampled) {
    if (!resource || !NeedsProxyFormat(requested)) return DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT size = sizeof(format);
    if (FAILED(resource->GetPrivateData(kProxyFormatGuid, &size, &format)) || size != sizeof(format)) return DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) texture->GetDesc(&desc);
    multisampled = desc.SampleDesc.Count > 1;
    return format;
}

HRESULT STDMETHODCALLTYPE Hook_CreateRenderTargetView(ID3D11Device* device, ID3D11Resource* resource,
                                                      const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out) {
    auto next = Next<PFN_CreateRTV>(s_CreateRTV, device, t_viewDepth);
    DepthGuard guard(t_viewDepth);
    bool ms = false;
    const DXGI_FORMAT format = ProxyViewFormat(resource, desc ? &desc->Format : nullptr, ms);
    if (format == DXGI_FORMAT_UNKNOWN) return next(device, resource, desc, out);
    LogViewFixup("D3D11", format);
    D3D11_RENDER_TARGET_VIEW_DESC typed{};
    if (desc) {
        typed = *desc;
    } else {
        typed.ViewDimension = ms ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    }
    typed.Format = format;
    return next(device, resource, &typed, out);
}

HRESULT STDMETHODCALLTYPE Hook_CreateShaderResourceView(ID3D11Device* device, ID3D11Resource* resource,
                                                        const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    auto next = Next<PFN_CreateSRV>(s_CreateSRV, device, t_viewDepth);
    DepthGuard guard(t_viewDepth);
    bool ms = false;
    const DXGI_FORMAT format = ProxyViewFormat(resource, desc ? &desc->Format : nullptr, ms);
    if (format == DXGI_FORMAT_UNKNOWN) return next(device, resource, desc, out);
    LogViewFixup("D3D11", format);
    D3D11_SHADER_RESOURCE_VIEW_DESC typed{};
    if (desc) {
        typed = *desc;
    } else {
        typed.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        typed.Texture2D.MipLevels = 1;
    }
    typed.Format = format;
    return next(device, resource, &typed, out);
}

HRESULT STDMETHODCALLTYPE Hook_CreateUnorderedAccessView(ID3D11Device* device, ID3D11Resource* resource,
                                                         const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, ID3D11UnorderedAccessView** out) {
    auto next = Next<PFN_CreateUAV>(s_CreateUAV, device, t_viewDepth);
    DepthGuard guard(t_viewDepth);
    bool ms = false;
    const DXGI_FORMAT format = ProxyViewFormat(resource, desc ? &desc->Format : nullptr, ms);
    if (format == DXGI_FORMAT_UNKNOWN) return next(device, resource, desc, out);
    LogViewFixup("D3D11", format);
    D3D11_UNORDERED_ACCESS_VIEW_DESC typed{};
    if (desc) {
        typed = *desc;
    } else {
        typed.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    }
    typed.Format = format;
    return next(device, resource, &typed, out);
}

void InstallDeviceHooks(ID3D11Device* device) {
    Install(s_CreateSRV, device, kDevice_CreateShaderResourceView, reinterpret_cast<void*>(&Hook_CreateShaderResourceView));
    Install(s_CreateUAV, device, kDevice_CreateUnorderedAccessView, reinterpret_cast<void*>(&Hook_CreateUnorderedAccessView));
    Install(s_CreateRTV, device, kDevice_CreateRenderTargetView, reinterpret_cast<void*>(&Hook_CreateRenderTargetView));
}

// D3D12: same fix-up for ID3D12Device's view methods (they return nothing; views go into descriptors).
// ID3D12Object is 3-6, then GetNodeCount 7 ... CreateConstantBufferView 17.
constexpr int kDevice12_CreateShaderResourceView = 18;
constexpr int kDevice12_CreateUnorderedAccessView = 19;
constexpr int kDevice12_CreateRenderTargetView = 20;

using PFN_CreateSRV12 = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateUAV12 = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateRTV12 = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);

SlotHook s_CreateSRV12, s_CreateUAV12, s_CreateRTV12;

DXGI_FORMAT ProxyViewFormat12(ID3D12Resource* resource, const DXGI_FORMAT* requested) {
    if (!resource || !NeedsProxyFormat(requested)) return DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT size = sizeof(format);
    if (FAILED(resource->GetPrivateData(kProxyFormatGuid, &size, &format)) || size != sizeof(format)) return DXGI_FORMAT_UNKNOWN;
    return format;
}

void STDMETHODCALLTYPE Hook_CreateRenderTargetView12(ID3D12Device* device, ID3D12Resource* resource,
                                                     const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    auto next = Next<PFN_CreateRTV12>(s_CreateRTV12, device, t_viewDepth);
    DepthGuard guard(t_viewDepth);
    const DXGI_FORMAT format = ProxyViewFormat12(resource, desc ? &desc->Format : nullptr);
    if (format == DXGI_FORMAT_UNKNOWN) return next(device, resource, desc, dest);
    LogViewFixup("D3D12", format);
    D3D12_RENDER_TARGET_VIEW_DESC typed{};
    if (desc) {
        typed = *desc;
    } else {
        typed.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    }
    typed.Format = format;
    next(device, resource, &typed, dest);
}

void STDMETHODCALLTYPE Hook_CreateShaderResourceView12(ID3D12Device* device, ID3D12Resource* resource,
                                                       const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    auto next = Next<PFN_CreateSRV12>(s_CreateSRV12, device, t_viewDepth);
    DepthGuard guard(t_viewDepth);
    const DXGI_FORMAT format = ProxyViewFormat12(resource, desc ? &desc->Format : nullptr);
    if (format == DXGI_FORMAT_UNKNOWN) return next(device, resource, desc, dest);
    LogViewFixup("D3D12", format);
    D3D12_SHADER_RESOURCE_VIEW_DESC typed{};
    if (desc) {
        typed = *desc;
    } else {
        typed.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        typed.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        typed.Texture2D.MipLevels = 1;
    }
    typed.Format = format;
    next(device, resource, &typed, dest);
}

void STDMETHODCALLTYPE Hook_CreateUnorderedAccessView12(ID3D12Device* device, ID3D12Resource* resource, ID3D12Resource* counter,
                                                        const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    auto next = Next<PFN_CreateUAV12>(s_CreateUAV12, device, t_viewDepth);
    DepthGuard guard(t_viewDepth);
    const DXGI_FORMAT format = ProxyViewFormat12(resource, desc ? &desc->Format : nullptr);
    if (format == DXGI_FORMAT_UNKNOWN) return next(device, resource, counter, desc, dest);
    LogViewFixup("D3D12", format);
    D3D12_UNORDERED_ACCESS_VIEW_DESC typed{};
    if (desc) {
        typed = *desc;
    } else {
        typed.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    typed.Format = format;
    next(device, resource, counter, &typed, dest);
}

void InstallDevice12Hooks(ID3D12Device* device) {
    Install(s_CreateSRV12, device, kDevice12_CreateShaderResourceView, reinterpret_cast<void*>(&Hook_CreateShaderResourceView12));
    Install(s_CreateUAV12, device, kDevice12_CreateUnorderedAccessView, reinterpret_cast<void*>(&Hook_CreateUnorderedAccessView12));
    Install(s_CreateRTV12, device, kDevice12_CreateRenderTargetView, reinterpret_cast<void*>(&Hook_CreateRenderTargetView12));
}

// The graphics API behind a swap chain: D3D11 passes its device, D3D12 its command queue.
struct GameDevice {
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D12CommandQueue> d3d12Queue;
};

bool IsFlipModel(DXGI_SWAP_EFFECT effect) {
    return effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
}

// Intel XeFG frame generation (loaded e.g. by OptiScaler) gives the game a swap chain of its own, which
// the game then presents and resizes directly (also through the FFX "wrap swap chain" call). Wrapping the
// game's swap chain would leave OpenHDR bypassed; see Hook_CreateSwapChainForHwnd for what happens instead.
const wchar_t* LoadedFrameGenerationModule() {
    for (const wchar_t* module : {L"libxess_fg.dll", L"igxess_fg.dll"}) {
        if (GetModuleHandleW(module)) return module;
    }
    return nullptr;
}

// Decides whether a new swap chain gets upgraded; on success also returns its device and display.
bool ShouldUpgrade(IDXGIFactory* factory, IUnknown* device, HWND hwnd, DXGI_FORMAT format, UINT width, UINT height,
                   GameDevice& game, DisplayInfo& display) {
    const Config cfg = GetConfig();
    if (!cfg.enabled) {
        Log("Skipping swap chain: OpenHDR is disabled (tray icon / Enabled=0)");
        return false;
    }
    if ((width && width < 64) || (height && height < 64)) {
        // Overlays create tiny throwaway swap chains to find DXGI's vtable; they must see the real one.
        Log("Skipping swap chain: %ux%u is too small to be a game's (probably an overlay's probe)", width, height);
        return false;
    }
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&game.d3d11)))) {
        if (game.d3d11->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_0) {
            Log("Skipping swap chain: feature level 0x%X is below 10_0", game.d3d11->GetFeatureLevel());
            return false;
        }
    } else if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&game.d3d12Queue)))) {
        Log("Skipping swap chain: neither D3D11 nor D3D12 (D3D10 is not supported)");
        return false;
    }
    if (!IsConvertibleFormat(format)) {
        Log("Skipping swap chain: format %d is not an SDR back buffer format", format);
        return false;
    }
    display = QueryDisplay(factory, hwnd);
    if (!display.hdr && !cfg.forceHdr) {
        Log("Skipping swap chain: Windows HDR is off on this display (ForceHDR=1 overrides)");
        return false;
    }
    return true;
}

// The swap chain format for a game's new swap chain (OutputFormat in the ini; auto = FP16 scRGB).
OutputMode ChooseOutputMode() {
    return GetConfig().outputFormat == OutputFormatSetting::Hdr10 ? OutputMode::Hdr10 : OutputMode::ScRgb;
}

// Replaces a freshly created HDR swap chain in `out` with its wrapper. On failure `out` is left
// alone and the caller must discard the HDR chain.
template <class SwapChainInterface>
bool WrapSwapChain(SwapChainInterface** out, const GameDevice& device, const GameBufferDesc& game,
                   const DisplayInfo& display, HWND hwnd, OutputMode output) {
    ComPtr<IDXGISwapChain4> real;
    HRESULT hr = (*out)->QueryInterface(IID_PPV_ARGS(&real));
    std::unique_ptr<Presenter> presenter;
    // View hooks go in before the game can create views of the proxies.
    if (SUCCEEDED(hr) && device.d3d11) {
        InstallDeviceHooks(device.d3d11.Get());
        hr = Dx11Presenter::Create(device.d3d11.Get(), real.Get(), game, display, hwnd, output, presenter);
    } else if (SUCCEEDED(hr)) {
        ComPtr<ID3D12Device> device12;
        hr = device.d3d12Queue->GetDevice(IID_PPV_ARGS(&device12));
        if (SUCCEEDED(hr)) InstallDevice12Hooks(device12.Get());
        if (SUCCEEDED(hr)) hr = Dx12Presenter::Create(device.d3d12Queue.Get(), real.Get(), game, display, hwnd, output, presenter);
    }
    if (FAILED(hr)) {
        Log("Presenter setup failed: 0x%08lX", hr);
        return false;
    }
    ApplyOutputColorSpace(real.Get(), output);
    Log("Swap chain %p (%s) upgraded to %s (game format %d, %u sample(s), swap effect %d, %u buffer(s))",
        real.Get(), device.d3d11 ? "D3D11" : "D3D12", OutputName(output), game.format, game.sample.Count, game.swapEffect, game.bufferCount);
    (*out)->Release();  // the wrapper keeps its own reference
    *out = new SwapChainWrapper(std::move(real), std::move(presenter));
    return true;
}

// ---- factory hooks ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    auto next = Next<PFN_CreateSwapChain>(s_CreateSwapChain, factory, t_createDepth);
    if (t_createDepth > 0 || !desc || !out) return next(factory, device, desc, out);
    DepthGuard guard(t_createDepth);

    Log("CreateSwapChain: window %p, %ux%u, format %d, swap effect %d, %u buffer(s), windowed %d, flags 0x%X",
        desc->OutputWindow, desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, desc->SwapEffect,
        desc->BufferCount, desc->Windowed, desc->Flags);
    GameDevice gameDevice;
    DisplayInfo display;
    if (!ShouldUpgrade(factory, device, desc->OutputWindow, desc->BufferDesc.Format, desc->BufferDesc.Width, desc->BufferDesc.Height, gameDevice, display)) {
        return next(factory, device, desc, out);
    }

    const OutputMode output = ChooseOutputMode();
    DXGI_SWAP_CHAIN_DESC upgraded = *desc;
    upgraded.BufferDesc.Format = OutputFormat(output);
    upgraded.SampleDesc = {1, 0};
    upgraded.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    upgraded.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    if (!IsFlipModel(upgraded.SwapEffect)) upgraded.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // HDR output needs flip model
    upgraded.BufferCount = std::max(2u, upgraded.BufferCount);

    HRESULT hr = next(factory, device, &upgraded, out);
    if (FAILED(hr) && !IsFlipModel(desc->SwapEffect)) {
        Log("%s flip-model swap chain creation failed (0x%08lX); retrying in the game's blt model", OutputName(output), hr);
        upgraded.SwapEffect = desc->SwapEffect;
        hr = next(factory, device, &upgraded, out);
    }
    if (SUCCEEDED(hr)) {
        const GameBufferDesc game{desc->BufferDesc.Format, desc->SampleDesc, desc->BufferCount, desc->SwapEffect, desc->BufferUsage};
        if (WrapSwapChain(out, gameDevice, game, display, desc->OutputWindow, output)) return hr;
        (*out)->Release();
        *out = nullptr;
    } else {
        Log("%s swap chain creation failed (0x%08lX)", OutputName(output), hr);
    }
    hr = next(factory, device, desc, out);
    Log("Created the game's original swap chain instead: 0x%08lX", hr);
    return hr;
}

// Creates an upgraded swap chain for `desc` and wraps it; false (with *out unset) if that failed.
bool CreateUpgradedForHwnd(PFN_CreateSwapChainForHwnd next, IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
                           const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                           IDXGIOutput* restrictTo, IDXGISwapChain1** out, const GameDevice& gameDevice, const DisplayInfo& display) {
    const OutputMode output = ChooseOutputMode();
    DXGI_SWAP_CHAIN_DESC1 upgraded = *desc;
    upgraded.Format = OutputFormat(output);
    upgraded.SampleDesc = {1, 0};
    upgraded.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    upgraded.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    if (!IsFlipModel(upgraded.SwapEffect)) upgraded.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    upgraded.BufferCount = std::max(2u, upgraded.BufferCount);

    HRESULT hr = next(factory, device, hwnd, &upgraded, fullscreen, restrictTo, out);
    if (FAILED(hr) && !IsFlipModel(desc->SwapEffect)) {
        // A window can have only one flip-model swap chain. Unity keeps its windowed flip chain alive
        // and adds a blt-model chain for exclusive fullscreen on the same window, so that one stays blt.
        Log("%s flip-model swap chain creation failed (0x%08lX); retrying in the game's blt model", OutputName(output), hr);
        upgraded.SwapEffect = desc->SwapEffect;
        hr = next(factory, device, hwnd, &upgraded, fullscreen, restrictTo, out);
    }
    if (FAILED(hr)) {
        Log("%s swap chain creation failed (0x%08lX)", OutputName(output), hr);
        return false;
    }
    const GameBufferDesc game{desc->Format, desc->SampleDesc, desc->BufferCount, desc->SwapEffect, desc->BufferUsage};
    if (WrapSwapChain(out, gameDevice, game, display, hwnd, output)) return true;
    (*out)->Release();
    *out = nullptr;
    return false;
}

// Upgrades a swap chain that was created in the game's format (nobody has its buffers yet) by switching
// its buffers to our output format, then wraps it. False (leaving *out as it was) if that isn't possible.
bool UpgradeCreatedSwapChain(IDXGISwapChain1** out, const DXGI_SWAP_CHAIN_DESC1* desc, HWND hwnd, const GameDevice& gameDevice,
                             const DisplayInfo& display) {
    if (!IsFlipModel(desc->SwapEffect)) return false;  // HDR formats need flip model, which can't be changed afterwards
    const OutputMode output = ChooseOutputMode();
    const HRESULT hr = (*out)->ResizeBuffers(0, 0, 0, OutputFormat(output), desc->Flags & ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
    if (FAILED(hr)) {
        Log("Switching the created swap chain to %s failed (0x%08lX)", OutputName(output), hr);
        return false;
    }
    const GameBufferDesc game{desc->Format, desc->SampleDesc, desc->BufferCount, desc->SwapEffect, desc->BufferUsage};
    if (WrapSwapChain(out, gameDevice, game, display, hwnd, output)) return true;
    (*out)->ResizeBuffers(0, 0, 0, desc->Format, desc->Flags);  // back to what the game asked for
    return false;
}

// Frame generation layers (Intel XeFG, e.g. through OptiScaler) hand the game a swap chain of their own
// and create the one they present to inside the game's CreateSwapChainForHwnd. That inner swap chain is
// the one to convert: the game and the frame generation then render SDR into our proxies, every frame
// they present (generated ones included) is converted, and their resizes reach our wrapper. Armed for
// the game's call on this thread; the first nested creation on the same window takes it.
struct NestedUpgrade {
    bool armed = false;
    HWND hwnd = nullptr;
    bool done = false;
};
thread_local NestedUpgrade t_nested;

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrictTo,
                                                      IDXGISwapChain1** out) {
    auto next = Next<PFN_CreateSwapChainForHwnd>(s_CreateSwapChainForHwnd, factory, t_createDepth);
    if (!desc || !out) return next(factory, device, hwnd, desc, fullscreen, restrictTo, out);
    if (t_createDepth > 0) {
        if (!t_nested.armed || hwnd != t_nested.hwnd) return next(factory, device, hwnd, desc, fullscreen, restrictTo, out);
        t_nested.armed = false;
        DepthGuard guard(t_createDepth);
        Log("Nested CreateSwapChainForHwnd (the frame generation's own swap chain): %ux%u, format %d, %u buffer(s), flags 0x%X",
            desc->Width, desc->Height, desc->Format, desc->BufferCount, desc->Flags);
        GameDevice gameDevice;
        DisplayInfo display;
        if (ShouldUpgrade(factory, device, hwnd, desc->Format, desc->Width, desc->Height, gameDevice, display) &&
            CreateUpgradedForHwnd(next, factory, device, hwnd, desc, fullscreen, restrictTo, out, gameDevice, display)) {
            t_nested.done = true;
            return S_OK;
        }
        const HRESULT hr = next(factory, device, hwnd, desc, fullscreen, restrictTo, out);
        Log("Created the frame generation's original swap chain instead: 0x%08lX", hr);
        return hr;
    }
    DepthGuard guard(t_createDepth);

    Log("CreateSwapChainForHwnd: window %p, %ux%u, format %d, swap effect %d, %u buffer(s), windowed %d, flags 0x%X",
        hwnd, desc->Width, desc->Height, desc->Format, desc->SwapEffect, desc->BufferCount,
        fullscreen ? fullscreen->Windowed : TRUE, desc->Flags);
    GameDevice gameDevice;
    DisplayInfo display;
    if (!ShouldUpgrade(factory, device, hwnd, desc->Format, desc->Width, desc->Height, gameDevice, display)) {
        return next(factory, device, hwnd, desc, fullscreen, restrictTo, out);
    }

    if (const wchar_t* module = LoadedFrameGenerationModule()) {
        // The game's own request goes through unchanged, so the frame generation sees what it expects.
        Log("%ls is loaded (XeFG frame generation): converting the swap chain it creates for itself", module);
        t_nested = {true, hwnd, false};
        const HRESULT hr = next(factory, device, hwnd, desc, fullscreen, restrictTo, out);
        const bool converted = t_nested.done;
        t_nested = {};
        if (SUCCEEDED(hr) && !converted) {
            Log("No nested swap chain was created (frame generation inactive?): converting the game's swap chain");
            if (!UpgradeCreatedSwapChain(out, desc, hwnd, gameDevice, display)) Log("This swap chain is not converted");
        }
        return hr;
    }

    if (CreateUpgradedForHwnd(next, factory, device, hwnd, desc, fullscreen, restrictTo, out, gameDevice, display)) return S_OK;
    const HRESULT hr = next(factory, device, hwnd, desc, fullscreen, restrictTo, out);
    Log("Created the game's original swap chain instead: 0x%08lX", hr);
    return hr;
}

void InstallFactoryHooks(IUnknown* factory) {
    ComPtr<IDXGIFactory> f;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f)))) {
        Install(s_CreateSwapChain, f.Get(), kFactory_CreateSwapChain, reinterpret_cast<void*>(&Hook_CreateSwapChain));
    }
    ComPtr<IDXGIFactory2> f2;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2)))) {
        Install(s_CreateSwapChainForHwnd, f2.Get(), kFactory2_CreateSwapChainForHwnd,
                reinterpret_cast<void*>(&Hook_CreateSwapChainForHwnd));
    }
}

}  // namespace

void InstallDxgiHooks() {
    // Every factory in the process shares the vtable of this throwaway one, including those that
    // d3d11.dll creates internally for D3D11CreateDeviceAndSwapChain.
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = create ? create(IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    if (FAILED(hr)) {
        Log("Could not create a DXGI factory (0x%08lX); hooks not installed", hr);
        return;
    }
    InstallFactoryHooks(factory.Get());
    Log("DXGI hooks installed");
}

}  // namespace openhdr
