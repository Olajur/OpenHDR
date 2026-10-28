#pragma once

#include "common.h"

#include <d3d12.h>

namespace openhdr {

// Writes a texture as a portable float map (RGB, little endian, bottom row first): FP16 textures as
// their linear values, UNORM textures as code values 0..1. Readable by Python, GIMP, Photoshop...
// Stalls the GPU; debugging only.
bool DumpTexturePfm(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const std::wstring& path);

// D3D12 version. `state` is the resource's current state; it is restored afterwards. Waits for `queue`.
bool DumpTexturePfm(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                    const std::wstring& path);

}  // namespace openhdr
