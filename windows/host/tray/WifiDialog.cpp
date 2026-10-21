#include "tray/WifiDialog.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include "connection/PairingStore.h"
#include "resources/resource.h"
#include "transport/Discovery.h"

namespace phonecam::tray {
namespace {
constexpr UINT kScanFinished = WM_APP + 20;
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
}

struct WifiDialog::Impl {
    phonecam::connection::ConnectionManager& connection;
    HWND hwnd = nullptr;
    std::thread scanner;
    std::atomic<bool> cancelScan{false};
    std::mutex mutex;
    std::vector<phonecam::transport::DiscoveredPhone> scanned;
    std::vector<phonecam::transport::DiscoveredPhone> phones;
    bool scanning = false;
    uint64_t request = 0;

    explicit Impl(phonecam::connection::ConnectionManager& value) : connection(value) {}
    void Status(const std::wstring& text) { SetDlgItemTextW(hwnd, IDC_WIFI_STATUS, text.c_str()); }
    int Selected() const { return static_cast<int>(SendDlgItemMessageW(hwnd, IDC_WIFI_PHONES, LB_GETCURSEL, 0, 0)); }

    void SelectionChanged() {
        request = 0;
        const int index = Selected();
        const bool selected = index >= 0 && static_cast<size_t>(index) < phones.size();
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_CONNECT), selected && phones[index].protocolVersion == 6 && !scanning);
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_FORGET), selected);
        std::string code;
        if (selected) {
            const auto& phone = phones[index];
            code = phonecam::connection::LoadPairingCode(phone.address, phone.controlPort);
            if (phone.protocolVersion != 6) Status(L"App versions differ. Update PhoneCam on the phone and laptop.");
            else Status(code.empty() ? L"Enter the pairing code shown on the phone, then Connect."
                                     : L"Saved code loaded from Windows. Select Connect to use it.");
        }
        SetDlgItemTextW(hwnd, IDC_WIFI_CODE, Wide(code).c_str());
        CheckDlgButton(hwnd, IDC_WIFI_REMEMBER, code.empty() ? BST_UNCHECKED : BST_CHECKED);
    }

    void Scan() {
        if (scanning) return;
        if (scanner.joinable()) scanner.join();
        cancelScan.store(false);
        scanning = true;
        request = 0;
        phones.clear();
        SendDlgItemMessageW(hwnd, IDC_WIFI_PHONES, LB_RESETCONTENT, 0, 0);
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_REFRESH), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_CONNECT), FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_FORGET), FALSE);
        Status(L"Scanning for phones...");
        scanner = std::thread([this]() {
            auto found = phonecam::transport::DiscoverPhones(2200, [this]() { return cancelScan.load(); });
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
            { std::lock_guard<std::mutex> lock(mutex); scanned = std::move(found); }
            if (!cancelScan.load()) PostMessageW(hwnd, kScanFinished, 0, 0);
        });
    }

    void ScanFinished() {
        if (scanner.joinable()) scanner.join();
        scanning = false;
        { std::lock_guard<std::mutex> lock(mutex); phones = scanned; }
        for (const auto& phone : phones) {
            const auto label = Wide("PhoneCam - " + phone.address +
                                    (phone.protocolVersion == 6 ? "" : " (update required)"));
            SendDlgItemMessageW(hwnd, IDC_WIFI_PHONES, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_REFRESH), TRUE);
        if (phones.empty()) {
            Status(L"No phone found. Open PhoneCam, check both devices' Wi-Fi, then Scan again.");
        } else {
            SendDlgItemMessageW(hwnd, IDC_WIFI_PHONES, LB_SETCURSEL, 0, 0);
            Status(L"Select your phone and enter its pairing code, then Connect.");
            SelectionChanged();
        }
    }

    void Connect() {
        request = 0;
        const int index = Selected();
        if (index < 0 || static_cast<size_t>(index) >= phones.size()) {
            Status(L"Select a phone first."); return;
        }
        const auto& phone = phones[index];
        if (phone.protocolVersion != 6) { Status(L"Update PhoneCam on both devices first."); return; }
        wchar_t text[64] = {};
        GetDlgItemTextW(hwnd, IDC_WIFI_CODE, text, static_cast<int>(std::size(text)));
        const auto code = phonecam::connection::NormalizePairingCode(text);
        SecureZeroMemory(text, sizeof(text));
        if (!phonecam::connection::ValidPairingCode(code)) {
            Status(L"Enter the 12-character code shown on the phone. Hyphens and spaces are optional.");
            SetFocus(GetDlgItem(hwnd, IDC_WIFI_CODE));
            return;
        }
        phonecam::connection::Target target;
        target.mode = SendDlgItemMessageW(hwnd, IDC_WIFI_TRANSPORT, CB_GETCURSEL, 0, 0) == 1
                          ? phonecam::connection::Mode::WifiTcp : phonecam::connection::Mode::WifiRtp;
        target.address = phone.address;
        target.controlPort = phone.controlPort;
        target.videoPort = phone.videoPort;
        target.pairingCode = code;
        target.remember = IsDlgButtonChecked(hwnd, IDC_WIFI_REMEMBER) == BST_CHECKED;
        request = connection.Connect(std::move(target));
        Status(L"Connecting...");
        EnableWindow(GetDlgItem(hwnd, IDC_WIFI_CONNECT), FALSE);
    }

    void RefreshStatus() {
        if (!request || scanning) return;
        const auto state = connection.GetSnapshot();
        if (state.request != request) return;
        Status(Wide(state.message));
        if (state.state == phonecam::connection::State::Streaming || state.state == phonecam::connection::State::Stopped) {
            Destroy();
            return;
        }
        if (state.state == phonecam::connection::State::Error || state.state == phonecam::connection::State::Streaming)
            EnableWindow(GetDlgItem(hwnd, IDC_WIFI_CONNECT), TRUE);
    }

    void Destroy() {
        cancelScan.store(true);
        if (scanner.joinable()) scanner.join();
        scanning = false;
        request = 0;
        if (hwnd) DestroyWindow(hwnd);
        hwnd = nullptr;
    }
};

namespace {
INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<WifiDialog::Impl*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<WifiDialog::Impl*>(lParam);
        self->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON))));
        SendDlgItemMessageW(hwnd, IDC_WIFI_CODE, EM_SETLIMITTEXT, 32, 0);
        SendDlgItemMessageW(hwnd, IDC_WIFI_TRANSPORT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low latency (UDP, recommended)"));
        SendDlgItemMessageW(hwnd, IDC_WIFI_TRANSPORT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Compatibility (TCP)"));
        SendDlgItemMessageW(hwnd, IDC_WIFI_TRANSPORT, CB_SETCURSEL, 0, 0);
        SetTimer(hwnd, 1, 400, nullptr);
        self->Scan();
        return TRUE;
    }
    if (!self) return FALSE;
    switch (message) {
        case kScanFinished: self->ScanFinished(); return TRUE;
        case WM_TIMER: self->RefreshStatus(); return TRUE;
        case WM_CLOSE: self->Destroy(); return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDCANCEL: self->Destroy(); return TRUE;
                case IDC_WIFI_REFRESH: self->Scan(); return TRUE;
                case IDC_WIFI_CONNECT: self->Connect(); return TRUE;
                case IDC_WIFI_PHONES:
                    if (HIWORD(wParam) == LBN_SELCHANGE) self->SelectionChanged();
                    return TRUE;
                case IDC_WIFI_FORGET: {
                    const int index = self->Selected();
                    if (index >= 0 && static_cast<size_t>(index) < self->phones.size()) {
                        const auto& phone = self->phones[index];
                        if (phonecam::connection::ForgetPairingCode(phone.address, phone.controlPort)) {
                            self->SelectionChanged();
                            self->request = 0;
                            self->Status(L"Saved code removed. The current connection is unchanged.");
                        } else self->Status(L"Windows could not remove the saved code.");
                    }
                    return TRUE;
                }
            }
            break;
        case WM_DESTROY: KillTimer(hwnd, 1); self->hwnd = nullptr; return TRUE;
    }
    return FALSE;
}
}

WifiDialog::WifiDialog(phonecam::connection::ConnectionManager& connection) : impl_(std::make_unique<Impl>(connection)) {}
WifiDialog::~WifiDialog() { impl_->Destroy(); }
void WifiDialog::Show() {
    if (!impl_->hwnd) CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_WIFI_CONNECT), nullptr,
                                         DialogProc, reinterpret_cast<LPARAM>(impl_.get()));
    if (impl_->hwnd) { ShowWindow(impl_->hwnd, SW_SHOWNORMAL); SetForegroundWindow(impl_->hwnd); }
}
}
