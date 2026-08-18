#pragma once

// Phase 4's "capability-driven UI": a keyboard-driven console command
// surface generated from the received CapabilityDescriptor, standing in for
// a real GUI. Satisfies the exit criterion ("every advertised control
// visibly changes the live image"; never offer an unsupported command)
// without importing Phase 5's tray-app/GUI-framework scope into Phase 4 --
// see docs/architecture.md.

#include <memory>

#include "control/ControlChannel.h"

namespace phonecam::control {

class ConsoleControlUi {
public:
    explicit ConsoleControlUi(ControlChannel& channel);
    ~ConsoleControlUi();

    ConsoleControlUi(const ConsoleControlUi&) = delete;
    ConsoleControlUi& operator=(const ConsoleControlUi&) = delete;

    // Call once per received message (from ControlChannel::RunReceiveLoop) to
    // keep the UI's capability cache current and print telemetry.
    void OnMessage(const ControlMessage& msg);

    // Runs the blocking stdin command loop on the calling thread until "quit"
    // or EOF. Prints a one-line help banner first.
    void RunCommandLoop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace phonecam::control
