#!/usr/bin/env python3
"""Summarize one PhoneCam baseline run.

The Android app logs PhoneCamBench lines when log.tag.PhoneCamBench is DEBUG. The host logs
BENCH lines when PHONECAM_BENCH=1. This script only consumes files; it never touches the device
or the host process.

Usage:
  python bench/analyze.py bench/runs/<run>/android.log bench/runs/<run>/host.err.log
"""

from __future__ import annotations

import argparse
import json
import re
from bisect import bisect_left
from pathlib import Path
from typing import Iterable

SENSOR_RE = re.compile(r"PhoneCamBench: sensor ts_ns=(\d+) frame_ns=(\d+)")
ENCODER_RE = re.compile(
    r"PhoneCamBench: encoder_frame seq=(\d+) pts_us=(\d+) output_ns=(\d+) uptime_ms=(\d+) "
    r"dequeue_wait_us=(\d+) write_us=(\d+) size=(\d+) key=(\d+)"
)
HOST_RE = re.compile(
    r"BENCH host ts_us=(\d+) recv_ns=(\d+) decoded_ns=(\d+) ring_ns=(\d+) "
    r"decode_us=(\d+) ring_us=(\d+)"
)


def parse_android(path: Path) -> tuple[list[dict], list[dict]]:
    sensors: list[dict] = []
    frames: list[dict] = []
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if match := SENSOR_RE.search(line):
            sensors.append(
                {
                    "ts_ns": int(match.group(1)),
                    "frame_ns": int(match.group(2)),
                    "ae_fps_range": line.split(" ae_fps=", 1)[1].strip() if " ae_fps=" in line else "unknown",
                }
            )
        elif match := ENCODER_RE.search(line):
            frames.append(
                {
                    "seq": int(match.group(1)),
                    "pts_us": int(match.group(2)),
                    "output_ns": int(match.group(3)),
                    "uptime_ms": int(match.group(4)),
                    "dequeue_wait_us": int(match.group(5)),
                    "write_us": int(match.group(6)),
                    "size": int(match.group(7)),
                    "key": bool(int(match.group(8))),
                }
            )
    return sensors, frames


def parse_host(path: Path) -> list[dict]:
    frames: list[dict] = []
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if match := HOST_RE.search(line):
            frames.append(
                {
                    "ts_us": int(match.group(1)),
                    "recv_ns": int(match.group(2)),
                    "decoded_ns": int(match.group(3)),
                    "ring_ns": int(match.group(4)),
                    "decode_us": int(match.group(5)),
                    "ring_us": int(match.group(6)),
                }
            )
    return frames


def percentile(values: Iterable[float], fraction: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    position = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * fraction)))
    return ordered[position]


def describe(name: str, values: Iterable[float], unit: str = "us") -> dict:
    ordered = sorted(values)
    if not ordered:
        return {"name": name, "count": 0}
    return {
        "name": name,
        "count": len(ordered),
        "min": round(ordered[0], 3),
        "p50": round(percentile(ordered, 0.50), 3),
        "p95": round(percentile(ordered, 0.95), 3),
        "p99": round(percentile(ordered, 0.99), 3),
        "max": round(ordered[-1], 3),
        "unit": unit,
    }


def diffs_us(values: list[int]) -> list[float]:
    return [(b - a) / 1000.0 for a, b in zip(values, values[1:]) if b >= a]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("android_log", type=Path)
    parser.add_argument("host_log", type=Path)
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON only")
    parser.add_argument("--require-frames", type=int, default=1,
                        help="minimum frames required on sensor, encoder and host (default: 1)")
    parser.add_argument("--warmup-seconds", type=float, default=0,
                        help="discard this initial span independently on each side (no cross-clock subtraction)")
    args = parser.parse_args()

    sensors, frames = parse_android(args.android_log)
    host_frames = parse_host(args.host_log)
    if args.warmup_seconds < 0:
        parser.error("--warmup-seconds must be nonnegative")
    warmup_ns = int(args.warmup_seconds * 1e9)
    if sensors:
        cutoff = sensors[0]["ts_ns"] + warmup_ns
        sensors = [row for row in sensors if row["ts_ns"] >= cutoff]
    if frames:
        cutoff = frames[0]["output_ns"] + warmup_ns
        frames = [row for row in frames if row["output_ns"] >= cutoff]
    if host_frames:
        cutoff = host_frames[0]["recv_ns"] + warmup_ns
        host_frames = [row for row in host_frames if row["recv_ns"] >= cutoff]
    if args.require_frames < 1:
        parser.error("--require-frames must be positive")
    if min(len(sensors), len(frames), len(host_frames)) < args.require_frames:
        parser.error(
            f"Invalid baseline: need at least {args.require_frames} sensor/encoder/host frames; "
            f"got {len(sensors)}/{len(frames)}/{len(host_frames)}. "
            "Check Stop state, transport and telemetry before rerunning."
        )

    sensor_times_us = [s["ts_ns"] / 1000.0 for s in sensors]
    sensor_to_encoder: list[float] = []
    capture_errors: list[float] = []
    mapping_misses = 0
    for frame in frames:
        # MediaCodec's surface-input PTS uses the uptime clock on this device; the Camera2
        # SENSOR_TIMESTAMP uses elapsedRealtime. Convert PTS to elapsedRealtime with the offset
        # observed at output time, then pair it with the nearest sensor result.
        offset_us = frame["output_ns"] / 1000.0 - frame["uptime_ms"] * 1000.0
        capture_us = frame["pts_us"] + offset_us
        index = bisect_left(sensor_times_us, capture_us)
        choices = [i for i in (index - 1, index) if 0 <= i < len(sensor_times_us)]
        if not choices:
            mapping_misses += 1
            continue
        nearest = min(choices, key=lambda i: abs(sensor_times_us[i] - capture_us))
        error_us = capture_us - sensor_times_us[nearest]
        if abs(error_us) > 10_000:
            mapping_misses += 1
            continue
        capture_errors.append(error_us)
        sensor_to_encoder.append(frame["output_ns"] / 1000.0 - sensor_times_us[nearest])

    keyframe_count = sum(f["key"] for f in frames)
    keyframe_intervals = [
        (b - a) / 1_000_000_000.0
        for a, b in zip(
            [f["output_ns"] for f in frames if f["key"]],
            [f["output_ns"] for f in frames if f["key"]][1:],
        )
    ]

    host_decode = [f["decode_us"] for f in host_frames]
    host_ring = [f["ring_us"] for f in host_frames]
    host_total = [f["decode_us"] + f["ring_us"] for f in host_frames]
    host_interval = diffs_us([f["recv_ns"] for f in host_frames])

    sensor_span_s = (
        (sensors[-1]["ts_ns"] - sensors[0]["ts_ns"]) / 1_000_000_000.0 if len(sensors) > 1 else 0
    )
    encoder_span_s = (
        (frames[-1]["output_ns"] - frames[0]["output_ns"]) / 1_000_000_000.0 if len(frames) > 1 else 0
    )
    host_span_s = (
        (host_frames[-1]["recv_ns"] - host_frames[0]["recv_ns"]) / 1_000_000_000.0
        if len(host_frames) > 1
        else 0
    )

    summary = {
        "warmup_seconds": args.warmup_seconds,
        "counts": {
            "sensor_results": len(sensors),
            "encoded_frames": len(frames),
            "keyframes": sum(f["key"] for f in frames),
            "host_frames": len(host_frames),
            "sensor_encoder_pairs": len(sensor_to_encoder),
            "mapping_misses": mapping_misses,
        },
        "rates": {
            "sensor_fps": round((len(sensors) - 1) / sensor_span_s, 3) if sensor_span_s else None,
            "encoder_fps": round((len(frames) - 1) / encoder_span_s, 3) if encoder_span_s else None,
            "host_fps": round((len(host_frames) - 1) / host_span_s, 3) if host_span_s else None,
        },
        "encoded_bitrate_bps": round(sum(f["size"] for f in frames[1:]) * 8 / encoder_span_s)
        if encoder_span_s else None,
        "encoder_sequence_resets": sum(b["seq"] <= a["seq"] for a, b in zip(frames, frames[1:])),
        "sensor_frame_interval_us": describe(
            "sensor_frame_interval", diffs_us([s["ts_ns"] for s in sensors])
        ),
        "reported_sensor_frame_duration_us": describe(
            "reported_sensor_frame_duration", [s["frame_ns"] / 1000 for s in sensors if s["frame_ns"] > 0]
        ),
        "reported_ae_fps_ranges": sorted({s["ae_fps_range"] for s in sensors}),
        "encoder_output_interval_us": describe(
            "encoder_output_interval", diffs_us([f["output_ns"] for f in frames])
        ),
        "keyframe_interval_s": {
            "count": keyframe_count,
            "values": [round(v, 3) for v in keyframe_intervals],
        },
        "sensor_to_encoder_output_us": describe(
            "sensor_to_encoder_output", sensor_to_encoder
        ),
        "sensor_pairing_error_us": describe("sensor_pairing_error", capture_errors),
        "encoder_dequeue_wait_us": describe(
            "encoder_dequeue_wait", [f["dequeue_wait_us"] for f in frames]
        ),
        "encoder_write_us": describe("encoder_write", [f["write_us"] for f in frames]),
        "encoded_frame_size_bytes": describe(
            "encoded_frame_size", [f["size"] for f in frames], "bytes"
        ),
        "keyframe_size_bytes": describe(
            "keyframe_size", [f["size"] for f in frames if f["key"]], "bytes"
        ),
        "host_receive_interval_us": describe("host_receive_interval", host_interval),
        "host_decode_us": describe("host_decode", host_decode),
        "host_ring_write_us": describe("host_ring_write", host_ring),
        "host_receive_to_ring_us": describe("host_receive_to_ring", host_total),
        "notes": [
            "Phone-side and host-side clocks are not combined here.",
            "Use the external reference timer for end-to-end latency.",
        ],
    }

    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print(f"Android: {args.android_log}")
        print(f"Host:    {args.host_log}")
        print(f"Counts:  {summary['counts']}")
        print(f"Rates:   {summary['rates']}")
        for key, value in summary.items():
            if key in {"counts", "rates", "notes"}:
                continue
            if isinstance(value, dict) and "p50" in value:
                print(
                    f"{key}: n={value['count']} p50={value['p50']} p95={value['p95']} "
                    f"p99={value['p99']} max={value['max']} {value.get('unit', '')}"
                )
            elif key == "keyframe_interval_s":
                print(f"{key}: {value}")
        print("Notes:")
        for note in summary["notes"]:
            print(f"  - {note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
