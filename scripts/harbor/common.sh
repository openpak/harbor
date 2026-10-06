# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Shared setup for the Harbor build scripts. Source it, do not run it.

SCR=$HOME/REPOS/Openpak/scratch/harbor-eden
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCK=/tmp/openpak-build.lock

# Refuse to build while an emulator is running on this machine.
refuse_while_emulator_runs() {
    local hits=""
    hits+="$(pgrep -ax Ryujinx || true)"$'\n'
    hits+="$(pgrep -ax eden || true)"$'\n'
    hits+="$(pgrep -ax harbor || true)"$'\n'
    hits+="$(pgrep -ax yuzu || true)"$'\n'
    hits+="$(pgrep -af Ryujinx.dll || true)"$'\n'
    hits+="$(pgrep -af 'org.openpak' || true)"$'\n'
    hits="$(printf '%s' "$hits" | sed '/^$/d')"
    if [ -n "$hits" ]; then
        echo "An emulator is running; not building:" >&2
        echo "$hits" >&2
        exit 3
    fi
}

# Everything that would land in /tmp goes to the workspace instead.
export TMPDIR="$SCR/tmp"
mkdir -p "$TMPDIR"

# Run a command under the machine-wide build lock, at the lowest CPU priority.
locked() {
    flock --close "$LOCK" nice -n 19 "$@"
}

SHORT_SHA="$(git -C "$SRC" rev-parse --short HEAD)"
START_TS=$(date +%s)
print_elapsed() {
    local s=$(( $(date +%s) - START_TS ))
    printf 'Elapsed: %dh %02dm %02ds\n' $((s / 3600)) $((s % 3600 / 60)) $((s % 60))
}
