#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Send commands to a run started by run-desktop.sh, through its control FIFO.
#
#   drive.sh [--layout us|uk|auto] <name> <command> [args...] [<command> ...]
#
# Commands (a command word starts a new command; arguments follow it):
#   press <button> [ms]   hold <button>   release <button>   stick <l|r> <x> <y> [ms]
#   home   home-long   capture   power   screenshot <png>   threads   quit
#   type <text>   type-line <file> <n>   key <name>   detach   attach   dock   terminate
#   layout <us|uk|auto>   key-timing <hold ms> <gap ms>
#   menu <harbor|settings|frame-gen|controllers|back|close|backup|restore|new-console>
#   hostkey <Enter|Escape|Backspace|Space|Up|Down|Left|Right|Tab|letter|digit> [ms]
#   pad plug   pad unplug   pad <tap|down|up> <button> [ms]   pad axis <n> <value> [ms]
#   sleep <seconds>       (handled here, not sent)
# type-line types line n of a file (the value alone for key=value) and is logged only by length: use it
# for passwords. type's text must not contain a command word.
# Typing follows the guest's keyboard layout: before every type / type-line, drive.sh reads the layout
# the guest uses (the last GetKeyboardLayout/SetKeyboardLayout in the run's log, else the system setting
# in the run's NAND) and tells the harness; 1 = US and 3 = UK are supported, any other layout types as US
# with a warning. --layout or the layout command overrides it. Keys are held and released for 100 ms each
# by default (the firmware reads the keyboard once per frame); key-timing changes that.
# Buttons: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK
# pad drives an SDL virtual gamepad ("Harbor test pad") that Eden finds like a real one; its buttons are
# SDL's names (a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft
# dpright misc1) or numbers, axes SDL's numbers (0-1 left stick, 2-3 right, 4-5 triggers).
# hostkey presses a key as the render window passes it on (controller mappings, the Harbor menu), not
# on the guest's USB keyboard (that is key).
# Relative screenshot paths land under the run directory. A screenshot is taken by the renderer (frame
# only, no window chrome); if the guest is idle and presents no frame, the window is captured with grim.
#
# Example: drive.sh s1 press A sleep 2 screenshot shots/1.png
set -euo pipefail

SCR=$HOME/REPOS/Openpak/scratch/harbor-eden
LAYOUT=auto
if [ "${1:-}" = "--layout" ]; then LAYOUT="${2:-}"; shift 2 || true; fi
[ $# -ge 2 ] || { echo "usage: drive.sh <name> <command>..." >&2; exit 2; }
NAME="$1"; shift
RUN="$SCR/run/$NAME"
FIFO="$RUN/control"
[ -f "$RUN/pid" ] && kill -0 "$(cat "$RUN/pid")" 2>/dev/null \
    || { echo "drive: run '$NAME' is not running" >&2; exit 2; }
[ -p "$FIFO" ] || { echo "drive: no control FIFO at $FIFO (is the run started?)" >&2; exit 2; }

is_command() {
    case "$1" in
        press|hold|release|stick|home|home-long|capture|power|screenshot|threads|quit|sleep|type|type-line|key|detach|attach|layout|key-timing|terminate|dock|menu|hostkey|pad) return 0 ;;
        *) return 1 ;;
    esac
}

# The guest's keyboard layout number (set:sys KeyboardLayout: 1 US, 3 UK). The log has the latest value
# the guest read or wrote; the NAND's settings.dat (16-byte header, then the struct; the field is at
# file offset 39360) is the fallback for a run whose guest has not asked yet.
guest_layout() {
    local n=""
    [ -r "$RUN/user/log/eden_log.txt" ] && n=$(grep -h 'KeyboardLayout: called, keyboard_layout=' "$RUN/user/log/eden_log.txt" \
        | tail -1 | sed 's/.*keyboard_layout=//' | tr -dc '0-9') || true
    local f="$RUN/user/nand/system/save/8000000000000050/settings.dat"
    if [ -z "$n" ] && [ -r "$f" ] && [ "$(stat -c %s "$f")" = 210608 ]; then
        n=$(od -An -tu4 -j39360 -N4 "$f" | tr -d ' ')
    fi
    echo "${n:-1}"
}

# Prints the layout argument for the harness: us or uk.
resolve_layout() {
    local l="$LAYOUT"
    [ "$l" = auto ] && l=$(guest_layout)
    case "$l" in
        us|1) echo us ;;
        uk|3) echo uk ;;
        *) echo "drive: keyboard layout '$l' is not supported; typing as US" >&2; echo us ;;
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
        layout) LAYOUT="${args[0]:-auto}"; emit "layout $(resolve_layout)" ;;
        type|type-line) emit "layout $(resolve_layout)"; emit "$cmd ${args[*]:-}" ;;
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
