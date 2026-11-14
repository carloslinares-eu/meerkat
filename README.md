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

> **Status: early development.** Nothing works yet. The plan is in
> [PLAN.md](PLAN.md) and the task list in [DEVPLAN.md](DEVPLAN.md). The
> original prototype is in [`legacy/`](legacy/).

## Build

Needs CMake ≥ 3.25, Ninja, a C++20 compiler (GCC 12+), `pkg-config` and
libsystemd (`sudo apt install pkg-config libsystemd-dev`). The other dependencies
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
FIPS 180-4, not copied, and is MIT like the rest. [`legacy/meerkat.cpp`](legacy/meerkat.cpp) is the original
prototype, generated with Google Gemini for this project; it isn't built.

When a dependency is added or updated: update this section and
`THIRD_PARTY_LICENSES.md` (copy the licence text unchanged from the release).
A test fails if the versions in `cmake/Deps.cmake` and the notice file
disagree.

## License

[MIT](LICENSE)
