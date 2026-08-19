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
(H.264, zero-copy)          / ADB / AOA          H.264 decode              seen by ALL apps
     ▲                                                 │
     └──────────────── control channel (bidirectional) ┘
```

The phone captures and hardware-encodes video, streams it over USB, and a Windows host process decodes it and feeds a registered [`MFCreateVirtualCamera`](https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/) media source — which Windows' Frame Server serves to both Media Foundation apps (Chrome, Windows Camera, new Teams) and DirectShow apps (Zoom, classic OBS) at once. A bidirectional control channel carries camera commands (PC → phone) and capabilities/telemetry (phone → PC).

Three USB transports exist side by side, auto-detected at startup in this order:

- **USB tethering** (the default when available) — flip the phone's USB tethering toggle and the phone presents a network interface, which Windows binds with its own built-in driver. Both video *and* camera controls run over it, with no Developer Options, **no driver to install, and nothing else on the phone displaced**. Measured at 2-5ms round-trip: USB latency, not Wi-Fi latency. This is the path to use.
- **ADB** — needs Developer Options → USB debugging on. The original, fully proven path; still the fallback when tethering isn't available but debugging is.
- **AOA** (Android Open Accessory) — no Developer Options either, but it needs a Windows driver bound to the phone (handled automatically, one admin prompt, no Zadig) which makes file transfer unavailable until reverted, and it carries video only. Kept as the last resort for devices where the carrier gates USB tethering.

Nothing about this is wireless — tethering is a USB cable carrying a network protocol, which is exactly why it keeps USB's latency while needing none of USB's driver machinery.

Full design and phased roadmap: [`docs/architecture.md`](docs/architecture.md).

## Status

Not yet a finished product — actively developed, most of the core pipeline works and has been verified against real hardware, not just in theory.

**Working and verified live**, streamed from a real phone into a real virtual camera visible in Zoom/OBS/Chrome/Windows Camera:
- Full ADB-based pipeline: capture → H.264 encode → USB transport → Media Foundation decode → virtual camera, 1080p30.
- Live camera controls from the PC (zoom, exposure, focus incl. tap-to-focus, torch, lens switch, bitrate) with capability-driven UI (never shows a control the phone doesn't actually support).
- Adaptive bitrate, thermal-aware, with live stats telemetry.
- Reconnect/robustness: survives USB replug and the phone backgrounding without restarting either side.
- **USB tethering transport — the one to use.** No Developer Options, no driver install, no admin prompt, and nothing else on the phone displaced. Flip the phone's USB tethering toggle and open the app; that's the entire setup. Windows binds its own built-in RNDIS driver automatically, and the host discovers the phone deterministically (it's the tethered link's gateway — no IP to type, no pairing). Measured at 2-5ms round-trip. Verified live with USB debugging off, end to end: video *and* controls, 1080p and 720p, with `adb forward --list` empty proving nothing routed through adb.
- **Camera controls work without USB debugging** for the first time, over that same tethered link — the control channel was always just TCP; it only ever used adb to get a route. All seven tray controls populate from the phone's real reported capabilities.
- **Recovers unattended.** Observed following the phone across three different tethered addresses in one session (every USB reconfiguration hands out a new subnet) and surviving USB debugging being toggled off mid-stream — both channels reconnected with no restart, because discovery re-runs per attempt rather than caching.
- AOA transport (also no USB debugging): proven end-to-end, kept as the fallback for devices where a carrier gates USB tethering. Its Windows driver step is one click in the tray behind a single admin prompt — no Zadig, ever — with a matching "Remove USB driver" that fully undoes it. Note the tradeoff tethering avoids entirely: binding that driver makes the phone's file transfer unavailable until reverted, and AOA carries video only, so camera controls don't work over it.

- The virtual camera now genuinely advertises 4 resolutions (1920x1080, 1280x720, and true portrait 1080x1920/720x1280) — the consuming app picks one, same as any real UVC webcam. Confirmed via `ffmpeg -f dshow -list_options` and independently via a native Media Foundation probe.
- Rotation/mirror (0°/90°/180°/270° + mirror), applied PC-side, live from the tray — defaults to 90° clockwise (the reference mounting is the phone held vertically; a landscape sensor frame needs that rotation to look normal). Verified live against the running virtual camera, including the default applying automatically with no tray interaction needed.
- **Known platform limitation, not yet worked around:** the two portrait sizes render correctly in native Media Foundation apps (Chrome, Windows Camera, modern capture backends) but are corrupted by a confirmed bug in Windows' own DirectShow compatibility bridge (`VCAMDS`) — this affects DirectShow-based apps, which includes Zoom's classic Windows client and older/classic OBS. Landscape sizes are unaffected everywhere. See `docs/architecture.md`'s Phase 3 section for the full diagnosis.
- Full tray controls: camera/lens switch, zoom, exposure, torch, focus mode, plus the rotation/mirror above and a read-only resolution display — all real submenus, not stubs, populated from the phone's own reported capabilities (a control never appears unless the phone actually supports it). Resolution is display-only by design: the consuming app picks it, exactly like a real webcam, so you set it in OBS/Zoom/Chrome's own device settings.
- Camera controls need either USB tethering or ADB — they don't work over AOA, which carries video only.

**Not built yet:** a proactive nudge for first-time users (today the tray items are there to click, but nothing prompts you toward them automatically), the installer itself (registers the vcam, bundles what's needed — including the USB driver helper), Wi-Fi as a transport (the network transport already accepts an explicit address, but there's no discovery or UI for it), and synced audio.

See [`docs/architecture.md`](docs/architecture.md) for the full phase-by-phase history, including real bugs found and fixed along the way.

## Project layout

- `android/` — the installed Kotlin/Camera2 app (capture, encode, transport, control, foreground service).
- `windows/common/` — shared C++ static lib: wire framing, shared-memory frame ring, logging.
- `windows/host/` — `phonecam-host.exe`: USB transport (ADB or AOA), Media Foundation decode, virtual-camera control.
- `windows/vcam/` — `phonecam-vcam.dll`: the `MFCreateVirtualCamera` COM media source.
- `windows/tools/aoa_probe/` — standalone diagnostic tool used to develop/debug the AOA transport; not shipped.
- `windows/installer/` — not built yet (see Status above).
- `proto/` — shared FlatBuffers schemas (wire + control protocol) — the contract both sides build against.
- `third_party/libusb/` — vendored libusb, used by the AOA transport and `aoa_probe`.
- `docs/` — architecture (the authoritative, detailed project history), protocol specs, build instructions.

## Building

See [`docs/build.md`](docs/build.md).

## License

[GPL-3.0-or-later](LICENSE).
