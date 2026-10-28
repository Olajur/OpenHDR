#pragma once

#include "common.h"

#include <memory>

namespace openhdr {

// Capture mode: converts a game's window without running anything inside the game process.
//
// The window's image is captured with Windows.Graphics.Capture (the API screen recorders use),
// converted by the same shader and settings as the injected mode, and shown in a click-through,
// topmost HDR window placed exactly over the game's client area. Works for any graphics API, in
// windowed and borderless modes (not exclusive fullscreen). Runs on its own thread.
class CaptureSession {
public:
    // Starts converting `game`, a top-level window of `gameExe`. Null if capture can't start.
    static std::unique_ptr<CaptureSession> Start(HWND game, const std::wstring& gameExe);
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    HWND Game() const;
    // False once the session ended by itself (window closed, capture or device error).
    bool Alive() const;

    struct Impl;

private:
    explicit CaptureSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace openhdr
