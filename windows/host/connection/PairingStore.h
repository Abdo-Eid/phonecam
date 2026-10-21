#pragma once
#include <cstdint>
#include <string>

namespace phonecam::connection {
std::string NormalizePairingCode(const std::wstring& text);
bool ValidPairingCode(const std::string& code);
// Generic credentials are protected by Windows for the current user, never stored in argv/JSON.
std::string LoadPairingCode(const std::string& address, uint16_t port);
bool SavePairingCode(const std::string& address, uint16_t port, const std::string& code);
bool ForgetPairingCode(const std::string& address, uint16_t port);
struct PairedPhone { std::string pcId, phoneId, key, name; };
std::string PcIdentity();
std::string PcName();
std::string RandomHex(size_t bytes);
bool SavePairedPhone(const PairedPhone& phone);
PairedPhone LoadPairedPhone();
void UseRememberedWifi(bool enabled);
bool RememberedWifiEnabled();
bool ForgetPairedPhone();
}
