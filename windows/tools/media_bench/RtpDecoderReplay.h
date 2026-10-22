#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

// Synthetic loopback RTP through the real depacketizer and MF decoder. No phone,
// live camera, virtual camera registration or external network is used.
void VerifyRtpRecovery(const std::vector<std::vector<uint8_t>>& units,
                       const std::map<uint64_t, uint64_t>& reference,
                       const std::filesystem::path& output);
