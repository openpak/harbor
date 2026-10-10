#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Start the desktop Harbor (qlaunch) on a hidden Hyprland workspace, with an isolated portable data
# directory and a control FIFO for scripted input.
#
#   run-desktop.sh <name> [--fresh] [--docked] [--auto-sleep] [--console] [--netns auto|<address>]
#                  [-- extra harbor args]
#
# The console runs in handheld mode (the firmware's setup waits for input there; docked it stops at
# "remove the console from the dock"); --docked switches to docked.
#
# --netns puts the console in its own household: a user + network namespace (same uid) with a private
# /24, NATed by pasta and leaving the PC from its own LAN address (the uplink), so two consoles on this
# PC reach each other only through the router's public address, as two homes would.
#
# Run directory: $SCR/run/<name>/  (user/ = data, control = FIFO, stdout.log, pid)
# See docs/harbor/test-harness.md and scripts/harbor/drive.sh.
set -euo pipefail

SCR=$HOME/REPOS/Openpak/scratch/harbor-eden
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WT_NAME="$(basename "$SRC")"
if [ "$WT_NAME" = src ]; then BUILD_ROOT="$SCR/build"; else BUILD_ROOT="$SCR/build/$WT_NAME"; fi
HARBOR_BIN="${HARBOR_BIN:-$BUILD_ROOT/desktop/bin/harbor}"
FIRMWARE_ZIP=$HOME/Downloads/Firmware.23.0.1.zip
FIRMWARE_DIR="$SCR/run/_fw-23.0.1"

export TMPDIR="$SCR/tmp"
mkdir -p "$TMPDIR"

die() { echo "run-desktop: $*" >&2; exit 2; }

[ $# -ge 1 ] || die "usage: run-desktop.sh <name> [--fresh] [--docked] [--auto-sleep] [--console] [--netns auto|<address>] [-- extra harbor args]"
NAME="$1"; shift
[[ "$NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "bad run name: $NAME"
FRESH=0
DOCKED=0
AUTO_SLEEP=0
CONSOLE=0
NETNS=""
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --fresh) FRESH=1 ;;
        --docked) DOCKED=1 ;;
        --auto-sleep) AUTO_SLEEP=1 ;;
        --console) CONSOLE=1 ;;
        --netns) shift; NETNS="${1:-}"; [ -n "$NETNS" ] || die "--netns needs auto or an address" ;;
        --) shift; EXTRA=("$@"); break ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done

RUN="$SCR/run/$NAME"
[ -x "$HARBOR_BIN" ] || die "no binary at $HARBOR_BIN (build with scripts/harbor/build-desktop.sh, or set HARBOR_BIN)"
mkdir -p "$RUN"

# Refuse to start a second instance on the same run directory.
if [ -f "$RUN/pid" ] && kill -0 "$(cat "$RUN/pid")" 2>/dev/null \
    && [ "$(readlink "/proc/$(cat "$RUN/pid")/cwd" 2>/dev/null)" = "$RUN" ]; then
    die "run '$NAME' is already running (pid $(cat "$RUN/pid")); drive.sh $NAME quit first"
fi

if [ "$FRESH" = 1 ]; then
    # Guard: only ever delete a user/ directory that lives under $SCR/run/.
    target="$(realpath -m "$RUN/user")"
    case "$target" in
        "$SCR/run/"?*/user) rm -rf --one-file-system "$target" ;;
        *) die "refusing to wipe $target" ;;
    esac
fi

# Firmware: extract once, copy into this run's registered cache.
if [ ! -f "$FIRMWARE_DIR/.done" ]; then
    [ -f "$FIRMWARE_ZIP" ] || die "missing $FIRMWARE_ZIP"
    mkdir -p "$FIRMWARE_DIR"
    unzip -qo "$FIRMWARE_ZIP" -d "$FIRMWARE_DIR"
    touch "$FIRMWARE_DIR/.done"
fi
REGISTERED="$RUN/user/nand/system/Contents/registered"
if [ ! -d "$REGISTERED" ]; then
    mkdir -p "$REGISTERED"
    find "$FIRMWARE_DIR" -maxdepth 1 -name '*.nca' -exec cp --reflink=auto -t "$REGISTERED" {} +
fi

# Keys: symlinks only, never copied.
mkdir -p "$RUN/user/keys"
for k in prod title; do
    ln -sfn "$HOME/.switch/$k.keys" "$RUN/user/keys/$k.keys"
done

# OpenPak: a run with no user/openpak/ never connects, so every Nintendo name fails closed and the
# account sign-in page shows 2810-1006. A new run gets the shared test template: the public server
# settings (server.json) and the news-test-boot marker that makes a --qlaunch run connect at boot.
# Never copied from the owner's data. The CA, network profile and ceiling are fetched at boot, and
# the console identity comes from the run's own calibration.
TEMPLATE="$SCR/run/_openpak-template"
if [ ! -f "$TEMPLATE/server.json" ]; then
    mkdir -p "$TEMPLATE"
    printf '{\n    "website": "https://openpak.org"\n}\n' > "$TEMPLATE/server.json"
    : > "$TEMPLATE/news-test-boot"
fi
if [ ! -d "$RUN/user/openpak" ]; then
    mkdir -p "$RUN/user/openpak"
    cp -n "$TEMPLATE/server.json" "$TEMPLATE/news-test-boot" "$RUN/user/openpak/"
fi

# Config: written once per user/ dir so no first-run prompt appears. Keys written with
# "\default=false" override Eden's defaults; everything else keeps them.
CONFIG="$RUN/user/config/qt-config.ini"
if [ ! -f "$CONFIG" ]; then
    mkdir -p "$RUN/user/config"
    cat > "$CONFIG" <<'EOF'
[Controls]
player_0_connected\default=false
player_0_connected=false
player_8_connected\default=false
player_8_connected=true
player_8_type\default=false
player_8_type=4

[UI]
firstStart\default=false
firstStart=false
confirmStop\default=false
confirmStop=2
gui_hide_backend_warning\default=false
gui_hide_backend_warning=true
check_for_updates\default=false
check_for_updates=false
pauseWhenInBackground\default=false
pauseWhenInBackground=false
muteWhenInBackground\default=false
muteWhenInBackground=false
enable_gamemode\default=false
enable_gamemode=false

[Renderer]
use_vsync\default=false
use_vsync=0

[Miscellaneous]
flush_line\default=false
flush_line=true

[Audio]
output_engine\default=false
output_engine=3
EOF
fi

# Data directories always point into this run. Eden stores absolute paths in the config, so a run
# copied from another run would otherwise keep writing into the original's NAND and SD card.
sed -i -E "s#^([a-z_]+_directory=).*/user(/.*)?\$#\\1$RUN/user\\2#" "$CONFIG"

# Console mode (re-applied on every start, so --docked works on an existing run).
sed -i '/^use_docked_mode/d' "$CONFIG"
if ! grep -q '^\[System\]' "$CONFIG"; then printf '\n[System]\n' >> "$CONFIG"; fi
sed -i "/^\[System\]/a use_docked_mode\\\\default=false\nuse_docked_mode=$DOCKED" "$CONFIG"

# Household (--netns): the uplink is a LAN address of this PC on the router's subnet, other than the
# one the default route leaves from (the other consoles and the desktop use that one) and not used by
# another household: two households on one uplink share pasta's host ports, so a game binding the same
# port in both gets one public mapping for two consoles.
ip2int() { local IFS=.; set -- $1; echo $(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 )); }
if [ -n "$NETNS" ]; then
    # pasta re-executes itself as passt.avx2, so match the command line, not the process name.
    used=" $( (pgrep -af '^pasta .* -o [0-9.]+ ' || true) | sed -n 's/.* -o \([0-9.]*\) .*/\1/p' | tr '\n' ' ')"
    command -v pasta >/dev/null || die "--netns needs pasta (passt package)"
    primary="$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')"
    lan="$(ip -4 -o addr show scope global | awk -v a="$primary" '{split($4, x, "/"); if (x[1] == a) print $4}')"
    net=$(( $(ip2int "${lan%/*}") >> (32 - ${lan#*/}) ))
    if [ "$NETNS" = auto ]; then
        NETNS=""
        for a in $(ip -4 -o addr show scope global | awk '{split($4, x, "/"); print x[1]}' | sort -u -t. -k4,4n); do
            [ "$a" = "$primary" ] && continue
            [ $(( $(ip2int "$a") >> (32 - ${lan#*/}) )) = "$net" ] || continue
            [[ "$used" == *" $a "* ]] && continue
            NETNS="$a"; break
        done
        [ -n "$NETNS" ] || die "--netns auto: no free LAN address besides $primary (give one with --netns <address>)"
    fi
    [[ "$(ip -4 -o addr show scope global)" == *" $NETNS/"* ]] || die "--netns: $NETNS is not an address of this PC"
    [ "$NETNS" != "$primary" ] || echo "run-desktop: warning: uplink $NETNS is the default route's address, which the other consoles share" >&2
    [[ "$used" != *" $NETNS "* ]] || die "--netns: uplink $NETNS is already used by another household"
    HOUSEHOLD="192.168.$(( 100 + ${NETNS##*.} % 150 ))"
    printf 'uplink=%s\nlan=%s.2/24\ngateway=%s.1\n' "$NETNS" "$HOUSEHOLD" "$HOUSEHOLD" > "$RUN/household"
fi

# Launcher: runs inside Hyprland's exec_cmd, so the environment is set here.
LAUNCH="$RUN/launch.sh"
{
    echo '#!/bin/sh'
    echo "cd '$RUN'"
    echo "export TMPDIR='$TMPDIR'"
    echo "export HARBOR_CONTROL='$RUN/control'"
    # Every window Harbor opens carries the harness app id, so the window rule below hides it:
    # Qt's (the console, its dialogs, the crash and stopped pages) through HARBOR_CONTROL, SDL's
    # (the Borealis setup wizard) through SDL_APP_ID.
    echo "export SDL_APP_ID='org.openpak.harbor.harness'"
    [ "$AUTO_SLEEP" = 1 ] && echo "export HARBOR_AUTO_SLEEP=1"
    # The setup wizard's scripted answers (test-harness.md), passed on from this shell.
    for v in HARBOR_SETUP_PICKS HARBOR_SETUP_TEXT HARBOR_SETUP_AUTO; do
        [ -n "${!v:-}" ] && printf 'export %s=%q\n' "$v" "${!v}"
    done
    # Console mode (--console) is the owner's: the setup wizard when the setup is not complete,
    # then HOME, whose Restart and Turn Off restart and close Harbor. Otherwise --qlaunch boots
    # HOME directly and power requests only end the session.
    if [ "$CONSOLE" = 1 ]; then cmd="'$HARBOR_BIN' --harbor-console"; else cmd="'$HARBOR_BIN' --qlaunch"; fi
    for a in "${EXTRA[@]}"; do cmd+=" '$a'"; done
    if [ -z "$NETNS" ]; then
        echo "echo \$\$ > '$RUN/pid'"
        echo "exec $cmd > '$RUN/stdout.log' 2>&1"
    else
        # The console in its own user + network namespace, mapped to the same uid (files, D-Bus and
        # the Wayland socket work as before). pasta joins it, gives it the household address and
        # NATs it out of the uplink: no ports forwarded in either direction, so only replies to
        # flows the console opened come back, as through a home router. pasta exits with the console.
        echo "rm -f '$RUN/netns-ready' '$RUN/pasta.pid'"
        echo "unshare --user --map-current-user --net -- sh -c 'echo \$\$ > \"\$0/pid\"; while [ ! -e \"\$0/netns-ready\" ]; do sleep 0.05; done; exec \"\$@\"' '$RUN' $cmd > '$RUN/stdout.log' 2>&1 &"
        echo "while [ ! -s '$RUN/pid' ]; do sleep 0.05; done"
        echo "pasta -4 -t none -u none -T none -U none -l '$RUN/pasta.log' --config-net -a $HOUSEHOLD.2/24 -g $HOUSEHOLD.1 -o $NETNS -P '$RUN/pasta.pid' \"\$(cat '$RUN/pid')\" || kill \"\$(cat '$RUN/pid')\""
        echo "touch '$RUN/netns-ready'"
        echo "wait"
    fi
} > "$LAUNCH"
chmod +x "$LAUNCH"
rm -f "$RUN/pid" "$RUN/control"

# Every window of the harness app id (dialogs included) goes to a hidden special workspace.
hyprctl dispatch '(function() hl.window_rule({ name = "harbor-harness", match = { class = "org.openpak.harbor.harness" }, workspace = "special:harbor-harness silent", float = true }) return hl.dsp.no_op() end)()' >/dev/null

hyprctl dispatch "hl.dsp.exec_cmd(\"$LAUNCH\", { workspace = \"special:harbor-$NAME silent\", float = true, size = \"1280 760\" })" >/dev/null

# Wait for the process and its window.
PID=""
for _ in $(seq 1 100); do
    [ -f "$RUN/pid" ] && PID="$(cat "$RUN/pid")" && break
    sleep 0.1
done
[ -n "$PID" ] || die "the launcher did not start"
STABLE=""
for _ in $(seq 1 300); do
    kill -0 "$PID" 2>/dev/null || die "harbor exited early; see $RUN/stdout.log and $RUN/user/log/eden_log.txt"
    STABLE="$(hyprctl clients -j | jq -r --argjson pid "$PID" '.[] | select(.pid == $pid) | .stableId' | head -1)"
    [ -n "$STABLE" ] && break
    sleep 0.2
done
echo "$STABLE" > "$RUN/window"
echo "run=$NAME pid=$PID window=${STABLE:-none} dir=$RUN"
echo "log: $RUN/user/log/eden_log.txt"
