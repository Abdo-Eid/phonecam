#include "log/Log.h"

// Phase 0 skeleton: proves the toolchain (CMake + MSVC) and the common
// library link correctly. USB transport, MF decode, and virtual-camera
// control land in Phases 1-3 — see docs/architecture.md.
int main() {
    phonecam::log::Info("phonecam-host skeleton build (Phase 0) starting");
    return 0;
}
