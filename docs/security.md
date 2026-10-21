# Network access protection

## Scope and explicit decision

PhoneCam requires pairing for network control and authenticates each framed-TCP
video connection. **Video encryption is deliberately not implemented**, at the
user's request (2026-10-21). This closes the direct-connect video socket bypass;
it is not a claim of complete network confidentiality or protection from every
network attack. The execution checklist is in [plan.md](plan.md).

Use a trusted LAN or a personal hotspot. The pairing code is a shared secret,
not a per-PC identity/allow-list. Anyone given the code can act as a paired PC.
It is generated randomly on the phone, stored in private app preferences, and
shown in the app. `--secret CODE` or `PHONECAM_SECRET` supplies it to Windows;
the displayed grouping hyphens are ignored by the host. The environment avoids
putting it directly in command-line arguments, but is not protection from local
process inspection by a sufficiently privileged user.

## Entry points and their gates

| Entry point | Protection |
|---|---|
| Discovery, UDP 27186 | Public probe/reply with protocol and port information only; not authenticated and not proof of the phone's identity. |
| Network control, TCP 27184 on phone | Pairing-code HMAC challenge/response before commands, including `StartVideo`, are allowed. |
| Network video, TCP 27183 on phone | Current authenticated network control session **and** matching peer address **and** a fresh session-bound HMAC proof, before encoder/config/frame bytes. No unauthenticated fallback. |
| RTP video, UDP 27185 on PC by default | Phone sends only after authenticated `StartVideo`, to the control peer's address and requested port. There is no phone-side RTP video socket for viewers to connect to. RTP packets themselves are plaintext and unauthenticated. |
| ADB abstract sockets | Trusted local path, unchanged; requires access to the phone's ADB interface. No network-video bypass is granted by a trusted ADB control session. |
| AOA accessory pipe | Existing local USB video path, unchanged. No network video listener/auth bypass. |

The TCP video listener exists while a framed-stream capture is waiting/running.
Merely reaching a listening port is not authorization. A rejected client may get
the authentication challenge, but never codec configuration or a video packet.
An IP match is an additional restriction, not the credential: another client
sharing an address still needs the secret and current-session proof.

## Control login

1. Phone generates a fresh phone nonce and sends `AuthChallenge`.
2. PC generates its nonce and returns lowercase hex
   `HMAC-SHA256(code, phoneNonce + "|" + pcNonce)` in `AuthResponse`.
3. Phone checks the proof and publishes the authorized network-session context.
   Capabilities follow successful authentication. Normal host startup, watchdog,
   and keyframe requests wait for the compatible capabilities/ready signal.

The phone rejects commands before login. Incorrect proofs close only that client,
not the listeners. A silent control handshake times out (10-second socket read
timeout); listeners remain available for the next attempt. The pairing code
itself is not sent over the network. Nonces/proofs are not secret bearer tokens.

## TCP video connection: protocol v4

Both apps must be rebuilt together. Protocol v4 requires this video prelude;
ordinary video packet framing is unchanged after authentication.

1. Phone looks up the current authorized network control session. A missing
   session or a different peer IP is rejected without a challenge.
2. Phone generates a fresh cryptographically random 16-byte video nonce and sends
   exactly 20 bytes: ASCII `PCV1` followed by the raw nonce.
3. PC requires its current network control session to be ready, then sends exactly
   64 ASCII bytes: lowercase hexadecimal HMAC-SHA256, with the pairing code as key
   and this UTF-8 message (no trailing newline):

   ```text
   phonecam-video-v1|<phone-control-nonce>|<pc-control-nonce>|<video-nonce-hex>
   ```

   The video nonce is 32 lowercase hex characters. The domain prefix prevents a
   control-login proof from serving as a video proof.
4. Phone compares the proof, rechecks the same session is still current and not
   cancelled, and sends a single `0x01` acceptance byte. Only then may ordinary
   video framing start (magic `0x9c`).

Missing, partial, wrong, replayed, or expired-session proofs are closed. A fresh
nonce prevents reusing a proof on another video socket; the control nonces bind
it to one control session. Disconnecting/reconnecting control invalidates the old
context even when the pairing code and IP stay the same.

The phone's video handshake has a two-second overall read deadline, including
partial/slow reads. Rejections return to the accept loop rather than stopping
capture/listening. Host handshake I/O also uses bounded read/write timeouts.
Normal media reads have no authentication timeout and perform no per-frame MAC
or encryption operation. There is connection-setup work/round-trip cost, not
ongoing video encryption cost.

## Session lifetime and pairing-code changes

- Control disconnect clears network video authorization and stops PC-driven
  capture (TCP or RTP). A network video socket used by a phone-started capture is
  also closed when its authorizing control session ends.
- Stopping capture closes its video sockets. A later video connection must pass a
  new challenge; saved proofs cannot reopen it.
- The host clears video authorization on control disconnect/reconnect. It does
  not use credentials from a previous successful connection while a new login
  is pending.
- `NEW` on the phone changes the code used by future logins. It is not a remote
  per-PC revocation list, and existing authorized control sessions are not
  automatically evicted by code regeneration. Disconnect/restart PhoneCam as
  well when immediate revocation is required. A PC already holding the old code
  cannot authenticate a new control session using it.

## Accepted limits: what this does not protect

- **No confidentiality:** TCP/RTP media is plaintext. A device able to intercept
  that traffic can still watch it. This work blocks unauthorized direct video
  connections, not sniffing.
- **No end-to-end integrity:** authenticated control commands afterward and media
  packets are not individually MACed/signed. Active injection/hijacking, MITM,
  real-time relaying, and compromised access points are not solved.
- **No authenticated phone identity:** discovery replies and the phone's identity
  are not verified to the PC by certificate/pinning.
- **No denial-of-service guarantee:** timeouts/recovery bound an individual video
  handshake, but flooding connections or saturating the network can disrupt use.
- **No protection from compromised endpoints or a stolen/shared pairing code.**
- WPA2/WPA3 protects a radio link; it is not a substitute for end-to-end encryption
  against an active network peer/access point. Do not claim every other LAN user
  is categorically unable to observe unencrypted traffic.

## Verification

Eight automated JVM socket tests exercise absent control authorization, wrong peer,
wrong proof followed by success, fresh-connection replay, replaced control
session, revocation during handshake, silent/partial timeouts, and media gating.
The Windows proof is independently checked against .NET HMACSHA256; Android uses
the same fixed vector:

```text
code:        ABCDEFGHJKLM
phoneNonce:  phone-nonce-123
pcNonce:     pc-nonce-456
videoNonce:  00112233445566778899aabbccddeeff
proof:       be207603bc8e58f89611cf9ff820e3a2d19ca7bb3ff5257550c85a65fe38b95d
```

Commands:

```powershell
# Unit tests
cd android
.\gradlew.bat testDebugUnitTest

# From the repository root, with PhoneCam open and phonecam-host stopped:
# set PHONECAM_SECRET locally to the current pairing code first.
python scripts/test_network_video_auth.py <phone-ip>

windows\build\host\Debug\phonecam-host.exe --test-video-auth ABCDEFGHJKLM phone-nonce-123 pc-nonce-456 00112233445566778899aabbccddeeff
```

The live probe intentionally starts/stops capture and reconnects control. It
checks no video is returned for missing/wrong/replayed/old-session proofs,
checks a valid current session receives framed video, and checks control
disconnect closes video. It never prints the actual pairing code. Three
consecutive final live probe rounds passed on the Redmi Note 8 / Android 11;
normal authenticated Windows TCP video also reached DirectShow. Live normal
streaming/regression results, including the separate intermittent RTP-startup
observation, are recorded in [architecture.md](architecture.md).
