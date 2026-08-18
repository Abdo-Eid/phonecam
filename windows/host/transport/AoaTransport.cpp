#include "transport/AoaTransport.h"

#include <libusb.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "aoa/AoaIdentity.h"
#include "log/Log.h"

namespace phonecam::transport {

namespace {

// Same VID/PID facts aoa_probe/main.cpp already established live on the
// reference device -- see its header comment and docs/architecture.md's
// Phase 7 section for how these were learned (not assumed).
constexpr uint16_t kXiaomiVid = 0x2717;
constexpr uint16_t kGoogleVid = 0x18D1;
constexpr uint16_t kAccessoryPidLow = 0x2D00;
constexpr uint16_t kAccessoryPidHigh = 0x2D05;
bool IsAccessoryModePid(uint16_t pid) { return pid >= kAccessoryPidLow && pid <= kAccessoryPidHigh; }

// AOA control requests (see source.android.com/docs/core/interaction/accessories/aoa).
constexpr uint8_t kAoaGetProtocol = 51;
constexpr uint8_t kAoaSendString = 52;
constexpr uint8_t kAoaStart = 53;
constexpr int kUsbControlTimeoutMs = 2000;

// Per-bulk-transfer read timeout: short enough that Disconnect() (which waits
// for the in-flight transfer's lock, see RunReceiveLoop) doesn't stall
// shutdown noticeably, matching the ~200ms cadence the rest of this codebase
// already uses for its own poll/backoff loops (e.g. LiveVideoBridge's
// kReconnectPollInterval).
constexpr unsigned int kReadTimeoutMs = 200;

// WireFraming.kt's fixed header, see docs/wire-protocol.md.
constexpr size_t kWireHeaderSize = 20;
constexpr uint8_t kChannelVideo = 0;
constexpr uint8_t kChannelControl = 1;

bool SendIdString(libusb_device_handle* handle, uint16_t index, const char* value) {
    auto len = static_cast<uint16_t>(std::strlen(value) + 1);  // include NUL, matches AOA spec examples
    int transferred = libusb_control_transfer(
        handle, /*bmRequestType=*/0x40, kAoaSendString, /*wValue=*/0, index,
        reinterpret_cast<unsigned char*>(const_cast<char*>(value)), len, kUsbControlTimeoutMs);
    return transferred >= 0;
}

// Finds the re-enumerated accessory-mode device, opens it, and claims its AOA
// accessory interface (distinguished from an "accessory,adb" composite
// config's other interface by class triple -- accessory is 0xFF/0xFF/0x00,
// adb is 0xFF/0x42/0x01 -- NOT by assuming interface 0). Direct port of
// aoa_probe/main.cpp's OpenAccessoryInterface(), which this exact sequence
// was debugged and proven against on the reference device.
bool OpenAccessoryInterface(libusb_context* ctx, libusb_device_handle** outHandle, int* outInterfaceNum,
                             uint8_t* outBulkIn, uint8_t* outBulkOut) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) return false;

    libusb_device* accessory = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor == kGoogleVid && IsAccessoryModePid(desc.idProduct)) {
            accessory = list[i];
            break;
        }
    }
    if (!accessory) {
        libusb_free_device_list(list, 1);
        return false;
    }

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(accessory, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0) {
        phonecam::log::Error("AoaVideoTransport: libusb_open (accessory) failed");
        return false;
    }

    libusb_config_descriptor* config = nullptr;
    rc = libusb_get_active_config_descriptor(accessory, &config);
    if (rc != 0) {
        libusb_close(handle);
        return false;
    }

    int accessoryInterfaceNum = -1;
    uint8_t bulkInEndpoint = 0;
    uint8_t bulkOutEndpoint = 0;
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface_descriptor& iface = config->interface[i].altsetting[0];
        bool isAccessory = iface.bInterfaceClass == 0xFF && iface.bInterfaceSubClass == 0xFF &&
                            iface.bInterfaceProtocol == 0x00;
        if (!isAccessory) continue;
        accessoryInterfaceNum = iface.bInterfaceNumber;
        for (int e = 0; e < iface.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = iface.endpoint[e];
            bool isBulk = (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
            bool isIn = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
            if (isBulk && isIn) bulkInEndpoint = ep.bEndpointAddress;
            if (isBulk && !isIn) bulkOutEndpoint = ep.bEndpointAddress;
        }
    }
    libusb_free_config_descriptor(config);

    if (accessoryInterfaceNum < 0 || bulkInEndpoint == 0) {
        libusb_close(handle);
        return false;
    }

    rc = libusb_claim_interface(handle, accessoryInterfaceNum);
    if (rc != 0) {
        phonecam::log::Error("AoaVideoTransport: libusb_claim_interface failed");
        libusb_close(handle);
        return false;
    }

    *outHandle = handle;
    *outInterfaceNum = accessoryInterfaceNum;
    *outBulkIn = bulkInEndpoint;
    *outBulkOut = bulkOutEndpoint;
    return true;
}

// Finds the phone in normal (pre-accessory) mode, sends the AOA handshake
// (GET_PROTOCOL + identification strings + ACCESSORY_START), and waits up to
// 10s for it to re-enumerate as an accessory-mode PID -- direct port of
// aoa_probe/main.cpp's RunHandshake(), minus its console logging. `cancelled`
// is polled during the wait so a concurrent Disconnect() can abort a
// Connect() that's stuck here.
bool RunHandshake(libusb_context* ctx, const std::atomic<bool>& cancelled) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) return false;

    libusb_device* target = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        bool candidateVid = desc.idVendor == kXiaomiVid || desc.idVendor == kGoogleVid;
        if (candidateVid && !IsAccessoryModePid(desc.idProduct)) {
            target = list[i];
            break;
        }
    }
    if (!target) {
        libusb_free_device_list(list, 1);
        return false;
    }

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(target, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0) return false;

    unsigned char protocolBuf[2] = {0, 0};
    int transferred = libusb_control_transfer(handle, /*bmRequestType=*/0xC0, kAoaGetProtocol, 0, 0, protocolBuf,
                                               sizeof(protocolBuf), kUsbControlTimeoutMs);
    if (transferred < 0 || (protocolBuf[0] | (protocolBuf[1] << 8)) == 0) {
        libusb_close(handle);
        return false;
    }

    using namespace phonecam::aoa;
    bool ok = SendIdString(handle, 0, kManufacturer) && SendIdString(handle, 1, kModel) &&
              SendIdString(handle, 2, kDescription) && SendIdString(handle, 3, kVersion) &&
              SendIdString(handle, 4, kUri) && SendIdString(handle, 5, kSerial);
    if (ok) {
        transferred = libusb_control_transfer(handle, 0x40, kAoaStart, 0, 0, nullptr, 0, kUsbControlTimeoutMs);
        ok = transferred >= 0;
    }
    libusb_close(handle);
    if (!ok) return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled.load()) return false;
        libusb_device** reenum = nullptr;
        ssize_t reenumCount = libusb_get_device_list(ctx, &reenum);
        bool found = false;
        for (ssize_t i = 0; i < reenumCount; ++i) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(reenum[i], &desc) != 0) continue;
            if (desc.idVendor == kGoogleVid && IsAccessoryModePid(desc.idProduct)) {
                found = true;
                break;
            }
        }
        libusb_free_device_list(reenum, 1);
        if (found) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

}  // namespace

struct AoaVideoTransport::Impl {
    libusb_context* ctx = nullptr;
    bool ctxOk = false;
    libusb_device_handle* handle = nullptr;
    int interfaceNum = -1;
    uint8_t bulkIn = 0;
    uint8_t bulkOut = 0;
    // Guards handle/interfaceNum/bulkIn/bulkOut: Connect() publishes them,
    // Disconnect() (possibly from another thread, e.g. LiveVideoBridge::Stop()
    // unblocking a worker thread stuck in RunReceiveLoop) tears them down.
    // RunReceiveLoop holds this lock for the duration of each individual
    // libusb_bulk_transfer call (not the whole loop) specifically so
    // Disconnect() can't call libusb_close() on a handle mid-transfer --
    // libusb documents that as unsafe -- while still bounding how long
    // Disconnect() can block waiting for the lock to ~kReadTimeoutMs.
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
};

AoaVideoTransport::AoaVideoTransport() : impl_(std::make_unique<Impl>()) {
    int rc = libusb_init(&impl_->ctx);
    if (rc != 0) {
        phonecam::log::Error("AoaVideoTransport: libusb_init failed");
        return;
    }
    impl_->ctxOk = true;
}

AoaVideoTransport::~AoaVideoTransport() {
    Disconnect();
    if (impl_->ctxOk) {
        libusb_exit(impl_->ctx);
    }
}

bool AoaVideoTransport::Connect() {
    if (!impl_->ctxOk) return false;
    impl_->cancelled.store(false);

    libusb_device_handle* handle = nullptr;
    int interfaceNum = -1;
    uint8_t bulkIn = 0, bulkOut = 0;
    if (!OpenAccessoryInterface(impl_->ctx, &handle, &interfaceNum, &bulkIn, &bulkOut)) {
        // Not already in accessory mode -- try to find it in normal mode and
        // switch it. A phone that's simply unplugged, off, or has USB
        // debugging disconnected will fail RunHandshake() here too, which is
        // the expected, non-fatal "not ready yet" case the caller retries.
        if (!RunHandshake(impl_->ctx, impl_->cancelled)) return false;
        if (impl_->cancelled.load()) return false;
        if (!OpenAccessoryInterface(impl_->ctx, &handle, &interfaceNum, &bulkIn, &bulkOut)) return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->cancelled.load()) {
        // Disconnect() arrived while we were connecting -- don't publish a
        // handle Stop() already believes is torn down.
        libusb_release_interface(handle, interfaceNum);
        libusb_close(handle);
        return false;
    }
    impl_->handle = handle;
    impl_->interfaceNum = interfaceNum;
    impl_->bulkIn = bulkIn;
    impl_->bulkOut = bulkOut;
    phonecam::log::Info("AoaVideoTransport: connected");
    return true;
}

void AoaVideoTransport::Disconnect() {
    impl_->cancelled.store(true);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->handle != nullptr) {
        libusb_release_interface(impl_->handle, impl_->interfaceNum);
        libusb_close(impl_->handle);
        impl_->handle = nullptr;
    }
}

void AoaVideoTransport::RunReceiveLoop(const VideoPacketCallback& onPacket) {
    std::vector<uint8_t> accum;
    uint8_t readBuf[16 * 1024];

    while (!impl_->cancelled.load()) {
        int actual = 0;
        int rc;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->handle == nullptr) break;
            rc = libusb_bulk_transfer(impl_->handle, impl_->bulkIn, readBuf, sizeof(readBuf), &actual,
                                       kReadTimeoutMs);
        }
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            phonecam::log::Info("AoaVideoTransport: device disappeared");
            break;
        }
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (actual <= 0) continue;
        accum.insert(accum.end(), readBuf, readBuf + actual);

        // Consume every complete frame currently at the front of accum,
        // keeping only a genuinely incomplete trailing frame (if any) for the
        // next read to complete -- see VideoTransport.h and
        // AoaAccessoryTransport.kt's doc comment for why a read/parse smaller
        // than a queued frame must never be attempted.
        size_t pos = 0;
        while (pos < accum.size()) {
            uint8_t tag = accum[pos];
            if (tag == kChannelVideo) {
                if (accum.size() - pos < 1 + kWireHeaderSize) break;  // header not fully arrived yet
                const uint8_t* header = accum.data() + pos + 1;
                // WireFraming layout: [0]=magic [1]=type [2]=flags [3]=reserved
                // [4..7]=seq LE [8..15]=pts_us LE [16..19]=payload_len LE.
                // Keying on the outer Channel tag first, then trusting this
                // header -- never scan for the magic byte itself to resync,
                // which could false-lock onto payload data.
                uint32_t seq = static_cast<uint32_t>(header[4]) | (static_cast<uint32_t>(header[5]) << 8) |
                               (static_cast<uint32_t>(header[6]) << 16) | (static_cast<uint32_t>(header[7]) << 24);
                uint64_t ptsUs = 0;
                for (int b = 0; b < 8; ++b) ptsUs |= static_cast<uint64_t>(header[8 + b]) << (8 * b);
                uint32_t payloadLen = static_cast<uint32_t>(header[16]) | (static_cast<uint32_t>(header[17]) << 8) |
                                       (static_cast<uint32_t>(header[18]) << 16) |
                                       (static_cast<uint32_t>(header[19]) << 24);
                size_t frameTotal = 1 + kWireHeaderSize + payloadLen;
                if (accum.size() - pos < frameTotal) break;  // payload not fully arrived yet

                const VideoPacket pkt{
                    header[1] == 0 ? PacketType::Config : PacketType::Frame, (header[2] & 0x01) != 0, seq, ptsUs,
                    accum.data() + pos + 1 + kWireHeaderSize, payloadLen,
                };
                onPacket(pkt);
                pos += frameTotal;
            } else if (tag == kChannelControl) {
                // Not wired to the control channel yet (still on adb) -- skip
                // over it rather than mis-parsing it as video, see class doc.
                if (accum.size() - pos < 5) break;
                uint32_t bodyLen = static_cast<uint32_t>(accum[pos + 1]) |
                                    (static_cast<uint32_t>(accum[pos + 2]) << 8) |
                                    (static_cast<uint32_t>(accum[pos + 3]) << 16) |
                                    (static_cast<uint32_t>(accum[pos + 4]) << 24);
                if (accum.size() - pos < 5 + bodyLen) break;
                pos += 5 + bodyLen;
            } else {
                pos += 1;  // unexpected tag -- drop one byte and resync
            }
        }
        if (pos > 0) {
            accum.erase(accum.begin(), accum.begin() + static_cast<std::ptrdiff_t>(pos));
        }
    }
}

}  // namespace phonecam::transport
