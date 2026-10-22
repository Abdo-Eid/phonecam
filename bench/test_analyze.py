"""Offline regression checks; no phone, host or window is started."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class AnalyzeTests(unittest.TestCase):
    def test_empty_logs_fail_instead_of_reporting_a_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            empty = Path(directory) / "empty.log"
            empty.write_text("", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("analyze.py")), str(empty), str(empty)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Invalid baseline", result.stderr)

    def test_units_and_rates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            android = root / "android.log"
            host = root / "host.log"
            android_lines = []
            host_lines = []
            for i in range(31):
                sensor_ns = 10_000_000_000 + i * 100_000_000
                output_ns = sensor_ns + 96_000_000
                android_lines.extend([
                    f"PhoneCamBench: sensor ts_ns={sensor_ns} frame_ns=100000000",
                    f"PhoneCamBench: encoder_frame seq={i} pts_us={sensor_ns // 1000} "
                    f"output_ns={output_ns} uptime_ms={output_ns // 1000000} "
                    f"dequeue_wait_us=2000 write_us=200 size=24000 key={int(i % 10 == 0)}",
                ])
                recv_ns = 20_000_000_000 + i * 100_000_000
                host_lines.append(
                    f"BENCH host ts_us={sensor_ns // 1000} recv_ns={recv_ns} "
                    f"decoded_ns={recv_ns + 6000000} ring_ns={recv_ns + 6200000} "
                    "decode_us=6000 ring_us=200"
                )
            android.write_text("\n".join(android_lines), encoding="utf-8")
            host.write_text("\n".join(host_lines), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("analyze.py")),
                 str(android), str(host), "--json"],
                capture_output=True, text=True, check=True,
            )
            summary = json.loads(result.stdout)
            self.assertEqual(summary["rates"]["sensor_fps"], 10)
            self.assertEqual(summary["rates"]["encoder_fps"], 10)
            self.assertEqual(summary["rates"]["host_fps"], 10)
            self.assertEqual(summary["host_receive_interval_us"]["p50"], 100000)
            self.assertEqual(summary["keyframe_interval_s"]["values"], [1, 1, 1])
            self.assertEqual(summary["sensor_to_encoder_output_us"]["p50"], 96000)
            self.assertEqual(summary["counts"]["mapping_misses"], 0)
            trimmed = subprocess.run(
                [sys.executable, str(Path(__file__).with_name("analyze.py")),
                 str(android), str(host), "--json", "--warmup-seconds", "1"],
                capture_output=True, text=True, check=True,
            )
            steady = json.loads(trimmed.stdout)
            self.assertEqual(steady["counts"]["encoded_frames"], 21)
            self.assertEqual(steady["rates"]["encoder_fps"], 10)
            self.assertEqual(steady["counts"]["mapping_misses"], 0)


if __name__ == "__main__":
    unittest.main()
