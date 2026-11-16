#!/bin/sh
# Packs the release archive: the binary with LICENSE and THIRD_PARTY_LICENSES.md
# (the notices that must go with every copy), README.md, config.example.toml
# and the service files. Refuses a binary that needs a shared library other
# than glibc and libsystemd: those two stay dynamic (LGPL), the rest is
# compiled in.
#
# Usage: packaging/release-archive.sh BINARY VERSION OUTDIR
# Writes OUTDIR/meerkat-VERSION-linux-x86_64.tar.gz and prints its path.
set -eu
umask 022

[ $# -eq 3 ] || { echo "usage: $0 BINARY VERSION OUTDIR" >&2; exit 2; }
binary=$1
case $binary in */*) ;; *) binary=./$binary ;; esac  # never a program on PATH
version=$2
outdir=$3
repo=$(cd "$(dirname "$0")/.." && pwd)

# Direct dependencies only (ldd would also list what libsystemd pulls in).
dynamic=$(readelf -d "$binary")  # set -e: stops here if readelf fails
needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
case " $(echo $needed) " in
    *" libc.so.6 "*) ;;
    *) echo "$0: no libc.so.6 among the libraries $binary needs: not a meerkat binary?" >&2; exit 1 ;;
esac
for lib in $needed; do
    case $lib in
        libc.so.6 | libm.so.6 | libsystemd.so.0 | ld-linux-x86-64.so.2) ;;
        *) echo "$0: $binary needs $lib; only glibc and libsystemd may stay dynamic" >&2; exit 1 ;;
    esac
done
"$binary" --version | grep -qxF "meerkat $version" || {
    echo "$0: $binary --version isn't 'meerkat $version'" >&2; exit 1; }

name=meerkat-$version-linux-x86_64
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$name/packaging"
install -m 0755 "$binary" "$stage/$name/meerkat"
install -m 0644 "$repo/LICENSE" "$repo/THIRD_PARTY_LICENSES.md" "$repo/README.md" \
    "$repo/config.example.toml" "$stage/$name/"
install -m 0644 "$repo/packaging/meerkat.service" "$repo/packaging/meerkat.sysusers" \
    "$repo/packaging/50-meerkat.rules" "$stage/$name/packaging/"
install -m 0755 "$repo/packaging/install.sh" "$stage/$name/packaging/"

mkdir -p "$outdir"
out=$(cd "$outdir" && pwd)/$name.tar.gz
# Same bytes for the same inputs: fixed owner, order and time.
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
    -C "$stage" -cf - "$name" | gzip -n -9 > "$out"
echo "$out"
