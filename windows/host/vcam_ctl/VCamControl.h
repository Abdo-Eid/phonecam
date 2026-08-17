#pragma once

#include <mfvirtualcamera.h>
#include <wrl/client.h>

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

}  // namespace phonecam::vcam_ctl
