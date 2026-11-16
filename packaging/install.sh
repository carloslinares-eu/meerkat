#!/usr/bin/env bash
# Installs meerkat as a systemd service (task 4.1). Run as root, from the
# repository, after a release build:
#
#   cmake --preset release && cmake --build --preset release
#   sudo packaging/install.sh [options]
#
# Run it again to update: the running service is restarted.
#
# Options:
#   --binary FILE       meerkat binary (default: build/release/meerkat)
#   --prefix DIR        where it goes (default: /usr/local; the binary goes to DIR/bin)
#   --config FILE       copy FILE to /etc/meerkat/config.toml (e.g. your
#                       ~/.config/meerkat/config.toml with the contract);
#                       without it the example config is installed if none exists
#   --speedtest FILE    copy the Ookla CLI to /usr/local/bin/speedtest (if the
#                       service can't find one in /usr/local/bin or /usr/bin)
#   --import-data DIR   copy an existing data directory (e.g. ~/.local/share/meerkat)
#                       into /var/lib/meerkat, byte for byte, so the evidence
#                       trail goes on; only into an empty /var/lib/meerkat
#   --user NAME         add NAME to the group meerkat, to read the data
#                       (default: the user who ran sudo)
#   --no-start          install, but don't enable and start the service
#   --uninstall         stop and remove the service and what install.sh put in
#                       place (binary, speedtest, unit, polkit rule); keeps
#                       /var/lib/meerkat, /etc/meerkat and the user
#   -h, --help          this text
#
# What it installs:
#   DIR/bin/meerkat                              the program
#   /etc/systemd/system/meerkat.service          the unit
#   /usr/lib/sysusers.d/meerkat.conf             the user and group meerkat
#   /etc/polkit-1/rules.d/50-meerkat.rules       lets the service keep the computer awake
#   /etc/meerkat/config.toml                     root:meerkat 0640
#   /var/lib/meerkat                             the data (created by systemd)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(dirname "$here")"
args=("$@")

binary="$repo/build/release/meerkat"
prefix=/usr/local
config=""
speedtest=""
import_data=""
user="${SUDO_USER:-}"
start=1
uninstall=0

unit_dest=/etc/systemd/system/meerkat.service
sysusers_dest=/usr/lib/sysusers.d/meerkat.conf
polkit_dir=/etc/polkit-1/rules.d
polkit_dest="$polkit_dir/50-meerkat.rules"
etc_dir=/etc/meerkat
config_dest="$etc_dir/config.toml"
data_dir=/var/lib/meerkat
# What install.sh installed, for --uninstall.
installed_list="$etc_dir/installed-files"
# --speedtest always goes here: on the service's PATH whatever --prefix says.
speedtest_dest=/usr/local/bin/speedtest
# systemd's default PATH for system services.
service_path=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin

die() { echo "install.sh: $*" >&2; exit 1; }
say() { echo "==> $*"; }
usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --binary) binary="${2:?--binary needs a file}"; shift 2 ;;
        --prefix) prefix="${2:?--prefix needs a directory}"; shift 2 ;;
        --config) config="${2:?--config needs a file}"; shift 2 ;;
        --speedtest) speedtest="${2:?--speedtest needs a file}"; shift 2 ;;
        --import-data) import_data="${2:?--import-data needs a directory}"; shift 2 ;;
        --user) user="${2:?--user needs a name}"; shift 2 ;;
        --no-start) start=0; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1' (see --help)" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0$(printf ' %q' "${args[@]}")"
command -v systemctl >/dev/null || die "systemd is needed"
case "$prefix" in /*) ;; *) die "--prefix must be an absolute path" ;; esac
bin_dest="$prefix/bin/meerkat"

if [ "$uninstall" -eq 1 ]; then
    say "stopping and removing meerkat.service"
    systemctl disable --now meerkat.service 2>/dev/null || true
    rm -f "$unit_dest" "$polkit_dest"
    if [ -r "$installed_list" ]; then
        while IFS= read -r f; do
            case "$f" in /*) rm -f -- "$f" ;; esac
        done < "$installed_list"
        rm -f "$installed_list"
    else
        rm -f "$bin_dest"
    fi
    systemctl daemon-reload
    say "kept: $data_dir (the evidence), $etc_dir, the user meerkat and $sysusers_dest"
    exit 0
fi

# --- checks before anything changes -------------------------------------------
[ -x "$binary" ] || die "no binary at $binary: build it first (cmake --preset release && cmake --build --preset release) or use --binary"
"$binary" --version >/dev/null || die "$binary doesn't run"
[ -z "$config" ] || [ -r "$config" ] || die "can't read --config $config"
[ -z "$speedtest" ] || [ -x "$speedtest" ] || die "--speedtest $speedtest isn't an executable file"
if [ -n "$user" ]; then
    id "$user" >/dev/null 2>&1 || die "no user '$user' (--user)"
fi
if [ -n "$import_data" ]; then
    [ -f "$import_data/speed.csv" ] || [ -d "$import_data/manifest" ] ||
        die "$import_data doesn't look like a meerkat data directory (no speed.csv, no manifest/)"
    if [ -d "$data_dir" ] && [ -n "$(ls -A "$data_dir")" ]; then
        die "$data_dir isn't empty: won't mix two evidence trails; move it away first"
    fi
    # A running recorder appends while we copy: the copy could end mid-row.
    # Any meerkat process with `run` among its arguments, flags anywhere.
    if pgrep -a -x meerkat | awk '{ for (i = 3; i <= NF; ++i) if ($i == "run") found = 1 } END { exit !found }'; then
        die "'meerkat run' is running: stop it first, so the copy is complete"
    fi
fi

# The program the service will run: [speed] program in the config that
# ends up installed, or `speedtest`.
if [ -n "$config" ]; then
    config_src="$config"
elif [ -e "$config_dest" ]; then
    config_src="$config_dest"
else
    config_src="$repo/config.example.toml"
fi
program="$(awk '
    /^[[:space:]]*\[/ { section = $0; gsub(/[][[:space:]]/, "", section) }
    section == "speed" && /^[[:space:]]*program[[:space:]]*=/ {
        sub(/^[^=]*=[[:space:]]*"/, ""); sub(/".*/, ""); print; exit
    }' "$config_src")"
program="${program:-speedtest}"
case "$program" in
    /home/*|/root/*|/run/user/*)
        die "the config runs the Ookla CLI from $program, which the service can't reach (ProtectHome=yes): use --speedtest $program and set program = \"speedtest\" in the config" ;;
    /*)
        [ -x "$program" ] || die "the config's [speed] program $program isn't an executable file" ;;
    *)
        if [ -n "$speedtest" ] && [ "$program" != speedtest ]; then
            die "--speedtest installs 'speedtest', but the config runs '$program'"
        fi
        if [ -z "$speedtest" ] && ! PATH="$service_path" command -v "$program" >/dev/null; then
            hint=""
            if [ -n "$user" ]; then
                home="$(getent passwd "$user" | cut -d: -f6)"
                [ -x "$home/.local/bin/$program" ] && hint=" (e.g. --speedtest $home/.local/bin/$program)"
            fi
            die "the service can't find the Ookla CLI '$program' in /usr/local/bin or /usr/bin: install it there, or pass --speedtest FILE$hint"
        fi ;;
esac

# --- install ---------------------------------------------------------------------
say "creating the user meerkat ($sysusers_dest)"
install -D -m 0644 "$here/meerkat.sysusers" "$sysusers_dest"
systemd-sysusers "$sysusers_dest"
install -d -m 0750 -o root -g meerkat "$etc_dir"
chown root:meerkat "$etc_dir"
chmod 0750 "$etc_dir"

say "installing $bin_dest"
install -D -m 0755 "$binary" "$bin_dest"
echo "$bin_dest" > "$installed_list"
if [ -n "$speedtest" ]; then
    say "installing the Ookla CLI to $speedtest_dest"
    install -D -m 0755 "$speedtest" "$speedtest_dest"
    echo "$speedtest_dest" >> "$installed_list"
fi
chmod 0644 "$installed_list"

if [ -d "$polkit_dir" ] || [ -d /usr/share/polkit-1 ]; then
    say "installing the polkit rule $polkit_dest"
    install -D -m 0644 "$here/50-meerkat.rules" "$polkit_dest"
else
    echo "    no polkit here: logind decides alone; see 'meerkat check' if it can't keep the computer awake"
fi

say "installing the config $config_dest"
if [ -n "$config" ]; then
    install -m 0640 -o root -g meerkat "$config" "$config_dest"
elif [ ! -e "$config_dest" ]; then
    install -m 0640 -o root -g meerkat "$repo/config.example.toml" "$config_dest"
    echo "    from config.example.toml: put your contract figures into it ([contract])"
else
    echo "    kept the existing one"
fi
if grep -Eq '^[[:space:]]*data_dir[[:space:]]*=' "$config_dest"; then
    echo "    note: $config_dest sets data_dir; the service's data then isn't in $data_dir"
fi

if [ -n "$import_data" ]; then
    say "copying $import_data to $data_dir"
    install -d -m 0750 -o meerkat -g meerkat "$data_dir"
    cp -a --no-preserve=ownership "$import_data"/. "$data_dir"/
    chown -R meerkat:meerkat "$data_dir"
    # The group may read (report, verify, export), nobody else; never write.
    chmod -R g+rX,g-w,o-rwx "$data_dir"
fi

say "installing the unit $unit_dest"
sed "s|^ExecStart=/usr/local/bin/meerkat |ExecStart=$bin_dest |" "$here/meerkat.service" > "$unit_dest.tmp"
chmod 0644 "$unit_dest.tmp"
mv "$unit_dest.tmp" "$unit_dest"
systemctl daemon-reload

if [ -n "$user" ]; then
    say "adding $user to the group meerkat (to read the data; log in again for it to apply)"
    usermod -a -G meerkat "$user"
fi

if [ "$start" -eq 1 ]; then
    if systemctl is-active --quiet meerkat.service; then
        say "restarting meerkat.service (the new binary and config take effect)"
        systemctl enable meerkat.service
        systemctl restart meerkat.service
    else
        say "enabling and starting meerkat.service"
        systemctl enable --now meerkat.service
    fi
    sleep 3
    systemctl --no-pager --lines=0 status meerkat.service || true

    say "checking the setup as the service sees it (same user and sandbox)"
    # The unit's hardening lines, as properties of a one-off run.
    sandbox=()
    while IFS= read -r line; do
        sandbox+=(-p "$line")
    done < <(sed -n '/^NoNewPrivileges=/,/^SystemCallErrorNumber=/{/^[A-Za-z]/p}' "$unit_dest")
    systemd-run --quiet --wait --pipe --collect -p User=meerkat -p Group=meerkat -p UMask=0027 \
        -p Environment=HOME=/var/cache/meerkat -p Environment=STATE_DIRECTORY="$data_dir" \
        -p ReadWritePaths="$data_dir" "${sandbox[@]}" \
        "$bin_dest" check --config "$config_dest" || true
fi

cat <<EOF

Done.
  Logs:     journalctl -u meerkat -f
  Status:   systemctl status meerkat
  Report:   meerkat report --config $config_dest --data-dir $data_dir
            (as ${user:-a member of the group meerkat} once logged in again; or put
            data_dir = "$data_dir" in ~/.config/meerkat/config.toml)
  Pause:    sudo -u meerkat $bin_dest pause --for 2h --config $config_dest --data-dir $data_dir
  Update:   build, then run this script again
  Remove:   sudo $0 --uninstall
EOF
