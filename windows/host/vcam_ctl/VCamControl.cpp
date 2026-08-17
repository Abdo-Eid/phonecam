#include "vcam_ctl/VCamControl.h"

#include "log/Log.h"

#pragma comment(lib, "mfsensorgroup")

namespace phonecam::vcam_ctl {

HRESULT VCamControl::Start(const wchar_t* friendlyName, const GUID& sourceClsid) {
    wchar_t clsidStr[64];
    if (StringFromGUID2(sourceClsid, clsidStr, ARRAYSIZE(clsidStr)) == 0) {
        return E_UNEXPECTED;
    }

    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        friendlyName,
        clsidStr,
        nullptr,
        0,
        &camera_);
    if (FAILED(hr)) {
        phonecam::log::Error("MFCreateVirtualCamera failed");
        return hr;
    }

    hr = camera_->Start(nullptr);
    if (FAILED(hr)) {
        phonecam::log::Error("IMFVirtualCamera::Start failed (is phonecam-vcam.dll registered via regsvr32?)");
        camera_.Reset();
        return hr;
    }

    phonecam::log::Info("Virtual camera started");
    return S_OK;
}

HRESULT VCamControl::Stop() {
    if (!camera_) {
        return S_OK;
    }
    // NOTE: don't call Shutdown() here -- the media source's own DLL_PROCESS_DETACH
    // / Frame Server teardown already does that; calling it again here causes a
    // double-Shutdown that prevents clean removal (same caveat as upstream VCamSample).
    HRESULT hr = camera_->Remove();
    camera_.Reset();
    return hr;
}

}  // namespace phonecam::vcam_ctl
