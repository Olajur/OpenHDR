#include "host/library.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace openhdr {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kExeSection[] = L"LibraryExe";

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

// Undoes the backslash escapes of Valve's KeyValues and JSON strings ("C:\\Games" -> "C:\Games").
std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

// Every "key" "value" pair in a Valve KeyValues file (libraryfolders.vdf, appmanifest_*.acf), flat.
std::vector<std::pair<std::string, std::string>> VdfPairs(const std::string& text) {
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' || text[i] == '}') {
            tokens.push_back(std::string(1, text[i]));
        } else if (text[i] == '"') {
            size_t j = i + 1;
            while (j < text.size() && text[j] != '"') j += text[j] == '\\' ? 2 : 1;
            tokens.push_back(Unescape(text.substr(i + 1, j - i - 1)));
            i = j;
        }
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] != "{" && tokens[i] != "}" && tokens[i + 1] != "{" && tokens[i + 1] != "}") {
            pairs.emplace_back(tokens[i], tokens[i + 1]);
            ++i;
        }
    }
    return pairs;
}

std::string VdfValue(const std::vector<std::pair<std::string, std::string>>& pairs, const char* key) {
    for (const auto& p : pairs) {
        if (_stricmp(p.first.c_str(), key) == 0) return p.second;
    }
    return {};
}

// The string value of "key" in a JSON document (first occurrence; enough for Epic's manifests).
std::string JsonString(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) return {};
    at = text.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    at = text.find('"', at);
    if (at == std::string::npos) return {};
    size_t end = at + 1;
    while (end < text.size() && text[end] != '"') end += text[end] == '\\' ? 2 : 1;
    return Unescape(text.substr(at + 1, end - at - 1));
}

std::wstring RegString(HKEY root, const wchar_t* path, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// Steam apps that aren't games.
bool IsSteamTool(const std::wstring& appId, const std::wstring& name) {
    static const std::set<std::wstring> ids = {L"228980", L"250820", L"1070560", L"1391110", L"1628350", L"1493710", L"2180100"};
    if (ids.count(appId)) return true;  // redistributables, SteamVR, Steam Linux Runtime, Proton
    const std::wstring n = Lower(name);
    return n.find(L"redistributable") != std::wstring::npos || n.rfind(L"proton ", 0) == 0 || n.find(L"steamworks") != std::wstring::npos;
}

// Executables that are never the game's own process.
bool IsHelperExe(const std::wstring& lowerName) {
    static const wchar_t* const parts[] = {
        L"unins",        L"setup",         L"install",       L"redist",     L"vcredist",   L"dxsetup",    L"directx",
        L"crash",        L"report",        L"prereq",        L"easyanticheat", L"eac_",    L"beservice",  L"battleye",
        L"dotnet",       L"python",        L"7z",            L"helper",     L"updater",    L"update.exe", L"cefprocess",
        L"webhelper",    L"overlay",       L"benchmark",     L"server.exe", L"dedicated",  L"editor",     L"modtool",
        L"touchup",      L"cleanup",       L"activation",    L"register",   L"config",     L"settings",   L"vrmonitor",
        L"unitycrash",   L"ue4prereq",     L"epicwebhelper", L"quicksfv",   L"oalinst",    L"physx",      L"dxwebsetup",
        // engine and modding tools shipped with games (Source, Source 2, ...)
        L"elementviewer", L"vconsole",     L"hammer",        L"studiomdl",  L"vrad",       L"vvis",       L"vbsp",
        L"resourcecompiler", L"compiler",  L"dmxconvert",    L"steamcmd",   L"tool",       L"sdk",
    };
    for (const wchar_t* p : parts) {
        if (lowerName.find(p) != std::wstring::npos) return true;
    }
    return false;
}

// The game's own process in an install folder: Unreal's *-Win64-Shipping.exe if there is one, else
// the exe whose name looks most like the game's (or its folder's) name, else the largest one.
std::wstring PickExe(const fs::path& dir, const std::wstring& gameName) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};
    struct Candidate {
        fs::path path;
        uintmax_t size;
        int depth;
    };
    std::vector<Candidate> candidates;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (int visited = 0; it != end && visited < 20000; it.increment(ec), ++visited) {
        if (ec) break;
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::wstring name = Lower(it->path().filename().wstring());
        if (name.size() < 5 || name.compare(name.size() - 4, 4, L".exe") != 0 || IsHelperExe(name)) continue;
        candidates.push_back({it->path(), it->file_size(ec), it.depth()});
    }
    if (candidates.empty()) return {};
    for (const Candidate& c : candidates) {
        const std::wstring name = Lower(c.path.filename().wstring());
        if (name.find(L"-win64-shipping.exe") != std::wstring::npos || name.find(L"-wingdk-shipping.exe") != std::wstring::npos) {
            return c.path.wstring();
        }
    }
    // Letters and digits only, for comparing "Half-Life 2" with "hl2.exe"-ish names loosely.
    auto key = [](const std::wstring& s) {
        std::wstring k;
        for (wchar_t ch : s) {
            if (iswalnum(ch)) k += static_cast<wchar_t>(towlower(ch));
        }
        return k;
    };
    const std::wstring gameKey = key(gameName), dirKey = key(dir.filename().wstring());
    const Candidate* best = nullptr;
    int bestScore = -1;
    for (const Candidate& c : candidates) {
        const std::wstring stem = key(c.path.stem().wstring());
        int score = 0;
        if (!stem.empty() && (stem == gameKey || stem == dirKey)) score += 4;
        else if (!stem.empty() && (gameKey.find(stem) != std::wstring::npos || dirKey.find(stem) != std::wstring::npos ||
                                   stem.find(dirKey) != std::wstring::npos)) score += 2;
        if (Lower(c.path.stem().wstring()).find(L"launcher") != std::wstring::npos) score -= 3;
        score += c.depth == 0 ? 2 : c.depth == 1 ? 1 : 0;  // the game's exe is usually near the top
        if (score > bestScore || (score == bestScore && c.size > best->size)) {
            best = &c;
            bestScore = score;
        }
    }
    return best ? best->path.wstring() : std::wstring();
}

void DetectUpscalers(LibraryGame& g) {
    // The install folder and the exe's folder (often bin\x64 or Binaries\Win64), where mods go.
    std::error_code ec;
    std::vector<fs::path> dirs = {g.installDir};
    if (!g.exePath.empty() && fs::path(g.exePath).parent_path() != fs::path(g.installDir)) dirs.push_back(fs::path(g.exePath).parent_path());
    for (const fs::path& d : dirs) {
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const std::wstring name = Lower(it->path().filename().wstring());
            if (name == L"optiscaler.ini" || name == L"optiscaler.dll") g.optiScaler = true;
            if (name == L"nvngx_dlss.dll" || name == L"nvngx_dlssg.dll" || name == L"sl.dlss.dll") g.dlss = true;
            if (name.find(L"ffx_fsr") == 0 || name.find(L"amd_fidelityfx") == 0 || name.find(L"fsr2") != std::wstring::npos) g.fsr = true;
            if (name == L"libxess.dll" || name == L"libxess_fg.dll") g.xess = true;
        }
    }
}

// Steam keeps cover art per app in appcache\librarycache: "<id>_library_600x900.jpg" (older) or
// "<id>\...\library_600x900.jpg" (newer, in a per-app folder).
std::wstring SteamCover(const fs::path& steam, const std::wstring& appId) {
    std::error_code ec;
    const fs::path cache = steam / L"appcache" / L"librarycache";
    for (const wchar_t* name : {L"_library_600x900_2x.jpg", L"_library_600x900.jpg"}) {
        const fs::path flat = cache / (appId + name);
        if (fs::exists(flat, ec)) return flat.wstring();
    }
    const fs::path folder = cache / appId;
    if (!fs::is_directory(folder, ec)) return {};
    std::wstring found;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::wstring name = Lower(it->path().filename().wstring());
        if (name == L"library_600x900_2x.jpg") return it->path().wstring();
        if (name == L"library_600x900.jpg" || (found.empty() && name == L"library_capsule.jpg")) found = it->path().wstring();
    }
    return found;
}

void ScanSteam(std::vector<LibraryGame>& out) {
    std::wstring steamPath = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steamPath.empty()) steamPath = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steamPath.empty()) return;
    const fs::path steam = fs::path(steamPath).make_preferred();
    std::vector<fs::path> libraries = {steam};
    for (const auto& p : VdfPairs(ReadFile(steam / L"steamapps" / L"libraryfolders.vdf"))) {
        if (_stricmp(p.first.c_str(), "path") == 0) libraries.push_back(fs::path(Widen(p.second)).make_preferred());
    }
    std::set<std::wstring> seen;
    std::error_code ec;
    for (const fs::path& lib : libraries) {
        const fs::path apps = lib / L"steamapps";
        if (!seen.insert(Lower(apps.wstring())).second) continue;
        for (fs::directory_iterator it(apps, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const std::wstring file = Lower(it->path().filename().wstring());
            if (file.rfind(L"appmanifest_", 0) != 0 || it->path().extension() != L".acf") continue;
            const auto kv = VdfPairs(ReadFile(it->path()));
            LibraryGame g;
            g.store = L"Steam";
            g.id = Widen(VdfValue(kv, "appid"));
            g.name = Widen(VdfValue(kv, "name"));
            const std::wstring installDir = Widen(VdfValue(kv, "installdir"));
            if (g.id.empty() || g.name.empty() || installDir.empty() || IsSteamTool(g.id, g.name)) continue;
            g.installDir = (apps / L"common" / installDir).wstring();
            g.coverPath = SteamCover(steam, g.id);
            out.push_back(std::move(g));
        }
    }
}

void ScanEpic(std::vector<LibraryGame>& out) {
    wchar_t programData[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH)) return;
    const fs::path manifests = fs::path(programData) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
    std::error_code ec;
    for (fs::directory_iterator it(manifests, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (Lower(it->path().extension().wstring()) != L".item") continue;
        const std::string json = ReadFile(it->path());
        LibraryGame g;
        g.store = L"Epic";
        g.id = Widen(JsonString(json, "AppName"));
        g.name = Widen(JsonString(json, "DisplayName"));
        g.installDir = fs::path(Widen(JsonString(json, "InstallLocation"))).make_preferred().wstring();
        const std::wstring launch = Widen(JsonString(json, "LaunchExecutable"));
        if (g.name.empty() || g.installDir.empty()) continue;
        if (json.find("\"bIsApplication\": false") != std::string::npos) continue;  // DLC and add-ons
        if (!launch.empty()) {
            const fs::path exe = (fs::path(g.installDir) / launch).make_preferred();
            if (fs::exists(exe, ec)) g.exePath = exe.wstring();
        }
        out.push_back(std::move(g));
    }
}

void ScanGog(std::vector<LibraryGame>& out) {
    HKEY games = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &games) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0; RegEnumKeyW(games, i, sub, 256) == ERROR_SUCCESS; ++i) {
        const std::wstring key = std::wstring(L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + sub;
        LibraryGame g;
        g.store = L"GOG";
        g.id = sub;
        g.name = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"gameName");
        g.installDir = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"path");
        g.exePath = RegString(HKEY_LOCAL_MACHINE, key.c_str(), L"exe");
        if (g.name.empty() || g.installDir.empty()) continue;
        out.push_back(std::move(g));
    }
    RegCloseKey(games);
}

std::wstring LibraryKey(const LibraryGame& g) { return g.store + L":" + g.id; }

}  // namespace

std::wstring LibraryGame::ExeName() const {
    if (exePath.empty()) return id;
    const size_t slash = exePath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? exePath : exePath.substr(slash + 1);
}

std::vector<LibraryGame> ScanLibrary() {
    std::vector<LibraryGame> games;
    ScanSteam(games);
    ScanEpic(games);
    ScanGog(games);

    const std::wstring ini = GlobalIniPath();
    for (LibraryGame& g : games) {
        wchar_t chosen[MAX_PATH];
        GetPrivateProfileStringW(kExeSection, LibraryKey(g).c_str(), L"", chosen, MAX_PATH, ini.c_str());
        std::error_code ec;
        if (chosen[0] && fs::exists(chosen, ec)) g.exePath = chosen;
        if (g.exePath.empty()) g.exePath = PickExe(g.installDir, g.name);
        DetectUpscalers(g);
    }
    games.erase(std::remove_if(games.begin(), games.end(), [](const LibraryGame& g) { return g.exePath.empty(); }), games.end());

    // Games listed in [Games] that no store reports (added by hand) appear as manual entries.
    std::set<std::wstring> known;
    for (const LibraryGame& g : games) known.insert(Lower(g.ExeName()));
    for (const auto& listed : ListGames()) {
        if (known.count(Lower(listed.first))) continue;
        LibraryGame g;
        g.store = L"Manual";
        g.id = listed.first;
        g.name = GetGameDisplayName(listed.first);
        games.push_back(std::move(g));
    }
    std::sort(games.begin(), games.end(), [](const LibraryGame& a, const LibraryGame& b) { return Lower(a.name) < Lower(b.name); });
    return games;
}

void SetLibraryExe(const LibraryGame& game, const std::wstring& exePath) {
    if (game.store == L"Manual") return;
    WritePrivateProfileStringW(kExeSection, LibraryKey(game).c_str(), exePath.c_str(), GlobalIniPath().c_str());
}

}  // namespace openhdr
