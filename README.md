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

Needs CMake ≥ 3.25, Ninja and a C++20 compiler (GCC 12+). The dependencies
(CLI11, nlohmann/json, toml++, doctest) are downloaded at configure time.

```sh
cmake --preset release        # or: debug, asan
cmake --build --preset release
ctest --preset release
./build/release/meerkat --version
```

## License

[MIT](LICENSE)
