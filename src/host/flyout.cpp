#include "host/flyout.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>

namespace openhdr {
namespace {

using ui::Rect;
using ui::TextStyle;

constexpr wchar_t kClass[] = L"OpenHDR.Flyout";
constexpr float kWidth = 400.0f;
constexpr float kPad = 16.0f;
constexpr float kRowHeight = 60.0f;
constexpr float kRowGap = 8.0f;
constexpr UINT_PTR kTipTimer = 1;
constexpr UINT kTipDelayMs = 500;

enum : int {
    kIdEnabled = 1,
    kIdAddGame,
    kIdAutostart,
    kIdSettings,
    kIdLogs,
    kIdQuit,
    kIdThemeFirst = 50,  // + 0 System, 1 Light, 2 Dark
    kIdGameFirst = 100,  // + index * 10 + kGame*
};
enum : int { kGameCapture = 1, kGameInjected = 2, kGameRemove = 3, kGameStride = 10 };

}  // namespace

Flyout::Flyout(FlyoutActions actions) : actions_(std::move(actions)) {}

Flyout::~Flyout() {
    if (hwnd_) DestroyWindow(hwnd_);
}

float Flyout::ContentHeight() const {
    const size_t rows = std::max<size_t>(model_.games.size(), 1);
    // header 64 + display card 56 + section title 40 + rows + footer 64
    return kPad + 48.0f + 12.0f + 52.0f + 16.0f + 32.0f + rows * (kRowHeight + kRowGap) + 8.0f + 12.0f + 44.0f + 44.0f;
}

void Flyout::Draw(ui::Canvas& c) {
    const ui::Theme& t = c.Colors();
    const float w = c.Size().width;
    float y = kPad;

    // Header: badge, name, status, master switch.
    c.Badge(Rect(kPad, y + 4, 40, 40), model_.enabled);
    c.Text(L"OpenHDR", Rect(kPad + 52, y + 2, 200, 24), TextStyle::Title, t.text);
    c.Text(model_.enabled ? L"On · converting your listed games" : L"Off · Windows Auto HDR can take over",
           Rect(kPad + 52, y + 26, w - kPad * 2 - 52 - 56, 18), TextStyle::Caption, t.textSecondary);
    c.Toggle(Rect(w - kPad - 40, y + 14, 40, 20), model_.enabled, kIdEnabled);
    c.Tip(kIdEnabled, model_.enabled ? L"Turn OpenHDR off" : L"Turn OpenHDR on");
    y += 48.0f + 12.0f;

    // Display status.
    c.Card(Rect(kPad, y, w - kPad * 2, 52));
    c.Text(ui::glyph::kDisplay, Rect(kPad + 14, y, 20, 52), TextStyle::Icon, model_.hdrOn ? t.accent : t.danger);
    wchar_t status[96];
    if (model_.hdrOn) {
        swprintf_s(status, L"HDR display · %.0f nits peak", model_.peakNits);
    } else {
        wcscpy_s(status, L"Windows HDR is off · turn it on in Display settings");
    }
    c.Text(status, Rect(kPad + 46, y, w - kPad * 2 - 56, 52), TextStyle::Body, t.text);
    y += 52.0f + 16.0f;

    // Games.
    c.Text(L"Games", Rect(kPad + 2, y, 120, 32), TextStyle::Subtitle, t.text);
    c.Button(Rect(w - kPad - 112, y + 2, 112, 28), L"Add game", kIdAddGame, false, ui::glyph::kAdd);
    c.Tip(kIdAddGame, L"Pick a game's .exe to convert it to HDR");
    y += 32.0f + 8.0f;
    if (model_.games.empty()) {
        c.Card(Rect(kPad, y, w - kPad * 2, kRowHeight));
        c.Text(L"No games yet. Add one to start converting it.", Rect(kPad + 16, y, w - kPad * 2 - 32, kRowHeight), TextStyle::Body,
               t.textSecondary);
        y += kRowHeight + kRowGap;
    }
    for (size_t i = 0; i < model_.games.size(); ++i) {
        const int base = kIdGameFirst + static_cast<int>(i) * kGameStride;
        const bool capture = model_.games[i].second == GameMode::Capture;
        c.Card(Rect(kPad, y, w - kPad * 2, kRowHeight));
        const float segWidth = 150.0f;
        const float textWidth = w - kPad * 2 - 16 - segWidth - 8 - 32 - 12;
        c.Text(GetGameDisplayName(model_.games[i].first), Rect(kPad + 16, y + 10, textWidth, 20), TextStyle::BodyStrong, t.text);
        c.Text(capture ? L"Nothing runs in the game" : L"Lowest latency", Rect(kPad + 16, y + 30, textWidth, 18),
               TextStyle::Caption, t.textSecondary);
        const float segX = w - kPad - 12 - 32 - 8 - segWidth;
        c.Segmented(Rect(segX, y + 14, segWidth, 32), {L"Capture", L"Injected"}, capture ? 0 : 1, base + kGameCapture);
        c.IconButton(Rect(w - kPad - 12 - 32, y + 14, 32, 32), ui::glyph::kDelete, base + kGameRemove, true);
        c.Tip(base + kGameCapture, L"Capture: converts the window from outside the game");
        c.Tip(base + kGameInjected, L"Injected: converts inside the game, lowest latency");
        c.Tip(base + kGameRemove, L"Remove from OpenHDR");
        y += kRowHeight + kRowGap;
    }

    // Footer: autostart switch and app actions.
    y += 8.0f;
    c.Fill(Rect(0, y, w, 1), t.stroke);
    y += 12.0f;
    c.Text(L"Appearance", Rect(kPad, y, 140, 36), TextStyle::Body, t.text);
    c.Segmented(Rect(w - kPad - 216, y + 2, 216, 32), {L"System", L"Light", L"Dark"}, static_cast<int>(model_.theme), kIdThemeFirst);
    c.Tip(kIdThemeFirst, L"Follow the Windows light/dark setting");
    c.Tip(kIdThemeFirst + 1, L"Always use the light theme");
    c.Tip(kIdThemeFirst + 2, L"Always use the dark theme");
    y += 44.0f;
    c.Toggle(Rect(kPad, y + 10, 40, 20), model_.autostart, kIdAutostart);
    c.Text(L"Start with Windows", Rect(kPad + 52, y, 180, 40), TextStyle::Body, t.text);
    c.Hit(Rect(kPad + 52, y, 150, 40), kIdAutostart);  // the label toggles too
    c.Tip(kIdAutostart, L"Start OpenHDR in the tray when you sign in");
    c.IconButton(Rect(w - kPad - 3 * 36 - 8, y + 2, 36, 36), ui::glyph::kSettings, kIdSettings);
    c.IconButton(Rect(w - kPad - 2 * 36 - 4, y + 2, 36, 36), ui::glyph::kFolder, kIdLogs);
    c.IconButton(Rect(w - kPad - 36, y + 2, 36, 36), ui::glyph::kPower, kIdQuit, true);
    c.Tip(kIdSettings, L"Open OpenHDR: game library and settings");
    c.Tip(kIdLogs, L"Open the logs folder");
    c.Tip(kIdQuit, L"Quit OpenHDR (closes the tray app)");

    if (tipShown_) c.DrawTip(c.HoverId());
}

void Flyout::OnClick(int id) {
    switch (id) {
        case kIdEnabled: actions_.setEnabled(!model_.enabled); break;
        case kIdAutostart: actions_.setAutostart(!model_.autostart); break;
        case kIdThemeFirst:
        case kIdThemeFirst + 1:
        case kIdThemeFirst + 2:
            actions_.setTheme(static_cast<ui::ThemeMode>(id - kIdThemeFirst));
            ui::ApplyWindowStyle(hwnd_, ui::CurrentTheme().dark);
            break;
        case kIdSettings: Hide(); actions_.openSettings(); return;
        case kIdLogs: Hide(); actions_.openLogs(); return;
        case kIdQuit: Hide(); actions_.quit(); return;
        case kIdAddGame:
            Hide();
            if (actions_.addGame(nullptr)) Show();
            return;
        default: {
            if (id < kIdGameFirst) return;
            const size_t index = static_cast<size_t>((id - kIdGameFirst) / kGameStride);
            const int part = (id - kIdGameFirst) % kGameStride;
            if (index >= model_.games.size()) return;
            const GameMode mode = part == kGameCapture ? GameMode::Capture : part == kGameInjected ? GameMode::Injected : GameMode::None;
            actions_.setGameMode(model_.games[index].first, mode);
            break;
        }
    }
    model_ = actions_.load();
    Resize();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::Resize() {
    if (!hwnd_) return;
    // Bottom-right of the work area, above the taskbar, like Windows 11's own flyouts.
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const float scale = GetDpiForWindow(hwnd_) / 96.0f;
    const int w = static_cast<int>(kWidth * scale), h = static_cast<int>(ContentHeight() * scale);
    const int margin = static_cast<int>(12 * scale);
    SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcWork.right - w - margin, mi.rcWork.bottom - h - margin, w, h, SWP_NOACTIVATE);
}

void Flyout::Show() {
    if (!hwnd_) {
        static const ATOM cls = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            wc.style = CS_DROPSHADOW;
            return RegisterClassW(&wc);
        }();
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, MAKEINTATOM(cls), L"OpenHDR", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        canvas_.AttachWindow(hwnd_);
    }
    model_ = actions_.load();
    ui::ApplyWindowStyle(hwnd_, ui::CurrentTheme().dark);
    Resize();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Flyout::Hide() {
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        hiddenAt_ = GetTickCount64();
    }
}

void Flyout::Toggle() {
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        Hide();
    } else if (GetTickCount64() - hiddenAt_ > 300) {  // the click that closed it via focus loss
        Show();
    }
}

bool Flyout::Snapshot(const std::wstring& path, float scale, int hoverId) {
    model_ = actions_.load();
    ui::Canvas canvas;
    if (!canvas.AttachBitmap(static_cast<UINT>(kWidth), static_cast<UINT>(ContentHeight()), scale)) return false;
    canvas.SetHover(hoverId);
    tipShown_ = hoverId != 0;
    canvas.Begin(ui::CurrentTheme());
    Draw(canvas);
    canvas.End();
    return canvas.SavePng(path);
}

LRESULT CALLBACK Flyout::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* created = static_cast<Flyout*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        created->hwnd_ = hwnd;  // Handle() uses it before CreateWindowEx returns
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<Flyout*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Flyout::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            canvas_.Begin(ui::CurrentTheme());
            Draw(canvas_);
            canvas_.End();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) Hide();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Hide();
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            const float scale = canvas_.Scale();
            const int id = canvas_.HitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
            if (!canvas_.Hover(id)) {
                canvas_.SetHover(id);
                tipShown_ = false;
                if (id) SetTimer(hwnd_, kTipTimer, kTipDelayMs, nullptr);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kTipTimer) {
                KillTimer(hwnd_, kTipTimer);
                tipShown_ = canvas_.HoverId() != 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            tipShown_ = false;
            canvas_.SetHover(0);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const float scale = canvas_.Scale();
            const int id = canvas_.HitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
            tipShown_ = false;
            if (id) OnClick(id);
            return 0;
        }
        case WM_SETTINGCHANGE:  // light/dark switch
            ui::ApplyWindowStyle(hwnd_, ui::CurrentTheme().dark);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case WM_DPICHANGED:
            Resize();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace openhdr
