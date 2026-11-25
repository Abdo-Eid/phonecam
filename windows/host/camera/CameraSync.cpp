#include "camera/CameraSync.h"

#include <algorithm>

namespace phonecam::camera {

namespace {
constexpr int kAckOk = 0;
constexpr int kAckBusy = 3;
constexpr int64_t kAckTimeoutMs = 2000;
constexpr int64_t kBusyRetryMs = 400;
constexpr int kMaxAttempts = 30;
// The phone reports a camera as open slightly before its capture session accepts requests.
constexpr int64_t kCameraSettleMs = 400;
constexpr int64_t kLensChangeTimeoutMs = 5000;
// A lens that keeps not sticking (camera in use elsewhere, refused by policy) must not be
// retried forever: every attempt interrupts the picture.
constexpr int kMaxLensAttempts = 2;
}  // namespace

const phonecam::control::LensInfo* CameraSync::FindLens(const std::string& cameraId) const {
    for (const auto& lens : capabilities_.lenses) {
        if (lens.cameraId == cameraId) return &lens;
    }
    return nullptr;
}

void CameraSync::Apply(const CameraChoice& choice, int64_t nowMs) {
    if (choice.lens) ChooseLens(*choice.lens, nowMs);
    if (choice.zoom) ChooseZoom(*choice.zoom, nowMs);
    if (choice.ev) ChooseEv(*choice.ev, nowMs);
    if (choice.focus) ChooseFocus(*choice.focus, nowMs);
}

void CameraSync::ChooseLens(const std::string& cameraId, int64_t nowMs) {
    desired_.lens = cameraId;
    lensAttempts_ = 0;
    lens_.Request(nowMs);
}

void CameraSync::ChooseZoom(float ratio, int64_t nowMs) {
    desired_.zoom = ratio;
    zoom_.Request(nowMs);
}

void CameraSync::ChooseEv(int32_t steps, int64_t nowMs) {
    desired_.ev = steps;
    ev_.Request(nowMs);
}

void CameraSync::ChooseFocus(int mode, int64_t nowMs) {
    desired_.focus = mode;
    focus_.Request(nowMs);
}

void CameraSync::ReplayControls(int64_t when) {
    if (desired_.zoom) zoom_.Request(when);
    if (desired_.ev) ev_.Request(when);
    if (desired_.focus) focus_.Request(when);
}

bool CameraSync::OnAck(Slot& slot, const phonecam::control::AckInfo& ack, int64_t nowMs) {
    if (slot.inFlight == 0 || slot.inFlight != ack.cmdId) return false;
    slot.inFlight = 0;
    if (ack.result == kAckBusy && !slot.pending && ++slot.attempts < kMaxAttempts) {
        slot.pending = true;
        slot.notBefore = nowMs + kBusyRetryMs;
    }
    return true;
}

void CameraSync::OnMessage(const phonecam::control::ControlMessage& message, int64_t nowMs) {
    using phonecam::control::ControlMsgKind;
    switch (message.kind) {
        case ControlMsgKind::Capabilities:
            // A new control session. Ids from the old one will never be acknowledged.
            capabilities_ = message.capabilities;
            hasCapabilities_ = true;
            currentLens_.clear();
            lens_ = zoom_ = ev_ = focus_ = Slot{};
            awaitingLensUntil_ = 0;
            lensAttempts_ = 0;
            if (desired_.lens) lens_.Request(nowMs + kCameraSettleMs);
            ReplayControls(nowMs + kCameraSettleMs);
            break;
        case ControlMsgKind::LensChanged:
            // A camera (re)opened with everything back at defaults.
            currentLens_ = message.lensChanged.cameraId;
            awaitingLensUntil_ = 0;
            ReplayControls(nowMs + kCameraSettleMs);
            if (!desired_.lens || *desired_.lens == currentLens_) {
                lensAttempts_ = 0;
                lens_.pending = false;
            } else if (lensAttempts_ < kMaxLensAttempts) {
                lens_.Request(nowMs + kCameraSettleMs);
            }
            break;
        case ControlMsgKind::Settings:
            // Only to learn the lens when LensChanged was missed (the phone started streaming
            // before this PC's control connection existed).
            if (currentLens_.empty() && !message.settings.cameraId.empty()) {
                currentLens_ = message.settings.cameraId;
                if (desired_.lens && *desired_.lens != currentLens_) lens_.Request(nowMs);
            }
            break;
        case ControlMsgKind::Ack:
            if (OnAck(lens_, message.ack, nowMs)) {
                if (message.ack.result != kAckOk) awaitingLensUntil_ = 0;
            } else if (!OnAck(zoom_, message.ack, nowMs) && !OnAck(ev_, message.ack, nowMs)) {
                OnAck(focus_, message.ack, nowMs);
            }
            break;
        default:
            break;
    }
}

bool CameraSync::Due(Slot& slot, int64_t nowMs) {
    if (slot.inFlight != 0 && nowMs - slot.sentAt > kAckTimeoutMs) slot.inFlight = 0;
    return slot.pending && slot.inFlight == 0 && nowMs >= slot.notBefore;
}

void CameraSync::Sent(Slot& slot, uint32_t id, int64_t nowMs) {
    slot.pending = false;
    slot.inFlight = id;
    slot.sentAt = nowMs;
}

void CameraSync::Pump(bool streaming, int64_t nowMs) {
    if (!streaming || !hasCapabilities_ || capabilities_.lenses.empty()) return;
    if (awaitingLensUntil_ != 0) {
        if (nowMs < awaitingLensUntil_) return;
        awaitingLensUntil_ = 0;
    }

    if (Due(lens_, nowMs) && !currentLens_.empty()) {
        if (!desired_.lens || *desired_.lens == currentLens_ || !FindLens(*desired_.lens)) {
            lens_.pending = false;  // already there, or a lens this phone doesn't have
        } else {
            ++lensAttempts_;
            Sent(lens_, commands_.setLens(*desired_.lens), nowMs);
            awaitingLensUntil_ = nowMs + kLensChangeTimeoutMs;
            return;
        }
    }

    // Until the phone says which lens is active, assume its default (the first one listed).
    const auto* lens = FindLens(currentLens_);
    if (!lens) lens = &capabilities_.lenses.front();

    // A saved lens can't be restored without knowing the current one, and the phone only
    // reports it after a camera command: make sure there is one.
    if (lens_.pending && currentLens_.empty() && !desired_.zoom && zoom_.inFlight == 0 && nowMs >= lens_.notBefore &&
        lens_.attempts < kMaxAttempts) {
        ++lens_.attempts;
        lens_.notBefore = nowMs + kBusyRetryMs;
        commands_.setZoom(std::clamp(1.0f, lens->zoomMin, lens->zoomMax));
    }

    if (Due(zoom_, nowMs)) {
        zoom_.pending = false;
        if (desired_.zoom && lens->zoomMax > lens->zoomMin) {
            Sent(zoom_, commands_.setZoom(std::clamp(*desired_.zoom, lens->zoomMin, lens->zoomMax)), nowMs);
        }
    }
    if (Due(ev_, nowMs)) {
        ev_.pending = false;
        if (desired_.ev && lens->evMax > lens->evMin) {
            Sent(ev_, commands_.setEv(std::clamp(*desired_.ev, lens->evMin, lens->evMax)), nowMs);
        }
    }
    if (Due(focus_, nowMs)) {
        focus_.pending = false;
        if (desired_.focus &&
            std::find(lens->afModes.begin(), lens->afModes.end(), *desired_.focus) != lens->afModes.end()) {
            Sent(focus_, commands_.setFocus(*desired_.focus), nowMs);
        }
    }
}

}  // namespace phonecam::camera
