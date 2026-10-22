"""Exercise the existing keyframe command, not reconnect/network behavior.

Measures PC command-issue -> next decoded keyframe published at the host ring.
This is NOT clean-image recovery after corruption and NOT glass-to-glass latency.
Run only after other measurement batches have finished.
"""

import argparse
import csv
import ctypes
import json
import os
import re
import subprocess
import time
from datetime import datetime
from pathlib import Path

from analyze import describe, parse_android, parse_host


ROOT = Path(__file__).resolve().parent.parent
TAGS = ["PCBenchLowLatency", "PCBenchLatency0", "PCBenchPriority0", "PCBenchNoBFrames",
        "PCBenchIntraRefresh", "PCBenchIdr2", "PCBenchIdr5", "PCBenchBaseline", "PCBenchConstrained"]


def responses(encoded, decoded, requests, begin, end):
    if not decoded or not all(begin <= row["recv_ns"] <= end for row in decoded):
        raise ValueError("Host telemetry is missing or not in the expected PC QPC time domain")
    key_pts = {row["pts_us"] for row in encoded if row["key"]}
    result = []
    for request in requests:
        matching = [row for row in decoded if row["ts_us"] in key_pts
                    and request["issue_ns"] <= row["recv_ns"]
                    and row["ring_ns"] <= request["issue_ns"] + 1_000_000_000]
        row = min(matching, key=lambda item: item["ring_ns"]) if matching else None
        result.append({**request, "response_us": (row["ring_ns"] - request["issue_ns"]) / 1000 if row else None,
                       "keyframe_pts_us": row["ts_us"] if row else None})
    return result


def adb(*arguments):
    return subprocess.run(["adb", *arguments], check=True, capture_output=True, text=True, timeout=20).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=int, default=10)
    args = parser.parse_args()
    if not 1 <= args.requests <= 30:
        parser.error("request count must be 1..30")
    # Refuse to drive an existing host. Do not terminate or change another session.
    active = subprocess.run(["tasklist", "/FI", "IMAGENAME eq phonecam-host.exe", "/NH"],
                            capture_output=True, text=True, check=True)
    if "phonecam-host.exe" in active.stdout.lower():
        raise RuntimeError("Another host is running")
    frequency, before, after = ctypes.c_longlong(), ctypes.c_longlong(), ctypes.c_longlong()
    ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(before))
    probe = time.perf_counter_ns()
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(after))
    if not before.value * 10**9 // frequency.value <= probe <= after.value * 10**9 // frequency.value + 1000:
        raise RuntimeError("Python perf_counter is not aligned with Windows QPC")

    run = ROOT / "bench" / "runs" / (datetime.now().strftime("%Y%m%d-%H%M%S") + "-E9-keyframe-response")
    run.mkdir()
    package = adb("shell", "pm", "path", "io.github.abdoeid.phonecam").splitlines()[0].removeprefix("package:")
    installed = adb("shell", "sha256sum", package).split()[0]
    import hashlib
    with (ROOT / "android/app/build/outputs/apk/debug/app-debug.apk").open("rb") as source:
        apk_hash = hashlib.file_digest(source, "sha256").hexdigest()
    if installed != apk_hash:
        raise RuntimeError("Installed APK differs from local benchmark APK")
    process = None
    requests = []
    begin = time.perf_counter_ns()
    try:
        adb("shell", "setprop", "log.tag.PhoneCamBench", "DEBUG")
        for tag in TAGS:
            adb("shell", "setprop", "log.tag." + tag, "INFO")
        adb("shell", "setprop", "log.tag.PCBenchIdr5", "DEBUG")
        adb("shell", "am", "force-stop", "io.github.abdoeid.phonecam")
        adb("logcat", "-c")
        adb("shell", "am", "start", "-n", "io.github.abdoeid.phonecam/.MainActivity")
        time.sleep(3)
        environment = {**os.environ, "PHONECAM_BENCH": "1", "PHONECAM_BENCH_MEDIA": "default"}
        with (run / "host.out.log").open("w") as stdout, (run / "host.err.log").open("w") as stderr:
            process = subprocess.Popen([str(ROOT / "windows/build/host/Debug/phonecam-host.exe"), "--adb"],
                                       stdin=subprocess.PIPE, stdout=stdout, stderr=stderr,
                                       text=True, env=environment, cwd=ROOT)
            deadline = time.monotonic() + 25
            resumed = False
            while True:
                status = (run / "host.err.log").read_text(errors="replace")
                messages = re.findall(r"Connection: (.+)", status)
                latest = messages[-1] if messages else ""
                if latest.startswith("Streaming via"):
                    break
                if latest.startswith("Stopped") and not resumed:
                    subprocess.run(["pwsh", "-NoProfile", "-File", str(ROOT / "scripts/test_stopped_gui.ps1"),
                                    "-ProcessId", str(process.pid), "-Action", "Resume", "-LogPath",
                                    str(run / "host.err.log")], check=True, timeout=40)
                    resumed = True
                    deadline = time.monotonic() + 15
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Host did not start streaming")
                time.sleep(0.25)
            time.sleep(8)
            for index in range(args.requests):
                issue = time.perf_counter_ns()
                process.stdin.write("keyframe\n")
                process.stdin.flush()
                requests.append({"request": index + 1, "issue_ns": issue})
                time.sleep(1.5)
            (run / "android.log").write_text(adb("logcat", "-d", "-v", "monotonic", "-s", "PhoneCamBench:D", "*:S"), encoding="utf-8")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        (run / "requests.json").write_text(json.dumps(requests, indent=2))
        for tag in TAGS:
            adb("shell", "setprop", "log.tag." + tag, "INFO")
    end = time.perf_counter_ns()
    text = (run / "android.log").read_text()
    if "encoder_case name=idr5" not in text:
        raise RuntimeError("5-second IDR test setting was not confirmed")
    _, encoded = parse_android(run / "android.log")
    decoded = parse_host(run / "host.err.log")
    result = responses(encoded, decoded, requests, begin, end)
    output = (run / "host.out.log").read_text()
    commands = re.findall(r"-> RequestKeyframe\(\) cmd_id=(\d+)", output)
    acknowledgments = {int(command): status for command, status in re.findall(r"Ack cmd_id=(\d+) result=(\w+)", output)}
    if len(commands) != len(requests):
        raise RuntimeError("Console did not process every command")
    for row, command in zip(result, commands):
        row["command_id"] = int(command)
        row["ack"] = acknowledgments.get(int(command), "missing")
    with (run / "responses.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=result[0].keys())
        writer.writeheader()
        writer.writerows(result)
    successful = [row["response_us"] for row in result if row["ack"] == "Ok" and row["response_us"] is not None]
    summary = {"requested": len(requests), "successful": len(successful),
               "next_decoded_keyframe_us": describe("next_decoded_keyframe", successful),
               "notes": ["PC command issue to next decoded keyframe at host ring, using same-host QPC.",
                         "PTS is used only as frame identity, not subtracted across devices.",
                         "Not corrupted-picture recovery, not an optical end-to-end latency measurement."],
               "installed_apk_sha256": installed}
    (run / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    print("run=" + str(run))
    if len(successful) != len(requests):
        raise RuntimeError("Some requests failed or lacked a subsequent decoded keyframe within one second")


if __name__ == "__main__":
    main()
