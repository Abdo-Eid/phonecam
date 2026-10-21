#include "tray/QrPairDialog.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "connection/PairingStore.h"
#include "transport/Discovery.h"
#include "resources/resource.h"
#include "qrcodegen.hpp"

namespace phonecam::tray {
namespace {
constexpr UINT kPaired = WM_APP + 30;
std::string Hex(const std::string& text) {
    const char digits[] = "0123456789abcdef"; std::string out;
    for (unsigned char c : text) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}
phonecam::connection::Target TargetFor(const phonecam::connection::PairedPhone& phone) {
    phonecam::connection::Target target;
    target.mode = phonecam::connection::Mode::WifiRtp;
    target.clientId = phone.pcId; target.phoneId = phone.phoneId; target.pairingCode = phone.key;
    return target;
}
}
struct QrPairDialog::Impl {
    phonecam::connection::ConnectionManager& connection;
    std::function<void()> manual;
    std::function<void()> onPaired;
    HWND hwnd = nullptr;
    HBITMAP image = nullptr;
    std::thread worker;
    std::atomic<bool> cancelled{false};
    std::chrono::steady_clock::time_point expires;
    uint64_t request = 0;
    bool pairingDone = false;
    bool paired = false; // written by worker, read only after join on kPaired
    unsigned generation = 0;

    Impl(phonecam::connection::ConnectionManager& value, std::function<void()> fallback, std::function<void()> notify)
        : connection(value), manual(std::move(fallback)), onPaired(std::move(notify)) {}
    void Status(const std::string& text) {
        SetDlgItemTextW(hwnd, IDC_QR_STATUS, std::wstring(text.begin(), text.end()).c_str());
    }
    void StopWorker() {
        ++generation;
        cancelled.store(true);
        if (worker.joinable()) worker.join();
    }
    void NewQr() {
        StopWorker();
        cancelled.store(false); request = 0; pairingDone = true; paired = false;
        const auto pcId = phonecam::connection::PcIdentity();
        const auto token = phonecam::connection::RandomHex(32);
        if (pcId.empty() || token.empty()) { Status("Windows could not create secure pairing credentials."); return; }
        const auto payload = "phonecam-pair:1:" + pcId + ":" + token + ":" + Hex(phonecam::connection::PcName());
        try {
            const auto qr = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            RECT bounds; GetClientRect(GetDlgItem(hwnd, IDC_QR_IMAGE), &bounds);
            const int modules = qr.getSize() + 8;
            const int scale = (bounds.right < bounds.bottom ? bounds.right : bounds.bottom) / modules;
            const int size = modules * (scale > 0 ? scale : 1);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = size;
            info.bmiHeader.biHeight = -size; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            void* pixels = nullptr;
            auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap) { Status("Could not draw QR. Use code instead."); return; }
            auto* colors = static_cast<uint32_t*>(pixels);
            for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
                const int mx = x / (scale > 0 ? scale : 1) - 4, my = y / (scale > 0 ? scale : 1) - 4;
                colors[y * size + x] = mx >= 0 && my >= 0 && mx < qr.getSize() && my < qr.getSize() && qr.getModule(mx, my)
                                         ? 0x00000000 : 0x00ffffff;
            }
            SendDlgItemMessageW(hwnd, IDC_QR_IMAGE, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmap));
            if (image) DeleteObject(image);
            image = bitmap;
        } catch (const std::exception&) { Status("Could not create QR. Use code instead."); return; }
        expires = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        pairingDone = false;
        Status("Scan this QR in PhoneCam on the phone (valid for 2 minutes).");
        const auto current = generation;
        worker = std::thread([this, pcId, token, current]() {
            phonecam::transport::QrPairedPhone result;
            paired = phonecam::transport::PairQrPhone(pcId, token, 120000, result, [this]() { return cancelled.load(); });
            if (paired && !cancelled.load()) {
                phonecam::connection::PairedPhone phone{pcId, result.id, result.key, result.name};
                paired = phonecam::connection::SavePairedPhone(phone);
                if (paired) {
                    auto target = TargetFor(phone);
                    target.address = result.endpoint.address;
                    target.controlPort = result.endpoint.controlPort;
                    target.videoPort = result.endpoint.videoPort;
                    request = connection.Connect(std::move(target));
                }
            }
            if (!cancelled.load()) PostMessageW(hwnd, kPaired, current, 0);
        });
    }
    void Update() {
        if (pairingDone && paired) {
            const auto state = connection.GetSnapshot();
            if (state.request == request) Status(state.message);
        } else if (!pairingDone) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(expires - std::chrono::steady_clock::now()).count();
            Status(seconds > 0 ? "Scan in PhoneCam. QR expires in " + std::to_string(seconds) + " seconds."
                               : "QR expired. Choose New QR to try again.");
        }
    }
    void Close() {
        StopWorker();
        if (hwnd) DestroyWindow(hwnd);
        hwnd = nullptr;
        if (image) { DeleteObject(image); image = nullptr; }
    }
};
namespace {
INT_PTR CALLBACK QrProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<QrPairDialog::Impl*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<QrPairDialog::Impl*>(l); self->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        SetTimer(hwnd, 1, 500, nullptr);
        EnableWindow(GetDlgItem(hwnd, IDC_QR_RECONNECT), !phonecam::connection::LoadPairedPhone().key.empty());
        self->NewQr(); return TRUE;
    }
    if (!self) return FALSE;
    if (msg == WM_TIMER) { self->Update(); return TRUE; }
    if (msg == kPaired) {
        if (w != self->generation) return TRUE;
        if (self->worker.joinable()) self->worker.join();
        self->pairingDone = true;
        if (!self->paired) self->Status("Pairing expired or could not be saved. Choose New QR to retry.");
        else {
            self->Close(); // both sides have durably stored the pairing; nothing left to confirm
            if (self->onPaired) self->onPaired();
        }
        return TRUE;
    }
    if (msg == WM_CLOSE) { self->Close(); return TRUE; }
    if (msg == WM_DESTROY) { KillTimer(hwnd, 1); self->hwnd = nullptr; return TRUE; }
    if (msg == WM_COMMAND) {
        switch (LOWORD(w)) {
            case IDCANCEL: self->Close(); return TRUE;
            case IDC_QR_REFRESH: self->NewQr(); return TRUE;
            case IDC_QR_MANUAL: self->Close(); self->manual(); return TRUE;
            case IDC_QR_RECONNECT: {
                const auto phone = phonecam::connection::LoadPairedPhone();
                if (phone.key.empty()) { self->Status("No remembered phone. Scan the QR first."); return TRUE; }
                self->StopWorker(); self->paired = self->pairingDone = true;
                phonecam::connection::UseRememberedWifi(true);
                self->request = self->connection.Connect(TargetFor(phone)); self->Close(); return TRUE;
            }
            case IDC_QR_FORGET:
                self->StopWorker();
                if (phonecam::connection::ForgetPairedPhone()) {
                    self->connection.Connect({phonecam::connection::Mode::Disconnected});
                    self->pairingDone = true; self->paired = false;
                    self->Status("Phone forgotten on this PC. On the phone, use Forget PC to revoke its access.");
                } else self->Status("Windows could not remove the saved pairing.");
                EnableWindow(GetDlgItem(hwnd, IDC_QR_RECONNECT), FALSE);
                return TRUE;
        }
    }
    return FALSE;
}
}
QrPairDialog::QrPairDialog(phonecam::connection::ConnectionManager& connection, std::function<void()> manual,
                         std::function<void()> onPaired)
    : impl_(std::make_unique<Impl>(connection, std::move(manual), std::move(onPaired))) {}
QrPairDialog::~QrPairDialog() { impl_->Close(); }
void QrPairDialog::Show() {
    if (!impl_->hwnd) CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_QR_PAIR), nullptr, QrProc, reinterpret_cast<LPARAM>(impl_.get()));
    if (impl_->hwnd) { ShowWindow(impl_->hwnd, SW_SHOWNORMAL); SetForegroundWindow(impl_->hwnd); }
}
}
