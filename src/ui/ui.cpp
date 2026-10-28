#include "ui/ui.h"

#include <dwmapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

namespace openhdr::ui {
namespace {

D2D1_COLOR_F Hex(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

bool FontInstalled(IDWriteFactory* dwrite, const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(dwrite->GetSystemFontCollection(&fonts)) && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

}  // namespace

namespace {
ThemeMode g_themeMode = ThemeMode::System;
}  // namespace

void SetThemeMode(ThemeMode mode) { g_themeMode = mode; }
ThemeMode GetThemeMode() { return g_themeMode; }

ThemeMode ParseThemeMode(const wchar_t* text) {
    if (!_wcsicmp(text, L"light")) return ThemeMode::Light;
    if (!_wcsicmp(text, L"dark")) return ThemeMode::Dark;
    return ThemeMode::System;
}

const wchar_t* ThemeModeName(ThemeMode mode) {
    return mode == ThemeMode::Light ? L"light" : mode == ThemeMode::Dark ? L"dark" : L"system";
}

Theme CurrentTheme() {
    DWORD light = 0, size = sizeof(light);
    bool dark = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                             L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS ||
                light == 0;
    if (g_themeMode != ThemeMode::System) dark = g_themeMode == ThemeMode::Dark;
    wchar_t forced[8];  // design checks: OPENHDR_THEME=light|dark
    if (GetEnvironmentVariableW(L"OPENHDR_THEME", forced, 8) > 0) dark = _wcsicmp(forced, L"light") != 0;
    Theme t;
    t.dark = dark;
    if (dark) {
        t.background = Hex(0x202020);
        t.card = Hex(0x2B2B2B);
        t.cardHover = Hex(0x323232);
        t.control = Hex(0x373737);
        t.controlHover = Hex(0x3F3F3F);
        t.stroke = Hex(0xFFFFFF, 0.08f);
        t.text = Hex(0xFFFFFF);
        t.textSecondary = Hex(0xC5C5C5);
        t.accent = Hex(0xFFA630);
        t.accentHover = Hex(0xFFB859);
        t.onAccent = Hex(0x1A1A1A);
        t.danger = Hex(0xFF8A80);
    } else {
        t.background = Hex(0xF3F3F3);
        t.card = Hex(0xFFFFFF);
        t.cardHover = Hex(0xF9F9F9);
        t.control = Hex(0xFBFBFB);
        t.controlHover = Hex(0xF0F0F0);
        t.stroke = Hex(0x000000, 0.09f);
        t.text = Hex(0x1A1A1A);
        t.textSecondary = Hex(0x5D5D5D);
        t.accent = Hex(0xD9730D);
        t.accentHover = Hex(0xE8851F);
        t.onAccent = Hex(0xFFFFFF);
        t.danger = Hex(0xC42B1C);
    }
    return t;
}

Canvas::Canvas() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
}

bool Canvas::AttachWindow(HWND hwnd) {
    hwnd_ = hwnd;
    return factory_ && dwrite_;
}

bool Canvas::AttachBitmap(UINT width, UINT height, float scale) {
    if (!factory_ || !dwrite_) return false;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(static_cast<UINT>(width * scale), static_cast<UINT>(height * scale), GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &bitmap))) {
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * scale,
        96.0f * scale);
    if (FAILED(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), &props, &target_))) return false;
    scale_ = scale;
    wicFactory_ = wic;
    wicBitmap_ = bitmap;
    return true;
}

bool Canvas::SavePng(const std::wstring& path) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    if (!wicFactory_ || FAILED(wicFactory_.As(&wic)) || FAILED(wicBitmap_.As(&bitmap))) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    UINT w = 0, h = 0;
    bitmap->GetSize(&w, &h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    return SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

void Canvas::Begin(const Theme& theme) {
    theme_ = theme;
    if (hwnd_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom));
        scale_ = GetDpiForWindow(hwnd_) / 96.0f;
        if (!hwndTarget_) {
            factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(hwnd_, size), &hwndTarget_);
            target_ = hwndTarget_;
            brush_.Reset();
            images_.clear();  // bitmaps belong to the old target
        } else {
            hwndTarget_->Resize(size);
        }
        target_->SetDpi(96.0f * scale_, 96.0f * scale_);
    }
    hits_.clear();
    tips_.clear();
    target_->BeginDraw();
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->Clear(theme_.background);
}

bool Canvas::End() {
    const HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        hwndTarget_.Reset();
        target_.Reset();
        brush_.Reset();
        images_.clear();
        return false;
    }
    return SUCCEEDED(hr);
}

D2D1_SIZE_F Canvas::Size() const { return target_ ? target_->GetSize() : D2D1::SizeF(0, 0); }

int Canvas::HitTest(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->rect.left && x < it->rect.right && y >= it->rect.top && y < it->rect.bottom) return it->id;
    }
    return 0;
}

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F color) {
    if (!brush_) target_->CreateSolidColorBrush(color, &brush_);
    brush_->SetColor(color);
    return brush_.Get();
}

IDWriteTextFormat* Canvas::Format(TextStyle style) {
    const int i = static_cast<int>(style);
    if (!formats_[i]) {
        static const bool fluentIcons = FontInstalled(dwrite_.Get(), L"Segoe Fluent Icons");
        static const bool variable = FontInstalled(dwrite_.Get(), L"Segoe UI Variable Text");
        const wchar_t* text = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
        const wchar_t* display = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
        const wchar_t* icons = fluentIcons ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        struct Spec { const wchar_t* family; DWRITE_FONT_WEIGHT weight; float size; };
        const Spec specs[] = {
            {display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f},  // Title
            {text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f},     // Subtitle
            {text, DWRITE_FONT_WEIGHT_NORMAL, 14.0f},        // Body
            {text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f},     // BodyStrong
            {text, DWRITE_FONT_WEIGHT_NORMAL, 12.0f},        // Caption
            {icons, DWRITE_FONT_WEIGHT_NORMAL, 16.0f},       // Icon
            {icons, DWRITE_FONT_WEIGHT_NORMAL, 12.0f},       // IconSmall
            {text, DWRITE_FONT_WEIGHT_NORMAL, 14.0f},        // BodyWrap
        };
        const Spec& s = specs[i];
        dwrite_->CreateTextFormat(s.family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, L"en-us",
                                  &formats_[i]);
        formats_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (style == TextStyle::BodyWrap) return formats_[i].Get();  // wraps instead of trimming
        formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite_->CreateEllipsisTrimmingSign(formats_[i].Get(), &ellipsis);
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        formats_[i]->SetTrimming(&trimming, ellipsis.Get());
    }
    return formats_[i].Get();
}

void Canvas::Fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) {
    if (radius > 0.0f) {
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), Brush(color));
    } else {
        target_->FillRectangle(rect, Brush(color));
    }
}

void Canvas::Stroke(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius, float width) {
    // Inset by half the width so 1px lines land on pixel boundaries.
    const float inset = width / 2.0f;
    const D2D1_RECT_F r = D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
    target_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(color), width);
}

void Canvas::Text(const std::wstring& text, D2D1_RECT_F rect, TextStyle style, D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* format = Format(style);
    format->SetTextAlignment(align);
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, Brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Canvas::TextWidth(const std::wstring& text, TextStyle style) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), Format(style), 10000.0f, 100.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void Canvas::Card(D2D1_RECT_F rect, int id) {
    Fill(rect, id && Hover(id) ? theme_.cardHover : theme_.card, 8.0f);
    Stroke(rect, theme_.stroke, 8.0f);
    if (id) Hit(rect, id);
}

void Canvas::Button(D2D1_RECT_F rect, const std::wstring& label, int id, bool primary, const wchar_t* icon) {
    const bool hover = Hover(id);
    Fill(rect, primary ? (hover ? theme_.accentHover : theme_.accent) : (hover ? theme_.controlHover : theme_.control), 6.0f);
    if (!primary) Stroke(rect, theme_.stroke, 6.0f);
    const D2D1_COLOR_F color = primary ? theme_.onAccent : theme_.text;
    if (icon) {
        const float labelWidth = TextWidth(label, TextStyle::Body);
        const float total = 12.0f + 8.0f + labelWidth;
        const float x = (rect.left + rect.right - total) / 2.0f;
        Text(icon, D2D1::RectF(x, rect.top, x + 12.0f, rect.bottom), TextStyle::IconSmall, color);
        Text(label, D2D1::RectF(x + 20.0f, rect.top, rect.right, rect.bottom), TextStyle::Body, color);
    } else {
        Text(label, rect, primary ? TextStyle::BodyStrong : TextStyle::Body, color, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    Hit(rect, id);
}

void Canvas::IconButton(D2D1_RECT_F rect, const wchar_t* icon, int id, bool danger) {
    const bool hover = Hover(id);
    if (hover) Fill(rect, theme_.controlHover, 6.0f);
    Text(icon, rect, TextStyle::Icon, hover && danger ? theme_.danger : theme_.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    Hit(rect, id);
}

void Canvas::Toggle(D2D1_RECT_F rect, bool on, int id) {
    // Fluent toggle switch: 40 x 20 pill, 12 px knob (14 px on hover).
    const float h = rect.bottom - rect.top;
    const bool hover = Hover(id);
    const float knob = hover ? 14.0f : 12.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    if (on) {
        Fill(rect, hover ? theme_.accentHover : theme_.accent, h / 2.0f);
        const float cx = rect.right - h / 2.0f;
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), knob / 2.0f, knob / 2.0f), Brush(theme_.onAccent));
    } else {
        if (hover) Fill(rect, theme_.controlHover, h / 2.0f);
        Stroke(rect, theme_.textSecondary, h / 2.0f);
        const float cx = rect.left + h / 2.0f;
        target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), knob / 2.0f, knob / 2.0f), Brush(theme_.textSecondary));
    }
    Hit(rect, id);
}

void Canvas::Segmented(D2D1_RECT_F rect, const std::vector<std::wstring>& labels, int selected, int id) {
    Fill(rect, theme_.control, 6.0f);
    Stroke(rect, theme_.stroke, 6.0f);
    const float w = (rect.right - rect.left) / labels.size();
    for (size_t i = 0; i < labels.size(); ++i) {
        const D2D1_RECT_F seg = D2D1::RectF(rect.left + w * i, rect.top, rect.left + w * (i + 1), rect.bottom);
        const int segId = id + static_cast<int>(i);
        const bool isSelected = static_cast<int>(i) == selected;
        if (isSelected) {
            const D2D1_RECT_F inner = D2D1::RectF(seg.left + 3.0f, seg.top + 3.0f, seg.right - 3.0f, seg.bottom - 3.0f);
            Fill(inner, theme_.accent, 4.0f);
        } else if (Hover(segId)) {
            const D2D1_RECT_F inner = D2D1::RectF(seg.left + 3.0f, seg.top + 3.0f, seg.right - 3.0f, seg.bottom - 3.0f);
            Fill(inner, theme_.controlHover, 4.0f);
        }
        Text(labels[i], seg, isSelected ? TextStyle::BodyStrong : TextStyle::Body, isSelected ? theme_.onAccent : theme_.text,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        Hit(seg, segId);
    }
}

void Canvas::Badge(D2D1_RECT_F rect, bool on) {
    Fill(rect, on ? theme_.accent : theme_.control, 8.0f);
    Text(L"H", rect, TextStyle::Title, on ? theme_.onAccent : theme_.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
}

bool Canvas::Image(const std::wstring& path, D2D1_RECT_F rect, float radius) {
    if (path.empty() || failedImages_.count(path)) return false;
    auto found = images_.find(path);
    if (found == images_.end()) {
        ComPtr<IWICImagingFactory> wic;
        if (!imageFactory_) {
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            imageFactory_ = wic;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        ComPtr<ID2D1Bitmap> bitmap;
        UINT w = 0, h = 0;
        // Decoded at about the size it is drawn at (cover art is 600 x 900 or more), to keep memory small.
        const float wantH = (rect.bottom - rect.top) * scale_;
        bool ok = imageFactory_ && SUCCEEDED(imageFactory_.As(&wic)) &&
                  SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                  SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h;
        if (ok) {
            const float factor = std::min(1.0f, std::max(wantH, 64.0f) / h);
            ok = SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                 SUCCEEDED(scaler->Initialize(frame.Get(), std::max(1u, static_cast<UINT>(w * factor)), std::max(1u, static_cast<UINT>(h * factor)),
                                              WICBitmapInterpolationModeFant)) &&
                 SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                 SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                                 WICBitmapPaletteTypeMedianCut)) &&
                 SUCCEEDED(target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
        }
        if (!ok) {
            failedImages_.insert(path);
            return false;
        }
        found = images_.emplace(path, bitmap).first;
    }
    ID2D1Bitmap* bitmap = found->second.Get();
    const D2D1_SIZE_F size = bitmap->GetSize();
    const float rw = rect.right - rect.left, rh = rect.bottom - rect.top;
    if (size.width <= 0 || size.height <= 0 || rw <= 0 || rh <= 0) return false;
    // Cover: scale so the image fills the rect, centred, through a bitmap brush (keeps the rounded corners).
    const float s = std::max(rw / size.width, rh / size.height);
    const float ox = rect.left + (rw - size.width * s) / 2.0f, oy = rect.top + (rh - size.height * s) / 2.0f;
    ComPtr<ID2D1BitmapBrush> brush;
    if (FAILED(target_->CreateBitmapBrush(bitmap, &brush))) return false;
    brush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    brush->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(ox, oy));
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    return true;
}

void Canvas::PushClip(D2D1_RECT_F rect) { target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED); }

void Canvas::PopClip() { target_->PopAxisAlignedClip(); }

void Canvas::DrawTip(int id) {
    const std::wstring* text = nullptr;
    for (const auto& tip : tips_) {
        if (tip.first == id) text = &tip.second;
    }
    const HitRect* anchor = nullptr;
    for (const HitRect& hit : hits_) {
        if (hit.id == id) anchor = &hit;
    }
    if (!text || !anchor) return;

    // Fluent tooltip: small elevated card, above the widget (below if there's no room), kept inside the window.
    const D2D1_SIZE_F size = Size();
    const float w = std::min(TextWidth(*text, TextStyle::Caption) + 20.0f, size.width - 12.0f), h = 28.0f;
    const float cx = (anchor->rect.left + anchor->rect.right) / 2.0f;
    const float x = std::clamp(cx - w / 2.0f, 6.0f, size.width - 6.0f - w);
    float y = anchor->rect.top - h - 6.0f;
    if (y < 6.0f) y = anchor->rect.bottom + 6.0f;
    const D2D1_RECT_F bubble = D2D1::RectF(x, y, x + w, y + h);
    Fill(D2D1::RectF(x, y + 1.5f, x + w, y + h + 1.5f), D2D1::ColorF(0, 0, 0, theme_.dark ? 0.35f : 0.12f), 6.0f);  // soft shadow
    Fill(bubble, theme_.dark ? theme_.cardHover : theme_.card, 6.0f);
    Stroke(bubble, theme_.stroke, 6.0f);
    Text(*text, bubble, TextStyle::Caption, theme_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void ApplyWindowStyle(HWND hwnd, bool dark) {
    const BOOL darkMode = dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));
    const DWORD round = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
}

}  // namespace openhdr::ui
