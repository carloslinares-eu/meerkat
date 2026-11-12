# meerkat: development plan

Small tasks, grouped by milestone. Each task fits in one Claude Code session or
less. The design and its reasons are in `PLAN.md`.

**Columns**
- **Effort**: the recommended Claude reasoning effort (`/effort`):
  - `low`: mechanical work.
  - `medium`: normal coding.
  - `high`: tricky logic, processes and signals, or evidence correctness.
- **Agents**: how many Claude agents run at the same time. **1** means the main
  session only. A number above 1 means independent subtasks that can run in
  parallel git worktrees.
- **Skills**: the domain knowledge needed, plus the Claude Code skills to run
  afterwards.
- **HW**: what the task needs physically (✋ = the user does physical steps).

General rules:
- C++20 with Doxygen on everything, the same conventions as duckling (they get
  copied into meerkat's `CLAUDE.md` in 0.1).
- One branch and one PR per task (`task-1.2-process`). CI must pass. Every
  task that writes code ends with `/code-review medium`.
- Faults that come up during a task become GitHub issues and get closed with
  `Fixes #N`.

---

## M0: Project scaffolding
| # | Task | Effort | Agents | Skills | HW | Depends |
|---|---|---|---|---|---|---|
| 0.1 ✅ | `git init`. Move the draft to `legacy/` for reference. Add `CLAUDE.md` (conventions), `.gitignore` (excludes `*.pdf`, `config.toml` and data, so the contract and the provider name stay private), `LICENSE` (MIT), and a stub `README.md`. Create the GitHub repo and push | low | 1 | git, `gh` CLI | – | – |
| 0.2 ✅ | CMake skeleton: `src/main.cpp` (prints the version), `src/` library target, FetchContent (nlohmann/json, CLI11, toml++, doctest), `tests/` target, presets (debug, release, asan) | medium | 1 | CMake, FetchContent | – | 0.1 |
| 0.3 ✅ | GitHub Actions: build and test on ubuntu-latest and a Debian container, ASan/UBSan job, Doxygen warnings report | medium | 1 | GitHub Actions | – | 0.2 |
| 0.4 ✅ | Install the runtime tools on carbon (Ookla `speedtest` 1.2.0 from the official tarball into `~/.local/bin`, `iw`, `libsystemd-dev`). Save anonymised samples of `iw`, `ping` and `speedtest` output to `tests/fixtures/` | low | 1 | Debian packaging | carbon | – |

## M1: Measurement core (replaces the draft's functions)
| # | Task | Effort | Agents | Skills | HW | Depends |
|---|---|---|---|---|---|---|
| 1.1 ✅ | `core::Measurement` model: `Status` enum, optional values (never 0 for missing), UTC timestamp with ISO-8601 formatting. Unit tests | medium | 1 | std::chrono, std::optional | – | 0.2 |
| 1.2 ✅ | `sys::Process`: fork/exec with an argv array (no shell), timeout with SIGTERM then SIGKILL, captures stdout/stderr and the exit code, `PR_SET_PDEATHSIG`. Tests with `sleep`/`echo`/`false` | high | 1 | POSIX processes, signals | – | 0.2 |
| 1.3 ✅ | `net::Route`: default-route interface and gateway from `/proc/net/route` (IPv6 fallback). `net::Link`: type (wired/Wi-Fi), carrier, Ethernet speed from sysfs. Fixture-based tests | medium | 1 | Linux sysfs/procfs | – | 1.1 |
| 1.4 ✅ | `net::Wifi`: parse `iw dev <if> link` (SSID, BSSID, frequency, signal dBm, tx/rx bitrate). Tests from 0.4 fixtures | low | 1 | iw output | – | 1.2, 0.4 |
| 1.5 ✅ | `probe::Ping`: run `ping -c N -i 0.2 -W 1`, parse sent/received/loss and min/avg/max/mdev. Handle total loss and an unreachable network. Tests on fixtures (German and C locale, so run with `LC_ALL=C`) | medium | 1 | ping output | – | 1.2, 1.1 |
| 1.6 ✅ | `probe::Dns`: time a `getaddrinfo` lookup with a timeout | low | 1 | resolver | – | 1.1 |
| 1.7 ✅ | `probe::SpeedBackend` interface plus `OoklaBackend`: run `speedtest --format=json`, parse down/up (bytes/s → Mbit/s), latency, jitter, loss, server id/name, ISP, external IP, result URL. Keep the raw JSON. Map errors and timeouts to a `Status`. Optional `--server-id` | high | 1 | Ookla CLI JSON | – | 1.2, 1.1, 0.4 |
| 1.8 ✅ | `meerkat once`: one connectivity and speed measurement, printed in a human-readable form. First end-to-end check | medium | 1 | `/run` | carbon | 1.3–1.7 |

## M2: Long-running recorder
| # | Task | Effort | Agents | Skills | HW | Depends |
|---|---|---|---|---|---|---|
| 2.1 ✅ | `store::CsvWriter`: `speed.csv` and `connectivity.csv`, schema version in the header, correct escaping, fsync after each row, refuses to append to a file with a different schema | medium | 1 | CSV (RFC 4180) | – | 1.1 |
| 2.2 ✅ | `store::RawLog` (JSONL of the backend output) and a daily SHA-256 manifest (vendored SHA-256 or OpenSSL) | medium | 1 | hashing | – | 2.1 |
| 2.3 ✅ | `core::Config`: TOML file with CLI11 flags on top (intervals, output dir, backend, server id, ping targets, contract figures). Commit only a generic `config.example.toml`. Validation and `meerkat check` (dependencies, permissions, write access, route) | medium | 1 | toml++, CLI11 | – | 0.2 |
| 2.4 ✅ | `core::Scheduler`: two fixed-rate jobs lined up with the wall clock, overrun → skip and log, wakes on shutdown right away. Tests with a fake clock | high | 1 | std::chrono, condition variables | – | 1.1 |
| 2.5 ✅ | `sys::SleepInhibitor`: logind `Inhibit("sleep:idle", …, "block")` through sd-bus, the lock is a held fd. Clear error and a fallback hint when polkit refuses | medium | 1 | sd-bus, logind | carbon | 0.4 |
| 2.6 ✅ | `core::OutageTracker`: turns connectivity samples into outage start/end events (configurable: K consecutive failures). Tests | medium | 1 | state machines | – | 1.5 |
| 2.7 | `core::LinkGuard` (WLAN warning): checks the default-route link type before every speed test. Warns when `run`/`once` starts and in `meerkat check` if it's Wi-Fi, and warns again on each change from cable to Wi-Fi. Every row gets a `link_type` column. `require_wired = true` → status `skipped_wlan` instead of a test. Tests with a fake `net::Link` | medium | 1 | – | – | 1.3, 2.3 |
| 2.8 | `meerkat run`: connects everything (including the LinkGuard), handles SIGINT/SIGTERM with `sigaction`, logs to stderr/journald, shows a status line in the foreground | medium | 1 | – | – | 2.1–2.7 |
| 2.9 | M2 acceptance: run 24 h on carbon. Pull the cable or turn off the router once. Check that the outage is recorded, that there are no 0-for-missing values, that the machine didn't suspend, and that switching to WLAN triggers the warning | medium | 1 | `/run` | ✋ carbon | 2.8 |
| 2.10 | `core::OfficialGuard`: keeps meerkat from spoiling official measurements. While the Breitbandmessung app is running (process match, names set in the config), no speed tests run and `paused_official` rows are written. The connectivity probe drops to a lower rate. Adds `meerkat pause --for <duration>` / `meerkat resume` as a manual fallback (through a state file the running instance watches). Tests with a fake process list; manual check with the real app | medium | 1 | procfs, IPC via a state file | ✋ carbon + app | 2.8 |

## M3: Evidence report
| # | Task | Effort | Agents | Skills | HW | Depends |
|---|---|---|---|---|---|---|
| 3.1 | `report::Loader` + `report::Stats`: read the CSVs for a date range. Per-test thresholds against the contract (maximum, normally available and minimum from the local config or flags, never built in; see PLAN.md §4), with the three BNetzA rules checked for download and upload, both per calendar day and over sliding 3-day windows. Percentiles, hour-of-day buckets, outage totals. Only wired rows are compared with the contract; WLAN rows get a separate section with a warning banner. Tests | high | 1 | statistics, BNetzA Allgemeinverfügung | – | 2.1, 2.6 |
| 3.2 | `report::Svg`: small inline-SVG chart writer (time series with contract lines, hour-of-day bar chart, outage timeline). Golden-file tests | medium | 1 | SVG, `dataviz` skill | – | 0.2 |
| 3.3 | `meerkat report --from --to -o report.html`: one self-contained page that prints well to PDF. Contains the summary table, charts, outage list, measurement setup (interface, link speed, server), and the manifest hashes | medium | 1 | HTML/CSS print layout | – | 3.1, 3.2 |
| 3.4 | `meerkat export`: CSV/ZIP bundle for the provider (data + raw log + manifest + report + `meerkat verify` result, see 3.8) | low | 1 | – | – | 3.3, 3.8 |
| 3.5 | `meerkat advise-bbm`: picks the best hours and days for the official campaign from meerkat's data (the slots where download or upload is most often below the normally available and the minimum speed). Lists them with the campaign timing rules, which the app enforces anyway. Also a section in the report | medium | 1 | statistics | – | 3.1 |
| 3.6 | `meerkat import-bbm <file>`: install the Breitbandmessung app on carbon and run **one** test so there's a real export. Find out what it exports (PDF protocol, CSV?) and save it as a fixture. Then write the parser → `official.csv` (`source=breitbandmessung`, timestamp, down, up, ping, measurement number). Keep the original file unchanged under `official/`, with its SHA-256 in the manifest. Don't guess the format | medium | 1 | PDF/CSV parsing | ✋ carbon + app + cable | 2.2 |
| 3.7 | Report "official measurements" section: official results drawn on the same charts as meerkat's. Each official measurement compared with the meerkat tests ±30 min around it (difference and agreement). The three BNetzA rules run on the official data and shown as **the legal finding**; meerkat's verdict is labelled as supporting long-term evidence. `meerkat export` adds the protocol PDF unchanged | high | 1 | statistics, BNetzA Allgemeinverfügung | – | 3.3, 3.6 |
| 3.8 | `meerkat verify [--data DIR]`: checks the evidence trail before it's handed over. Reads every manifest in date order, runs `verify_manifest` on each and lists every file as ok / changed / shorter / missing. Checks the chain (each manifest's previous-manifest entry, gaps between days), and flags days with data but no manifest and raw-log lines added after the next day's manifest sealed them. Exit code ≠ 0 on any problem. `meerkat export` (3.4) runs it first and puts the result in the bundle. Tests on a fixture data dir with planted edits | medium | 1 | – | – | 2.2, 2.3 |

## M4: Packaging and release
| # | Task | Effort | Agents | Skills | HW | Depends |
|---|---|---|---|---|---|---|
| 4.1 | systemd unit (`meerkat.service`, dedicated user, hardening options, `StateDirectory=meerkat`), `install.sh`. Test on carbon | medium | 1 | systemd | carbon | 2.8 |
| 4.2 | CPack `.deb` (binary, unit, default config, `Recommends: iw`). Document how to install the Ookla CLI | low | 1 | CPack | – | 4.1 |
| 4.3 | Release workflow: tag → GitHub Release with the `.deb` and a static-ish binary | low | 1 | GitHub Actions | – | 4.2, 0.3 |
| 4.4 | README: purpose, quick start, config reference, data format, the "how to collect evidence" guide (cable, test server, data volume, Breitbandmessung), sample report screenshot. Doxygen with no warnings | low | 1 | technical writing | – | 3.3, 4.1 |
| 4.5 | Final review pass: `/security-review` (process spawning, file permissions, service hardening) and `/simplify` | high | 1 | `/security-review`, `/simplify` | – | 4.1 |

## M5: Collecting the evidence (the actual goal)
| # | Task | Effort | Agents | Skills | HW | Depends |
|---|---|---|---|---|---|---|
| 5.1 | Measurement setup: connect carbon to the router by cable, put the contract figures in the config, choose and pin a speed-test server, start the service | low | 1 | – | ✋ cable to router | 4.1 |
| 5.2 | Campaign: 2–4 weeks of continuous recording. Check `meerkat report` weekly | low | 1 | – | ✋ carbon left on | 5.1 |
| 5.3 | Official campaign: after about 1–2 weeks of data, run `meerkat advise-bbm`. Run the 30 Breitbandmessung measurements (3 days) in the hours it suggests, while meerkat pauses itself (2.10). Then `meerkat import-bbm` the protocol | low | 1 | – | ✋ carbon + app + cable | 5.2 (≥ 1 week in), 2.10, 3.5, 3.6 |
| 5.4 | Final report + export bundle (meerkat data + official protocol). Help write the complaint letter: the official finding first, meerkat's long-term data as support | medium | 1 | – | – | 5.2, 5.3, 3.7 |
