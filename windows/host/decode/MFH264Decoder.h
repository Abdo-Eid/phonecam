#pragma once

// Synchronous H.264 -> NV12 decoder wrapping the built-in Windows Media
// Foundation software decoder MFT (CLSID_CMSH264DecoderMFT). Used by
// phonecam-host to turn the Annex-B stream received from the phone into NV12
// frames for SharedFrameRing::WriteFrame.
//
// Usage: Initialize() once, then Feed() each Annex-B chunk (CONFIG = leading
// SPS/PPS, or one/more NALUs for a coded picture) in decode order. Zero or
// more decoded frames come back synchronously via the callback per Feed()
// call, as the MFT's internal pipeline fills and drains.

#include <codecapi.h>   // CODECAPI_AVLowLatencyMode and friends (property GUIDs)
#include <icodecapi.h>  // ICodecAPI (the interface itself)
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace phonecam::decode {

// Packed NV12 (stride == width), ready for SharedFrameRing::WriteFrame.
// Only valid for the duration of the FrameCallback invocation.
struct DecodedFrame {
    uint32_t width;
    uint32_t height;
    uint64_t timestampUs;
    const uint8_t* nv12;
    size_t nv12Size;
};

using FrameCallback = std::function<void(const DecodedFrame&)>;

class MFH264Decoder {
public:
    MFH264Decoder() = default;
    ~MFH264Decoder() = default;

    MFH264Decoder(const MFH264Decoder&) = delete;
    MFH264Decoder& operator=(const MFH264Decoder&) = delete;

    bool Initialize();

    // data/size: one Annex-B chunk, starting at a NALU boundary (internal
    // start codes within the chunk are fine). timestampUs is attached to
    // whatever decoded frame(s) this input eventually produces.
    bool Feed(const uint8_t* data, size_t size, uint64_t timestampUs, const FrameCallback& onFrame);

private:
    bool NegotiateOutputType();
    void DrainOutput(const FrameCallback& onFrame);
    bool DeliverSample(IMFSample* sample, const FrameCallback& onFrame);

    Microsoft::WRL::ComPtr<IMFTransform> decoder_;
    bool providesSamples_ = false;
    DWORD outputSampleSize_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;        // true display height (from the clean aperture)
    uint32_t codedHeight_ = 0;   // padded/macroblock-aligned surface height -- the UV plane starts after THIS many Y rows, not height_
    std::vector<uint8_t> packBuffer_;
};

}  // namespace phonecam::decode
