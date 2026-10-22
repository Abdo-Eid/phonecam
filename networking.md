# PhoneCam networking investigation

Updated: 2026-10-22. **Closed at the user's request:** recovery removed perceived
pixelation; bounded reordering was reported better, but lag remained unacceptable.
The user chose USB and stopped further hotspot work. Final evidence, installed
state and limits are recorded in section 22. The hotspot is **not marked fixed**.

Historical investigation status: initial A/B, proposed-change trials and return-B
recorded. Return-B recovered for the first PC checks but was poor during the
later hotspot checks; a subsequent idle check recovered again. The standalone
offline hotspot test is now complete and also shows poor local ICMP behaviour,
without an upstream association. Router/default DNS is user-reported and
PC DHCP DNS verified. With the phone reconnected to the hotspot, the slowdown
returned despite router/default DNS. The router b/g/n change is now reported
applied and tested: reported PC link rate increased, but poor responsiveness
remains. The first post-DNS sample had a different phone association and is
preserved as a separate condition, not a DNS-only success.
Earlier investigation made no assistant-initiated settings changes. In the
explicitly approved compatibility trial in section 17, the assistant changed
only the adapter's ax/ac mode to ac. No privacy permissions, drivers or
application defaults were changed. The user enabled USB debugging
to allow the requested ADB diagnostics.
Subsequently, an explicitly authorized PC-side RTP recovery improvement was
implemented and installed (section 20); no media bitrate/resolution, USB path,
phone APK, driver or network settings were changed by that application update.

## 1. The situation, correctly understood

**The phone has remained connected to the PC's Windows mobile hotspot.**
The important change was the **PC's upstream Wi-Fi network**, not moving the
phone between the two routers.

Use these labels throughout the investigation:

- **Network A:** the user's usual network/router; historically problematic.
- **Network B:** the uncle's network; reported to give a smooth stream despite
  being farther away.
- **Hotspot:** the laptop's mobile hotspot, used by the phone in both cases.

```text
Network A OR Network B access point
               |
       PC upstream Wi-Fi connection
               |
       laptop / Windows networking
               |
       laptop's mobile hotspot
               |
             phone

Local video: phone -> laptop hotspot -> PhoneCam on the laptop.
PC internet/chat: laptop -> upstream Wi-Fi -> router/internet.
Phone internet, IF using hotspot internet sharing:
phone -> laptop hotspot -> laptop's upstream connection -> internet.
```

Which device was used for every chat/internet observation was not separately
recorded. Do not assume that all internet testing was on the phone.

The local video should not need to traverse the upstream router or internet.
Nevertheless, changing the upstream association can affect a laptop's concurrent
hotspot operation: RF airtime, channel/band allocation, driver scheduling and
power-management behaviour are possible dependencies.

**Correction to earlier assistant explanations:** this was not a comparison of
phone-to-PC over hotspot versus phone-to-PC over the uncle's router. Also,
"the hotspot is only as good as the upstream internet" is not an established
explanation for corruption in a local video stream. The observed dependency is
real; its mechanism remains unproven.

## 2. Story and evidence so far

1. The earlier media-pipeline experiment cohorts deliberately used **USB/ADB**.
   They did not validate hotspot performance or wireless recovery. Their ~30 fps
   and timestamp-continuity results do not establish clean images over RTP.
2. At the user's request, the installed PC application was updated from the old
   August desktop binaries to the current Release build. The installed service
   was moved from the checkout's Debug path to `C:\Program Files\PhoneCam`.
   The installed virtual-camera DLL already matched the current Release output.
   Connection registry settings were verified unchanged. The PC-only synthetic
   camera smoke test passed; that was **not** a wireless test.
3. On the usual network setup, the user reported severe moving-picture breakup,
   like corrupted video on an unreliable connection: pixelated blocks and old
   picture content smearing into moving objects.
4. With permission, the assistant captured the existing virtual-camera stream
   without restarting or changing its connection. The five-second capture saved
   11 images from 151 received samples. The contact sheet shows severe blocky
   corruption around a moving hand and face, with at least one cleaner image
   between corrupted ones. This is more than ordinary motion blur.
5. During that observation, the installed PC host had an RTP UDP listener on
   port **27185** and an established control connection from hotspot-side
   `192.168.137.1` to phone `192.168.137.241:27184`. These are historical addresses,
   not values to hard-code into future tests. They support a local hotspot video
   path; they do not show video routed through the upstream router.
6. The native consumer still received **30 fps**, with no duplicate/backward
   scheduled sample timestamps. Therefore smooth sample timing can coexist
   with visibly damaged pictures; timestamps alone are insufficient.
7. The user also reported that the problematic setup makes internet/chat so
   slow that communicating with the assistant becomes difficult. "Saturation"
   describes the experienced slowdown, not a measured bandwidth result yet.
8. The user tried disconnecting the upstream Wi-Fi association while leaving
   Wi-Fi/hotspot enabled—a trick that helps with Syncthing. It did **not** resolve
   this issue. The exact adapter/channel state during that attempt was not saved.
9. Changing to Network B made the stream smooth and allowed normal chat again.
   **The phone still used the laptop hotspot.** The upstream network was the
   changed factor. No simultaneous diagnostic capture of that successful
   condition was obtained.
10. The user clarified that trouble with the usual network/router predates this
    desktop update. Neither the update nor a particular router defect has been
    established as the cause.

### Preserved artifacts

- Corrupt live stream: `bench/e2e/receipt-20261022-184702-352/`.
  See `capture/contact-sheet.png`, original BMPs and `summary.json`.
- Desktop deployment: `windows/build/updates/20261022-181944/`.
- Other media experiments and their limitations: `plan.md`, `bench/results.md`.

These artifact directories are intentionally ignored by Git. Do not delete them
or publish screenshots containing the user or their room without permission.
The saved damaged-stream images establish the symptom, not the packet-level cause.

## 3. What the assistant inspected directly

Read-only PC snapshot taken after the clarification. Network B was the user's
last reported upstream network, but its actual SSID could not be independently
queried. **No PhoneCam host process was running at this later snapshot**, so
these are idle machine observations, not measurements during the corrupt or
smooth live stream.

| Item | Observation | What it does / does not establish |
|---|---|---|
| Physical Wi-Fi hardware | One MediaTek Wi-Fi 6 MT7921 adapter | No second physical Wi-Fi adapter was enumerated |
| Ethernet | Realtek PCIe GbE, disconnected | Not the active internet uplink at the snapshot |
| Upstream interface | `Wi-Fi`, interface index 10, `192.168.1.15/24` | PC's infrastructure-side address at that moment |
| IPv4 default route | Wi-Fi -> `192.168.1.1` | Upstream gateway, not proof of which router identity or internet quality |
| Hotspot-side interface | Microsoft Wi-Fi Direct Virtual Adapter #2, `Local Area Connection* 2`, index 4, `192.168.137.1/24` | Active virtual hotspot-side interface; not a second physical radio |
| Sharing services | `SharedAccess` and `icssvc` running | Consistent with Windows mobile hotspot/ICS; exact sharing-interface mapping was not accessible |
| Driver | MediaTek `3.0.1.1338`, reported date 2026-04-07 | Record for reproducibility; not evidence that the driver is faulty |
| Supported radio standards | 802.11b/a/g/n/ac/ax; 2.4 and 5 GHz | Capability, not the currently selected band/standard |
| Driver concurrency report | Two concurrent channels supported; Wi-Fi Direct GO on 5 GHz supported | Do not claim the two connections must use one channel or that two independent physical radios exist |
| Reported link speed | 54 Mbps from `Get-NetAdapter` | Not measured throughput, usable airtime or proof of saturation; verify under each condition |
| Preferred-band setting | Prefer 5 GHz | Preference only; actual upstream/hotspot/phone bands remain unverified |
| Channel-width settings | 2.4/5 GHz both Auto | Configuration, not negotiated width |
| Other settings | ax enabled, medium-high roaming, highest transmit power, U-APSD disabled, Miracast band Auto | Recorded, not changed; no recommendation to alter them without comparison |
| Interface error/discard counters | Exposed counters were zero on Wi-Fi and hotspot virtual interface | Does not rule out over-the-air loss, retries, RTP sequence gaps or decoder reference damage |

### Access limitations—not silently guessed

- `netsh wlan show interfaces` was blocked by Windows WLAN location/privacy
  permissions. Actual SSID, band, channel, BSSID and signal/link details were
  not obtained. Location permissions were **not** enabled or bypassed.
- Read-only `HNetCfg.HNetShare` inspection returned access denied. The actual
  ICS public/private interface configuration was not confirmed. No bridge was
  demonstrated; ordinary Windows hotspot uses sharing/NAT, not necessarily an
  Ethernet-style transparent bridge.
- No USB/ADB device was available during the corruption investigation. No phone
  logs, effective encoder settings or phone-side Wi-Fi details were collected.
- The existing host's decoder/RTP loss logs were not captured. Do not describe
  packet loss or a particular decoder error as log-confirmed.

## 4. Working hypotheses, not conclusions

1. **Concurrent upstream + hotspot operation changes with the selected AP.**
   Different bands/channels, airtime allocation or MT7921 driver behaviour could
   change the local hotspot even though the local video route stays the same.
2. **The usual RF environment/AP association is problematic.** Interference,
   congestion, negotiated mode/rate, roaming or retries could affect both internet
   responsiveness and the local stream. Farther distance alone does not predict
   worse performance. This is not yet proof that Network A's router is broken.
3. **Traffic contention or queueing under streaming load.** Need stream-off versus
   stream-on measurements on both upstream networks. Do not infer internet
   bandwidth consumption from video bitrate alone.
4. **H.264 recovery makes network disturbances visible as persistent corruption.**
   Missing/reordered RTP data can damage reference pictures until a clean IDR.
   Code inspection shows the receiver drops in-progress assembly on loss and
   requests a keyframe, but resumes at a clean NAL boundary; that is not by itself
   proof that subsequent predicted pictures have clean references. The earlier
   synthetic decoder test showed reference damage can persist until an IDR.
   Live causality, loss rate and recovery behaviour still need evidence.
5. **Encoder/other application fault remains possible.** Snapshots alone cannot
   distinguish packet damage from every encoder/decoder issue. A/B network
   reproduction, fixed settings and correlated logs are required before a fix.

There may be both a network problem and insufficient application recovery.
Neither "increase bitrate" nor a transport/driver redesign is justified yet.

## 5. Comparison protocol

**Subsequent clarification:** the user also wants hotspot connection behaviour
and speed compared, still without testing video corruption. For that additional
stage, leave the phone connected to the laptop hotspot, with PhoneCam closed;
USB video/network delivery is not used. A USB cable **may be connected for ADB
diagnostics only**: inspect phone Wi-Fi band/frequency, RSSI/link rates, routes
and available counters without starting PhoneCam or enabling USB tethering.
Distinguish this diagnostic channel from the hotspot link being measured.
The PC-only baseline below is followed by light local
PC↔phone connectivity checks and, where a client-side test endpoint is available,
short, bounded local transfers. Measure those separately from internet speed.
Do not run unbounded/full-speed transfers that prevent chat. No handheld setup
is needed. Keep the same phone/hotspot association for both upstream networks.

**Scope correction from the user:** investigate the two networks and internet
responsiveness, not video corruption. The user's observations are accepted;
reproducing the damaged stream is not a prerequisite. Keep PhoneCam closed.
No phone interaction, camera scene, snapshots or motion test is needed for this
comparison. The historical video evidence above is context, not a new test task.

### Keep the topology and workload fixed

- **Only change the PC's upstream Wi-Fi between A and B.** Leave the laptop
  hotspot setting unchanged. There is no need to connect or handle the phone
  for the PC-side comparison. If Windows restarts the hotspot or
  changes its band/channel when switching upstream, record that as an outcome.
- Keep PC position, power state and hotspot settings the same. Record any
  existing hotspot clients without requiring one or silently changing them.
- Leave PhoneCam closed on the PC; no video-streaming trials are planned.
- Avoid downloads, updates, backups and speed tests during the primary trials.
  Record any unavoidable background traffic instead of assuming an idle network.
- Keep authentication data, hotspot password and saved credentials unchanged.

### Primary trials: PC network B -> A -> B

Start with the working Network B so a good reference and offline instructions
exist before risking chat connectivity. Allow ~15 seconds after association
changes, then use a 30-second observation window per condition.

| Order | PC upstream | PhoneCam | Observe |
|---|---|---|---|
| 1 | B: uncle | Closed | Gateway/internet latency, loss, DNS/HTTPS responsiveness and interface state |
| 2 | A: usual | Closed | Repeat exactly the same PC-side measurements |
| 3 | B: uncle | Closed | Check whether returning to B restores the earlier network behaviour |

Repeat in reverse order if practical. Three repeats per condition are preferable
before attributing differences, but stop early if connectivity becomes unusable;
do not make the user wait or keep holding the phone.

### Measurements the assistant should collect, not quiz the user about

- Timestamp and A/B condition; adapters, driver, IP addresses, default route,
  physical/virtual-interface statistics before and after each window.
- Actual upstream and hotspot bands/channels/widths/rates where Windows exposes
  them. Distinguish permissions failure from an actual missing capability.
- Existing hotspot-client state where accessible; no phone interaction or
  phone-side diagnostics are prerequisites.
- Small, bounded latency/loss samples to the upstream gateway and
  one consistent external target. Keep probes light (for example one per second
  for 30 seconds), not bandwidth saturation tests. ICMP blocking is not proof
  of general connectivity failure; external ICMP is not equivalent to chat/HTTPS.
- Small DNS and HTTPS checks against consistent targets, with timeouts and
  results saved locally so an A-network chat interruption does not lose evidence.
- Separate **gateway latency/loss**, **external latency/loss**, **DNS resolution**,
  **HTTPS responsiveness** and **link state** rather than one "network speed" number.

### Brief human observations needed

The user only needs to identify when they intentionally switch A/B and report
normal/slow/unusable internet or chat. Their previous video observations do not
need to be demonstrated again. Router
credentials or troubleshooting jargon are not required.

| Trial / time | Upstream A/B | Hotspot setting/state | Internet/chat | Assistant's measurements |
|---|---|---|---|---|
| Pending | | | | |

If A makes chat unusable, **return to B** and send the result afterward. Local
logs can be analyzed after reconnection; staying online on A is not a requirement.

### Follow-ups, only after the primary comparison

Run one change at a time, retain a B control, and obtain agreement before changing
configuration:

- Repeat upstream-disconnected/hotspot-on with the resulting route and hotspot
  association recorded; loss of internet in this condition is expected.
- Compare actual 2.4/5 GHz/channel assignments; test a supported alternative only
  if justified, not an unmeasured channel/band overhaul.
- Use an Ethernet uplink or a separate existing adapter, if available, to isolate
  Wi-Fi upstream/hotspot concurrency. Do not buy hardware as the first step.
- Add capped, controlled traffic only after the PC-only comparison; avoid a full
  speed test that reproduces saturation and destroys chat access unnecessarily.
- Router firmware/channel/QoS changes or driver updates come last, with saved
  settings and an explicit rollback plan. Do not change the uncle's router.
- Hotspot-client connectivity and bounded local throughput are now requested as
  a second stage; do not confuse them with PC upstream/internet measurements.
  Video-recovery experiments remain outside this comparison.

## 6. Immediate next prompt

**The upstream-disconnected hotspot test completed; results are in section 15.**
Keep the current settings unchanged while reviewing the findings. No additional
collector, payload listener or automatic network switch is running. Further
power-state, client, driver or radio isolation should be a separately agreed step.
No automatic switching, router changes or video tests are authorized by this document.

## 7. Network B baseline — recorded 2026-10-22

User explicitly identified this as the uncle network, with hotspot on and phone
connected. PhoneCam was not running. USB debugging was initially off, then the
user enabled it; a USB device became available (private serial retained only in
ignored local diagnostics). No USB networking/tethering,
ADB port forwarding or video delivery was used. The interrupted first launch
(`20261022-193101-B-uncle-r1`, empty directory) is not a result.

### PC upstream/internet

Completed run: `bench/logs/networking/20261022-193225-B-uncle-r1-complete/`.

| Check | Result | Qualification |
|---|---|---|
| PC -> upstream gateway `192.168.1.1` | 30/30 replies; median 2 ms, p95 3 ms, maximum 7 ms | Small ICMP samples, not capacity |
| PC -> external `1.1.1.1` | 30/30 replies; median 48 ms, p95 51 ms, maximum 52 ms | One external destination, not all internet services |
| HTTPS to the same small Microsoft robots file | 3/3 HTTP 200; total ~297–535 ms | DNS/TLS/CDN/cache influence these times; not a speed test |
| `nslookup` using default DNS selection | Timed out in the initial 8-second bounds | Follow-up explains which resolver it selected; not labelled general internet failure |

Addendum `bench/logs/networking/20261022-194344-B-uncle-addendum-r1/`:
`nslookup` selected IPv6 DNS `fe80::1`, which did not answer its queries within
~10 seconds. Windows `Resolve-DnsName` resolved the name successfully in ~1.58 s,
and HTTPS worked. Preserve this DNS-path distinction when comparing A; a DNS
issue already present on B is not automatically the explanation for A being worse.
No DNS configuration was changed.

Windows link-speed reports varied during these observations: upstream 432 Mbps
at initial inspection, 103 Mbps in the saved PC run; hotspot virtual adapter
87.5 Mbps initially, 29 Mbps in the saved run. Do not treat these reports as
measured throughput or directly equate them with the phone's Wi-Fi link rate.

### Phone/hotspot association confirmed through ADB

Raw diagnostics: `bench/logs/networking/20261022-193147-B-uncle-phone-r1/`.
The phone's actual Wi-Fi connection reported **5180 MHz (5 GHz, channel 36)**,
Wi-Fi standard 5, RSSI approximately **-40 to -41 dBm**, and Tx/Rx link rates
**433 Mbps**. Source address was `192.168.137.241`; Android's Wi-Fi routing table
had a default via hotspot gateway `192.168.137.1`.

Thus the phone was on the PC hotspot, not switched to the uncle's AP. SSID/BSSID,
MAC addresses and historical scan details remain only in ignored raw diagnostics.
The PC's upstream band/channel is still unverified because its WLAN query was
privacy-restricted. Do not infer it from the phone-side channel.

### Local hotspot checks and bounded transfer

Initial run: `bench/logs/networking/20261022-194142-B-uncle-hotspot-r1/`.
The Android interface-name-bound ping was denied (`SO_BINDTODEVICE`); that is
**not packet loss**. PC -> phone did return 30/30 replies but with idle latency
2–122 ms, median 70 ms. This is a real idle observation, not a diagnosis of its
cause. Source-IP-bound phone probes subsequently worked without elevated phone
privileges or security-setting changes.

Use corrected run `bench/logs/networking/20261022-194539-B-uncle-hotspot-r1-sourcebound/`
for the matched A comparison. Its concurrent phone probes are a different
activity/power-state workload from the initial PC-only ping:

| Check | Result |
|---|---|
| Phone -> PC, source bound to phone hotspot IPv4 | 30/30 replies; mean 14.94 ms, range 2.30–24.30 ms |
| PC -> phone, during concurrent phone probes | 30/30 replies; median 18 ms, p95 95 ms, maximum 114 ms |
| Phone -> external target, source bound to hotspot IPv4 | 30/30 replies; mean 64.66 ms, range 50.73–75.03 ms |
| PC -> phone TCP payload | All 2 MiB received and counted on phone, ~2.098 s at the imposed ~8 Mbps rate |
| Phone -> PC TCP payload | All 2 MiB received on PC, ~2.098 s at the imposed ~8 Mbps rate |
| PC -> upstream gateway, probes spanning the transfer phase | 10/10 replies; median 1 ms, maximum 2 ms |
| PC -> external target, probes spanning the transfer phase | 10/10 replies; median 47 ms, maximum 48 ms |

These are **paced offered-load checks, not maximum throughput**. The 8 Mbps cap
and 2 MiB payload limit kept each direction short. Temporary one-shot listeners
were bound to the phone's hotspot IP, controlled via USB ADB and terminated after
the tests. Payload sockets used the PC/phone hotspot addresses, not USB. Payloads
were zeros, not personal files. No PC listener/firewall rule was needed.

The phone-to-PC sender reports `Connection reset by peer` when the PC deliberately
closes after receiving its byte limit; this expected test shutdown is not evidence
of network corruption. TCP can hide retransmissions, so these checks do not prove
zero wireless retries or UDP loss. Ten-sample transfer-phase pings also span some
idle time; they are not pure sustained-load latency percentiles.

**Current conclusion:** B carried this small bounded local load without missed
ICMP replies or an observed upstream latency increase. There is no A/B causal
conclusion yet, no maximum-speed result, and no video retest. Phone idle latency
and DNS resolver behaviour are recorded rather than dismissed because B feels good.

Reusable collectors: `bench/network_baseline.py --label <condition> --count 30`
and `bench/network_phone.py --label <condition>`. Thirteen offline regression
tests passed, including Windows/Android ping parsing and permission-failure handling.
All results remain in ignored `bench/logs/networking/`; do not replace older runs.

## 8. Network A baseline — recorded before proposed changes

User reported ready on the usual network. Runs:

- PC: `bench/logs/networking/20261022-195210-A-usual-r1/`.
- Hotspot/ADB: `bench/logs/networking/20261022-195424-A-usual-hotspot-r1-sourcebound/`.
- Initial comparison: `bench/logs/networking/20261022-195738-AB-comparison.json`.

| Check | B: uncle | A: usual |
|---|---|---|
| PC -> upstream gateway, 30 probes | 30 replies; median 2 ms, p95 3 ms, max 7 ms | 28 replies; median 92 ms, p95 294 ms, max 551 ms |
| PC -> external target, 30 probes | 30 replies; median 48 ms, p95 51 ms | 25 replies; median 255 ms, p95 600 ms |
| Phone -> PC hotspot, 30 source-bound probes | 30 replies; mean 14.94 ms, max 24.30 ms | 30 replies; mean 30.98 ms, max 89.55 ms |
| PC -> phone, 30 probes concurrent with phone probes | 30 replies; median 18 ms, p95 95 ms | 30 replies; median 46 ms, p95 92 ms |
| Phone -> internet through hotspot, 30 probes | 30 replies; mean 64.66 ms | 25 replies; mean 386.25 ms |
| Bounded local TCP payload, each direction | All 2 MiB completed at imposed ~8 Mbps | All 2 MiB completed at imposed ~8 Mbps |
| Small HTTPS checks | 3/3 HTTP 200; ~297–535 ms total | 3/3 HTTP 200; ~657–977 ms total |

These are one initial sequence, not three-repeat controls. Ping percentiles use
successful replies only; missed replies must be considered separately. Small
ICMP samples cannot alone locate a router hardware fault or distinguish RF loss
from all ICMP handling/queueing effects. No video stream was running.

During the brief transfer phase on A, PC gateway replies were 9/10 (median
215 ms) and external replies 8/10 (median 459 ms). Those windows span idle time
as well as the capped transfers and are not pure loaded-latency percentiles.

The phone stayed on **5180 MHz/channel 36**, with 433 Mbps Tx/Rx link reports
in both runs. Its recorded signal was -30 dBm on the corrected B run and -22 dBm
on A, so its RSSI was not held identical. The PC's reported upstream link speed
was 103 Mbps in B's saved snapshot and 54 Mbps in A's; these reports are not
maximum-throughput tests or proof of the exact negotiated radio mode.

DNS deserves separate treatment: A's default `nslookup` queries succeeded via
the advertised IPv6 resolver `fe80::1` (~113–531 ms process elapsed), whereas
B's direct `nslookup` queries timed out and Windows resolution/HTTPS still
worked. Thus the initial measurements do **not** support a simple conclusion
that slower DNS alone caused A's poor gateway/internet ping behaviour.

**Current interpretation:** A's upstream path is already much less responsive
with PhoneCam closed, while the local hotspot passed this small capped TCP load.
That narrows the investigation but does not establish maximum Wi-Fi capacity,
the cause of every symptom, or a particular required fix. The requested return-B
confirmation was not yet collected when the user proposed the following changes.

## 9. Two user-proposed changes — status tracked separately

The user explicitly requested that **both** ideas and their sequence be recorded:

| ID | Proposed change | User's intended purpose | Status / missing detail |
|---|---|---|---|
| A-DNS | Stop using `1.1.1.3` for DNS; leave DNS to the router/default configuration | Check whether that DNS choice contributes to the network problem | Applied/retested: PC IPv4 DHCP DNS verified, but the poor gateway/internet measurements returned once the phone rejoined the hotspot; not a demonstrated fix. Router's upstream resolver remains unverified |
| A-WIFI-MODE | Set the router's **2.4 GHz transmission mode to 802.11b/g/n** | Avoid the Wi-Fi being limited under the current mode | User reports applied and provides settings screenshot; PC reports 300 Mbps instead of 54 Mbps, but the first post-change checks still show poor responsiveness. Original router mode was not recorded |

The `1.1.1.3` setting is user-reported. The assistant has not independently
confirmed whether it is configured on the router, PC, phone, or as an upstream
forwarder behind an advertised local resolver. Seeing `fe80::1` in a client DNS
query does not establish which resolver a router uses upstream.

**Do not confuse addresses:** the diagnostic ICMP target `1.1.1.1` was not
`1.1.1.3`, and sending ICMP probes did not change any DNS configuration.

The possible benefit of b/g/n depends on the original setting, radio band and
hardware. Enabling n could help if a 2.4 GHz radio was restricted to b/g, but
b/g/n is not universally faster than ac/ax and can exclude newer modes in some
settings. The observed 54 Mbps report is a clue, not proof of the current mode.
No improvement from either proposal is claimed before a repeat measurement.

### Recommended comparison order

1. Save the original DNS and Wi-Fi mode, and identify where each setting lives.
   Do not record passwords or private router credentials in this document.
2. **DNS only:** change away from `1.1.1.3`, leaving the Wi-Fi mode unchanged.
   Record the exact replacement and repeat A's PC/phone checks.
3. **Mode only:** preferably restore the original DNS first, then change only the
   Wi-Fi mode to b/g/n. Record the original/new mode and repeat the same checks.
4. If both changes are retained together, label that result **combined**, not a
   separate proof that either one was responsible. If the user changes both
   before testing, accept and document that limitation instead of attributing
   the result to one setting.
5. Keep PC position, phone hotspot connection, USB ADB, PhoneCam-closed state,
   test payload limits and other settings unchanged where possible. Record any
   reconnection, channel change or DHCP change caused by a setting update.
6. Record normal/slow/unusable chat and retain local logs. Switch back to B if
   needed; do not stay disconnected merely to finish a probe window.

No DNS, radio-mode, router or driver change has been performed by the assistant.

## 10. Router/default DNS update — first observation, topology changed

The user reported: DNS changed, leaving the router to handle it as on the uncle
network. Read-only verification found the PC Wi-Fi interface using IPv4 DNS
`192.168.1.1` (both DHCP-advertised entries), empty static `NameServer`, and
`EnableDHCP=1`. IPv6 DNS remains `fe80::1`. These client settings do not reveal
which DNS service the router forwards to. The PC adapter still reports ax enabled
and a 54 Mbps upstream link; the proposed router/adapter b/g/n change has not
been confirmed applied.

PC run: `bench/logs/networking/20261022-201219-DNS-router-current-r1/`.

- Gateway: 30/30 replies, median 1 ms, p95 9 ms, maximum 11 ms.
- External target: 30/30 replies, median 65 ms, p95 78 ms, maximum 95 ms.
- HTTPS: 3/3 HTTP 200, total ~351–377 ms.

This is better than the initial A sample, but **not a controlled DNS-only result**.
The hotspot follow-up
`bench/logs/networking/20261022-201530-DNS-router-hotspot-current-r1/`
found the phone at **192.168.1.9**, directly associated to the upstream router on
**2412 MHz/channel 1**, with a 54 Mbps link and RSSI -45 dBm. It was no longer
on the PC hotspot at `192.168.137.x`. The PC hotspot interface itself remained
present at `192.168.137.1`. The phone collector intentionally stopped at its
topology guard; no local transfer or phone ping comparison was run on this
different path.

The exact instant the phone changed association was not recorded, so the PC
sample cannot be assumed to have the original hotspot-client workload. A missing
hotspot client/concurrency change, reconnection, time-varying congestion and the
DNS update are confounded. IP-address ICMP probes do not require DNS resolution;
their improvement alone is not proof that the DNS resolver was the direct cause.

Next: reconnect the phone to the **PC hotspot**, retain USB ADB and the new
router/default DNS, leave the Wi-Fi mode unchanged, then repeat. No extra phone
handling, app/video test or automatic network change is required.

## 11. Router/default DNS with phone back on hotspot — slowdown remains

User confirmed reconnecting the phone to the PC hotspot. ADB independently
verified `192.168.137.241`, hotspot association on 5180 MHz/channel 36, RSSI
about -29 dBm and 433 Mbps Tx/Rx reports. PC DNS remained router-provided
(`192.168.1.1`, with IPv6 `fe80::1`); no PC host process was running.

Runs:

- `bench/logs/networking/20261022-202500-A-DNS-router-hotspot-restored-r1/`.
- `bench/logs/networking/20261022-202713-A-DNS-router-hotspot-restored-r1-sourcebound/`.

| Check | Result with router/default DNS and hotspot client restored |
|---|---|
| PC -> upstream gateway | 27/30 replies; median 74 ms, p95 475 ms, maximum 546 ms |
| PC -> external target | 23/30 replies; median 279 ms, p95 588 ms, maximum 641 ms |
| HTTPS | 3/3 HTTP 200; total ~771–1677 ms |
| Phone -> PC hotspot | 29/30 replies; mean 15.51 ms, maximum 113.77 ms |
| PC -> phone hotspot | 30/30 replies; median 25 ms, p95 81 ms, maximum 82 ms |
| Phone -> external target through hotspot | 25/30 replies; mean 351.79 ms, maximum 844.63 ms |
| Local TCP payloads | Both 2 MiB transfers completed at the imposed ~8 Mbps rate |
| PC gateway probes spanning transfer phase | 9/10 replies; median 382 ms, maximum 628 ms |
| PC external probes spanning transfer phase | 7/10 replies; median 426 ms, maximum 764 ms |

These retain the earlier sampling, TCP-pacing, ICMP and mixed-phase caveats.
No maximum-bandwidth or video-quality claim is made.

**Finding:** leaving DNS to the router did not remove the observed problem when
the original phone-on-PC-hotspot condition was restored. Under the same reported
DNS configuration, the earlier phone-direct-to-router observation was much more
responsive (gateway median 1 ms, no missed replies), whereas this restored
hotspot observation degraded again with PhoneCam closed. This strengthens the
case for investigating the upstream/hotspot combination, rather than treating
DNS as a demonstrated fix. It is not yet a randomized/repeated isolation of
adapter concurrency, router/RF conditions, background traffic or driver behaviour.

Next proposed variable: the user's b/g/n setting, with original/new values
recorded and router/default DNS held fixed. That would test the mode change
relative to this post-DNS control, not claim a pure original-mode-only comparison
against the pre-DNS baseline. Return-to-B confirmation remains outstanding.

## 12. Router b/g/n update — link rate rises, problem remains

The user reported the Wi-Fi mode change with a router settings screenshot. It
shows **2.4 GHz transmission mode 802.11b/g/n**, transmit power **100%**, channel
**Auto**, WMM checked (greyed), 11n MCS **Auto**, bandwidth **20/40 MHz**, and guard
interval **Long**. The screenshot itself shows selected UI values, not a separate
saved-configuration export. The original router values were not captured, so
do not assert that every other router control was independently held constant.
The mode-change location is now identified as the router's 2.4 GHz settings,
not the PC adapter's ax/ac/n property.

Read-only checks still found router/default PC DNS and the phone on the original
PC hotspot at `192.168.137.241`, **5180 MHz/channel 36**, RSSI around -28 dBm and
433 Mbps Tx/Rx reports. No PhoneCam host was running. The PC's upstream adapter
reported **300 Mbps before and after** the saved run, versus 54 Mbps in the
previous A runs. This confirms a changed link-rate report, not maximum capacity.

Runs:

- `bench/logs/networking/20261022-204053-A-DNS-router-bgn-r1/`.
- `bench/logs/networking/20261022-204255-A-DNS-router-bgn-hotspot-r1-sourcebound/`.
- Four-condition summary:
  `bench/logs/networking/20261022-204547-all-conditions-comparison.json`.

| Check | Result after router/default DNS + b/g/n update |
|---|---|
| PC -> upstream gateway | 30/30 replies; median 51 ms, p95 108 ms, maximum 116 ms |
| PC -> external target | 25/30 replies; median 242 ms, p95 321 ms, maximum 349 ms |
| HTTPS | 3/3 HTTP 200; total ~795, 865 and 3177 ms |
| Phone -> PC hotspot | 30/30 replies; mean 29.91 ms, maximum 110.73 ms |
| PC -> phone hotspot | 30/30 replies; median 51 ms, p95 109 ms, maximum 114 ms |
| Phone -> external target through hotspot | 28/30 replies; mean 365.32 ms, maximum 600.21 ms |
| Local TCP payloads | Both 2 MiB transfers completed at the imposed ~8 Mbps rate |
| PC gateway probes spanning transfer phase | 10/10 replies; median 48 ms, maximum 122 ms |
| PC external probes spanning transfer phase | 9/10 replies; median 200 ms, maximum 276 ms |

Gateway reply count and delay look better in this sample than in the preceding
post-DNS A run, but remain worse than the first B control (gateway median 2 ms).
External missed replies and poor responsiveness persist. With one sample per
condition and no return-B yet, this is not proof of a complete fix or a precisely
isolated mode-change benefit. No network/application defaults were changed by
the assistant, and no video experiment was run.

### Additional clue: phone Wi-Fi counter deltas

The recorded `cmd wifi status` before/after values yield the following deltas
across each complete phone ping/transfer collection:

| Condition | Successful Tx delta | Retried Tx delta | Lost Tx delta |
|---|---:|---:|---:|
| B: uncle | 1216 | 12 | 0 |
| A: original | 1288 | 352 | 3 |
| A: router DNS, hotspot restored | 1809 | 535 | 13 |
| A: router DNS + b/g/n | 1357 | 290 | 7 |

Counters remained monotonic within these windows. These are driver/vendor
counters, **not RTP losses or a calculated packet-loss percentage**. Window
durations, received traffic and device activity differ; do not compare raw
counts as normalized rates. They nevertheless warrant attention: the phone's
local hotspot radio reported substantially more retries in these A observations
than in B, despite staying on the same 5 GHz hotspot/channel. This supports
further upstream/hotspot interaction investigation, not a claim that the local
path is fault-free merely because reliable paced TCP transfers completed.

Next: return only the PC upstream connection to B and repeat the controls before
making further configuration changes. Keep the phone on the PC hotspot and ADB
connected; verify the association again because a previous switch moved it.

## 13. Return to Network B — idle PC recovery, later hotspot instability

The user confirmed returning the PC to the uncle network. ADB verified the phone
was still on the PC hotspot (`192.168.137.241`, 5180 MHz/channel 36, 433 Mbps
Tx/Rx reports), with RSSI roughly -21 dBm initially and -23 dBm at the end of
the hotspot run. PhoneCam was not running. No video or configuration change
was introduced by the assistant.

Runs:

- Initial PC return control:
  `bench/logs/networking/20261022-205157-B-uncle-return-r1/`.
- Subsequent hotspot stage:
  `bench/logs/networking/20261022-205246-B-uncle-return-hotspot-r1-sourcebound/`.
- Later short idle-recovery observation:
  `bench/logs/networking/20261022-205629-B-return-idle-recovery-r1/`.

### The initial PC internet/gateway check recovered

| Check | Return-B result |
|---|---|
| PC -> upstream gateway | 30/30 replies; median 1 ms, p95 8 ms, maximum 9 ms |
| PC -> external target | 30/30 replies; median 47 ms, p95 56 ms, maximum 58 ms |
| Small HTTPS checks | 3/3 HTTP 200; total ~448–494 ms |

The PC's DHCP-advertised IPv4 DNS on B was `62.240.110.197` / `62.240.110.198`,
with no IPv6 DNS listed. This differs from A's advertised router DNS and reflects
the network supplying the automatic client configuration. It is not evidence of
the assistant manually changing DNS.

### The subsequent hotspot collection was NOT a clean recovery

| Check | Return-B hotspot-stage result |
|---|---|
| Phone -> PC hotspot | 29/30 replies; mean 53.08 ms, maximum 335.79 ms |
| PC -> phone hotspot | 30/30 replies; median 63 ms, p95 281 ms, maximum 374 ms |
| Phone -> external target through hotspot, before TCP transfers | 9/30 replies; mean 1815.29 ms, maximum 4154.45 ms |
| PC -> phone capped TCP payload | All 2 MiB received; ~3.031 s, ~5.536 Mbps achieved against the 8 Mbps cap |
| Phone -> PC capped TCP payload | All 2 MiB received; ~2.098 s, ~7.997 Mbps at the imposed cap |
| PC gateway probes spanning transfer phase | 0/10 replies; raw output contains request timeouts |
| PC external probes spanning transfer phase | 0/10 replies; raw output contains request timeouts |

The poor phone-internet phase occurred **before bulk TCP transfers started**.
Do not claim the transfers alone caused the entire episode. The transfer-phase
probe windows also include idle time and are not exclusively loaded intervals.
The earlier nominally good B sample did not reproduce this severity; retain both
results instead of discarding the inconvenient return control.

Phone Wi-Fi counter deltas over the return hotspot collection were successful
Tx **1890**, retried Tx **587**, lost Tx **24**, successful Rx **2315**. As with
the preceding counter table, these are vendor counters over mixed traffic and
unequal windows, not packet-loss percentages. The return shows that elevated
hotspot retry/loss counters are **not exclusive to Network A**.

### Later idle recovery, with no more TCP transfer traffic

The assistant stopped generating transfer traffic when the bounded tests ended,
then took five light PC probes per destination in a subsequent observation:

- Gateway: **5/5 replies**, median **2 ms**, range 1–3 ms.
- External target: **5/5 replies**, median **49 ms**, range 48–53 ms.
- PC Wi-Fi link report: **866.7 Mbps** at this later snapshot; it had reported
  19.5 Mbps at return preflight and 26 Mbps before the hotspot phase. These
  widely varying reports must not be mistaken for measured maximum throughput
  or used alone to identify a radio band, channel or cause.

This shows recovery in a later short idle observation, **not immediate recovery
at a precisely measured delay**, nor proof that an entire route disconnected.

### Updated interpretation and stopping point

1. The user's usual-network problems are accepted and were measured with video
   closed. Router DNS and b/g/n changes did not fully resolve the measured issue.
2. The first PC return-B check recovered, consistent with an upstream-dependent
   component. But B was not consistently healthy during the subsequent hotspot
   stage. A blanket conclusion that only the usual router is faulty is unsupported.
3. Shared-adapter upstream/hotspot interaction, changing RF/link conditions,
   roaming/band/channel behaviour, driver scheduling, and other time-varying
   contention remain candidates. The measurements do not choose one yet.
4. No maximum-capacity test, video-corruption retest, driver update, firewall
   change or automatic network switch was performed. All temporary payload
   listeners have ended. No further active probe/transfer job is left running.
5. The next useful isolation, if agreed, would hold one upstream network fixed
   and compare hotspot/client states, with synchronized adapter/channel/event
   evidence. That is a new controlled step, not another request to demonstrate
   the user's video observations or blindly change more DNS/router settings.

## 14. Standalone upstream-disconnected hotspot collector

The user explicitly requested a command/script to gather insights while the PC
is disconnected from **both upstream networks**, but its **Wi-Fi radio remains
enabled**, **mobile hotspot stays on**, and **phone stays on that hotspot**.
USB ADB remains connected for control/diagnostics, not networking. This tests
the isolated local hotspot path without treating the expected internet outage
as a failure or requiring the assistant/chat to remain connected.

Run this in a local PowerShell terminal **before or after disconnecting**:

```powershell
python D:\work\phonecam\bench\hotspot_offline.py
```

Then:

1. Leave PhoneCam closed on PC and phone, USB debugging authorized, and mobile
   hotspot enabled. Rest the phone; no handheld scene or camera test is needed.
2. If not already disconnected, in Windows Wi-Fi controls, **Disconnect from the current upstream network**.
   Do not turn off Wi-Fi, turn off mobile hotspot, or connect to the other router.
   Keep the phone joined to the PC hotspot despite the expected lack of internet.
3. The script waits up to 180 seconds if this manual change is still needed.
   It checks that Wi-Fi is administratively enabled but disconnected upstream,
   and that the hotspot interface/address remains Up. It records retained
   IPv4/IPv6 default routes on interfaces confirmed disconnected, but does not
   treat those as live uplinks. Active or unknown-interface routes still block.
   It does not disconnect, delete routes or make settings changes itself.
4. Once offline, it checks the phone association/source address, collects 30
   local ping samples each way, and runs the same capped local TCP transfers:
   at most 2 MiB per direction at an offered 8 Mbps. Ten local PC-to-phone pings
   span that transfer phase. It records PC/phone state at phase boundaries.
5. Wait until the terminal prints **DONE** or **STOPPED**. Healthy collection
   usually takes about a minute after disconnect detection; timeouts can take
   longer. Do not reconnect partway through a run. Ctrl+C stops collection and
   retains available partial results.
6. Reconnect the PC to the working upstream network and tell the assistant
   **"offline test finished"**. The assistant can read the files already on disk;
   no screenshots or manual transcription are required.

Outputs: a timestamped directory ending `-offline-hotspot` under
`bench/logs/networking/`, printed in the terminal. `results.json` is updated at
checkpoints; `summary.json` is written on completion. The disconnect history,
IPv4/IPv6 routes, adapter states/rates/counters, phone Wi-Fi status/address/routes
and collector SHA256 hashes are saved locally. Logs may include private SSIDs
and device identifiers; keep raw files out of Git/public reports.

There are **no internet, DNS, video, router, firewall or driver-setting tests or
changes** in this script. Temporary phone listeners are time-bounded and use
hotspot IP addresses; only generated zero payloads are transferred. The script
does not create a PC listening socket or require a firewall rule. TCP may hide
wireless retries, so successful capped transfers are not maximum-capacity or
zero-loss claims.

The script refuses a changed condition at phase boundaries, for example phone
roaming to the router, hotspot shutting down, a different phone address, an
active/unverified upstream route returning, or PhoneCam running. These sampled checks do not
prove continuous state at every instant. This offline workload omits the
online collector's phone-to-internet ping; consider that workload/power-state
difference when interpreting local latency.

Validation: **20 offline regression tests passed**, including disconnected
station/hotspot-active acceptance, IPv4/IPv6 default-route rejection, radio-off
and hotspot-off rejection, phone-association and ADB-failure handling. A real
preflight while still online correctly stopped without sending probes or
transfers (`20261022-210552-199475-offline-hotspot`). At this preparation stage
the actual offline test had not run; its subsequent result is in section 15.
A state-only diagnostic is also available:

```powershell
python D:\work\phonecam\bench\hotspot_offline.py --inspect
```

### Offline-detection correction after the user's first attempt

Attempt `bench/logs/networking/20261022-210912-688014-offline-hotspot/`
correctly had physical Wi-Fi **AdminStatus Up / Status Disconnected /
MediaConnectionState Disconnected**, hotspot interface 4 **Up**, and phone
`192.168.137.241` on the hotspot. Windows nevertheless retained an IPv4 default
route on disconnected Wi-Fi interface 10. The first script incorrectly blocked
on the existence of any default route. The user interrupted its wait; no ping
or transfer stage ran. **The user had disconnected correctly.**

Fixed: correlate each default route with its interface state. A retained route
on a confirmed disconnected interface is recorded, not treated as an active
uplink. Unknown-interface routes, live Ethernet/VPN routes, upstream Wi-Fi
association, disabled radio and an inactive hotspot remain blockers. All adapter
states—not just physical adapters—are collected for route classification. No
route deletion, IPv6 disabling or DNS change is needed for this correction.

Validation after the fix: **26 regression tests passed**. Replaying the actual
saved initial state and all four disconnected observations now accepts the
requested condition. A read-only `--inspect` run also passed
(`20261022-211852-160005-offline-hotspot`). The corrected collector can start
already disconnected; to refuse
immediately rather than wait if an active uplink remains, use:

```powershell
python D:\work\phonecam\bench\hotspot_offline.py --wait-offline 0
```

## 15. Completed standalone offline hotspot result

The user ran the corrected command after disconnecting the PC's upstream Wi-Fi
and supplied its DONE output. The assistant inspected the saved full results:
`bench/logs/networking/20261022-212256-047947-offline-hotspot/`.

### Requested topology was present at every sampled phase boundary

- Physical PC Wi-Fi: **AdminStatus Up**, **Status Disconnected**,
  **MediaConnectionState Disconnected**. Ethernet was also disconnected.
- Hotspot virtual interface 4 remained **Up**, at `192.168.137.1`.
- One old IPv4 default route via `192.168.1.1` remained on disconnected interface
  10. It was recorded as inactive, not deleted or mistaken for a live uplink.
  There was no IPv6 default route or identified active/unknown default route.
- Phone stayed on the PC hotspot at `192.168.137.241`, **5180 MHz/channel 36**,
  with **433 Mbps Tx/Rx** reports. RSSI snapshots ranged about **-33 to -21 dBm**;
  it was not held numerically identical between samples.
- No PhoneCam process, external probe, DNS lookup, upstream association change,
  firewall change or settings change was introduced by the collector.

The physical adapter's retained "300 Mbps" report while disconnected is **not**
an active upstream data rate. Sampled boundaries do not prove every instant of
the interval; all retained raw snapshots are available for review.

### Measured local results

| Check | Result |
|---|---|
| PC -> phone, 30 local ping probes | 28/30 replies; median 173 ms, p95 696 ms, maximum 735 ms |
| Phone -> PC, 30 source-IP-bound local probes | 20/30 replies; mean 63.98 ms among replies, maximum 194.63 ms |
| PC -> phone TCP payload | All 2 MiB received and counted on phone, ~2.098 s at the imposed ~8 Mbps rate |
| Phone -> PC TCP payload | All 2 MiB received on PC, ~2.098 s at the imposed ~8 Mbps rate |
| PC -> phone probes spanning the transfer phase | 10/10 replies; median 92 ms, p95/maximum 537 ms |

Phone Wi-Fi vendor-counter changes separate the low-load probe phase from the
later bounded transfer phase:

| Phase | Successful Tx delta | Retried Tx delta | Lost Tx delta | Successful Rx delta |
|---|---:|---:|---:|---:|
| Local ping phase | 112 | 549 | 25 | 50 |
| Transfer phase plus intervening snapshots | 1180 | 56 | 2 | 2039 |

These are raw monotonic counter deltas, not normalized packet-loss percentages
or equivalent-duration traffic comparisons. Nevertheless, the counter increases
and poor ICMP responses were already present **before** the bulk transfers.
This is not evidence that the 8 Mbps transfer alone saturated the hotspot.

### What this narrows down—and what it does not

1. The observed poor **local** ping behaviour does not require association with
   either upstream router, internet traffic forwarding, DNS resolution or an
   active video stream. Those are not necessary conditions for this local symptom.
2. This does **not** establish that the usual router has no separate problems,
   or that its association cannot worsen the situation. It does rule out using
   upstream DNS/router forwarding as the sole explanation for all observations.
3. The remaining local candidates include Windows hotspot/MT7921 driver behaviour,
   background scanning/scheduling, phone Wi-Fi power saving, RF interference and
   client/AP interoperability. Strong reported RSSI does not rule out interference.
   No one of these has been proved responsible.
4. Reliable TCP delivered the small paced payloads. Therefore ICMP reply loss must
   not be generalized to an identical TCP/application loss rate, and the successful
   8 Mbps checks must not be called measured maximum capacity or proof of zero
   retransmission. Sparse-packet/power-state behaviour merits isolation.
5. No further active job remains. If more diagnosis is requested, isolate one local
   variable at a time—such as recorded phone awake/asleep state or another available
   hotspot client—before broad driver/router/channel changes. Do not ask the user
    to demonstrate the previously accepted video symptom again.

## 16. Offline PC-hosted goal, phone-hosted contrast and online research

The user clarified the actual goal: **a fast, reliable local PC-hosted hotspot
connection to the phone without internet**, utilizing the available wireless
capacity. Internet sharing is not a requirement. The user also reports having
tried the **phone's hotspot without internet**, with performance **close to cable**.
Accept this observation; it is not a controlled, instrumented throughput or
optical measurement. It reverses AP/client roles and may also change band,
channel, width, power saving and driver behaviour. Thus it strengthens the case
for investigating the PC-hosted path, not a claim that either radio is faulty.
The earlier A/B topology statements apply to those historical router comparisons,
not to this separately reported phone-hosted experiment.

**8 Mbps was our collector's deliberate offered-load cap, not a measured hotspot
ceiling.** Neither 300 Mbps nor 433 Mbps reported PHY rate guarantees that amount
of useful application throughput. The next agreed measurement should address
actual local delivery, delay variation and loss under controlled load, rather
than only internet behaviour or idle ICMP. No larger-load test is authorized or
running by this research request.

Sources checked online on 2026-10-22:

1. [Microsoft's local Wi-Fi Direct legacy AP sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/wifi-direct-legacy/)
   explicitly supports a Windows PC acting as an AP for ordinary clients to
   communicate over TCP/UDP, without providing internet cross-connectivity.
   This is a supported architectural candidate for an offline-only PC-hosted
   connection, not a demonstrated performance fix. It still depends on the same
   radio/driver. [Microsoft's Wi-Fi Direct overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/partnerapps/wi-fi-direct)
   says Mobile Hotspot and the legacy Wi-Fi Direct group owner cannot operate
   simultaneously; any comparison must explicitly stop one before starting the
   other, with user agreement. Do not use obsolete `netsh hostednetwork` recipes
   as if they represented this modern API.
2. [Microsoft Wi-Fi power-management design guidance](https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/wi-fi-power-management-for-modern-standby-platforms)
   documents a latency cost from Wi-Fi power saving. Its station/Modern Standby
   discussion is not proof of our hotspot mechanism or a guaranteed Device
   Manager fix. A [community Wi-Fi Direct report](https://learn.microsoft.com/en-us/answers/questions/34630/wifi-direct-power-save-mode)
   describes approximately 100 ms power-save interruptions even during iperf;
   it has no technical resolution. Treat it as an anecdote, not vendor confirmation.
3. [Windows hotspot local VR streaming report](https://learn.microsoft.com/en-us/answers/questions/4094135/windows-11-mobile-hotspot-slow-and-unstable-speed)
   describes unstable local rates and periodic stutter on Intel hardware.
   [An ASUS community MT7921 report](https://rog-forum.asus.com/t5/gaming-notebooks/rog-zephyrus-duo-16-2023-pinging-local-router-gives-me-300ms-not/td-p/953064)
   describes local latency spikes with internet blocked, with only partial
   improvement after a driver rollback. These establish analogous user reports,
   not a universal Windows/MediaTek defect or a known-good driver for this laptop.
   Linux MT7921 module/ASPM workarounds do not directly apply to Windows.
4. [Microsoft's current wireless troubleshooting guidance](https://learn.microsoft.com/en-us/troubleshoot/windows-client/networking/wireless-network-connectivity-issues-troubleshooting)
   recommends preserving evidence before resets and comparing with manufacturer-
   recommended drivers/firmware. A sensible first preparation is read-only laptop
   model, OEM driver and power-policy inspection, followed by an explicitly agreed,
   reversible one-variable power or OEM-driver comparison. No blind third-party
   driver download, registry bundle or network reset is justified by these sources.
5. [Windows WLAN interface API documentation](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/ne-wlanapi-wlan_intf_opcode)
   limits disabling background scans and enabling media-streaming mode to a
   connected interface; disconnection automatically restores scanning/disables
   streaming mode. These are not established fixes for our upstream-disconnected
   hotspot. Do not stop WLAN AutoConfig or bypass location permission as a shortcut.
6. [Windows 11 24H2 tethering priority API](https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.tetheringwifiperformancepriority?view=winrt-26100)
   concerns station/hotspot band interference, specifically documented 5 GHz
   station versus 6 GHz hotspot constraints. It is **not** a generic bandwidth
   unlock or demonstrated fix for the offline 5 GHz link to this phone.

Recommended direction: prioritize the offline PC-hosted requirement. First
inspect OEM/power options without changing them, then choose one reversible
comparison. A separately approved local-only Wi-Fi Direct AP prototype is an
alternative implementation experiment if Mobile Hotspot remains unsuitable,
with no performance promise. A role-reversal measurement can use the user's
phone-hosted success as a reference if needed, not as a substitute for the goal.
No setting, driver, application code or network topology was changed for this
research; no video retest was requested.

### Fast-path inspection and user acceptance criterion

The user requested minimal questions and clarified that **their PhoneCam test
is the acceptance criterion**. They will wait until told the setup is ready;
synthetic ping/throughput improvements alone must not be called a successful fix.
No prolonged handheld observation is needed.

Read-only inspection identified ASUS TUF Gaming A15 **FA507RC**. Current Windows
registry reports build **26300.9711**, DisplayVersion **26H2** (the legacy
ProductName string says Windows 10 and is not sufficient to identify the OS).
The active Performance plan already requests **Maximum Performance** for Wi-Fi
on both AC and DC; U-APSD is already **Disabled**. Transmit power is Highest,
preferred band 5 GHz and both channel widths Auto. These settings do not need
redundant changes, nor do they prove all firmware power saving is disabled.
`Get-NetAdapterPowerManagement` failed with a device-not-functioning message;
that query error alone does not establish hardware failure.

The [official ASUS FA507RC support page](https://www.asus.com/supportonly/fa507rc/helpdesk_download/)
currently lists **V3.00.01.1338**, published 2026/06/22, matching installed
**3.0.1.1338** (driver date 2026-04-07). Older **3.00.01.1325** is also listed.
There is no verified newer model-specific OEM update to recommend from this
page. Do not blindly reinstall the same version or label an older version a fix.

The adapter exposes an immediately reversible **802.11ax -> 802.11ac** option
(`CurrPhyMode` 0 -> 1). This is a short compatibility experiment, **not** an
online-verified remedy for this particular offline hotspot. It retains Wi-Fi 5
operation supported by the Redmi Note 8; it is not a bandwidth-unlock claim.
The current roaming setting is Medium-High; a roaming adjustment has no proven
benefit for the already upstream-disconnected scenario and is not bundled in.
Any adapter change/restart can interrupt the current internet/hotspot and needs
one explicit approval under the existing no-automatic-settings-change constraint.

Current topology differs from the earlier saved offline run: Wi-Fi is Up,
virtual adapter #3/interface 39 is Up and former #2/interface 4 is Disabled.
The cause/time of that change and current upstream identity were not established;
do not overwrite the current topology or assume the old virtual adapter index.
No network setting, driver or PhoneCam code was changed during this inspection.

## 17. Authorized ac compatibility trial — ready for user evaluation

The user explicitly approved saving the current adapter setting and temporarily
changing **802.11ax -> 802.11ac**, including the possible Wi-Fi/hotspot interruption.
Original state and restoration command were saved locally in:
`bench/logs/networking/20261022-215855-063408-ac-compatibility-trial/`.

The initial non-elevated change failed with Access Denied. The narrowly scoped
local `apply-ac.ps1` was then run through the normal Windows UAC elevation flow;
it validated adapter identity and the original value before applying the change.
The elevated result reports success, and the later readback confirms
**CurrPhyMode = 1 / 802.11ac**. Comparison against all saved advanced settings
confirms **this was the only changed advanced property**. Roaming, channel width,
preferred band, power options, driver and application settings were left alone.

The adapter restart briefly removed the active hotspot interface. A subsequent
read-only check found Wi-Fi Up again and the original virtual adapter #3/index
39 **Up**, with the PC hotspot address **192.168.137.1** restored. The assistant
did not select an upstream network, manually reconnect Wi-Fi or toggle the
hotspot. The returned upstream identity was not independently identified.

Read-only phone readiness queries failed through USB ADB (exit code 1), so phone
association, band, address and PhoneCam process state are **not verified** after
the change. This is not a failed wireless performance test. No pings, transfers
or video captures were run; do not require reconnecting USB solely for the
user's acceptance test. The PC host and service were observed running and were
not restarted or stopped by this trial.

**Ready for the user's requested trial, not a claimed fix:** leave the phone
on/reconnect it to the PC hotspot if needed, disconnect only the upstream
association for the offline goal while leaving Wi-Fi/hotspot enabled, and try
PhoneCam normally with the phone resting. Compare the user's perceived image
quality/responsiveness; do not substitute synthetic scores for their verdict.
The user subsequently reported **"same"**: the ac trial did not improve their
PhoneCam experience. No new video capture or synthetic score overrides that
verdict. The original **ax / CurrPhyMode 0** setting was restored through normal
UAC elevation and verified; `restore-result.json` records success. Wi-Fi and the
hotspot interface 39/address 192.168.137.1 were Up in the later read-only check.
**Do not retain or recommend ac mode as a demonstrated fix.**

The restoration command, retained for reproducibility, was:
under the user's authorization rather than retaining an unearned default:

```powershell
# Administrator PowerShell; also briefly restarts Wi-Fi/hotspot.
Set-NetAdapterAdvancedProperty -Name 'Wi-Fi' -RegistryKeyword 'CurrPhyMode' -RegistryValue '0'
```

### Next narrowly scoped candidate, not yet applied

Read-only `pnputil /enum-drivers /class Net` shows both the current MediaTek
**3.0.1.1338 / oem36.inf** and the older **3.0.1.1325 / oem114.inf** already in
the local Windows driver store. The latter version is also listed on the ASUS
FA507RC support page. Its local INF contains a matching hardware ID for this
adapter. This avoids third-party downloads, but does **not** establish that the
older driver performs better. Other non-MediaTek network packages found in the
store were not changed or blamed.

A driver comparison is a separate, more disruptive action requiring explicit
approval, a saved current package/configuration and a restoration path. No driver
was installed, removed, forced, or rolled back during these checks.

The user chose **Stop here** when offered that driver comparison. The current
3.0.1.1338 driver and restored ax mode were retained; no rollback was authorized.

## 18. User-reported uncle-network check after stopping changes

The user reported **"it's uncle again"** and requested a network check. Saved
read-only snapshot:
`bench/logs/networking/20261022-221043-071721-B-uncle-current-check/state.json`.

- Upstream Wi-Fi was **Up / Connected**, PC `192.168.1.4`, gateway
  `192.168.1.1`; DNS `62.240.110.197` / `62.240.110.198` matches earlier B
  configuration. Network B identity remains **user-reported**, not independently
  confirmed by SSID: the standard Windows WLAN query was still denied by
  location permissions, which were not changed or bypassed.
- Upstream reported link speed **58.5 Mbps**; hotspot virtual interface #3/index
  39 **Up / Connected**, address `192.168.137.1`, link report **390 Mbps**.
  Neither value measures useful throughput or current latency.
- A read-only Windows tethering-manager query confirmed **hotspot On, one
  associated client**, reporting `192.168.137.83`. Its hostname was unknown;
  it was not independently identified as the phone. Do not hard-code historical
  phone address `.241` for this condition. Neighbor-cache enumeration initially
  returned no active entries, so that alone would have missed the client.
- Adapter mode remains **802.11ax**. No new setting or driver change was made.
- USB ADB listed no attached device, so phone-side band/counters were unavailable.
- Installed `phonecam-host.exe` was running with UDP endpoint 27185, but no
  host-owned TCP connection was found at that sampled instant. This does not
  establish a healthy active PhoneCam session or video quality.

No ping, TCP payload, internet application probe or video capture was run in
this state-only check; the existing PhoneCam-closed constraint was retained.
This confirms the current network topology/configuration, **not** renewed good
latency or the user's perceived stream performance. No test process was left running.

## 19. User stops network comparisons and chooses USB

The user explicitly said to skip the preceding network checks, depend on USB,
and asked whether hypothesis **4: persistent H.264 corruption/recovery** has a
solution. Network diagnosis is paused; do not request another router switch,
hotspot comparison, driver trial or corruption demonstration for it.
The user then clarified that USB is a fallback, **not** cancellation of possible
hotspot-path code improvements, and authorized offline development followed by
one final user hotspot trial. This clarification supersedes any implication
that software recovery work should stop; see section 20.

There is a concrete application-level recovery strategy: reject damaged or
incomplete access units, stop feeding dependent pictures with invalid references,
request a fresh IDR, reset/gate the decoder as appropriate and resume clean output
at a validated recovery point. Existing keyframe requests/NAL-boundary handling
are partial protection, not a proven live solution for the captured corruption.
This policy bounds propagation; it does not repair missing wireless bytes and
can replace corruption with a brief freeze/drop while waiting for recovery.

USB avoids this particular Wi-Fi path but does not intrinsically fix every
possible encoder, framing or decoder defect. Use the existing USB workflow first
(USB tethering preferred when available, existing ADB fallback supported). If the
user's USB picture is clean, keep current codec defaults; do not add speculative
codec changes or claim that the wireless root cause was established. Any future
recovery implementation is separate work, not performed by this clarification.

## 20. Concrete RTP recovery fix — implemented, PC-only validated and deployed

### Scope and identified code defect

The user authorized improvements without connecting the phone, followed by
**one** user hotspot trial. USB remains the dependable fallback. No more router,
driver, power or upstream comparisons were requested or performed.

Inspection of `windows/host/transport/RtpVideoTransport.cpp` confirmed that loss
handling discarded assembly and requested a keyframe, but cleared its desync
state at the next NAL boundary. It could therefore deliver dependent P pictures
with damaged reference history, or surviving slices of a damaged IDR. These are
concrete receiver defects; they do **not** prove the exact packet history of the
earlier live corruption or identify the underlying Wi-Fi instability.

The initial new PC-only suite reproduced **eight failing recovery cases** against
the old receiver, with the duplicate/sequence-wrap control passing. No real phone,
camera, hotspot traffic or private image was involved.

### Changed behaviour

- Startup/new SSRC and any detected loss require a complete **IDR with SPS/PPS
  and its first slice** before delivering more video. The existing Android RTP
  sender already repeats SPS/PPS with each keyframe, so no phone update is needed.
- A sequence gap quarantines the entire affected RTP timestamp. Later slices of
  that same damaged IDR cannot falsely release recovery.
- The first recovered frame marks `VideoPacket.discontinuity`, using the existing
  bridge decoder-reset path to discard old decoder references. Parameter sets are
  carried in that resumed IDR; stale cached parameters are not substituted.
- Incomplete/malformed FU-A, changed fragment headers/timestamps, interrupted
  fragments, malformed STAP-A and invalid picture boundaries are rejected rather
  than forwarding an intact-looking prefix as a complete picture.
- Recovery requests are throttled and retried as further nonrecoverable pictures
  arrive. No fixed jitter buffer, retransmission queue or extra image latency was
  added; decoder/encoder/media defaults and all USB transports are unchanged.

**Trade-off:** hold the last clean image while awaiting a recoverable keyframe
instead of displaying damaged dependent pictures. With sustained severe wireless
loss, freezes can still persist; this is corruption containment/recovery, not
a promise of smooth bandwidth or a repaired radio/driver.

### Validation

`phonecam-rtp-recovery-tests` now covers **15 cases**: P frames after a gap,
damaged multi-slice IDR, missing marker, fragment timestamp changes, malformed
STAP, orphan fragments, missing recovery parameters, mid-GOP join, duplicates
and sequence wrap, healthy fragmented/multi-slice IDR, healthy STAP, bad FU
marker, changed FU header, NAL interrupting FU, and new-SSRC recovery.

- Final Debug and Release host builds passed.
- All 15 packet cases and existing video clock/RTP startup tests passed in both
  configurations. The Release packet suite also passed **10 consecutive runs**.
- Release media-settings isolation tests and **26 Python regression tests** passed.
- Added synthetic loopback RTP -> actual `MFH264Decoder` -> decoded NV12 hash
  replay. Five scenarios passed at **640x360** and **1920x1080 / 30 fps**; the
  1080p fixture used requested 6 Mbps, four slices and a 30-frame GOP. This is
  paced synthetic testing, not a throughput or live recovery-latency measurement.

| Synthetic scenario (120 reference pictures) | Delivered pictures | Mismatched delivered hashes | Recovery frame |
|---|---:|---:|---:|
| Healthy RTP | 120 | 0 | 0 |
| P-picture fragment lost at frame 45 | 105 | 0 | 60 |
| IDR fragment lost at frame 60 | 90 | 0 | 90 |
| IDR SPS packet lost at frame 60 | 90 | 0 | 90 |
| Receiver joins mid-GOP at frame 45 | 60 | 0 | 60 |

The resumed pictures match the complete-stream reference from the stated
recovery frame onward. Withheld frames are deliberately absent, not counted as
successful fresh pictures. These synthetic schedules do not establish wall-clock
live recovery speed. Artifacts:
`bench/logs/20261022-223019-rtp-recovery-fix/`.

### Installed candidate and remaining user acceptance

Only `C:\Program Files\PhoneCam\phonecam-host.exe` was replaced via normal
Windows UAC elevation and relaunched. Installed SHA256:
`BAF0D86B56FB07EDA010435594EB53A9CACC6F98BD32B5AEAC0672C564446638`.
The old host is preserved under
`windows/build/updates/rtp-recovery-20261022/phonecam-host-before.exe`, SHA256
`DFF9F6227D5744A722EBBB93E245341EA1757FA49619AB8F48BF80045BD700E6`.
Deployment hashes were verified, exported connection-registry snapshots match,
and the existing installed service stayed Running with its path/start mode intact.
No service/DLL/driver/phone/network/encoder settings were updated. The adapter
remains at original ax mode, and the PC hotspot address/interface remained Up.

The installed host is running with local stdout/stderr trial logs in that update
directory. No synthetic sender, temporary test receiver or capture remains active.
No device connection or Android build/install was needed for this Windows-only
change. No commits or publishing were performed.

**Next and only requested live action:** user joins the phone to the PC hotspot
and tries PhoneCam normally once, with the phone resting. No USB diagnostic cable,
network switch, router experiment or repeat corruption demonstration is required.
The user's perceived result is acceptance. They subsequently reported **no
pixelation, but freezing/lagging** (section 21); this was not a smoothness success.
Synthetic checks must not be presented as proof the live hotspot is fixed.

## 21. Live freezes after corruption containment; bounded reorder follow-up

### User verdict and existing log evidence

The user's one requested trial reported **"no pixelation. but it now freezes and
lags"**. Accept that report. This establishes perceived corruption improvement
for that trial, **not** an acceptable or generally fixed hotspot path. Do not ask
the user to reproduce it again or treat median decoder timings as disproving lag.

The saved installed-host log was inspected and archived without capturing video,
issuing probes or requesting another phone connection. Artifact:
`bench/logs/20261022-224259-hotspot-live-recovery-verdict/`.

- Raw log has **90 sequence-gap events**, totalling **94 skipped RTP sequence
  numbers**, **26 clean-IDR resumption messages** (including startup) and **23
  recovery-keyframe request messages**. These are log counts, not normalized loss.
- The last periodic cumulative snapshot reports **11,147 datagrams**, **14,869
  KiB**, **159 access units**, **70 loss events** and **380 pictures withheld**.
  More events followed that snapshot; do not mix it with final totals or sum
  cumulative snapshots as disjoint windows. No exact trial duration is inferred.
- A sequence hole in arrival order can be loss **or** late/reordered delivery.
  The first candidate had no reorder window and could not distinguish those.
  Calling all 94 sequence numbers confirmed over-the-air loss would be wrong.
- Bridge reference-relative delay values describe delivered samples, not time
  spent repeating the last clean picture; they are not end-to-end image latency.
- Log ended with stopped/reconnecting state. No agent-issued Start or new capture
  was sent during analysis or follow-up development.

### Additional PC-only improvement

`RtpReorderBuffer.h` now gives a sequence hole a **12 ms target deadline** and a
**64-packet capacity** before forwarding an unresolved hole to existing clean-IDR
recovery. In-order packets are released immediately; there is no deliberate
per-picture buffer. An OS scheduler can overshoot a timer target, so this is not
a hard real-time 12 ms latency guarantee.

The finite buffer handles sequence wrap, duplicates, SSRC resets and capacity
pressure. A socket `select()` deadline flushes a hole even if no further packet
arrives; disconnect still unblocks the receiver. Counters distinguish late packets
recovered, expired reorder gaps, late/duplicate ignores and capacity drops.
This may avoid **unnecessary** GOP freezes caused by slight reordering. It does
not retransmit or reconstruct genuinely missing packets, and cannot promise to
fix the user's live lag. The corruption guard remains enabled.

Validation:

- Debug/Release builds and existing video clock/startup checks passed.
- Receiver suite expanded from 15 to **17 cases**, including a reordered FU that
  must not request an IDR, and a real quiet-socket deadline test. Release suite
  passed ten consecutive runs.
- Pure bounded-reorder checks cover immediate in-order delivery, repaired late
  delivery, expiry without a new arrival, non-extending deadlines, sequence wrap,
  old/pending duplicates, bounded capacity/forced release and SSRC reset.
- Six synthetic **1080p/30** RTP -> actual MF decoder -> NV12 hash scenarios pass.
  Healthy and deliberately reordered streams both deliver **120/120 reference
  images with zero hash mismatches**. All prior genuine-loss scenarios still
  withhold unsafe pictures and recover to reference-matching pictures.
- No phone, Wi-Fi workload, Android update, encoder/USB/default media setting or
  private image was involved in this follow-up validation. The saved replay is
  offline evidence, not proof that the live sequence gaps were reordering.

### Deployment and stopping point

Only the installed PC host was updated again using normal UAC, preserving the
previous corruption-containment candidate and connection settings. Installed
SHA256: `010117C0C6FD63BB8F0083DCACDF8F74D4C3D77DA2D16A8B6D708EC8FD9E367C`.
Backup (`BAF0D86B...`), manifest, verified deployment result, matching before/after
connection-registry snapshots and new local host logs:
`windows/build/updates/rtp-reorder-20261022/`.
The pre-fix original remains in the earlier `rtp-recovery-20261022` directory.
Service, driver, adapter settings, phone APK and media defaults were unchanged.

**No additional phone trial is requested now.** The user may depend on USB;
hotspot improvement remains in scope, but live smoothness is unresolved and this
follow-up was not marked accepted at this stage. The user subsequently reported
it better but still lagging and stopped further work (section 22).
No synthetic test sender/capture remains active.

## 22. Final user verdict, retained state and authorized publishing

The user reported **"better but still lagging it doesn't worth the effort
anymore"**, requested that everything be documented, and authorized **commit and
push**. **Stop hotspot optimization and further live/network experiments here.**
USB is the dependable connection. This is not authorization to delete evidence,
undo improvements, change more settings or demand another phone test.

### Outcome, without overstating it

1. The reported pixelation was real. The clean-IDR safeguard removed perceived
   pixelation in the user's first trial, at the cost of visible freezes/lag.
2. The bounded-reorder follow-up was perceived as better, but remaining lag was
   not worth further effort. Treat this as **partial subjective improvement,
   failed smoothness acceptance**, not a controlled latency/throughput A/B win.
3. Retain the tested receiver safeguards and the USB fallback. Neither a precise
   network root cause nor a universally smooth hotspot configuration was found.
   Do not turn the documented hypotheses into a vendor/driver fault conclusion.
4. No encoder hint, changed IDR interval, bitrate or resolution earned adoption.
   Original media acceptance gaps and interrupted final Android validation remain
   explicitly unfinished; this closeout does not convert them into passed work.

### Final saved follow-up log

The reordering host log contains its new counters, confirming that candidate
ran during the user's follow-up. Archive and summary:
`bench/logs/20261022-225502-hotspot-final-closeout/`.

- Complete saved log: **49 sequence-gap events**, **58 skipped sequence numbers**,
  **16 clean-IDR resumption messages** including startup.
- Last periodic cumulative snapshot: **8,406 datagrams**, **11,207 KiB**, **124
  access units**, **45 loss events**, **288 withheld pictures**, **0 late packets
  recovered**, **45 reorder gaps expired**, **6 late/duplicate packets ignored**,
  **0 capacity drops**. More events followed that snapshot.
- Those are mixed-duration cumulative counts, not per-second rates or loss
  percentages. The two live trials were not equal-duration/workload comparisons.
- No recovered late packet in that sampled window does not establish that all
  missing data was over-the-air loss, or justify enlarging the window. The
  existing 12 ms deadline may exclude later delivery. The user chose to stop.
- Neither phone-reported fps nor the bridge's reference-relative delay measures
  fresh, smooth delivered pictures during a frozen interval.

At the final read-only process inspection, **no PC host process was found** and
the saved log ended in stopped/disconnected supervision. Its absence is not
attributed to a crash: the exit cause was not investigated, and the user was not
asked to restart anything. The assistant did not relaunch after the stop request.
The installed host file remains verified at SHA256
`010117C0C6FD63BB8F0083DCACDF8F74D4C3D77DA2D16A8B6D708EC8FD9E367C`.
Both earlier host backups, deployment manifests and unchanged connection-setting
snapshots remain local under `windows/build/updates/`. The ax mode is restored;
the user declined a driver rollback. No new setting change was made at closeout.

### Validation and publication boundary

Final closeout validation used **no phone/camera/hotspot workload**:
Debug/Release reordering unit checks, all **17 receiver regression cases**, video
clock/startup tests, media-setting tests, **28 Python tests**, PowerShell parsing
and `git diff --check` passed. Six synthetic 1080p image-hash scenarios had
previously passed for the final receiver. No final Android build or live smoothness
pass is claimed. No more collector, synthetic sender or capture remains running.

Publish the accumulated source, reusable measurement/diagnostic tools, regression
tests, sanitized summaries and this history. Preserve **raw logs, SSIDs, unique
device identifiers, pairing/registry exports and images locally**, not in Git.
The network phone helper's formerly hard-coded private serial was removed for
publication; it now selects the single USB device by default (`adb -d`) or accepts
an explicitly selected local `ANDROID_SERIAL`. That cleanup has two offline
tests and does not change the already collected results or installed phone app.

The authoritative next action is only the authorized **commit/push and stop**.
Any future hotspot investigation needs a new request; do not automatically resume
the historical scripts or acceptance tasks listed above.
