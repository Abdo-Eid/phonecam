"""Phone hotspot diagnostics over USB ADB; test payloads use local Wi-Fi TCP.

No video, installation, router/firewall changes or private-file transfers.
Each TCP direction is capped at 8 Mbps, 2 MiB and a short deadline. This checks
a modest offered load, NOT maximum capacity or RTP/UDP loss behaviour.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

from network_baseline import ping_probe, run, snapshot


def adb_arguments(command):
    # Default to the single USB device, not a private handset identifier or an
    # accidentally selected wireless ADB connection. Multiple USB devices can
    # be selected explicitly through the standard ANDROID_SERIAL environment.
    serial = os.environ.get("ANDROID_SERIAL", "").strip()
    return ["adb", *(["-s", serial] if serial else ["-d"]), "shell", command]


def adb(command, timeout=12):
    return run(adb_arguments(command), timeout)


def phone_ping(target, count, source_ip):
    # Binding by interface name needs SO_BINDTODEVICE privileges on this phone.
    # Ordinary source-address binding is permitted and selects its hotspot IP.
    result = adb(f"ping -I {source_ip} -c {count} -i 1 -W 1 {target}", count * 2 + 10)
    text = result.get("stdout", "")
    packet = re.search(r"(\d+) packets transmitted, (\d+) (?:packets )?received", text)
    timing = re.search(r"(?:rtt|round-trip) min/avg/max/(?:mdev|stddev) = ([\d./]+)", text)
    summary = {}
    if packet:
        sent, received = map(int, packet.groups())
        summary.update(sent=sent, echo_replies=received,
                       no_echo_reply_percent=round(100 * (1 - received / sent), 2))
    if timing:
        summary.update(zip(("min_ms", "mean_ms", "max_ms", "deviation_ms"),
                           map(float, timing[1].split("/"))))
    return {"target": target, **result, "summary": summary}


def transfer(direction, phone_ip, pc_ip, port):
    rate = 1_000_000  # bytes/s = 8 Mbps; not a maximum-speed test
    limit = 2 * 1024 * 1024
    base = f"toybox timeout -k 1 10 toybox nc -s {phone_ip} -p {port} -l"
    command = (base + " < /dev/null | toybox wc -c") if direction == "pc_to_phone" else (base + " < /dev/zero")
    process = subprocess.Popen(adb_arguments(command),
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    result = {"direction": direction, "rate_cap_mbps": 8, "byte_limit": limit,
              "phone_endpoint": f"{phone_ip}:{port}", "pc_source_ip": pc_ip}
    transferred = 0
    started = None
    try:
        time.sleep(.75)  # Allow the one-shot listener to start, not a connection probe.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
            connection.settimeout(2)
            connection.bind((pc_ip, 0))  # Explicit Wi-Fi hotspot source, never USB.
            connection.connect((phone_ip, port))
            started = time.perf_counter()
            chunk = bytes(16384)
            while transferred < limit and time.perf_counter() - started < 6:
                size = min(len(chunk), limit - transferred)
                if direction == "pc_to_phone":
                    connection.sendall(chunk[:size])
                    transferred += size
                else:
                    payload = connection.recv(size)
                    if not payload:
                        break
                    transferred += len(payload)
                delay = transferred / rate - (time.perf_counter() - started)
                if delay > 0:
                    time.sleep(delay)
            connection.shutdown(socket.SHUT_RDWR)
        elapsed = time.perf_counter() - started
        result.update(bytes=transferred, elapsed_s=round(elapsed, 4),
                      paced_mbps=round(transferred * 8 / elapsed / 1e6, 3),
                      completed=transferred == limit)
    except (OSError, TimeoutError) as error:
        result.update(error=str(error), bytes=transferred, completed=False)
    finally:
        try:
            stdout, stderr = process.communicate(timeout=12)
            result.update(listener_exit_code=process.returncode,
                          listener_stdout=stdout.strip(), listener_stderr=stderr.strip())
            if direction == "pc_to_phone" and stdout.strip().isdigit():
                result["phone_received_bytes"] = int(stdout.strip())
                result["completed"] = result.get("completed", False) and int(stdout.strip()) == limit
        except subprocess.TimeoutExpired:
            process.kill()  # Only this script's ADB client; Android timeout bounds its listener.
            process.communicate()
            result["listener_timeout"] = True
            result["completed"] = False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,60}", args.label):
        parser.error("Use a simple A/B label")
    directory = Path(__file__).resolve().parent / "logs" / "networking" / (
        datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + args.label)
    directory.mkdir(parents=True)
    pc = snapshot()
    (directory / "pc-before.json").write_text(json.dumps(pc, indent=2))
    if pc["phonecam_hosts"]:
        raise RuntimeError("PhoneCam is running on the PC; close it before this network-only check")
    upstream = {row["NextHop"] for row in pc["routes"] if row["NextHop"] != "0.0.0.0"}
    if len(upstream) != 1:
        raise RuntimeError("Expected one PC upstream gateway")
    upstream_gateway = next(iter(upstream))
    before = adb("cmd wifi status")
    address = adb("ip -4 -o addr show dev wlan0")
    routes = adb("ip -4 route show table all")
    for name, result in (("wifi-before", before), ("addresses", address), ("routes", routes)):
        (directory / (name + ".json")).write_text(json.dumps(result, indent=2))
    match = re.search(r"\binet (\d+\.\d+\.\d+\.\d+)/", address.get("stdout", ""))
    gateway = re.search(r"default via (\d+\.\d+\.\d+\.\d+) dev wlan0", routes.get("stdout", ""))
    if not match or not gateway or not match[1].startswith("192.168.137.") or gateway[1] != "192.168.137.1":
        raise RuntimeError("Expected the recorded Windows hotspot subnet; refusing to guess the test path")
    phone_ip, pc_ip = match[1], gateway[1]
    if adb("pidof io.github.abdoeid.phonecam").get("stdout", "").strip():
        raise RuntimeError("PhoneCam is running on the phone; close it before the network-only check")
    with ThreadPoolExecutor(max_workers=3) as executor:
        tasks = {"phone_to_pc": executor.submit(phone_ping, pc_ip, 30, phone_ip),
                 "pc_to_phone": executor.submit(ping_probe, phone_ip, 30),
                 "phone_to_internet_via_wifi": executor.submit(phone_ping, "1.1.1.1", 30, phone_ip)}
        pings = {name: task.result() for name, task in tasks.items()}
    (directory / "idle-pings.json").write_text(json.dumps(pings, indent=2))
    # Separate the idle and modest-load phases; concurrent latency probes span
    # both short TCP directions and some idle time, not a pure loaded percentile.
    with ThreadPoolExecutor(max_workers=3) as executor:
        loaded = {"pc_to_phone": executor.submit(ping_probe, phone_ip, 10),
                  "pc_to_gateway": executor.submit(ping_probe, upstream_gateway, 10),
                  "pc_to_external": executor.submit(ping_probe, "1.1.1.1", 10)}
        transfers = [transfer("pc_to_phone", phone_ip, pc_ip, 39874),
                     transfer("phone_to_pc", phone_ip, pc_ip, 39875)]
        loaded_results = {name: task.result() for name, task in loaded.items()}
    result = {"label": args.label, "phone_ip": phone_ip, "pc_hotspot_ip": pc_ip,
              "idle_pings": pings, "transfers": transfers,
              "pings_spanning_transfer_phase": loaded_results,
              "wifi_after": adb("cmd wifi status"),
              "notes": ["USB ADB controls/records only; TCP payload uses hotspot addresses.",
                        "Each direction: 2 MiB maximum, paced at 8 Mbps; not maximum throughput.",
                        "Temporary phone listeners time out automatically; no firewall rules or persistent settings changed.",
                        "TCP reliability can hide retransmissions; this does not measure RTP/UDP loss.",
                        "Transfer-phase pings include idle time and must not be called pure loaded-latency statistics."]}
    (directory / "results.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({"label": args.label, "idle_pings": {
        name: row["summary"] for name, row in pings.items()}, "transfers": transfers,
        "transfer_phase_pings": {name: row["summary"] for name, row in loaded_results.items()}}, indent=2))
    print("run=" + str(directory))


if __name__ == "__main__":
    main()
