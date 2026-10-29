#pragma once

// Tiny immediate-mode UI kit for OpenHDR's windows (tray flyout, installer), in the Windows 11
// Fluent style: Direct2D + DirectWrite only, no frameworks or runtimes. Each frame a window draws
// its widgets through a Canvas, which also records their rectangles for hit testing.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace openhdr::ui {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kAppVersion[] = L"0.4.1";

struct Theme {
    bool dark = true;
    D2D1_COLOR_F background;     // window
    D2D1_COLOR_F card;           // grouped content
    D2D1_COLOR_F cardHover;
    D2D1_COLOR_F control;        // buttons, segmented background
    D2D1_COLOR_F controlHover;
    D2D1_COLOR_F stroke;         // 1px card/control borders
    D2D1_COLOR_F text;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F accent;         // OpenHDR orange
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F onAccent;       // text and glyphs on the accent colour
    D2D1_COLOR_F danger;
};

// Which colours CurrentTheme() returns: the Windows "app mode" setting, or always light/dark.
enum class ThemeMode { System, Light, Dark };
void SetThemeMode(ThemeMode mode);
ThemeMode GetThemeMode();
// "system" / "light" / "dark" (the Theme value in OpenHDR.ini).
ThemeMode ParseThemeMode(const wchar_t* text);
const wchar_t* ThemeModeName(ThemeMode mode);

Theme CurrentTheme();

enum class TextStyle { Title, Subtitle, Body, BodyStrong, Caption, Icon, IconSmall, BodyWrap };

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) code points.
namespace glyph {
constexpr wchar_t kAdd[] = L"";
constexpr wchar_t kClose[] = L"";
constexpr wchar_t kDelete[] = L"";
constexpr wchar_t kSettings[] = L"";
constexpr wchar_t kFolder[] = L"";
constexpr wchar_t kPower[] = L"";
constexpr wchar_t kGame[] = L"";
constexpr wchar_t kDisplay[] = L"";
constexpr wchar_t kCheck[] = L"";

constexpr wchar_t kSearch[] = L"\uE721";
constexpr wchar_t kRefresh[] = L"\uE72C";
constexpr wchar_t kLibrary[] = L"\uE8F1";
constexpr wchar_t kKeyboard[] = L"\uE765";
constexpr wchar_t kEdit[] = L"\uE70F";
constexpr wchar_t kRemove[] = L"\uE738";
constexpr wchar_t kOpen[] = L"\uE8A7";
}  // namespace glyph

// A clickable region recorded while drawing.
struct HitRect {
    D2D1_RECT_F rect;
    int id;
};

// Draws on any Direct2D render target (a window, or a bitmap for snapshots) in DIPs; `scale`
// converts DIPs to pixels. Widgets take an id; Hover() tells whether the pointer is over one.
class Canvas {
public:
    Canvas();
    // Render target for a window, resized as needed. Returns false if Direct2D isn't available.
    bool AttachWindow(HWND hwnd);
    // Draws into an offscreen bitmap instead (see SavePng).
    bool AttachBitmap(UINT width, UINT height, float scale);
    bool SavePng(const std::wstring& path);

    void Begin(const Theme& theme);
    // Returns false if the device was lost (the next Begin recreates it).
    bool End();

    float Scale() const { return scale_; }
    D2D1_SIZE_F Size() const;  // in DIPs
    const Theme& Colors() const { return theme_; }

    // Hit testing: the pointer position in DIPs, and the widgets recorded in the last frame.
    void SetPointer(float x, float y) { pointerX_ = x, pointerY_ = y; }
    int HitTest(float x, float y) const;
    bool Hover(int id) const { return hoverId_ == id; }
    void SetHover(int id) { hoverId_ = id; }

    // Drawing primitives.
    void Fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0.0f);
    void Stroke(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0.0f, float width = 1.0f);
    void Text(const std::wstring& text, D2D1_RECT_F rect, TextStyle style, D2D1_COLOR_F color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextWidth(const std::wstring& text, TextStyle style);

    // Widgets (each records its rect under `id` for hit testing).
    void Card(D2D1_RECT_F rect, int id = 0);
    void Button(D2D1_RECT_F rect, const std::wstring& label, int id, bool primary = false, const wchar_t* icon = nullptr);
    void IconButton(D2D1_RECT_F rect, const wchar_t* icon, int id, bool danger = false);
    void Toggle(D2D1_RECT_F rect, bool on, int id);
    // Segment i is recorded as id + i.
    void Segmented(D2D1_RECT_F rect, const std::vector<std::wstring>& labels, int selected, int id);
    void Badge(D2D1_RECT_F rect, bool on);  // the "H" app badge
    // An image file (JPEG, PNG...) scaled to cover `rect` (cropped, centred) with rounded corners.
    // Decoded once per file and target; false if the file can't be read.
    bool Image(const std::wstring& path, D2D1_RECT_F rect, float radius = 0.0f);
    // Clips drawing to `rect` until PopClip.
    void PushClip(D2D1_RECT_F rect);
    void PopClip();
    void Hit(D2D1_RECT_F rect, int id) { hits_.push_back({rect, id}); }

    // Tooltips: Tip() attaches a short explanation to a widget drawn this frame; DrawTip() draws it
    // next to that widget (call it last, so it is on top). Windows show it after a short hover.
    void Tip(int id, const std::wstring& text) { tips_.emplace_back(id, text); }
    void DrawTip(int id);
    int HoverId() const { return hoverId_; }

private:
    IDWriteTextFormat* Format(TextStyle style);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color);

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1RenderTarget> target_;
    ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    ComPtr<IUnknown> wicFactory_;
    ComPtr<IUnknown> wicBitmap_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> formats_[8];
    ComPtr<IUnknown> imageFactory_;  // IWICImagingFactory for Image()
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> images_;
    std::set<std::wstring> failedImages_;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    Theme theme_;
    std::vector<HitRect> hits_;
    std::vector<std::pair<int, std::wstring>> tips_;
    float pointerX_ = -1.0f, pointerY_ = -1.0f;
    int hoverId_ = 0;
};

// Rounded corners, dark title bar and shadow for a popup on Windows 11 (no-ops on Windows 10).
void ApplyWindowStyle(HWND hwnd, bool dark);

inline D2D1_RECT_F Rect(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

}  // namespace openhdr::ui
