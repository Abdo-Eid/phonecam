#include "decode/MFH264Decoder.h"

#include <mferror.h>

#include <cstring>
#include <format>

#include "log/Log.h"

using Microsoft::WRL::ComPtr;

namespace phonecam::decode {

namespace {

// Normally declared in wmcodecdsp.h, but that header transitively includes
// DirectShow's strmif.h, which redefines CodecAPIEventData already declared
// by icodecapi.h -- a long-standing Windows SDK header conflict. Declaring
// this well-known, stable constant directly sidesteps it entirely.
constexpr GUID kClsidCMSH264DecoderMFT = {
    0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};

// NV12's chroma plane immediately follows the luma plane at the SAME row
// pitch (MF's documented layout) -- but it follows the FULL CODED (padded)
// luma plane, not just the display height. The decoder's surface allocation
// is always macroblock-aligned (e.g. a 1080 display height is backed by a
// 1088-row surface, the next multiple of 16); the chroma plane starts at
// codedHeight rows down, regardless of where the display crop ends. Getting
// this wrong doesn't shear or crash -- it reads a handful of luma padding
// rows as if they were chroma, which showed up as a thin green band at the
// top of the image (confirmed empirically against the 1080p scratchpad
// capture; 720p's dimensions are both multiples of 16 so it never exposed
// this). This is also why a flat memcpy is unsafe in general: row pitch can
// independently exceed width too.
bool PackNv12FromStrided(const uint8_t* scanline0, LONG pitch, uint32_t width, uint32_t height, uint32_t codedHeight,
                          uint8_t* dst) {
    const size_t rowBytes = width;
    const size_t absPitch = static_cast<size_t>(pitch >= 0 ? pitch : -pitch);
    if (absPitch < rowBytes || codedHeight < height) {
        return false;
    }

    const uint8_t* row = scanline0;
    uint8_t* out = dst;
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out, row, rowBytes);
        out += rowBytes;
        row += pitch;
    }

    const uint8_t* uvRow = scanline0 + static_cast<size_t>(codedHeight) * pitch;
    for (uint32_t y = 0; y < height / 2; ++y) {
        std::memcpy(out, uvRow, rowBytes);
        out += rowBytes;
        uvRow += pitch;
    }
    return true;
}

}  // namespace

bool MFH264Decoder::Initialize() {
    HRESULT hr = CoCreateInstance(kClsidCMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_));
    if (FAILED(hr)) {
        phonecam::log::Error(std::format("MFH264Decoder: CoCreateInstance failed hr=0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = decoder_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        phonecam::log::Error(std::format("MFH264Decoder: SetInputType failed hr=0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }

    // Best-effort: not fatal if this particular decoder build ignores it.
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(decoder_.As(&codecApi))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        VariantClear(&v);
    }

    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool MFH264Decoder::Feed(const uint8_t* data, size_t size, uint64_t timestampUs, const FrameCallback& onFrame) {
    if (!decoder_ || size == 0) {
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return false;
    std::memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return false;
    sample->SetSampleTime(static_cast<LONGLONG>(timestampUs) * 10);  // 100ns units

    for (int attempt = 0; attempt < 4; ++attempt) {
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            DrainOutput(onFrame);
            continue;
        }
        break;
    }
    if (FAILED(hr)) {
        phonecam::log::Error(std::format("MFH264Decoder: ProcessInput failed hr=0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }

    DrainOutput(onFrame);
    return true;
}

bool MFH264Decoder::NegotiateOutputType() {
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr = decoder_->GetOutputAvailableType(0, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
        GUID subtype{};
        candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            chosen = candidate;
            break;
        }
    }
    if (!chosen) {
        phonecam::log::Error("MFH264Decoder: no NV12 output type offered");
        return false;
    }

    HRESULT hr = decoder_->SetOutputType(0, chosen.Get(), 0);
    if (FAILED(hr)) {
        phonecam::log::Error(std::format("MFH264Decoder: SetOutputType failed hr=0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(chosen.Get(), MF_MT_FRAME_SIZE, &w, &h);
    codedHeight_ = h;  // padded/macroblock-aligned -- where the UV plane actually starts

    // MF_MT_FRAME_SIZE is the padded/macroblock-aligned CODED size (e.g.
    // 1080 becomes 1088, the next multiple of 16) -- not the true display
    // size. The clean aperture carries the real crop rectangle; prefer it
    // when present, matching what an on-screen consumer would show.
    MFVideoArea aperture{};
    UINT32 blobSize = 0;
    if (SUCCEEDED(chosen->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture),
                                   sizeof(aperture), &blobSize)) &&
        blobSize == sizeof(aperture)) {
        w = static_cast<UINT32>(aperture.Area.cx);
        h = static_cast<UINT32>(aperture.Area.cy);
    }
    width_ = w;
    height_ = h;

    MFT_OUTPUT_STREAM_INFO info{};
    decoder_->GetOutputStreamInfo(0, &info);
    providesSamples_ = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    outputSampleSize_ = info.cbSize;

    packBuffer_.resize(static_cast<size_t>(width_) * height_ * 3 / 2);

    phonecam::log::Info(std::format("MFH264Decoder: output negotiated {}x{} providesSamples={} cbSize={}", width_,
                                     height_, providesSamples_ ? 1 : 0, static_cast<unsigned>(outputSampleSize_)));
    return true;
}

void MFH264Decoder::DrainOutput(const FrameCallback& onFrame) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
        outputBuffer.dwStreamID = 0;

        ComPtr<IMFSample> outSample;
        if (!providesSamples_) {
            if (outputSampleSize_ == 0) {
                // Output type not negotiated yet -- must happen via the
                // MF_E_TRANSFORM_STREAM_CHANGE branch below first.
                outputBuffer.pSample = nullptr;
            } else {
                if (FAILED(MFCreateSample(&outSample))) return;
                ComPtr<IMFMediaBuffer> outMediaBuffer;
                if (FAILED(MFCreateMemoryBuffer(outputSampleSize_, &outMediaBuffer))) return;
                outSample->AddBuffer(outMediaBuffer.Get());
                outputBuffer.pSample = outSample.Get();
            }
        }

        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &outputBuffer, &status);

        ComPtr<IMFSample> allocatedSample;
        if (providesSamples_ && outputBuffer.pSample) {
            allocatedSample.Attach(outputBuffer.pSample);
        }
        ComPtr<IMFCollection> events;
        if (outputBuffer.pEvents) {
            events.Attach(outputBuffer.pEvents);
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            // TYPE_NOT_SET: no output type negotiated yet (haven't seen an
            // SPS this decoder is willing to commit to). STREAM_CHANGE: type
            // was set, but the decoder now knows the real coded dimensions
            // differ. Both are resolved the same way -- (re)negotiate and
            // retry ProcessOutput.
            if (!NegotiateOutputType()) return;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return;
        }
        if (FAILED(hr)) {
            phonecam::log::Error(std::format("MFH264Decoder: ProcessOutput failed hr=0x{:08X}", static_cast<unsigned>(hr)));
            return;
        }

        IMFSample* produced = providesSamples_ ? allocatedSample.Get() : outSample.Get();
        if (produced) {
            DeliverSample(produced, onFrame);
        }
    }
}

bool MFH264Decoder::DeliverSample(IMFSample* sample, const FrameCallback& onFrame) {
    if (width_ == 0 || height_ == 0) return false;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) {
        phonecam::log::Error(std::format("MFH264Decoder: ConvertToContiguousBuffer failed hr=0x{:08X}", static_cast<unsigned>(hr)));
        return false;
    }

    // Try IMF2DBuffer first (needed for a real hardware/D3D-backed decoder
    // later -- see Phase 6 in docs/architecture.md), but it is NOT a given:
    // confirmed empirically that this software decoder's client-allocated
    // buffers (MFCreateMemoryBuffer, the providesSamples_ == false path
    // exercised today) don't implement it at all (E_NOINTERFACE every time)
    // -- so the flat fallback below is this configuration's actual primary
    // path, not a rarely-hit corner case.
    bool ok = false;
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        BYTE* scanline0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer2d->Lock2D(&scanline0, &pitch))) {
            ok = PackNv12FromStrided(scanline0, pitch, width_, height_, codedHeight_, packBuffer_.data());
            buffer2d->Unlock2D();
        }
    }
    if (!ok) {
        // Flat buffer: pitch == width_ (no row padding), but the Y plane
        // still spans codedHeight_ rows before the UV plane begins -- same
        // reasoning as the 2D path, just with an implied rather than a
        // queried pitch (confirmed by the earlier green-band bug: assuming
        // the UV plane started at height_ rows, not codedHeight_, was wrong
        // here too).
        BYTE* raw = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (SUCCEEDED(buffer->Lock(&raw, &maxLen, &curLen))) {
            const size_t need = (static_cast<size_t>(codedHeight_) + height_ / 2) * width_;
            if (curLen >= need) {
                ok = PackNv12FromStrided(raw, static_cast<LONG>(width_), width_, height_, codedHeight_, packBuffer_.data());
            }
            buffer->Unlock();
        }
    }
    if (!ok) {
        phonecam::log::Error(std::format("MFH264Decoder: failed to pack sample width={} height={} codedHeight={}",
                                          width_, height_, codedHeight_));
        return false;
    }

    LONGLONG sampleTime = 0;
    sample->GetSampleTime(&sampleTime);
    const uint64_t timestampUs = static_cast<uint64_t>(sampleTime / 10);

    DecodedFrame frame{width_, height_, timestampUs, packBuffer_.data(), packBuffer_.size()};
    onFrame(frame);
    return true;
}

}  // namespace phonecam::decode
