#include "display.h"

namespace openhdr {

DisplayInfo QueryDisplay(IDXGIFactory* factory, HWND hwnd) {
    DisplayInfo info;
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter> adapter;
    for (UINT a = 0; factory->EnumAdapters(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1;
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1))) {
                info.found = true;
                info.hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                info.maxNits = desc1.MaxLuminance;
                info.maxFullFrameNits = desc1.MaxFullFrameLuminance;
                Log("Display %ls: HDR %s, peak %.0f nits, full-frame %.0f nits", desc.DeviceName,
                    info.hdr ? "on" : "off", info.maxNits, info.maxFullFrameNits);
            }
            return info;
        }
    }
    Log("Display for window %p not found among DXGI outputs", hwnd);
    return info;
}

}  // namespace openhdr
