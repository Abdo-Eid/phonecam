import copy
import unittest

from hotspot_offline import default_route_state, offline_problem, phone_address


class OfflineGuardTests(unittest.TestCase):
    def setUp(self):
        self.pc = {
            "phonecam_hosts": [], "routes": [], "routes_ipv6": [],
            "adapters": [
                {"Name": "Wi-Fi", "ifIndex": 10, "AdminStatus": "Up",
                 "MediaConnectionState": "Disconnected", "Status": "Disconnected"},
                {"Name": "Local Area Connection* 2", "ifIndex": 4, "Status": "Up"},
            ],
            "addresses": [{"IPAddress": "192.168.137.1", "InterfaceIndex": 4}],
        }
        self.phone = {name: {"exit_code": 0, "stdout": ""} for name in
                      ("wifi", "addresses", "routes", "phonecam_process")}
        self.phone["addresses"]["stdout"] = "30: wlan0 inet 192.168.137.241/24"
        self.phone["routes"]["stdout"] = "default via 192.168.137.1 dev wlan0 table 1030"

    def test_disconnected_station_can_keep_hotspot_running(self):
        self.assertIsNone(offline_problem(self.pc))

    def test_ipv6_default_route_also_blocks_offline_test(self):
        self.pc["routes_ipv6"] = [{"NextHop": "fe80::1"}]
        self.assertIn("default route", offline_problem(self.pc))

    def test_ipv4_default_route_blocks_offline_test(self):
        self.pc["routes"] = [{"NextHop": "192.168.1.1"}]
        self.assertIn("default route", offline_problem(self.pc))

    def test_retained_route_on_disconnected_wifi_is_not_a_live_uplink(self):
        # Reproduces the user's real interrupted run: index 10 disconnected,
        # hotspot index 4 Up, but Windows still lists the old IPv4 gateway.
        self.pc["routes"] = [{"InterfaceAlias": "Wi-Fi", "InterfaceIndex": 10,
                              "NextHop": "192.168.1.1", "RouteMetric": 0}]
        self.assertIsNone(offline_problem(self.pc))
        state = default_route_state(self.pc)
        self.assertEqual(len(state["retained_on_disconnected_interfaces"]), 1)
        self.assertEqual(state["active_or_unverified"], [])

    def test_retained_ipv6_route_on_disconnected_wifi_is_also_recorded(self):
        self.pc["routes_ipv6"] = [{"InterfaceIndex": 10, "NextHop": "fe80::1"}]
        self.assertIsNone(offline_problem(self.pc))
        self.assertEqual(default_route_state(self.pc)["retained_on_disconnected_interfaces"][0]["family"], "IPv6")

    def test_live_ethernet_default_route_still_blocks(self):
        self.pc["adapters"].append({"Name": "Ethernet", "ifIndex": 7, "Status": "Up",
                                    "MediaConnectionState": "Connected", "AdminStatus": "Up"})
        self.pc["routes"] = [{"InterfaceIndex": 7, "NextHop": "192.168.0.1"}]
        self.assertIn("active or unverified", offline_problem(self.pc))

    def test_live_virtual_default_route_is_not_lost_by_adapter_filter(self):
        self.pc["route_interfaces"] = self.pc["adapters"] + [
            {"Name": "VPN", "ifIndex": 30, "Status": "Up", "MediaConnectionState": "Connected"}]
        self.pc["routes"] = [{"InterfaceIndex": 30, "NextHop": "10.0.0.1"}]
        self.assertIn("active or unverified", offline_problem(self.pc))

    def test_unknown_interface_route_is_not_ignored(self):
        self.pc["routes"] = [{"InterfaceIndex": 999, "NextHop": "192.168.0.1"}]
        self.assertIsNotNone(offline_problem(self.pc))

    def test_conflicting_up_status_is_not_treated_as_stale(self):
        self.pc["adapters"][0]["Status"] = "Up"
        self.pc["routes"] = [{"InterfaceIndex": 10, "NextHop": "192.168.1.1"}]
        self.assertIsNotNone(offline_problem(self.pc))

    def test_radio_off_and_hotspot_off_are_not_the_requested_condition(self):
        self.pc["adapters"][0]["AdminStatus"] = "Down"
        self.assertIn("Keep Wi-Fi enabled", offline_problem(self.pc))
        self.pc["adapters"][0]["AdminStatus"] = "Up"
        self.pc["adapters"][1]["Status"] = "Disconnected"
        self.assertIn("hotspot interface", offline_problem(self.pc))

    def test_remaining_station_association_is_rejected_even_without_routes(self):
        self.pc["adapters"][0]["MediaConnectionState"] = "Connected"
        self.assertIn("association", offline_problem(self.pc))

    def test_phone_must_stay_on_hotspot(self):
        self.assertEqual(phone_address(self.phone), "192.168.137.241")
        other = copy.deepcopy(self.phone)
        other["addresses"]["stdout"] = "30: wlan0 inet 192.168.1.9/24"
        with self.assertRaisesRegex(RuntimeError, "not on the expected PC hotspot"):
            phone_address(other)

    def test_adb_failure_is_not_a_network_measurement(self):
        self.phone["wifi"] = {"error": "USB disconnected"}
        with self.assertRaisesRegex(RuntimeError, "USB ADB"):
            phone_address(self.phone)


if __name__ == "__main__":
    unittest.main()
