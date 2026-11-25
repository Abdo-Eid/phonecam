// Offline checks for the picture path, saved settings and camera replay: no phone, no camera.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "camera/CameraSync.h"
#include "picture/Framing.h"
#include "picture/Nv12.h"
#include "settings/Settings.h"
#include "vcam_ctl/VCamControl.h"

namespace {

using namespace phonecam;
using picture::Crop;
using picture::Picture;
using picture::SeenRect;
using picture::Shape;
using picture::Size;
using picture::View;
using shm::Rotation;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr Rotation kRotations[] = {Rotation::Deg0, Rotation::Deg90, Rotation::Deg180, Rotation::Deg270};
constexpr Shape kShapes[] = {Shape::Follow, Shape::Wide, Shape::Classic, Shape::Square, Shape::ClassicTall, Shape::Tall};
constexpr Size kSources[] = {{1920, 1080}, {1280, 720}, {1440, 1080}, {1600, 1200}};

Picture Choose(Shape shape, float size = 1.0f, float x = 0.5f, float y = 0.5f, bool compact = false) {
    Picture p;
    p.shape = shape;
    p.compact = compact;
    p.cropSize = size;
    p.cropX = x;
    p.cropY = y;
    return p;
}

Crop CropFor(const View& view, const Picture& p) { return picture::ComputeCrop(view, p, picture::OutputSize(p, view)); }
SeenRect BoxFor(const View& view, const Picture& p) { return picture::ToSeen(view, CropFor(view, p)); }

// What the virtual camera DLL does with a frame (FrameGenerator: rotate, scale the whole frame to
// fit, center), reduced to how much of the app's frame stays black.
double BlackPixels(const Crop& crop, const View& view, Size output) {
    const double width = view.Swaps() ? crop.height : crop.width;
    const double height = view.Swaps() ? crop.width : crop.height;
    const double scale = (std::min)(output.width / width, output.height / height);
    return (output.width - width * scale) + (output.height - height * scale);
}

void TestNoShapeHasBlackBars() {
    for (const auto source : kSources) {
        for (const auto rotation : kRotations) {
            for (const bool mirror : {false, true}) {
                const View view{source, rotation, mirror};
                for (const auto shape : kShapes) {
                    for (const bool compact : {false, true}) {
                        for (const float size : {0.0f, 0.2f, 0.37f, 0.8f, 1.0f, 5.0f}) {
                            for (const float at : {-1.0f, 0.0f, 0.31f, 0.5f, 1.0f, 9.0f}) {
                                const Picture p = Choose(shape, size, at, 1.0f - at, compact);
                                const Size output = picture::OutputSize(p, view);
                                const Crop c = CropFor(view, p);
                                Check(((c.x | c.y | c.width | c.height) & 1) == 0, "crop must be even-aligned for NV12");
                                Check(c.width >= 64 && c.height >= 64, "crop keeps a usable size");
                                Check(c.x + c.width <= source.width && c.y + c.height <= source.height, "crop stays inside");
                                Check(BlackPixels(c, view, output) < 1e-6, "every shape fills the app's frame");
                            }
                        }
                        const Crop largest = CropFor(view, Choose(shape, 1.0f, 0.5f, 0.5f, compact));
                        Check(largest.width + 32 > source.width || largest.height + 32 > source.height,
                              "by default, as large as the phone's picture allows");
                    }
                }
            }
        }
    }
}

// The virtual camera offers this one size; see OutputSize for why the widths are aligned.
void TestOfferedSizes() {
    const double ratios[] = {0, 16.0 / 9, 4.0 / 3, 1.0, 3.0 / 4, 9.0 / 16};
    const View upright{{1920, 1080}, Rotation::Deg90, false}, sideways{{1920, 1080}, Rotation::Deg0, false};
    for (const auto shape : kShapes) {
        for (const bool compact : {false, true}) {
            const Size size = picture::OutputSize(Choose(shape, 1, 0.5f, 0.5f, compact), upright);
            Check(size.width % 64 == 0 || size == Size{1920, 1080}, "row length the DirectShow bridge handles");
            Check(size.width % 8 == 0 && size.height % 8 == 0 && size.width / 8 <= 0xFFF && size.height / 8 <= 0xFFF,
                  "fits the ring's packed setting");
            Check(size.width >= 128 && size.height >= 128 && size.width <= 2560 && size.height <= 2560,
                  "inside the DLL's limits");
            if (shape != Shape::Follow) {
                const double ratio = static_cast<double>(size.width) / size.height;
                Check(std::abs(ratio - ratios[static_cast<size_t>(shape)]) < 1e-9, "offered size has exactly the named shape");
                Check(size == picture::OutputSize(Choose(shape, 1, 0.5f, 0.5f, compact), sideways),
                      "a named shape does not depend on how the phone is held");
            }
        }
    }
    // Follow the phone: upright is tall, sideways is wide, and nothing is cut off either way.
    const Picture follow;
    Check(picture::OutputSize(follow, upright) == Size{1152, 2048}, "upright phone gives tall video");
    Check(picture::OutputSize(follow, sideways) == Size{1920, 1080}, "sideways phone gives wide video");
    for (const auto rotation : kRotations) {
        const View view{{1920, 1080}, rotation, false};
        Check(CropFor(view, follow) == Crop{0, 0, 1920, 1080}, "following the phone shows everything");
    }
    Check(CropFor(upright, Choose(Shape::Square)) == Crop{420, 0, 1080, 1080}, "square uses the full width of an upright phone");
    // An extra turn counts as turning the phone.
    const View turned{{1920, 1080}, picture::Combine(Rotation::Deg90, Rotation::Deg90), false};
    Check(picture::OutputSize(follow, turned) == Size{1920, 1080}, "extra turn");
}

void TestCameraIsRecreatedOnlyWhenFree() {
    const Size square{1152, 1152}, tall{1152, 2048}, wide{1920, 1080};
    vcam_ctl::OfferedSizeWatch watch(square);
    Check(!watch.Due(square, false, 1000), "nothing to do while the size is unchanged");
    Check(!watch.Due(tall, false, 1000) && !watch.Due(tall, false, 1500), "waits for the choice to settle");
    Check(!watch.Due(wide, false, 1550) && !watch.Due(wide, false, 2100), "a further change starts the wait again");
    Check(!watch.Due(wide, true, 9000), "never taken away from an app");
    Check(watch.Due(wide, false, 9250), "re-created once it is free");
    watch.Failed(9250);
    Check(!watch.Due(wide, false, 9500) && watch.Due(wide, false, 12300), "a failure is retried, not hammered");
    watch.Created();
    Check(!watch.Due(wide, false, 12500), "done");
    Check(!watch.Due(tall, true, 13000) && !watch.Due(wide, true, 14000) && !watch.Due(wide, false, 20000),
          "changing back while in use needs nothing");
}

// Maps a point of the frame through the same rotate-then-mirror the DLL applies.
std::pair<double, double> SeenPoint(double x, double y, const View& view) {
    x -= view.source.width / 2.0;
    y -= view.source.height / 2.0;
    for (int turn = 0; turn < static_cast<int>(view.rotation); ++turn) {
        const double nextX = -y;
        y = x;
        x = nextX;
    }
    if (view.mirror) x = -x;
    const Size seen = view.Seen();
    return {(x + seen.width / 2.0) / seen.width, (y + seen.height / 2.0) / seen.height};
}

void TestCropMatchesWhatTheViewerSees() {
    for (const auto rotation : kRotations) {
        for (const bool mirror : {false, true}) {
            const View view{{1920, 1080}, rotation, mirror};
            for (const auto shape : {Shape::Square, Shape::Wide, Shape::Tall, Shape::Classic}) {
                for (const float x : {0.0f, 0.25f, 1.0f}) {
                    for (const float y : {0.0f, 0.6f, 1.0f}) {
                        const Picture p = Choose(shape, 0.5f, x, y);
                        const Crop c = CropFor(view, p);
                        const SeenRect box = picture::ToSeen(view, c);
                        const auto one = SeenPoint(c.x, c.y, view), two = SeenPoint(c.x + c.width, c.y + c.height, view);
                        Check(std::abs((std::min)(one.first, two.first) - box.left) < 1e-5 &&
                                  std::abs((std::min)(one.second, two.second) - box.top) < 1e-5 &&
                                  std::abs(std::abs(one.first - two.first) - box.width) < 1e-5 &&
                                  std::abs(std::abs(one.second - two.second) - box.height) < 1e-5,
                              "the drawn box is the published crop");
                        Check(std::abs(box.left - x * (1 - box.width)) < 0.004 && std::abs(box.top - y * (1 - box.height)) < 0.004,
                              "position is measured from the top left as seen");
                    }
                }
            }
        }
    }
}

void TestEditingTheCrop() {
    for (const auto rotation : kRotations) {
        for (const bool mirror : {false, true}) {
            const View view{{1920, 1080}, rotation, mirror};
            for (const auto shape : {Shape::Square, Shape::Wide, Shape::Tall}) {
                Picture p = Choose(shape, 0.5f, 0.5f, 0.5f);
                const Size output = picture::OutputSize(p, view);
                const SeenRect largest = picture::LargestCrop(view, p, output);

                SeenRect box = BoxFor(view, p);
                picture::MoveCropTo(p, box, 0.1f, 0.2f);
                SeenRect moved = BoxFor(view, p);
                Check(std::abs(moved.left - 0.1f) < 0.004f && std::abs(moved.top - 0.2f) < 0.004f, "box goes where it is dragged");
                picture::MoveCropTo(p, box, -3.0f, 9.0f);
                moved = BoxFor(view, p);
                Check(moved.left < 0.002f && moved.top + moved.height > 0.998f, "dragging past the edge stops at the edge");

                for (const bool left : {false, true}) {
                    for (const bool top : {false, true}) {
                        for (const float reach : {-0.3f, 0.05f, 0.2f, 0.5f, 3.0f}) {
                            p = Choose(shape, 0.5f, 0.5f, 0.5f);
                            box = BoxFor(view, p);
                            const float anchorX = left ? box.left + box.width : box.left;
                            const float anchorY = top ? box.top + box.height : box.top;
                            picture::ResizeCrop(p, box, largest, left, top, anchorX + (left ? -reach : reach),
                                                anchorY + (top ? -reach : reach));
                            Check(p.cropSize >= picture::kMinCropSize - 1e-4f && p.cropSize <= 1.0f, "size stays in range");
                            const SeenRect resized = BoxFor(view, p);
                            const float keptX = left ? resized.left + resized.width : resized.left;
                            const float keptY = top ? resized.top + resized.height : resized.top;
                            Check(std::abs(keptX - anchorX) < 0.015f && std::abs(keptY - anchorY) < 0.015f,
                                  "the opposite corner stays put");
                            Check(resized.left > -1e-4f && resized.top > -1e-4f && resized.left + resized.width < 1.0001f &&
                                      resized.top + resized.height < 1.0001f,
                                  "a resized box stays inside the picture");
                        }
                    }
                }

                p = Choose(shape, 0.5f, 0.5f, 0.5f);
                box = BoxFor(view, p);
                picture::ScaleCrop(p, box, largest, 0.3f);
                const SeenRect scaled = BoxFor(view, p);
                Check(std::abs(scaled.left + scaled.width / 2 - box.left - box.width / 2) < 0.004f &&
                          std::abs(scaled.top + scaled.height / 2 - box.top - box.height / 2) < 0.004f,
                      "wheel zooms around the box's center");
                picture::ScaleCrop(p, scaled, largest, 9.0f);
                Check(p.cropSize == 1.0f, "cannot grow past the picture");
            }
        }
    }
}

void TestNv12() {
    constexpr uint32_t kWidth = 8, kHeight = 6;
    std::vector<uint8_t> source(picture::Nv12Size(kWidth, kHeight));
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i);
    const Crop crop{2, 2, 4, 4};
    std::vector<uint8_t> out(picture::Nv12Size(crop.width, crop.height));
    picture::CropNv12(source.data(), kWidth, kHeight, crop, out.data());
    for (uint32_t y = 0; y < crop.height; ++y)
        for (uint32_t x = 0; x < crop.width; ++x)
            Check(out[y * crop.width + x] == source[(y + 2) * kWidth + x + 2], "luma crop");
    for (uint32_t y = 0; y < crop.height / 2; ++y)
        for (uint32_t x = 0; x < crop.width; ++x)
            Check(out[(crop.height + y) * crop.width + x] == source[kHeight * kWidth + (y + 1) * kWidth + x + 2],
                  "chroma crop keeps U/V pairs");

    for (const uint32_t step : {1u, 2u}) {
        std::vector<uint8_t> flat(picture::Nv12Size(4, 4), 128);
        std::vector<uint8_t> bgra;
        uint32_t width = 0, height = 0;
        for (const auto& [luma, expected] : {std::pair{16, 0}, {235, 255}}) {
            std::fill(flat.begin(), flat.begin() + 16, static_cast<uint8_t>(luma));
            picture::Nv12ToBgra(flat.data(), 4, 4, step, width, height, bgra);
            Check(width == 4 / step && height == 4 / step, "preview size");
            Check(bgra.size() == static_cast<size_t>(width) * height * 4, "preview buffer size");
            for (size_t i = 0; i < bgra.size(); i += 4)
                for (size_t c = 0; c < 3; ++c)
                    Check(std::abs(static_cast<int>(bgra[i + c]) - expected) <= 1, "black and white survive the conversion");
        }
    }
}

void TestSettings() {
    const std::wstring root = L"Software\\PhoneCamTests-" + std::to_wstring(GetCurrentProcessId());
    struct Cleanup {
        std::wstring root;
        ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()); }
    } cleanup{root};
    const settings::Store store(root);

    Check(store.LoadCurrent() == settings::Profile{}, "first run uses the defaults");
    settings::Profile profile;
    profile.picture = Picture{Rotation::Deg270, true, Shape::Square, true, 0.5f, 0.25f, 0.75f};
    profile.camera = {std::string("1"), 2.5f, -3, 4};
    Check(store.SaveCurrent(profile), "save");
    Check(settings::Store(root).LoadCurrent() == profile, "everything survives a restart, negative exposure included");
    Check(store.SaveCurrent(settings::Profile{}), "save again");
    Check(store.LoadCurrent() == settings::Profile{}, "cleared choices do not linger");

    const size_t builtIn = settings::BuiltInPresets().size();
    Check(store.Presets().size() == builtIn, "only built-in presets at first");
    Check(store.SavePreset(L"  Zoom calls ", profile), "save preset");
    Check(store.SavePreset(L"اجتماع", settings::Profile{}), "Arabic preset names work");
    auto presets = store.Presets();
    Check(presets.size() == builtIn + 2, "two own presets");
    Check(presets[builtIn].name == L"Zoom calls" && presets[builtIn].profile == profile && !presets[builtIn].builtIn,
          "preset keeps its name and contents");
    Check(store.SavePreset(L"zoom CALLS", settings::Profile{}), "same name replaces");
    Check(store.Presets().size() == builtIn + 2, "names ignore case");
    Check(!store.SavePreset(L"", profile) && !store.SavePreset(L"a\\b", profile) &&
              !store.SavePreset(std::wstring(80, L'x'), profile) &&
              !store.SavePreset(settings::BuiltInPresets().front().name, profile),
          "unusable names are refused");
    Check(!store.DeletePreset(L"") && !store.DeletePreset(settings::BuiltInPresets().front().name),
          "built-ins (and the whole preset key) cannot be deleted");
    Check(store.DeletePreset(L"Zoom calls") && store.Presets().size() == builtIn + 1, "delete own preset");

    const DWORD nonsense = 99999;
    for (const wchar_t* name : {L"Turn", L"Shape", L"CropSize", L"CropX", L"CropY"})
        RegSetKeyValueW(HKEY_CURRENT_USER, (root + L"\\Current").c_str(), name, REG_DWORD, &nonsense, sizeof(nonsense));
    Check(store.LoadCurrent() == settings::Profile{}, "damaged values fall back to defaults");

    Check(store.LoadNumber(L"Connection", 7) == 7 && store.SaveNumber(L"Connection", 2) &&
              store.LoadNumber(L"Connection", 7) == 2,
          "plain numbers");
}

struct FakePhone {
    std::vector<std::string> sent;
    uint32_t nextId = 1;
    camera::CameraSync sync{camera::CameraCommands{
        [this](const std::string& id) { return Log("lens " + id); },
        [this](float ratio) { return Log("zoom " + std::to_string(static_cast<int>(std::lround(ratio * 10)))); },
        [this](int32_t steps) { return Log("ev " + std::to_string(steps)); },
        [this](int mode) { return Log("focus " + std::to_string(mode)); },
    }};

    uint32_t Log(const std::string& what) {
        sent.push_back(what);
        return nextId++;
    }
    std::vector<std::string> Take() { return std::exchange(sent, {}); }

    void Capabilities(int64_t now) {
        control::ControlMessage m;
        m.kind = control::ControlMsgKind::Capabilities;
        control::LensInfo back, front;
        back.cameraId = "0";
        back.zoomMin = 1.0f;
        back.zoomMax = 8.0f;
        back.evMin = -12;
        back.evMax = 12;
        back.afModes = {0, 1, 2};
        front.cameraId = "1";
        front.zoomMin = 1.0f;
        front.zoomMax = 4.0f;
        front.evMin = -6;
        front.evMax = 6;
        m.capabilities.lenses = {back, front};
        sync.OnMessage(m, now);
    }
    void LensChanged(const char* id, int64_t now) {
        control::ControlMessage m;
        m.kind = control::ControlMsgKind::LensChanged;
        m.lensChanged.cameraId = id;
        sync.OnMessage(m, now);
    }
    void Settings(const char* id, int64_t now) {
        control::ControlMessage m;
        m.kind = control::ControlMsgKind::Settings;
        m.settings.cameraId = id;
        sync.OnMessage(m, now);
    }
    void Ack(uint32_t id, int result, int64_t now) {
        control::ControlMessage m;
        m.kind = control::ControlMsgKind::Ack;
        m.ack.cmdId = id;
        m.ack.result = result;
        sync.OnMessage(m, now);
    }
};

using Sent = std::vector<std::string>;

void TestCameraReplay() {
    {
        FakePhone phone;
        phone.Capabilities(0);
        phone.sync.Pump(true, 5000);
        Check(phone.Take().empty(), "nothing chosen, nothing sent");
    }
    {
        // Saved settings come back on a new session, lens first, the rest after it has settled.
        FakePhone phone;
        phone.sync.Load({std::string("1"), 9.0f, -2, 1});
        phone.Capabilities(0);
        phone.LensChanged("0", 10);
        phone.sync.Pump(false, 1000);
        Check(phone.Take().empty(), "waits until video is running");
        phone.sync.Pump(true, 1000);
        Check(phone.Take() == Sent{"lens 1"}, "lens goes first");
        phone.Ack(1, 0, 1100);
        phone.sync.Pump(true, 1200);
        Check(phone.Take().empty(), "holds everything during the lens change");
        Check(phone.sync.AwaitedLens() == "1", "a picker can already show the new lens");
        phone.LensChanged("1", 1500);
        Check(phone.sync.AwaitedLens().empty() && phone.sync.CurrentLens() == "1", "lens change done");
        phone.sync.Pump(true, 1600);
        Check(phone.Take().empty(), "lets the new camera settle");
        phone.sync.Pump(true, 2000);
        Check(phone.Take() == Sent{"zoom 40", "ev -2"}, "clamped to this lens; focus mode it lacks is skipped");
    }
    {
        // Busy is retried; a real refusal is not.
        FakePhone phone;
        phone.Capabilities(0);
        phone.LensChanged("0", 0);
        phone.sync.ChooseZoom(2.0f, 1000);
        phone.sync.Pump(true, 1000);
        Check(phone.Take() == Sent{"zoom 20"}, "sent");
        phone.Ack(1, 3, 1050);
        phone.sync.Pump(true, 1100);
        Check(phone.Take().empty(), "backs off after Busy");
        phone.sync.Pump(true, 1500);
        Check(phone.Take() == Sent{"zoom 20"}, "retries");
        phone.Ack(2, 2, 1600);
        phone.sync.Pump(true, 9000);
        Check(phone.Take().empty(), "does not retry a refusal");
    }
    {
        // Dragging a slider: one command outstanding, then only the newest value.
        FakePhone phone;
        phone.Capabilities(0);
        phone.LensChanged("0", 0);
        for (int i = 0; i < 20; ++i) {
            phone.sync.ChooseZoom(1.0f + 0.1f * static_cast<float>(i), 1000 + i);
            phone.sync.Pump(true, 1000 + i);
        }
        Check(phone.Take() == Sent{"zoom 10"}, "one at a time");
        phone.Ack(1, 0, 1100);
        phone.sync.Pump(true, 1100);
        Check(phone.Take() == Sent{"zoom 29"}, "then the latest");
        phone.sync.ChooseZoom(5.0f, 1200);
        phone.sync.Pump(true, 1200);
        Check(phone.Take().empty(), "still outstanding");
        phone.sync.Pump(true, 3500);
        Check(phone.Take() == Sent{"zoom 50"}, "a lost Ack does not block the slider forever");
    }
    {
        // Connected after the phone was already streaming: LensChanged was missed.
        FakePhone phone;
        phone.sync.Load({std::string("1"), std::nullopt, std::nullopt, std::nullopt});
        phone.Capabilities(0);
        phone.sync.Pump(true, 1000);
        Check(phone.Take() == Sent{"zoom 10"}, "harmless command to make the phone report its lens");
        phone.Settings("0", 1100);
        phone.sync.Pump(true, 1200);
        Check(phone.Take() == Sent{"lens 1"}, "then restores it");
    }
    {
        // A lens that never sticks must not flap forever, and an unknown one is never sent.
        FakePhone phone;
        phone.sync.Load({std::string("1"), std::nullopt, std::nullopt, std::nullopt});
        phone.Capabilities(0);
        int64_t now = 0;
        size_t lensCommands = 0;
        for (int i = 0; i < 10; ++i) {
            phone.LensChanged("0", now);
            phone.sync.Pump(true, now + 1000);
            lensCommands += phone.Take().size();
            now += 10000;
        }
        Check(lensCommands == 2, "gives up after two attempts");
        phone.sync.ChooseLens("77", now);
        phone.sync.Pump(true, now + 1000);
        Check(phone.Take().empty(), "unknown lens is not sent");
    }
    {
        // The phone restarts its camera (Stop/Start): zoom and exposure are put back.
        FakePhone phone;
        phone.Capabilities(0);
        phone.LensChanged("0", 0);
        phone.sync.ChooseZoom(3.0f, 1000);
        phone.sync.ChooseEv(4, 1000);
        phone.sync.Pump(true, 1000);
        phone.Take();
        phone.Ack(1, 0, 1001);
        phone.Ack(2, 0, 1001);
        phone.LensChanged("0", 5000);
        phone.sync.Pump(true, 6000);
        Check((phone.Take() == Sent{"zoom 30", "ev 4"}), "replayed after a camera restart");
    }
}

}  // namespace

int main() {
    try {
        TestNoShapeHasBlackBars();
        TestOfferedSizes();
        TestCameraIsRecreatedOnlyWhenFree();
        TestCropMatchesWhatTheViewerSees();
        TestEditingTheCrop();
        TestNv12();
        TestSettings();
        TestCameraReplay();
        std::cout << "Picture, settings and camera replay tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
