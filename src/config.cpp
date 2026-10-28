#include "config.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace openhdr {
namespace {

constexpr wchar_t kSection[] = L"OpenHDR";
constexpr wchar_t kGamesSection[] = L"Games";
constexpr wchar_t kGameNamesSection[] = L"GameNames";

FILETIME Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return {};
    return attr.ftLastWriteTime;
}

// Reads a key from the per-game file first, then from the global file.
class Reader {
public:
    Reader(const std::wstring& game, const std::wstring& global) : game_(game), global_(global) {}

    float Float(const wchar_t* key, float def) const {
        wchar_t buf[64];
        return Raw(key, buf, 64) ? static_cast<float>(_wtof(buf)) : def;
    }
    bool Bool(const wchar_t* key, bool def) const {
        wchar_t buf[16];
        return Raw(key, buf, 16) ? _wtoi(buf) != 0 : def;
    }
    int Int(const wchar_t* key, int def) const {
        wchar_t buf[16];
        return Raw(key, buf, 16) ? _wtoi(buf) : def;
    }
    bool Raw(const wchar_t* key, wchar_t* buf, DWORD size) const {
        GetPrivateProfileStringW(kSection, key, L"", buf, size, game_.c_str());
        if (buf[0]) return true;
        GetPrivateProfileStringW(kSection, key, L"", buf, size, global_.c_str());
        return buf[0] != 0;
    }

private:
    const std::wstring& game_;
    const std::wstring& global_;
};

}  // namespace

std::wstring GlobalIniPath() { return DataDir() + L"\\OpenHDR.ini"; }

std::wstring ExeFileName(const std::wstring& path) {
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name;
}

ConfigStore::ConfigStore(std::wstring gameExe)
    : gameExe_(std::move(gameExe)), gameIni_(DataDir() + L"\\games\\" + gameExe_ + L".ini") {}

// Caller holds lock_ exclusively.
void ConfigStore::LoadLocked(bool log) {
    const std::wstring global = GlobalIniPath();
    const Reader r(gameIni_, global);
    Config c;
    c.enabled = GetPrivateProfileIntW(kSection, L"Enabled", 1, global.c_str()) != 0;
    c.conversion = r.Bool(L"Conversion", c.conversion);
    c.forceHdr = r.Bool(L"ForceHDR", c.forceHdr);
    c.peakBrightness = std::max(r.Float(L"PeakBrightness", c.peakBrightness), 0.0f);
    c.middleGrey = std::max(r.Float(L"MiddleGrey", c.middleGrey), 1.0f);
    c.paperWhite = std::max(r.Float(L"PaperWhite", c.paperWhite), 0.0f);
    // New keys: the old Contrast/Saturation multipliers (0.5..2) meant something else.
    c.contrastLevel = std::clamp(r.Int(L"ContrastLevel", c.contrastLevel), -100, 100);
    c.saturationLevel = std::clamp(r.Int(L"SaturationLevel", c.saturationLevel), -100, 100);
    c.sceneAdaptation = std::clamp(r.Float(L"SceneAdaptation", c.sceneAdaptation), 0.0f, 1.0f);
    c.sceneDebug = r.Bool(L"SceneDebug", c.sceneDebug);
    wchar_t deband[16];
    if (r.Raw(L"Debanding", deband, 16)) {
        c.debanding = !_wcsicmp(deband, L"high") ? 2 : !_wcsicmp(deband, L"low") ? 1 : std::clamp(_wtoi(deband), 0, 2);
    }
    c.shoulderStart = std::clamp(r.Float(L"ShoulderStart", c.shoulderStart), 0.0f, 0.95f);
    c.expansionPower = std::clamp(r.Float(L"ExpansionPower", c.expansionPower), 1.0f, 8.0f);
    c.highlightSaturation = std::clamp(r.Float(L"HighlightSaturation", c.highlightSaturation), 0.0f, 1.0f);
    c.dither = std::clamp(r.Float(L"Dither", c.dither), 0.0f, 4.0f);
    c.compareSplit = std::clamp(r.Float(L"CompareSplit", c.compareSplit), 0.0f, 1.0f);
    c.dumpFrame = std::max(r.Int(L"DumpFrame", 0), 0);
    c.hotkeys = ReadHotkeys(global);

    wchar_t transfer[32];
    if (!r.Raw(L"InputTransfer", transfer, 32)) wcscpy_s(transfer, L"gamma22");
    c.inputTransfer = _wcsicmp(transfer, L"srgb") == 0 ? InputTransfer::Srgb : InputTransfer::Gamma22;
    wchar_t output[16];
    if (r.Raw(L"OutputFormat", output, 16)) {
        c.outputFormat = !_wcsicmp(output, L"hdr10")   ? OutputFormatSetting::Hdr10
                         : !_wcsicmp(output, L"scrgb") ? OutputFormatSetting::ScRgb
                                                       : OutputFormatSetting::Auto;
    }

    c.version = config_.version + 1;
    config_ = c;
    globalStamp_ = Stamp(global);
    gameStamp_ = Stamp(gameIni_);
    loaded_ = true;
    if (log) {
        Log("Config v%u for %ls: Enabled=%d Conversion=%d ForceHDR=%d Peak=%.0f MiddleGrey=%.1f PaperWhite=%.0f "
            "Contrast=%+d Saturation=%+d Debanding=%d Shoulder=%.2f Power=%.2f HighlightSat=%.2f Transfer=%s Dither=%.2f",
            c.version, gameExe_.c_str(), c.enabled, c.conversion, c.forceHdr, c.peakBrightness, c.middleGrey,
            c.paperWhite, c.contrastLevel, c.saturationLevel, c.debanding, c.shoulderStart, c.expansionPower, c.highlightSaturation,
            c.inputTransfer == InputTransfer::Srgb ? "srgb" : "gamma22", c.dither);
    }
}

Config ConfigStore::Get() {
    AcquireSRWLockExclusive(&lock_);
    if (!loaded_) LoadLocked(true);
    Config c = config_;
    ReleaseSRWLockExclusive(&lock_);
    return c;
}

void ConfigStore::ReloadIfChanged() {
    AcquireSRWLockExclusive(&lock_);
    FILETIME global = Stamp(GlobalIniPath());
    FILETIME game = Stamp(gameIni_);
    if (!loaded_ || CompareFileTime(&global, &globalStamp_) != 0 || CompareFileTime(&game, &gameStamp_) != 0) {
        LoadLocked(true);
    }
    ReleaseSRWLockExclusive(&lock_);
}

void ConfigStore::SetGameValue(const wchar_t* key, float value) {
    wchar_t buf[32];
    swprintf_s(buf, L"%g", value);
    AcquireSRWLockExclusive(&lock_);
    WritePrivateProfileStringW(kSection, key, buf, gameIni_.c_str());
    LoadLocked(false);
    ReleaseSRWLockExclusive(&lock_);
}

void ConfigStore::ResetGameValues() {
    AcquireSRWLockExclusive(&lock_);
    WritePrivateProfileStringW(kSection, nullptr, nullptr, gameIni_.c_str());
    LoadLocked(true);
    ReleaseSRWLockExclusive(&lock_);
}

ConfigStore& ProcessConfig() {
    static ConfigStore store(ExeName());
    return store;
}

GameMode GetGameMode(const std::wstring& exe) {
    const UINT value = GetPrivateProfileIntW(kGamesSection, exe.c_str(), 0, GlobalIniPath().c_str());
    return value == 1 ? GameMode::Injected : value == 2 ? GameMode::Capture : GameMode::None;
}

void SetGameMode(const std::wstring& exe, GameMode mode) {
    const wchar_t* value = mode == GameMode::Injected ? L"1" : mode == GameMode::Capture ? L"2" : nullptr;
    WritePrivateProfileStringW(kGamesSection, ExeFileName(exe).c_str(), value, GlobalIniPath().c_str());
    if (mode == GameMode::None) WritePrivateProfileStringW(kGameNamesSection, ExeFileName(exe).c_str(), nullptr, GlobalIniPath().c_str());
}

std::wstring GetGameDisplayName(const std::wstring& exe) {
    wchar_t name[128];
    GetPrivateProfileStringW(kGameNamesSection, exe.c_str(), L"", name, 128, GlobalIniPath().c_str());
    if (name[0]) return name;
    std::wstring fallback = exe.substr(0, exe.rfind(L'.'));
    if (!fallback.empty()) fallback[0] = static_cast<wchar_t>(std::towupper(fallback[0]));
    return fallback;
}

std::vector<std::pair<std::wstring, GameMode>> ListGames() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(kGamesSection, buf.data(), static_cast<DWORD>(buf.size()), GlobalIniPath().c_str());
    std::vector<std::pair<std::wstring, GameMode>> games;
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        const std::wstring entry(p);
        const size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos) continue;
        const int value = _wtoi(entry.c_str() + eq + 1);
        if (value == 1 || value == 2) games.emplace_back(entry.substr(0, eq), static_cast<GameMode>(value));
    }
    return games;
}

float AutoPeakNits(float displayMaxNits) { return displayMaxNits >= 400.0f ? displayMaxNits : 1000.0f; }

float PeakNits(const Config& cfg, float autoPeakNits) {
    return cfg.peakBrightness > 0.0f ? cfg.peakBrightness : autoPeakNits;
}

float PaperWhiteNits(const Config& cfg, float peakNits) {
    return std::min(cfg.paperWhite > 0.0f ? cfg.paperWhite : cfg.middleGrey / 0.18f, peakNits);
}

}  // namespace openhdr
