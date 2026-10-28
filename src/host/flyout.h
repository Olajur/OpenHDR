#pragma once

#include "config.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace openhdr {

// What the flyout shows; rebuilt each time it opens and after every action.
struct FlyoutModel {
    bool enabled = true;
    bool autostart = false;
    ui::ThemeMode theme = ui::ThemeMode::System;
    bool hdrOn = false;
    float peakNits = 0.0f;
    std::vector<std::pair<std::wstring, GameMode>> games;
};

// What the flyout's controls do (implemented by the tray app).
struct FlyoutActions {
    std::function<FlyoutModel()> load;
    std::function<void(bool)> setEnabled;
    std::function<void(bool)> setAutostart;
    std::function<void(ui::ThemeMode)> setTheme;
    std::function<void(const std::wstring&, GameMode)> setGameMode;  // GameMode::None removes the game
    std::function<bool(HWND owner)> addGame;                         // true if a game was added
    std::function<void()> openSettings;
    std::function<void()> openLogs;
    std::function<void()> quit;
};

// The tray icon's panel, in the Windows 11 quick-settings style: master switch, display status,
// games with their mode, and app actions. Opens above the taskbar and closes when it loses focus.
class Flyout {
public:
    explicit Flyout(FlyoutActions actions);
    ~Flyout();

    void Toggle();
    void Show();
    void Hide();
    // Renders the panel into a PNG (no window) for checking the design; hoverId shows that control's tooltip.
    bool Snapshot(const std::wstring& path, float scale, int hoverId = 0);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    float ContentHeight() const;
    void Draw(ui::Canvas& canvas);
    void OnClick(int id);
    void Resize();

    FlyoutActions actions_;
    FlyoutModel model_;
    ui::Canvas canvas_;
    HWND hwnd_ = nullptr;
    ULONGLONG hiddenAt_ = 0;  // a click on the tray icon right after closing must not reopen it
    bool tracking_ = false;
    bool tipShown_ = false;  // the hovered control's tooltip, after a short delay
};

}  // namespace openhdr
