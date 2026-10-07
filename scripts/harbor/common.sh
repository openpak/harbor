# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Shared setup for the Harbor build scripts. Source it, do not run it.

SCR=$HOME/REPOS/Openpak/scratch/harbor-eden
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCK=/tmp/openpak-build.lock

# Refuse to build while an emulator is running on this machine (exit 3).
# Covers Eden, Yuzu, Harbor (native or the Ryujinx build) and Ryujinx.
refuse_while_emulator_runs() {
    local hits=""
    hits+="$(pgrep -ax Ryujinx || true)"$'\n'
    hits+="$(pgrep -ax eden || true)"$'\n'
    # Harbor test runs started from $SCR/run (isolated data dirs) are ours and do not block builds.
    local pid
    for pid in $(pgrep -ix harbor || true); do
        case "$(readlink "/proc/$pid/cwd" 2>/dev/null)" in
            "$SCR/run"/*) ;;
            *) hits+="$(ps -o pid=,args= -p "$pid")"$'\n' ;;
        esac
    done
    hits+="$(pgrep -ax yuzu || true)"$'\n'
    hits+="$(pgrep -af '^[^ ]*dotnet[^ ]* .*Ryujinx\.dll' || true)"$'\n'
    hits+="$(pgrep -af '^[^ ]*dotnet[^ ]* .*Harbor\.dll' || true)"$'\n'
    hits="$(printf '%s' "$hits" | sed '/^$/d')"
    if [ -n "$hits" ]; then
        echo "An emulator is running; not building:" >&2
        echo "$hits" >&2
        exit 3
    fi
}
export -f refuse_while_emulator_runs

# Everything that would land in /tmp goes to the workspace instead.
export TMPDIR="$SCR/tmp"
mkdir -p "$TMPDIR"

# Run a command under a build slot, at the lowest CPU priority. Harbor has two slots: the machine-wide
# OpenPak lock (shared with every other project's builds) and one Harbor-only lock, so at most two builds
# run at once and other projects still wait for one slot only. A build also waits for 8 GiB of free RAM.
# The emulator check runs again inside the slot, because waiting can take a long time.
# ponytail: two slots and a fixed RAM floor; tune MIN_FREE_KB/slots if the machine changes.
MIN_FREE_KB=$((8 * 1024 * 1024))
# A build started with HARBOR_BUILD_PRIORITY=1 (the phone APK) goes first: while one waits, other
# Harbor builds do not take a free slot.
PRIORITY_DIR="$SCR/tmp/build-priority"
priority_waiting() {
    local f
    for f in "$PRIORITY_DIR"/*; do
        [ -e "$f" ] || continue
        if kill -0 "$(basename "$f")" 2>/dev/null; then return 0; fi
        rm -f "$f"
    done
    return 1
}
locked() {
    local slot rc
    if [ "${HARBOR_BUILD_PRIORITY:-0}" = 1 ]; then
        mkdir -p "$PRIORITY_DIR"; touch "$PRIORITY_DIR/$$"
        trap 'rm -f "$PRIORITY_DIR/$$"' EXIT
    fi
    while :; do
        if [ "${HARBOR_BUILD_PRIORITY:-0}" != 1 ] && priority_waiting; then
            sleep 10; continue
        fi
        for slot in "$LOCK" "$LOCK.harbor2"; do
            if [ "$(awk '/MemAvailable/ {print $2}' /proc/meminfo)" -lt "$MIN_FREE_KB" ]; then
                break
            fi
            rc=0
            # The priority marker goes as soon as the slot is held, so the other slot is free to use.
            flock -n -E 200 --close "$slot" nice -n 19 \
                env HARBOR_PRIORITY_MARK="$PRIORITY_DIR/$$" bash -c \
                'rm -f "$HARBOR_PRIORITY_MARK"; refuse_while_emulator_runs; exec "$@"' _ "$@" || rc=$?
            [ "$rc" -ne 200 ] && return "$rc"
        done
        sleep 10
    done
}

# Each worktree builds into its own directory; the main checkout keeps $SCR/build/desktop.
WT_NAME="$(basename "$SRC")"
if [ "$WT_NAME" = src ]; then BUILD_ROOT="$SCR/build"; else BUILD_ROOT="$SCR/build/$WT_NAME"; fi
export SCR

SHORT_SHA="$(git -C "$SRC" rev-parse --short HEAD)"
START_TS=$(date +%s)
print_elapsed() {
    local s=$(( $(date +%s) - START_TS ))
    printf 'Elapsed: %dh %02dm %02ds\n' $((s / 3600)) $((s % 3600 / 60)) $((s % 60))
}
