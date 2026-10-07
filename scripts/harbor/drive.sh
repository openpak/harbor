#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Send commands to a run started by run-desktop.sh, through its control FIFO.
#
#   drive.sh <name> <command> [args...] [<command> ...]
#
# Commands (a command word starts a new command; arguments follow it):
#   press <button> [ms]   hold <button>   release <button>   stick <l|r> <x> <y> [ms]
#   home   home-long   capture   power   screenshot <png>   threads   quit
#   type <text>   type-line <file> <n>   key <name>   detach   attach
#   sleep <seconds>       (handled here, not sent)
# type-line types line n of a file (the value alone for key=value) and is logged only by length: use it
# for passwords. type's text must not contain a command word.
# Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK
# Relative screenshot paths land under the run directory. A screenshot is taken by the renderer (frame
# only, no window chrome); if the guest is idle and presents no frame, the window is captured with grim.
#
# Example: drive.sh s1 press A sleep 2 screenshot shots/1.png
set -euo pipefail

SCR=$HOME/REPOS/Openpak/scratch/harbor-eden
[ $# -ge 2 ] || { echo "usage: drive.sh <name> <command>..." >&2; exit 2; }
NAME="$1"; shift
RUN="$SCR/run/$NAME"
FIFO="$RUN/control"
[ -f "$RUN/pid" ] && kill -0 "$(cat "$RUN/pid")" 2>/dev/null \
    || { echo "drive: run '$NAME' is not running" >&2; exit 2; }
[ -p "$FIFO" ] || { echo "drive: no control FIFO at $FIFO (is the run started?)" >&2; exit 2; }

is_command() {
    case "$1" in
        press|hold|release|stick|home|home-long|capture|power|screenshot|threads|quit|sleep|type|type-line|key|detach|attach) return 0 ;;
        *) return 1 ;;
    esac
}

# Opening the FIFO read-write never blocks, even when nothing is reading it.
emit() { printf '%s\n' "$*" 1<>"$FIFO"; }

# The game-side screenshot waits for the next presented frame; wait for the file here.
while [ $# -gt 0 ]; do
    cmd="$1"; shift
    args=()
    while [ $# -gt 0 ] && ! is_command "$1"; do args+=("$1"); shift; done
    case "$cmd" in
        sleep) sleep "${args[0]:-1}" ;;
        screenshot)
            [ ${#args[@]} -ge 1 ] || { echo "drive: screenshot needs a path" >&2; exit 2; }
            path="${args[0]}"
            case "$path" in /*) ;; *) path="$RUN/$path" ;; esac
            rm -f "$path"
            emit "screenshot $path"
            for _ in $(seq 1 70); do [ -s "$path" ] && break; sleep 0.1; done
            if [ -s "$path" ]; then
                echo "screenshot: $path"
            elif [ -s "$RUN/window" ] && grim -T "$(cat "$RUN/window")" "$path" 2>/dev/null; then
                # The renderer only captures on a new frame; an idle guest never produces one.
                echo "screenshot (window capture, the guest is not presenting): $path"
            else
                echo "drive: screenshot did not appear: $path" >&2
            fi
            ;;
        *) emit "$cmd ${args[*]:-}" ;;
    esac
done
