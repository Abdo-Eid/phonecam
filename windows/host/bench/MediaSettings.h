#pragma once

#include <windows.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace phonecam::bench {
struct MediaSettings {
    uint16_t width = 1920, height = 1080, fps = 30;
    uint32_t bitrate = 6'000'000;
};

inline std::optional<MediaSettings> ProfileForName(std::string_view name) {
    if (name.empty() || name == "default") return MediaSettings{};
    if (name == "720p6m") return MediaSettings{1280, 720, 30, 6'000'000};
    if (name == "1080p4m") return MediaSettings{1920, 1080, 30, 4'000'000};
    if (name == "1080p8m") return MediaSettings{1920, 1080, 30, 8'000'000};
    if (name == "1080p12m") return MediaSettings{1920, 1080, 30, 12'000'000};
    return std::nullopt;
}

// Changes only media fields in the existing StartVideo command, never transport
// selection, pairing, discovery or reconnect behavior. Release ignores the hook.
inline std::optional<MediaSettings> RequestedMediaSettings() {
#ifdef _DEBUG
    char enabled[8]{}, name[32]{};
    if (GetEnvironmentVariableA("PHONECAM_BENCH", enabled, sizeof(enabled)) > 0 && enabled[0] == '1') {
        const DWORD length = GetEnvironmentVariableA("PHONECAM_BENCH_MEDIA", name, sizeof(name));
        if (length >= sizeof(name)) return std::nullopt;
        return ProfileForName(name);
    }
#endif
    return MediaSettings{};
}
}  // namespace phonecam::bench
