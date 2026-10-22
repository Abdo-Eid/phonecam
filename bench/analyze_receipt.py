"""Analyze the native consumer's receipt timestamps, never file modification times."""

import argparse
import csv
import json
from pathlib import Path

from analyze import describe


def summarize(directory: Path, readings: Path | None = None) -> dict:
    manifest = dict(line.split("=", 1) for line in
                    (directory / "format.txt").read_text().splitlines() if "=" in line)
    frequency = int(manifest["qpc_frequency"])
    with (directory / "receipt.csv").open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if frequency <= 0 or len(rows) < 2:
        raise ValueError("Need a positive QPC frequency and at least two samples")
    qpc = [int(row["arrival_qpc"]) for row in rows]
    times = [int(row["sample_time_100ns"]) for row in rows]
    if any(b <= a for a, b in zip(qpc, qpc[1:])):
        raise ValueError("Receipt QPC timestamps must increase")
    intervals = [(b - a) * 1e6 / frequency for a, b in zip(qpc, qpc[1:])]
    media_intervals = [(b - a) / 10 for a, b in zip(times, times[1:])]
    result = {
        "consumer": manifest["consumer"],
        "samples": len(rows),
        "receipt_fps": round((len(rows) - 1) * frequency / (qpc[-1] - qpc[0]), 3),
        "receipt_interval_us": describe("receipt_interval", intervals),
        "sample_interval_us": describe("sample_interval", media_intervals),
        "duplicate_sample_times": sum(b == a for a, b in zip(times, times[1:])),
        "backward_sample_times": sum(b < a for a, b in zip(times, times[1:])),
        "empty_reads": int(manifest["empty_reads"]),
        "saved_images": sum(bool(row["image"]) for row in rows),
        "notes": [
            "Sample-time continuity does not prove image-content continuity or end-to-end latency.",
            "Receipt timing includes this fixed consumer's buffering; image copies can affect later samples.",
        ],
    }
    if all(row.get("mf_receipt_100ns") for row in rows):
        ages = [(int(row["mf_receipt_100ns"]) - int(row["sample_time_100ns"])) / 10 for row in rows]
        if min(ages) < -1000:
            raise ValueError("Sample and MF receipt clocks are incompatible or timestamps are in the future")
        result["mf_sample_to_receipt_us"] = describe("mf_sample_to_receipt", ages)
        third = max(1, len(ages) // 3)
        result["mf_age_first_third_us"] = describe("first_third", ages[:third])
        result["mf_age_last_third_us"] = describe("last_third", ages[-third:])
        result["read_delay_ms"] = int(manifest.get("read_delay_ms", 0))
        result["notes"].append("MF sample age measures delivery/queueing after scheduled generation, not sensor/image age.")
    if readings:
        by_number = {int(row["frame"]): row for row in rows}
        seen = set()
        ages = []
        with readings.open(newline="", encoding="utf-8") as source:
            timer_rows = list(csv.DictReader(source))
        for reading in timer_rows:
            number = int(reading["frame"])
            row = by_number[number]
            if number in seen or not row["image"]:
                raise ValueError("Each timer reading must refer to a unique saved image")
            seen.add(number)
            age = int(row["arrival_unix_us"]) - int(reading["timer_unix_ms"]) * 1000
            if age < 0:
                raise ValueError("Timer is later than receipt; check orientation/digits/clock")
            ages.append(age)
        if not ages:
            raise ValueError("No optical timer readings")
        result["visual_age_us"] = describe("visual_age", ages)
        result["notes"].append(
            "Visual age includes timer refresh/scanout, exposure and consumer delay; "
            "manually transcribed digits and clock adjustments require validation."
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--timer-readings", type=Path,
                        help="optional CSV: frame,timer_unix_ms (read from saved images)")
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.capture, args.timer_readings), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
