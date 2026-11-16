#!/bin/sh
# Checks `sudo meerkat import-bbm` on a data directory another user owns
# (#133): it must not follow a symlinked official/, and the files it
# creates belong to the data directory's owner. Needs passwordless sudo
# and a non-root caller (as on the CI runners).
# Usage: import-as-root.sh MEERKAT FIXTURES_DIR
set -eu
meerkat=$(realpath "$1")
fixtures=$(realpath "$2")
me=$(id -un)
[ "$(id -u)" -ne 0 ] || { echo "run as a normal user with sudo" >&2; exit 2; }

work=$(mktemp -d)
trap 'sudo rm -rf "$work"' EXIT
mkdir "$work/data"
sudo mkdir -m 0755 "$work/rootdir"            # stands for a directory like /etc/bash_completion.d
sudo install -m 0600 "$fixtures/bbm/single.csv" "$work/export.csv"   # readable by root only

# 1. A symlinked official/ is refused, and the target is untouched.
ln -s "$work/rootdir" "$work/data/official"
if sudo "$meerkat" import-bbm --data-dir "$work/data" "$work/export.csv"; then
    echo "FAIL: imported through a symlinked official/" >&2; exit 1
fi
[ -z "$(sudo ls -A "$work/rootdir")" ] || { echo "FAIL: a file appeared in the symlink's target" >&2; exit 1; }
[ "$(stat -c %a "$work/rootdir")" = 755 ] || { echo "FAIL: the target's mode changed" >&2; exit 1; }
[ "$(stat -c %U "$work/rootdir")" = root ] || { echo "FAIL: the target's owner changed" >&2; exit 1; }
rm "$work/data/official"

# 2. A normal import as root: the file is read as root, everything new belongs to the owner.
sudo "$meerkat" import-bbm --data-dir "$work/data" "$work/export.csv" "$fixtures/bbm/protocol.pdf"
for f in official official/export.csv official/protocol.pdf official.csv; do
    owner=$(stat -c %U "$work/data/$f")
    [ "$owner" = "$me" ] || { echo "FAIL: $f belongs to $owner, not $me" >&2; exit 1; }
done
echo "import-bbm as root: OK"
