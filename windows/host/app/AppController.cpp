#include "app/AppController.h"

#include <chrono>
#include <cmath>

namespace phonecam::app {

namespace {

using phonecam::connection::Mode;

constexpr int kFocusContinuous = 2;  // phonecam.wire.AfMode, the phone's own default
constexpr wchar_t kConnectionValue[] = L"Connection";

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A preset only constrains what it sets; compared with the resolution they are stored at, so a
// slider value that went through the registry still counts as the same value.
bool Describes(const phonecam::settings::Profile& preset, const phonecam::settings::Profile& current) {
    const auto& a = preset.picture;
    const auto& b = current.picture;
    const auto same = [](float x, float y) { return std::fabs(x - y) <= 0.001f; };
    if (a.turn != b.turn || a.mirror != b.mirror || a.shape != b.shape || a.compact != b.compact) return false;
    if (!same(a.cropSize, b.cropSize) || !same(a.cropX, b.cropX) || !same(a.cropY, b.cropY)) return false;
    const auto& wanted = preset.camera;
    const auto& actual = current.camera;
    if (wanted.lens && wanted.lens != actual.lens) return false;
    if (wanted.zoom && (!actual.zoom || std::fabs(*wanted.zoom - *actual.zoom) > 0.01f)) return false;
    if (wanted.ev && wanted.ev != actual.ev) return false;
    if (wanted.focus && wanted.focus != actual.focus) return false;
    return true;
}

}  // namespace

AppController::AppController(phonecam::connection::ConnectionManager& connection, phonecam::settings::Store store)
    : connection_(connection),
      store_(std::move(store)),
      camera_(phonecam::camera::CameraCommands{
          [&connection](const std::string& id) { return connection.Control().SendSetLens(id); },
          [&connection](float ratio) { return connection.Control().SendSetZoomRatio(ratio); },
          [&connection](int32_t steps) { return connection.Control().SendSetEv(steps); },
          [&connection](int mode) { return connection.Control().SendSetFocusMode(mode); },
      }) {}

void AppController::Load() {
    saved_ = store_.LoadCurrent();
    connection_.Bridge().SetPicture(saved_.picture);
    std::lock_guard<std::mutex> lock(mutex_);
    camera_.Load(saved_.camera);
}

phonecam::settings::Profile AppController::CurrentProfile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {connection_.Bridge().GetPicture(), camera_.Desired()};
}

void AppController::Flush() {
    const auto profile = CurrentProfile();
    if (profile == saved_) return;
    if (store_.SaveCurrent(profile)) saved_ = profile;
}

void AppController::Pump() {
    const bool streaming = connection_.GetSnapshot().state == phonecam::connection::State::Streaming;
    std::lock_guard<std::mutex> lock(mutex_);
    camera_.Pump(streaming, NowMs());
}

void AppController::Tick() {
    connection_.Bridge().PollConsumer();
    Pump();
    Flush();
}

void AppController::OnMessage(const phonecam::control::ControlMessage& message) {
    // Phone apps from before this message exist: the picture then stays turned for an upright
    // phone's back lens, as it always was.
    if (message.kind == phonecam::control::ControlMsgKind::Orientation) {
        connection_.Bridge().SetPhoneRotation(
            static_cast<phonecam::shm::Rotation>((message.orientation.degrees / 90) % 4));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.OnMessage(message, NowMs());
    }
    // An Ack frees the slot for the newest slider value right away instead of on the next Tick.
    if (message.kind == phonecam::control::ControlMsgKind::Ack) Pump();
}

phonecam::picture::Picture AppController::GetPicture() const { return connection_.Bridge().GetPicture(); }

void AppController::SetPicture(const phonecam::picture::Picture& picture) { connection_.Bridge().SetPicture(picture); }

AppController::CameraState AppController::GetCamera() const {
    auto& control = connection_.Control();
    CameraState state;
    state.focus = kFocusContinuous;
    if (control.HasSettings()) {
        const auto reported = control.GetLastSettings();
        state.lens = reported.cameraId;
        state.zoom = reported.zoomRatio;
        state.ev = reported.evSteps;
        state.focus = reported.focusMode;
        state.torch = reported.torchOn;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!camera_.CurrentLens().empty()) state.lens = camera_.CurrentLens();
    if (const auto awaited = camera_.AwaitedLens(); !awaited.empty()) state.lens = awaited;
    const auto& desired = camera_.Desired();
    if (desired.zoom) state.zoom = *desired.zoom;
    if (desired.ev) state.ev = *desired.ev;
    if (desired.focus) state.focus = *desired.focus;
    return state;
}

void AppController::ChooseLens(const std::string& cameraId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.ChooseLens(cameraId, NowMs());
    }
    Pump();
}

void AppController::ChooseZoom(float ratio) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.ChooseZoom(ratio, NowMs());
    }
    Pump();
}

void AppController::ChooseEv(int32_t steps) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.ChooseEv(steps, NowMs());
    }
    Pump();
}

void AppController::ChooseFocus(int mode) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.ChooseFocus(mode, NowMs());
    }
    Pump();
}

void AppController::ResetCamera() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        camera_.Apply({std::nullopt, 1.0f, 0, kFocusContinuous}, NowMs());
    }
    Pump();
}

void AppController::SetTorch(bool on) { connection_.Control().SendSetTorch(on); }

std::wstring AppController::ActivePreset(const std::vector<phonecam::settings::Preset>& presets) const {
    const auto current = CurrentProfile();
    // The user's own presets say more (camera settings too), so they win over a built-in match.
    for (auto it = presets.rbegin(); it != presets.rend(); ++it) {
        if (Describes(it->profile, current)) return it->name;
    }
    return {};
}

bool AppController::ApplyPreset(const std::wstring& name) {
    for (const auto& preset : store_.Presets()) {
        if (!phonecam::settings::SameName(preset.name, name)) continue;
        SetPicture(preset.profile.picture);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            camera_.Apply(preset.profile.camera, NowMs());
        }
        Pump();
        return true;
    }
    return false;
}

bool AppController::SavePreset(const std::wstring& name) {
    // "As it looks now", including settings the user never touched -- but only if a phone is
    // there to report them.
    auto profile = CurrentProfile();
    if (connection_.Control().HasSettings()) {
        const auto camera = GetCamera();
        if (!camera.lens.empty()) profile.camera.lens = camera.lens;
        profile.camera.zoom = camera.zoom;
        profile.camera.ev = camera.ev;
        profile.camera.focus = camera.focus;
    }
    if (!store_.SavePreset(name, profile)) return false;
    // Adopt them as chosen, otherwise the new preset would not count as the active one.
    std::lock_guard<std::mutex> lock(mutex_);
    camera_.Load(profile.camera);
    return true;
}

bool AppController::DeletePreset(const std::wstring& name) { return store_.DeletePreset(name); }

Mode AppController::SavedMode() const {
    const auto value = store_.LoadNumber(kConnectionValue, static_cast<uint32_t>(Mode::Automatic));
    // Stored by Mode's numeric value: append to that enum, don't reorder it.
    // Disconnected is a thing to do now, not a way to start next time.
    if (value == static_cast<uint32_t>(Mode::Adb)) return Mode::Adb;
    if (value == static_cast<uint32_t>(Mode::Tethered)) return Mode::Tethered;
    return Mode::Automatic;
}

void AppController::ChooseMode(Mode mode) {
    chosenMode_ = mode;
    if (mode != Mode::Disconnected) store_.SaveNumber(kConnectionValue, static_cast<uint32_t>(mode));
    phonecam::connection::Target target;
    target.mode = mode;
    connection_.Connect(target);
}

}  // namespace phonecam::app
