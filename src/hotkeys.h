#pragma once

#include "common.h"

#include <string>

namespace openhdr {

// The in-game shortcuts, both modes. Bound in OpenHDR.ini's [Hotkeys] section (or the main window),
// written like "Alt+1", "Ctrl+Shift+F11" or "None".
enum class HotkeyAction : int { Panel = 0, Conversion, Compare, Dump, Count };
constexpr int kHotkeyCount = static_cast<int>(HotkeyAction::Count);

struct Hotkey {
    UINT vk = 0;    // 0 = unbound
    UINT mods = 0;  // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    bool operator==(const Hotkey& o) const { return vk == o.vk && mods == o.mods; }
};

struct HotkeySet {
    Hotkey keys[kHotkeyCount];
    const Hotkey& operator[](HotkeyAction a) const { return keys[static_cast<int>(a)]; }
    Hotkey& operator[](HotkeyAction a) { return keys[static_cast<int>(a)]; }
};

HotkeySet DefaultHotkeys();
const wchar_t* HotkeyIniKey(HotkeyAction action);  // "Panel", "Conversion", ...
const wchar_t* HotkeyLabel(HotkeyAction action);   // "Settings panel", ...

// "Alt+1" <-> Hotkey. Parsing is case-insensitive; false for text that isn't a key combination.
bool ParseHotkey(const wchar_t* text, Hotkey& out);
std::wstring FormatHotkey(const Hotkey& key);
// The [Hotkeys] section of an ini file; actions without a (valid) entry keep their default.
HotkeySet ReadHotkeys(const std::wstring& iniPath);
void WriteHotkey(const std::wstring& iniPath, HotkeyAction action, const Hotkey& key);

// Detects presses: an action fires when its key goes down while exactly its modifiers are held.
// Key state is tracked on every poll (focused or not), so a key held across a focus change doesn't fire.
class HotkeyWatcher {
public:
    // Bit i set = action i was pressed since the last poll.
    uint32_t Poll(const HotkeySet& keys);

private:
    bool down_[kHotkeyCount] = {};
};

}  // namespace openhdr
