# Harbor desktop test harness

Runs the desktop Harbor (firmware qlaunch) invisibly on a hidden Hyprland workspace, with scripted
input, screenshots and a guest thread dump. Nothing touches the owner's keyboard focus or data.

## Usage

```
scripts/harbor/run-desktop.sh <name> [--fresh] [--docked] [-- extra harbor args]
scripts/harbor/drive.sh [--layout us|uk|auto] <name> <command> [args...] [<command> ...]
```

`run-desktop.sh` uses the binary of the worktree it is run from
(`$SCR/build/<worktree>/desktop/bin/harbor`, or `$SCR/build/desktop` for the main checkout); set
`HARBOR_BIN` to override. It creates `$SCR/run/<name>/` (the run directory is the working directory,
so the data lives in `<run>/user/`), extracts firmware 23.0.1 once into `$SCR/run/_fw-23.0.1/`,
symlinks `~/.switch/{prod,title}.keys`, gives a run with no `user/openpak/` the shared test template
`$SCR/run/_openpak-template/` (`server.json` with the public site, and the `news-test-boot` marker
that makes a `--qlaunch` run connect to OpenPak at boot; never the owner's data, so without it every
Nintendo name fails closed and the account sign-in shows 2810-1006), writes a minimal `qt-config.ini` (no first-run prompts,
vsync Immediate, null audio, line-flushed log, no pause or confirm dialogs, Handheld
connected and Player 1 not, so a title sees one controller), and starts `harbor --qlaunch` in handheld mode (the 23.0.1 starter stops at "remove the
console from the dock" when docked; `--docked` selects docked, re-applied on every start; `--auto-sleep` lets the run dim and auto-sleep, which harness runs otherwise never do).
`--console` starts console mode (`--harbor-console`), as the owner runs Harbor: the setup wizard
while `<run>/user/harbor-setup.json` is not complete, then HOME, whose Restart restarts the console
in the same process and Turn Off closes Harbor. Every window Harbor opens carries the harness app id
(Qt's through `HARBOR_CONTROL`, the Borealis wizard's through `SDL_APP_ID`), so the wizard, the crash
and stopped pages and every dialog stay on the hidden workspace. To skip the wizard, write a
completed record (`keys_present`, `firmware_version`, `game_dirs`, `dlc_dirs`, `update_dirs`,
`"completed": true`). `--fresh`
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
| `home-long` | Home for 1200 ms (long press, above the poller's 500 ms in a HOME session, 800 ms outside one) |
| `capture` | Capture button, short press |
| `power` | the power button, middle press (F5 in the window) |
| `power-tap` | the power button, short press (an F5 tap in the window): HOME starts the sleep sequence; while the console sleeps it wakes it |
| `auto-sleep <on\|off>` | harness runs neither dim nor auto-sleep (idle:sys) unless turned on here or started with `run-desktop.sh --auto-sleep` (`HARBOR_AUTO_SLEEP=1`); `check-sleep.sh --auto` turns it on for its check and off again |
| `terminate` | force-terminates the running application (F10 in the window) |
| `dock` | toggles docked / handheld (the Qt "Docked" menu entry); in console mode it switches silently, with no "Invalid config" dialog |
| `type <text>` | types the text on the emulated USB keyboard, following the guest's keyboard layout (see below); logged |
| `type-line <file> <n>` | types line `n` (1-based) of `file`, only the value when the line is `key=value`; for secrets: logged only as `harbor-control: type-line (N chars)`, never echoed |
| `layout <us\|uk\|auto>` | the layout `type` / `type-line` use (default `auto`; `--layout` is the same for a whole `drive.sh` call) |
| `key-timing <hold> <gap>` | how long a key is held and then left released, in ms (default 100 / 100) |
| `key <name>` | one key: `Enter`, `Backspace`, `Tab`, `Escape`, `Space`, `Up`, `Down`, `Left`, `Right`, `Delete`, `Home`, `End` |
| `detach` / `attach` | Joy-Cons off / back on the rails: Handheld disconnects and Player 1 connects as dual Joy-Con, and back (F7 in the window toggles it); logs `harbor: Joy-Cons detached ...` |
| `screenshot <png>` | renderer screenshot (frame only, no window chrome) |
| `menu <name>` | the Harbor menu, drawn on the console's screen (screenshots show it): `harbor` (the menu), `settings`, `frame-gen` (Display & Performance), `controllers`, `back`, `close`; and the console entries `backup`, `restore`, `new-console`, whose host dialogs then take `key` and `type` |
| `hostkey <name> [ms]` | a key as the render window passes it on (the keyboard engine: controller mappings and the Harbor menu, which reads Enter, Space, Escape, Backspace, the arrows and X), not the guest's USB keyboard: `Enter`, `Escape`, `Backspace`, `Space`, `Up`, `Down`, `Left`, `Right`, `Tab`, a letter or a digit |
| `pad plug` / `pad unplug` | an SDL virtual gamepad, "Harbor test pad", that Eden's SDL driver finds as it finds a real pad (its own GUID): for press-to-assign, per-controller mappings and reconnects |
| `pad tap <button> [ms]`, `pad down <button>`, `pad up <button>` | its buttons by SDL's names (`a b x y back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1`) or numbers |
| `pad axis <n> <value> [ms]` | its axes by SDL's numbers (0-1 left stick, 2-3 right stick, 4-5 triggers), -1 to 1 |
| `threads` | guest thread dump to the log |
| `quit` | closes the main window (clean shutdown, about 10 s) |

Buttons: `A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK`. `drive.sh` also takes
`sleep <seconds>`. Relative screenshot paths land in the run directory.

Input goes through InputCommon's virtual gamepad (the path the Android overlay uses), which every
emulated controller already maps. It is sent to Player 1 and Handheld together (whichever is connected), so Home and Capture
reach `am/button_poller.cpp`, which logs `system button press, type=N` (1 = home short, 2 = home
long, 6 = capture short, 7 = capture long) in `WindowSystem::OnSystemButtonPress`.

Typing goes to the emulated USB keyboard (hid keyboard, `keyboard_enabled` is switched on for it),
which the firmware's own software keyboard reads as a console reads a USB keyboard. hid writes the
keyboard into the shared memory of the most recently created applet resource, which is the swkbd
applet while it is open. While Eden's Qt keyboard dialog is showing (swkbd HLE, outside a HOME
session) the keys go to that dialog instead. Typing follows the guest's keyboard layout, because the firmware turns a key position into a
character with the layout set in System Settings (`set:sys` GetKeyboardLayout). Before every `type` and
`type-line`, `drive.sh` reads that layout (the last `GetKeyboardLayout`/`SetKeyboardLayout` line in the
run's `user/log/eden_log.txt`, else the field in the run's `nand/system/save/8000000000000050/settings.dat`)
and sends `layout us` or `layout uk` (layout 1 = English US, 3 = English UK; any other layout types as
US, with a warning). On UK, `@` and `"` swap places and `#`, `~`, `\` and `|` are on the two extra
keys (usage 0x32 and 0x64). Each key is held 100 ms and left released 100 ms (Shift is pressed and
released with the same gaps): the firmware reads the keyboard once per frame, so a shorter hold drops a
key and a shorter gap merges a doubled key ("66") into one press. Only printable ASCII is typed; a line with any other
character types nothing.

## The setup wizard in scripted runs

The wizard (before Qt) does not read the control FIFO. Under `HARBOR_CONTROL` it only runs with
`--harbor-setup` (and console mode only with `--harbor-setup` or `--harbor-console`). Two environment
variables answer its pickers and text fields without a dialog or a keyboard, in order:

| Variable | Answers |
|---|---|
| `HARBOR_SETUP_PICKS="<path>\|<path>\|..."` | the next file and folder pickers (prod.keys, the firmware zip, the game/DLC/update folders) |
| `HARBOR_SETUP_TEXT="<file>\|<n>\|=<text>\|..."` | the next text fields (the OpenPak account page): `<n>` is line `n` of `<file>`, only the value of a `key=value` line; `=<text>` is the text itself |
| `HARBOR_SETUP_AUTO=1` | walks the wizard by itself: Next where it can, the next picker where a page needs a file or folder, Skip (Later) on the driver and account pages, then Start Harbor |

`run-desktop.sh` passes these three on from its own environment. A first run end to end:

```
HARBOR_SETUP_AUTO=1 HARBOR_SETUP_PICKS="$SCR/run/<name>/games" scripts/harbor/run-desktop.sh <name> --fresh --console
```

(the keys and firmware are already in place, so the only picker is the games folder); the log shows
`harbor: setup closed, completed`, `harbor: setup completed; starting the console in a fresh process`, then
HOME booting in console mode.

`HARBOR_SETUP_TEXT` is for the test account: point it at the secrets file and give line numbers, so the
values never appear on a command line, in the log (only `harbor: scripted text`) or in this repo. The
field order is the page's: email, password, then (create mode) nickname, birthday, country. Fields
are opened with A (Return) on their row. A screenshot of a filled form shows the email address: crop
it out or take none.

## The Harbor menu in scripted runs

The menu takes over the console's own picture (video_core/host_screen.h), so `screenshot` captures it.
It reads the console's controllers: `press` (the virtual gamepad) navigates it, A confirms and B goes
back. The virtual gamepad is not a mappable device, so assignment is tested with the test pad:

```
drive.sh m1 pad plug menu controllers sleep 2 press RIGHT press A        # Player 1
drive.sh m1 press A sleep 1                                              # Controller: pick the test pad
drive.sh m1 press DOWN press A pad tap b sleep 1 screenshot shots/a.png   # A assigned to the pad's B
drive.sh m1 pad unplug sleep 2 pad plug sleep 3                          # it comes back mapped
```

The pad's mapping is kept in `user/config/harbor-input.json`, by its GUID; the log says
`harbor: player 1: a known controller came back, its saved mapping is applied` when it is applied again.

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
- Exit after a HOME session used to crash in `Service::Event`'s destructor (the general channel's
  event outlived the kernel); the event now goes with the session, so a `quit` exits normally, with
  no core dump. A core dump after `quit` is a real crash.
- Runs start in `--qlaunch` mode unless `--console` is given: there HOME's Restart and Turn Off end
  the session but do not restart or close Harbor (the window stays on its last screen); `quit` then.
- Guest memory reads from a non-core host thread (the thread dump, F12) used to index past
  `rasterizer_read_areas` in `core/memory.cpp` and crash; this branch clamps the index.
- `quit` closes the window; `confirmStop` is set to never ask. If a modal guest dialog is open,
  the main window may not close; kill the pid in `<run>/pid` (your own run only).
