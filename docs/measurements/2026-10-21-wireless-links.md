# Router Wi-Fi versus laptop hotspot — 2026-10-21

This compares the observed network configurations, **not absolute video latency**.
Both runs use the same PhoneCam protocol-v4 Release host and Redmi Note 8
(Android 11), RTP/H.264, 1920x1080 at 30 fps, requested bitrate 6 Mbps. Video
remains plaintext and control is authenticated.

## Method

`scripts/measure_wireless_link.ps1` restarts this repository's host, targets the
phone's current Wi-Fi address, warms up for 15 seconds, then samples for 60 seconds.
It sends 32-byte ICMP pings with a 1-second timeout and a 100 ms pause between
probes, while the actual video stream is running. Ping durations also contribute
to the sampling interval; sample counts need not match between runs.

Decoded FPS is approximate, derived from observed periodic host frame-counter
reports. RTP "lost" counters are loss/reassembly events, not an exact number or
percentage of missing datagrams. Encoder telemetry is checked for bitrate and
thermal changes. No conclusions use the host's reference-relative delay counter
as an absolute latency measurement.

The user switches the phone between runs. Radio frequency, RSSI, and negotiated
link speeds are recorded when available; band/signal/congestion/power-saving
differences are not isolated from the change in network topology. This is one
short paired experiment, not a universal performance guarantee.

## Router baseline

- Phone: `192.168.1.29`; PC interface: Wi-Fi.
- Frequency: 5180 MHz (5 GHz); phone RSSI: -57 dBm initially, -50 dBm at end.
- Negotiated phone Tx: 433 Mbps; Rx: 390 → 433 Mbps.
- 468/468 ping replies, median RTT 8 ms, p95 42 ms, p99 236 ms, maximum 659 ms.
- Approximately 30.28 decoded fps; 35,372 observed RTP datagrams and zero reported
  RTP loss events in the sampled counter interval.
- Final encoder telemetry: 30.1 fps, 6000 kbps, zero dropped, thermal status 0.
- Raw JSON and logs (ignored build output):
  `windows/build/link-comparison/router-wifi-20261021-145345.*`.

## Hotspot comparison

- Phone: `192.168.137.241`; PC interface: `Local Area Connection* 2`
  (`192.168.137.1`). The new control connection authenticated with protocol v4.
- Frequency: 5180 MHz (5 GHz); phone RSSI: -28 dBm initially, -30 dBm at end.
- Negotiated phone Tx/Rx: 433 Mbps throughout.
- 538/538 ping replies, median RTT 1 ms, p95 3 ms, p99 11 ms, maximum 15 ms.
- Approximately 30.28 decoded fps; 33,299 observed RTP datagrams and zero reported
  RTP loss events in the sampled counter interval.
- Final encoder telemetry: 29.9 fps, 6000 kbps, zero dropped, thermal status 0.
- Raw JSON and logs (ignored build output):
  `windows/build/link-comparison/laptop-hotspot-20261021-145600.*`.

## Results

| Metric | Router Wi-Fi | Laptop hotspot |
|---|---:|---:|
| Median ICMP RTT | 8 ms | **1 ms** |
| p95 ICMP RTT | 42 ms | **3 ms** |
| p99 ICMP RTT | 236 ms | **11 ms** |
| Maximum ICMP RTT | 659 ms | **15 ms** |
| Ping failures | 0 / 468 | 0 / 538 |
| Approximate decoded FPS | 30.28 | 30.28 |
| Observed RTP loss/reassembly events | 0 | 0 |

**For this setup, the laptop hotspot had lower network RTT and substantially
smaller latency spikes**, while both delivered about 30 fps without reported RTP
loss events. Median RTT dropped by 7 ms and p95 by 39 ms. The hotspot is the better
observed link here, not evidence that video itself is eight times faster.

Both links used 5 GHz and comparable negotiated speeds, but the phone's signal to
the laptop hotspot was much stronger than to the router. The results cannot
attribute the entire improvement solely to removing an access-point hop. They
also do not measure absolute glass-to-glass latency or one-way video delay.
Windows ICMP RTT reports integer milliseconds, so a 0 ms minimum does not mean
zero physical delay. The host was left streaming on the hotspot after the test.
