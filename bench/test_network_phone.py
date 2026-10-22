import unittest
import os
from unittest.mock import patch

from network_phone import adb_arguments, phone_ping


class PhonePingTests(unittest.TestCase):
    @patch.dict(os.environ, {}, clear=True)
    def test_default_adb_selection_is_single_usb_device(self):
        self.assertEqual(adb_arguments("cmd wifi status"), ["adb", "-d", "shell", "cmd wifi status"])

    @patch.dict(os.environ, {"ANDROID_SERIAL": "example-usb-device"}, clear=True)
    def test_explicit_serial_is_not_hardcoded(self):
        self.assertEqual(adb_arguments("cmd wifi status"),
                         ["adb", "-s", "example-usb-device", "shell", "cmd wifi status"])

    @patch("network_phone.adb")
    def test_source_address_and_linux_summary(self, adb):
        adb.return_value = {"exit_code": 0, "stdout":
                            "5 packets transmitted, 5 received, 0% packet loss\n"
                            "rtt min/avg/max/mdev = 1.0/2.0/3.0/0.5 ms\n"}
        result = phone_ping("192.168.137.1", 5, "192.168.137.241")
        self.assertIn("-I 192.168.137.241", adb.call_args.args[0])
        self.assertEqual(result["summary"]["mean_ms"], 2)
        self.assertEqual(result["summary"]["no_echo_reply_percent"], 0)

    @patch("network_phone.adb")
    def test_permission_failure_is_not_a_packet_loss_measurement(self, adb):
        adb.return_value = {"exit_code": 2, "stdout": "", "stderr": "Operation not permitted"}
        result = phone_ping("192.168.137.1", 5, "192.168.137.241")
        self.assertEqual(result["summary"], {})


if __name__ == "__main__":
    unittest.main()
