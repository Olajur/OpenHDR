#include "osd.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

namespace openhdr {
namespace {

enum class Kind { Toggle, Slider, Action };

struct Item {
    const wchar_t* label;
    const wchar_t* key;  // per-game ini key; null for runtime-only items
    Kind kind;
    float minValue, maxValue, step;
};

constexpr Item kItems[] = {
    {L"HDR conversion", L"Conversion", Kind::Toggle, 0, 1, 1},
    {L"Peak brightness", L"PeakBrightness", Kind::Slider, 100, 2000, 10},
    {L"Middle grey", L"MiddleGrey", Kind::Slider, 10, 100, 1},
    // -100..+100 slider scales (see itm_params.cpp).
    {L"Contrast", L"ContrastLevel", Kind::Slider, -100, 100, 5},
    {L"Saturation", L"SaturationLevel", Kind::Slider, -100, 100, 5},
    {L"Debanding", L"Debanding", Kind::Slider, 0, 2, 1},
    {L"Scene adaptation", L"SceneAdaptation", Kind::Toggle, 0, 1, 1},
    {L"Split-screen compare", nullptr, Kind::Toggle, 0, 1, 1},
    {L"Reset to defaults", nullptr, Kind::Action, 0, 0, 0},
};
constexpr int kItemCount = static_cast<int>(std::size(kItems));
enum { kConversion, kPeak, kMiddleGrey, kContrast, kSaturation, kDebanding, kScene, kCompare, kReset };

constexpr COLORREF kBackground = RGB(16, 16, 20);
constexpr COLORREF kSelectedRow = RGB(38, 38, 48);
constexpr COLORREF kAccent = RGB(255, 166, 48);
constexpr COLORREF kText = RGB(235, 235, 235);
constexpr COLORREF kTextDim = RGB(170, 170, 176);
constexpr COLORREF kHint = RGB(120, 120, 128);
constexpr COLORREF kTrack = RGB(60, 60, 68);

float SliderValue(int index, const Config& cfg, float autoPeakNits) {
    switch (index) {
        case kPeak: return PeakNits(cfg, autoPeakNits);
        case kMiddleGrey: return cfg.middleGrey;
        case kContrast: return static_cast<float>(cfg.contrastLevel);
        case kSaturation: return static_cast<float>(cfg.saturationLevel);
        case kDebanding: return static_cast<float>(cfg.debanding);
        default: return 0.0f;
    }
}

std::wstring ValueText(int index, const Config& cfg, float autoPeakNits, bool compare) {
    wchar_t buf[64] = L"";
    switch (index) {
        case kConversion: return cfg.conversion ? L"On" : L"Off";
        case kPeak:
            if (cfg.peakBrightness > 0.0f) swprintf_s(buf, L"%.0f nits", cfg.peakBrightness);
            else swprintf_s(buf, L"Auto (%.0f nits)", autoPeakNits);
            break;
        case kMiddleGrey: swprintf_s(buf, L"%.0f nits", cfg.middleGrey); break;
        case kContrast:
        case kSaturation: {
            const long level = std::lround(SliderValue(index, cfg, autoPeakNits));
            swprintf_s(buf, level > 0 ? L"+%ld" : L"%ld", level);
            break;
        }
        case kDebanding: return cfg.debanding == 0 ? L"Off" : cfg.debanding == 1 ? L"Low" : L"High";
        case kScene: return cfg.sceneAdaptation > 0.0f ? L"On" : L"Off";
        case kCompare: return compare ? L"On" : L"Off";
        default: break;
    }
    return buf;
}

}  // namespace

Osd::Osd(ConfigStore& store) : store_(store) {
    // Debug aid for automated captures: start with the panel open.
    wchar_t value[8];
    visible_ = GetEnvironmentVariableW(L"OPENHDR_OSD_OPEN", value, 8) > 0 && value[0] == L'1';
}

bool Osd::RepeatKey::Poll(int vk) {
    const bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const ULONGLONG now = GetTickCount64();
    bool fire = false;
    if (isDown && !down) {
        fire = true;
        next = now + 350;
    } else if (isDown && now >= next) {
        fire = true;
        next = now + 50;
    }
    down = isDown;
    return fire;
}

void Osd::Update(bool focused, bool togglePanel, float autoPeakNits, bool& compare) {
    // Key state is tracked even when unfocused so a held key doesn't fire on focus change.
    const bool up = up_.Poll(VK_UP);
    const bool down = down_.Poll(VK_DOWN);
    const bool left = left_.Poll(VK_LEFT);
    const bool right = right_.Poll(VK_RIGHT);
    if (!focused) return;

    if (togglePanel) {
        visible_ = !visible_;
        Log("Settings panel %s", visible_ ? "opened" : "closed");
    }
    if (!visible_) return;
    if (up) selected_ = (selected_ + kItemCount - 1) % kItemCount;
    if (down) selected_ = (selected_ + 1) % kItemCount;
    if (left != right) Adjust(right ? 1 : -1, autoPeakNits, compare);
}

void Osd::Adjust(int direction, float autoPeakNits, bool& compare) {
    const Config cfg = store_.Get();
    const Item& item = kItems[selected_];
    switch (selected_) {
        case kConversion:
            store_.SetGameValue(item.key, cfg.conversion ? 0.0f : 1.0f);
            break;
        case kScene:
            store_.SetGameValue(item.key, cfg.sceneAdaptation > 0.0f ? 0.0f : 1.0f);
            break;
        case kCompare:
            compare = !compare;
            break;
        case kReset:
            store_.ResetGameValues();
            compare = false;
            break;
        default: {
            const float current = SliderValue(selected_, cfg, autoPeakNits);
            const float stepped = std::round(current / item.step + direction) * item.step;
            store_.SetGameValue(item.key, std::clamp(stepped, item.minValue, item.maxValue));
            break;
        }
    }
}

bool Osd::Rasterize(UINT targetHeight, const Config& cfg, float autoPeakNits, bool compare) {
    if (!visible_) return false;
    if (!pixels_.empty() && cfg.version == shownVersion_ && selected_ == shownSelected_ && compare == shownCompare_ &&
        targetHeight == shownTargetHeight_) {
        return false;
    }

    const float scale = std::max(1.0f, targetHeight / 1080.0f);
    auto px = [scale](float v) { return static_cast<int>(std::lround(v * scale)); };
    const int w = px(420), pad = px(18), titleH = px(52), rowH = px(36), footerH = px(34);
    const int h = titleH + kItemCount * rowH + footerH;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap) {
        if (dc) DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    auto makeFont = [&](float size, int weight) {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT titleFont = makeFont(22, FW_SEMIBOLD);
    HFONT rowFont = makeFont(17, FW_NORMAL);
    HFONT smallFont = makeFont(14, FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(dc, rowFont);
    SetBkMode(dc, TRANSPARENT);

    auto fill = [&](int x, int y, int fw, int fh, COLORREF color) {
        RECT r{x, y, x + fw, y + fh};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    };
    auto text = [&](HFONT font, COLORREF color, RECT r, const wchar_t* str, UINT format) {
        SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, str, -1, &r, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };

    fill(0, 0, w, h, kBackground);
    fill(0, 0, w, std::max(1, px(3)), kAccent);
    text(titleFont, kAccent, {pad, 0, w - pad, titleH}, L"OpenHDR", DT_LEFT);
    const std::wstring hint = FormatHotkey(cfg.hotkeys[HotkeyAction::Panel]) + L" to close";
    text(smallFont, kHint, {pad, 0, w - pad, titleH}, hint.c_str(), DT_RIGHT);

    for (int i = 0; i < kItemCount; ++i) {
        const int y = titleH + i * rowH;
        const bool selected = i == selected_;
        if (selected) {
            fill(0, y, w, rowH, kSelectedRow);
            fill(0, y, std::max(2, px(4)), rowH, kAccent);
        }
        const RECT row{pad, y, w - pad, y + rowH};
        text(rowFont, selected ? kText : kTextDim, row, kItems[i].label, DT_LEFT);
        text(rowFont, selected ? kAccent : kText, row, ValueText(i, cfg, autoPeakNits, compare).c_str(), DT_RIGHT);
        if (kItems[i].kind == Kind::Slider) {
            const Item& item = kItems[i];
            const float t = std::clamp((SliderValue(i, cfg, autoPeakNits) - item.minValue) / (item.maxValue - item.minValue), 0.0f, 1.0f);
            const int barW = w - 2 * pad, barH = std::max(1, px(2)), barY = y + rowH - px(5);
            fill(pad, barY, barW, barH, kTrack);
            fill(pad, barY, static_cast<int>(barW * t), barH, selected ? kAccent : kTextDim);
        }
    }
    text(smallFont, kHint, {pad, h - footerH, w - pad, h}, L"↑↓ select     ←→ adjust", DT_LEFT);
    GdiFlush();

    width_ = static_cast<UINT>(w);
    height_ = static_cast<UINT>(h);
    pixels_.resize(size_t(w) * h);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < pixels_.size(); ++i) {
        const uint32_t bgra = src[i];  // GDI leaves alpha at 0; the panel is opaque
        pixels_[i] = 0xff000000u | ((bgra & 0xffu) << 16) | (bgra & 0xff00u) | ((bgra >> 16) & 0xffu);
    }

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(titleFont);
    DeleteObject(rowFont);
    DeleteObject(smallFont);
    DeleteObject(bitmap);
    DeleteDC(dc);

    shownVersion_ = cfg.version;
    shownSelected_ = selected_;
    shownCompare_ = compare;
    shownTargetHeight_ = targetHeight;
    return true;
}

void OsdTexture::Upload(ID3D11Device* device, ID3D11DeviceContext* context, const Osd& osd, bool changed) {
    if ((!changed && srv) || osd.Pixels().empty()) return;
    const UINT w = osd.Width(), h = osd.Height();
    D3D11_TEXTURE2D_DESC current{};
    if (texture) texture->GetDesc(&current);
    if (!texture || current.Width != w || current.Height != h) {
        srv.Reset();
        texture.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
            texture.Reset();
            srv.Reset();
            return;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        const uint32_t* src = osd.Pixels().data();
        for (UINT y = 0; y < h; ++y) {
            memcpy(static_cast<uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, src + size_t(y) * w, size_t(w) * 4);
        }
        context->Unmap(texture.Get(), 0);
    }
}

}  // namespace openhdr
