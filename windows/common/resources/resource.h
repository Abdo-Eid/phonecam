#pragma once

// Shared resource ID for phonecam.ico, referenced by both host/resources/phonecam.rc and
// svc/resources/phonecam.rc (each exe embeds its own copy of the icon via its own .rc -- Win32
// resources don't flow through a linked static lib) and by TrayIcon.cpp to load it at runtime via
// LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APPICON)).
// 101 is deliberately avoided -- TrayIcon.cpp's kMenuIdExit already uses that value for an
// unrelated WM_COMMAND id, and reusing it here would be a landmine for anyone who later moves
// that value into a .rc file.
#define IDI_APPICON 500
