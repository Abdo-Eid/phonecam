#include "tray/TrayIcon.h"

#include <windows.h>
#include <shellapi.h>

#include "log/Log.h"

namespace phonecam::tray {

namespace {
constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT kTimerId = 1;
constexpr UINT kTimerIntervalMs = 3000;
constexpr UINT kIconId = 1;
constexpr UINT kMenuIdStatus = 100;  // disabled, display-only
constexpr UINT kMenuIdExit = 101;
constexpr const wchar_t* kWindowClassName = L"PhoneCamTrayWindow";
}  // namespace

struct TrayIcon::Impl {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    std::function<std::string()> statusProvider;
    bool iconAdded = false;

    void RefreshTooltip() {
        if (!iconAdded) return;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kIconId;
        nid.uFlags = NIF_TIP;
        const std::string status = statusProvider ? statusProvider() : std::string();
        const std::wstring tip = L"PhoneCam - " + std::wstring(status.begin(), status.end());
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void ShowContextMenu() {
        POINT pt;
        GetCursorPos(&pt);

        HMENU menu = CreatePopupMenu();
        const std::string status = statusProvider ? statusProvider() : std::string();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuIdStatus, std::wstring(status.begin(), status.end()).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuIdExit, L"Exit");

        // SetForegroundWindow + the post-TrackPopupMenu WM_NULL are the documented idiom for a
        // tray menu that reliably dismisses on an outside click instead of staying stuck open.
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }
};

namespace {

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* impl = reinterpret_cast<TrayIcon::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case kCallbackMessage:
            if (impl && (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP)) {
                impl->ShowContextMenu();
            }
            return 0;
        case WM_TIMER:
            if (impl && wParam == kTimerId) impl->RefreshTooltip();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kMenuIdExit) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            if (impl) {
                if (impl->iconAdded) {
                    NOTIFYICONDATAW nid{};
                    nid.cbSize = sizeof(nid);
                    nid.hWnd = hwnd;
                    nid.uID = kIconId;
                    Shell_NotifyIconW(NIM_DELETE, &nid);
                    impl->iconAdded = false;
                }
                KillTimer(hwnd, kTimerId);
                // So ~TrayIcon()'s DestroyWindow(impl_->hwnd) is a clean no-op if the window was
                // already torn down via the message loop (Exit menu item or Close()).
                impl->hwnd = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

TrayIcon::TrayIcon() : impl_(std::make_unique<Impl>()) {}
TrayIcon::~TrayIcon() {
    if (impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
    }
}

bool TrayIcon::Create(std::function<std::string()> statusProvider) {
    impl_->statusProvider = std::move(statusProvider);

    const HINSTANCE hInstance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    // RegisterClassExW fails harmlessly with ERROR_CLASS_ALREADY_EXISTS if called twice in the
    // same process -- not expected here (one TrayIcon per process), but not treated as fatal.
    RegisterClassExW(&wc);

    // HWND_MESSAGE: a message-only window, invisible and without a taskbar entry -- exactly what
    // a pure tray-icon host needs, no separate visible window to manage.
    impl_->hwnd = CreateWindowExW(0, kWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, impl_.get());
    if (!impl_->hwnd) {
        phonecam::log::Error("TrayIcon: CreateWindowExW failed");
        return false;
    }

    impl_->icon = LoadIconW(nullptr, IDI_APPLICATION);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = impl_->hwnd;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kCallbackMessage;
    nid.hIcon = impl_->icon;
    wcsncpy_s(nid.szTip, L"PhoneCam", _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        phonecam::log::Error("TrayIcon: Shell_NotifyIconW(NIM_ADD) failed");
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
        return false;
    }
    impl_->iconAdded = true;

    SetTimer(impl_->hwnd, kTimerId, kTimerIntervalMs, nullptr);
    return true;
}

void TrayIcon::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayIcon::Close() {
    if (impl_->hwnd) {
        PostMessageW(impl_->hwnd, WM_CLOSE, 0, 0);
    }
}

}  // namespace phonecam::tray
