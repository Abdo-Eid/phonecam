# Wire protocol

This is the transport-level spec: channels, connection bootstrap, and the exact
byte framing for both the video/audio path and the control path. It is the
implementation source of truth alongside `proto/wire.fbs` and `proto/control.fbs`.

For *what the control messages mean* (Camera2 mapping, synchronization rules),
see [`control-protocol.md`](control-protocol.md).

## Channels

Three logical channels are defined. Video and control are implemented over
ADB-forwarded sockets or USB-tethered TCP; audio remains unimplemented.

| Channel | Direction | Content | Phase |
|---|---|---|---|
| `video` | phone → PC | H.264 Annex-B, framed (below) | supported |
| `control` | bidirectional | FlatBuffers `ControlEnvelope`, length-prefixed | supported |
| `audio` | phone → PC | Opus, framed (same header shape as video) | not implemented |

The PC is always the connecting **client**; the phone's transport listens.

## Connection bootstrap

1. Phone app starts a foreground service that opens `LocalServerSocket`s bound
   to abstract names `phonecam_video` and `phonecam_control`. It also binds TCP
   ports 27183 and 27184 only to an IPv4 address on a detected USB-tethering
   interface (`rndis*`, `usb*`, or `ncm*`); those TCP listeners are refreshed
   as tethering appears or its address changes.
2. For USB debugging, the host runs:
   ```
   adb forward tcp:27183 localabstract:phonecam_video
   adb forward tcp:27184 localabstract:phonecam_control
   ```
3. For USB tethering, Windows identifies the phone as the tethered adapter's
   gateway and connects directly to its control and video TCP ports. There is
   no discovery exchange, QR enrollment, or application-level login.
4. The phone sends `StreamIntent` and `CapabilityDescriptor` at the start of
   each control session. ADB access relies on Android's ADB authorization;
   tethered TCP access relies only on reachability of the USB-tether interface.
5. **Video channel:** phone sends a `CONFIG` packet (SPS/PPS) before the first
   `FRAME`. Host must not attempt to initialize the MF decoder until `CONFIG` is seen.
6. On disconnect, the host tears down both channels and reconnects over the
   selected USB path. It sends `RequestKeyframe` after control reconnects.

The phone listens on both supported USB paths so either can be selected without
restarting the app.

## Video / audio frame framing

Deliberately **not** FlatBuffers: this is the hot path (up to ~60 packets/sec of
compressed video, one syscall's worth of parsing each), so it's a fixed,
hand-rolled 20-byte little-endian header with no schema evolution machinery.
Both the Kotlin sender and the C++ receiver implement this directly (no codegen).

```
offset  size  field         notes
0       1     magic         0x9C  (chosen to be non-ASCII, easy to spot in a hex dump)
1       1     type          0 = CONFIG (SPS/PPS, or Opus header for audio)
                             1 = FRAME
2       1     flags         bit0 = KEYFRAME/IDR (always set for CONFIG); bits1-7 reserved = 0
3       1     reserved      = 0
4       4     seq           u32 LE, monotonic per channel, wraps at 2^32
8       8     pts_us        u64 LE, capture presentation timestamp, microseconds,
                             monotonic clock (Android SystemClock.elapsedRealtimeNanos()/1000
                             at capture time — NOT wall clock)
16      4     payload_len   u32 LE, length of the payload that follows
20      *     payload       `payload_len` bytes: Annex-B H.264 (start-code-prefixed
                             NALUs) for video CONFIG/FRAME; Opus packet bytes for audio
```

Rules:
- `CONFIG` payload is Annex-B `SPS` immediately followed by `PPS` (each with its
  `00 00 00 01` start code); the decoder is (re)initialized from this and only this.
- A new `CONFIG` may arrive mid-stream (e.g. after `SetResolution`) — the host
  must reinitialize the decoder and expect a following `FRAME` with `KEYFRAME` set.
- `seq` gaps indicate dropped frames or a stream discontinuity.
- Multi-NALU frames (e.g. a keyframe carrying SEI) are sent as one `FRAME`
  packet containing multiple concatenated Annex-B NALUs in `payload`.

## Control channel framing

Every message is a **4-byte little-endian length prefix** followed by exactly
that many bytes of a FlatBuffers-encoded `phonecam.control.ControlEnvelope`
(`proto/control.fbs`). This is a stream socket, not a datagram, so the length
prefix is required for the reader to know where one message ends and the next
begins — FlatBuffers itself is not self-delimiting on a byte stream.

```
offset  size  field
0       4     envelope_len   u32 LE
4       *     envelope       `envelope_len` bytes, FlatBuffers ControlEnvelope
```

Encoders on both sides use `include "wire.fbs"` codegen for
`phonecam::control::ControlEnvelope` — see `docs/build.md` for the flatc
invocation wired into each build.

## USB-tethered access and security

The host discovers the phone from the USB-tethered adapter's gateway and connects
to TCP ports 27184 (control) and 27183 (video). On Android, each listener is
bound to a detected USB-tethering interface address, never a wildcard/Wi-Fi/
cellular address. There is no QR pairing, UDP enrollment, challenge-response,
or application-level authentication. A peer able to reach those ports can send
camera commands and receive video.

Video packets begin directly with the ordinary 20-byte video framing described
above; there is no authentication prelude. Control starts with the phone's
`StreamIntent` and `CapabilityDescriptor` messages. Both channels are plaintext.
See [security.md](security.md) for the trust boundary and accepted limits.

## Versioning

Current protocol: **v8**. v8 removes the former authentication payloads from
`ControlPayload`; the resulting FlatBuffers union ordinals differ from v7. Both
apps must be updated together. See [control-protocol.md](control-protocol.md).

`CapabilityDescriptor.protocol_version` and any future breaking change bump a
single integer. The host refuses to proceed (shows an "update the app on your
phone" style error) if the phone's `protocol_version` is newer than what the
host understands; the phone does the same in reverse if it ever needs to. Until
v1.0 this number may change freely between phases.

**The version number is not optional bookkeeping here.** FlatBuffers assigns
union payload types by declaration order, so *adding a command to
`ControlPayload` renumbers the existing ones*. A host and a phone app built
against different schema versions then interpret each other's payload types
differently -- silently, and in a way that looks like a broken camera rather
than a version mismatch. The host checks the phone's reported version on connect
and logs a specific error either way; bump `protocol_version` (both sides)
whenever the union changes shape.
