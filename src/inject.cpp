// Entry point of the global hook installed by OpenHDR.exe.
//
// SetWindowsHookEx(WH_CBT) makes Windows load this DLL into every 64-bit GUI process on the
// desktop the first time that process creates or activates a window, which for games happens
// before the swap chain exists. The DLL becomes active only in games listed under [Games] in
// OpenHDR.ini; everywhere else it does nothing and is unloaded again when the hook is removed.
#include "common.h"
#include "config.h"
#include "crash_log.h"
#include "hooks.h"

namespace openhdr {
namespace {

INIT_ONCE g_activation = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK Activate(PINIT_ONCE, PVOID, PVOID*) {
    if (!IsGameAllowed()) return TRUE;

    // The vtable hooks point into this DLL, so it must stay loaded after the global hook is removed.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&Activate), &self);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("OpenHDR active in %ls (pid %lu)", exe, GetCurrentProcessId());
    InstallCrashLog();
    InstallDxgiHooks();
    return TRUE;
}

}  // namespace
}  // namespace openhdr

extern "C" __declspec(dllexport) LRESULT CALLBACK OpenHdrCbtProc(int code, WPARAM wParam, LPARAM lParam) {
    InitOnceExecuteOnce(&openhdr::g_activation, openhdr::Activate, nullptr, nullptr);
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
