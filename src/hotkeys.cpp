#include "hotkeys.h"

#include <cwctype>
#include <iterator>
#include <vector>

namespace openhdr {
namespace {

constexpr wchar_t kSection[] = L"Hotkeys";

struct ActionInfo {
    const wchar_t* iniKey;
    const wchar_t* label;
    Hotkey fallback;
};
const ActionInfo kActions[kHotkeyCount] = {
    {L"Panel", L"Settings panel", {'1', MOD_ALT}},
    {L"Conversion", L"HDR conversion on/off", {'2', MOD_ALT}},
    {L"Compare", L"Split-screen compare", {'3', MOD_ALT}},
    {L"Dump", L"Dump frame (debug)", {VK_F11, MOD_CONTROL | MOD_SHIFT}},
};

struct KeyName {
    UINT vk;
    const wchar_t* name;
};
const KeyName kNames[] = {
    {VK_SPACE, L"Space"},   {VK_TAB, L"Tab"},         {VK_RETURN, L"Enter"},   {VK_BACK, L"Backspace"},
    {VK_INSERT, L"Insert"}, {VK_DELETE, L"Delete"},   {VK_HOME, L"Home"},      {VK_END, L"End"},
    {VK_PRIOR, L"PageUp"},  {VK_NEXT, L"PageDown"},   {VK_PAUSE, L"Pause"},    {VK_SCROLL, L"ScrollLock"},
    {VK_OEM_3, L"`"},       {VK_OEM_MINUS, L"-"},     {VK_OEM_PLUS, L"="},     {VK_OEM_4, L"["},
    {VK_OEM_6, L"]"},       {VK_OEM_5, L"\\"},        {VK_OEM_1, L";"},        {VK_OEM_7, L"'"},
    {VK_OEM_COMMA, L","},   {VK_OEM_PERIOD, L"."},    {VK_OEM_2, L"/"},        {VK_MULTIPLY, L"Num*"},
    {VK_ADD, L"Num+"},      {VK_SUBTRACT, L"Num-"},   {VK_DIVIDE, L"Num/"},    {VK_DECIMAL, L"Num."},
};

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

UINT HeldModifiers() {
    return (Down(VK_MENU) ? MOD_ALT : 0) | (Down(VK_CONTROL) ? MOD_CONTROL : 0) | (Down(VK_SHIFT) ? MOD_SHIFT : 0) |
           (Down(VK_LWIN) || Down(VK_RWIN) ? MOD_WIN : 0);
}

bool ParseKey(const std::wstring& token, UINT& vk) {
    if (token.size() == 1 && iswalnum(token[0])) {
        vk = towupper(token[0]);
        return true;
    }
    if (token.size() >= 2 && (token[0] == L'F' || token[0] == L'f') && iswdigit(token[1])) {
        const int n = _wtoi(token.c_str() + 1);
        if (n >= 1 && n <= 24) {
            vk = VK_F1 + n - 1;
            return true;
        }
    }
    if (token.size() == 4 && !_wcsnicmp(token.c_str(), L"Num", 3) && iswdigit(token[3])) {
        vk = VK_NUMPAD0 + (token[3] - L'0');
        return true;
    }
    for (const KeyName& k : kNames) {
        if (!_wcsicmp(token.c_str(), k.name)) {
            vk = k.vk;
            return true;
        }
    }
    return false;
}

}  // namespace

HotkeySet DefaultHotkeys() {
    HotkeySet set;
    for (int i = 0; i < kHotkeyCount; ++i) set.keys[i] = kActions[i].fallback;
    return set;
}

const wchar_t* HotkeyIniKey(HotkeyAction action) { return kActions[static_cast<int>(action)].iniKey; }
const wchar_t* HotkeyLabel(HotkeyAction action) { return kActions[static_cast<int>(action)].label; }

bool ParseHotkey(const wchar_t* text, Hotkey& out) {
    std::wstring s(text ? text : L"");
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    if (!_wcsicmp(s.c_str(), L"None")) {
        out = {};
        return true;
    }
    // Split on '+'; the '+' of "Num+" at the end belongs to the key name.
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'+' && !current.empty() && !(i + 1 == s.size() && !_wcsicmp(current.c_str(), L"Num"))) {
            tokens.push_back(current);
            current.clear();
        } else if (!iswspace(s[i])) {
            current += s[i];
        }
    }
    if (!current.empty()) tokens.push_back(current);

    Hotkey key;
    for (const std::wstring& token : tokens) {
        if (!_wcsicmp(token.c_str(), L"Alt")) key.mods |= MOD_ALT;
        else if (!_wcsicmp(token.c_str(), L"Ctrl") || !_wcsicmp(token.c_str(), L"Control")) key.mods |= MOD_CONTROL;
        else if (!_wcsicmp(token.c_str(), L"Shift")) key.mods |= MOD_SHIFT;
        else if (!_wcsicmp(token.c_str(), L"Win")) key.mods |= MOD_WIN;
        else if (key.vk || !ParseKey(token, key.vk)) return false;  // one main key only
    }
    if (!key.vk) return false;
    out = key;
    return true;
}

std::wstring FormatHotkey(const Hotkey& key) {
    if (!key.vk) return L"None";
    std::wstring s;
    if (key.mods & MOD_CONTROL) s += L"Ctrl+";
    if (key.mods & MOD_ALT) s += L"Alt+";
    if (key.mods & MOD_SHIFT) s += L"Shift+";
    if (key.mods & MOD_WIN) s += L"Win+";
    if ((key.vk >= '0' && key.vk <= '9') || (key.vk >= 'A' && key.vk <= 'Z')) {
        s += static_cast<wchar_t>(key.vk);
    } else if (key.vk >= VK_F1 && key.vk <= VK_F24) {
        s += L"F" + std::to_wstring(key.vk - VK_F1 + 1);
    } else if (key.vk >= VK_NUMPAD0 && key.vk <= VK_NUMPAD9) {
        s += L"Num" + std::to_wstring(key.vk - VK_NUMPAD0);
    } else {
        const wchar_t* name = nullptr;
        for (const KeyName& k : kNames) {
            if (k.vk == key.vk) name = k.name;
        }
        s += name ? name : (L"#" + std::to_wstring(key.vk)).c_str();
    }
    return s;
}

HotkeySet ReadHotkeys(const std::wstring& iniPath) {
    HotkeySet set = DefaultHotkeys();
    for (int i = 0; i < kHotkeyCount; ++i) {
        wchar_t buf[64];
        GetPrivateProfileStringW(kSection, kActions[i].iniKey, L"", buf, 64, iniPath.c_str());
        Hotkey key;
        if (buf[0] && ParseHotkey(buf, key)) set.keys[i] = key;
    }
    return set;
}

void WriteHotkey(const std::wstring& iniPath, HotkeyAction action, const Hotkey& key) {
    WritePrivateProfileStringW(kSection, HotkeyIniKey(action), FormatHotkey(key).c_str(), iniPath.c_str());
}

uint32_t HotkeyWatcher::Poll(const HotkeySet& keys) {
    uint32_t pressed = 0;
    const UINT held = HeldModifiers();
    for (int i = 0; i < kHotkeyCount; ++i) {
        const Hotkey& key = keys.keys[i];
        const bool isDown = key.vk && Down(static_cast<int>(key.vk));
        if (isDown && !down_[i] && held == key.mods) pressed |= 1u << i;
        down_[i] = isDown;
    }
    return pressed;
}

}  // namespace openhdr
