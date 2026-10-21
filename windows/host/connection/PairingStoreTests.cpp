#include "connection/PairingStore.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
    using namespace phonecam::connection;
    const std::string key = "unit-test-" + std::to_string(GetCurrentProcessId());
    try {
        Check(NormalizePairingCode(L"abcd-efgh-jklm") == "ABCDEFGHJKLM", "grouped/lowercase code");
        Check(NormalizePairingCode(L" ABCD EFGH JKLM\t") == "ABCDEFGHJKLM", "whitespace code");
        Check(ValidPairingCode("ABCDEFGHJKLM"), "valid generated code");
        Check(!ValidPairingCode("ABCDEFGHIJKL"), "ambiguous I must be rejected");
        Check(!ValidPairingCode("SHORT"), "short code must be rejected");
        Check(!ValidPairingCode(NormalizePairingCode(L"ABCD-EFGH-\u00e9KLM")), "non-ASCII code");
        Check(LoadPairingCode(key, 27184).empty(), "isolated test credential must not exist");
        Check(!SavePairingCode(key, 27184, "WRONG"), "invalid code must not be saved");
        Check(SavePairingCode(key, 27184, "ABCDEFGHJKLM"), "credential write");
        Check(LoadPairingCode(key, 27184) == "ABCDEFGHJKLM", "credential roundtrip");
        Check(LoadPairingCode(key, 27185).empty(), "credentials scoped by endpoint");
        Check(ForgetPairingCode(key, 27184), "credential delete");
        Check(LoadPairingCode(key, 27184).empty(), "deleted credential gone");
        Check(ForgetPairingCode(key, 27184), "delete missing credential idempotent");
        std::cout << "Pairing validation and Windows Credential Manager tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        ForgetPairingCode(key, 27184); // only the unique test credential, never a real phone entry
        std::cerr << error.what() << '\n';
        return 1;
    }
}
