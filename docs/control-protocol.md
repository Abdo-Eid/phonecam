# Control protocol

Semantics for every message in `proto/control.fbs`, how each command maps to
the Android Camera2 API, and the synchronization discipline that keeps the PC
UI and the phone's actual camera state from drifting apart. See
[`wire-protocol.md`](wire-protocol.md) for how these messages are framed on
the socket.

## Capability-driven UI (read this first)

The phone is the source of truth for what it can do. On connect it sends
`CapabilityDescriptor` (`wire.fbs`) once, before anything else. **The host UI
is generated from this descriptor — it must never render a control that isn't
backed by a matching capability, and must never send a command the descriptor
doesn't support.** This is what keeps the tool honest on a mid-range
`LIMITED`-hardware-level phone (the reference device, a Redmi Note 8, very
likely lacks `MANUAL_SENSOR` — see `docs/architecture.md` risk R9): if
`has_manual_sensor = false`, the host must gray out / hide `SetManualExposure`,
and if `af_modes` doesn't list a mode, that mode isn't offered.

Concretely, on `CapabilityDescriptor` receipt the host:
1. Builds the lens switcher from `lenses[]`.
2. For the active lens, builds each control widget only if its backing field
   is present/non-empty (e.g. zoom slider bounds = `zoom_ratio_range`; EV
   slider bounds = `ev_compensation_range` × `ev_step`).
3. Re-does step 2 whenever `LensChanged` arrives (a different lens can have
   different capabilities — e.g. front camera often lacks torch).

## Commands (PC → phone)

Each is sent inside a `ControlEnvelope` with a fresh, monotonically increasing
`cmd_id` (never 0 — that value is reserved for unsolicited phone→PC messages).

| Command | Camera2 mapping | Notes |
|---|---|---|
| `Start` / `Stop` | opens/closes the `CameraCaptureSession` and encoder | `Stop` keeps the control socket alive, just halts the video channel |
| `SetResolution` | re-negotiates the `MediaCodec` input `Surface` + repeating request | triggers a new `CONFIG` on the video channel |
| `SetFps` | `CONTROL_AE_TARGET_FPS_RANGE` (fixed range `[fps, fps]`) | clamp to the resolution's `max_fps` from `Resolution` |
| `SetBitrate` | `MediaCodec` dynamic bitrate (`PARAMETER_KEY_VIDEO_BITRATE`) | no re-negotiation needed |
| `SetLens` | closes current session, opens `camera_id` | must be one of `CapabilityDescriptor.lenses[].camera_id` |
| `SetZoomRatio` | `CONTROL_ZOOM_RATIO` | clamp to `zoom_ratio_range` |
| `TapToFocus` | `CONTROL_AF_REGIONS` + `CONTROL_AE_REGIONS` (converted from normalized coords to sensor rect) + `CONTROL_AF_TRIGGER_START` | only sent if the active lens has an AF mode other than `Off` |
| `SetFocusMode` | `CONTROL_AF_MODE` | must be one of `af_modes` |
| `SetEv` | `CONTROL_AE_EXPOSURE_COMPENSATION` | `steps` is in units of `ev_step`; clamp to `ev_compensation_range` |
| `SetManualExposure` | `CONTROL_AE_MODE_OFF` + `SENSOR_SENSITIVITY` (iso) + `SENSOR_EXPOSURE_TIME` | **requires `has_manual_sensor`**; both fields set atomically in one capture request |
| `SetAutoExposure` | `CONTROL_AE_MODE_ON` | reverts `SetManualExposure` |
| `SetWhiteBalanceMode` | `CONTROL_AWB_MODE` | must be one of `awb_modes` |
| `SetManualWhiteBalance` | `CONTROL_AWB_MODE_OFF` + `COLOR_CORRECTION_GAINS` (temperature-to-gains conversion) | only if `AwbMode.Off` is in `awb_modes` |
| `SetTorch` | `FLASH_MODE_TORCH` vs `FLASH_MODE_OFF` | only if `has_torch` |
| `SetStabilization` | `LENS_OPTICAL_STABILIZATION_MODE` (or `CONTROL_VIDEO_STABILIZATION_MODE` if no OIS) | only if `has_optical_stabilization` |
| `RequestKeyframe` | `MediaCodec` `REQUEST_SYNC_FRAME` parameter | sent by host on control-channel (re)connect and whenever a new virtual-camera consumer attaches |

## Telemetry (phone → PC)

| Message | `cmd_id` | Meaning |
|---|---|---|
| `CapabilityDescriptor` | 0 | sent once, first message after connect |
| `Ack` | echoes the command's `cmd_id` | `result` = `Ok` / `Unsupported` / `OutOfRange` / `Busy` / `Error`, with a human-readable `message` for the last two |
| `CurrentSettings` | 0 (unsolicited) or the triggering `cmd_id` | full state snapshot; see reconciliation below |
| `Stats` | 0 | pushed ~1/sec: encode fps, bitrate, dropped frames, `thermal_status` |
| `AfStateChanged` / `AeStateChanged` | 0 | mirrors Camera2 `CaptureResult` AF/AE state, for a focus/exposure indicator in the host UI |
| `LensChanged` | 0 | confirms a `SetLens` took effect (or reflects the phone's default lens right after `Start`) |
| `OrientationChanged` | 0 | sensor-relative rotation; host uses this to rotate the frame it feeds the virtual camera if needed |
| `Error` | 0 or triggering `cmd_id` | unrecoverable-for-this-operation condition, e.g. camera taken by another app |

## Synchronization discipline

1. **Optimistic UI apply.** When the user moves a slider/toggle, the host
   updates the widget immediately (no waiting for a round trip) and sends the
   command.
2. **Correlate by `cmd_id`.** Every sent command's `cmd_id` is tracked until
   its `Ack` arrives (or a timeout — treat as `Error` after ~2s). Out-of-order
   `Ack`s are expected and fine; they're matched by id, not arrival order.
3. **Clamp before sending.** The host never sends a value outside the range
   advertised in `CapabilityDescriptor` — clamp client-side. `Ack{OutOfRange}`
   is a defensive backstop for a stale/mismatched descriptor, not the primary
   validation path.
4. **Debounce continuous controls.** Zoom and EV sliders coalesce to at most
   one in-flight command at a time: if the user drags while a `SetZoomRatio`
   is outstanding, queue only the latest value and send it when the `Ack` for
   the in-flight one arrives.
5. **Reconcile periodically.** `CurrentSettings` arrives after every state
   change and opportunistically (e.g. every few seconds, or after `LensChanged`).
   The host UI always trusts the most recent `CurrentSettings` over its own
   optimistic state — this is what prevents permanent drift if an `Ack` is
   lost or a change happens for a reason other than a direct command (e.g. AF
   re-triggering after a lens switch).
6. **Never send an unsupported command.** Enforced by construction (§
   Capability-driven UI) rather than relying on `Ack{Unsupported}` — the
   latter is a correctness backstop, not a UI mechanism.

## Implementation status (Phase 4)

Implemented and verified against the real device: the whole handshake
(`CapabilityDescriptor` on connect), `SetZoomRatio`, `SetEv`, `SetTorch`,
`SetFocusMode`, `TapToFocus`, `RequestKeyframe`, `SetBitrate`, `SetLens`
(with `LensChanged` + `CurrentSettings` confirmation), and `Ack` for all of
the above. The host side is a console command surface
(`windows/host/control/ConsoleControlUi`), not a GUI yet — see
`docs/architecture.md`'s Phase 4 section for why.

**Not yet implemented** (the phone replies `Ack{Unsupported}`):
`SetResolution`, `SetFps` (blocked on `windows/vcam` only supporting one
fixed stream size — see architecture.md), `SetManualExposure`,
`SetAutoExposure`, `SetWhiteBalanceMode`, `SetManualWhiteBalance`,
`SetStabilization` (deferred by choice since Phase 2, not by device
capability — a live capability probe now shows this device actually
supports `MANUAL_SENSOR` on both lenses), `Start`, `Stop` (video
start/stop is still driven by the phone's own UI, not the control
channel). `Stats` telemetry exists as a method but isn't pushed
periodically yet. The control channel's lifecycle is tied to
`CaptureController`'s (torn down on video Stop, not durable the way this
doc describes) — Phase 5 robustness scope.
