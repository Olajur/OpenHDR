#include "frame_dump.h"

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace openhdr {
namespace {

float HalfToFloat(uint16_t h) {
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    const int exponent = (h >> 10) & 0x1f;
    const int mantissa = h & 0x3ff;
    if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31) return mantissa ? NAN : sign * INFINITY;
    return sign * std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
}

// Mapped pixel rows to float RGB, top row first. Supports the formats OpenHDR creates or accepts.
bool Decode(DXGI_FORMAT format, const void* data, size_t rowPitch, UINT width, UINT height, std::vector<float>& rgb) {
    rgb.resize(size_t(width) * height * 3);
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(data) + size_t(y) * rowPitch;
        float* dst = &rgb[size_t(y) * width * 3];
        for (UINT x = 0; x < width; ++x, dst += 3) {
            switch (format) {
                case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
                    dst[0] = HalfToFloat(p[0]), dst[1] = HalfToFloat(p[1]), dst[2] = HalfToFloat(p[2]);
                    break;
                }
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
                    const uint8_t* p = row + x * 4;
                    dst[0] = p[0] / 255.0f, dst[1] = p[1] / 255.0f, dst[2] = p[2] / 255.0f;
                    break;
                }
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
                    const uint8_t* p = row + x * 4;
                    dst[0] = p[2] / 255.0f, dst[1] = p[1] / 255.0f, dst[2] = p[0] / 255.0f;
                    break;
                }
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM: {
                    const uint32_t v = reinterpret_cast<const uint32_t*>(row)[x];
                    dst[0] = (v & 1023) / 1023.0f, dst[1] = ((v >> 10) & 1023) / 1023.0f, dst[2] = ((v >> 20) & 1023) / 1023.0f;
                    break;
                }
                default:
                    return false;
            }
        }
    }
    return true;
}

bool WritePfm(const std::wstring& path, const std::vector<float>& rgb, UINT width, UINT height) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fprintf(f, "PF\n%u %u\n-1.0\n", width, height);
    for (UINT y = height; y-- > 0;) fwrite(&rgb[size_t(y) * width * 3], sizeof(float), size_t(width) * 3, f);
    fclose(f);
    Log("Dumped %ux%u frame to %ls", width, height, path.c_str());
    return true;
}

}  // namespace

bool DumpTexturePfm(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    std::vector<float> rgb;
    const bool ok = Decode(desc.Format, mapped.pData, mapped.RowPitch, desc.Width, desc.Height, rgb);
    context->Unmap(staging.Get(), 0);
    return ok && WritePfm(path, rgb, desc.Width, desc.Height);
}

bool DumpTexturePfm(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                    const std::wstring& path) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc = {1, 0};
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, done);
    WaitForSingleObject(done, 5000);
    CloseHandle(done);

    void* data = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &range, &data))) return false;
    std::vector<float> rgb;
    const UINT width = static_cast<UINT>(desc.Width), height = desc.Height;
    const bool ok = Decode(desc.Format, static_cast<uint8_t*>(data) + footprint.Offset, footprint.Footprint.RowPitch, width, height, rgb);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return ok && WritePfm(path, rgb, width, height);
}

}  // namespace openhdr
