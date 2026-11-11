# Test fixtures

Real output captured on a Debian 13 laptop (task 0.4), with `LC_ALL=C`.
Everything that could identify the person, the place or the provider has been
replaced before committing. The speed-test numbers (speeds, latencies, byte
counts, time) are made up; the Wi-Fi and ping numbers are real but generic.

| Replaced | With |
|---|---|
| MAC addresses | `02:00:00:00:00:01`, `02:00:00:00:00:02` (locally administered) |
| Wi-Fi SSID | `ExampleNet` |
| IPv4 addresses | `192.0.2.1`, `198.51.100.1` (RFC 5737) |
| IPv6 addresses | `2001:db8:…` (RFC 3849) |
| ISP, test server, server location | `Example ISP`, `speedtest.example.net`, `Example Server`, `Example City` |
| Ookla result id / URL | an all-zero UUID |

| File | Command |
|---|---|
| `iw/dev.txt` | `/usr/sbin/iw dev` |
| `iw/link_wifi6_160mhz.txt` | `/usr/sbin/iw dev <if> link` (Wi-Fi 6, 160 MHz, connected) |
| `ping/gateway_ok.txt` | `ping -c 5 -i 0.2 -W 1 <gateway>` (no loss) |
| `ping/loss_100.txt` | same, against an unreachable address (100 % loss, exit code 1) |
| `speedtest/ookla_ok.json` | `speedtest --format=json --accept-license --accept-gdpr` (Ookla CLI 1.2.0). `bandwidth` is in **bytes/s** |

Notes for the parsers:
- On Debian, `iw` lives in `/usr/sbin`, which isn't on a normal user's PATH.
- On the first run the Ookla CLI prints the licence and privacy text to stderr,
  even with the `--accept-*` flags. stdout carries only the JSON.

## Hand-made `/proc` and `/sys` trees (task 1.3)

`proc/<case>/net/{route,ipv6_route}` and `sys/class/net/<if>/` copy the exact
format of the real kernel files on carbon, with documentation addresses
(`192.0.2.0/24`, `2001:db8::/32`) instead of the real ones:

| Directory | Situation |
|---|---|
| `proc/wifi_only` | Default route over Wi-Fi only (how carbon is connected today), plus Docker routes |
| `proc/cable_and_wifi` | Cable (metric 100) and Wi-Fi (metric 600) both up: the cable must win |
| `proc/ipv6_only` | No IPv4 default; IPv6 default via `fe80::1` (reported as `fe80::1%wlp4s0`), plus the kernel's rejecting `::/0` on `lo` |
| `proc/offline` | No usable default route at all |
| `sys/class/net/wlp4s0` | Wi-Fi (`wireless/`, `phy80211`); no `speed` (reading it fails with EINVAL) |
| `sys/class/net/enp0s25` | Built-in Ethernet, cable plugged in, 1000 Mbit/s |
| `sys/class/net/enx0c3796000001` | USB Ethernet adapter, unplugged (`speed` = -1) |
| `sys/class/net/enp0s31f6` | Ethernet that is administratively down (`ip link set … down`): no readable `carrier` or `speed` |
| `sys/class/net/docker0` | Virtual bridge (no `device`) |
| `sys/class/net/lo` | Loopback (`type` 772) |

Directories that are symlinks in real sysfs (`device`, `phy80211`) are plain
entries here; meerkat only checks that they exist.
