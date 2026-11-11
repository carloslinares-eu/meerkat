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
