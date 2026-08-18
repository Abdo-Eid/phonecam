# PhoneCam

![PhoneCam](assets/logo_full.png)

Turn an Android phone into a fast, high-quality **USB webcam** on Windows — recognized as a real camera device by every app (Zoom, Teams, Chrome, Windows Camera, OBS), not just an OBS plugin.

USB only. No root. Free, open-source, no watermark.

## Why

Every phone-as-webcam app (DroidCam, Iriun, Camo, iVCam) uses the same underlying trick: a companion PC driver that registers a virtual camera, fed by a stream from a phone-side app over USB. There's no shortcut around this on a non-rooted phone — Android only lets the phone *itself* enumerate as a real USB camera (`DeviceAsWebcam`) on Android 14+ with OEM support, which most phones (including the reference device for this project, a Redmi Note 8 on Android 11) don't have.

So PhoneCam plays that same architecture straight, and competes on what the paid incumbents under-serve or paywall:

- **All camera controls free** — exposure, focus (incl. tap-to-focus), zoom, torch, white balance, lens switch — driven live from the PC.
- **Synced audio** over the same USB cable (native Android UVC webcam mode is video-only).
- **A friendlier USB path** — Android Open Accessory (AOA) instead of forcing users through Developer Options → USB debugging.
- **Genuinely low latency** — hardware H.264, zero-copy capture, low-latency decode; targeting sub-150ms glass-to-glass.
- **No watermark, no paywall, no subscription.**

## Architecture

```
Android app                    USB                  Windows host              Virtual camera
Camera2 → MediaCodec   ──►  ADB-forward   ──►   Media Foundation    ──►   MFCreateVirtualCamera
(H.264, zero-copy)          (AOA later)         H.264 decode              seen by ALL apps
     ▲                                                 │
     └──────────────── control channel (bidirectional) ┘
```

The phone captures and hardware-encodes video (and eventually audio), streams it over USB, and a Windows host process decodes it and feeds a registered [`MFCreateVirtualCamera`](https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/) media source — which Windows' Frame Server serves to both Media Foundation apps (Chrome, Windows Camera, new Teams) and DirectShow apps (Zoom, classic OBS) at once. A bidirectional control channel carries camera commands (PC → phone) and capabilities/telemetry (phone → PC).

Full design and phased roadmap: [`docs/architecture.md`](docs/architecture.md).

## Status

Early development. See [`docs/architecture.md`](docs/architecture.md) for the current phase.

## Project layout

- `android/` — the installed Kotlin/Camera2 app (capture, encode, transport, control, foreground service).
- `windows/common/` — shared C++ static lib: wire framing, shared-memory frame ring, logging.
- `windows/host/` — `phonecam-host.exe`: USB transport, Media Foundation decode, virtual-camera control.
- `windows/vcam/` — `phonecam-vcam.dll`: the `MFCreateVirtualCamera` COM media source.
- `windows/installer/` — installer (registers the vcam DLL, bundles adb).
- `proto/` — shared FlatBuffers schemas (wire + control protocol) — the contract both sides build against.
- `docs/` — architecture, protocol specs, build/install instructions.

## Building

See [`docs/build.md`](docs/build.md) (once toolchain scaffolding lands).

## License

[GPL-3.0-or-later](LICENSE).
