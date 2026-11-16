#!/usr/bin/env python3
"""Writes a made-up meerkat data directory for the README's sample report.

Two weeks of speed tests on an imaginary line with the example tariff from
config.example.toml (250/200/150 down, 50/40/30 up): the download drops in
the evening, the upload holds. Plus connectivity probes with one outage, one
official-campaign pause, the raw logs and the daily manifests. All names and
addresses are placeholders (documentation IPs, "Example ISP").

Usage: make-sample-data.py DATA_DIR
Then:  meerkat report --data-dir DATA_DIR --from 2026-03-02 --to 2026-03-15 \
           --contract-down 250/200/150 --contract-up 50/40/30 \
           --connectivity-interval 1m -o sample-report.html
"""
import hashlib
import json
import random
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

FIRST_DAY = datetime(2026, 3, 2, tzinfo=timezone.utc)
DAYS = 14
SPEED_EVERY = timedelta(minutes=20)
PROBE_EVERY = timedelta(minutes=1)
UTC_OFFSET = 1  # the imaginary line is in Germany: local time is UTC+1 in March before the switch

SPEED_COLUMNS = ("time,status,error,backend,link_type,interface,gateway,link_speed_mbps,wifi_ssid,"
                 "wifi_bssid,wifi_frequency_mhz,wifi_signal_dbm,wifi_rx_bitrate_mbps,"
                 "wifi_tx_bitrate_mbps,download_mbps,upload_mbps,latency_ms,jitter_ms,"
                 "packet_loss_percent,server_id,server_name,server_location,isp,external_ip,result_url")
CONN_COLUMNS = ("time,probe,target,status,error,link_type,interface,gateway,sent,received,loss_percent,"
                "rtt_min_ms,rtt_avg_ms,rtt_max_ms,rtt_mdev_ms,duration_ms")
GATEWAY = "192.0.2.1"
HOSTS = ("1.1.1.1", "9.9.9.9")
OUTAGE = (datetime(2026, 3, 10, 13, 12, tzinfo=timezone.utc), timedelta(minutes=23))
PAUSE = (datetime(2026, 3, 12, 17, 0, tzinfo=timezone.utc), timedelta(hours=2))


def iso(t):
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


def download_for(local_hour, rng):
    """Evening (18–23 h local) is congested; nights and mornings are fine."""
    if 18 <= local_hour <= 23:
        base = rng.gauss(150 if local_hour in (20, 21) else 175, 18)
    elif 1 <= local_hour <= 7:
        base = rng.gauss(238, 6)
    else:
        base = rng.gauss(215, 14)
    return max(40.0, min(base, 249.0))


def main():
    if len(sys.argv) != 2:
        sys.exit("Usage: make-sample-data.py DATA_DIR")
    out = Path(sys.argv[1])
    if out.exists() and any(out.iterdir()):
        sys.exit(f"{out} isn't empty: give a new folder (the manifests are read-only)")
    (out / "raw").mkdir(parents=True)
    (out / "manifest").mkdir()
    rng = random.Random(7)
    speed = open(out / "speed.csv", "w", newline="")
    conn = open(out / "connectivity.csv", "w", newline="")
    speed.write("# meerkat speed v1\r\n" + SPEED_COLUMNS + "\r\n")
    conn.write("# meerkat connectivity v1\r\n" + CONN_COLUMNS + "\r\n")
    previous = None
    for day in range(DAYS):
        start = FIRST_DAY + timedelta(days=day)
        raw = open(out / "raw" / f"{start:%Y-%m-%d}.jsonl", "w")
        t = start
        while t < start + timedelta(days=1):
            in_outage = OUTAGE[0] <= t < OUTAGE[0] + OUTAGE[1]
            in_pause = PAUSE[0] <= t < PAUSE[0] + PAUSE[1]
            if (t - FIRST_DAY) % SPEED_EVERY == timedelta(0):
                write_speed(speed, raw, t, in_outage, in_pause, rng)
            write_probes(conn, t, in_outage, rng)
            t += PROBE_EVERY
        raw.close()
        speed.flush()
        conn.flush()
        previous = write_manifest(out, start, previous)
    speed.close()
    conn.close()


def write_speed(speed, raw, t, in_outage, in_pause, rng):
    common = f"ookla,wired,eth0,{GATEWAY},1000,,,,,,"
    if in_pause:
        speed.write(f"{iso(t)},paused_official,,{common},,,,,,,,,,,\r\n")
        return
    if in_outage:
        error = "Cannot read from socket"
        speed.write(f"{iso(t)},error,{error},{common},,,,,,,,,,,\r\n")
        log(raw, t, "error", error, "")
        return
    local_hour = (t.hour + UTC_OFFSET) % 24
    down = download_for(local_hour, rng)
    up = max(30.5, min(rng.gauss(46.5, 1.8), 49.9))
    ping = rng.gauss(12 if down > 200 else 19, 1.5)
    jitter = abs(rng.gauss(1.2, 0.5))
    result = f"https://www.speedtest.net/result/c/00000000-0000-4000-8000-{int(t.timestamp()):012d}"
    speed.write(f"{iso(t)},ok,,{common},{down:.2f},{up:.2f},{ping:.1f},{jitter:.1f},0.0,"
                f"12345,Example Server,Example City,Example ISP,203.0.113.7,{result}\r\n")
    output = json.dumps({
        "type": "result", "timestamp": iso(t),
        "ping": {"jitter": round(jitter, 3), "latency": round(ping, 3)},
        "download": {"bandwidth": int(down * 125000)}, "upload": {"bandwidth": int(up * 125000)},
        "packetLoss": 0, "isp": "Example ISP",
        "interface": {"name": "eth0", "isVpn": False, "externalIp": "203.0.113.7"},
        "server": {"id": 12345, "name": "Example Server", "location": "Example City"},
        "result": {"url": result}})
    log(raw, t, "ok", "", output)


def log(raw, t, status, error, output):
    raw.write(json.dumps({"time": iso(t), "backend": "ookla", "status": status, "error": error,
                          "output": output}) + "\n")


def write_probes(conn, t, in_outage, rng):
    common = f"wired,eth0,{GATEWAY}"
    gw = abs(rng.gauss(0.6, 0.1))
    conn.write(f"{iso(t)},ping,{GATEWAY},ok,,{common},3,3,0.0,{gw:.3f},{gw + .1:.3f},{gw + .3:.3f},0.080,\r\n")
    for host in HOSTS:
        if in_outage:
            conn.write(f"{iso(t)},ping,{host},ok,,{common},3,0,100.0,,,,,\r\n")
        else:
            r = abs(rng.gauss(11.5, 1.2))
            conn.write(f"{iso(t)},ping,{host},ok,,{common},3,3,0.0,{r - .8:.3f},{r:.3f},{r + 1.4:.3f},0.400,\r\n")
    if in_outage:
        conn.write(f"{iso(t)},dns,example.com,timeout,DNS lookup timed out,{common},,,,,,,,5000\r\n")
    else:
        conn.write(f"{iso(t)},dns,example.com,ok,,{common},,,,,,,,{abs(rng.gauss(14, 3)):.0f}\r\n")


def write_manifest(out, day, previous):
    """The day's seal, written just after midnight as meerkat would."""
    name = f"{day:%Y-%m-%d}"
    paths = [f"raw/{(day - timedelta(days=1)):%Y-%m-%d}.jsonl", f"raw/{name}.jsonl",
             "speed.csv", "connectivity.csv"]
    if previous:
        paths.append(previous)
    lines = ["# meerkat manifest v1", f"# day {name}",
             f"# created {iso(day + timedelta(days=1, seconds=5))}"]
    for p in paths:
        f = out / p
        if f.exists():
            data = f.read_bytes()
            lines.append(f"{hashlib.sha256(data).hexdigest()}  {len(data)}  {p}")
    path = f"manifest/{name}.sha256"
    (out / path).write_text("\n".join(lines) + "\n")
    (out / path).chmod(0o444)
    return path


if __name__ == "__main__":
    main()
