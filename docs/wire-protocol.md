# Wire protocol

This is the transport-level spec: channels, connection bootstrap, and the exact
byte framing for both the video/audio path and the control path. It is the
implementation source of truth alongside `proto/wire.fbs` and `proto/control.fbs`.

For *what the control messages mean* (Camera2 mapping, synchronization rules),
see [`control-protocol.md`](control-protocol.md).

## Channels

Three logical channels. In the Phase 3 (ADB) transport each is a separate
socket; in the Phase 7 (AOA) transport all three are multiplexed over one pair
of USB bulk endpoints with a 1-byte channel tag per packet (`phonecam.wire.Channel`
in `wire.fbs`).

| Channel | Direction | Content | Phase |
|---|---|---|---|
| `video` | phone → PC | H.264 Annex-B, framed (below) | 3 |
| `control` | bidirectional | FlatBuffers `ControlEnvelope`, length-prefixed | 3 |
| `audio` | phone → PC | Opus, framed (same header shape as video) | 8 |

The PC is always the connecting **client**; the phone's transport listens.

## Connection bootstrap (ADB transport, Phase 3)

1. Phone app starts a foreground service that opens two `LocalServerSocket`s
   bound to abstract names `phonecam_video` and `phonecam_control`.
2. PC host locates a working `adb` (bundled copy in `third_party/platform-tools`,
   falling back to one on `PATH`), confirms exactly one authorized device, and runs:
   ```
   adb forward tcp:27183 localabstract:phonecam_video
   adb forward tcp:27184 localabstract:phonecam_control
   ```
3. Host connects as a TCP client to both `127.0.0.1` ports.
4. **Control channel handshake:** phone immediately sends a `ControlEnvelope`
   wrapping `CapabilityDescriptor` with `cmd_id = 0`. The host does not consider
   the connection ready, and shows no controls, until this arrives.
5. **Video channel:** phone sends a `CONFIG` packet (SPS/PPS) before the first
   `FRAME`. Host must not attempt to init the MF decoder until `CONFIG` is seen.
6. On any disconnect, host tears down both sockets, re-runs `adb forward`, and
   reconnects. `RequestKeyframe` is sent once control reconnects, so the video
   channel doesn't need to carry a keyframe-on-connect guarantee itself.

The AOA transport (Phase 7) replaces steps 1–3 with the AOA accessory handshake
(`ACCESSORY_START` + bulk endpoints) but preserves steps 4–6 unchanged — this is
why the transport sits behind one `ITransport` interface (host) / `Transport`
interface (Android): everything above the socket is transport-agnostic.

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
- `seq` gaps indicate drops (relevant transports here don't silently drop, but
  the field exists for the AOA transport and for diagnostics either way).
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

## Video over RTP/UDP (the wireless transport)

The transports above deliver the video channel over a reliable byte stream. The
wireless transport doesn't: it carries H.264 as **RTP (RFC 3550) over UDP**,
packetized per **RFC 6184**.

Why: for a live camera, a late frame is worth less than a dropped one. TCP's
retransmit and head-of-line blocking turn one lost Wi-Fi frame into a latency
spike that can outlast a whole GOP. UDP lets the receiver drop what it must
and ask for a fresh keyframe instead. The control channel stays TCP (it was
always just TCP -- adb only ever provided a route to it).

| | |
|---|---|
| Ports | video: **UDP 27185** on the PC (the PC binds it), control: TCP 27184 to the phone |
| Packetization | single NAL unit packets; FU-A for NALs larger than 1400 bytes |
| Payload type | 96 (dynamic), 90 kHz clock |
| Marker bit | set on the last packet of every access unit |
| Loss handling | a sequence gap drops the in-progress access unit and asks the phone for a keyframe (`RequestKeyframe` over the control channel) |

Each encoder output buffer is one access unit, so the packetizer's call
boundaries are access-unit boundaries by construction -- the receiver never has
to guess where a frame ends. The receiver reassembles access units, detects a
new picture by its first macroblock, and hands whole access units to the
decoder, exactly as the framed transports do.

The PC is the RTP *receiver* and the phone the *sender*. The phone learns the
PC's address from the control connection itself (see
[`control-protocol.md`](control-protocol.md)'s `StartVideo`), so there is no
address to type on the phone and no discovery protocol to speak of.

## Discovery (finding the phone on a network)

The wireless transports need the phone's address. Rather than making the user type
an IP, the PC asks for it: it broadcasts a probe and the phone answers.

Request/response, not a beacon: the PC asks only when it needs an address, so a
phone that isn't being used costs nothing on the network.

```
PC  -> phone:  "PHONECAM_DISCOVER\n"          (UDP broadcast, port 27186)
phone -> PC:   "PHONECAM_HERE <version> <control-port> <video-port>\n"
```

The phone's address is taken from the reply's *source address*, not from the
payload — a phone can't claim to be somewhere it isn't. The probe goes to the
limited broadcast (255.255.255.255) and to every adapter's directed broadcast
(192.168.137.255 on a hotspot, for instance); both are needed, because which one
reaches the peer depends on the adapter.

The reply carries the control and video ports and the protocol version, so the PC
doesn't hardcode them and can tell a mismatched pair apart from a working one.

## Session authentication (pairing)

Over a network, anyone on the link can reach the phone's control port. The phone
therefore challenges, and the PC proves it knows the shared **pairing code**:

```
phone -> PC:  AuthChallenge { nonce }         (a fresh random nonce)
PC  -> phone: AuthResponse { nonce, hmac }     hmac = HMAC-SHA256(code, phone_nonce + "|" + pc_nonce)
```

Both nonces are fresh per session, so a recorded exchange cannot be replayed, and
the code itself never crosses the wire — only a keyed hash of it.

The phone shows its pairing code in the app; the PC is given it with
`--secret <code>` (or the `PHONECAM_SECRET` environment variable, which keeps it
out of the process list).

**Only network sessions are challenged.** A session over the adb-forwarded
abstract socket is the local USB link, which the phone already trusts, and
asking for a code there would be friction for no gain.

**Network video access (protocol v4).** The framed-TCP video socket separately
requires a fresh HMAC proof bound to the current authenticated control session
and its peer. Before the existing video framing, the phone sends `PCV1` plus a
16-byte nonce; the PC returns 64 lowercase ASCII hex bytes of
`HMAC-SHA256(code, "phonecam-video-v1|" + phoneNonce + "|" + pcNonce + "|" + videoNonceHex)`.
An acceptance byte `0x01` precedes the first ordinary video packet. Missing/wrong,
replayed, or old-session proofs are closed without media. ADB/AOA stay unchanged.

**Video remains unencrypted by explicit product decision.** Connection access
control does not prevent interception or active MITM, authenticate the phone to
the PC, or authenticate each subsequent command/media packet. Use a trusted
network. Full protection model, lifecycle, limits, and test procedure:
[security.md](security.md).

## Versioning

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
than a version mismatch. This was hit for real: a host with the wireless
commands talked to a phone app without them, and every message was decoded as
the wrong type. The host now checks the phone's reported version on connect
and logs a specific error either way; bump `protocol_version` (both sides)
whenever the union changes shape.
