"""Bounded Windows network comparison; no settings changes or video capture.

The label identifies the upstream network according to the user, not an SSID
query. Local hotspot transfers are deliberately NOT inferred from link rates.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import time


SNAPSHOT = r"""
$ErrorActionPreference = 'Stop'
$allAdapters = @(Get-NetAdapter -IncludeHidden)
$adapters = @($allAdapters | Where-Object {
  $_.HardwareInterface -or $_.InterfaceDescription -like '*Wi-Fi Direct*'
} | Select-Object Name,InterfaceDescription,ifIndex,@{n='Status';e={$_.Status.ToString()}},
  @{n='AdminStatus';e={$_.AdminStatus.ToString()}},
  @{n='MediaConnectionState';e={$_.MediaConnectionState.ToString()}},LinkSpeed)
$routeInterfaces = @($allAdapters | Select-Object Name,ifIndex,
  @{n='Status';e={$_.Status.ToString()}},
  @{n='AdminStatus';e={$_.AdminStatus.ToString()}},
  @{n='MediaConnectionState';e={$_.MediaConnectionState.ToString()}})
$addresses = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
  $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*'
} | Select-Object InterfaceAlias,InterfaceIndex,IPAddress,PrefixLength)
$routes = @(Get-NetRoute -AddressFamily IPv4 | Where-Object { $_.DestinationPrefix -eq '0.0.0.0/0' } |
  Select-Object InterfaceAlias,InterfaceIndex,NextHop,RouteMetric)
$routes6 = @(Get-NetRoute -AddressFamily IPv6 | Where-Object { $_.DestinationPrefix -eq '::/0' } |
  Select-Object InterfaceAlias,InterfaceIndex,NextHop,RouteMetric)
$private = @($addresses | Where-Object { $_.IPAddress -eq '192.168.137.1' })
$neighbors = @(foreach ($address in $private) {
  Get-NetNeighbor -InterfaceIndex $address.InterfaceIndex -AddressFamily IPv4 |
    Where-Object { $_.State -notin @('Unreachable','Permanent','Incomplete') } |
    Select-Object IPAddress,@{n='State';e={$_.State.ToString()}}
})
$hosts = @(Get-CimInstance Win32_Process -Filter "Name='phonecam-host.exe'" |
  Select-Object ProcessId,ExecutablePath)
$statistics = @(Get-NetAdapterStatistics -Name @($adapters.Name) |
  Select-Object Name,ReceivedBytes,SentBytes,ReceivedDiscardedPackets,
    OutboundDiscardedPackets,ReceivedPacketErrors,OutboundPacketErrors)
[pscustomobject]@{
  captured_utc=[DateTime]::UtcNow.ToString('o'); adapters=$adapters;
  addresses=$addresses; routes=$routes; routes_ipv6=$routes6;
  route_interfaces=$routeInterfaces; hotspot_neighbors=$neighbors;
  phonecam_hosts=$hosts; adapter_statistics=$statistics;
  dns_servers=@(Get-DnsClientServerAddress |
    Where-Object { $_.ServerAddresses.Count } |
    Select-Object InterfaceAlias,InterfaceIndex,AddressFamily,ServerAddresses)
} | ConvertTo-Json -Depth 5
"""


def run(arguments, timeout):
    started = time.perf_counter()
    try:
        result = subprocess.run(arguments, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                errors="replace", timeout=timeout)
        return {"exit_code": result.returncode, "stdout": result.stdout,
                "stderr": result.stderr, "elapsed_s": round(time.perf_counter() - started, 3)}
    except subprocess.TimeoutExpired as error:
        def text(value):
            return value.decode(errors="replace") if isinstance(value, bytes) else value
        return {"error": str(error), "stdout": text(error.stdout) or "",
                "stderr": text(error.stderr) or "", "elapsed_s": round(time.perf_counter() - started, 3)}
    except OSError as error:
        return {"error": str(error), "elapsed_s": round(time.perf_counter() - started, 3)}


def snapshot():
    result = run(["pwsh", "-NoProfile", "-Command", SNAPSHOT], 25)
    if result.get("exit_code") != 0:
        raise RuntimeError("Network snapshot failed: " + json.dumps(result))
    return json.loads(result["stdout"])


def ping_summary(text):
    # Windows English ping output. Preserve the raw output if localization or
    # an ICMP error prevents parsing. 'Destination unreachable' is not a reply.
    times = []
    below_ms = 0
    for line in text.splitlines():
        if "TTL=" not in line.upper():
            continue
        match = re.search(r"time([=<])\s*(\d+)ms", line, re.IGNORECASE)
        if match:
            times.append(float(match[2]))
            below_ms += match[1] == "<"
    sent = re.search(r"Sent = (\d+)", text)
    result = {"echo_replies": len(times), "sent": int(sent[1]) if sent else None,
              "sub_ms_replies_reported_as_1ms": below_ms}
    if result["sent"]:
        result["no_echo_reply_percent"] = round(100 * (1 - len(times) / result["sent"]), 2)
    if times:
        ordered = sorted(times)
        result.update(min_ms=min(times), p50_ms=ordered[len(times) // 2],
                      p95_ms=ordered[min(len(times) - 1, int(len(times) * .95))],
                      max_ms=max(times), mean_ms=round(sum(times) / len(times), 3))
    return result


def ping_probe(target, count):
    raw = run(["ping.exe", "-4", "-n", str(count), "-w", "1000", "-l", "32", target],
              count * 2 + 10)
    return {"target": target, **raw, "summary": ping_summary(raw.get("stdout", ""))}


def application_probes():
    results = []
    for index in range(3):
        dns = run(["nslookup.exe", "-timeout=2", "-retry=1", "www.microsoft.com"], 8)
        https = run(["curl.exe", "--silent", "--show-error", "--output", "NUL",
                     "--connect-timeout", "4", "--max-time", "8",
                     "--write-out", "%{json}", "https://www.microsoft.com/robots.txt"], 10)
        if https.get("exit_code") == 0:
            try:
                metrics = json.loads(https["stdout"])
                # Do not retain curl's full connection metadata/local addresses.
                https["metrics"] = {key: metrics.get(key) for key in (
                    "http_code", "time_namelookup", "time_connect", "time_appconnect",
                    "time_starttransfer", "time_total", "size_download")}
                https.pop("stdout")
            except (ValueError, TypeError):
                pass
        results.append({"probe": index + 1, "captured_utc": datetime.now(timezone.utc).isoformat(),
                        "dns": dns, "https": https})
        if index < 2:
            time.sleep(5)  # Probe pacing, not background-job polling.
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True)
    parser.add_argument("--count", type=int, default=30)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,60}", args.label) or not 5 <= args.count <= 60:
        parser.error("Use a simple label and 5..60 ping samples")
    directory = Path(__file__).resolve().parent / "logs" / "networking" / (
        datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + args.label)
    directory.mkdir(parents=True)
    before = snapshot()
    (directory / "before.json").write_text(json.dumps(before, indent=2))
    if before["phonecam_hosts"]:
        raise RuntimeError("PhoneCam is running; close it before the network-only comparison")
    adb = run(["adb", "devices", "-l"], 8)
    (directory / "adb-devices.json").write_text(json.dumps(adb, indent=2))
    wlan = run(["netsh", "wlan", "show", "interfaces"], 8)
    (directory / "wlan-interfaces.json").write_text(json.dumps(wlan, indent=2))
    targets = {"external": "1.1.1.1"}
    gateways = {row["NextHop"] for row in before["routes"] if row["NextHop"] != "0.0.0.0"}
    if len(gateways) != 1:
        raise RuntimeError("Expected one upstream gateway; inspect snapshot rather than guessing")
    targets["gateway"] = next(iter(gateways))
    peers = sorted({row["IPAddress"] for row in before["hotspot_neighbors"]})[:3]
    for index, peer in enumerate(peers):
        targets[f"hotspot_peer_{index + 1}"] = peer
    with ThreadPoolExecutor(max_workers=6) as executor:
        futures = {name: executor.submit(ping_probe, target, args.count)
                   for name, target in targets.items()}
        application = executor.submit(application_probes)
        pings = {name: future.result() for name, future in futures.items()}
        applications = application.result()
    after = snapshot()
    result = {"label": args.label, "ping_count": args.count, "pings": pings,
              "application_probes": applications, "after": after,
              "notes": ["Upstream network label comes from the user; not independently confirmed by SSID.",
                        "32-byte ICMP probes at Windows ping pacing; no speed test or video.",
                        "ICMP failure alone does not establish internet or application failure.",
                        "DNS/HTTPS may use cached answers; DNS elapsed time includes nslookup startup.",
                        "Link-speed reports are not throughput measurements; hotspot peers are not identified as the phone.",
                        "No network, permissions, router, tethering or application settings changed."]}
    (directory / "results.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({"label": args.label, "ping_summaries": {
        name: {"target": row["target"], **row["summary"]} for name, row in pings.items()},
        "https": [row["https"].get("metrics", row["https"]) for row in applications]}, indent=2))
    print("run=" + str(directory))


if __name__ == "__main__":
    main()
