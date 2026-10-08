#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Scripted check of the sleep sequence on a running HOME session (run-desktop.sh <name>, at HOME
# or in a title): a short power press must take the console through the sequence to sleep, a
# second one must wake it, and HOME must be told the sequence finished.
#
#   check-sleep.sh <name>
#
# Expected log lines, in order (user/log/eden_log.txt):
#   am: sleep sequence N requested by pid P             (HOME answered DetectShortPressingPowerButton)
#   am: sleep sequence N: the console sleeps
#   psc: PmState SleepReady -> EssentialServicesSleepReady
#   psc: PmState MinimumAwake -> FullAwake
#   am: sleep sequence N: the console wakes (power button)
#   am: -> pid P FinishedSleepSequence
set -euo pipefail

SCR=$HOME/REPOS/Openpak/scratch/harbor-eden
DRIVE="$(dirname "$0")/drive.sh"
[ $# -eq 1 ] || { echo "usage: check-sleep.sh <name>" >&2; exit 2; }
LOG="$SCR/run/$1/user/log/eden_log.txt"
[ -r "$LOG" ] || { echo "check-sleep: no log at $LOG" >&2; exit 2; }

start=$(wc -l < "$LOG")
since() { tail -n +"$((start + 1))" "$LOG"; }
wait_for() {
    local pattern=$1 seconds=$2
    for _ in $(seq "$seconds"); do
        since | grep -- "$pattern" >/dev/null && return 0
        sleep 1
    done
    echo "FAIL: no '$pattern' within ${seconds} s" >&2
    exit 1
}

"$DRIVE" "$1" power-tap
wait_for 'am: sleep sequence [0-9]* requested' 10
wait_for 'the console sleeps' 40
wait_for 'PmState SleepReady -> EssentialServicesSleepReady' 5
sleep 3
"$DRIVE" "$1" power-tap
wait_for 'the console wakes' 10
wait_for 'PmState MinimumAwake -> FullAwake' 5
wait_for 'FinishedSleepSequence' 5
echo "PASS: sleep and wake ($(since | grep -c 'unknown contract') unknown-contract line(s) on the way)"
