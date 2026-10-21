#include "connection/PairingStore.h"
#include <windows.h>
#include <wincred.h>
#include <cwctype>
#include <bcrypt.h>
#include <vector>

namespace phonecam::connection {
namespace {
std::wstring Target(const std::string& address, uint16_t port) {
    return L"PhoneCam/WiFi/" + std::wstring(address.begin(), address.end()) + L":" + std::to_wstring(port);
}
}

std::string NormalizePairingCode(const std::wstring& text) {
    std::string code;
    for (wchar_t c : text) {
        if (c == L'-' || iswspace(c)) continue;
        if (c > 127) return {};
        code.push_back(static_cast<char>(towupper(c)));
    }
    return code;
}

bool ValidPairingCode(const std::string& code) {
    return code.size() == 12 && code.find_first_not_of("ABCDEFGHJKLMNPQRSTUVWXYZ23456789") == std::string::npos;
}

std::string LoadPairingCode(const std::string& address, uint16_t port) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(Target(address, port).c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
    std::string code;
    if (credential->CredentialBlob && credential->CredentialBlobSize <= 128) {
        code.assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    }
    CredFree(credential);
    return ValidPairingCode(code) ? code : std::string();
}

bool SavePairingCode(const std::string& address, uint16_t port, const std::string& code) {
    if (!ValidPairingCode(code)) return false;
    auto target = Target(address, port);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(code.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(code.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

bool ForgetPairingCode(const std::string& address, uint16_t port) {
    if (CredDeleteW(Target(address, port).c_str(), CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

namespace {
constexpr wchar_t kSettings[] = L"Software\\PhoneCam\\Connection";
std::string Setting(const wchar_t* name) {
    wchar_t value[256] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettings, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    std::string text;
    for (const wchar_t* p = value; *p; ++p) { if (*p > 127) return {}; text.push_back(static_cast<char>(*p)); }
    return text;
}
bool SetSetting(const wchar_t* name, const std::string& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettings, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const std::wstring wide(value.begin(), value.end());
    const bool ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(wide.c_str()),
                                  static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}
bool IsHex(const std::string& text, size_t size) {
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
}

std::string RandomHex(size_t count) {
    std::vector<uint8_t> bytes(count);
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    const char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

std::string PcIdentity() {
    auto id = Setting(L"PcId");
    if (IsHex(id, 32)) return id;
    id = RandomHex(16);
    return !id.empty() && SetSetting(L"PcId", id) ? id : "";
}
std::string PcName() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD length = sizeof(name);
    return GetComputerNameA(name, &length) ? name : "PhoneCam PC";
}

bool SavePairedPhone(const PairedPhone& phone) {
    if (!IsHex(phone.phoneId, 32) || !IsHex(phone.key, 64)) return false;
    const std::wstring target = L"PhoneCam/QR/" + std::wstring(phone.phoneId.begin(), phone.phoneId.end());
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(phone.key.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(phone.key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) && SetSetting(L"PreferredPhone", phone.phoneId) && SetSetting(L"UseWifi", "1");
}
PairedPhone LoadPairedPhone() {
    PairedPhone phone;
    phone.pcId = PcIdentity();
    phone.phoneId = Setting(L"PreferredPhone");
    if (!IsHex(phone.phoneId, 32)) return {};
    PCREDENTIALW credential = nullptr;
    const std::wstring target = L"PhoneCam/QR/" + std::wstring(phone.phoneId.begin(), phone.phoneId.end());
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
    if (credential->CredentialBlobSize == 64)
        phone.key.assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    return IsHex(phone.key, 64) ? phone : PairedPhone{};
}
void UseRememberedWifi(bool enabled) { SetSetting(L"UseWifi", enabled ? "1" : "0"); }
bool RememberedWifiEnabled() { return Setting(L"UseWifi") == "1"; }
bool ForgetPairedPhone() {
    const auto phone = LoadPairedPhone();
    bool ok = true;
    if (!phone.phoneId.empty()) {
        const std::wstring target = L"PhoneCam/QR/" + std::wstring(phone.phoneId.begin(), phone.phoneId.end());
        ok = CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
    }
    UseRememberedWifi(false);
    SetSetting(L"PreferredPhone", "");
    return ok;
}
}
