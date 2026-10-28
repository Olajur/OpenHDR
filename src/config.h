#pragma once

#include "common.h"
#include "hotkeys.h"

#include <utility>
#include <vector>

namespace openhdr {

enum class InputTransfer : int { Gamma22 = 0, Srgb = 1 };
// Injected mode's swap chain format (see OutputMode in presenter.h).
enum class OutputFormatSetting : int { Auto = 0, ScRgb = 1, Hdr10 = 2 };

// Settings from %LOCALAPPDATA%\OpenHDR\OpenHDR.ini, overridden per game by games\<exe>.ini
// (written by the in-game panel). See OpenHDR.ini for what each value does.
struct Config {
    bool enabled = true;     // global switch, owned by the tray app
    bool conversion = true;  // per game: conversion on, or plain SDR at paper white
    bool forceHdr = false;
    float peakBrightness = 0.0f;  // nits, 0 = from display
    float middleGrey = 50.0f;     // nits
    float paperWhite = 0.0f;      // nits, 0 = derived from middleGrey
    // Picture sliders, -100..+100 (see itm_params.cpp for what they map to).
    int contrastLevel = 0;
    int saturationLevel = 0;
    int debanding = 0;  // 0 off, 1 low, 2 high
    float sceneAdaptation = 1.0f;  // 0 = fixed curve, 1 = full scene adaptation (see scene.h)
    bool sceneDebug = false;       // log the scene statistics every frame
    float shoulderStart = 0.25f;
    float expansionPower = 2.0f;
    float highlightSaturation = 0.5f;
    InputTransfer inputTransfer = InputTransfer::Gamma22;
    float dither = 1.0f;
    OutputFormatSetting outputFormat = OutputFormatSetting::Auto;
    float compareSplit = 0.5f;
    int dumpFrame = 0;
    HotkeySet hotkeys = DefaultHotkeys();  // [Hotkeys] in OpenHDR.ini
    uint32_t version = 0;  // incremented on every (re)load
};

// The settings of one game. Thread-safe; files are read on first use and re-read when they change.
class ConfigStore {
public:
    explicit ConfigStore(std::wstring gameExe);

    Config Get();
    void ReloadIfChanged();
    // Per-game overrides, applied immediately.
    void SetGameValue(const wchar_t* key, float value);
    void ResetGameValues();
    const std::wstring& GameExe() const { return gameExe_; }

private:
    void LoadLocked(bool log);

    std::wstring gameExe_;
    std::wstring gameIni_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    Config config_;
    FILETIME globalStamp_{};
    FILETIME gameStamp_{};
    bool loaded_ = false;
};

// The settings of the game this process is (for the injected mode).
ConfigStore& ProcessConfig();
inline Config GetConfig() { return ProcessConfig().Get(); }
inline void ReloadConfigIfChanged() { ProcessConfig().ReloadIfChanged(); }

// How OpenHDR handles a game listed under [Games] in OpenHDR.ini (the value: 1 or 2).
enum class GameMode : int { None = 0, Injected = 1, Capture = 2 };
GameMode GetGameMode(const std::wstring& exe);
void SetGameMode(const std::wstring& exe, GameMode mode);  // None removes the game
std::vector<std::pair<std::wstring, GameMode>> ListGames();
// Friendly name shown in the tray UI ([GameNames] in OpenHDR.ini), e.g. "Risk of Rain 2" for
// "risk of rain 2.exe". Falls back to the exe name without extension.
std::wstring GetGameDisplayName(const std::wstring& exe);
// Whether this process's exe is listed for the injected mode.
inline bool IsGameAllowed() { return GetGameMode(ExeName()) == GameMode::Injected; }

std::wstring GlobalIniPath();
// Lower-case file name of a path, e.g. "C:\Games\Foo.exe" -> "foo.exe".
std::wstring ExeFileName(const std::wstring& path);

// Peak used when PeakBrightness is 0: the display's reported peak, or 1000 nits if it reports nonsense.
float AutoPeakNits(float displayMaxNits);
float PeakNits(const Config& cfg, float autoPeakNits);
float PaperWhiteNits(const Config& cfg, float peakNits);

}  // namespace openhdr
