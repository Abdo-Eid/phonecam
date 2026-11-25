#pragma once

// What PhoneCam remembers between runs: the current picture/camera settings, named presets, and
// a few window/connection choices. Stored per user under HKCU\Software\PhoneCam -- the registry
// rather than a file because each value is written atomically by Windows, there is no format to
// parse, and it needs no library. (The retired Wi-Fi pairing code kept its settings under the
// same key; it was removed with Wi-Fi itself, not because the storage was a problem.)

#include <cstdint>
#include <string>
#include <vector>

#include "camera/CameraSync.h"
#include "picture/Framing.h"

namespace phonecam::settings {

struct Profile {
    phonecam::picture::Picture picture;
    phonecam::camera::CameraChoice camera;

    bool operator==(const Profile&) const = default;
};

struct Preset {
    std::wstring name;
    Profile profile;
    bool builtIn = false;
};

// Ready-made starting points. Picture only: they leave whatever lens, zoom and exposure the user
// already has.
const std::vector<Preset>& BuiltInPresets();

// Why a name can't be used, or empty if it can. Names become registry keys and menu items.
std::wstring PresetNameProblem(const std::wstring& name);
std::wstring TrimName(const std::wstring& name);
bool SameName(const std::wstring& a, const std::wstring& b);

class Store {
public:
    // Tests pass their own root so they never touch the user's settings.
    explicit Store(std::wstring root = L"Software\\PhoneCam") : root_(std::move(root)) {}

    Profile LoadCurrent() const;
    bool SaveCurrent(const Profile& profile) const;

    // Built-in presets first, then the user's own, alphabetically.
    std::vector<Preset> Presets() const;
    bool SavePreset(const std::wstring& name, const Profile& profile) const;
    bool DeletePreset(const std::wstring& name) const;

    uint32_t LoadNumber(const wchar_t* name, uint32_t fallback) const;
    bool SaveNumber(const wchar_t* name, uint32_t value) const;

private:
    Profile ReadProfile(const std::wstring& key) const;
    bool WriteProfile(const std::wstring& key, const Profile& profile) const;

    std::wstring root_;
};

}  // namespace phonecam::settings
