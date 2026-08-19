#include "control/ControlChannel.h"

#include <atomic>
#include <mutex>

#include "control/ControlTransport.h"
#include "control_generated.h"
#include "log/Log.h"

namespace phonecam::control {

namespace fb = flatbuffers;
namespace pcw = phonecam::wire;
namespace pcc = phonecam::control;

namespace {

CapabilityInfo DecodeCapabilities(const pcw::CapabilityDescriptor* d) {
    CapabilityInfo out;
    if (!d) return out;
    out.protocolVersion = d->protocol_version();
    if (d->device_model()) out.deviceModel = d->device_model()->str();
    if (d->android_release()) out.androidRelease = d->android_release()->str();
    if (d->lenses()) {
        for (const auto* lens : *d->lenses()) {
            LensInfo info;
            if (lens->camera_id()) info.cameraId = lens->camera_id()->str();
            info.facing = static_cast<int>(lens->facing());
            info.hardwareLevel = static_cast<int>(lens->hardware_level());
            info.hasManualSensor = lens->has_manual_sensor();
            if (lens->resolutions()) info.resolutionCount = static_cast<int>(lens->resolutions()->size());
            if (const auto* zoom = lens->zoom_ratio_range()) {
                info.zoomMin = zoom->min();
                info.zoomMax = zoom->max();
            }
            if (const auto* iso = lens->iso_range()) {
                info.isoMin = iso->min();
                info.isoMax = iso->max();
            }
            if (const auto* exp = lens->exposure_time_ns_range()) {
                info.exposureTimeNsMin = exp->min();
                info.exposureTimeNsMax = exp->max();
            }
            if (const auto* ev = lens->ev_compensation_range()) {
                info.evMin = ev->min();
                info.evMax = ev->max();
            }
            info.evStep = lens->ev_step();
            if (lens->af_modes()) {
                for (auto mode : *lens->af_modes()) info.afModes.push_back(static_cast<int>(mode));
            }
            if (lens->awb_modes()) {
                for (auto mode : *lens->awb_modes()) info.awbModes.push_back(static_cast<int>(mode));
            }
            info.hasTorch = lens->has_torch();
            info.hasOpticalStabilization = lens->has_optical_stabilization();
            out.lenses.push_back(std::move(info));
        }
    }
    return out;
}

ControlMessage DecodeEnvelope(const uint8_t* data, size_t size) {
    ControlMessage msg;
    fb::Verifier verifier(data, size);
    if (!VerifyControlEnvelopeBuffer(verifier)) {
        phonecam::log::Error("ControlChannel: envelope failed verification, dropping");
        return msg;
    }
    const auto* envelope = GetControlEnvelope(data);
    switch (envelope->payload_type()) {
        case pcc::ControlPayload::phonecam_wire_CapabilityDescriptor:
            msg.kind = ControlMsgKind::Capabilities;
            msg.capabilities = DecodeCapabilities(envelope->payload_as_phonecam_wire_CapabilityDescriptor());
            break;
        case pcc::ControlPayload::Ack: {
            const auto* ack = envelope->payload_as_Ack();
            msg.kind = ControlMsgKind::Ack;
            msg.ack.cmdId = envelope->cmd_id();
            msg.ack.result = static_cast<int>(ack->result());
            if (ack->message()) msg.ack.message = ack->message()->str();
            break;
        }
        case pcc::ControlPayload::CurrentSettings: {
            const auto* s = envelope->payload_as_CurrentSettings();
            msg.kind = ControlMsgKind::Settings;
            if (s->camera_id()) msg.settings.cameraId = s->camera_id()->str();
            msg.settings.width = s->width();
            msg.settings.height = s->height();
            msg.settings.fps = s->fps();
            msg.settings.bitrateBps = s->bitrate_bps();
            msg.settings.zoomRatio = s->zoom_ratio();
            msg.settings.focusMode = static_cast<int>(s->focus_mode());
            msg.settings.evSteps = s->ev_steps();
            msg.settings.manualExposure = s->manual_exposure();
            msg.settings.iso = s->iso();
            msg.settings.exposureTimeNs = s->exposure_time_ns();
            msg.settings.awbMode = static_cast<int>(s->awb_mode());
            msg.settings.whiteBalanceTemperatureK = s->white_balance_temperature_k();
            msg.settings.torchOn = s->torch_on();
            msg.settings.stabilizationOn = s->stabilization_on();
            break;
        }
        case pcc::ControlPayload::Stats: {
            const auto* s = envelope->payload_as_Stats();
            msg.kind = ControlMsgKind::Stats;
            msg.stats.encodeFps = s->encode_fps();
            msg.stats.bitrateKbps = s->bitrate_kbps();
            msg.stats.droppedFrames = s->dropped_frames();
            msg.stats.thermalStatus = static_cast<int>(s->thermal_status());
            break;
        }
        case pcc::ControlPayload::AfStateChanged:
            msg.kind = ControlMsgKind::AfState;
            msg.afState = static_cast<int>(envelope->payload_as_AfStateChanged()->state());
            break;
        case pcc::ControlPayload::AeStateChanged:
            msg.kind = ControlMsgKind::AeState;
            msg.aeState = static_cast<int>(envelope->payload_as_AeStateChanged()->state());
            break;
        case pcc::ControlPayload::LensChanged: {
            const auto* lc = envelope->payload_as_LensChanged();
            msg.kind = ControlMsgKind::LensChanged;
            if (lc->camera_id()) msg.lensChanged.cameraId = lc->camera_id()->str();
            break;
        }
        case pcc::ControlPayload::OrientationChanged:
            msg.kind = ControlMsgKind::Orientation;
            msg.orientation.degrees = envelope->payload_as_OrientationChanged()->degrees();
            break;
        case pcc::ControlPayload::Error: {
            const auto* e = envelope->payload_as_Error();
            msg.kind = ControlMsgKind::Error;
            msg.error.cmdId = envelope->cmd_id();
            msg.error.code = e->code();
            if (e->message()) msg.error.message = e->message()->str();
            break;
        }
        default:
            msg.kind = ControlMsgKind::Unknown;
            break;
    }
    return msg;
}

}  // namespace

struct ControlChannel::Impl {
    ControlTransport transport;
    std::atomic<uint32_t> nextCmdId{1};

    mutable std::mutex cacheMutex;
    CapabilityInfo lastCapabilities;
    bool hasCapabilities = false;
    SettingsInfo lastSettings;
    bool hasSettings = false;

    uint32_t NextCmdId() { return nextCmdId.fetch_add(1); }

    template <typename BuildPayload>
    uint32_t SendCommand(pcc::ControlPayload type, BuildPayload buildPayload) {
        const uint32_t cmdId = NextCmdId();
        fb::FlatBufferBuilder fbb;
        const fb::Offset<void> payload = buildPayload(fbb);
        const auto envelope = pcc::CreateControlEnvelope(fbb, cmdId, type, payload);
        pcc::FinishControlEnvelopeBuffer(fbb, envelope);
        if (!transport.Send(fbb.GetBufferPointer(), fbb.GetSize())) {
            phonecam::log::Error("ControlChannel: send failed");
        }
        return cmdId;
    }
};

ControlChannel::ControlChannel() : impl_(std::make_unique<Impl>()) {}
ControlChannel::~ControlChannel() = default;

bool ControlChannel::Connect(uint16_t port, const std::string& host) {
    return impl_->transport.Connect(port, host);
}
void ControlChannel::Disconnect() { impl_->transport.Disconnect(); }

void ControlChannel::RunReceiveLoop(const ControlMessageHandler& onMessage) {
    impl_->transport.RunReceiveLoop([&](const uint8_t* data, size_t size) {
        if (size == 0) return;
        const ControlMessage msg = DecodeEnvelope(data, size);
        if (msg.kind == ControlMsgKind::Capabilities) {
            std::lock_guard<std::mutex> lock(impl_->cacheMutex);
            impl_->lastCapabilities = msg.capabilities;
            impl_->hasCapabilities = true;
        } else if (msg.kind == ControlMsgKind::Settings) {
            std::lock_guard<std::mutex> lock(impl_->cacheMutex);
            impl_->lastSettings = msg.settings;
            impl_->hasSettings = true;
        }
        onMessage(msg);
    });
}

uint32_t ControlChannel::SendSetZoomRatio(float ratio) {
    return impl_->SendCommand(pcc::ControlPayload::SetZoomRatio, [&](fb::FlatBufferBuilder& fbb) {
        return pcc::CreateSetZoomRatio(fbb, ratio).Union();
    });
}

uint32_t ControlChannel::SendSetEv(int32_t steps) {
    return impl_->SendCommand(pcc::ControlPayload::SetEv,
                               [&](fb::FlatBufferBuilder& fbb) { return pcc::CreateSetEv(fbb, steps).Union(); });
}

uint32_t ControlChannel::SendSetTorch(bool on) {
    return impl_->SendCommand(pcc::ControlPayload::SetTorch,
                               [&](fb::FlatBufferBuilder& fbb) { return pcc::CreateSetTorch(fbb, on).Union(); });
}

uint32_t ControlChannel::SendSetFocusMode(int afMode) {
    return impl_->SendCommand(pcc::ControlPayload::SetFocusMode, [&](fb::FlatBufferBuilder& fbb) {
        return pcc::CreateSetFocusMode(fbb, static_cast<pcw::AfMode>(afMode)).Union();
    });
}

uint32_t ControlChannel::SendTapToFocus(float nx, float ny, float regionSize) {
    return impl_->SendCommand(pcc::ControlPayload::TapToFocus, [&](fb::FlatBufferBuilder& fbb) {
        return pcc::CreateTapToFocus(fbb, nx, ny, regionSize).Union();
    });
}

uint32_t ControlChannel::SendRequestKeyframe() {
    return impl_->SendCommand(pcc::ControlPayload::RequestKeyframe,
                               [&](fb::FlatBufferBuilder& fbb) { return pcc::CreateRequestKeyframe(fbb).Union(); });
}

uint32_t ControlChannel::SendSetBitrate(uint32_t bitrateBps) {
    return impl_->SendCommand(pcc::ControlPayload::SetBitrate, [&](fb::FlatBufferBuilder& fbb) {
        return pcc::CreateSetBitrate(fbb, bitrateBps).Union();
    });
}

uint32_t ControlChannel::SendSetLens(const std::string& cameraId) {
    return impl_->SendCommand(pcc::ControlPayload::SetLens, [&](fb::FlatBufferBuilder& fbb) {
        return pcc::CreateSetLensDirect(fbb, cameraId.c_str()).Union();
    });
}

CapabilityInfo ControlChannel::GetLastCapabilities() const {
    std::lock_guard<std::mutex> lock(impl_->cacheMutex);
    return impl_->lastCapabilities;
}

bool ControlChannel::HasCapabilities() const {
    std::lock_guard<std::mutex> lock(impl_->cacheMutex);
    return impl_->hasCapabilities;
}

SettingsInfo ControlChannel::GetLastSettings() const {
    std::lock_guard<std::mutex> lock(impl_->cacheMutex);
    return impl_->lastSettings;
}

bool ControlChannel::HasSettings() const {
    std::lock_guard<std::mutex> lock(impl_->cacheMutex);
    return impl_->hasSettings;
}

}  // namespace phonecam::control
