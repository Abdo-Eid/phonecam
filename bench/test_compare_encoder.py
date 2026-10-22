import csv
import json
import tempfile
import unittest
from pathlib import Path

from compare_encoder import compare


class CompareEncoderTests(unittest.TestCase):
    def make_cohort(self, root, different_hash=False):
        rows = []
        for index, case in enumerate(["default", "latency0"]):
            run = root / f"run-{index}"
            run.mkdir()
            summary = {
                "warmup_seconds": 8,
                "sensor_to_encoder_output_us": {"p50": 96000 - index * 1000, "p95": 102000 - index * 1000},
                "rates": {"host_fps": 30}, "encoded_bitrate_bps": 6000000,
                "keyframe_interval_s": {"values": [1, 1]},
                "counts": {"mapping_misses": 0}, "encoder_sequence_resets": 0,
            }
            (run / "summary.json").write_text(json.dumps(summary))
            with (run / "artifact-hashes.csv").open("w", newline="") as output:
                writer = csv.writer(output)
                writer.writerow(["Algorithm", "Hash", "Path"])
                for artifact in ["host.exe", "app.apk", "vcam.dll"]:
                    digest = "different" if different_hash and index == 1 and artifact == "app.apk" else artifact
                    writer.writerow(["SHA256", digest, str(root / artifact)])
            rows.append([1, case, "passed", str(run), ""])
        with (root / "run-order.csv").open("w", newline="") as output:
            writer = csv.writer(output)
            writer.writerow(["round", "encoder_case", "status", "run", "error"])
            writer.writerows(rows)

    def test_reports_segment_delta_without_declaring_a_win(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_cohort(root)
            result = compare(root)
            self.assertEqual(result["cases"]["latency0"]["median_p95_delta_from_default_ms"], -1)
            self.assertTrue(any("NOT a proven end-to-end" in note for note in result["notes"]))

    def test_rejects_different_builds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_cohort(root, different_hash=True)
            with self.assertRaisesRegex(ValueError, "different artifacts"):
                compare(root)


if __name__ == "__main__":
    unittest.main()
