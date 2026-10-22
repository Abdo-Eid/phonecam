"""Compare repeated same-build encoder runs. Segment differences are not E2E wins."""

import argparse
import csv
import json
import statistics
from pathlib import Path


def compare(directory: Path) -> dict:
    with (directory / "run-order.csv").open(newline="", encoding="utf-8-sig") as source:
        rows = list(csv.DictReader(source))
    groups = {}
    failures = []
    identity = None
    warmup = None
    for row in rows:
        if row["status"] != "passed":
            failures.append(row)
            continue
        run_dir = Path(row["run"])
        summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8-sig"))
        with (run_dir / "artifact-hashes.csv").open(newline="", encoding="utf-8-sig") as source:
            hashes = {Path(item["Path"]).name: item["Hash"].upper() for item in csv.DictReader(source)}
        if len(hashes) != 3:
            raise ValueError(f"Incomplete host/APK/vcam hashes: {run_dir}")
        if identity is None:
            identity = hashes
            warmup = summary["warmup_seconds"]
        if hashes != identity or summary["warmup_seconds"] != warmup:
            raise ValueError("Cannot compare runs from different artifacts or warmup procedures")
        groups.setdefault(row.get("configuration") or row["encoder_case"], []).append(summary)
    output = {}
    for name, runs in groups.items():
        p95 = [run["sensor_to_encoder_output_us"]["p95"] / 1000 for run in runs]
        p50 = [run["sensor_to_encoder_output_us"]["p50"] / 1000 for run in runs]
        output[name] = {
            "runs": len(runs),
            "sensor_to_encoder_p50_ms": p50,
            "sensor_to_encoder_p95_ms": p95,
            "median_p50_ms": round(statistics.median(p50), 3),
            "median_p95_ms": round(statistics.median(p95), 3),
            "host_fps": [run["rates"]["host_fps"] for run in runs],
            "encoded_bitrate_bps": [run["encoded_bitrate_bps"] for run in runs],
            "median_keyframe_interval_s": [statistics.median(run["keyframe_interval_s"]["values"])
                if run["keyframe_interval_s"]["values"] else None for run in runs],
            "pairing_misses": [run["counts"]["mapping_misses"] for run in runs],
            "encoder_sequence_resets": [run["encoder_sequence_resets"] for run in runs],
        }
    default = output.get("default")
    if not default:
        raise ValueError("No successful default runs")
    for case in output.values():
        case["median_p95_delta_from_default_ms"] = round(case["median_p95_ms"] - default["median_p95_ms"], 3)
    return {
        "artifact_hashes": identity,
        "warmup_seconds": warmup,
        "cases": output,
        "failed_runs": failures,
        "notes": [
            "Negative delta is a lower phone segment duration, NOT a proven end-to-end improvement.",
            "Scene, thermal drift and background load may confound differences; examine interleaved controls.",
            "Configuration acceptance alone does not prove that a key was honored.",
            "No candidate is automatically adopted or declared statistically significant.",
        ],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cohort", type=Path)
    args = parser.parse_args()
    print(json.dumps(compare(args.cohort), indent=2))
