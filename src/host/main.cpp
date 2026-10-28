// OpenHDR.exe - tray app. Each game in [Games] of %LOCALAPPDATA%\OpenHDR\OpenHDR.ini has a mode:
//
//   Capture (2):  the default. Nothing runs inside the game. While the game's window is in the
//                 foreground, this app captures it and shows the converted image over it
//                 (see capture/capture.cpp).
//   Injected (1): opt-in. A global WH_CBT hook makes Windows load OpenHDR64.dll into newly started
//                 GUI processes; the DLL converts the swap chain from inside the game. Lowest latency,
//                 exclusive fullscreen works. The hook is installed only while such games are listed.
//
// Disabling OpenHDR removes the hook and stops capture, so Windows Auto HDR can take over.
//
//   OpenHDR.exe                              run in the tray and open the main window (or bring up
//                                            the running instance's)
//   OpenHDR.exe --tray                       run in the tray only (autostart)
//   OpenHDR.exe --add-game x.exe             add a game (capture mode) and exit
//   OpenHDR.exe --add-game-injected x.exe    add a game (injected mode) and exit
//   OpenHDR.exe --remove-game x.exe          remove a game and exit
//   OpenHDR.exe --exit                       close the running instance
#include "capture/capture.h"
#include "common.h"
#include "config.h"
#include "display.h"
#include "host/flyout.h"
#include "host/library.h"
#include "host/main_window.h"

#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>

#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace openhdr;

namespace {

constexpr wchar_t kWindowClass[] = L"OpenHDR.Host";
constexpr wchar_t kSection[] = L"OpenHDR";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kCaptureTimer = 1;
constexpr UINT kCaptureCheckMs = 250;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HINSTANCE g_instance = nullptr;
HOOKPROC g_hookProc = nullptr;
HMODULE g_core = nullptr;
HHOOK g_hook = nullptr;
NOTIFYICONDATAW g_tray{};
HICON g_iconOn = nullptr;
HICON g_iconOff = nullptr;
UINT g_taskbarCreated = 0;
HWND g_hwnd = nullptr;
std::unique_ptr<Flyout> g_flyout;
std::unique_ptr<MainWindow> g_main;
UINT g_showMainMessage = 0;  // sent by a second OpenHDR.exe started without --tray
std::unique_ptr<CaptureSession> g_capture;
// A window that refused capture: Windows sometimes does right after a window appears, so it is
// retried a few times, a second apart, but not every 250 ms forever.
HWND g_failedCaptureWindow = nullptr;
int g_captureFailures = 0;
ULONGLONG g_captureFailedAt = 0;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

// Windows loads a global hook's DLL into each process by file path and calls the hook at the offset
// it has in the installing process. If OpenHDR64.dll were replaced (an update or a rebuild) while
// this app runs, new processes would load the new file at the old offset and crash. So the hook
// always uses a private copy named after the build's size and timestamp, which is never replaced.
std::wstring PrivateCoreCopy() {
    const std::wstring source = ExeDir() + L"\\OpenHDR64.dll";
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &attr)) return source;
    wchar_t name[64];
    swprintf_s(name, L"OpenHDR64-%08lx%08lx-%lx.dll", attr.ftLastWriteTime.dwHighDateTime, attr.ftLastWriteTime.dwLowDateTime,
               attr.nFileSizeLow);
    const std::wstring dir = DataDir() + L"\\bin";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring copy = dir + L"\\" + name;

    // Remove copies of older builds; any still loaded somewhere fail to delete and are skipped.
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((dir + L"\\OpenHDR64-*.dll").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (_wcsicmp(found.cFileName, name) != 0) DeleteFileW((dir + L"\\" + found.cFileName).c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(source.c_str(), copy.c_str(), TRUE)) {
        return source;
    }
    return copy;
}

// First run: start from the commented template shipped next to the exe.
void EnsureGlobalIni() {
    const std::wstring ini = GlobalIniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CopyFileW((ExeDir() + L"\\OpenHDR.ini").c_str(), ini.c_str(), TRUE);
    }
}

bool IsEnabled() { return GetPrivateProfileIntW(kSection, L"Enabled", 1, GlobalIniPath().c_str()) != 0; }

// Reads the product name from an exe's version resource (or FileDescription), e.g. "Risk of Rain 2",
// and remembers it as the game's display name in [GameNames].
void RememberGameDisplayName(const std::wstring& exePath) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &handle);
    if (!size) return;
    std::vector<uint8_t> info(size);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, size, info.data())) return;
    struct Translation {
        WORD language, codePage;
    }* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes) || !bytes) return;
    for (const wchar_t* field : {L"ProductName", L"FileDescription"}) {
        wchar_t query[96];
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].codePage, field);
        wchar_t* value = nullptr;
        UINT length = 0;
        if (VerQueryValueW(info.data(), query, reinterpret_cast<void**>(&value), &length) && length > 1 && value[0]) {
            WritePrivateProfileStringW(L"GameNames", ExeFileName(exePath).c_str(), value, GlobalIniPath().c_str());
            return;
        }
    }
}

// Light/dark preference for OpenHDR's own windows (Theme= in OpenHDR.ini).
ui::ThemeMode ReadThemeMode() {
    wchar_t value[16];
    GetPrivateProfileStringW(kSection, L"Theme", L"system", value, 16, GlobalIniPath().c_str());
    return ui::ParseThemeMode(value);
}

void SetThemeMode(ui::ThemeMode mode) {
    WritePrivateProfileStringW(kSection, L"Theme", ui::ThemeModeName(mode), GlobalIniPath().c_str());
    ui::SetThemeMode(mode);
}

bool IsAutostart() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"OpenHDR", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"OpenHDR");
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring command = L"\"" + std::wstring(exe) + L"\" --tray";  // sign-in: tray only
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"OpenHDR", REG_SZ, command.c_str(),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
}

bool AnyInjectedGame() {
    for (const auto& game : ListGames()) {
        if (game.second == GameMode::Injected) return true;
    }
    return false;
}

// Tray icons from the exe's resources (host.rc): 1 = enabled (orange), 2 = disabled (grey).
HICON LoadTrayIcon(bool on) {
    return static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(on ? 1 : 2), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
}

void UpdateTray(bool add) {
    const bool on = IsEnabled();
    g_tray.hIcon = on ? g_iconOn : g_iconOff;
    wcscpy_s(g_tray.szTip, on ? L"OpenHDR: on" : L"OpenHDR: off");
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_tray);
}

// The global hook is only installed while OpenHDR is enabled and some game uses the injected mode,
// so otherwise no other process gets the DLL at all.
bool ApplyHookState() {
    const bool want = IsEnabled() && AnyInjectedGame();
    if (want && !g_hook) {
        g_hook = SetWindowsHookExW(WH_CBT, g_hookProc, g_core, 0);
        if (!g_hook) return false;
        Log("Global hook installed (injected-mode games are listed)");
    } else if (!want && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        Log("Global hook removed");
    }
    return true;
}

// Lower-case exe name of the process owning `hwnd`, or empty.
// Full path of the exe owning `hwnd`, or empty.
std::wstring WindowExePath(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    return ok ? std::wstring(path, size) : std::wstring();
}

std::wstring WindowExe(HWND hwnd) {
    const std::wstring path = WindowExePath(hwnd);
    return path.empty() ? path : ExeFileName(path);
}

bool IsCaptureCandidate(HWND root, std::wstring& exe) {
    RECT rect{};
    if (!root || !IsWindowVisible(root) || IsIconic(root) || !GetClientRect(root, &rect) || rect.right < 320 || rect.bottom < 240) {
        return false;
    }
    exe = WindowExe(root);
    return !exe.empty() && GetGameMode(exe) == GameMode::Capture;
}

// Automated tests run from a background process, whose windows Windows won't bring to the front.
bool CaptureAnyWindow() {
    wchar_t value[4];
    return GetEnvironmentVariableW(L"OPENHDR_CAPTURE_ANY_WINDOW", value, 4) > 0 && value[0] == L'1';
}

// Capture follows the foreground window: it runs while a capture-mode game's window is in front.
// (Our topmost window would otherwise cover whatever the user puts over the game.)
void UpdateCapture() {
    if (g_capture && !g_capture->Alive()) g_capture.reset();
    HWND target = nullptr;
    std::wstring exe;
    if (IsEnabled()) {
        HWND foreground = GetForegroundWindow();
        HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (IsCaptureCandidate(root, exe)) {
            target = root;
        } else if (CaptureAnyWindow()) {
            struct Search { HWND found; std::wstring exe; } search{nullptr, {}};
            EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                auto* s = reinterpret_cast<Search*>(lp);
                if (!IsCaptureCandidate(hwnd, s->exe)) return TRUE;
                s->found = hwnd;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&search));
            target = search.found;
            exe = search.exe;
        }
    }
    if (g_capture && g_capture->Game() == target) return;
    g_capture.reset();
    if (!target) {
        g_failedCaptureWindow = nullptr;
        g_captureFailures = 0;
        return;
    }
    if (target == g_failedCaptureWindow && (g_captureFailures >= 5 || GetTickCount64() - g_captureFailedAt < 1000)) return;
    if (target != g_failedCaptureWindow) g_captureFailures = 0;
    Log("Starting capture of %ls (window %p)", exe.c_str(), target);
    wchar_t knownName[4];
    if (!GetPrivateProfileStringW(L"GameNames", exe.c_str(), L"", knownName, 4, GlobalIniPath().c_str())) {
        RememberGameDisplayName(WindowExePath(target));  // games added by name only get their real name here
    }
    g_capture = CaptureSession::Start(target, exe);
    if (!g_capture) {
        g_failedCaptureWindow = target;
        g_captureFailedAt = GetTickCount64();
        ++g_captureFailures;
    }
}

void SetEnabled(bool on) {
    // Running injected games poll this value and switch between converted and plain SDR output.
    WritePrivateProfileStringW(kSection, L"Enabled", on ? L"1" : L"0", GlobalIniPath().c_str());
    ApplyHookState();
    UpdateCapture();
    UpdateTray(false);
}

bool AddGameWithDialog(HWND owner) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Game executable (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Add game to OpenHDR";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        // Capture mode by default: it runs nothing inside the game. Injected mode is opt-in per game.
        SetGameMode(file, GameMode::Capture);
        RememberGameDisplayName(file);
        ApplyHookState();
        UpdateCapture();
        return true;
    }
    return false;
}

FlyoutModel LoadFlyoutModel() {
    FlyoutModel model;
    model.enabled = IsEnabled();
    model.autostart = IsAutostart();
    model.theme = ui::GetThemeMode();
    model.games = ListGames();
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        const DisplayInfo display = QueryDisplay(factory.Get(), GetDesktopWindow());
        model.hdrOn = display.hdr;
        model.peakNits = display.maxNits;
    }
    return model;
}

// The display's peak for the windows, re-queried at most every few seconds (it needs a DXGI factory).
float DisplayPeakNits() {
    static float cached = 0.0f;
    static ULONGLONG at = 0;
    const ULONGLONG now = GetTickCount64();
    if (at && now - at < 3000) return cached;
    at = now;
    cached = 0.0f;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        const DisplayInfo display = QueryDisplay(factory.Get(), GetDesktopWindow());
        if (display.hdr) cached = display.maxNits;
    }
    return cached;
}

MainWindowActions MakeMainActions() {
    MainWindowActions a;
    a.enabled = IsEnabled;
    a.setEnabled = SetEnabled;
    a.autostart = IsAutostart;
    a.setAutostart = SetAutostart;
    a.setTheme = SetThemeMode;
    a.gamesChanged = [] {
        ApplyHookState();
        UpdateCapture();
    };
    a.addGameManually = [](HWND owner) { return AddGameWithDialog(owner); };
    a.displayPeakNits = DisplayPeakNits;
    a.openLogs = [] { ShellExecuteW(nullptr, L"open", (DataDir() + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    a.openIni = [] { ShellExecuteW(nullptr, L"open", L"notepad.exe", GlobalIniPath().c_str(), nullptr, SW_SHOWNORMAL); };
    return a;
}

void ShowMainWindow() {
    if (!g_main) g_main = std::make_unique<MainWindow>(MakeMainActions());
    g_main->Show();
}

FlyoutActions MakeFlyoutActions() {
    FlyoutActions a;
    a.load = LoadFlyoutModel;
    a.setEnabled = SetEnabled;
    a.setAutostart = SetAutostart;
    a.setTheme = SetThemeMode;
    a.setGameMode = [](const std::wstring& exe, GameMode mode) {
        SetGameMode(exe, mode);
        ApplyHookState();
        UpdateCapture();
    };
    a.addGame = [](HWND owner) { return AddGameWithDialog(owner); };
    a.openSettings = ShowMainWindow;
    a.openLogs = [] { ShellExecuteW(nullptr, L"open", (DataDir() + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    a.quit = [] { DestroyWindow(g_hwnd); };
    return a;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_showMainMessage && g_showMainMessage) {
        ShowMainWindow();
        return 0;
    }
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        UpdateTray(true);  // Explorer restarted
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                g_flyout->Hide();
                ShowMainWindow();
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) {
                g_flyout->Toggle();
            }
            return 0;
        case WM_TIMER:
            if (wp == kCaptureTimer) UpdateCapture();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kCaptureTimer);
            g_capture.reset();
            g_main.reset();
            g_flyout.reset();
            Shell_NotifyIconW(NIM_DELETE, &g_tray);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Asks the running instance to close and waits for it.
int ExitRunningInstance() {
    HWND hwnd = FindWindowW(kWindowClass, nullptr);
    if (!hwnd) return 1;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (process) {
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Physical pixels everywhere: capture mode positions its window over other apps' windows.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnsureGlobalIni();
    ui::SetThemeMode(ReadThemeMode());

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool trayOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--tray")) trayOnly = true;
        if (!wcscmp(argv[i], L"--exit")) return ExitRunningInstance();
        if (!wcscmp(argv[i], L"--snapshot-main") && i + 1 < argc) {  // design check: page 0 library / 1 settings, [game index]
            MainWindow window(MakeMainActions());
            return window.Snapshot(argv[i + 1], i + 2 < argc ? _wtoi(argv[i + 2]) : 0, i + 3 < argc ? _wtoi(argv[i + 3]) : -1, 1.0f) ? 0 : 1;
        }
        if (!wcscmp(argv[i], L"--add-game") && i + 1 < argc) return SetGameMode(argv[i + 1], GameMode::Capture), RememberGameDisplayName(argv[i + 1]), 0;
        if (!wcscmp(argv[i], L"--add-game-injected") && i + 1 < argc) return SetGameMode(argv[i + 1], GameMode::Injected), RememberGameDisplayName(argv[i + 1]), 0;
        if (!wcscmp(argv[i], L"--remove-game") && i + 1 < argc) return SetGameMode(argv[i + 1], GameMode::None), 0;
        if (!wcscmp(argv[i], L"--scan-library") && i + 1 < argc) {  // diagnostics: what the library finds, as text
            FILE* f = nullptr;
            if (_wfopen_s(&f, argv[i + 1], L"w, ccs=UTF-8") != 0 || !f) return 1;
            for (const LibraryGame& g : ScanLibrary()) {
                fwprintf(f, L"%ls | %ls | %ls | exe %ls | cover %ls |%ls%ls%ls%ls\n", g.store.c_str(), g.id.c_str(), g.name.c_str(),
                         g.exePath.c_str(), g.coverPath.empty() ? L"-" : L"yes", g.optiScaler ? L" OptiScaler" : L"", g.dlss ? L" DLSS" : L"",
                         g.fsr ? L" FSR" : L"", g.xess ? L" XeSS" : L"");
            }
            fclose(f);
            return 0;
        }
        if (!wcscmp(argv[i], L"--snapshot-flyout") && i + 1 < argc) {  // design check: render the flyout to a PNG
            return Flyout(MakeFlyoutActions()).Snapshot(argv[i + 1], 1.5f, i + 2 < argc ? _wtoi(argv[i + 2]) : 0) ? 0 : 1;
        }
    }
    LocalFree(argv);

    g_showMainMessage = RegisterWindowMessageW(L"OpenHDR.ShowMain");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\OpenHDR.Host");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already in the tray: a shortcut or second start brings up its main window instead.
        if (!trayOnly) {
            if (HWND running = FindWindowW(kWindowClass, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(running, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(running, g_showMainMessage, 0, 0);
            }
        }
        return 0;
    }
    if (IsAutostart()) SetAutostart(true);  // older versions registered the command without --tray

    const std::wstring corePath = PrivateCoreCopy();
    g_core = LoadLibraryW(corePath.c_str());
    g_hookProc = g_core ? reinterpret_cast<HOOKPROC>(GetProcAddress(g_core, "OpenHdrCbtProc")) : nullptr;
    if (!g_hookProc) {
        MessageBoxW(nullptr, (L"Could not load " + corePath).c_str(), L"OpenHDR", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);
    // Hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    HWND hwnd = CreateWindowW(kWindowClass, L"OpenHDR", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_hwnd = hwnd;
    g_flyout = std::make_unique<Flyout>(MakeFlyoutActions());
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_iconOn = LoadTrayIcon(true);
    g_iconOff = LoadTrayIcon(false);
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_TRAY;
    UpdateTray(true);

    if (!ApplyHookState()) {
        MessageBoxW(nullptr, L"Could not install the OpenHDR hook.", L"OpenHDR", MB_ICONERROR);
        DestroyWindow(hwnd);
    }
    SetTimer(hwnd, kCaptureTimer, kCaptureCheckMs, nullptr);
    if (!trayOnly) ShowMainWindow();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    DestroyIcon(g_iconOn);
    DestroyIcon(g_iconOff);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
