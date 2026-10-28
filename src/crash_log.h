#pragma once

namespace openhdr {

// Logs the first few crash-type exceptions in the game process (code, faulting module + offset, a
// short call stack), so a crash with other mods in the game shows which module it happened in.
void InstallCrashLog();

}  // namespace openhdr
