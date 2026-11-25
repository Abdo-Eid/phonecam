#pragma once

// PhoneCam's window: a live preview of what video apps receive, and the settings that used to
// be spread over tray submenus. Plain Win32 controls plus Direct2D for the preview -- both part
// of Windows, so nothing is added to what PhoneCam ships. The window is created when opened and
// destroyed when closed; while closed it costs no memory, GPU or CPU.
//
// Must be used on the thread that runs TrayIcon::RunMessageLoop. No windows.h types in this
// header, same rule as TrayIcon.h.

#include <functional>
#include <memory>
#include <string>

#include "app/AppController.h"
#include "connection/ConnectionManager.h"

namespace phonecam::ui {

class MainWindow {
public:
    MainWindow(phonecam::app::AppController& app, phonecam::connection::ConnectionManager& connection,
               std::function<void(const std::string&)> notify);
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Opens the window, or brings the open one to the front.
    void Show();
    // Gives the window Tab/Enter/arrow-key handling; see TrayIcon::SetMessageFilter.
    bool FilterMessage(void* msg);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::ui
