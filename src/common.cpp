#include "common.h"

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <cwctype>

namespace openhdr {
namespace {

SRWLOCK g_logLock = SRWLOCK_INIT;
FILE* g_logFile = nullptr;
bool g_logOpened = false;

}  // namespace

const std::wstring& DataDir() {
    // Environment variable instead of SHGetKnownFolderPath: this DLL is loaded into every GUI
    // process by the global hook and should not pull shell32 in with it.
    static const std::wstring dir = [] {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        std::wstring d = (n && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
        d += L"\\OpenHDR";
        CreateDirectoryW(d.c_str(), nullptr);
        for (const wchar_t* sub : {L"\\logs", L"\\games", L"\\dumps"}) CreateDirectoryW((d + sub).c_str(), nullptr);
        return d;
    }();
    return dir;
}

const std::wstring& ExeName() {
    static const std::wstring name = [] {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring s(path, n);
        size_t slash = s.find_last_of(L"\\/");
        if (slash != std::wstring::npos) s = s.substr(slash + 1);
        for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    }();
    return name;
}

void Log(const char* fmt, ...) {
    AcquireSRWLockExclusive(&g_logLock);
    if (!g_logOpened) {
        g_logOpened = true;
        // Shared for reading, so the log can be watched while the game runs (_wfopen_s would deny that).
        g_logFile = _wfsopen((DataDir() + L"\\logs\\" + ExeName() + L".log").c_str(), L"w", _SH_DENYWR);
    }
    if (g_logFile) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_logFile, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

}  // namespace openhdr
