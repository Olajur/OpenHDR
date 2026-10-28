#pragma once

#include "config.h"

#include <string>
#include <vector>

namespace openhdr {

// Installed games found on this PC, for the main window's library. Everything comes from local files
// and the registry (no network): Steam's library folders, manifests and cover cache, the Epic Games
// Launcher's manifests, and GOG Galaxy's registry entries.
struct LibraryGame {
    std::wstring store;       // "Steam", "Epic", "GOG" or "Manual" (listed in OpenHDR.ini only)
    std::wstring id;          // store id (Steam app id...), or the exe name for manual entries
    std::wstring name;
    std::wstring installDir;
    std::wstring exePath;     // the game's process: picked automatically, or the user's choice
    std::wstring coverPath;   // portrait cover image, if one is cached locally
    // Upscaler files found next to the game (shown as chips).
    bool optiScaler = false, dlss = false, fsr = false, xess = false;
    std::wstring ExeName() const;  // the [Games] key: the exe's file name
};

// Scans all stores (a few hundred milliseconds to seconds; call off the UI thread). Manual entries in
// [Games] that no store lists are included too.
std::vector<LibraryGame> ScanLibrary();

// The user picked another exe for a detected game (remembered in [LibraryExe]).
void SetLibraryExe(const LibraryGame& game, const std::wstring& exePath);

}  // namespace openhdr
