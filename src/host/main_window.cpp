#include "host/main_window.h"

#include "hotkeys.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

namespace openhdr {
namespace {

using ui::Rect;
using ui::TextStyle;

constexpr wchar_t kClass[] = L"OpenHDR.Main";
constexpr wchar_t kSection[] = L"OpenHDR";
constexpr UINT WM_APP_SCANNED = WM_APP + 20;  // lParam: std::vector<LibraryGame>* from the scan thread
constexpr UINT_PTR kTipTimer = 1;
constexpr UINT kTipDelayMs = 500;

constexpr float kWindowW = 1240.0f, kWindowH = 800.0f;
constexpr float kSidebarW = 224.0f;
constexpr float kPad = 28.0f;
constexpr float kCardW = 172.0f, kCoverW = 156.0f, kCoverH = 234.0f, kCardH = 306.0f, kCardGap = 18.0f;
constexpr float kPanelW = 392.0f;
constexpr float kRowH = 44.0f;

enum : int {
    kIdNavLibrary = 1,
    kIdNavSettings,
    kIdMaster,
    kIdScan = 10,
    kIdAdd,
    kIdSearch,
    kIdFilterFirst = 20,  // + Filter
    kIdPanelClose = 30,
    kIdModeFirst = 40,  // + 0 off, 1 capture, 2 injected
    kIdChangeExe = 50,
    kIdOpenFolder,
    kIdGamePicture = 100,      // picture settings of the selected game (+ kPic*)
    kIdDefaultPicture = 200,   // defaults for all games (+ kPic*)
    kIdHotkeyFirst = 300,      // + action: change
    kIdHotkeysDefault = 320,
    kIdThemeFirst = 330,       // + ThemeMode
    kIdAutostart = 340,
    kIdLogs,
    kIdIni,
    kIdCardFirst = 1000,       // + game index
};
enum : int {
    kPicPeakMinus = 0, kPicPeakPlus, kPicGreyMinus, kPicGreyPlus, kPicContrastMinus, kPicContrastPlus,
    kPicSatMinus, kPicSatPlus, kPicDebandFirst = 8, kPicScene = 11, kPicOutputFirst = 12, kPicReset = 15, kPicCount = 16,
};

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, a); }

D2D1_COLOR_F Hex(UINT32 rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// A small pill with text; returns its width.
float Chip(ui::Canvas& c, float x, float y, const std::wstring& text, D2D1_COLOR_F fill, D2D1_COLOR_F ink, D2D1_COLOR_F border) {
    const float w = c.TextWidth(text, TextStyle::Caption) + 16.0f;
    const D2D1_RECT_F r = Rect(x, y, w, 20);
    c.Fill(r, fill, 5.0f);
    if (border.a > 0) c.Stroke(r, border, 5.0f);
    c.Text(text, r, TextStyle::Caption, ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    return w;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

const wchar_t* ModeName(GameMode mode) {
    return mode == GameMode::Capture ? L"Capture" : mode == GameMode::Injected ? L"Injected" : L"Off";
}

std::wstring Initials(const std::wstring& name) {
    std::wstring out;
    bool start = true;
    for (wchar_t ch : name) {
        if (iswalnum(ch) && start && out.size() < 3) out += static_cast<wchar_t>(towupper(ch));
        start = !iswalnum(ch);
    }
    return out.empty() ? L"?" : out;
}

// "−  value  +" control at the right edge of a row.
void Stepper(ui::Canvas& c, float right, float y, const std::wstring& value, int minusId, int plusId) {
    const float valueW = 124.0f;
    c.Button(Rect(right - 32, y + 6, 32, 32), L"+", plusId);
    c.Text(value, Rect(right - 32 - valueW, y, valueW, kRowH), TextStyle::Body, c.Colors().text, DWRITE_TEXT_ALIGNMENT_CENTER);
    c.Button(Rect(right - 32 - valueW - 32, y + 6, 32, 32), L"−", minusId);
}

}  // namespace

MainWindow::MainWindow(MainWindowActions actions) : actions_(std::move(actions)) {}

MainWindow::~MainWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void MainWindow::Show() {
    if (!hwnd_) {
        static const ATOM cls = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
            wc.lpszClassName = kClass;
            return RegisterClassW(&wc);
        }();
        // Centred on the monitor under the cursor, at the window's DPI.
        POINT cursor;
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        UINT dpiX = 96, dpiY = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(monitor, 0, &dpiX, &dpiY);
        }
        const float scale = dpiX / 96.0f;
        const RECT work = mi.rcWork;
        const int w = std::min(static_cast<int>(kWindowW * scale), static_cast<int>(work.right - work.left));
        const int h = std::min(static_cast<int>(kWindowH * scale), static_cast<int>(work.bottom - work.top));
        hwnd_ = CreateWindowExW(0, MAKEINTATOM(cls), L"OpenHDR", WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - w) / 2,
                                work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        canvas_.AttachWindow(hwnd_);
        ui::ApplyWindowStyle(hwnd_, ui::CurrentTheme().dark);
        ShowWindow(hwnd_, SW_SHOW);
        if (games_.empty()) StartScan();
    } else {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ShowWindow(hwnd_, SW_SHOW);
    }
    SetForegroundWindow(hwnd_);
    Invalidate();
}

bool MainWindow::Snapshot(const std::wstring& path, int page, int selected, float scale) {
    games_ = ScanLibrary();
    page_ = page == 1 ? Page::Settings : Page::Library;
    selected_ = selected >= 0 && selected < static_cast<int>(games_.size()) ? selected : -1;
    ui::Canvas canvas;
    if (!canvas.AttachBitmap(static_cast<UINT>(kWindowW), static_cast<UINT>(kWindowH), scale)) return false;
    canvas.Begin(ui::CurrentTheme());
    Draw(canvas);
    canvas.End();
    return canvas.SavePng(path);
}

void MainWindow::StartScan() {
    if (scanning_ || !hwnd_) return;
    scanning_ = true;
    HWND hwnd = hwnd_;
    std::thread([hwnd] {
        auto* found = new std::vector<LibraryGame>(ScanLibrary());
        if (!PostMessageW(hwnd, WM_APP_SCANNED, 0, reinterpret_cast<LPARAM>(found))) delete found;
    }).detach();
    Invalidate();
}

void MainWindow::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

ConfigStore& MainWindow::Store(const std::wstring& exe) {
    auto& store = stores_[exe];
    if (!store) store = std::make_unique<ConfigStore>(exe);  // "" has no game file: the defaults
    store->ReloadIfChanged();
    return *store;
}

// Writes a setting as text: to OpenHDR.ini's defaults (exe empty) or the game's own file.
void MainWindow::WriteSetting(const std::wstring& exe, const wchar_t* key, const std::wstring& value) {
    if (exe.empty()) {
        WritePrivateProfileStringW(kSection, key, value.c_str(), GlobalIniPath().c_str());
    } else {
        const std::wstring dir = DataDir() + L"\\games";
        CreateDirectoryW(dir.c_str(), nullptr);
        WritePrivateProfileStringW(kSection, key, value.c_str(), (dir + L"\\" + exe + L".ini").c_str());
    }
    Store(exe).ReloadIfChanged();
}

GameMode MainWindow::ModeOf(const LibraryGame& g) const { return GetGameMode(g.ExeName()); }

void MainWindow::SetMode(const LibraryGame& g, GameMode mode) {
    const std::wstring exe = g.ExeName();
    SetGameMode(exe, mode);
    if (mode != GameMode::None && g.store != L"Manual") {
        // The library's name ("Cyberpunk 2077"), not the exe's, in the tray and the panel.
        WritePrivateProfileStringW(L"GameNames", ExeFileName(exe).c_str(), g.name.c_str(), GlobalIniPath().c_str());
    }
    if (actions_.gamesChanged) actions_.gamesChanged();
}

void MainWindow::ChangeExe(LibraryGame& g) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Game executable (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g.installDir.empty() ? nullptr : g.installDir.c_str();
    ofn.lpstrTitle = L"The game's process (as in Task Manager)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    const GameMode mode = ModeOf(g);
    if (mode != GameMode::None) SetGameMode(g.ExeName(), GameMode::None);  // the mode moves to the new exe
    g.exePath = file;
    SetLibraryExe(g, file);
    if (mode != GameMode::None) SetMode(g, mode);
}

std::vector<int> MainWindow::VisibleGames() const {
    std::vector<int> out;
    const std::wstring query = Lower(search_);
    for (int i = 0; i < static_cast<int>(games_.size()); ++i) {
        const LibraryGame& g = games_[i];
        const bool added = ModeOf(g) != GameMode::None;
        if ((filter_ == Filter::Added && !added) || (filter_ == Filter::NotAdded && added)) continue;
        if (!query.empty() && Lower(g.name).find(query) == std::wstring::npos && Lower(g.ExeName()).find(query) == std::wstring::npos) {
            continue;
        }
        out.push_back(i);
    }
    return out;
}

// ---- drawing --------------------------------------------------------------------

void MainWindow::Draw(ui::Canvas& c) {
    const D2D1_SIZE_F size = c.Size();
    DrawSidebar(c, size.height);
    const D2D1_RECT_F area = D2D1::RectF(kSidebarW, 0, size.width, size.height);
    if (page_ == Page::Library) {
        DrawLibrary(c, area);
    } else {
        DrawSettings(c, area);
    }
    if (tipShown_) c.DrawTip(c.HoverId());
}

void MainWindow::DrawSidebar(ui::Canvas& c, float h) {
    const ui::Theme& t = c.Colors();
    c.Fill(Rect(0, 0, kSidebarW, h), t.dark ? Hex(0x1A1A1A) : Hex(0xEBEBEB));
    c.Fill(Rect(kSidebarW - 1, 0, 1, h), t.stroke);
    const bool on = actions_.enabled ? actions_.enabled() : true;
    c.Badge(Rect(20, 22, 36, 36), on);
    c.Text(L"OpenHDR", Rect(66, 20, 140, 24), TextStyle::Subtitle, t.text);
    c.Text(L"SDR to HDR for any GPU", Rect(66, 40, 150, 18), TextStyle::Caption, t.textSecondary);

    auto nav = [&](float y, const wchar_t* icon, const wchar_t* label, bool selected, int id) {
        const D2D1_RECT_F r = Rect(12, y, kSidebarW - 24, 40);
        if (selected) {
            c.Fill(r, t.control, 6.0f);
            c.Fill(Rect(12, y + 11, 3, 18), t.accent, 1.5f);
        } else if (c.Hover(id)) {
            c.Fill(r, WithAlpha(t.controlHover, 0.6f), 6.0f);
        }
        c.Text(icon, Rect(28, y, 20, 40), TextStyle::Icon, selected ? t.accent : t.textSecondary);
        c.Text(label, Rect(58, y, 140, 40), selected ? TextStyle::BodyStrong : TextStyle::Body, t.text);
        c.Hit(r, id);
    };
    nav(84, ui::glyph::kLibrary, L"Library", page_ == Page::Library, kIdNavLibrary);
    nav(128, ui::glyph::kSettings, L"Settings", page_ == Page::Settings, kIdNavSettings);

    // Bottom: master switch and display status.
    const float peak = actions_.displayPeakNits ? actions_.displayPeakNits() : 0.0f;
    c.Fill(Rect(12, h - 116, kSidebarW - 24, 1), t.stroke);
    c.Text(L"Conversion", Rect(20, h - 104, 120, 24), TextStyle::BodyStrong, t.text);
    c.Text(on ? L"On for your games" : L"Off (Auto HDR can take over)", Rect(20, h - 82, 190, 18), TextStyle::Caption, t.textSecondary);
    c.Toggle(Rect(kSidebarW - 20 - 40, h - 100, 40, 20), on, kIdMaster);
    c.Tip(kIdMaster, on ? L"Turn OpenHDR off" : L"Turn OpenHDR on");
    wchar_t display[64];
    if (peak > 0) swprintf_s(display, L"HDR display · %.0f nits", peak);
    else wcscpy_s(display, L"Windows HDR is off");
    c.Text(ui::glyph::kDisplay, Rect(20, h - 52, 18, 32), TextStyle::IconSmall, peak > 0 ? t.accent : t.danger);
    c.Text(display, Rect(42, h - 52, 170, 32), TextStyle::Caption, t.textSecondary);
}

void MainWindow::DrawLibrary(ui::Canvas& c, D2D1_RECT_F area) {
    const ui::Theme& t = c.Colors();
    const bool panel = selected_ >= 0 && selected_ < static_cast<int>(games_.size());
    const float right = panel ? area.right - kPanelW : area.right;
    const float x0 = area.left + kPad;

    // Header.
    c.Text(L"Library", Rect(x0, 22, 200, 32), TextStyle::Title, t.text);
    int added = 0;
    for (const LibraryGame& g : games_) added += ModeOf(g) != GameMode::None;
    wchar_t count[96];
    if (scanning_ && games_.empty()) wcscpy_s(count, L"Looking for installed games…");
    else swprintf_s(count, L"%zu games found · %d added%ls", games_.size(), added, scanning_ ? L" · scanning…" : L"");
    c.Text(count, Rect(x0, 52, 400, 20), TextStyle::Caption, t.textSecondary);
    c.Button(Rect(right - kPad - 150, 26, 150, 34), L"Add manually", kIdAdd, false, ui::glyph::kAdd);
    c.Button(Rect(right - kPad - 150 - 10 - 140, 26, 140, 34), scanning_ ? L"Scanning…" : L"Scan games", kIdScan, true, ui::glyph::kRefresh);
    c.Tip(kIdScan, L"Look for games installed with Steam, Epic and GOG");
    c.Tip(kIdAdd, L"Pick any game's .exe yourself");

    // Search box and filter.
    const float rowY = 84.0f;
    const D2D1_RECT_F box = Rect(x0, rowY, std::min(340.0f, right - x0 - kPad - 330), 34);
    c.Fill(box, t.control, 6.0f);
    c.Stroke(box, searchFocused_ ? t.accent : t.stroke, 6.0f, searchFocused_ ? 1.5f : 1.0f);
    c.Text(ui::glyph::kSearch, Rect(box.left + 12, rowY, 16, 34), TextStyle::IconSmall, t.textSecondary);
    const bool caret = searchFocused_ && (GetTickCount64() / 530) % 2 == 0;
    c.Text(search_.empty() && !searchFocused_ ? L"Search games…" : search_ + (caret ? L"|" : L""),
           Rect(box.left + 36, rowY, box.right - box.left - 44, 34), TextStyle::Body, search_.empty() ? t.textSecondary : t.text);
    c.Hit(box, kIdSearch);
    c.Segmented(Rect(right - kPad - 300, rowY, 300, 34), {L"All", L"Added", L"Not added"}, static_cast<int>(filter_), kIdFilterFirst);

    // Grid.
    const float gridTop = rowY + 34 + 22;
    const D2D1_RECT_F grid = D2D1::RectF(x0, gridTop, right - kPad + 8, area.bottom);
    const std::vector<int> visible = VisibleGames();
    const int columns = std::max(1, static_cast<int>((grid.right - grid.left - 8 + kCardGap) / (kCardW + kCardGap)));
    const int rows = (static_cast<int>(visible.size()) + columns - 1) / columns;
    const float contentH = rows * (kCardH + kCardGap) + kPad;
    maxScroll_ = std::max(0.0f, contentH - (grid.bottom - grid.top));
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll_);
    if (visible.empty()) {
        const wchar_t* empty = scanning_ ? L"Scanning your Steam, Epic and GOG libraries…"
                               : search_.empty() ? L"No games here yet. Use Scan games or Add manually."
                                                 : L"No game matches your search.";
        c.Text(empty, Rect(x0, gridTop + 40, right - x0 - kPad, 30), TextStyle::Body, t.textSecondary);
    }
    c.PushClip(grid);
    for (size_t k = 0; k < visible.size(); ++k) {
        const int col = static_cast<int>(k) % columns, row = static_cast<int>(k) / columns;
        const float x = grid.left + col * (kCardW + kCardGap), y = grid.top + row * (kCardH + kCardGap) - scroll_;
        if (y > grid.bottom || y + kCardH < grid.top) continue;
        DrawCard(c, games_[visible[k]], visible[k], Rect(x, y, kCardW, kCardH));
    }
    c.PopClip();
    if (maxScroll_ > 0) {  // thin scroll indicator
        const float trackH = grid.bottom - grid.top - 16;
        const float thumbH = std::max(40.0f, trackH * (grid.bottom - grid.top) / contentH);
        const float thumbY = grid.top + 8 + (trackH - thumbH) * (scroll_ / maxScroll_);
        c.Fill(Rect(right - 10, thumbY, 4, thumbH), WithAlpha(t.textSecondary, 0.5f), 2.0f);
    }

    if (panel) DrawGamePanel(c, D2D1::RectF(right, 0, area.right, area.bottom));
}

void MainWindow::DrawCard(ui::Canvas& c, const LibraryGame& g, int index, D2D1_RECT_F r) {
    const ui::Theme& t = c.Colors();
    const int id = kIdCardFirst + index;
    const bool hover = c.Hover(id), selected = index == selected_;
    c.Fill(r, hover || selected ? t.cardHover : t.card, 10.0f);
    c.Stroke(r, selected ? t.accent : hover ? WithAlpha(t.accent, 0.6f) : t.stroke, 10.0f, selected ? 2.0f : 1.0f);
    const D2D1_RECT_F cover = Rect(r.left + 8, r.top + 8, kCoverW, kCoverH);
    if (!c.Image(g.coverPath, cover, 6.0f)) {
        c.Fill(cover, t.dark ? Hex(0x2B2B33) : Hex(0xDADAE2), 6.0f);
        c.Text(Initials(g.name), Rect(cover.left, cover.top + kCoverH / 2 - 30, kCoverW, 40), TextStyle::Title, t.textSecondary,
               DWRITE_TEXT_ALIGNMENT_CENTER);
        c.Text(g.store, Rect(cover.left, cover.top + kCoverH / 2 + 8, kCoverW, 20), TextStyle::Caption, t.textSecondary,
               DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Store tag (top left) and mode tag (top right) over the cover.
    Chip(c, cover.left + 8, cover.top + 8, g.store, Hex(0x000000, 0.62f), Hex(0xFFFFFF), Hex(0, 0));
    const GameMode mode = ModeOf(g);
    if (mode != GameMode::None) {
        const std::wstring label = ModeName(mode);
        const float w = c.TextWidth(label, TextStyle::Caption) + 16.0f;
        Chip(c, cover.right - 8 - w, cover.top + 8, label, t.accent, t.onAccent, Hex(0, 0));
    }
    if (g.optiScaler) Chip(c, cover.left + 8, cover.bottom - 28, L"✦ OptiScaler", Hex(0x2A1E5C, 0.9f), Hex(0xD9CCFF), Hex(0x8B6CFF, 0.8f));

    c.Text(g.name, Rect(r.left + 10, cover.bottom + 6, kCardW - 20, 22), TextStyle::BodyStrong, t.text);
    float x = r.left + 10;
    const float y = cover.bottom + 32;
    if (g.dlss) x += Chip(c, x, y, L"DLSS", Hex(0x76B900, 0.16f), t.dark ? Hex(0x9BE04A) : Hex(0x3C6E00), Hex(0x76B900, 0.5f)) + 4;
    if (g.fsr) x += Chip(c, x, y, L"FSR", Hex(0xED1C24, 0.14f), t.dark ? Hex(0xFF7A7F) : Hex(0xA8101A), Hex(0xED1C24, 0.5f)) + 4;
    if (g.xess) x += Chip(c, x, y, L"XeSS", Hex(0x0068B5, 0.18f), t.dark ? Hex(0x6CB6FF) : Hex(0x00508C), Hex(0x0068B5, 0.6f)) + 4;
    if (!g.dlss && !g.fsr && !g.xess) c.Text(g.ExeName(), Rect(r.left + 10, y, kCardW - 20, 20), TextStyle::Caption, t.textSecondary);
    c.Hit(r, id);
}

void MainWindow::DrawGamePanel(ui::Canvas& c, D2D1_RECT_F area) {
    const ui::Theme& t = c.Colors();
    const LibraryGame& g = games_[selected_];
    c.Fill(area, t.dark ? Hex(0x232323) : Hex(0xFAFAFA));
    c.Fill(Rect(area.left, 0, 1, area.bottom), t.stroke);
    const float x = area.left + 24, w = area.right - area.left - 48;
    float y = 24;
    c.IconButton(Rect(area.right - 24 - 32, y, 32, 32), ui::glyph::kClose, kIdPanelClose);
    c.Tip(kIdPanelClose, L"Close");

    const D2D1_RECT_F cover = Rect(x, y, 84, 126);
    if (!c.Image(g.coverPath, cover, 6.0f)) {
        c.Fill(cover, t.control, 6.0f);
        c.Text(Initials(g.name), cover, TextStyle::Subtitle, t.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    c.Text(g.name, Rect(x + 100, y + 4, w - 100 - 36, 44), TextStyle::BodyWrap, t.text);
    c.Text(g.store, Rect(x + 100, y + 52, w - 100, 18), TextStyle::Caption, t.textSecondary);
    c.Text(g.ExeName(), Rect(x + 100, y + 72, w - 100, 18), TextStyle::Caption, t.textSecondary);
    if (g.store != L"Manual") {
        c.Button(Rect(x + 100, y + 96, 110, 28), L"Change exe", kIdChangeExe, false, ui::glyph::kEdit);
        c.Tip(kIdChangeExe, L"Pick the game's real process if this one is a launcher");
        c.Button(Rect(x + 216, y + 96, 92, 28), L"Folder", kIdOpenFolder, false, ui::glyph::kFolder);
        c.Tip(kIdOpenFolder, L"Open the install folder");
    }
    y += 126 + 24;

    const GameMode mode = ModeOf(g);
    c.Text(L"HDR conversion", Rect(x, y, w, 22), TextStyle::Subtitle, t.text);
    y += 28;
    c.Segmented(Rect(x, y, w, 34), {L"Off", L"Capture", L"Injected"}, mode == GameMode::None ? 0 : mode == GameMode::Capture ? 1 : 2,
                kIdModeFirst);
    c.Tip(kIdModeFirst, L"Don't convert this game");
    c.Tip(kIdModeFirst + 1, L"Converts the window from outside the game (nothing runs in it)");
    c.Tip(kIdModeFirst + 2, L"Converts inside the game: lowest latency, exclusive fullscreen");
    y += 42;
    const wchar_t* explain = mode == GameMode::Capture ? L"Windowed or borderless. About one frame of extra latency."
                             : mode == GameMode::Injected ? L"Start the game after OpenHDR. D3D11 and D3D12."
                                                           : L"Pick Capture (safe for any game) or Injected (lowest latency).";
    c.Text(explain, Rect(x, y, w, 36), TextStyle::BodyWrap, t.textSecondary);
    y += 48;

    if (mode == GameMode::None) return;
    c.Text(L"Picture", Rect(x, y, w, 22), TextStyle::Subtitle, t.text);
    c.Text(L"For this game; the rest follow the defaults in Settings", Rect(x, y + 22, w, 18), TextStyle::Caption, t.textSecondary);
    y += 46;
    DrawPictureSettings(c, x, y, w, g.ExeName(), kIdGamePicture);
}

float MainWindow::DrawPictureSettings(ui::Canvas& c, float x, float y, float w, const std::wstring& exe, int base) {
    const ui::Theme& t = c.Colors();
    const Config cfg = Store(exe).Get();
    const float right = x + w;
    const float y0 = y;
    // Labels use the space left of the controls (the widest, a stepper, is 188 wide).
    const float labelW = w - 196.0f;
    auto label = [&](const wchar_t* text, const wchar_t* caption) {
        c.Text(text, Rect(x, y + (caption ? 4 : 0), labelW, caption ? 20 : kRowH), TextStyle::Body, t.text);
        if (caption) c.Text(caption, Rect(x, y + 23, labelW, 16), TextStyle::Caption, t.textSecondary);
    };
    wchar_t value[64];

    const float autoPeak = actions_.displayPeakNits ? actions_.displayPeakNits() : 0.0f;
    if (cfg.peakBrightness <= 0) swprintf_s(value, autoPeak > 0 ? L"Auto (%.0f)" : L"Auto", AutoPeakNits(autoPeak));
    else swprintf_s(value, L"%.0f nits", cfg.peakBrightness);
    label(L"Peak brightness", L"Brightest highlights");
    Stepper(c, right, y, value, base + kPicPeakMinus, base + kPicPeakPlus);
    y += kRowH + 4;

    swprintf_s(value, L"%.0f nits", cfg.middleGrey);
    label(L"Middle grey", L"Midtone brightness");
    Stepper(c, right, y, value, base + kPicGreyMinus, base + kPicGreyPlus);
    y += kRowH + 4;

    swprintf_s(value, L"%+d", cfg.contrastLevel);
    label(L"Contrast", L"−100 to +100");
    Stepper(c, right, y, value, base + kPicContrastMinus, base + kPicContrastPlus);
    y += kRowH + 4;

    swprintf_s(value, L"%+d", cfg.saturationLevel);
    label(L"Saturation", L"−100 to +100");
    Stepper(c, right, y, value, base + kPicSatMinus, base + kPicSatPlus);
    y += kRowH + 4;

    label(L"Debanding", nullptr);
    c.Segmented(Rect(right - 188, y + 5, 188, 34), {L"Off", L"Low", L"High"}, cfg.debanding, base + kPicDebandFirst);
    y += kRowH + 4;

    label(L"Scene adaptation", L"Follows the scene");
    c.Toggle(Rect(right - 40, y + 12, 40, 20), cfg.sceneAdaptation > 0.0f, base + kPicScene);
    y += kRowH + 4;

    label(L"Injected output", L"HDR10 for 10-bit-only layers");
    c.Segmented(Rect(right - 188, y + 5, 188, 34), {L"Auto", L"FP16", L"HDR10"}, static_cast<int>(cfg.outputFormat), base + kPicOutputFirst);
    c.Tip(base + kPicOutputFirst, L"FP16 scRGB (the most precise)");
    c.Tip(base + kPicOutputFirst + 1, L"FP16 scRGB");
    c.Tip(base + kPicOutputFirst + 2, L"10-bit HDR10, for frame generation or overlays that need it");
    y += kRowH + 4;

    if (!exe.empty()) {
        c.Button(Rect(x, y + 8, 168, 32), L"Reset to defaults", base + kPicReset, false, ui::glyph::kRefresh);
        c.Tip(base + kPicReset, L"Use the defaults from Settings for this game");
        y += 48;
    }
    return y - y0;
}

void MainWindow::DrawSettings(ui::Canvas& c, D2D1_RECT_F area) {
    const ui::Theme& t = c.Colors();
    const float x0 = area.left + kPad;
    const float w = std::min(760.0f, area.right - x0 - kPad);
    c.Text(L"Settings", Rect(x0, 22, 300, 32), TextStyle::Title, t.text);
    c.Text(L"Changes apply right away, also in running games", Rect(x0, 52, 500, 20), TextStyle::Caption, t.textSecondary);

    const D2D1_RECT_F view = D2D1::RectF(area.left, 84, area.right, area.bottom);
    c.PushClip(view);
    float y = view.top + 4 - settingsScroll_;

    // Defaults for all games.
    c.Text(L"Defaults for all games", Rect(x0, y, w, 24), TextStyle::Subtitle, t.text);
    y += 32;
    y += DrawPictureSettings(c, x0 + 4, y, w - 4, L"", kIdDefaultPicture) + 20;

    // Hotkeys.
    c.Text(L"Hotkeys", Rect(x0, y, w, 24), TextStyle::Subtitle, t.text);
    c.Text(L"In the game's window. Click Change, then press the new combination (Esc cancels, Backspace clears).",
           Rect(x0, y + 24, w, 18), TextStyle::Caption, t.textSecondary);
    y += 52;
    const HotkeySet keys = ReadHotkeys(GlobalIniPath());
    for (int i = 0; i < kHotkeyCount; ++i) {
        const auto action = static_cast<HotkeyAction>(i);
        const bool recording = recordingHotkey_ == i;
        c.Text(ui::glyph::kKeyboard, Rect(x0 + 4, y, 20, kRowH), TextStyle::IconSmall, t.textSecondary);
        c.Text(HotkeyLabel(action), Rect(x0 + 32, y, w * 0.45f, kRowH), TextStyle::Body, t.text);
        const D2D1_RECT_F box = Rect(x0 + w - 110 - 12 - 220, y + 6, 220, 32);
        c.Fill(box, t.control, 6.0f);
        c.Stroke(box, recording ? t.accent : t.stroke, 6.0f, recording ? 1.5f : 1.0f);
        c.Text(recording ? L"Press keys…" : FormatHotkey(keys[action]), box, recording ? TextStyle::Body : TextStyle::BodyStrong,
               recording ? t.accent : t.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        c.Button(Rect(x0 + w - 110, y + 6, 110, 32), recording ? L"Cancel" : L"Change", kIdHotkeyFirst + i);
        y += kRowH + 4;
    }
    c.Button(Rect(x0, y + 6, 168, 32), L"Restore defaults", kIdHotkeysDefault, false, ui::glyph::kRefresh);
    c.Tip(kIdHotkeysDefault, L"Alt+1 panel, Alt+2 conversion, Alt+3 compare");
    y += 64;

    // App.
    c.Text(L"App", Rect(x0, y, w, 24), TextStyle::Subtitle, t.text);
    y += 32;
    c.Text(L"Appearance", Rect(x0 + 4, y, 200, kRowH), TextStyle::Body, t.text);
    c.Segmented(Rect(x0 + w - 240, y + 5, 240, 34), {L"System", L"Light", L"Dark"}, static_cast<int>(ui::GetThemeMode()), kIdThemeFirst);
    y += kRowH + 4;
    const bool autostart = actions_.autostart && actions_.autostart();
    c.Text(L"Start with Windows", Rect(x0 + 4, y, 260, kRowH), TextStyle::Body, t.text);
    c.Toggle(Rect(x0 + w - 40, y + 12, 40, 20), autostart, kIdAutostart);
    y += kRowH + 4;
    c.Button(Rect(x0, y + 6, 140, 32), L"Open logs", kIdLogs, false, ui::glyph::kFolder);
    c.Button(Rect(x0 + 150, y + 6, 170, 32), L"Edit OpenHDR.ini", kIdIni, false, ui::glyph::kEdit);
    c.Tip(kIdLogs, L"Logs of the tray app and of each game");
    c.Tip(kIdIni, L"All settings as text, including advanced ones");
    y += 56;
    wchar_t about[96];
    swprintf_s(about, L"OpenHDR %ls", ui::kAppVersion);
    c.Text(about, Rect(x0, y, w, 20), TextStyle::Caption, t.textSecondary);
    y += 40;
    c.PopClip();

    const float contentH = y + settingsScroll_ - view.top;
    settingsMaxScroll_ = std::max(0.0f, contentH - (view.bottom - view.top));
    settingsScroll_ = std::clamp(settingsScroll_, 0.0f, settingsMaxScroll_);
}

// ---- input ------------------------------------------------------------------------

void MainWindow::OnPictureClick(int id, int base, const std::wstring& exe) {
    ConfigStore& store = Store(exe);
    const Config cfg = store.Get();
    const int part = id - base;
    // Numbers go to the game's file (SetGameValue) or the defaults; choices are written as text.
    auto setNumber = [&](const wchar_t* key, float v) {
        if (exe.empty()) {
            wchar_t buf[32];
            swprintf_s(buf, L"%g", v);
            WriteSetting(exe, key, buf);
        } else {
            store.SetGameValue(key, v);
        }
    };
    switch (part) {
        case kPicPeakMinus:
        case kPicPeakPlus: {
            // 0 = auto (the display's peak); otherwise 300..2000 nits in 50 nit steps.
            const float autoPeak = AutoPeakNits(actions_.displayPeakNits ? actions_.displayPeakNits() : 0.0f);
            float v = cfg.peakBrightness > 0 ? cfg.peakBrightness : std::round(autoPeak / 50.0f) * 50.0f;
            v += part == kPicPeakPlus ? 50.0f : -50.0f;
            if (cfg.peakBrightness <= 0 && part == kPicPeakMinus) v = std::round(autoPeak / 50.0f) * 50.0f - 50.0f;
            setNumber(L"PeakBrightness", v < 300.0f ? 0.0f : std::min(v, 2000.0f));
            break;
        }
        case kPicGreyMinus:
        case kPicGreyPlus:
            setNumber(L"MiddleGrey", std::clamp(std::round(cfg.middleGrey / 5.0f) * 5.0f + (part == kPicGreyPlus ? 5.0f : -5.0f), 10.0f, 100.0f));
            break;
        case kPicContrastMinus:
        case kPicContrastPlus:
            setNumber(L"ContrastLevel", static_cast<float>(std::clamp(cfg.contrastLevel + (part == kPicContrastPlus ? 5 : -5), -100, 100)));
            break;
        case kPicSatMinus:
        case kPicSatPlus:
            setNumber(L"SaturationLevel", static_cast<float>(std::clamp(cfg.saturationLevel + (part == kPicSatPlus ? 5 : -5), -100, 100)));
            break;
        case kPicDebandFirst:
        case kPicDebandFirst + 1:
        case kPicDebandFirst + 2: {
            static const wchar_t* const names[] = {L"off", L"low", L"high"};
            WriteSetting(exe, L"Debanding", names[part - kPicDebandFirst]);
            break;
        }
        case kPicScene:
            setNumber(L"SceneAdaptation", cfg.sceneAdaptation > 0.0f ? 0.0f : 1.0f);
            break;
        case kPicOutputFirst:
        case kPicOutputFirst + 1:
        case kPicOutputFirst + 2: {
            static const wchar_t* const names[] = {L"auto", L"scrgb", L"hdr10"};
            WriteSetting(exe, L"OutputFormat", names[part - kPicOutputFirst]);
            break;
        }
        case kPicReset:
            if (!exe.empty()) store.ResetGameValues();
            break;
    }
}

void MainWindow::OnClick(int id) {
    if (id != kIdSearch) searchFocused_ = false;
    if (recordingHotkey_ >= 0 && id != kIdHotkeyFirst + recordingHotkey_) recordingHotkey_ = -1;
    if (id >= kIdCardFirst) {
        const int index = id - kIdCardFirst;
        selected_ = selected_ == index ? -1 : index;
    } else if (id >= kIdGamePicture && id < kIdGamePicture + static_cast<int>(kPicCount)) {
        if (selected_ >= 0) OnPictureClick(id, kIdGamePicture, games_[selected_].ExeName());
    } else if (id >= kIdDefaultPicture && id < kIdDefaultPicture + static_cast<int>(kPicCount)) {
        OnPictureClick(id, kIdDefaultPicture, L"");
    } else if (id >= kIdHotkeyFirst && id < kIdHotkeyFirst + kHotkeyCount) {
        const int i = id - kIdHotkeyFirst;
        recordingHotkey_ = recordingHotkey_ == i ? -1 : i;
    } else if (id >= kIdFilterFirst && id < kIdFilterFirst + 3) {
        filter_ = static_cast<Filter>(id - kIdFilterFirst);
        scroll_ = 0;
    } else if (id >= kIdModeFirst && id < kIdModeFirst + 3) {
        if (selected_ >= 0) {
            static const GameMode modes[] = {GameMode::None, GameMode::Capture, GameMode::Injected};
            SetMode(games_[selected_], modes[id - kIdModeFirst]);
        }
    } else if (id >= kIdThemeFirst && id < kIdThemeFirst + 3) {
        if (actions_.setTheme) actions_.setTheme(static_cast<ui::ThemeMode>(id - kIdThemeFirst));
        ui::ApplyWindowStyle(hwnd_, ui::CurrentTheme().dark);
    } else {
        switch (id) {
            case kIdNavLibrary: page_ = Page::Library; break;
            case kIdNavSettings: page_ = Page::Settings; break;
            case kIdMaster: if (actions_.setEnabled && actions_.enabled) actions_.setEnabled(!actions_.enabled()); break;
            case kIdScan: StartScan(); break;
            case kIdAdd:
                if (actions_.addGameManually && actions_.addGameManually(hwnd_)) StartScan();
                break;
            case kIdSearch: searchFocused_ = true; break;
            case kIdPanelClose: selected_ = -1; break;
            case kIdChangeExe:
                if (selected_ >= 0) ChangeExe(games_[selected_]);
                break;
            case kIdOpenFolder:
                if (selected_ >= 0 && !games_[selected_].installDir.empty()) {
                    ShellExecuteW(nullptr, L"open", games_[selected_].installDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                break;
            case kIdHotkeysDefault: {
                const HotkeySet defaults = DefaultHotkeys();
                for (int i = 0; i < kHotkeyCount; ++i) WriteHotkey(GlobalIniPath(), static_cast<HotkeyAction>(i), defaults.keys[i]);
                break;
            }
            case kIdAutostart: if (actions_.setAutostart && actions_.autostart) actions_.setAutostart(!actions_.autostart()); break;
            case kIdLogs: if (actions_.openLogs) actions_.openLogs(); break;
            case kIdIni: if (actions_.openIni) actions_.openIni(); break;
        }
    }
    Invalidate();
}

// Key presses while recording a hotkey, or typing into the search box. True if handled.
bool MainWindow::OnKey(UINT vk, bool down) {
    if (!down) return recordingHotkey_ >= 0;
    if (recordingHotkey_ >= 0) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LSHIFT ||
            vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) {
            return true;  // wait for the main key
        }
        const auto action = static_cast<HotkeyAction>(recordingHotkey_);
        if (vk == VK_ESCAPE) {
            recordingHotkey_ = -1;
        } else {
            Hotkey key;
            if (vk != VK_BACK) {
                key.vk = vk;
                key.mods = ((GetKeyState(VK_MENU) & 0x8000) ? MOD_ALT : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? MOD_CONTROL : 0) |
                           ((GetKeyState(VK_SHIFT) & 0x8000) ? MOD_SHIFT : 0) |
                           (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? MOD_WIN : 0);
            }
            // One action per combination: an action that had it before loses it.
            const HotkeySet keys = ReadHotkeys(GlobalIniPath());
            for (int i = 0; i < kHotkeyCount; ++i) {
                if (i != recordingHotkey_ && key.vk && keys.keys[i] == key) WriteHotkey(GlobalIniPath(), static_cast<HotkeyAction>(i), Hotkey{});
            }
            WriteHotkey(GlobalIniPath(), action, key);
            recordingHotkey_ = -1;
        }
        Invalidate();
        return true;
    }
    if (searchFocused_) {
        if (vk == VK_ESCAPE) {
            search_.clear();
            searchFocused_ = false;
        } else if (vk == VK_BACK) {
            if (!search_.empty()) search_.pop_back();
        } else {
            return false;  // characters arrive as WM_CHAR
        }
        scroll_ = 0;
        Invalidate();
        return true;
    }
    if (vk == VK_ESCAPE && selected_ >= 0) {
        selected_ = -1;
        Invalidate();
        return true;
    }
    return false;
}

void MainWindow::OnChar(wchar_t ch) {
    if (!searchFocused_ || ch < 32 || search_.size() >= 60) return;
    search_ += ch;
    scroll_ = 0;
    Invalidate();
}

// ---- window -----------------------------------------------------------------------

LRESULT CALLBACK MainWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* created = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        created->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY && self) {
        // Closed (the tray app keeps running): drop everything tied to this window.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->canvas_ = ui::Canvas();
        self->stores_.clear();
        self->tracking_ = false;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            canvas_.Begin(ui::CurrentTheme());
            Draw(canvas_);
            canvas_.End();
            EndPaint(hwnd_, &ps);
            if (searchFocused_) SetTimer(hwnd_, 2, 530, nullptr);  // caret blink
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            const float scale = GetDpiForWindow(hwnd_) / 96.0f;
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {static_cast<LONG>(900 * scale), static_cast<LONG>(620 * scale)};
            return 0;
        }
        case WM_APP_SCANNED: {
            std::unique_ptr<std::vector<LibraryGame>> found(reinterpret_cast<std::vector<LibraryGame>*>(lp));
            // Keep the selection on the same game.
            std::wstring selectedKey;
            if (selected_ >= 0 && selected_ < static_cast<int>(games_.size())) selectedKey = games_[selected_].store + games_[selected_].id;
            games_ = std::move(*found);
            selected_ = -1;
            for (int i = 0; i < static_cast<int>(games_.size()); ++i) {
                if (games_[i].store + games_[i].id == selectedKey) selected_ = i;
            }
            scanning_ = false;
            Invalidate();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const float delta = -GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 96.0f;
            if (page_ == Page::Library) scroll_ = std::clamp(scroll_ + delta, 0.0f, maxScroll_);
            else settingsScroll_ = std::clamp(settingsScroll_ + delta, 0.0f, settingsMaxScroll_);
            Invalidate();
            return 0;
        }
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
                Invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            tipShown_ = false;
            canvas_.SetHover(0);
            Invalidate();
            return 0;
        case WM_TIMER:
            if (wp == kTipTimer) {
                KillTimer(hwnd_, kTipTimer);
                tipShown_ = canvas_.HoverId() != 0;
            } else {
                KillTimer(hwnd_, wp);
            }
            Invalidate();
            return 0;
        case WM_LBUTTONUP: {
            const float scale = canvas_.Scale();
            const int id = canvas_.HitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
            tipShown_ = false;
            if (id) {
                OnClick(id);
            } else if (searchFocused_ || recordingHotkey_ >= 0) {
                searchFocused_ = false;
                recordingHotkey_ = -1;
                Invalidate();
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKey(static_cast<UINT>(wp), true)) return 0;  // Alt+key while recording must not open the system menu
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (OnKey(static_cast<UINT>(wp), false)) return 0;
            break;
        case WM_SYSCHAR:
            if (recordingHotkey_ >= 0) return 0;  // no beep for Alt+key while recording
            break;
        case WM_CHAR:
            OnChar(static_cast<wchar_t>(wp));
            return 0;
        case WM_ACTIVATE:
            Invalidate();  // the master switch or modes may have changed from the tray meanwhile
            break;
        case WM_SETTINGCHANGE:
            ui::ApplyWindowStyle(hwnd_, ui::CurrentTheme().dark);
            Invalidate();
            break;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace openhdr
