# Closed wireless investigation — distilled record

The investigation ended on 2026-10-22. The user's final verdict was **“better
but still lagging; not worth the effort anymore.”** USB is the dependable path.
Main is now USB-only; RTP, Wi-Fi connection UI, discovery and pairing were
removed. This is historical evidence, not instructions to change network
settings or resume hotspot experiments.

The full 22-section journal, sources, run identifiers and deployment hashes are
preserved at [commit 253bd2d](https://github.com/Abdo-Eid/phonecam/blob/253bd2d/networking.md)
and on the local `archive` branch. No raw artifacts were deleted by distillation.

## What was observed

- Changing the PC's upstream network changed the user's experience even though
  phone-to-PC video still crossed the same PC-hosted hotspot. Association,
  radio concurrency, queueing and drivers remained hypotheses, not a proven
  single cause.
- Initial Network B checks were better than A, but later B hotspot checks also
  degraded. A return to a better idle ping did not establish stable video.
- Router/default DNS did not fix responsiveness once the phone rejoined the
  hotspot. A router b/g/n change raised reported link speed from 54 to 300 Mbps
  but did not resolve poor gateway/internet responsiveness.
- With upstream disconnected and hotspot still on, local ICMP impairment
  persisted while two bounded, 8 Mbps-capped TCP transfers completed. Those
  deliberately capped tests did not measure maximum link capacity or prove
  smooth video.
- The OEM driver and existing power settings were inspected. An approved ax→ac
  trial was reported unchanged by the user; the original ax setting was restored
  and verified. The proposed driver rollback was declined, not performed.
- The earlier [router/hotspot comparison](docs/measurements/2026-10-21-wireless-links.md)
  found lower RTT in that one hotspot setup; it did not contradict later
  instability or establish universal glass-to-glass latency.

## Historical RTP recovery work

Clean-IDR recovery discarded damaged pictures and withheld dependent pictures
until intact SPS/PPS and an IDR first slice. The user's first trial reported no
pixelation but freezes/lag. A gap-only **12 ms / 64-packet** bounded reorder
follow-up was subjectively better, but did not pass smoothness acceptance.

At the old implementation, Debug/Release reorder checks, 17 receiver cases and
six synthetic 1080p image-hash scenarios passed. These tests do not prove that
live gaps were reordering rather than loss, or that the radio was repaired.
The old final Android validation was interrupted and is not retroactively
claimed passed by newer USB-only builds.

The final saved log contained 49 sequence-gap events and 58 skipped sequence
numbers. Its last periodic snapshot reported 8,406 datagrams, 45 loss events,
288 withheld pictures, 0 recovered late packets and 45 expired reorder gaps.
Mixed-duration counters are not packet-loss percentages or comparable trial
rates; no recovered packets does not justify enlarging the deadline without
evidence. No encoder/media default changed as a result.

## Retained evidence and current boundary

- Sanitized performance evidence: [bench/results.md](bench/results.md).
- Original journals and removed receiver implementation: commit `253bd2d`.
- Raw runs, room images, SSIDs, device identifiers, pairing/registry exports and
  backup binaries remain local under ignored `bench/runs/`, `bench/logs/`,
  `bench/e2e/` and `windows/build/updates/`.
- The last old installed host hash was
  `010117C0C6FD63BB8F0083DCACDF8F74D4C3D77DA2D16A8B6D708EC8FD9E367C`.
  That historical deployment is **not** the new USB-only host installation;
  current status is in [USB validation](docs/usb-validation-2026-11-08.md).

Do not change adapters, drivers, network/security settings or start a camera
collector merely by following old scripts. Any new wireless work needs a new
request and a deliberately chosen historical build.
