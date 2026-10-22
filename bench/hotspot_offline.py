"""Standalone offline hotspot collector. Does not change network settings.

Start before OR after manually disconnecting the PC's upstream Wi-Fi association
(NOT its Wi-Fi radio or mobile hotspot), leaving the phone on the hotspot.
USB ADB is diagnostic/control only. No internet/DNS/video probes are performed.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import time

from network_baseline import ping_probe, snapshot
from network_phone import adb, phone_ping, transfer


def default_route_state(pc):
    """A retained route on a confirmed disconnected interface is not a live uplink.

    Keep active and unknown-interface routes blocking. Never delete a route or
    infer that a route is stale merely from its address or the absence of traffic.
    """
    interfaces = pc.get("route_interfaces", pc["adapters"])
    active, inactive = [], []
    for family, rows in (("IPv4", pc.get("routes", [])), ("IPv6", pc.get("routes_ipv6", []))):
        for route in rows:
            interface = next((row for row in interfaces
                              if row.get("ifIndex") == route.get("InterfaceIndex")
                              and route.get("InterfaceIndex") is not None), None)
            entry = {"family": family, "route": route, "interface": interface}
            if (interface and interface.get("MediaConnectionState") == "Disconnected"
                    and interface.get("Status") in ("Disconnected", "Disabled", "Not Present")):
                entry["reason"] = "Retained route on an interface confirmed disconnected."
                inactive.append(entry)
            else:
                entry["reason"] = "Interface is active or its disconnected state is not established."
                active.append(entry)
    return {"active_or_unverified": active, "retained_on_disconnected_interfaces": inactive}


def offline_problem(pc):
    if pc.get("phonecam_hosts"):
        return "Close PhoneCam on the PC first."
    wifi = next((row for row in pc["adapters"] if row["Name"] == "Wi-Fi"), None)
    if not wifi:
        return "The expected PC Wi-Fi adapter was not found."
    if wifi.get("AdminStatus") != "Up":
        return "Keep Wi-Fi enabled; disconnect the upstream network, not the adapter."
    if wifi.get("MediaConnectionState") != "Disconnected":
        return "PC upstream Wi-Fi still reports an association; waiting for Disconnected."
    if default_route_state(pc)["active_or_unverified"]:
        return "An active or unverified IPv4/IPv6 default route remains; disconnect that upstream connection."
    address = next((row for row in pc["addresses"] if row["IPAddress"] == "192.168.137.1"), None)
    if not address:
        return "PC hotspot address 192.168.137.1 is missing; keep mobile hotspot on."
    interface = next((row for row in pc["adapters"] if row["ifIndex"] == address["InterfaceIndex"]), None)
    if not interface or interface["Status"] != "Up":
        return "The hotspot interface is not Up; keep mobile hotspot on."
    return None


def phone_state():
    state = {
        "wifi": adb("cmd wifi status"),
        "addresses": adb("ip -4 -o addr show dev wlan0"),
        "routes": adb("ip -4 route show table all"),
        "phonecam_process": adb("pidof io.github.abdoeid.phonecam"),
        "captured_utc": datetime.now(timezone.utc).isoformat(),
    }
    return state


def phone_address(state):
    for name in ("wifi", "addresses", "routes"):
        if state[name].get("exit_code") != 0:
            raise RuntimeError("USB ADB phone query failed; check the cable/debugging authorization.")
    if state["phonecam_process"].get("stdout", "").strip():
        raise RuntimeError("Close PhoneCam on the phone before this network-only test.")
    match = re.search(r"\binet (192\.168\.137\.\d+)/", state["addresses"].get("stdout", ""))
    route = re.search(r"default via 192\.168\.137\.1 dev wlan0", state["routes"].get("stdout", ""))
    if not match or not route or match[1] in ("192.168.137.1", "192.168.137.255"):
        raise RuntimeError("Phone is not on the expected PC hotspot. Reconnect it to the hotspot, not a router.")
    return match[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wait-offline", type=int, default=180,
                        help="Seconds to wait for manual upstream disconnect (default 180).")
    parser.add_argument("--count", type=int, default=30, help="Local ping samples per direction (5..60).")
    parser.add_argument("--inspect", action="store_true", help="Save one state snapshot only; no probes/transfers.")
    args = parser.parse_args()
    if not 0 <= args.wait_offline <= 600 or not 5 <= args.count <= 60:
        parser.error("Use --wait-offline 0..600 and --count 5..60")
    directory = Path(__file__).resolve().parent / "logs" / "networking" / (
        datetime.now().strftime("%Y%m%d-%H%M%S-%f") + "-offline-hotspot")
    directory.mkdir(parents=True)
    result = {
        "status": "starting", "started_utc": datetime.now(timezone.utc).isoformat(),
        "settings_changed": False, "internet_probes": False, "ping_count": args.count,
        "notes": [
            "USB ADB only controls and reads diagnostics; payloads use the hotspot IPv4 addresses.",
            "No DNS, internet, video, router, firewall or adapter-setting changes are made.",
            "TCP directions are capped at 8 Mbps and 2 MiB each; not a maximum-speed test.",
            "Condition is checked at phase boundaries, not continuously proven for every instant.",
            "Retained default routes are recorded but non-blocking only on interfaces confirmed disconnected; active/unknown routes block.",
            "No external phone ping in offline mode: workload differs from the online collector.",
            "Sender reset after the receive byte limit is an expected bounded-test shutdown.",
        ],
        "collector_hashes": {name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
                             for name in ("hotspot_offline.py", "network_baseline.py", "network_phone.py")},
    }

    def save():
        (directory / "results.json").write_text(json.dumps(result, indent=2))

    def checkpoint(name, expected_phone=None):
        pc = snapshot()
        phone = phone_state()
        result[name] = {"pc": pc, "phone": phone, "default_route_state": default_route_state(pc)}
        save()
        problem = offline_problem(pc)
        if problem:
            raise RuntimeError("Offline condition changed: " + problem)
        address = phone_address(phone)
        if expected_phone and address != expected_phone:
            raise RuntimeError("Phone IP changed mid-run; results are not a stable-condition comparison.")
        return address

    print("Results: " + str(directory), flush=True)
    print("Keep PhoneCam closed, USB ADB connected, Wi-Fi ON and mobile hotspot ON.", flush=True)
    print("If not already disconnected, disconnect only the PC's upstream Wi-Fi. Wait for DONE or STOPPED.", flush=True)
    save()
    exit_code = 0
    try:
        result["initial_pc"] = snapshot()
        result["initial_default_route_state"] = default_route_state(result["initial_pc"])
        result["initial_phone"] = phone_state()
        if args.inspect:
            result.update(status="inspected_only", offline_wait_reason=offline_problem(result["initial_pc"]))
            save()
            print("Inspection only; no network probes or transfers were run.", flush=True)
            return 0
        if result["initial_pc"]["phonecam_hosts"]:
            raise RuntimeError("Close PhoneCam on the PC first.")
        phone_address(result["initial_phone"])
        deadline = time.monotonic() + args.wait_offline
        pc = result["initial_pc"]
        result["status"] = "waiting_for_manual_disconnect"
        save()
        while True:
            reason = offline_problem(pc)
            observation = {"captured_utc": pc["captured_utc"], "wait_reason": reason,
                           "adapters": pc["adapters"], "routes": pc["routes"],
                           "routes_ipv6": pc.get("routes_ipv6", []),
                           "default_route_state": default_route_state(pc)}
            with (directory / "disconnect-observations.jsonl").open("a") as stream:
                stream.write(json.dumps(observation) + "\n")
            if not reason:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Offline condition was not reached: " + reason)
            print("Waiting: " + reason, flush=True)
            time.sleep(2)  # Finite wait for the user's manual state change.
            pc = snapshot()
        print("Upstream disconnected and hotspot still Up. Checking phone association...", flush=True)
        retained = default_route_state(pc)["retained_on_disconnected_interfaces"]
        if retained:
            print(f"Recorded {len(retained)} retained default route(s) on disconnected interfaces; not treated as live uplinks.", flush=True)
        phone_ip = checkpoint("before_local_probes")
        pc_ip = "192.168.137.1"
        result.update(status="local_pings", phone_ip=phone_ip, pc_hotspot_ip=pc_ip)
        save()
        print(f"Local latency/loss: {args.count} samples in each direction. No internet probes.", flush=True)
        with ThreadPoolExecutor(max_workers=2) as pool:
            tasks = {"pc_to_phone": pool.submit(ping_probe, phone_ip, args.count),
                     "phone_to_pc": pool.submit(phone_ping, pc_ip, args.count, phone_ip)}
            result["local_pings"] = {name: task.result() for name, task in tasks.items()}
        save()
        checkpoint("after_local_probes", phone_ip)
        result["status"] = "bounded_local_transfers"
        result["transfers"] = []
        save()
        print("Two short local TCP transfers: up to 2 MiB per direction, capped at 8 Mbps.", flush=True)
        with ThreadPoolExecutor(max_workers=1) as pool:
            latency = pool.submit(ping_probe, phone_ip, 10)
            for direction, port in (("pc_to_phone", 39874), ("phone_to_pc", 39875)):
                result["transfers"].append(transfer(direction, phone_ip, pc_ip, port))
                save()
            result["pings_spanning_transfer_phase"] = latency.result()
        save()
        checkpoint("after_local_transfers", phone_ip)
        result.update(status="completed", completed_utc=datetime.now(timezone.utc).isoformat())
        save()
        summary = {"local_pings": {name: row["summary"] for name, row in result["local_pings"].items()},
                   "transfers": [{key: row.get(key) for key in (
                       "direction", "bytes", "elapsed_s", "paced_mbps", "completed", "error")}
                                 for row in result["transfers"]]}
        (directory / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary, indent=2), flush=True)
        print("DONE. Tests ended. You may reconnect the PC upstream network and return to chat.", flush=True)
    except (Exception, KeyboardInterrupt) as error:
        exit_code = 130 if isinstance(error, KeyboardInterrupt) else 1
        result.update(status="interrupted" if exit_code == 130 else "stopped", error=str(error))
        save()
        print("STOPPED. " + (str(error) or "Interrupted by user."), flush=True)
        print("Partial diagnostics are saved; no settings were changed. You may reconnect.", flush=True)
    finally:
        print("run=" + str(directory), flush=True)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
