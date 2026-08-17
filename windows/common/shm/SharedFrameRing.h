#pragma once

// Cross-process, cross-session frame handoff between phonecam-host (producer)
// and phonecam-vcam.dll (consumer, loaded in-process into whichever
// application -- or Windows service -- is reading the virtual camera).
//
// MUST live in the Global\ kernel object namespace, not Local\ (== unprefixed,
// session-relative): Windows 11's Frame Server / Frame Server Monitor
// services load phonecam-vcam.dll into their OWN process, running as
// LocalService/LocalSystem in Session 0 -- a different session than the
// interactive host process. Local\ objects are only visible within the
// creating session, so Session 0 could never see them. The explicit SDDL
// below is required for the same reason: the default DACL on an object
// created by the interactive user does not grant access to the
// LocalService/LocalSystem SIDs the Frame Server runs as.
// See: https://github.com/smourier/VCamSample/issues/1
//
// Header-only by design: phonecam-host builds via CMake, phonecam-vcam.dll
// builds via its own MSBuild project (forked from VCamSample, which needs
// NuGet-distributed WIL/CppWinRT that CMake doesn't naturally consume). The
// two are process-isolated and talk ONLY through this struct layout, so they
// can keep different build systems -- this header is the entire contract.

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace phonecam::shm {

inline constexpr wchar_t kMappingName[] = L"Global\\PhoneCam_FrameRing_v1";
inline constexpr wchar_t kReadyEventName[] = L"Global\\PhoneCam_FrameReady_v1";

// SYSTEM, Administrators, Local Service, Network Service, Everyone: generic
// all access. Broad on purpose -- this is single-machine local IPC carrying
// nothing more sensitive than camera pixels, and the consumer's identity
// (which service/session loads phonecam-vcam.dll) isn't known ahead of time.
inline constexpr wchar_t kSecurityDescriptorSddl[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;WD)";

inline constexpr uint32_t kRingSlots = 3;
inline constexpr uint32_t kMaxWidth = 1920;
inline constexpr uint32_t kMaxHeight = 1080;
// NV12, packed, stride == width (no row padding): Y plane (height rows) then
// an interleaved UV plane (height/2 rows) immediately after, one contiguous
// buffer -- the same layout windows/vcam/Tools.cpp's RGB32ToNV12 produces.
inline constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 3 / 2;

struct FrameSlot {
    // Seqlock: even == stable/readable, odd == writer in progress. Reader
    // retries if it observes an odd value, or if the value changed between
    // the start and end of its copy (torn read).
    std::atomic<uint32_t> sequence;
    uint32_t width;
    uint32_t height;
    uint64_t timestampUs;
    uint8_t data[kMaxFrameBytes];
};

struct RingHeader {
    std::atomic<uint32_t> writeIndex;  // index into slots[] last published
    FrameSlot slots[kRingSlots];
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "FrameSlot::sequence must be lock-free to be safe across process boundaries");

namespace detail {

inline bool MakeSecurityAttributes(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSecurityDescriptorSddl, SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    sa.lpSecurityDescriptor = sd;
    return true;
}

}  // namespace detail

// Not copyable; one instance per process (producer or consumer).
class SharedFrameRing {
public:
    SharedFrameRing() = default;
    ~SharedFrameRing() { Close(); }
    SharedFrameRing(const SharedFrameRing&) = delete;
    SharedFrameRing& operator=(const SharedFrameRing&) = delete;

    // Producer: create the mapping if it doesn't exist yet, or attach to it
    // if it does (idempotent -- safe even if a previous instance crashed
    // without cleaning up, since the mapping is reference-counted by the OS).
    bool OpenOrCreate() {
        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!detail::MakeSecurityAttributes(sa, sd)) {
            return false;
        }

        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(sizeof(RingHeader)), kMappingName);
        // Capture the error immediately: LocalFree (or anything else) can
        // clobber GetLastError before we read it, silently corrupting the
        // alreadyExisted check and causing a live ring to be re-zeroed out
        // from under an existing reader/writer.
        const DWORD createError = GetLastError();
        LocalFree(sd);
        if (!mapping_) {
            return false;
        }
        const bool alreadyExisted = (createError == ERROR_ALREADY_EXISTS);

        if (!MapAndOpenEvent()) {
            return false;
        }
        if (!alreadyExisted) {
            ZeroMemory(view_, sizeof(RingHeader));
        }
        return true;
    }

    // Consumer: open an already-created mapping. Fails (returns false) if
    // the host hasn't created it yet -- caller should treat that as "no
    // frame available", not a fatal error.
    bool Open() {
        mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
        if (!mapping_) {
            return false;
        }
        return MapAndOpenEvent();
    }

    void Close() {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (readyEvent_) {
            CloseHandle(readyEvent_);
            readyEvent_ = nullptr;
        }
    }

    bool IsOpen() const { return view_ != nullptr; }

    // Producer. data must be exactly width*height*3/2 bytes, NV12, packed
    // (stride == width). Not thread-safe against concurrent writers -- the
    // host has exactly one producer thread.
    bool WriteFrame(uint32_t width, uint32_t height, uint64_t timestampUs, const uint8_t* nv12Data) {
        if (!view_ || width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight) {
            return false;
        }
        const size_t frameBytes = static_cast<size_t>(width) * height * 3 / 2;
        if (frameBytes > kMaxFrameBytes) {
            return false;
        }

        RingHeader* header = static_cast<RingHeader*>(view_);
        const uint32_t prevIndex = header->writeIndex.load(std::memory_order_relaxed);
        const uint32_t nextIndex = (prevIndex + 1) % kRingSlots;
        FrameSlot& slot = header->slots[nextIndex];

        const uint32_t seq = slot.sequence.fetch_add(1, std::memory_order_acq_rel) + 1;  // now odd: write in progress
        (void)seq;
        slot.width = width;
        slot.height = height;
        slot.timestampUs = timestampUs;
        std::memcpy(slot.data, nv12Data, frameBytes);
        slot.sequence.fetch_add(1, std::memory_order_release);  // now even: stable

        header->writeIndex.store(nextIndex, std::memory_order_release);
        if (readyEvent_) {
            SetEvent(readyEvent_);
        }
        return true;
    }

    // Consumer. outBufferCapacity must be >= kMaxFrameBytes. Returns false if
    // no frame has ever been published, or after exhausting retries against a
    // torn read (an extremely unlikely race against a producer that's
    // publishing faster than kRingSlots allows for the time this copy takes).
    bool TryReadLatestFrame(uint32_t& width, uint32_t& height, uint64_t& timestampUs, uint8_t* outNv12Buffer,
                             size_t outBufferCapacity) {
        if (!view_) {
            return false;
        }
        RingHeader* header = static_cast<RingHeader*>(view_);

        for (int retry = 0; retry < 8; ++retry) {
            const uint32_t index = header->writeIndex.load(std::memory_order_acquire);
            FrameSlot& slot = header->slots[index];

            const uint32_t seqBefore = slot.sequence.load(std::memory_order_acquire);
            if (seqBefore == 0) {
                return false;  // never written
            }
            if (seqBefore & 1) {
                continue;  // writer mid-publish; retry
            }

            const uint32_t w = slot.width;
            const uint32_t h = slot.height;
            const uint64_t ts = slot.timestampUs;
            const size_t frameBytes = static_cast<size_t>(w) * h * 3 / 2;
            if (frameBytes == 0 || frameBytes > outBufferCapacity || frameBytes > kMaxFrameBytes) {
                return false;
            }
            std::memcpy(outNv12Buffer, slot.data, frameBytes);

            const uint32_t seqAfter = slot.sequence.load(std::memory_order_acquire);
            if (seqBefore != seqAfter) {
                continue;  // torn read (writer overtook us mid-copy); retry
            }

            width = w;
            height = h;
            timestampUs = ts;
            return true;
        }
        return false;
    }

    // Consumer: block until a new frame is published, or timeoutMs elapses.
    // Optional -- TryReadLatestFrame() alone is enough for a poll-driven
    // reader (e.g. MediaStream's per-frame pull in Phase 1b); this exists for
    // a future push-driven consumer.
    bool WaitForFrame(DWORD timeoutMs) {
        if (!readyEvent_) {
            return false;
        }
        return WaitForSingleObject(readyEvent_, timeoutMs) == WAIT_OBJECT_0;
    }

private:
    bool MapAndOpenEvent() {
        view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RingHeader));
        if (!view_) {
            return false;
        }

        SECURITY_ATTRIBUTES sa{};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!detail::MakeSecurityAttributes(sa, sd)) {
            return false;
        }
        readyEvent_ = CreateEventW(&sa, /*manualReset=*/FALSE, /*initialState=*/FALSE, kReadyEventName);
        LocalFree(sd);
        return readyEvent_ != nullptr;
    }

    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    void* view_ = nullptr;
};

}  // namespace phonecam::shm
