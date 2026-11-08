# Current work and remaining checks

Updated 2026-11-08. Main supports **USB tethering and USB debugging (ADB) only**.
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

Evidence and limits: [USB validation](docs/usb-validation-2026-11-08.md).

## Outstanding validation / deployment

- The phone has the updated Debug APK and the PC now has the matching Release
  host. A live control/video check of that **installed Release** host is still
  pending: the user chose to finish installation without another phone check.
  Earlier live results used the checkout Debug host; do not conflate them.
- Physical USB unplug/replug and tether-address changes on the USB-only build.
- Simultaneous Wi-Fi/tether isolation, debugging-off tethering, more OEM device
  interface names, physical Recents swipe and screen-lock lifecycle checks.
- A fresh Windows account/install/upgrade and broader device coverage.
- Known portrait corruption in Windows' DirectShow bridge; no workaround yet.

These are pending checks, not active device/network jobs or completed claims.

## Performance and future work

Retain 1080p30/6 Mbps defaults: the prior encoder/profile sweeps established no
end-to-end improvement that justified changing them. Detailed measurements and
unresolved optical/quality/continuity questions are in
[bench/results.md](bench/results.md). GPU zero-copy, slices, 60 fps and audio are
not implemented/validated improvements.

The hotspot investigation is **closed**: it improved subjectively but remained
too laggy, and main no longer includes wireless transport. See
[networking.md](networking.md). Do not automatically run the archived collectors
or revive that work. Raw logs, images, backups and private device/network
metadata stay in ignored local artifact directories.
