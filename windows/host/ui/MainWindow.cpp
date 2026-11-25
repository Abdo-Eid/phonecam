#include "ui/MainWindow.h"

#include <windows.h>

#include <commctrl.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <vector>

#include "log/Log.h"
#include "resources/resource.h"
#include "text/Utf8.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace phonecam::ui {

namespace {

using phonecam::bridge::PreviewImage;
using phonecam::connection::Mode;
using phonecam::connection::State;
using phonecam::control::LensInfo;
using phonecam::picture::Picture;
using phonecam::picture::SeenRect;
using phonecam::picture::Shape;
using phonecam::picture::Size;
using phonecam::shm::Rotation;
using phonecam::text::Widen;

constexpr wchar_t kWindowClass[] = L"PhoneCamMainWindow";
constexpr wchar_t kPreviewClass[] = L"PhoneCamPreview";
constexpr UINT kPreviewMessage = WM_APP + 10;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 250;

// All layout numbers are for 100% scaling; Impl::S converts.
constexpr int kPanelWidth = 340;
constexpr int kMargin = 16;
constexpr int kLabelWidth = 104;
constexpr int kRow = 32;
constexpr int kMinPreviewWidth = 360;
constexpr int kDefaultPreviewWidth = 720;
constexpr int kClientHeight = 614;

constexpr int kCropSteps = 100;  // percent
constexpr int kZoomSteps = 100;  // per 1x

enum ControlId : int {
    kStartStop = 100,
    kPreset,
    kPresetSave,
    kPresetDelete,
    kShape,
    kSize,
    kCrop,
    kAdjust,
    kRotation,
    kMirror,
    kLens,
    kZoom,
    kExposure,
    kFocus,
    kTorch,
    kReset,
    kMode,
    kPlainText,  // labels
    kQuietText,  // labels drawn in gray
};

struct RotationChoice {
    const wchar_t* label;
    Rotation rotation;
};
constexpr RotationChoice kRotations[] = {
    {L"Automatic", Rotation::Deg0},
    {L"Quarter turn right", Rotation::Deg90},
    {L"Upside down", Rotation::Deg180},
    {L"Quarter turn left", Rotation::Deg270},
};

struct ShapeChoice {
    const wchar_t* label;
    Shape shape;
};
constexpr ShapeChoice kShapes[] = {
    {L"Follow the phone", Shape::Follow}, {L"Square 1:1", Shape::Square},        {L"Wide 16:9", Shape::Wide},
    {L"Tall 9:16", Shape::Tall},          {L"Classic 4:3", Shape::Classic},      {L"Portrait 3:4", Shape::ClassicTall},
};

// What the mouse would do to the crop box at a point.
enum class Grab { None, Move, TopLeft, TopRight, BottomLeft, BottomRight };

struct ModeChoice {
    const wchar_t* label;
    Mode mode;
};
constexpr ModeChoice kModes[] = {
    {L"Automatic", Mode::Automatic},
    {L"USB tethering", Mode::Tethered},
    {L"USB debugging (ADB)", Mode::Adb},
    {L"Disconnected", Mode::Disconnected},
};

const wchar_t* FocusName(int mode) {
    switch (mode) {
        case 0: return L"Off";
        case 1: return L"Auto";
        case 2: return L"Continuous";
        case 3: return L"Macro";
        case 4: return L"Extended depth";
        default: return L"Other";
    }
}

std::wstring LensName(const std::vector<LensInfo>& lenses, size_t index) {
    const int facing = lenses[index].facing;
    const wchar_t* base = facing == 0 ? L"Back camera" : facing == 1 ? L"Front camera" : L"External camera";
    size_t sameFacing = 0, position = 0;
    for (size_t i = 0; i < lenses.size(); ++i) {
        if (lenses[i].facing != facing) continue;
        ++sameFacing;
        if (i == index) position = sameFacing;
    }
    return sameFacing > 1 ? std::format(L"{} {}", base, position) : std::wstring(base);
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(window, text.data(), length + 1)));
    return text;
}

// Everything below is refreshed on a timer: touching a control only when its value really
// changed is what keeps it from flickering and from fighting the user.
void SetTextIfChanged(HWND window, const std::wstring& text) {
    if (WindowText(window) != text) SetWindowTextW(window, text.c_str());
}

void EnableIfChanged(HWND window, bool enabled) {
    if ((IsWindowEnabled(window) != FALSE) != enabled) EnableWindow(window, enabled);
}

void SetCheckIfChanged(HWND window, bool checked) {
    if ((Button_GetCheck(window) == BST_CHECKED) != checked) Button_SetCheck(window, checked ? BST_CHECKED : BST_UNCHECKED);
}

void SelectIfChanged(HWND combo, int index) {
    if (ComboBox_GetDroppedState(combo)) return;  // the user is choosing
    if (ComboBox_GetCurSel(combo) != index) ComboBox_SetCurSel(combo, index);
}

void SetSliderRange(HWND slider, int low, int high, int page) {
    if (SendMessageW(slider, TBM_GETRANGEMIN, 0, 0) == low && SendMessageW(slider, TBM_GETRANGEMAX, 0, 0) == high) return;
    SendMessageW(slider, TBM_SETRANGEMIN, FALSE, low);
    SendMessageW(slider, TBM_SETRANGEMAX, TRUE, high);
    SendMessageW(slider, TBM_SETPAGESIZE, 0, page);
}

void SetSliderIfChanged(HWND slider, int position) {
    if (GetCapture() == slider) return;  // being dragged
    if (SendMessageW(slider, TBM_GETPOS, 0, 0) != position) SendMessageW(slider, TBM_SETPOS, TRUE, position);
}

int SliderPosition(HWND slider) { return static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)); }

}  // namespace

struct MainWindow::Impl {
    phonecam::app::AppController& app;
    phonecam::connection::ConnectionManager& connection;
    std::function<void(const std::string&)> notify;

    HWND window = nullptr;
    HWND preview = nullptr;
    HWND lastFocus = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;

    HWND status{}, startStop{};
    HWND presetTitle{}, preset{}, presetSave{}, presetDelete{}, presetHint{};
    HWND pictureTitle{}, shapeLabel{}, shape{}, sizes{}, cropLabel{}, crop{}, adjust{}, rotationLabel{}, rotation{}, mirror{};
    bool adjustOpen = false;  // rotation and mirror are occasional corrections, not part of first sight
    HWND cameraTitle{}, lensLabel{}, lens{}, zoomLabel{}, zoom{}, exposureLabel{}, exposure{}, focusLabel{}, focus{},
        torch{}, reset{};
    HWND connectionTitle{}, mode{}, output{};

    std::vector<phonecam::settings::Preset> presets;
    std::vector<LensInfo> lenses;
    std::vector<int> focusModes;
    Size sizesShown[2]{};  // what the size list currently offers

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> textFormat, smallFormat;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1Bitmap> bitmap;
    PreviewImage image;
    bool hasImage = false;
    bool bitmapStale = false;
    bool previewEnabled = false;
    D2D1_RECT_F pictureRect{};  // where the phone's whole picture is drawn, in preview pixels
    Grab grab = Grab::None;
    POINT grabPoint{};
    SeenRect grabBox;  // the crop box when the drag began

    Impl(phonecam::app::AppController& a, phonecam::connection::ConnectionManager& c,
         std::function<void(const std::string&)> n)
        : app(a), connection(c), notify(std::move(n)) {}

    int S(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }

    // --- lifetime ---

    static void RegisterClasses() {
        static const bool registered = [] {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
            InitCommonControlsEx(&controls);
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = kWindowClass;
            RegisterClassExW(&wc);
            wc.lpfnWndProc = PreviewProc;
            wc.style = CS_DBLCLKS;
            wc.hIcon = nullptr;
            wc.hCursor = nullptr;  // set per position
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kPreviewClass;
            RegisterClassExW(&wc);
            return true;
        }();
        (void)registered;
    }

    void Show() {
        if (!window && !Create()) {
            phonecam::log::Error("MainWindow: could not create the window");
            return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }

    bool Create() {
        RegisterClasses();
        if (!CreateWindowExW(0, kWindowClass, L"PhoneCam", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), this)) {
            return false;
        }
        RestorePlacement();
        SetFocus(startStop);
        return true;
    }

    void OnCreate() {
        dpi = GetDpiForWindow(window);
        CreateFonts();
        const auto make = [this](const wchar_t* type, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        };
        const auto label = [&](const wchar_t* text, int id = kPlainText) {
            return make(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, id);
        };
        const auto combo = [&](int id, DWORD kind = CBS_DROPDOWNLIST) {
            return make(WC_COMBOBOXW, L"", kind | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, id);
        };
        const auto slider = [&](int id) {
            return make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, id);
        };
        const auto button = [&](const wchar_t* text, int id, DWORD kind = BS_PUSHBUTTON) {
            return make(WC_BUTTONW, text, kind | WS_TABSTOP, id);
        };

        // Creation order is Tab order, and a label right before its control names it for screen
        // readers.
        status = label(L"");
        startStop = button(L"Stop", kStartStop);

        presetTitle = label(L"Preset");
        preset = combo(kPreset, CBS_DROPDOWN);
        SendMessageW(preset, CB_LIMITTEXT, 40, 0);
        SendMessageW(preset, CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Custom settings"));
        presetSave = button(L"Save", kPresetSave);
        presetDelete = button(L"Delete", kPresetDelete);
        presetHint = label(L"To save these settings, type a name and press Save.", kQuietText);

        pictureTitle = label(L"Picture");
        shapeLabel = label(L"Shape");
        shape = combo(kShape);
        for (const auto& choice : kShapes) ComboBox_AddString(shape, choice.label);
        sizes = combo(kSize);
        sizesShown[0] = sizesShown[1] = {};
        cropLabel = label(L"Crop");
        crop = slider(kCrop);
        SetSliderRange(crop, static_cast<int>(phonecam::picture::kMinCropSize * kCropSteps), kCropSteps, 5);
        adjust = button(L"", kAdjust);
        rotationLabel = label(L"Rotation");
        rotation = combo(kRotation);
        for (const auto& choice : kRotations) ComboBox_AddString(rotation, choice.label);
        mirror = button(L"Mirror", kMirror, BS_AUTOCHECKBOX);
        adjustOpen = false;
        ShowAdjustments();

        cameraTitle = label(L"Camera");
        lensLabel = label(L"Lens");
        lens = combo(kLens);
        zoomLabel = label(L"Zoom");
        zoom = slider(kZoom);
        exposureLabel = label(L"Exposure");
        exposure = slider(kExposure);
        focusLabel = label(L"Focus");
        focus = combo(kFocus);
        torch = button(L"Light", kTorch, BS_AUTOCHECKBOX);
        reset = button(L"Reset camera", kReset);

        connectionTitle = label(L"Connection");
        mode = combo(kMode);
        for (const auto& choice : kModes) ComboBox_AddString(mode, choice.label);
        output = label(L"", kQuietText);

        preview = CreateWindowExW(0, kPreviewClass, L"Preview and crop", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                  window, nullptr, GetModuleHandleW(nullptr), this);

        ApplyFonts();
        RefreshPresets();
        Refresh();
        SetTimer(window, kRefreshTimer, kRefreshMs, nullptr);
        EnablePreview(true);
    }

    void OnDestroy() {
        EnablePreview(false);
        KillTimer(window, kRefreshTimer);
        bitmap.Reset();
        target.Reset();
        textFormat.Reset();
        smallFormat.Reset();
        dwrite.Reset();
        d2d.Reset();
        image = {};
        hasImage = false;
        lenses.clear();
        focusModes.clear();
        DeleteObject(font);
        DeleteObject(bold);
        font = bold = nullptr;
        window = preview = lastFocus = nullptr;
    }

    void EnablePreview(bool on) {
        if (on == previewEnabled) return;
        previewEnabled = on;
        auto& bridge = connection.Bridge();
        if (!on) {
            bridge.SetPreview(nullptr);
            return;
        }
        // Runs on the decode thread: posting is safe there, and harmless once the window is gone.
        bridge.SetPreview([target = window]() { PostMessageW(target, kPreviewMessage, 0, 0); });
    }

    void RestorePlacement() {
        const auto& store = app.Store();
        const auto load = [&store](const wchar_t* name) { return static_cast<LONG>(store.LoadNumber(name, 0)); };
        WINDOWPLACEMENT placement{sizeof(placement)};
        placement.rcNormalPosition = {load(L"WindowLeft"), load(L"WindowTop"), load(L"WindowRight"), load(L"WindowBottom")};
        if (!IsRectEmpty(&placement.rcNormalPosition)) {
            // Windows itself moves a window back on screen if its monitor is gone.
            placement.showCmd = load(L"WindowMaximized") ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            SetWindowPlacement(window, &placement);
            return;
        }
        RECT size{0, 0, S(kDefaultPreviewWidth + kPanelWidth), S(kClientHeight)};
        AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        SetWindowPos(window, nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(window, SW_SHOWNORMAL);
    }

    void SavePlacement() {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(window, &placement)) return;
        const auto& store = app.Store();
        store.SaveNumber(L"WindowLeft", static_cast<uint32_t>(placement.rcNormalPosition.left));
        store.SaveNumber(L"WindowTop", static_cast<uint32_t>(placement.rcNormalPosition.top));
        store.SaveNumber(L"WindowRight", static_cast<uint32_t>(placement.rcNormalPosition.right));
        store.SaveNumber(L"WindowBottom", static_cast<uint32_t>(placement.rcNormalPosition.bottom));
        store.SaveNumber(L"WindowMaximized", IsZoomed(window) ? 1 : 0);
    }

    void OnClose() {
        SavePlacement();
        if (app.Store().LoadNumber(L"CloseHintShown", 0) == 0) {
            app.Store().SaveNumber(L"CloseHintShown", 1);
            notify("PhoneCam keeps running here. Right-click the icon to exit.");
        }
        DestroyWindow(window);
    }

    // --- appearance ---

    void CreateFonts() {
        DeleteObject(font);
        DeleteObject(bold);
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
        font = CreateFontIndirectW(&metrics.lfMessageFont);
        metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
        bold = CreateFontIndirectW(&metrics.lfMessageFont);
    }

    void ApplyFonts() {
        EnumChildWindows(
            window,
            [](HWND child, LPARAM value) -> BOOL {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font));
        for (const HWND title : {status, presetTitle, pictureTitle, cameraTitle, connectionTitle}) {
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
        }
    }

    void Layout() {
        if (!preview) return;
        RECT client{};
        GetClientRect(window, &client);
        const int panelLeft = (std::max)(0L, client.right - S(kPanelWidth));
        MoveWindow(preview, 0, 0, panelLeft, client.bottom, TRUE);

        const int x = panelLeft + S(kMargin);
        const int width = S(kPanelWidth - 2 * kMargin);
        const int labelWidth = S(kLabelWidth);
        const int right = x + width;
        int y = S(kMargin);
        const auto place = [](HWND child, int left, int top, int w, int h) { MoveWindow(child, left, top, w, h, TRUE); };
        const auto title = [&](HWND child) {
            y += S(8);
            place(child, x, y, width, S(20));
            y += S(22);
        };
        // A combo's height includes its drop-down list.
        const auto labelled = [&](HWND text, HWND control, bool isCombo) {
            place(text, x, y + S(4), labelWidth, S(20));
            place(control, x + labelWidth, isCombo ? y : y + S(1), width - labelWidth, isCombo ? S(240) : S(26));
            y += S(kRow);
        };

        place(startStop, right - S(84), y, S(84), S(28));
        place(status, x, y, width - S(92), S(38));
        y += S(40);

        title(presetTitle);
        place(presetDelete, right - S(62), y - S(1), S(62), S(27));
        place(presetSave, right - S(62 + 6 + 54), y - S(1), S(54), S(27));
        place(preset, x, y, width - S(62 + 6 + 54 + 6), S(320));
        y += S(30);
        place(presetHint, x, y, width, S(18));
        y += S(18);

        title(pictureTitle);
        place(shapeLabel, x, y + S(4), S(48), S(20));
        place(shape, x + S(48), y, S(144), S(240));
        place(sizes, right - S(110), y, S(110), S(240));
        y += S(kRow);
        labelled(cropLabel, crop, false);
        place(adjust, x, y - S(1), S(132), S(27));
        y += S(kRow);
        if (adjustOpen) {
            place(rotationLabel, x, y + S(4), S(60), S(20));
            place(rotation, x + S(60), y, S(156), S(240));
            place(mirror, right - S(78), y, S(78), S(24));
            y += S(kRow);
        }

        title(cameraTitle);
        labelled(lensLabel, lens, true);
        labelled(zoomLabel, zoom, false);
        labelled(exposureLabel, exposure, false);
        labelled(focusLabel, focus, true);
        place(torch, x, y, width / 2, S(24));
        place(reset, right - S(108), y - S(2), S(108), S(27));
        y += S(28);

        title(connectionTitle);
        place(mode, x, y, width, S(200));
        y += S(30);
        place(output, x, y, width, S(20));
        ClearPresetHighlight();
    }

    void ShowAdjustments() {
        for (const HWND control : {rotationLabel, rotation, mirror}) ShowWindow(control, adjustOpen ? SW_SHOW : SW_HIDE);
        SetWindowTextW(adjust, adjustOpen ? L"Hide rotation" : L"Rotate or mirror");
    }

    // --- showing the current state ---

    const LensInfo* CurrentLens(const phonecam::app::AppController::CameraState& camera) const {
        for (const auto& info : lenses) {
            if (info.cameraId == camera.lens) return &info;
        }
        return lenses.empty() ? nullptr : &lenses.front();
    }

    void Refresh() {
        const auto snapshot = connection.GetSnapshot();
        auto& control = connection.Control();
        const bool haveCamera = snapshot.controlReady && control.HasCapabilities();

        SetTextIfChanged(status, Widen(snapshot.message));
        SetTextIfChanged(startStop, snapshot.state == State::Stopped ? L"Start" : L"Stop");
        EnableIfChanged(startStop, snapshot.controlReady);

        RefreshPicture();

        const auto capabilities = haveCamera ? control.GetLastCapabilities() : phonecam::control::CapabilityInfo{};
        RefreshCamera(haveCamera, capabilities);

        for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i) {
            if (kModes[i].mode == app.ChosenMode()) SelectIfChanged(mode, i);
        }
        // An app keeps the size it opened the camera with.
        const auto consumer = connection.Bridge().GetConsumer();
        const Size offered = Output();
        SetTextIfChanged(
            output, !consumer.active             ? std::wstring(L"No video app is using the camera right now.")
                    : consumer.size == offered   ? std::format(L"In use by a video app at {}", Text(offered))
                                                 : std::format(L"Turn the camera off and on in your app for {}.", Text(offered)));
        RefreshActivePreset();

        // No frames arrive while nothing is streaming, so the message is what needs redrawing.
        if (snapshot.state != State::Streaming) InvalidateRect(preview, nullptr, FALSE);
    }

    static std::wstring Text(Size value) { return std::format(L"{} × {}", value.width, value.height); }

    Size Output() const { return phonecam::picture::OutputSize(app.GetPicture(), connection.Bridge().GetView()); }

    SeenRect CropBox() const {
        const auto view = connection.Bridge().GetView();
        const Picture picture = app.GetPicture();
        return phonecam::picture::ToSeen(
            view, phonecam::picture::ComputeCrop(view, picture, phonecam::picture::OutputSize(picture, view)));
    }

    SeenRect LargestBox(const Picture& picture) const {
        const auto view = connection.Bridge().GetView();
        return phonecam::picture::LargestCrop(view, picture, phonecam::picture::OutputSize(picture, view));
    }

    void RefreshPicture() {
        const Picture picture = app.GetPicture();
        for (int i = 0; i < static_cast<int>(std::size(kRotations)); ++i) {
            if (kRotations[i].rotation == picture.turn) SelectIfChanged(rotation, i);
        }
        for (int i = 0; i < static_cast<int>(std::size(kShapes)); ++i) {
            if (kShapes[i].shape == picture.shape) SelectIfChanged(shape, i);
        }
        Size choices[2];
        for (int i = 0; i < 2; ++i) {
            Picture choice = picture;
            choice.compact = i == 1;
            choices[i] = phonecam::picture::OutputSize(choice, connection.Bridge().GetView());
        }
        if (!std::equal(std::begin(choices), std::end(choices), std::begin(sizesShown)) && !ComboBox_GetDroppedState(sizes)) {
            ComboBox_ResetContent(sizes);
            for (int i = 0; i < 2; ++i) {
                ComboBox_AddString(sizes, Text(choices[i]).c_str());
                sizesShown[i] = choices[i];
            }
        }
        SelectIfChanged(sizes, picture.compact ? 1 : 0);
        SetCheckIfChanged(mirror, picture.mirror);
        const int percent = static_cast<int>(std::lround(picture.cropSize * kCropSteps));
        SetSliderIfChanged(crop, percent);
        SetTextIfChanged(cropLabel, std::format(L"Crop  {}%", percent));
    }

    void RefreshCamera(bool available, const phonecam::control::CapabilityInfo& capabilities) {
        // Keep showing the last phone's controls (disabled) across a short reconnect.
        if (available) {
            const bool sameLenses =
                std::equal(lenses.begin(), lenses.end(), capabilities.lenses.begin(), capabilities.lenses.end(),
                           [](const LensInfo& a, const LensInfo& b) { return a.cameraId == b.cameraId; });
            lenses = capabilities.lenses;
            if (!sameLenses) {
                ComboBox_ResetContent(lens);
                for (size_t i = 0; i < lenses.size(); ++i) ComboBox_AddString(lens, LensName(lenses, i).c_str());
            }
        }
        const auto camera = app.GetCamera();
        const LensInfo* current = CurrentLens(camera);
        for (size_t i = 0; i < lenses.size(); ++i) {
            if (&lenses[i] == current) SelectIfChanged(lens, static_cast<int>(i));
        }

        const bool canZoom = available && current && current->zoomMax > current->zoomMin;
        const bool canExpose = available && current && current->evMax > current->evMin;
        const bool canFocus = available && current && !current->afModes.empty();
        const bool canLight = available && current && current->hasTorch;
        EnableIfChanged(lens, available && lenses.size() > 1);
        EnableIfChanged(lensLabel, available);
        EnableIfChanged(zoom, canZoom);
        EnableIfChanged(zoomLabel, canZoom);
        EnableIfChanged(exposure, canExpose);
        EnableIfChanged(exposureLabel, canExpose);
        EnableIfChanged(focus, canFocus);
        EnableIfChanged(focusLabel, canFocus);
        EnableIfChanged(torch, canLight);
        EnableIfChanged(reset, canZoom || canExpose || canFocus);
        if (!current) return;

        if (current->zoomMax > current->zoomMin) {
            SetSliderRange(zoom, static_cast<int>(std::lround(current->zoomMin * kZoomSteps)),
                           static_cast<int>(std::lround(current->zoomMax * kZoomSteps)), kZoomSteps / 2);
            SetSliderIfChanged(zoom, static_cast<int>(std::lround(camera.zoom * kZoomSteps)));
            SetTextIfChanged(zoomLabel, std::format(L"Zoom  {:.1f}×", camera.zoom));
        }
        if (current->evMax > current->evMin) {
            SetSliderRange(exposure, current->evMin, current->evMax, 3);
            SetSliderIfChanged(exposure, camera.ev);
            // Steps mean nothing to a person; EV does, when the phone says how big a step is.
            const float value = static_cast<float>(camera.ev) * (current->evStep > 0 ? current->evStep : 1.0f);
            SetTextIfChanged(exposureLabel, std::format(L"Exposure  {:+.1f}", value));
        }
        if (focusModes != current->afModes) {
            focusModes = current->afModes;
            ComboBox_ResetContent(focus);
            for (const int afMode : focusModes) ComboBox_AddString(focus, FocusName(afMode));
        }
        const auto selected = std::find(focusModes.begin(), focusModes.end(), camera.focus);
        SelectIfChanged(focus, selected == focusModes.end() ? -1 : static_cast<int>(selected - focusModes.begin()));
        SetCheckIfChanged(torch, camera.torch);
    }

    HWND PresetEdit() const {
        COMBOBOXINFO info{sizeof(info)};
        return GetComboBoxInfo(preset, &info) ? info.hwndItem : nullptr;
    }

    // An editable combo box marks its whole text as selected whenever it is filled or resized,
    // which looks like it has the focus when it doesn't.
    void ClearPresetHighlight() {
        if (GetFocus() != PresetEdit()) SendMessageW(preset, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
    }

    void RefreshPresets() {
        presets = app.Presets();
        const std::wstring typed = WindowText(preset);
        ComboBox_ResetContent(preset);
        for (const auto& item : presets) ComboBox_AddString(preset, item.name.c_str());
        SetWindowTextW(preset, typed.c_str());
    }

    void RefreshActivePreset() {
        const HWND focused = GetFocus();
        if (focused != PresetEdit() && !ComboBox_GetDroppedState(preset)) {
            const std::wstring active = app.ActivePreset(presets);
            if (WindowText(preset) != active) {
                int index = -1;
                for (size_t i = 0; i < presets.size(); ++i) {
                    if (presets[i].name == active) index = static_cast<int>(i);
                }
                ComboBox_SetCurSel(preset, index);  // -1 clears the text, showing "Custom settings"
                ClearPresetHighlight();
            }
        }
        const std::wstring typed = phonecam::settings::TrimName(WindowText(preset));
        bool own = false, builtIn = false;
        for (const auto& item : presets) {
            if (!phonecam::settings::SameName(item.name, typed)) continue;
            (item.builtIn ? builtIn : own) = true;
        }
        EnableIfChanged(presetSave, !typed.empty() && !builtIn);
        EnableIfChanged(presetDelete, own);
    }

    // --- what the user does ---

    void OnCommand(int id, int code) {
        switch (id) {
            case IDOK:  // Enter, from IsDialogMessage
                if (GetFocus() == PresetEdit()) SavePreset();
                return;
            case kStartStop:
                connection.SetStreaming(connection.GetSnapshot().state == State::Stopped);
                break;
            case kPreset:
                if (code == CBN_SELENDOK) ApplySelectedPreset();
                if (code != CBN_SELENDOK && code != CBN_EDITCHANGE && code != CBN_KILLFOCUS) return;
                break;
            case kPresetSave: SavePreset(); break;
            case kPresetDelete: DeletePreset(); break;
            case kAdjust:
                adjustOpen = !adjustOpen;
                ShowAdjustments();
                Layout();
                break;
            case kRotation: {
                const int index = ComboBox_GetCurSel(rotation);
                if (code != CBN_SELCHANGE || index < 0) return;
                ChangePicture([&](Picture& p) { p.turn = kRotations[index].rotation; });
                break;
            }
            case kShape: {
                const int index = ComboBox_GetCurSel(shape);
                if (code != CBN_SELCHANGE || index < 0) return;
                ChangePicture([&](Picture& p) { p.shape = kShapes[index].shape; });
                break;
            }
            case kSize: {
                const int index = ComboBox_GetCurSel(sizes);
                if (code != CBN_SELCHANGE || index < 0) return;
                ChangePicture([&](Picture& p) { p.compact = index == 1; });
                break;
            }
            case kMirror: ChangePicture([&](Picture& p) { p.mirror = Button_GetCheck(mirror) == BST_CHECKED; }); break;
            case kLens: {
                const int index = ComboBox_GetCurSel(lens);
                if (code != CBN_SELCHANGE || index < 0 || index >= static_cast<int>(lenses.size())) return;
                app.ChooseLens(lenses[static_cast<size_t>(index)].cameraId);
                return;  // the list follows once the phone confirms
            }
            case kFocus: {
                const int index = ComboBox_GetCurSel(focus);
                if (code != CBN_SELCHANGE || index < 0 || index >= static_cast<int>(focusModes.size())) return;
                app.ChooseFocus(focusModes[static_cast<size_t>(index)]);
                break;
            }
            case kTorch: app.SetTorch(Button_GetCheck(torch) == BST_CHECKED); return;
            case kReset: app.ResetCamera(); break;
            case kMode: {
                const int index = ComboBox_GetCurSel(mode);
                if (code != CBN_SELCHANGE || index < 0) return;
                app.ChooseMode(kModes[index].mode);
                break;
            }
            default: return;
        }
        Refresh();
    }

    void OnSlider(HWND slider) {
        const int position = SliderPosition(slider);
        if (slider == crop) {
            ScaleCropTo(static_cast<float>(position) / kCropSteps);
        } else if (slider == zoom) {
            app.ChooseZoom(static_cast<float>(position) / kZoomSteps);
        } else if (slider == exposure) {
            app.ChooseEv(position);
        }
        Refresh();
    }

    template <typename Change>
    void ChangePicture(Change change) {
        Picture picture = app.GetPicture();
        change(picture);
        app.SetPicture(picture);
        InvalidateRect(preview, nullptr, FALSE);  // the crop box, even between frames
    }

    void ScaleCropTo(float value) {
        const SeenRect box = CropBox();
        ChangePicture([&](Picture& p) { phonecam::picture::ScaleCrop(p, box, LargestBox(p), value); });
    }

    void MoveCropBy(float x, float y) {
        const SeenRect box = CropBox();
        ChangePicture([&](Picture& p) { phonecam::picture::MoveCropTo(p, box, box.left + x, box.top + y); });
    }

    void ResetCrop() {
        ChangePicture([](Picture& p) {
            const Picture defaults;
            p.cropSize = defaults.cropSize;
            p.cropX = defaults.cropX;
            p.cropY = defaults.cropY;
        });
        RefreshPicture();
    }

    void Hint(const std::wstring& text) { SetTextIfChanged(presetHint, text); }

    void ApplySelectedPreset() {
        const int index = ComboBox_GetCurSel(preset);
        if (index < 0 || index >= static_cast<int>(presets.size())) return;
        app.ApplyPreset(presets[static_cast<size_t>(index)].name);
        Hint(L"Preset applied.");
    }

    void SavePreset() {
        const std::wstring name = phonecam::settings::TrimName(WindowText(preset));
        const std::wstring problem = phonecam::settings::PresetNameProblem(name);
        if (!problem.empty()) {
            Hint(problem);
            return;
        }
        Hint(app.SavePreset(name) ? L"Saved." : L"Could not save the preset.");
        RefreshPresets();
    }

    void DeletePreset() {
        const std::wstring name = phonecam::settings::TrimName(WindowText(preset));
        const std::wstring question = std::format(L"Delete the preset “{}”?", name);
        if (MessageBoxW(window, question.c_str(), L"PhoneCam", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
        Hint(app.DeletePreset(name) ? L"Deleted." : L"Could not delete the preset.");
        SetWindowTextW(preset, L"");
        RefreshPresets();
    }

    // --- preview ---

    bool EnsureTarget() {
        if (target) return true;
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
            return false;
        }
        if (!textFormat) {
            if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(S(16)), L"", &textFormat))) {
                return false;
            }
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (!smallFormat) {
            if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(S(12)), L"", &smallFormat))) {
                return false;
            }
            smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        RECT client{};
        GetClientRect(preview, &client);
        // 96 dpi: work in real pixels, the layout is already scaled.
        // Software on purpose: for one small picture at 15 fps it costs about the same CPU as the
        // GPU path, and it avoids loading a graphics driver into this process (measured: 42 MB
        // private memory with the window open instead of 94 MB).
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(), 96.0f, 96.0f);
        return SUCCEEDED(d2d->CreateHwndRenderTarget(
            properties,
            D2D1::HwndRenderTargetProperties(preview, D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom))),
            &target));
    }

    void OnPreviewFrame() {
        if (!connection.Bridge().TakePreview(image)) return;
        hasImage = true;
        bitmapStale = true;
        InvalidateRect(preview, nullptr, FALSE);
    }

    void UploadBitmap() {
        if (!bitmapStale || !hasImage) return;
        const auto size = D2D1::SizeU(image.width, image.height);
        if (bitmap) {
            const auto existing = bitmap->GetPixelSize();
            if (existing.width != size.width || existing.height != size.height) bitmap.Reset();
        }
        const UINT32 pitch = image.width * 4;
        if (bitmap) {
            bitmap->CopyFromMemory(nullptr, image.bgra.data(), pitch);
        } else {
            const auto properties =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            target->CreateBitmap(size, image.bgra.data(), pitch, properties, &bitmap);
        }
        bitmapStale = false;
    }

    // Everything the phone sees, with the part outside the crop darkened: what is bright is exactly
    // what video apps receive.
    void DrawPreview() {
        if (!EnsureTarget()) return;
        const auto snapshot = connection.GetSnapshot();
        const auto view = connection.Bridge().GetView();
        const Size seen = view.Seen();
        const auto area = target->GetSize();
        const float margin = static_cast<float>(S(16)), footer = static_cast<float>(S(24));
        const float scale = (std::min)((area.width - 2 * margin) / static_cast<float>(seen.width),
                                       (area.height - 2 * margin - footer) / static_cast<float>(seen.height));
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->Clear(D2D1::ColorF(0x2B2F36));
        ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(D2D1::ColorF(0xC8CCD4), &brush);
        if (scale > 0 && brush) {
            const float width = static_cast<float>(seen.width) * scale, height = static_cast<float>(seen.height) * scale;
            const float left = (area.width - width) / 2, top = (area.height - footer - height) / 2;
            pictureRect = D2D1::RectF(left, top, left + width, top + height);
            UploadBitmap();
            if (snapshot.state == State::Streaming && bitmap) {
                DrawPicture(view);
                DrawCropBox(brush.Get());
                const std::wstring footerText =
                    std::format(L"Video apps receive {}.  Drag the box or its corners to crop.", Text(Output()));
                brush->SetColor(D2D1::ColorF(0xC8CCD4));
                target->DrawTextW(footerText.c_str(), static_cast<UINT32>(footerText.size()), smallFormat.Get(),
                                  D2D1::RectF(0, area.height - footer - margin / 2, area.width, area.height - margin / 2),
                                  brush.Get());
            } else {
                const std::wstring message = Widen(snapshot.message);
                target->DrawTextW(message.c_str(), static_cast<UINT32>(message.size()), textFormat.Get(),
                                  D2D1::RectF(margin, 0, area.width - margin, area.height), brush.Get());
            }
        }
        if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
            bitmap.Reset();
            target.Reset();
            bitmapStale = hasImage;
        }
    }

    // The same rotate, then mirror, as windows/vcam/FrameGenerator.
    void DrawPicture(const phonecam::picture::View& view) {
        const auto pixels = bitmap->GetSize();
        const float across = view.Swaps() ? pixels.height : pixels.width;
        const float fitScale = (pictureRect.right - pictureRect.left) / across;
        const float angle = static_cast<float>(static_cast<int>(view.rotation) * 90);
        target->PushAxisAlignedClip(pictureRect, D2D1_ANTIALIAS_MODE_ALIASED);
        target->SetTransform(D2D1::Matrix3x2F::Rotation(angle) *
                             D2D1::Matrix3x2F::Scale(view.mirror ? -fitScale : fitScale, fitScale) *
                             D2D1::Matrix3x2F::Translation((pictureRect.left + pictureRect.right) / 2,
                                                           (pictureRect.top + pictureRect.bottom) / 2));
        target->DrawBitmap(bitmap.Get(),
                           D2D1::RectF(-pixels.width / 2, -pixels.height / 2, pixels.width / 2, pixels.height / 2));
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PopAxisAlignedClip();
    }

    D2D1_RECT_F ToPixels(const SeenRect& box) const {
        const float width = pictureRect.right - pictureRect.left, height = pictureRect.bottom - pictureRect.top;
        const float left = pictureRect.left + box.left * width, top = pictureRect.top + box.top * height;
        return D2D1::RectF(left, top, left + box.width * width, top + box.height * height);
    }

    void DrawCropBox(ID2D1SolidColorBrush* brush) {
        const auto box = ToPixels(CropBox());
        brush->SetColor(D2D1::ColorF(0x000000, 0.62f));
        target->FillRectangle(D2D1::RectF(pictureRect.left, pictureRect.top, pictureRect.right, box.top), brush);
        target->FillRectangle(D2D1::RectF(pictureRect.left, box.bottom, pictureRect.right, pictureRect.bottom), brush);
        target->FillRectangle(D2D1::RectF(pictureRect.left, box.top, box.left, box.bottom), brush);
        target->FillRectangle(D2D1::RectF(box.right, box.top, pictureRect.right, box.bottom), brush);

        // Blue while the arrow keys move it.
        const bool focused = GetFocus() == preview;
        brush->SetColor(focused ? D2D1::ColorF(0x4CA3FF) : D2D1::ColorF(0xFFFFFF, 0.9f));
        const float line = static_cast<float>(S(1)), inset = line / 2;
        target->DrawRectangle(D2D1::RectF(box.left + inset, box.top + inset, box.right - inset, box.bottom - inset), brush, line);
        const float thick = static_cast<float>(S(4)), length = static_cast<float>(S(18));
        for (const bool atLeft : {true, false}) {
            for (const bool atTop : {true, false}) {
                const float x = atLeft ? box.left : box.right, y = atTop ? box.top : box.bottom;
                const float inX = atLeft ? 1.0f : -1.0f, inY = atTop ? 1.0f : -1.0f;
                const auto bar = [&](float w, float h) {
                    target->FillRectangle(D2D1::RectF((std::min)(x, x + inX * w), (std::min)(y, y + inY * h),
                                                      (std::max)(x, x + inX * w), (std::max)(y, y + inY * h)),
                                          brush);
                };
                bar(length, thick);
                bar(thick, length);
            }
        }
    }

    Grab GrabAt(POINT point) const {
        if (!hasImage || connection.GetSnapshot().state != State::Streaming) return Grab::None;
        const auto box = ToPixels(CropBox());
        const float x = static_cast<float>(point.x), y = static_cast<float>(point.y), reach = static_cast<float>(S(16));
        const bool nearLeft = std::fabs(x - box.left) <= reach, nearRight = std::fabs(x - box.right) <= reach;
        const bool nearTop = std::fabs(y - box.top) <= reach, nearBottom = std::fabs(y - box.bottom) <= reach;
        if (nearTop && nearLeft) return Grab::TopLeft;
        if (nearTop && nearRight) return Grab::TopRight;
        if (nearBottom && nearLeft) return Grab::BottomLeft;
        if (nearBottom && nearRight) return Grab::BottomRight;
        const bool inside = x >= box.left && x < box.right && y >= box.top && y < box.bottom;
        const bool canMove = box.right - box.left < pictureRect.right - pictureRect.left - 1 ||
                             box.bottom - box.top < pictureRect.bottom - pictureRect.top - 1;
        return inside && canMove ? Grab::Move : Grab::None;
    }

    static LPCWSTR CursorFor(Grab what) {
        switch (what) {
            case Grab::Move: return IDC_SIZEALL;
            case Grab::TopLeft:
            case Grab::BottomRight: return IDC_SIZENWSE;
            case Grab::TopRight:
            case Grab::BottomLeft: return IDC_SIZENESW;
            case Grab::None: break;
        }
        return IDC_ARROW;
    }

    void DragCrop(POINT point) {
        const float width = pictureRect.right - pictureRect.left, height = pictureRect.bottom - pictureRect.top;
        if (width <= 0 || height <= 0) return;
        ChangePicture([&](Picture& p) {
            if (grab == Grab::Move) {
                phonecam::picture::MoveCropTo(p, grabBox, grabBox.left + static_cast<float>(point.x - grabPoint.x) / width,
                                              grabBox.top + static_cast<float>(point.y - grabPoint.y) / height);
            } else {
                phonecam::picture::ResizeCrop(p, grabBox, LargestBox(p), grab == Grab::TopLeft || grab == Grab::BottomLeft,
                                              grab == Grab::TopLeft || grab == Grab::TopRight,
                                              (static_cast<float>(point.x) - pictureRect.left) / width,
                                              (static_cast<float>(point.y) - pictureRect.top) / height);
            }
        });
        RefreshPicture();
    }

    bool OnPreviewKey(WPARAM key) {
        const float step = GetKeyState(VK_SHIFT) < 0 ? 0.05f : 0.01f;
        switch (key) {
            case VK_LEFT: MoveCropBy(-step, 0); break;
            case VK_RIGHT: MoveCropBy(step, 0); break;
            case VK_UP: MoveCropBy(0, -step); break;
            case VK_DOWN: MoveCropBy(0, step); break;
            case VK_ADD:
            case VK_OEM_PLUS: ScaleCropTo(app.GetPicture().cropSize + step); break;
            case VK_SUBTRACT:
            case VK_OEM_MINUS: ScaleCropTo(app.GetPicture().cropSize - step); break;
            default: return false;
        }
        RefreshPicture();
        return true;
    }

    // --- window procedures ---

    static Impl* From(HWND hwnd, UINT message, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* impl = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
            return impl;
        }
        return reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* impl = From(hwnd, message, lParam);
        if (!impl) return DefWindowProcW(hwnd, message, wParam, lParam);
        switch (message) {
            case WM_CREATE:
                impl->window = hwnd;
                impl->OnCreate();
                return 0;
            case WM_SIZE:
                // Nobody can see a minimized preview; don't make one.
                if (wParam == SIZE_MINIMIZED) {
                    impl->EnablePreview(false);
                } else {
                    impl->EnablePreview(true);
                    impl->Layout();
                }
                return 0;
            case WM_GETMINMAXINFO: {
                RECT size{0, 0, impl->S(kMinPreviewWidth + kPanelWidth), impl->S(kClientHeight)};
                AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, impl->dpi);
                reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {size.right - size.left, size.bottom - size.top};
                return 0;
            }
            case WM_DPICHANGED: {
                impl->dpi = HIWORD(wParam);
                impl->textFormat.Reset();
                impl->smallFormat.Reset();
                impl->bitmap.Reset();
                impl->target.Reset();
                impl->bitmapStale = impl->hasImage;
                impl->CreateFonts();
                impl->ApplyFonts();
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                impl->Layout();
                return 0;
            }
            case WM_TIMER:
                if (wParam == kRefreshTimer && !IsIconic(hwnd)) impl->Refresh();
                return 0;
            case kPreviewMessage:
                impl->OnPreviewFrame();
                return 0;
            case WM_COMMAND:
                impl->OnCommand(LOWORD(wParam), HIWORD(wParam));
                return 0;
            case WM_HSCROLL:
                if (lParam) impl->OnSlider(reinterpret_cast<HWND>(lParam));
                return 0;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                const bool quiet = GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == kQuietText;
                SetTextColor(dc, GetSysColor(quiet ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) {
                    const HWND focused = GetFocus();
                    if (focused && IsChild(hwnd, focused)) impl->lastFocus = focused;
                }
                break;
            case WM_SETFOCUS:
                if (impl->lastFocus) SetFocus(impl->lastFocus);
                return 0;
            case WM_CLOSE:
                impl->OnClose();
                return 0;
            case WM_DESTROY:
                impl->OnDestroy();
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Impl* impl = From(hwnd, message, lParam);
        if (!impl) return DefWindowProcW(hwnd, message, wParam, lParam);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        switch (message) {
            case WM_PAINT:
                impl->DrawPreview();
                ValidateRect(hwnd, nullptr);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE:
                if (impl->target) impl->target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    ScreenToClient(hwnd, &cursor);
                    SetCursor(LoadCursorW(nullptr, CursorFor(impl->grab != Grab::None ? impl->grab : impl->GrabAt(cursor))));
                    return TRUE;
                }
                break;
            case WM_LBUTTONDOWN:
                SetFocus(hwnd);
                impl->grab = impl->GrabAt(point);
                if (impl->grab != Grab::None) {
                    impl->grabPoint = point;
                    impl->grabBox = impl->CropBox();
                    SetCapture(hwnd);
                }
                return 0;
            case WM_MOUSEMOVE:
                if (impl->grab != Grab::None) impl->DragCrop(point);
                return 0;
            case WM_LBUTTONUP:
                if (impl->grab != Grab::None) ReleaseCapture();
                return 0;
            case WM_CAPTURECHANGED:
                impl->grab = Grab::None;
                return 0;
            case WM_LBUTTONDBLCLK:
                impl->ResetCrop();
                return 0;
            case WM_MOUSEWHEEL:
                // Forward zooms in, i.e. a smaller crop.
                impl->ScaleCropTo(impl->app.GetPicture().cropSize * (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 0.95f : 1.0f / 0.95f));
                impl->RefreshPicture();
                return 0;
            case WM_GETDLGCODE:
                return DLGC_WANTARROWS | DLGC_WANTCHARS;
            case WM_KEYDOWN:
                if (impl->OnPreviewKey(wParam)) return 0;
                break;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

MainWindow::MainWindow(phonecam::app::AppController& app, phonecam::connection::ConnectionManager& connection,
                       std::function<void(const std::string&)> notify)
    : impl_(std::make_unique<Impl>(app, connection, std::move(notify))) {}

MainWindow::~MainWindow() {
    if (!impl_->window) return;
    // Exit from the tray with the window open: remember where it was, without the close hint.
    impl_->SavePlacement();
    DestroyWindow(impl_->window);
}

void MainWindow::Show() { impl_->Show(); }

bool MainWindow::FilterMessage(void* msg) {
    return impl_->window && IsDialogMessageW(impl_->window, static_cast<MSG*>(msg));
}

}  // namespace phonecam::ui
