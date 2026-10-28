#include "crash_log.h"

#include "common.h"

#include <atomic>

namespace openhdr {
namespace {

std::atomic<int> g_logged{0};
constexpr int kMaxLogged = 8;
constexpr int kMaxFrames = 16;

bool IsCrashCode(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
            return true;
        default:
            return false;
    }
}

// "module.dll+0x1234", or the bare address outside any module.
void Describe(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        sprintf_s(out, size, "%ls+0x%llX", name ? name + 1 : path, address - reinterpret_cast<DWORD64>(module));
    } else {
        sprintf_s(out, size, "0x%llX", address);
    }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!IsCrashCode(record->ExceptionCode) || g_logged.fetch_add(1) >= kMaxLogged) return EXCEPTION_CONTINUE_SEARCH;

    char where[MAX_PATH + 32];
    Describe(reinterpret_cast<DWORD64>(record->ExceptionAddress), where, sizeof(where));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const char* kind = record->ExceptionInformation[0] == 0 ? "reading" : record->ExceptionInformation[0] == 1 ? "writing" : "executing";
        Log("Exception 0x%08lX at %s (thread %lu): %s address 0x%llX (first chance; the game may still handle it)",
            record->ExceptionCode, where, GetCurrentThreadId(), kind,
            static_cast<unsigned long long>(record->ExceptionInformation[1]));
    } else {
        Log("Exception 0x%08lX at %s (thread %lu) (first chance; the game may still handle it)", record->ExceptionCode, where,
            GetCurrentThreadId());
    }
    if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;  // no stack left to walk with

    // Walk the faulting thread's stack from the exception context.
    CONTEXT context = *info->ContextRecord;
    for (int frame = 0; frame < kMaxFrames && context.Rip; ++frame) {
        Describe(context.Rip, where, sizeof(where));
        Log("  #%d %s", frame, where);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function) {
            // A leaf function (or code without unwind data): the return address is on top of the stack.
            if (!context.Rsp || IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), sizeof(DWORD64))) break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashLog() {
    // First in the chain: a game's own crash handler may end the process without passing the exception on.
    // We only log and let the search continue, so the handlers after us behave as before.
    if (AddVectoredExceptionHandler(1, OnException)) Log("Crash logging installed");
}

}  // namespace openhdr
