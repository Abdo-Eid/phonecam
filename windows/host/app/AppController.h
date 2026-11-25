#pragma once

// The one place that knows what the user wants. The window and the tray both act through this,
// so neither owns state the other can't see, and everything chosen is saved and comes back on
// the next start or the next phone connection.
//
// Threads: OnMessage runs on the control-receive thread; everything else on the UI thread.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "camera/CameraSync.h"
#include "connection/ConnectionManager.h"
#include "picture/Framing.h"
#include "settings/Settings.h"

namespace phonecam::app {

class AppController {
public:
    explicit AppController(phonecam::connection::ConnectionManager& connection,
                           phonecam::settings::Store store = phonecam::settings::Store());

    // Brings back the saved picture and camera settings. Call before the connection starts.
    void Load();
    // Saves anything not written yet. Tick() does this continuously; call once more on exit.
    void Flush();
    // A few times a second on the UI thread: paces camera commands and batches saving, so
    // dragging a slider doesn't write the registry on every step.
    void Tick();
    void OnMessage(const phonecam::control::ControlMessage& message);

    phonecam::picture::Picture GetPicture() const;
    void SetPicture(const phonecam::picture::Picture& picture);

    // What the phone should be set to: the user's choice where there is one, else what the phone
    // reports. lens is empty until the phone has said which one is active, and already the new
    // one while a change is underway.
    struct CameraState {
        std::string lens;
        float zoom = 1.0f;
        int32_t ev = 0;
        int focus = 0;
        bool torch = false;
    };
    CameraState GetCamera() const;
    void ChooseLens(const std::string& cameraId);
    void ChooseZoom(float ratio);
    void ChooseEv(int32_t steps);
    void ChooseFocus(int mode);
    void ResetCamera();        // zoom, exposure and focus back to the phone's defaults
    void SetTorch(bool on);    // never saved

    std::vector<phonecam::settings::Preset> Presets() const { return store_.Presets(); }
    // The preset that describes the current settings exactly, or empty.
    std::wstring ActivePreset(const std::vector<phonecam::settings::Preset>& presets) const;
    bool ApplyPreset(const std::wstring& name);
    bool SavePreset(const std::wstring& name);
    bool DeletePreset(const std::wstring& name);

    // The connection the user picked, as opposed to the one Automatic settled on.
    phonecam::connection::Mode ChosenMode() const { return chosenMode_; }
    void ChooseMode(phonecam::connection::Mode mode);
    phonecam::connection::Mode SavedMode() const;
    // What this run starts with (a command-line override is not saved).
    void SetInitialMode(phonecam::connection::Mode mode) { chosenMode_ = mode; }

    const phonecam::settings::Store& Store() const { return store_; }

private:
    phonecam::settings::Profile CurrentProfile() const;
    void Pump();

    phonecam::connection::ConnectionManager& connection_;
    phonecam::settings::Store store_;
    mutable std::mutex mutex_;
    phonecam::camera::CameraSync camera_;  // guarded by mutex_
    phonecam::settings::Profile saved_;    // what the registry holds; UI thread only
    phonecam::connection::Mode chosenMode_ = phonecam::connection::Mode::Automatic;
};

}  // namespace phonecam::app
