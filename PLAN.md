# meerkat: assessment and design

meerkat measures your internet connection on a schedule and keeps a record that
can back a complaint to the provider. This file covers the state of the first
draft, the decisions for the full tool, and the open questions. The task list is
in `DEVPLAN.md`.

## 1. State of the draft (`legacy/meerkat.cpp`, 267 lines, from Gemini)

**What it does:** starts `systemd-inhibit … sleep infinity` to block suspend.
Every N minutes it reads WLAN bitrates with `iw`, pings the gateway 3 times,
runs `speedtest-cli --json`, and appends one CSV row. Options: `-i <minutes>`,
`-o <file>`.

**Does it build or run on carbon today?** No.
- `nlohmann/json.hpp` isn't installed, so it doesn't compile. There's no build
  system and no git repo.
- `speedtest-cli` and `iw` aren't installed either. They'd fail without any
  message, and every row would be zeros.

**Problems for use as evidence** (the ones that matter most come first):

| # | Problem | Why it matters |
|---|---|---|
| E1 | Failures are written as `0.00`. | You can't tell "the test crashed" from "the line was down" from "0 Mbps". A provider will dismiss the whole file over this. |
| E2 | It only measures over WLAN. | "It's your Wi-Fi" is the provider's first answer. Evidence has to come from a cable to the router. carbon has an Ethernet port (`enp0s25`), but it isn't connected right now. |
| E3 | It uses `speedtest-cli` (the old Python sivel tool). | It's unmaintained and reads low on fast lines. Providers trust the official Ookla `speedtest` CLI more, and that CLI also gives a result ID/URL that can be checked, the server, the ISP, the external IP, jitter and packet loss. |
| E4 | Timestamps are local time with no time zone. | Daylight-saving changes give duplicate or missing hours. Use ISO 8601 with an offset. |
| E5 | No context is recorded: no server, ISP, external IP, interface type or link speed. | Without context, a single slow result can't be pinned on the provider. |
| E6 | There's no outage detection. A speed test runs every 5 minutes, and nothing runs between tests. | Short outages and packet loss are often the strongest complaint. Detecting them needs a cheap probe that runs often (every 30–60 s). |
| E7 | There's no report. | Someone has to turn the CSV into "X % of tests were below the contracted minimum". |

**Code problems:**
- It runs commands through the shell with `popen` and builds them from parsed
  output (the interface name and the gateway IP). There's no timeout, so a
  hung `speedtest` stops the monitor for good.
- The schedule drifts. It sleeps the interval *after* each test, so the real
  period is the interval plus the test time.
- If meerkat gets SIGKILL or crashes, the `systemd-inhibit` child keeps
  running forever (there's no `PR_SET_PDEATHSIG`). It never checks whether the
  inhibitor started. Blocking sleep can need polkit permission over SSH.
- The gateway ping uses 3 packets and keeps only the average. It doesn't record
  loss, min, max or jitter.
- It takes the first `iw` interface even when that interface isn't the default
  route. It reads nothing for Ethernet.
- Everything is in one file with globals. There are no tests, docs, license,
  README or CI.
- Data volume: on a fast line, a speed test every 5 minutes downloads tens to
  hundreds of GB a day. That's fine on an unlimited plan, but the README should
  say so.

**What to keep:** the overall idea (keep the machine awake, run tests on a
schedule, append to a CSV), the getopt-style options, and the clean shutdown
on SIGINT/SIGTERM.

## 2. Decisions for the full tool

1. **C++20 and CMake.** Same conventions as duckling: Doxygen on everything,
   namespaces match folders, doctest, GitHub Actions. Dependencies come through
   FetchContent: nlohmann/json, CLI11, toml++ and doctest. Linux only (Debian
   first), because the tool depends on systemd, sysfs and `iw`.
2. **Two probes with different schedules.**
   - The *speed probe* runs a full speed test. Default every 5 minutes (as the
     draft does), and it can be changed.
   - The *connectivity probe* is cheap and runs every 30 s. It pings the
     gateway and 2 public hosts (loss, RTT min/avg/max, jitter), times a DNS
     lookup, and records outages as start and end events.
3. **Speed-test backend behind an interface.** The default is the Ookla
   `speedtest` CLI (`--format=json --accept-license --accept-gdpr`, with an
   optional pinned `--server-id`). Other backends (librespeed-cli, iperf3) can
   be added later. meerkat doesn't ship the Ookla binary. It only calls it.
4. **No shell.** External tools run through a single `Process` helper
   (fork/exec with an argv array, a timeout, and stdout/stderr captured). Data
   that the kernel already provides is read directly: the default route from
   `/proc/net/route`, the link speed from `/sys/class/net/*/speed`, and the
   interface type from sysfs. Only Wi-Fi details still come from `iw`.
5. **Every measurement has an explicit `status`** (`ok`, `timeout`, `error`,
   `no_network`, …) plus an error text. Values that weren't measured are left
   empty. They're never written as 0.
6. **Storage:**
   - `speed.csv` and `connectivity.csv`, with a versioned schema, UTC ISO-8601
     timestamps and a header.
   - `raw/*.jsonl` keeps the backend's untouched output as an audit trail.
   - A daily SHA-256 manifest shows that the files weren't edited afterwards.
7. **Fixed-rate scheduler** lined up with the wall clock (for example
   :00/:05/:10). If a test overruns, the next slot is skipped and the skip is
   logged.
8. **Sleep inhibition** uses logind's D-Bus `Inhibit()` call through sd-bus
   (libsystemd). The lock is a file descriptor, so it's released automatically
   when the process dies. If that fails, meerkat says so loudly.
9. **Runs as a systemd service** (`meerkat.service`, plus a matching
   `install.sh`/`.deb`). Running it in the foreground is still supported.
10. **`meerkat report`** reads the data plus your contract figures (maximum,
    normally available and minimum speeds) and writes one self-contained,
    printable HTML file with inline SVG charts and no JavaScript dependencies.
    It covers time series, hour-of-day patterns, the share of tests below each
    contract threshold, an outage list, and WLAN vs. wired results.
11. **CLI:** `meerkat run`, `meerkat once`, `meerkat report`, `meerkat check`
    (checks dependencies and permissions) and `meerkat --version`. Config is
    TOML at `~/.config/meerkat/config.toml`, or `/etc/meerkat/config.toml` for
    the service. Flags override the file.

12. **Warning on WLAN.** If the active connection (the default route) is
    Wi-Fi, meerkat warns when it starts (`run`, `once`), in `meerkat check`,
    and again whenever the route switches from cable to Wi-Fi while running.
    Every row records `link_type`. The report puts a banner on WLAN data and
    keeps it out of the contract comparison by default. `require_wired = true`
    in the config makes meerkat skip speed tests (status `skipped_wlan`) instead
    of only warning.

13. **Breitbandmessung app.** meerkat doesn't automate the official app; you
    run it yourself, so its results stay legally valid. meerkat works around it:
    - It **pauses** its own speed tests while the app runs (2.10), so they
      don't compete for bandwidth.
    - It **suggests** the worst hours for the campaign from its data (3.5).
    - It **imports** the app's export unchanged, with a checksum (3.6). The
      format gets checked against a real export first.
    - It **compares** each official result with meerkat's tests at the same
      time (3.7). The app's result is shown as the legal finding, and
      meerkat's data as long-term support.

## 3. Answers (2026-11-11)

- **Legal framework: Germany.** The measurement that counts in law (§ 57 TKG,
  reduction of payment) is the Bundesnetzagentur *Breitbandmessung
  Desktop-App*: 30 measurements on 3 different calendar days, with at least
  one day between measurements, over a cable. meerkat's long-term data supports
  that campaign and shows the pattern over time, but it doesn't replace it.
  Task 5.3 runs both. The report wording and thresholds follow the BNetzA
  rules (see 3.1).
- **Contract figures:** private; given at run time or in the local
  config file, never committed (see §4).
- **Repo:** `carloslinares-eu/meerkat`, MIT license.
- **Speed-test interval:** 5 minutes.

## 4. Contract figures and the BNetzA rules

The contract figures (maximum, normally available and minimum speed, for
download and upload) come from the provider's *Produktinformationsblatt*.
They are **private**: meerkat has no built-in values, and they are never
committed. They're given at run time (`--contract-down MAX/NORMAL/MIN`,
`--contract-up MAX/NORMAL/MIN`) or in the local config file
(`~/.config/meerkat/config.toml`, mode 0600), which `meerkat init` writes.
Without them, `meerkat report` doesn't compare against a contract. Docs and
tests use a made-up tariff (250/200/150 down, 50/40/30 up).

The BNetzA rules for a "significant, continuous or regularly recurring
deviation" (Allgemeinverfügung, Dec 2021), applied by task 3.1 and checked
separately for download and upload. A deviation exists if **any one** of these
is true:
1. 90 % of the maximum isn't reached at least once on each of 2 of the 3
   measurement days.
2. The normally available speed isn't reached in 90 % of all measurements.
3. The speed drops below the minimum at least once on each of 2 of the 3
   measurement days.

**Privacy:** the contract PDF, the real config and the measurement data stay
out of the public repo (`.gitignore`, plus a local pre-commit hook that
refuses commits containing private values). The repo ships a generic
`config.example.toml`.
