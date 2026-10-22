import unittest

from network_baseline import ping_summary


class PingSummaryTests(unittest.TestCase):
    def test_unreachable_is_not_an_echo_reply(self):
        result = ping_summary("Reply from 192.168.1.2: Destination host unreachable.\n"
                              "Packets: Sent = 1, Received = 1, Lost = 0")
        self.assertEqual(result["echo_replies"], 0)
        self.assertEqual(result["no_echo_reply_percent"], 100)

    def test_preserves_sub_ms_precision_limit_and_loss(self):
        result = ping_summary("Reply from 1.1.1.1: bytes=32 time<1ms TTL=64\n"
                              "Reply from 1.1.1.1: bytes=32 time=12ms TTL=64\n"
                              "Request timed out.\nPackets: Sent = 3, Received = 2, Lost = 1")
        self.assertEqual(result["echo_replies"], 2)
        self.assertEqual(result["sub_ms_replies_reported_as_1ms"], 1)
        self.assertEqual(result["no_echo_reply_percent"], 33.33)
        self.assertEqual(result["max_ms"], 12)


if __name__ == "__main__":
    unittest.main()
