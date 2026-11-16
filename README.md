# meerkat

[![CI](https://github.com/carloslinares-eu/meerkat/actions/workflows/ci.yml/badge.svg)](https://github.com/carloslinares-eu/meerkat/actions/workflows/ci.yml)

**Keeps watch over your internet connection and records evidence when it's
slower than your contract says.**

meerkat runs on a Linux computer connected to your router. It runs a speed test
on a schedule (every 5 minutes by default) and checks connectivity every 30 s.
It records every result with its context: time, interface, test server, and
whether the measurement ran over a cable or Wi-Fi. `meerkat report` then
compares the data with the speeds in your contract and produces a printable
report you can send to your provider.

It's built with German providers in mind. The report checks the
Bundesnetzagentur rules (maximum, normally available and minimum speeds) and
works next to the official *Breitbandmessung* app, not instead of it.

> **Status: early development.** Recording works (`meerkat run`, `once`,
> `check`, `pause`), and so do `report`, `verify`, `export` and `advise-bbm`. The plan is in [PLAN.md](PLAN.md)
> and the task list in [DEVPLAN.md](DEVPLAN.md). How it started:
> *Origin* below.

## Build

Needs CMake ≥ 3.25, Ninja, a C++20 compiler (GCC 12+), `pkg-config` and
libsystemd and zlib (`sudo apt install pkg-config libsystemd-dev zlib1g-dev`). The other dependencies
(CLI11, nlohmann/json, toml++, doctest) are downloaded at configure time.

```sh
cmake --preset release        # or: debug, asan
cmake --build --preset release
ctest --preset release
./build/release/meerkat --version
```

## Use

```sh
meerkat check   # config, tools, data directory, connection, sleep lock
meerkat once    # measure once and print the result
meerkat run     # record until Ctrl+C / SIGTERM
meerkat pause --for 2h   # no speed tests for 2 hours (official measurement)
meerkat resume           # end the pause early
meerkat report --from 2026-03-01 --to 2026-03-14 -o report.html
```

`meerkat run` writes `speed.csv`, `connectivity.csv`, the raw speed-test
output (`raw/`) and a daily SHA-256 manifest (`manifest/`) to the data
directory (default `~/.local/share/meerkat`). It keeps the computer from
suspending while it runs, warns when the connection is Wi-Fi, and logs
outages as they start and end. Only one `meerkat run` records into a data
directory at a time: it locks the directory, and a second one stops at once.
On file systems that can't lock a directory (NFS, for example) it only warns,
so make sure yourself that just one is running. Settings: see
[`config.example.toml`](config.example.toml).

While the Breitbandmessung app (Festnetz-Check) runs, `meerkat run` pauses
its speed tests by itself (rows with status `paused_official`) and probes the
connection less often, so it doesn't lower the official result; a speed test
already running when the app starts is stopped. `meerkat once` doesn't start
its speed test either, or stops it, the same way. If meerkat can't see the app
(it runs as another user, or on another computer on the same line), pause by
hand with `meerkat pause --for <duration>`.

`meerkat report` writes the evidence report: one HTML file with no scripts
and no external files, which prints to PDF from the browser. It has the
verdict per direction under the three BNetzA rules, the summary, charts
(speed over time with the contract lines, by hour of day, outages), the
day-by-day and three-day-window tables, the outage list, tests over Wi-Fi or
a VPN (listed apart, never compared with the contract), the measurement setup
and the manifest hashes. `--from`/`--to` are local days (default: from the
first test to today); the contract comes from `[contract]` in the config or
from `--contract-down`/`--contract-up`. It only reads the data directory, so
it can run while `meerkat run` records.

`meerkat advise-bbm` helps with the official campaign. It uses the last
four weeks of cable tests (`--from`/`--to` to change that) and ranks the
local hours and weekdays by how often the line was below the normally
available and the minimum speed. It then suggests:
- three days from tomorrow (`--start` to change that), with a free day
  between them, within 14 days;
- two hours on each of those days, at least 4 hours apart, for the two
  blocks of five measurements.

It prints this together with the campaign's rules, and it needs the
contract figures. `meerkat report --advice` adds the same advice to the
report as a section. It is left out by default, and `export` never
includes it, because the report may go to the provider and the plan
shouldn't.

`meerkat verify` checks the evidence trail before you hand it over. It reads
every daily manifest, hashes each file it sealed again and lists it as ok,
changed, grew, shorter or missing. It also checks the chain (each manifest
includes the one before it) and reports days with data but no manifest, and
raw logs created after their day was sealed. Data from today and yesterday
that isn't sealed yet is only a note. The exit code is 1 if anything is
wrong. Like `report`, it only reads the data directory.

`meerkat export [--from --to] [-o FILE]` writes one ZIP file for the
provider. It contains:
- the report for those days;
- the `verify` result, checked just before;
- the data directory byte for byte (speed.csv, connectivity.csv, the raw
  logs and the manifests);
- a README that explains how to check it;
- `SHA256SUMS` for `sha256sum -c`.

The data is always the whole recording, because the manifests seal the
files as a whole. A file that is still being written ends at its last
complete line. If `verify` finds problems, the export still goes ahead and
the problems are listed in the bundle. Like the report, the file is
readable only by you.

## Run as a service

To record around the clock, even with nobody logged in, install meerkat as
a systemd service. Build first, then install as root:

```sh
cmake --preset release && cmake --build --preset release
sudo packaging/install.sh --config ~/.config/meerkat/config.toml
```

The service:
- runs as its own user `meerkat`, with its data in `/var/lib/meerkat` and
  its config in `/etc/meerkat/config.toml`;
- is sandboxed: read-only system, no access to home directories, no
  capabilities, and only the network, netlink and the system bus.
  `systemd-analyze security meerkat` rates it about 1.5 ("OK");
- comes with a polkit rule (`/etc/polkit-1/rules.d/50-meerkat.rules`) that
  lets the user `meerkat` take logind's sleep, idle and lid locks.
  logind's own policy gives them only to someone logged in at the computer,
  so without the rule the computer could suspend.

`install.sh` options:
- `--config FILE` copies your config, with the contract figures, to
  `/etc/meerkat`. Without it, the example config is installed if there is
  none yet.
- `--import-data ~/.local/share/meerkat` carries an existing evidence trail
  over byte for byte, only into an empty `/var/lib/meerkat`. Stop
  `meerkat run` first.
- `--speedtest FILE` copies the Ookla CLI to `/usr/local/bin` if it is only
  in your home directory, where the service can't reach it.
- `--uninstall` removes the service but keeps the data and the config.

The user who ran `sudo` is added to the group `meerkat`. After logging in
again, `report`, `verify` and `export` work without sudo: pass
`--data-dir /var/lib/meerkat --config /etc/meerkat/config.toml`, or put
`data_dir = "/var/lib/meerkat"` in your own config. Logs:
`journalctl -u meerkat`.

## Licence compliance

meerkat is MIT-licensed ([LICENSE](LICENSE)). It uses the third-party software
below. Only the three libraries compiled into the binary need a notice to go
with it, and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) is that
notice. The rest is linked dynamically from the system, or is a separate
program that meerkat runs and doesn't ship.

**Compiled into meerkat** (header-only, fetched at configure time, see
[`cmake/Deps.cmake`](cmake/Deps.cmake)). Their copyright notice and licence
text must go with every copy, source or binary: ship
`THIRD_PARTY_LICENSES.md` with every binary.

| Library | Version | Licence | Used for |
|---|---|---|---|
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.5.0 | BSD-3-Clause | command line |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | Ookla JSON output |
| [toml++](https://github.com/marzer/tomlplusplus) | 3.4.0 | MIT | config file |

**Build and tests only** (not part of meerkat, no obligations for it):
[doctest](https://github.com/doctest/doctest) 2.5.3 (MIT); CMake, Ninja, GCC,
`pkg-config`, Doxygen; in CI the GitHub Actions `actions/checkout` and
`actions/upload-artifact` (MIT).

**Linked dynamically from the system** (Debian packages, not shipped):

| Library | Licence | Notes |
|---|---|---|
| libsystemd (sd-bus, for the logind sleep lock) | LGPL-2.1-or-later | Dynamic linking places no conditions on meerkat. A fully static build would have to let users relink it with a changed libsystemd, so the release binary (DEVPLAN 4.3) must keep libsystemd dynamic. |
| glibc | LGPL-2.1-or-later | Same as libsystemd. |
| zlib (deflate, for the `meerkat export` ZIP) | Zlib | Permissive; no notice has to go with a binary, linked dynamically or statically. |
| libstdc++, libgcc | GPL-3.0-or-later with the GCC Runtime Library Exception | The exception allows any licence for programs compiled with GCC. |

**Programs meerkat runs** (separate processes, no linking; installed by the
user, never shipped with meerkat):

| Program | Licence | Notes |
|---|---|---|
| Ookla Speedtest CLI (`speedtest`) | Proprietary, free download | Its [licence terms](https://www.speedtest.net/about/eula) and [privacy policy](https://www.speedtest.net/about/privacy) apply. meerkat runs it with `--accept-license --accept-gdpr`, so **by running meerkat you accept them**; read them first. meerkat doesn't redistribute the CLI or its output beyond your own data directory. |
| `ping` (iputils) | BSD-3-Clause and GPL-2.0-or-later | Run as a program; no conditions on meerkat. |
| `iw` | ISC | Same. |
| Breitbandmessung app (Festnetz-Check, BNetzA) | Proprietary | Not run or shipped by meerkat; meerkat only checks whether it is running. Its exported protocol is your own measurement data. |

**meerkat's own code**: SHA-256 (`src/store/sha256.*`) is written from
FIPS 180-4, not copied, and is MIT like the rest. No code from the original
prototype (see *Origin* below) is left in it.

When a dependency is added or updated: update this section and
`THIRD_PARTY_LICENSES.md` (copy the licence text unchanged from the release).
A test fails if the versions in `cmake/Deps.cmake` and the notice file
disagree.

## Origin

meerkat started as a single-file prototype, generated with Google Gemini from
this prompt:

> I want a c++ script that runs in Debian CLI that does the following:
>
> - prevents the computer from going to sleep
> - checks regularly the internet download/upload speed as well as the ping
> - it also checks the wlan speed and ping
> - saves the result in csv formatted data
> - the frequency by default is 5 minutes, but you can modify when launching
>   the command via an option

The prototype (267 lines) didn't build as it stood, and as evidence it had
gaps a provider could use to dismiss the data: failures written as `0.00`,
measuring only over Wi-Fi, local time without a time zone, no context and no
outage detection. [PLAN.md](PLAN.md) (section 1) lists
them. meerkat is a rewrite from scratch built around those gaps; it keeps the
prototype's goals, not its code.

The prototype is no longer in the tree. It stays in the git history:

```sh
git show 755e300:legacy/meerkat.cpp
```

## License

[MIT](LICENSE)
