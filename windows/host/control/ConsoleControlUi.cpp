#include "control/ConsoleControlUi.h"

#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace phonecam::control {

namespace {

const char* AfModeName(int m) {
    switch (m) {
        case 0: return "Off";
        case 1: return "Auto";
        case 2: return "Continuous";
        case 3: return "Macro";
        case 4: return "EdgeCapture";
        default: return "?";
    }
}

const char* AwbModeName(int m) {
    switch (m) {
        case 0: return "Off";
        case 1: return "Auto";
        case 2: return "Incandescent";
        case 3: return "Fluorescent";
        case 4: return "Daylight";
        case 5: return "CloudyDaylight";
        case 6: return "Twilight";
        case 7: return "Shade";
        default: return "?";
    }
}

const char* FacingName(int f) {
    switch (f) {
        case 0: return "Back";
        case 1: return "Front";
        case 2: return "External";
        default: return "?";
    }
}

const char* AckResultName(int r) {
    switch (r) {
        case 0: return "Ok";
        case 1: return "Unsupported";
        case 2: return "OutOfRange";
        case 3: return "Busy";
        case 4: return "Error";
        default: return "?";
    }
}

void PrintCapabilities(const CapabilityInfo& c) {
    std::printf("Capabilities: device=%s android=%s protocol_version=%u lenses=%zu\n", c.deviceModel.c_str(),
                c.androidRelease.c_str(), c.protocolVersion, c.lenses.size());
    for (size_t i = 0; i < c.lenses.size(); ++i) {
        const auto& l = c.lenses[i];
        std::printf(
            "  [%zu] camera_id=%s facing=%s hw_level=%d manual_sensor=%d resolutions=%d torch=%d ois=%d\n"
            "       zoom=[%.2f,%.2f] ev=[%d,%d]*%.3f\n"
            "       af_modes=",
            i, l.cameraId.c_str(), FacingName(l.facing), l.hardwareLevel, l.hasManualSensor, l.resolutionCount,
            l.hasTorch, l.hasOpticalStabilization, l.zoomMin, l.zoomMax, l.evMin, l.evMax, l.evStep);
        for (int m : l.afModes) std::printf("%s ", AfModeName(m));
        std::printf("\n       awb_modes=");
        for (int m : l.awbModes) std::printf("%s ", AwbModeName(m));
        std::printf("\n");
    }
}

}  // namespace

struct ConsoleControlUi::Impl {
    ControlChannel& channel;
    std::mutex mutex;
    CapabilityInfo lastCapabilities;
    bool hasCapabilities = false;

    explicit Impl(ControlChannel& ch) : channel(ch) {}
};

ConsoleControlUi::ConsoleControlUi(ControlChannel& channel) : impl_(std::make_unique<Impl>(channel)) {}
ConsoleControlUi::~ConsoleControlUi() = default;

void ConsoleControlUi::OnMessage(const ControlMessage& msg) {
    // Holds impl_->mutex for the whole call: this runs on ControlChannel's receive thread while
    // RunCommandLoop's command echoes run on the console-input thread, and unsynchronized
    // std::printf calls from two threads can interleave mid-line -- confirmed empirically (garbled
    // capability dumps) while testing lens-switch commands against the real device.
    std::lock_guard<std::mutex> lock(impl_->mutex);
    switch (msg.kind) {
        case ControlMsgKind::Capabilities: {
            impl_->lastCapabilities = msg.capabilities;
            impl_->hasCapabilities = true;
            PrintCapabilities(msg.capabilities);
            break;
        }
        case ControlMsgKind::Ack:
            std::printf("Ack cmd_id=%u result=%s%s%s\n", msg.ack.cmdId, AckResultName(msg.ack.result),
                        msg.ack.message.empty() ? "" : " message=", msg.ack.message.c_str());
            break;
        case ControlMsgKind::Settings:
            std::printf(
                "CurrentSettings camera_id=%s %ux%u@%u bitrate=%u zoom=%.2f focus=%s ev=%d torch=%d stab=%d\n",
                msg.settings.cameraId.c_str(), msg.settings.width, msg.settings.height, msg.settings.fps,
                msg.settings.bitrateBps, msg.settings.zoomRatio, AfModeName(msg.settings.focusMode),
                msg.settings.evSteps, msg.settings.torchOn, msg.settings.stabilizationOn);
            break;
        case ControlMsgKind::Stats:
            std::printf("Stats fps=%.1f bitrate_kbps=%u dropped=%u thermal=%d\n", msg.stats.encodeFps,
                        msg.stats.bitrateKbps, msg.stats.droppedFrames, msg.stats.thermalStatus);
            break;
        case ControlMsgKind::AfState:
            std::printf("AfStateChanged state=%d\n", msg.afState);
            break;
        case ControlMsgKind::AeState:
            std::printf("AeStateChanged state=%d\n", msg.aeState);
            break;
        case ControlMsgKind::LensChanged:
            std::printf("LensChanged camera_id=%s\n", msg.lensChanged.cameraId.c_str());
            break;
        case ControlMsgKind::Orientation:
            std::printf("OrientationChanged degrees=%u\n", msg.orientation.degrees);
            break;
        case ControlMsgKind::Error:
            std::printf("Error cmd_id=%u code=%u message=%s\n", msg.error.cmdId, msg.error.code,
                        msg.error.message.c_str());
            break;
        case ControlMsgKind::Unknown:
            std::printf("(unknown control message)\n");
            break;
    }
    std::fflush(stdout);
}

void ConsoleControlUi::RunCommandLoop() {
    std::printf(
        "Control console. Commands: caps | zoom <ratio> | ev <steps> | torch on|off | "
        "focus off|auto|continuous|macro|edge | tap <nx> <ny> <size> | keyframe | bitrate <bps> | "
        "lens <index> | quit\n");
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;

        if (cmd == "quit" || cmd == "exit") {
            break;
        }

        // Same rationale as OnMessage's lock: serializes this thread's command echoes against
        // the receive thread's telemetry prints so lines from the two threads don't interleave.
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (cmd == "caps") {
            if (impl_->hasCapabilities) {
                PrintCapabilities(impl_->lastCapabilities);
            } else {
                std::printf("No capabilities received yet.\n");
            }
        } else if (cmd == "zoom") {
            float ratio = 1.0f;
            if (iss >> ratio) {
                std::printf("-> SetZoomRatio(%.2f) cmd_id=%u\n", ratio, impl_->channel.SendSetZoomRatio(ratio));
            }
        } else if (cmd == "ev") {
            int32_t steps = 0;
            if (iss >> steps) {
                std::printf("-> SetEv(%d) cmd_id=%u\n", steps, impl_->channel.SendSetEv(steps));
            }
        } else if (cmd == "torch") {
            std::string onOff;
            if (iss >> onOff) {
                const bool on = onOff == "on";
                std::printf("-> SetTorch(%d) cmd_id=%u\n", on, impl_->channel.SendSetTorch(on));
            }
        } else if (cmd == "focus") {
            std::string mode;
            if (iss >> mode) {
                int af = -1;
                if (mode == "off") af = 0;
                else if (mode == "auto") af = 1;
                else if (mode == "continuous") af = 2;
                else if (mode == "macro") af = 3;
                else if (mode == "edge") af = 4;
                if (af >= 0) {
                    std::printf("-> SetFocusMode(%s) cmd_id=%u\n", AfModeName(af),
                                impl_->channel.SendSetFocusMode(af));
                } else {
                    std::printf("Unknown focus mode: %s\n", mode.c_str());
                }
            }
        } else if (cmd == "tap") {
            float nx = 0, ny = 0, size = 0.1f;
            if (iss >> nx >> ny >> size) {
                std::printf("-> TapToFocus(%.2f,%.2f,%.2f) cmd_id=%u\n", nx, ny, size,
                            impl_->channel.SendTapToFocus(nx, ny, size));
            }
        } else if (cmd == "keyframe") {
            std::printf("-> RequestKeyframe() cmd_id=%u\n", impl_->channel.SendRequestKeyframe());
        } else if (cmd == "bitrate") {
            uint32_t bps = 0;
            if (iss >> bps) {
                std::printf("-> SetBitrate(%u) cmd_id=%u\n", bps, impl_->channel.SendSetBitrate(bps));
            }
        } else if (cmd == "lens") {
            size_t index = 0;
            if (iss >> index) {
                std::string cameraId;
                if (!impl_->hasCapabilities || index >= impl_->lastCapabilities.lenses.size()) {
                    std::printf("No such lens index (run 'caps' first).\n");
                } else {
                    cameraId = impl_->lastCapabilities.lenses[index].cameraId;
                }
                if (!cameraId.empty()) {
                    std::printf("-> SetLens(%s) cmd_id=%u\n", cameraId.c_str(),
                                impl_->channel.SendSetLens(cameraId));
                }
            }
        } else {
            std::printf("Unknown command: %s\n", cmd.c_str());
        }
        std::fflush(stdout);
    }
}

}  // namespace phonecam::control
