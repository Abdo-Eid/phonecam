# Current plan

## Network video access (2026-10-21)

**Decision:** authenticate network video connections, but **do not encrypt video**.
This is the user's explicit latency/complexity tradeoff, not an encryption task
deferred silently to a later release.

- [x] Close the direct framed-TCP video socket bypass: no camera/config/frame bytes
  until the connecting PC proves authorization.
- [x] Bind that proof to the current authenticated control session, its peer, and
  a fresh video-connection challenge. Do not send the pairing code or a reusable
  bearer token. Reject missing, wrong, replayed, and expired-session proofs.
- [x] Keep listeners alive after rejection, bound handshake waiting, and preserve
  the trusted local ADB/AOA paths. RTP starts only through authenticated control
  and remains directed to that control peer.
- [x] Verify rejection/recovery, legitimate Wi-Fi/TCP streaming, RTP, and USB/ADB;
  record automated and live results.

**Accepted limits:** video remains plaintext (TCP and RTP). This does not stop
traffic interception, a compromised access point, or active MITM/relay attacks.
Control traffic is not encrypted or integrity-protected after login either.
Use a trusted network/hotspot. Do not describe this as end-to-end confidentiality.
There is no per-frame encryption/authentication work in this task.

Implemented as protocol v4. The real Redmi Note 8 passed the live unauthorized,
replay, stale-session, and control-disconnect probes, followed by normal
Wi-Fi/TCP, RTP, and secret-free ADB frame delivery. AOA's code path is unchanged
but was not re-exercised in this test run. Full protection documentation,
accepted limits, and repeatable tests: [security.md](security.md).

## After access control

1. Fix virtual-camera output timestamp/pacing warnings.
2. Investigate the intermittent RTP startup that receives access units but decodes
   no frames until the host is restarted (observed during final regression).
3. Measure absolute glass-to-glass latency; current timing is reference-relative.
4. Simplify remembering/using the phone's pairing code on the PC.
