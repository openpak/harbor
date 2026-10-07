# Harbor desktop test harness

Runs the desktop Harbor (firmware qlaunch) invisibly on a hidden Hyprland workspace, with scripted
input, screenshots and a guest thread dump. Nothing touches the owner's keyboard focus or data.

## Usage

```
scripts/harbor/run-desktop.sh <name> [--fresh] [--docked] [-- extra harbor args]
scripts/harbor/drive.sh <name> <command> [args...] [<command> ...]
```

`run-desktop.sh` uses the binary of the worktree it is run from
(`$SCR/build/<worktree>/desktop/bin/harbor`, or `$SCR/build/desktop` for the main checkout); set
`HARBOR_BIN` to override. It creates `$SCR/run/<name>/` (the run directory is the working directory,
so the data lives in `<run>/user/`), extracts firmware 23.0.1 once into `$SCR/run/_fw-23.0.1/`,
symlinks `~/.switch/{prod,title}.keys`, writes a minimal `qt-config.ini` (no first-run prompts,
vsync Immediate, null audio, line-flushed log, no pause or confirm dialogs, Player 1 and Handheld
connected), and starts `harbor --qlaunch` in handheld mode (the 23.0.1 starter stops at "remove the
console from the dock" when docked; `--docked` selects docked, re-applied on every start). `--fresh`
wipes only `<run>/user/` (refused unless the path is under `$SCR/run/`). It prints the pid and the
Hyprland stable window id. Files in the run directory: `stdout.log`, `pid`, `window`, `control`
(the FIFO), `launch.sh`; the emulator log is `user/log/eden_log.txt`.

Example:

```
scripts/harbor/run-desktop.sh s1 --fresh
scripts/harbor/drive.sh s1 sleep 15 screenshot shots/lock.png press A sleep 3 screenshot shots/home.png
scripts/harbor/drive.sh s1 home-long threads quit
```

## Commands

Written to `<run>/control` (the harness thread logs each one as `harbor-control: ...`).

| Command | Effect |
|---|---|
| `press <button> [ms]` | hold the button for `ms` (default 100), then release |
| `hold <button>` / `release <button>` | press or release until told otherwise |
| `stick <l\|r> <x> <y> [ms]` | set a stick (-1..1); with `ms` it returns to centre afterwards |
| `home` | Home for 100 ms (short press, below the poller's 400 ms) |
| `home-long` | Home for 1200 ms (long press, above the poller's 800 ms) |
| `capture` | Capture button, short press |
| `power` | not supported: this build has no power button path; logs a warning |
| `type <text>` | types the text on the emulated USB keyboard (US layout); logged |
| `type-line <file> <n>` | types line `n` (1-based) of `file`, only the value when the line is `key=value`; for secrets: logged only as `harbor-control: type-line (N chars)`, never echoed |
| `key <name>` | one key: `Enter`, `Backspace`, `Tab`, `Escape`, `Space`, `Up`, `Down`, `Left`, `Right`, `Delete`, `Home`, `End` |
| `detach` / `attach` | Joy-Cons off / back on the rails: Handheld disconnects and Player 1 connects as dual Joy-Con, and back (F7 in the window toggles it); logs `harbor: Joy-Cons detached ...` |
| `screenshot <png>` | renderer screenshot (frame only, no window chrome) |
| `threads` | guest thread dump to the log |
| `quit` | closes the main window (clean shutdown, about 10 s) |

Buttons: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK`. `drive.sh` also takes
`sleep <seconds>`. Relative screenshot paths land in the run directory.

Input goes through InputCommon's virtual gamepad (the path the Android overlay uses), which every
emulated controller already maps. It is sent to Player 1 and Handheld together, so Home and Capture
reach `am/button_poller.cpp`, which logs `system button press, type=N` (1 = home short, 2 = home
long, 6 = capture short, 7 = capture long) in `WindowSystem::OnSystemButtonPress`.

Typing goes to the emulated USB keyboard (hid keyboard, `keyboard_enabled` is switched on for it),
which the firmware's own software keyboard reads as a console reads a USB keyboard. hid writes the
keyboard into the shared memory of the most recently created applet resource, which is the swkbd
applet while it is open. While Eden's Qt keyboard dialog is showing (swkbd HLE, outside a HOME
session) the keys go to that dialog instead. Only printable ASCII is typed; a line with any other
character types nothing.

## Thread dump and svcBreak

`threads` (and F12 in the Qt window) calls `System::LogGuestThreads()`: for every guest process
the pid, program id and name, and for every thread id, name (when the guest set one), priority, core,
state and wait reason, PC/LR/SP and up to 16 backtrace frames. Lines start with `thread dump:`. A
thread running on a core is read live from the core (marked `live context`), others from their saved
context. Frames show `unknown+0x...` for modules without a symbol table (qlaunch has none).

Every `svcBreak` logs one `svcBreak:` line first: reason, notification-only flag, info1/info2, the
Result (as `2xxx-yyyy`) or the buffer contents (up to 0x100 bytes, when the size is at most 0x400),
and the process, thread, PC and LR.

## Hidden workspace

`run-desktop.sh` adds a Hyprland window rule for the app id `org.openpak.harbor.harness` (used by the
binary whenever `HARBOR_CONTROL` is set) so every window of the run, dialogs included, opens on
`special:harbor-harness` without taking focus. A hidden window gets no Wayland frame callbacks, so
the default FIFO vsync blocks the present after a few seconds and the guest stops; the run config
therefore uses `use_vsync=0` (Immediate). Verified: two screenshots seconds apart on the animated
first screen differ.

## Limits

- The renderer captures a screenshot on the next presented frame. A guest that is idle (a static
  HOME) presents none, so the request times out after 5 s; `drive.sh` then falls back to `grim -T`
  of the window (includes the Qt menu and status bar). A request that timed out stays pending until
  the next frame, so later requests fail fast until the guest draws again.
- The thread dump reads guest memory and kernel lists while the guest runs; it is a debugging aid,
  not a consistent snapshot. A thread running on a core is shown with its live registers.
- Eden's static destructors crash at exit after a HOME session (`Service::Event` teardown). With
  `HARBOR_CONTROL` set the binary flushes the log and `_exit`s instead, so a `quit` is clean and
  leaves no core dump; the underlying crash is still there without the harness.
- Guest memory reads from a non-core host thread (the thread dump, F12) used to index past
  `rasterizer_read_areas` in `core/memory.cpp` and crash; this branch clamps the index.
- `quit` closes the window; `confirmStop` is set to never ask. If a modal guest dialog is open,
  the main window may not close; kill the pid in `<run>/pid` (your own run only).
