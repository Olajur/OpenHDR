#pragma once

#include "config.h"
#include "host/library.h"
#include "ui/ui.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace openhdr {

// What the main window can't do on its own (implemented by the tray app).
struct MainWindowActions {
    std::function<bool()> enabled;
    std::function<void(bool)> setEnabled;
    std::function<bool()> autostart;
    std::function<void(bool)> setAutostart;
    std::function<void(ui::ThemeMode)> setTheme;
    // A game's mode changed (GameMode::None = removed): the tray app updates its hook and capture.
    std::function<void()> gamesChanged;
    std::function<bool(HWND owner)> addGameManually;
    std::function<float()> displayPeakNits;  // 0 = HDR is off
    std::function<void()> openLogs;
    std::function<void()> openIni;
};

// OpenHDR's main window: the game library (detected from Steam, Epic and GOG, with covers) where
// games are added and set up, and the settings page (defaults for all games, hotkeys, app options).
class MainWindow {
public:
    explicit MainWindow(MainWindowActions actions);
    ~MainWindow();

    void Show();  // opens it, or brings it to the front
    // Renders a page into a PNG without a window (design checks): page 0 library, 1 settings;
    // `selected` >= 0 opens that game's panel.
    bool Snapshot(const std::wstring& path, int page, int selected, float scale);

private:
    enum class Page { Library, Settings };
    enum class Filter { All, Added, NotAdded };

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Draw(ui::Canvas& c);
    void DrawSidebar(ui::Canvas& c, float h);
    void DrawLibrary(ui::Canvas& c, D2D1_RECT_F area);
    void DrawCard(ui::Canvas& c, const LibraryGame& g, int index, D2D1_RECT_F rect);
    void DrawGamePanel(ui::Canvas& c, D2D1_RECT_F area);
    void DrawSettings(ui::Canvas& c, D2D1_RECT_F area);
    // The picture settings block, for one game (exe) or the defaults (exe empty). Returns its height.
    float DrawPictureSettings(ui::Canvas& c, float x, float y, float w, const std::wstring& exe, int idBase);
    void OnClick(int id);
    void OnPictureClick(int id, int idBase, const std::wstring& exe);
    bool OnKey(UINT vk, bool down);
    void OnChar(wchar_t ch);
    void StartScan();
    void Invalidate();

    std::vector<int> VisibleGames() const;
    GameMode ModeOf(const LibraryGame& g) const;
    void SetMode(const LibraryGame& g, GameMode mode);
    void ChangeExe(LibraryGame& g);
    ConfigStore& Store(const std::wstring& exe);
    void WriteSetting(const std::wstring& exe, const wchar_t* key, const std::wstring& value);

    MainWindowActions actions_;
    ui::Canvas canvas_;
    HWND hwnd_ = nullptr;
    Page page_ = Page::Library;
    Filter filter_ = Filter::All;
    std::vector<LibraryGame> games_;
    bool scanning_ = false;
    int selected_ = -1;  // index into games_
    float scroll_ = 0.0f, maxScroll_ = 0.0f;
    float settingsScroll_ = 0.0f, settingsMaxScroll_ = 0.0f;
    std::wstring search_;
    bool searchFocused_ = false;
    int recordingHotkey_ = -1;  // the hotkey row waiting for a key combination
    bool tracking_ = false;
    bool tipShown_ = false;
    std::map<std::wstring, std::unique_ptr<ConfigStore>> stores_;
};

}  // namespace openhdr
