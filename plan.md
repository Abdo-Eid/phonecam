# Current work and remaining checks

Updated 2026-11-25. Main supports **USB tethering and USB debugging (ADB) only**.
The old experiment/migration checklist has been distilled; its full text remains
at [commit 253bd2d](https://github.com/Abdo-Eid/phonecam/blob/253bd2d/plan.md).

## Implemented and checked

- Removed AOA, Wi-Fi/RTP, QR pairing and app-layer authentication. Tethered TCP
  listeners bind to USB-tether interface addresses; both endpoints speak v8.
- Android background operation is limited to connected streaming. Leaving
  idle/waiting/retry shuts down its service; losing a hidden stream exits too.
  Persistent user Stop remains separate from this lifecycle cleanup.
- Android Debug builds/JVM tests and Windows Debug builds/local tests passed.
  ADB and tethered 1080p/30, transport switching, black output after Stop and
  Stop persistence were checked on the Redmi Note 8 / Android 11.
- Four device lifecycle regressions passed three consecutive runs. Connected
  streaming survived Home/rebind; hidden service exit after host loss passed.
- Installed the matching Windows Release host after rebuilding and passing its
  local tests. Hash verification, the installed executable's DirectShow test
  pattern and preservation of existing service/DLL/settings passed.

- 2026-11-25: native window with live preview and crop box, shapes the virtual camera
  offers as its only size, rotation that follows the phone, saved settings, presets,
  camera replay and a single running host. No protocol or ring-layout change; the ring
  service is unchanged. Offline tests passed in Debug and Release; phone JVM tests passed.
  Live over ADB on the Redmi Note 8 with the **build-folder Release host**, the updated
  phone app and the updated registered DLL: all ten sizes clean through FFmpeg
  DirectShow (synthetic pattern), a shape change reaching newly opened apps while an
  open one kept 360 frames in 12 s, square output with picture at all four edges, saved
  lens/zoom/exposure replayed in order. The user confirmed cropping, turning the phone,
  and square video in OBS and Cap.
- 2026-11-25, later: the same Release host was installed over the one in Program Files
  (hash verified; the old one kept as `phonecam-host.previous.exe`, the old DLL as
  `phonecam-vcam.previous.dll`). Started from the Start menu shortcut it loaded the saved
  square shape, offered apps only 1152×1152, and a second launch did not start a second
  copy. The phone had been unplugged by then, so **video through the installed copy has
  not been checked**; the identical file streamed from the build folder just before.
  Not exercised in either copy: tethering, front-camera rotation, tall video inside an app, saving and
  deleting presets by hand, other DPI settings or monitors.

Evidence and limits: [USB validation](docs/usb-validation-2026-11-08.md).

## Outstanding validation / deployment

- The phone has the updated Debug APK and the PC has the matching Release host and
  virtual-camera DLL. A live control/video check of that **installed** host is still
  pending. Earlier live results used checkout hosts; do not conflate them.
- The 0.9.0 installer was rebuilt and run silently on the development PC as an update over
  a running PhoneCam: files replaced (hashes match the builds), the stopped ring service
  started, `libusb-1.0.dll` removed, the AOA driver tool kept because drivers are still
  recorded there. Interactive setup, a first install, uninstall, Windows 10 refusal and
  another PC are not checked.
- Physical USB unplug/replug and tether-address changes on the USB-only build.
- Simultaneous Wi-Fi/tether isolation, debugging-off tethering, more OEM device
  interface names, physical Recents swipe and screen-lock lifecycle checks.
- A fresh Windows account/install/upgrade and broader device coverage.

These are pending checks, not active device/network jobs or completed claims.

## Performance and future work

Retain 1080p30/6 Mbps defaults: the prior encoder/profile sweeps established no
end-to-end improvement that justified changing them. Cropping is the new reason to
capture more: the Redmi Note 8 lists 3840×2160, 2592×1944 and 4:3 camera sizes such as
1600×1200 (encoder and 30 fps support unchecked). It needs a phone-side reconfigure that
does not stop the service; the ring holds frames up to 1920×1080 pixels' worth, so
1600×1200 already fits and larger ones could be cropped before publishing. Detailed
measurements and unresolved optical/quality/continuity questions are in
[bench/results.md](bench/results.md). GPU zero-copy, slices, 60 fps and audio are
not implemented/validated improvements.

The hotspot investigation is **closed**: it improved subjectively but remained
too laggy, and main no longer includes wireless transport. See
[networking.md](networking.md). Do not automatically run the archived collectors
or revive that work. Raw logs, images, backups and private device/network
metadata stay in ignored local artifact directories.
