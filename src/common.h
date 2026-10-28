#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

namespace openhdr {

// printf-style line to %LOCALAPPDATA%\OpenHDR\logs\<exe>.log.
void Log(const char* fmt, ...);

// %LOCALAPPDATA%\OpenHDR (created with its logs, games and dumps subfolders on first use).
const std::wstring& DataDir();
// File name of this process's executable, lower case (e.g. "game.exe").
const std::wstring& ExeName();

}  // namespace openhdr
