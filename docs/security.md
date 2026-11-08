# Security and trust boundary

## Connection model

PhoneCam supports two USB paths:

- **USB tethering:** the phone's control (TCP 27184) and video (TCP 27183)
  listeners bind only to IPv4 addresses on interfaces whose names begin with
  `rndis`, `usb`, or `ncm`. The listeners never bind to wildcard addresses,
  Wi-Fi, or cellular interfaces. They refresh their address as tethering becomes
  available or changes.
- **USB debugging (ADB):** the phone exposes abstract local sockets reached via
  `adb forward`. Access is subject to Android's ADB authorization and the host's
  access to the authorized ADB device.

There is **no QR pairing, remembered application credential, or application-level
authentication** on either path. Once a client can reach the tethered TCP ports,
it can issue camera commands and receive video. Binding to the USB-tethering
interface limits where the ports are reachable; it does not authenticate the PC
or make the traffic confidential.

## What the USB boundary does and does not provide

- **Plaintext:** control messages and H.264 video are not encrypted. A system
  with access to the tethered link can observe them.
- **No application-level access control:** the tethered TCP services do not
  challenge clients, verify identity, or require a pairing secret. Do not expose
  the tethered network through bridging, forwarding, or network sharing to
  untrusted devices.
- **No integrity or MITM protection:** commands and media packets are not
  cryptographically signed or authenticated. Active injection or modification
  is not prevented.
- **ADB remains separately authorized:** ADB forwarding relies on Android's
  debugging authorization and is not an app-layer login.
- **No endpoint protection or denial-of-service guarantee:** a compromised
  phone/PC or a peer with access to the tethered link can disrupt operation or
  access the camera stream.

Use a trusted phone, PC, USB cable, and USB-tethered link. USB tethering is
preferred for ordinary use; disable tethering or stop PhoneCam when the link
should no longer expose the listeners. USB-debugging users should disable
debugging when they no longer need it.

## Protocol compatibility

The current control protocol is **v8**. Both endpoints must use compatible
versions because FlatBuffers union members are assigned by declaration order.
Removing the former authentication payloads changed those ordinals; the host
checks the version advertised by the phone and refuses mismatches.

## Verification

`TetheredTcpListenerTest` checks that only the supported USB interface-name
prefixes are selected and that common Wi-Fi, cellular, Ethernet, and loopback
names are excluded. This unit test does not replace end-to-end verification on a
phone: a real tethered phone is needed to confirm interface discovery, bind
address changes, and control/video reachability. ADB lifecycle and transport
tests are separate from this interface-binding check.
