#include "settings/Settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <optional>

#include "log/Log.h"
#include "text/Utf8.h"

#pragma comment(lib, "advapi32.lib")

namespace phonecam::settings {

namespace {

using phonecam::picture::Picture;
using phonecam::picture::Shape;
using phonecam::shm::Rotation;

constexpr size_t kMaxNameLength = 40;

// Fractions are stored as whole numbers (REG_DWORD): readable in regedit, no locale issues.
constexpr float kCropScale = 1000.0f;
constexpr float kZoomScale = 100.0f;

std::optional<DWORD> ReadNumber(const std::wstring& key, const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::optional<std::wstring> ReadText(const std::wstring& key, const wchar_t* name) {
    wchar_t value[128]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return std::wstring(value);
}

bool WriteNumber(const std::wstring& key, const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

bool WriteText(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// Unset must also clear what an earlier save left behind.
bool WriteOptional(const std::wstring& key, const wchar_t* name, const std::optional<DWORD>& value) {
    if (value) return WriteNumber(key, name, *value);
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

DWORD Scaled(float value, float scale) { return static_cast<DWORD>(std::lround(value * scale)); }

Preset BuiltIn(const wchar_t* name, Shape shape) {
    Preset preset;
    preset.name = name;
    preset.builtIn = true;
    preset.profile.picture.shape = shape;
    return preset;
}

}  // namespace

const std::vector<Preset>& BuiltInPresets() {
    static const std::vector<Preset> presets = {
        BuiltIn(L"Whole picture", Shape::Follow),
        BuiltIn(L"Square", Shape::Square),
        BuiltIn(L"Video call", Shape::Wide),
        BuiltIn(L"Vertical video", Shape::Tall),
    };
    return presets;
}

std::wstring TrimName(const std::wstring& name) {
    const auto first = name.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    return name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    // Registry keys compare this way, so presets must too.
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring PresetNameProblem(const std::wstring& name) {
    if (TrimName(name).empty()) return L"Type a name for the preset first.";
    if (TrimName(name).size() > kMaxNameLength) return L"That name is too long.";
    if (name.find(L'\\') != std::wstring::npos) return L"A preset name can't contain a backslash.";
    for (const auto& preset : BuiltInPresets()) {
        if (SameName(preset.name, TrimName(name))) return L"That is a built-in preset. Choose another name.";
    }
    return {};
}

Profile Store::ReadProfile(const std::wstring& key) const {
    Profile profile;
    // Values are range-checked because a hand-edited or future registry must not become an
    // out-of-range enum further down the pipeline.
    if (const auto v = ReadNumber(key, L"Turn"); v && *v <= static_cast<DWORD>(Rotation::Deg270))
        profile.picture.turn = static_cast<Rotation>(*v);
    if (const auto v = ReadNumber(key, L"Mirror")) profile.picture.mirror = *v != 0;
    if (const auto v = ReadNumber(key, L"Shape"); v && *v <= static_cast<DWORD>(phonecam::picture::kLastShape))
        profile.picture.shape = static_cast<Shape>(*v);
    if (const auto v = ReadNumber(key, L"Compact")) profile.picture.compact = *v != 0;
    const auto fraction = [&](const wchar_t* name, float low, float& value) {
        const auto v = ReadNumber(key, name);
        if (!v || *v > static_cast<DWORD>(kCropScale)) return;
        value = (std::max)(low, static_cast<float>(*v) / kCropScale);
    };
    fraction(L"CropSize", phonecam::picture::kMinCropSize, profile.picture.cropSize);
    fraction(L"CropX", 0.0f, profile.picture.cropX);
    fraction(L"CropY", 0.0f, profile.picture.cropY);

    if (const auto v = ReadText(key, L"Lens"); v && !v->empty()) profile.camera.lens = phonecam::text::Narrow(*v);
    if (const auto v = ReadNumber(key, L"Zoom"); v && *v > 0 && *v <= 10000)
        profile.camera.zoom = static_cast<float>(*v) / kZoomScale;
    if (const auto v = ReadNumber(key, L"Exposure")) {
        const auto steps = static_cast<int32_t>(*v);
        if (steps >= -100 && steps <= 100) profile.camera.ev = steps;
    }
    if (const auto v = ReadNumber(key, L"Focus"); v && *v <= 4) profile.camera.focus = static_cast<int>(*v);
    return profile;
}

bool Store::WriteProfile(const std::wstring& key, const Profile& profile) const {
    const auto& picture = profile.picture;
    const auto& camera = profile.camera;
    const auto fraction = [](float value) { return Scaled(std::clamp(value, 0.0f, 1.0f), kCropScale); };
    bool ok = WriteNumber(key, L"Turn", static_cast<DWORD>(picture.turn));
    ok &= WriteNumber(key, L"Mirror", picture.mirror ? 1 : 0);
    ok &= WriteNumber(key, L"Shape", static_cast<DWORD>(picture.shape));
    ok &= WriteNumber(key, L"Compact", picture.compact ? 1 : 0);
    ok &= WriteNumber(key, L"CropSize", fraction(picture.cropSize));
    ok &= WriteNumber(key, L"CropX", fraction(picture.cropX));
    ok &= WriteNumber(key, L"CropY", fraction(picture.cropY));
    ok &= WriteText(key, L"Lens", camera.lens ? phonecam::text::Widen(*camera.lens) : std::wstring());
    ok &= WriteOptional(key, L"Zoom", camera.zoom ? std::optional<DWORD>(Scaled(*camera.zoom, kZoomScale)) : std::nullopt);
    ok &= WriteOptional(key, L"Exposure",
                        camera.ev ? std::optional<DWORD>(static_cast<DWORD>(*camera.ev)) : std::nullopt);
    ok &= WriteOptional(key, L"Focus",
                        camera.focus ? std::optional<DWORD>(static_cast<DWORD>(*camera.focus)) : std::nullopt);
    if (!ok) phonecam::log::Error("Settings: could not save to the registry");
    return ok;
}

Profile Store::LoadCurrent() const { return ReadProfile(root_ + L"\\Current"); }

bool Store::SaveCurrent(const Profile& profile) const { return WriteProfile(root_ + L"\\Current", profile); }

std::vector<Preset> Store::Presets() const {
    std::vector<Preset> presets = BuiltInPresets();
    const std::wstring key = root_ + L"\\Presets";
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return presets;
    const size_t firstOwn = presets.size();
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status = RegEnumKeyExW(handle, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || !PresetNameProblem(name).empty()) continue;
        presets.push_back({name, ReadProfile(key + L"\\" + name), false});
    }
    RegCloseKey(handle);
    std::sort(presets.begin() + static_cast<std::ptrdiff_t>(firstOwn), presets.end(),
              [](const Preset& a, const Preset& b) {
                  return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
              });
    return presets;
}

bool Store::SavePreset(const std::wstring& name, const Profile& profile) const {
    if (!PresetNameProblem(name).empty()) return false;
    return WriteProfile(root_ + L"\\Presets\\" + TrimName(name), profile);
}

bool Store::DeletePreset(const std::wstring& name) const {
    if (!PresetNameProblem(name).empty()) return false;  // built-ins included
    return RegDeleteTreeW(HKEY_CURRENT_USER, (root_ + L"\\Presets\\" + TrimName(name)).c_str()) == ERROR_SUCCESS;
}

uint32_t Store::LoadNumber(const wchar_t* name, uint32_t fallback) const {
    return ReadNumber(root_, name).value_or(fallback);
}

bool Store::SaveNumber(const wchar_t* name, uint32_t value) const { return WriteNumber(root_, name, value); }

}  // namespace phonecam::settings
