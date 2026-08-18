// Throwaway diagnostic tool for Phase 7 (AOA transport) -- NOT part of the shipping product.
// Answers two questions before any production code gets written against them:
//   1. Does the Redmi Note 8 show up over libusb at all, and what's its current (pre-accessory)
//      PID? (Xiaomi VID is 0x2717.)
//   2. Once bound to a libusb-usable driver (WinUSB via Zadig -- NOT done by this tool), does the
//      phone answer the AOA handshake, and which PID does it re-enumerate as afterward? 0x2D01
//      (accessory+adb) vs 0x2D00 (accessory-only) decides whether the adb dev loop survives
//      Phase 7 development -- see docs/architecture.md's Phase 7 section.
//
// Run with no args: enumerate-only, safe, no driver changes required.
// Run with --handshake: also attempts the AOA handshake -- requires the phone's relevant
// interface already bound to WinUSB (via Zadig), or libusb_open will fail with
// LIBUSB_ERROR_NOT_SUPPORTED / LIBUSB_ERROR_ACCESS.

#include <libusb.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "aoa/AoaIdentity.h"

namespace {

constexpr uint16_t kXiaomiVid = 0x2717;
constexpr uint16_t kGoogleVid = 0x18D1;

int ReadCheckpointLoop(libusb_context* ctx);  // defined below; RunHandshake calls it

// Frame tags for the host->phone OUT-direction tests below. Not the real wire-protocol.md
// Channel enum -- this is a throwaway rehearsal of "1-byte tag + self-delimiting body" framing,
// specifically to prove the OUT direction and multi-packet reassembly work before any of that
// design goes into production code. See docs/architecture.md's Phase 7 section.
constexpr uint8_t kTagTextMarker = 0;   // body: ASCII text ending in '\n'
constexpr uint8_t kTagLengthPrefixed = 1;  // body: 4-byte LE length + that many pattern bytes

// This phone enumerates under Google's VID even in normal (non-accessory) mode -- its ADB/MTP
// composite interface uses the well-known 0x18D1:0x4EE2 pair, not a Xiaomi-branded PID. Only
// 0x2D00-0x2D05 are actual AOA accessory-mode PIDs (see
// source.android.com/docs/core/interaction/accessories/aoa). Learned empirically via the
// enumerate-only probe before writing this, not assumed -- don't trust VID alone to mean
// "already in accessory mode."
constexpr uint16_t kAccessoryPidLow = 0x2D00;
constexpr uint16_t kAccessoryPidHigh = 0x2D05;
bool IsAccessoryModePid(uint16_t pid) { return pid >= kAccessoryPidLow && pid <= kAccessoryPidHigh; }

// AOA control requests (see source.android.com/docs/core/interaction/accessories/aoa).
constexpr uint8_t kAoaGetProtocol = 51;
constexpr uint8_t kAoaSendString = 52;
constexpr uint8_t kAoaStart = 53;

constexpr int kUsbTimeoutMs = 2000;

bool SendIdString(libusb_device_handle* handle, uint16_t index, const char* value) {
    auto len = static_cast<uint16_t>(std::strlen(value) + 1);  // include NUL, matches AOA spec examples
    int transferred = libusb_control_transfer(
        handle, /*bmRequestType=*/0x40, kAoaSendString, /*wValue=*/0, index,
        reinterpret_cast<unsigned char*>(const_cast<char*>(value)), len, kUsbTimeoutMs);
    if (transferred < 0) {
        std::printf("  SendString(%u, \"%s\") failed: %s\n", index, value, libusb_error_name(transferred));
        return false;
    }
    return true;
}

void EnumerateOnly(libusb_context* ctx) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        std::printf("libusb_get_device_list failed: %s\n", libusb_error_name(static_cast<int>(count)));
        return;
    }

    bool foundPhone = false;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != kXiaomiVid && desc.idVendor != kGoogleVid) continue;

        foundPhone = true;
        std::printf("Found candidate device: VID=0x%04X PID=0x%04X (bus %d, addr %d)\n", desc.idVendor,
                    desc.idProduct, libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]));
        if (IsAccessoryModePid(desc.idProduct)) {
            std::printf("  -> AOA accessory-mode PID. 0x2D00=accessory-only, 0x2D01=accessory+adb,\n");
            std::printf("     0x2D04/0x2D05=+audio variants.\n");
        } else {
            std::printf("  -> Normal (pre-accessory) mode.\n");
        }
    }
    if (!foundPhone) {
        std::printf("No Xiaomi (0x2717) or Google-accessory (0x18D1) VID device found.\n");
        std::printf("Is the phone plugged in and USB debugging authorized?\n");
    }

    libusb_free_device_list(list, 1);
}

int RunHandshake(libusb_context* ctx) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        std::printf("libusb_get_device_list failed: %s\n", libusb_error_name(static_cast<int>(count)));
        return 1;
    }

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
        std::printf("No phone found in normal (pre-accessory) mode to handshake with.\n");
        libusb_free_device_list(list, 1);
        return 1;
    }

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(target, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0) {
        std::printf("libusb_open failed: %s\n", libusb_error_name(rc));
        std::printf("Most likely cause: the phone's interface isn't bound to a libusb-usable driver\n");
        std::printf("(WinUSB) yet. That requires Zadig -- deliberately NOT done by this tool.\n");
        return 1;
    }

    unsigned char protocolBuf[2] = {0, 0};
    int transferred = libusb_control_transfer(handle, /*bmRequestType=*/0xC0, kAoaGetProtocol, 0, 0,
                                               protocolBuf, sizeof(protocolBuf), kUsbTimeoutMs);
    if (transferred < 0) {
        std::printf("GetProtocol failed: %s\n", libusb_error_name(transferred));
        libusb_close(handle);
        return 1;
    }
    uint16_t protocol = static_cast<uint16_t>(protocolBuf[0] | (protocolBuf[1] << 8));
    std::printf("AOA protocol version: %u\n", protocol);
    if (protocol == 0) {
        std::printf("Device does not support AOA.\n");
        libusb_close(handle);
        return 1;
    }

    using namespace phonecam::aoa;
    bool ok = true;
    ok &= SendIdString(handle, 0, kManufacturer);
    ok &= SendIdString(handle, 1, kModel);
    ok &= SendIdString(handle, 2, kDescription);
    ok &= SendIdString(handle, 3, kVersion);
    ok &= SendIdString(handle, 4, kUri);
    ok &= SendIdString(handle, 5, kSerial);
    if (!ok) {
        libusb_close(handle);
        return 1;
    }

    std::printf("Identification strings sent. Sending ACCESSORY_START...\n");
    transferred = libusb_control_transfer(handle, 0x40, kAoaStart, 0, 0, nullptr, 0, kUsbTimeoutMs);
    if (transferred < 0) {
        std::printf("ACCESSORY_START failed: %s\n", libusb_error_name(transferred));
        libusb_close(handle);
        return 1;
    }
    libusb_close(handle);

    std::printf("ACCESSORY_START sent. Phone should disconnect and re-enumerate as VID 0x18D1.\n");
    std::printf("Waiting up to 10s for re-enumeration (this can take longer than the AOA spec's\n");
    std::printf("usual few seconds -- confirmed empirically this phase)...\n");

    // --handshake's only job now is switching the phone into accessory mode and confirming it --
    // it used to also run ReadCheckpointLoop, but that tested AoaAccessoryTransport's throwaway
    // checkpoint pattern, which the real AoaTransport (now what MainActivity opens on accessory
    // attach) never sends. Run --read-only or --test-video as a separate step afterward.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        libusb_device** list = nullptr;
        ssize_t count = libusb_get_device_list(ctx, &list);
        bool found = false;
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
            if (desc.idVendor == kGoogleVid && IsAccessoryModePid(desc.idProduct)) {
                std::printf("Re-enumerated as VID=0x%04X PID=0x%04X\n", desc.idVendor, desc.idProduct);
                found = true;
                break;
            }
        }
        libusb_free_device_list(list, 1);
        if (found) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::printf("Device did not re-enumerate as an AOA accessory PID within the wait.\n");
    return 1;
}

// Finds the re-enumerated accessory-mode device, opens it, and claims its AOA accessory
// interface (distinguished from an "accessory,adb" composite config's other interface by class
// triple -- accessory is 0xFF/0xFF/0x00, adb is 0xFF/0x42/0x01, see source.android.com's AOA
// docs -- NOT by assuming interface 0, which isn't guaranteed and was a real bug here once).
// Shared by ReadCheckpointLoop and RunTestVideo so both open the device the same, already-
// debugged way.
bool OpenAccessoryInterface(libusb_context* ctx, libusb_device_handle** outHandle, int* outInterfaceNum,
                             uint8_t* outBulkIn, uint8_t* outBulkOut) {
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        std::printf("libusb_get_device_list failed: %s\n", libusb_error_name(static_cast<int>(count)));
        return false;
    }

    libusb_device* accessory = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor == kGoogleVid && IsAccessoryModePid(desc.idProduct)) {
            accessory = list[i];
            std::printf("Re-enumerated as VID=0x%04X PID=0x%04X\n", desc.idVendor, desc.idProduct);
            break;
        }
    }
    if (!accessory) {
        std::printf("Device did not re-enumerate as an AOA accessory PID within the wait.\n");
        libusb_free_device_list(list, 1);
        return false;
    }

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(accessory, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0) {
        std::printf("libusb_open (accessory) failed: %s\n", libusb_error_name(rc));
        return false;
    }

    libusb_config_descriptor* config = nullptr;
    rc = libusb_get_active_config_descriptor(accessory, &config);
    if (rc != 0) {
        std::printf("libusb_get_active_config_descriptor failed: %s\n", libusb_error_name(rc));
        libusb_close(handle);
        return false;
    }

    std::printf("Device has %d interface(s):\n", config->bNumInterfaces);
    int accessoryInterfaceNum = -1;
    uint8_t bulkInEndpoint = 0;
    uint8_t bulkOutEndpoint = 0;
    for (int i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface_descriptor& iface = config->interface[i].altsetting[0];
        std::printf("  interface %d: class=0x%02X subclass=0x%02X protocol=0x%02X\n", iface.bInterfaceNumber,
                    iface.bInterfaceClass, iface.bInterfaceSubClass, iface.bInterfaceProtocol);
        bool isAccessory = iface.bInterfaceClass == 0xFF && iface.bInterfaceSubClass == 0xFF &&
                            iface.bInterfaceProtocol == 0x00;
        uint8_t candidateBulkIn = 0;
        uint8_t candidateBulkOut = 0;
        for (int e = 0; e < iface.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = iface.endpoint[e];
            bool isBulk = (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
            bool isIn = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
            std::printf("    endpoint 0x%02X: %s %s wMaxPacketSize=%u\n", ep.bEndpointAddress,
                        isBulk ? "bulk" : "other", isIn ? "IN" : "OUT", ep.wMaxPacketSize);
            if (isBulk && isIn) candidateBulkIn = ep.bEndpointAddress;
            if (isBulk && !isIn) candidateBulkOut = ep.bEndpointAddress;
        }
        if (isAccessory) {
            std::printf("    -> this is the AOA accessory interface\n");
            accessoryInterfaceNum = iface.bInterfaceNumber;
            bulkInEndpoint = candidateBulkIn;
            bulkOutEndpoint = candidateBulkOut;
        }
    }
    libusb_free_config_descriptor(config);

    if (accessoryInterfaceNum < 0) {
        std::printf("No interface matched the AOA accessory class triple (0xFF/0xFF/0x00).\n");
        libusb_close(handle);
        return false;
    }

    rc = libusb_claim_interface(handle, accessoryInterfaceNum);
    if (rc != 0) {
        std::printf("libusb_claim_interface(%d) failed: %s\n", accessoryInterfaceNum, libusb_error_name(rc));
        libusb_close(handle);
        return false;
    }

    if (bulkInEndpoint == 0) {
        std::printf("No bulk IN endpoint found on the accessory interface.\n");
        libusb_release_interface(handle, accessoryInterfaceNum);
        libusb_close(handle);
        return false;
    }

    *outHandle = handle;
    *outInterfaceNum = accessoryInterfaceNum;
    *outBulkIn = bulkInEndpoint;
    *outBulkOut = bulkOutEndpoint;
    return true;
}

// This is meant to run in the SAME process invocation as the handshake above, immediately after
// ACCESSORY_START, specifically so there's no gap where a second process would have to re-find
// and re-open the device -- keeping it all in one run avoids any question of whether the phone's
// accessory-open timeout (Android reverts if nothing calls openAccessory() in time) could expire
// between two separate probe invocations.
int ReadCheckpointLoop(libusb_context* ctx) {
    libusb_device_handle* handle = nullptr;
    int accessoryInterfaceNum = -1;
    uint8_t bulkInEndpoint = 0;
    uint8_t bulkOutEndpoint = 0;
    if (!OpenAccessoryInterface(ctx, &handle, &accessoryInterfaceNum, &bulkInEndpoint, &bulkOutEndpoint)) {
        return 1;
    }
    int rc = 0;

    std::printf("Reading from bulk IN endpoint 0x%02X for up to 60s (waiting for the app to\n", bulkInEndpoint);
    std::printf("auto-launch and call openAccessory())...\n");

    unsigned char buf[512];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool gotData = false;
    int lastLoggedError = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        int actual = 0;
        rc = libusb_bulk_transfer(handle, bulkInEndpoint, buf, sizeof(buf), &actual, /*timeout=*/1000);
        if (rc == 0 && actual > 0) {
            gotData = true;
            std::printf("Received %d bytes: ", actual);
            for (int i = 0; i < actual; ++i) {
                unsigned char c = buf[i];
                std::putchar((c >= 32 && c < 127) ? c : '.');
            }
            std::printf("\n");
            break;
        }
        // Only LIBUSB_ERROR_NO_DEVICE (unplugged/reverted out of accessory mode) is fatal here.
        // Everything else -- including LIBUSB_ERROR_IO, which is expected before anything on the
        // phone side has opened the accessory pipe (the kernel gadget function isn't "ready" for
        // bulk I/O until userspace calls openAccessory()) -- just means "nothing to read yet,
        // keep waiting for the app to launch." Bailing on the first IO error was the bug that
        // made the previous run miss a checkpoint write that arrived ~24s in.
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            std::printf("Device disappeared (LIBUSB_ERROR_NO_DEVICE) -- did it drop out of accessory mode?\n");
            break;
        }
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            // NOT a stall -- verified via LIBUSB_OPTION_LOG_LEVEL_DEBUG: the completion lands
            // ~30us after submit, far too fast for a real USB round-trip, and libusb_clear_halt
            // itself failed with the same Win32 error 87 (ResetPipe on a pipe that isn't halted
            // legitimately fails). WinUSB is rejecting the read outright because nothing on the
            // phone side has opened the accessory yet -- the f_accessory gadget function's
            // endpoints exist in the descriptor but aren't backed by a live pipe until userspace
            // calls openAccessory(). clear_halt() was the wrong fix: it turned this into a
            // microsecond-scale spin (hundreds of thousands of pointless ResetPipe calls) instead
            // of just waiting. A plain pacing sleep is the correct response here.
            if (rc != lastLoggedError) {
                std::printf("  (still waiting -- last libusb_bulk_transfer status: %s)\n", libusb_error_name(rc));
                lastLoggedError = rc;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (!gotData) {
        std::printf("No data received within 60s.\n");
        libusb_release_interface(handle, accessoryInterfaceNum);
        libusb_close(handle);
        return 1;
    }

    // OUT-direction + multi-packet reassembly proof (advisor-recommended, see
    // docs/architecture.md's Phase 7 section): the IN-direction checkpoint above proves nothing
    // about writing host->phone, nor about payloads larger than one bulk transfer/read() call.
    // Both are required for the real control channel (bidirectional) and video channel (frames
    // far bigger than wMaxPacketSize=512). Verify success via `adb logcat -s PhoneCamAoa` on the
    // phone side (AoaAccessoryTransport's read loop logs what it receives) -- this tool has no
    // return channel of its own to confirm receipt.
    if (bulkOutEndpoint == 0) {
        std::printf("No bulk OUT endpoint found on the accessory interface -- skipping OUT tests.\n");
        libusb_release_interface(handle, accessoryInterfaceNum);
        libusb_close(handle);
        return 0;
    }

    {
        const char* marker = "PHONECAM-HOST-OUT-TEST\n";
        std::vector<unsigned char> frame;
        frame.push_back(kTagTextMarker);
        frame.insert(frame.end(), marker, marker + std::strlen(marker));
        int actual = 0;
        rc = libusb_bulk_transfer(handle, bulkOutEndpoint, frame.data(), static_cast<int>(frame.size()), &actual,
                                   kUsbTimeoutMs);
        std::printf("OUT text-marker test: wrote %d/%zu bytes, status=%s\n", actual, frame.size(),
                    libusb_error_name(rc));
    }

    {
        constexpr uint32_t kPayloadLen = 128 * 1024;  // exceeds wMaxPacketSize=512 by far, and any
                                                       // single Java InputStream.read() chunk
        std::vector<unsigned char> frame;
        frame.push_back(kTagLengthPrefixed);
        frame.push_back(static_cast<unsigned char>(kPayloadLen & 0xFF));
        frame.push_back(static_cast<unsigned char>((kPayloadLen >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>((kPayloadLen >> 16) & 0xFF));
        frame.push_back(static_cast<unsigned char>((kPayloadLen >> 24) & 0xFF));
        frame.reserve(frame.size() + kPayloadLen);
        for (uint32_t i = 0; i < kPayloadLen; ++i) {
            frame.push_back(static_cast<unsigned char>(i & 0xFF));  // deterministic, checkable pattern
        }
        int actual = 0;
        auto start = std::chrono::steady_clock::now();
        rc = libusb_bulk_transfer(handle, bulkOutEndpoint, frame.data(), static_cast<int>(frame.size()), &actual,
                                   /*timeout=*/10000);
        auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("OUT length-prefixed test: wrote %d/%zu bytes in %lldms, status=%s\n", actual, frame.size(),
                    static_cast<long long>(elapsedMs), libusb_error_name(rc));
    }

    // The phone-side reader (AoaAccessoryTransport.runReader) writes an "ACK:..." line back over
    // the same pipe after verifying each test above -- logcat turned out to be unusable for this
    // (MIUI silently drops this app's own Log output while backgrounded), so this closes the loop
    // over USB alone. The heartbeat is still running concurrently and will interleave in this
    // output too -- expected, not a bug.
    std::printf("Reading for up to 5s to catch the phone's ACK line(s) (heartbeat will interleave)...\n");
    {
        unsigned char ackBuf[512];
        auto ackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < ackDeadline) {
            int actual = 0;
            rc = libusb_bulk_transfer(handle, bulkInEndpoint, ackBuf, sizeof(ackBuf), &actual, /*timeout=*/500);
            if (rc == 0 && actual > 0) {
                std::printf("  <- ");
                for (int i = 0; i < actual; ++i) {
                    unsigned char c = ackBuf[i];
                    std::putchar((c >= 32 && c < 127) ? c : '.');
                }
                std::printf("\n");
            }
        }
    }

    libusb_release_interface(handle, accessoryInterfaceNum);
    libusb_close(handle);
    return 0;
}

// Phase 7's Phase-3B-equivalent proof: the real AoaTransport.kt (Android) tags real
// WireFraming-framed H.264 packets as Channel.Video and writes them over the same accessory pipe
// -- this reads them back, strips the 1-byte channel tag and WireFraming's own 20-byte header,
// and appends the raw Annex-B payload to a file, same discriminating check as
// docs/architecture.md's Phase 3B ("if the reassembled file decodes cleanly with ffmpeg, the
// framing is byte-correct end to end"). Requires MainActivity's TEMPORARY AoaTransport test
// wiring to already be running a capture session (see its class doc comment) -- this tool does
// not trigger capture itself, only reads whatever the phone is already sending.
//
// Deliberately reads into a generously-sized buffer and parses from an accumulator, never a
// small/fixed-size read per message -- see AoaAccessoryTransport.kt's doc comment for the
// concrete bug (permanently losing bytes) that shape of bug caused earlier in this phase.
int RunTestVideo(libusb_context* ctx, const std::wstring& outputPath) {
    libusb_device_handle* handle = nullptr;
    int accessoryInterfaceNum = -1;
    uint8_t bulkInEndpoint = 0;
    uint8_t bulkOutEndpoint = 0;
    if (!OpenAccessoryInterface(ctx, &handle, &accessoryInterfaceNum, &bulkInEndpoint, &bulkOutEndpoint)) {
        return 1;
    }

    // ASCII-only paths, per main()'s caller -- narrowed back just for printing, same assumption.
    std::string narrowPathForLog(outputPath.begin(), outputPath.end());

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "Cannot open output file: %s\n", narrowPathForLog.c_str());
        libusb_release_interface(handle, accessoryInterfaceNum);
        libusb_close(handle);
        return 1;
    }
    std::printf("Reading Channel.Video frames for up to 30s, writing Annex-B payload to %s...\n",
                narrowPathForLog.c_str());
    std::fflush(stdout);

    std::vector<unsigned char> accum;
    unsigned char readBuf[16 * 1024];
    int configCount = 0, frameCount = 0, keyframeCount = 0;
    size_t totalPayloadBytes = 0;
    constexpr uint8_t kChannelVideo = 0;
    constexpr uint8_t kChannelControl = 1;
    constexpr size_t kWireHeaderSize = 20;  // WireFraming.kt's fixed header, see docs/wire-protocol.md

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        int actual = 0;
        int rc = libusb_bulk_transfer(handle, bulkInEndpoint, readBuf, sizeof(readBuf), &actual, /*timeout=*/1000);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            std::printf("Device disappeared (LIBUSB_ERROR_NO_DEVICE).\n");
            break;
        }
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (actual > 0) {
            accum.insert(accum.end(), readBuf, readBuf + actual);
        }

        // Consume every complete frame currently at the front of accum, keeping only a genuinely
        // incomplete trailing frame (if any) for the next read to complete.
        size_t pos = 0;
        while (pos < accum.size()) {
            uint8_t tag = accum[pos];
            if (tag == kChannelVideo) {
                if (accum.size() - pos < 1 + kWireHeaderSize) break;  // header not fully arrived yet
                const unsigned char* header = accum.data() + pos + 1;
                // WireFraming layout: [0]=magic(0x9C) [1]=type [2]=flags [3]=reserved
                // [4..7]=seq LE [8..15]=pts_us LE [16..19]=payload_len LE. Keying on the outer
                // Channel tag first, then trusting this header, per the advisor's explicit
                // caution -- never scan for the magic byte itself to resync, which could
                // false-lock onto payload data instead of a real header boundary.
                uint32_t payloadLen = static_cast<uint32_t>(header[16]) | (static_cast<uint32_t>(header[17]) << 8) |
                                       (static_cast<uint32_t>(header[18]) << 16) |
                                       (static_cast<uint32_t>(header[19]) << 24);
                size_t frameTotal = 1 + kWireHeaderSize + payloadLen;
                if (accum.size() - pos < frameTotal) break;  // payload not fully arrived yet

                bool isConfig = header[1] == 0;  // WireFraming.TYPE_CONFIG
                bool keyframe = (header[2] & 0x01) != 0;
                const unsigned char* payload = accum.data() + pos + 1 + kWireHeaderSize;
                out.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(payloadLen));
                totalPayloadBytes += payloadLen;
                if (isConfig) {
                    ++configCount;
                } else {
                    ++frameCount;
                    if (keyframe) ++keyframeCount;
                }
                if ((configCount + frameCount) % 30 == 0) {
                    std::printf("... %d config, %d frames (%d keyframes), %zu payload bytes\n", configCount,
                                frameCount, keyframeCount, totalPayloadBytes);
                    std::fflush(stdout);
                }
                pos += frameTotal;
            } else if (tag == kChannelControl) {
                if (accum.size() - pos < 5) break;
                uint32_t bodyLen = static_cast<uint32_t>(accum[pos + 1]) | (static_cast<uint32_t>(accum[pos + 2]) << 8) |
                                    (static_cast<uint32_t>(accum[pos + 3]) << 16) |
                                    (static_cast<uint32_t>(accum[pos + 4]) << 24);
                if (accum.size() - pos < 5 + bodyLen) break;
                pos += 5 + bodyLen;  // not yet parsed/used by this test -- control isn't wired to AOA yet
            } else {
                ++pos;  // unexpected tag -- drop one byte and resync, matching AoaTransport.kt's reader
            }
        }
        if (pos > 0) {
            accum.erase(accum.begin(), accum.begin() + static_cast<std::ptrdiff_t>(pos));
        }
    }

    std::printf("Done. Total: %d config, %d frames (%d keyframes), %zu payload bytes.\n", configCount, frameCount,
                keyframeCount, totalPayloadBytes);

    libusb_release_interface(handle, accessoryInterfaceNum);
    libusb_close(handle);
    return (configCount > 0 && frameCount > 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "";
    bool handshake = std::strcmp(mode, "--handshake") == 0;
    // The phone doesn't auto-revert out of accessory mode just because the app that had it open
    // closed/died -- only an unplug or a host-side reset does that. --read-only skips
    // re-sending ACCESSORY_START (which would fail anyway; the device isn't in normal mode
    // anymore) and just claims+reads against whatever's already there.
    bool readOnly = std::strcmp(mode, "--read-only") == 0;
    bool testVideo = std::strcmp(mode, "--test-video") == 0;

    libusb_context* ctx = nullptr;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        std::printf("libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }

    // Only for the modes that actually open/claim/transfer -- libusb's Windows backend logs
    // driver-binding state explicitly here (e.g. an interface with no WinUSB driver bound),
    // which is exactly the kind of thing a plain error code doesn't say.
    if (handshake || readOnly) {
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
    }

    int result = 0;
    if (handshake) {
        result = RunHandshake(ctx);
    } else if (readOnly) {
        result = ReadCheckpointLoop(ctx);
    } else if (testVideo) {
        if (argc > 2) {
            std::string narrowPath = argv[2];
            std::wstring outputPath(narrowPath.begin(), narrowPath.end());  // ASCII paths only, fine here
            result = RunTestVideo(ctx, outputPath);
        } else {
            std::fprintf(stderr, "usage: aoa_probe --test-video <output.h264>\n");
            result = 1;
        }
    } else {
        EnumerateOnly(ctx);
    }

    libusb_exit(ctx);
    return result;
}
