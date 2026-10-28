#include "common.h"

// This DLL is loaded into every GUI process by the global hook, so nothing happens here.
// Activation (in allowed games only) runs from the hook callback, outside the loader lock.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
