# PhoneCam

![PhoneCam](assets/logo_full.png)

Turn an Android phone into a fast, high-quality **USB webcam** on Windows — recognized as a real camera device by every app (Zoom, Teams, Chrome, Windows Camera, OBS), not just an OBS plugin.

USB only. No root. Free, open-source, no watermark.

## Why

Every phone-as-webcam app (DroidCam, Iriun, Camo, iVCam) uses the same underlying trick: a companion PC driver that registers a virtual camera, fed by a stream from a phone-side app over USB. There's no shortcut around this on a non-rooted phone — Android only lets the phone *itself* enumerate as a real USB camera (`DeviceAsWebcam`) on Android 14+ with OEM support, which most phones (including the reference device for this project, a Redmi Note 8 on Android 11) don't have.

So PhoneCam plays that same architecture straight, and competes on what the paid incumbents under-serve or paywall:

- **All camera controls free** — exposure, focus (incl. tap-to-focus), zoom, torch, white balance, lens switch — driven live from the PC.
- **Synced audio** over the same USB cable (native Android UVC webcam mode is video-only).
- **A genuinely zero-setup USB path** — USB tethering instead of forcing users through Developer Options → USB debugging *or* a driver install. Flip one toggle, open the app. Nothing is installed on Windows, nothing needs admin, and nothing on the phone is displaced.
- **Genuinely low latency** — hardware H.264, zero-copy capture, low-latency decode; targeting sub-150ms glass-to-glass.
- **No watermark, no paywall, no subscription.**

## Architecture

```
Android app                     USB                  Windows host              Virtual camera
Camera2 → MediaCodec   ──►  USB tethering  ──►   Media Foundation    ──►   MFCreateVirtualCamera
(H.264, zero-copy)          or USB debugging     H.264 decode              seen by ALL apps
     └────────────────── control channel (bidirectional) ───────────────────┘
```

The phone captures and hardware-encodes video over USB, and a Windows host process decodes it and feeds a registered [`MFCreateVirtualCamera`](https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/) media source — which Windows' Frame Server serves to both Media Foundation apps (Chrome, Windows Camera, new Teams) and DirectShow apps (Zoom, classic OBS) at once. A bidirectional control channel carries camera commands (PC → phone) and capabilities/telemetry (phone → PC).

PhoneCam supports two USB connections:

- **USB tethering** — enable USB tethering on the phone. Windows uses its built-in network driver; video and camera controls work without Developer Options or a custom driver. This is preferred when the tethered link is available.
- **USB debugging (ADB)** — enable Developer Options → USB debugging. The host uses ADB forwarding over the authorized local ADB connection.

The host automatically selects tethering when available, otherwise it uses ADB. You can also select either path from the Windows tray menu. There is no QR pairing or application-level authentication. Tethered TCP listeners bind only to the phone's USB-tethering interface; ADB keeps its authorized local sockets. **Stop stays stopped until Start**, and the Windows virtual camera remains registered. Tethered control and video are plaintext and unauthenticated, so use a trusted, directly connected USB host.
See [security.md](docs/security.md) for the trust boundary and accepted limits. CLI transport
overrides `--net` and `--adb` remain available for development.

Measured on the reference phone (Redmi Note 8, Android 11), with the phone on the laptop's Windows Mobile Hotspot — one radio hop, not two:

| Transport | Reference-relative timing (not absolute latency) |
|---|---|
| USB (adb) | 1–2 ms |

These figures measure changes in delay relative to a reference frame, not absolute camera-to-PC or glass-to-glass latency. The clock-offset calculation also cancels the reference frame's pipeline delay; absolute latency still needs a separate measurement.

Full design and phased roadmap: [`docs/architecture.md`](docs/architecture.md).
Network trust boundary and accepted plaintext limits: [`docs/security.md`](docs/security.md).

## Status

Not yet a finished product — actively developed, most of the core pipeline works and has been verified against real hardware, not just in theory.

**Working and verified live**, streamed from a real phone into a real virtual camera visible in Zoom/OBS/Chrome/Windows Camera:
- Full ADB-based pipeline: capture → H.264 encode → USB transport → Media Foundation decode → virtual camera, 1080p30.
- Live camera controls from the PC (zoom, exposure, focus incl. tap-to-focus, torch, lens switch, bitrate) with capability-driven UI (never shows a control the phone doesn't actually support).
- Adaptive bitrate, thermal-aware, with live stats telemetry.
- Reconnect/robustness: survives USB replug and the phone backgrounding without restarting either side.
- **USB tethering transport — the one to use.** No Developer Options, no driver install, no admin prompt, and nothing else on the phone displaced. Flip the phone's USB tethering toggle and open the app. Windows binds its built-in RNDIS driver automatically, and the host discovers the phone deterministically (it's the tethered link's gateway — no IP to type). Tethered TCP listeners bind only on the phone's USB-tethering interface; there is no pairing or application-level authentication, and traffic is plaintext. Measured at 2-5ms round-trip. Verified live with USB debugging off, end to end: video *and* controls, 1080p and 720p, with `adb forward --list` empty proving nothing routed through ADB.
- **Camera controls work without USB debugging** for the first time, over that same tethered link — the control channel was always just TCP; it only ever used adb to get a route. All seven tray controls populate from the phone's real reported capabilities.
- **Recovers unattended.** Observed following the phone across three different tethered addresses in one session (every USB reconfiguration hands out a new subnet) and surviving USB debugging being toggled off mid-stream — both channels reconnected with no restart, because discovery re-runs per attempt rather than caching.

- The virtual camera now genuinely advertises 4 resolutions (1920x1080, 1280x720, and true portrait 1080x1920/720x1280) — the consuming app picks one, same as any real UVC webcam. Confirmed via `ffmpeg -f dshow -list_options` and independently via a native Media Foundation probe.
- Rotation/mirror (0°/90°/180°/270° + mirror), applied PC-side, live from the tray — defaults to 90° clockwise (the reference mounting is the phone held vertically; a landscape sensor frame needs that rotation to look normal). Verified live against the running virtual camera, including the default applying automatically with no tray interaction needed.
- **Known platform limitation, not yet worked around:** the two portrait sizes render correctly in native Media Foundation apps (Chrome, Windows Camera, modern capture backends) but are corrupted by a confirmed bug in Windows' own DirectShow compatibility bridge (`VCAMDS`) — this affects DirectShow-based apps, which includes Zoom's classic Windows client and older/classic OBS. Landscape sizes are unaffected everywhere. See `docs/architecture.md`'s Phase 3 section for the full diagnosis.
- Full tray controls: camera/lens switch, zoom, exposure, torch, focus mode, plus the rotation/mirror above and a read-only resolution display — all real submenus, not stubs, populated from the phone's own reported capabilities (a control never appears unless the phone actually supports it). Resolution is display-only by design: the consuming app picks it, exactly like a real webcam, so you set it in OBS/Zoom/Chrome's own device settings.

**Not built yet:** a proactive nudge for first-time users (today the tray items are there to click, but nothing prompts you toward them automatically), hotspot setup automation (the PC can use one, but it doesn't create it), and synced audio.

See [`docs/architecture.md`](docs/architecture.md) for the full phase-by-phase history, including real bugs found and fixed along the way.

## Project layout

- `android/` — the installed Kotlin/Camera2 app (capture, encode, transport, control, foreground service).
- `windows/common/` — shared C++ static lib: wire framing, shared-memory frame ring, logging.
- `windows/host/` — `phonecam-host.exe`: USB transports (ADB and tethered TCP), Media Foundation decode, virtual-camera control.
- `windows/vcam/` — `phonecam-vcam.dll`: the `MFCreateVirtualCamera` COM media source.
- `windows/installer/` — not built yet (see Status above).
- `proto/` — shared FlatBuffers schemas (wire + control protocol) — the contract both sides build against.
- `docs/` — architecture (the authoritative, detailed project history), protocol specs, build instructions.

## Building

See [`docs/build.md`](docs/build.md).

## License

[GPL-3.0-or-later](LICENSE).
