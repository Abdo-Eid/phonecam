#pragma once

// The six AOA identification strings (manufacturer/model/description/version/uri/serial), sent
// via ACCESSORY_SEND_STRING during the handshake (see aoa_probe's RunHandshake and the future
// AoaTransport::Connect). These must match android/app/src/main/res/xml/accessory_filter.xml
// EXACTLY -- Android's accessory auto-launch intent is matched by manufacturer+model+version, and
// a mismatch fails silently (no error, the intent just never fires). This header is the source of
// truth; accessory_filter.xml is a hand-maintained mirror of it, called out as such in a comment
// there.

namespace phonecam::aoa {

constexpr const char* kManufacturer = "PhoneCam";
constexpr const char* kModel = "PhoneCam USB Webcam";
constexpr const char* kDescription = "Turns this phone into a USB webcam";
constexpr const char* kVersion = "0.7.0";
constexpr const char* kUri = "https://github.com/abdoeid/phonecam";
constexpr const char* kSerial = "phonecam-0001";

}  // namespace phonecam::aoa
