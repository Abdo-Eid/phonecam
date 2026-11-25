#pragma once

#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdint>

#include "picture/Framing.h"

namespace phonecam::vcam_ctl {

// Thin wrapper around IMFVirtualCamera: creates and starts a Media
// Foundation virtual camera backed by the phonecam-vcam.dll media source
// (CLSID must already be registered via regsvr32 -- see docs/build.md).
class VCamControl {
public:
    HRESULT Start(const wchar_t* friendlyName, const GUID& sourceClsid);
    HRESULT Stop();

private:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> camera_;
};

// Decides when the camera must be re-created to offer a newly chosen size.
//
// Windows asks the media source for its sizes once, when the camera is created, and keeps
// answering apps from that (measured: rewriting the chosen size changed nothing, not even for
// apps opening the camera later). Re-creating it takes it away from every app using it, so that
// waits until nobody is. Until then apps keep their frame and the new shape is fitted into it.
class OfferedSizeWatch {
public:
    explicit OfferedSizeWatch(phonecam::picture::Size created) : created_(created), wanted_(created) {}

    // True when the camera should be re-created now. Call Created() once that has been done.
    bool Due(phonecam::picture::Size offered, bool inUse, int64_t nowMs) {
        if (offered != wanted_) {
            wanted_ = offered;
            since_ = nowMs;
        }
        // Settle first: stepping through the shapes, or a phone being turned, passes through
        // sizes nobody wants a camera for.
        return wanted_ != created_ && !inUse && nowMs - since_ >= kSettleMs && nowMs >= retryAt_;
    }
    void Created() { created_ = wanted_; }
    void Failed(int64_t nowMs) { retryAt_ = nowMs + kRetryMs; }

private:
    static constexpr int64_t kSettleMs = 600;
    static constexpr int64_t kRetryMs = 3000;
    phonecam::picture::Size created_, wanted_;
    int64_t since_ = 0;
    int64_t retryAt_ = 0;
};

}  // namespace phonecam::vcam_ctl
