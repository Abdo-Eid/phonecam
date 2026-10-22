#include <iostream>
#include "bench/MediaSettings.h"

int main() {
    using phonecam::bench::ProfileForName;
    const auto baseline = ProfileForName("default");
    const auto lower = ProfileForName("720p6m");
    const auto high = ProfileForName("1080p12m");
    if (!baseline || baseline->width != 1920 || baseline->height != 1080 || baseline->bitrate != 6000000 ||
        !lower || lower->width != 1280 || lower->height != 720 || lower->bitrate != baseline->bitrate ||
        !high || high->width != baseline->width || high->height != baseline->height || high->bitrate != 12000000 ||
        !ProfileForName("1080p4m") || !ProfileForName("1080p8m") || ProfileForName("invalid")) {
        std::cerr << "Media profile validation failed\n";
        return 1;
    }
    SetEnvironmentVariableA("PHONECAM_BENCH", "1");
    SetEnvironmentVariableA("PHONECAM_BENCH_MEDIA", "720p6m");
    const auto requested = phonecam::bench::RequestedMediaSettings();
#ifdef _DEBUG
    if (!requested || requested->width != 1280 || requested->height != 720) return 2;
    SetEnvironmentVariableA("PHONECAM_BENCH_MEDIA", "invalid");
    if (phonecam::bench::RequestedMediaSettings()) return 3;
    SetEnvironmentVariableA("PHONECAM_BENCH_MEDIA", "this-profile-name-is-longer-than-the-buffer");
    if (phonecam::bench::RequestedMediaSettings()) return 4;
#else
    if (!requested || requested->width != 1920 || requested->bitrate != 6000000) return 2;
    SetEnvironmentVariableA("PHONECAM_BENCH_MEDIA", "invalid");
    if (!phonecam::bench::RequestedMediaSettings()) return 3;
#endif
    SetEnvironmentVariableA("PHONECAM_BENCH", nullptr);
    const auto disabled = phonecam::bench::RequestedMediaSettings();
    if (!disabled || disabled->width != 1920 || disabled->bitrate != 6000000) return 5;
    std::cout << "Media profiles: PASS\n";
}
