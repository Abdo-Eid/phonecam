#pragma once

// Phase 5: a minimal Win32 system-tray icon for phonecam-host.exe, so it can run as a background
// process instead of requiring a visible console window to stay open. Deliberately plain
// Shell_NotifyIcon + a hidden message-only window -- no GUI framework, matching the project's
// all-C++/MSVC, fork-working-samples-over-hand-authoring stance (see docs/architecture.md). The
// console UI (ConsoleControlUi) is kept as-is alongside this, not replaced -- see main.cpp.
//
// No winsock/windows types in this header, same rule as transport/AdbTransport.h: keeps include
// order unconstrained at call sites (main.cpp already includes <mfapi.h> before this).

#include <functional>
#include <memory>
#include <string>

namespace phonecam::tray {

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // statusProvider is called both periodically (to refresh the hover tooltip) and on-demand
    // (fresh, right when the user opens the context menu) -- keep it cheap and non-blocking, it
    // runs on the tray's own message-pump thread.
    bool Create(std::function<std::string()> statusProvider);

    // Blocks running the Win32 message loop on the calling thread until the user picks Exit from
    // the tray menu, or Close() is called (from any thread). Meant to replace a plain
    // WaitForSingleObject(stopEvent, INFINITE) wait in main() -- the caller's normal shutdown
    // sequence runs after this returns, same as it did after that wait.
    void RunMessageLoop();

    // Thread-safe: posts a close request to the tray's message loop so it can be called from a
    // console Ctrl+C handler (a different thread) as well as from the tray's own Exit menu item --
    // both routes end up going through the same RunMessageLoop() return, not two teardown paths.
    void Close();

public:
    // Public so TrayIcon.cpp's free-function WndProc (a plain function, not a member -- Win32
    // window procedures must be plain function pointers) can name and dereference it. Still
    // forward-declared and opaque here regardless (only TrayIcon.cpp has the full definition), so
    // this doesn't leak any Windows types into callers of this header.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::tray
