import unittest

from run_keyframe_response import responses


class KeyframeResponseTests(unittest.TestCase):
    def test_uses_pc_receipt_time_not_phone_clock(self):
        encoded = [{"pts_us": 500000000, "key": True}]
        host = [{"ts_us": 500000000, "recv_ns": 1200000000, "ring_ns": 1206000000}]
        result = responses(encoded, host, [{"issue_ns": 1100000000}], 1000000000, 2000000000)
        self.assertEqual(result[0]["response_us"], 106000)

    def test_excludes_keyframes_already_received_before_command(self):
        encoded = [{"pts_us": 1, "key": True}]
        host = [{"ts_us": 1, "recv_ns": 1099000000, "ring_ns": 1101000000}]
        result = responses(encoded, host, [{"issue_ns": 1100000000}], 1000000000, 2000000000)
        self.assertIsNone(result[0]["response_us"])

    def test_rejects_incompatible_host_clock(self):
        with self.assertRaisesRegex(ValueError, "QPC time domain"):
            responses([], [{"recv_ns": 50}], [], 1000, 2000)


if __name__ == "__main__":
    unittest.main()
