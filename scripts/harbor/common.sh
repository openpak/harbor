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
    hits+="$(pgrep -aix harbor || true)"$'\n'
    hits+="$(pgrep -ax yuzu || true)"$'\n'
    hits+="$(pgrep -af Ryujinx.dll || true)"$'\n'
    hits+="$(pgrep -af Harbor.dll || true)"$'\n'
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

# Run a command under the machine-wide build lock, at the lowest CPU priority. The emulator
# check runs again inside the lock, because waiting for the lock can take a long time.
locked() {
    flock --close "$LOCK" nice -n 19 bash -c 'refuse_while_emulator_runs; exec "$@"' _ "$@"
}

SHORT_SHA="$(git -C "$SRC" rev-parse --short HEAD)"
START_TS=$(date +%s)
print_elapsed() {
    local s=$(( $(date +%s) - START_TS ))
    printf 'Elapsed: %dh %02dm %02ds\n' $((s / 3600)) $((s % 3600 / 60)) $((s % 60))
}
