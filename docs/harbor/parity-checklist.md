# Harbor on Eden: parity checklist

2026-10-06. Draft for phase 1 task 5. The Eden Harbor replaces the Ryujinx Harbor only when every item below passes and tobagin validates it (plan §7, phase 5). Every Eden status starts as "not started".

## How to read this

- One checkbox per testable behaviour: what a tester does and sees. Tick it only with evidence (log line, capture, test run) recorded outside the repository.
- `Harbor:` is the Ryujinx Harbor's state (`feature/android-port`, `ee683886b`), not Eden's.
  - **verified on PC**: run on desktop against an isolated data directory.
  - **verified on phone**: run on the ROG Phone 3.
  - **unverified**: implemented, designed or unit-tested only; no recorded run.
  - **NEVER PROVED**: Harbor itself never showed it working end to end. Record a golden IPC trace on the Ryujinx Harbor with a test account before porting (plan §6, risk 3), then diff Eden against it.
- `[D]` desktop Linux, `[P]` ROG Phone 3 (legacy flavour), `[M]` the API 33+ device (mainline flavour). An item tagged `[D P M]` runs on all three.
- Doc refs are file names under `emulators/ryujinx-android/docs/android-port/` or `docs/harbor/`, plus `checklist` (`prds/android-port/checklist.md`), `HANDOVER`, `acceptance` and `ledger` (OpenPak `docs/switch/emulator-console-acceptance-checklist.md`, `emulator-parity-ledger.md`).
- Rules for every run: isolated data directory, never tobagin's live data; no build or restart while tobagin is playing; one build at a time; the phone only with tobagin's say-so; evidence (logs, captures, traces) stays outside the repository.
- Privacy applies to every item: evidence must not contain account ids, tokens, names, nicknames, PINs, pairing codes or typed text.

## Device matrix

| Device | Flavour | Facts | Harbor so far |
|---|---|---|---|
| Desktop Linux (Qt) | desktop | keyboard, mouse, pad; isolated `--root-data-dir` | firmware 23.0.1 setup, HOME, applets, game launch verified on PC |
| ROG Phone 3 | legacy (minSdk 29) | Android 12, API 31, Adreno 650, stock Vulkan 1.1.128, 4 KiB pages, 12 GB, Kunai GamePad II (VID 0b05, PID 7904) | firmware 22.5.0 setup and applets, AAudio fix, Approve input fixed; firmware 23.0.1, games and drivers still pending |
| API 33+ device | mainline | device TBD | nothing (Harbor has one flavour) |

Hardware, dumps and test accounts come from tobagin. Record SoC, GPU, driver, Android/API, page size, RAM, settings and content version with every device result.

## Gate 1: repo, builds, clean picks (phase 1)

### Identity and hygiene
- [ ] **G1.ID.1** [P] The legacy APK installs beside the Ryujinx Harbor as `org.openpak.harbor.eden`, label "Harbor", lighthouse launcher icon (colour and monochrome). Harbor: verified on phone for install-over (HANDOVER); launcher icon on phone unverified (wizard-presentation). Eden: not started
- [ ] **G1.ID.2** [D P M] The built manifest and APK contain no `genshinSpoof` or `chromeOS` flavour, no `play {}` block, no `REQUEST_INSTALL_PACKAGES`, no Lossless DLL page. Harbor: not applicable, never had them (host.md §4). Eden: not started
- [ ] **G1.ID.3** [P M] `allowBackup` is false in the built manifest. Harbor: unverified (plan §5.7). Eden: not started
- [ ] **G1.ID.4** [D P M] Built with web service, update checker and Discord presence off; a capture of an idle first start shows no connection to Eden, Forgejo, GitHub or any third-party host. Harbor: unverified (by construction no such code; no capture recorded). Eden: not started

### Builds
- [ ] **G1.BLD.1** [D] The Linux Qt build succeeds from a clean checkout with every cache on the pool; cold build time recorded. Harbor: verified on PC, desktop Release 0 errors (checklist, account-browser-input). Eden: not started
- [ ] **G1.BLD.2** [P] The legacy APK builds signed; `zipalign -c -P 16` passes and every `.so` has LOAD alignment 0x4000. Harbor: verified build and alignment; runtime on a 16 KiB device untested (checklist M2, controller-support). Eden: not started
- [ ] **G1.BLD.3** [M] The mainline APK builds signed with the same alignment checks, including prebuilt FFmpeg and validation-layer libraries. Harbor: no equivalent (single flavour). Eden: not started
- [ ] **G1.BLD.4** [D] Each of the fork's clean cherry-picks builds on its own; none of the eight clean-room-listed commits (`73da2f25b7`, `497320b975`, `fd1b755fc4`, `03d14d62d1`, `fa69cbe39f`, `fb99c24f47`, `3ab97fbe7`, `081d5cd88`) is present. Harbor: not applicable. Eden: not started

### Stock Eden baseline
- [ ] **G1.BASE.1** [D] `-qlaunch` on an isolated data directory with firmware 23.0.1: record the first point where stock Eden stops. Harbor: reaches HOME on PC (firmware-23). Eden: not started
- [ ] **G1.BASE.2** [P] With tobagin's permission, the legacy APK boots HOME-menu mode on the phone; time NetConnect and PlayerSelect from creation to first frame. Harbor: verified on phone, 2.76 s and 2.54 s (applet-startup). Eden: not started
- [ ] **G1.BASE.3** First push to `openpak/harbor` (private) done; nothing is sent upstream. Harbor: not applicable. Eden: not started

## Gate 2: firmware 23.0.1 boots to HOME (phase 2; desktop, then phone)

### Core / am
- [ ] **G2.AM.1** [D P] A new empty data directory boots qlaunch (`0100000000001000`) from the installed 23.0.1 firmware; overlayDisp (`010000000000100C`) starts beside it, not in the foreground, and is transparent when idle (HOME looks unchanged). Harbor: verified on PC (qlaunch-probe, firmware-23); overlay booted on phone, blend not confirmed on device (HANDOVER, overlay-applet). Eden: not started
- [ ] **G2.AM.2** [D P] Every library applet in the HOME session is the firmware's own guest process (all LLE, 22.x program ids: Web/WifiWebAuth `…1043`; Shop, Lhub, Login, Share `…1042`; OfflineWeb `…100F`); no host-drawn keyboard, player select, web or shop dialog ever appears; a fallback to a host applet is logged as a gap. Harbor: verified on PC (qlaunch-probe "Every applet is the firmware's own"). Eden: not started
- [ ] **G2.AM.3** [D P] The starter gets ChangeIntoForeground/Background and never FocusStateChanged: setup passes the parental-controls and the two other qlaunch hand-offs without stalling. Harbor: verified on PC (`46ff794ef`, firmware-23); 23.0.1 on phone unverified. Eden: not started
- [ ] **G2.AM.4** [D P] qlaunch's foreground is held until the library applet's accessor is released: open and close Album, Internet Settings and Settings sub-applets five times each; no freeze, no deadlock (1 s fallback only if the accessor never closes). Harbor: verified on PC (`13d34f6bf`); phone unverified. Eden: not started
- [ ] **G2.AM.5** [D P] Partial-foreground applets are drawn over their caller and the caller stays visible (Login's inline keyboard over the web page). Harbor: verified on phone (`de9f5ed33`, account-browser-input). Eden: not started
- [ ] **G2.AM.6** [D] Pool sizes in the log return to their starting values after every applet and title exit; 20 open/close cycles show no leak. Harbor: unverified, single cycles verified on PC (qlaunch-probe; qlaunch-direction Q3 open). Eden: not started
- [ ] **G2.AM.7** [D] The Application pool is 3285 MiB and the Applet pool 507 MiB; ARMS (3242 MiB heap request) and Kirby's Dream Buffet (3250 MiB) allocate their heap with the shared framebuffer live. Harbor: verified on PC (game-launch-crashes). Eden: not started
- [ ] **G2.AM.8** [D] A second application started while one runs gets AppletLaunchFailed, not an ASSERT. Harbor: unverified (core.md). Eden: not started
- [ ] **G2.AM.9** [D P] A title behind HOME is suspended per its focus-handling mode (SuspendHomeSleep default; NoSuspend and AlwaysSuspend via ISelfController 13/16); Continue sends Resume and the title picks up where it was. Harbor: verified on PC (launch-application-plan M2; Slime Rancher). Eden: not started
- [ ] **G2.AM.10** [D] ISelfController 13 with notify false is honoured: a title that opted out never receives FocusStateChanged (Outbound name entry). Harbor: verified, owner-confirmed 2026-10-05, platform not recorded in docs (`b093e284b`). Eden: not started
- [ ] **G2.AM.11** [D] Force-terminating a title returns to HOME with the "software was closed because an error occurred" notice. Harbor: verified on PC (qlaunch-probe, F10). Eden: not started
- [ ] **G2.AM.12** [D P] A firmware reboot or shutdown request (EULA reboot, power menu) makes the host relaunch the console and the second boot skips the EULA. Harbor: verified on PC (qlaunch-probe). Eden: not started
- [ ] **G2.AM.13** [D] am 240 accepts 0 to 3 and returns am::InvalidParameters (0x3f480) for larger values, writing no state; commands 230, 350, 610/611 and 170 are answered under logged unknown contracts. Harbor: verified on PC (firmware-23). Eden: not started
- [ ] **G2.AM.14** [D] sm refuses ports outside a firmware program's NPDM access list (2021-0008) for program ids `0100000000000000` to `010000000000FFFF` only; MyPage opens without 2121-0090 and setup shows "link a Nintendo Account", "Additional users" and "To Parents and Guardians". Harbor: verified on PC (qlaunch-probe). Eden: not started
- [ ] **G2.AM.15** [D] fsp 210 (OpenDataStorageByCurrentProcess), 212 and 21 behave as Harbor's: overlayDisp mounts its RomFs, the web applet deletes its own browser save at start. Harbor: verified on PC (firmware-23, qlaunch-probe). Eden: not started
- [ ] **G2.AM.16** [D P] Each display session has its own vsync event: Album closes on B in 5 of 5 runs. Harbor: verified on PC (library-applet-plan). Eden: not started
- [ ] **G2.AM.17** [D P] F12 on desktop and the "Log threads" menu entry on Android log every guest thread's state and stack. Harbor: verified on PC for F12; Android entry unverified (`860394598`, HANDOVER). Eden: not started
- [ ] **G2.AM.18** [P] NetConnect and PlayerSelect creation-to-first-frame is no slower than Harbor's 2.76 s and 2.54 s on the phone. Harbor: verified on phone (applet-startup). Eden: not started
- [ ] **G2.AM.19** [D P] Every guessed or unimplemented command logs "Unknown contract" on each call and is on the written list; none is marked done. Harbor: verified on PC practice, list in qlaunch-probe "Open unknown contracts". Eden: not started

### HID
- [ ] **G2.HID.1** [D P] HID shared memory is per aruid: with HOME over a running title, presses and touches reach only HOME; the title sees neutral entries whose sampling numbers keep advancing. Harbor: verified on PC (`b093e284b`, HLE tests). Eden: not started
- [ ] **G2.HID.2** [D P] A button or touch already held when an aruid gains input stays hidden from it until released: in Outbound, type a name in the keyboard applet, confirm, close; the game shows no replayed press and no name loop. Harbor: verified, owner-confirmed 2026-10-05 (`b093e284b`). Eden: not started
- [ ] **G2.HID.3** [D P] A caller behind a full-screen applet or behind open quick settings gets no input; closing the full-screen keyboard or quick settings masks held buttons. Harbor: verified on PC (HLE regressions, native full-screen keyboard return; account-browser-input). Eden: not started
- [ ] **G2.HID.4** [P] An inline helper and its caller both stay interactive: on the Login page, touch and controller A press Approve while the inline keyboard is alive; starting or stopping the helper does not mask the caller's ongoing input. Harbor: verified on phone (owner, account-browser-input). Eden: not started
- [ ] **G2.HID.5** [D P] System applets read input (SystemExt style set for system aruids) and a game never sees that bit: Slime Rancher starts without the GetVibrationDeviceHandles assert; supported styles and players are per aruid. Harbor: verified on PC (library-applet-plan). Eden: not started
- [ ] **G2.HID.6** [D P] The Controllers icon opens the firmware Controller applet: console and Player 1 pair drawn, Change Grip/Order waits for L+R and registers the pair, A returns, B closes; an empty style set means the registered set (no dialog loop). Harbor: verified on PC; Find Controllers and Pairing New Controllers not walked (qlaunch-probe). Eden: not started
- [ ] **G2.HID.7** [D P] A touch on a HOME tile opens it (desktop mouse, phone finger). Harbor: unverified (qlaunch-direction Q2 "touch not yet"; touch-controller §7 step 1 pending). Eden: not started
- [ ] **G2.HID.8** [D] HID changes leave plain game boots untouched: Slime Rancher, ARMS and Kirby boot and take input. Harbor: verified on PC (game-launch-crashes; Slime Rancher in launch-application-plan). Eden: not started
- [ ] **G2.HID.9** [D] hid:sys 514 and 551 outputs are logged as unknown contracts and the Controllers applet does not abort. Harbor: verified on PC (qlaunch-probe). Eden: not started

### Display
- [ ] **G2.VI.1** [D P] qlaunch, overlay, applets and game each have their own layer; only the foreground owner shows, hidden layers drain, and the overlay is alpha-blended over everything every frame. Harbor: verified on PC, Vulkan only (overlay-applet). Eden: not started
- [ ] **G2.VI.2** [D P] The overlay blends on both Vulkan and OpenGL backends. Harbor: Vulkan only, OpenGL draws none (overlay-applet). Eden: not started
- [ ] **G2.VI.3** [D P] Three or more fbshare sessions (qlaunch, starter, the starter's applet) render into their own slots: no wrong or flickering frames during setup. Harbor: verified on PC (three sessions, first-boot-plan). Eden: not started
- [ ] **G2.VI.4** [D P] Album opens as the firmware PhotoViewer (empty-album page), B returns to HOME, HOME while it is open terminates it with LibraryAppletTerminated (0x2c80) and HOME shows. Harbor: verified on PC (library-applet-plan). Eden: not started
- [ ] **G2.VI.5** [D P] The capture button saves a picture into the album with a 96x54 thumbnail and Album lists it. Harbor: unverified; capture did nothing as of 2026-10-04 (touch-controller §5), routing added in `b36ac8efb`. Eden: not started
- [ ] **G2.VI.6** [D] A launched title's disk shader cache loads at launch, not only for boot processes. Harbor: verified on PC (qlaunch-probe, M2 step 2). Eden: not started
- [ ] **G2.VI.7** [P] The firmware language list draws the selected cell's text in its row with a filled highlight on the phone's stock driver. Harbor: bug reproduced on the stock driver; Turnip comparison pending (gpu-drivers). Eden: not started

### Audio
- [ ] **G2.AU.1** [D P] The 23.0.1 starter runs: audren accepts REV16 to REV18 through the existing layouts, refuses REV19 and returns InvalidUpdateInfo for incompatible update data; no fatal 2153-0002. Harbor: verified on PC, 90 audio tests (firmware-23). Eden: not started
- [ ] **G2.AU.2** [D P] The suspended title's audio renderer is silent while HOME is up and resumes on Continue. Harbor: verified on PC (launch-application-plan M2). Eden: not started
- [ ] **G2.AU.3** [D P] System volume and mute (audctl) apply as the output gain. Harbor: unverified (`90255efa8`). Eden: not started
- [ ] **G2.AU.4** [P] Six cold launches and five Home/reopen cycles on the phone give audible output and no AAudio abort; headphone-route changes and quality checked. Harbor: crash fix verified on phone, quality and headphone route unverified (launch-crash). Eden: not started

### Firmware setup
- [ ] **G2.SET.1** [D P] On an empty data directory setup runs language, region, EULA, Internet (wired), time zone, user creation, account link (Later), Additional users (Skip), To Parents and Guardians (Skip), "Setup is complete!", then HOME. Harbor: verified on PC on 23.0.1 (firmware-23); phone reached NetConnect, PlayerSelect and Login on 22.5.0, full 23.0.1 walk pending (applet-startup, checklist). Eden: not started
- [ ] **G2.SET.2** [D] Initial launch flags are 0 on a new data directory and 0x10001 after setup; the EULA is not pre-accepted; the host's region and language are not forced over the firmware's choice. Harbor: verified on PC (firmware-23, first-boot-plan). Eden: not started
- [ ] **G2.SET.3** [D P] A cold restart after setup goes straight to the lock screen and an interactive HOME; setup and EULA do not rerun. Harbor: verified on PC (firmware-23). Eden: not started
- [ ] **G2.SET.4** [D P] Setup choices persist across restarts: language, region, time zone, nickname, TV settings, colour set (Basic White or Black), PctlReadyFlag. Harbor: verified on PC (qlaunch-probe, applet-startup; `c60439854`). Eden: not started
- [ ] **G2.SET.5** [D P] Setup runs undocked with a handheld pad and the Joy-Con step completes through the detach action (F7 on desktop, Detach Joy-Con on the phone). Harbor: verified on PC for F7; phone button unverified (`4971a0207`). Eden: not started
- [ ] **G2.SET.6** [D P] The Internet step is the firmware NetConnect applet over one registered "Wired Connection"; HOME shows the wired icon and no airplane dialog. Harbor: verified on PC and phone (applet-startup, qlaunch-probe). Eden: not started
- [ ] **G2.SET.7** [P] Selecting a network moves to the location page without a long pause; timestamps from input to the location page recorded. Harbor: open, owner still reports lag, cause undiagnosed (applet-startup). Eden: not started
- [ ] **G2.SET.8** [D P] User creation opens the firmware PlayerSelect (UserCreatorForStarter) and the firmware keyboard; a typed nickname returns to the page and the user is created with a picture. Harbor: verified on PC (firmware-23); phone Login and keyboard follow-up pending (applet-startup). Eden: not started
- [ ] **G2.SET.9** [P] The firmware setup's first screens draw correctly on the stock Adreno driver (no clipped or misplaced rows beyond G2.VI.7). Harbor: unverified (gpu-drivers). Eden: not started

### HOME and applets
- [ ] **G2.HOME.1** [D P] HOME tiles list the games from the chosen game folders with icons (baseline JPEG) and names, DLC and updates merged; tile order persists; the tile Options menu opens. Harbor: verified on PC; phone unverified (qlaunch-probe, first-run). Eden: not started
- [ ] **G2.HOME.2** [D P] HOME to title to HOME to resume or close to another title: Start launches, HOME short press shows HOME over the title with the "Playing" badge, Continue resumes, Close Software exits and removes its layer, a second title launches with a new pid. Harbor: verified on PC with Slime Rancher, ARMS (relaunch) and Kirby; phone pending (launch-application-plan, game-launch-crashes). Eden: not started
- [ ] **G2.HOME.3** [D P] A HOME short press goes to the HOME menu, or to the foreground applet that set ICommonStateGetter 100; a watching overlay gets it first and keeps it if it set IOverlayFunctions 20; the title never receives it; a blocked press shows the "HOME cannot be used" notice. Harbor: HOME to menu verified on PC (F8); applet and overlay routing and the notice unverified (`b36ac8efb`). Eden: not started
- [ ] **G2.HOME.4** [D P] HOME while a library applet is open locks the foreground, terminates the applet with LibraryAppletTerminated and shows HOME. Harbor: verified on PC with Album (library-applet-plan). Eden: not started
- [ ] **G2.HOME.5** [D P] HOME long press opens quick settings (overlay); only the overlay gets input, the title and HOME get none; closing returns input with held buttons masked. Harbor: implemented with HLE regressions, no recorded run on PC or phone (`b36ac8efb`, account-browser-input, overlay-applet). Eden: not started
- [ ] **G2.HOME.6** [D P] Power middle press opens the power menu and Restart, Sleep Mode and Power Off shut down or relaunch the console as a console would. Harbor: unverified, IOverlayFunctions 10/11 not implemented as of 2026-10-04 (overlay-applet). Eden: not started
- [ ] **G2.HOME.7** [D P] Album, Controllers, MyPage and GameShare open and close with no freeze; MyPage ("<user>'s Page" with Profile, Friend List, Trending, Online Play Invites, Friend Suggestions, Add Friend, User Settings) is the firmware applet. Harbor: verified on PC; play activity and friend suggestions are stubs (qlaunch-probe). Eden: not started
- [ ] **G2.HOME.8** [D P] Every System Settings page renders, User Settings opens, and setting changes persist across a reboot. Harbor: verified on PC (qlaunch-probe, firmware-23). Eden: not started
- [ ] **G2.HOME.9** [D P] The profile avatar shows on HOME (baseline 256x256 JPEG; a progressive JPEG must be re-encoded, else a white circle). Harbor: verified on PC (qlaunch-probe, services.md §3.1). Eden: not started

### Accounts (console mode)
- [ ] **G2.ACC.1** [D P] A new data directory has zero users and no automatic "Eden" or "RyuPlayer" user; GetLastOpenedUser and TrySelectUserWithoutInteraction answer the invalid uid; qlaunch and the starter do not abort; no user is preselected for qlaunch. Harbor: verified on PC (first-boot-plan, firmware-23). Eden: not started
- [ ] **G2.ACC.2** [D P] acc:su 200/201/206 create, store (nickname, picture, user data) and open a nameless user; one ProfileManager serves every service. Harbor: 200/201 verified on PC; 206 unverified (services.md §3.1). Eden: not started
- [ ] **G2.ACC.3** [D P] A second local user can be added from HOME (Additional users, Add user) and selected. Harbor: unverified, not walked (qlaunch-probe). Eden: not started

### Network (wired link)
- [ ] **G2.NET.1** [D P] nifm presents one wired Ethernet link whatever the host uses, with a stored profile of stable uuid, an empty scan, Submit signalling both events and Cancel returning to Free; airplane mode is only a toggle. Harbor: verified on PC and phone for NetConnect (qlaunch-probe, applet-startup). Eden: not started
- [ ] **G2.NET.2** [D P] With no network HOME still boots and local titles run. Harbor: unverified (checklist M4 offline item). Eden: not started

### Saves and settings
- [ ] **G2.SAV.1** [D P] A title's save data is created on first launch and survives Close Software, relaunch and app restart. Harbor: creation verified on PC; persistence across relaunch unrecorded (qlaunch-probe; checklist M3 open). Eden: not started
- [ ] **G2.SAV.2** [P] Force-stopping the app during play leaves saves coherent on the next start. Harbor: unverified (checklist M3). Eden: not started

### Phone repeat
- [ ] **G2.PH.1** [P] With tobagin's permission, the whole Gate 2 sequence (setup to HOME, title, Album, Controllers, MyPage) repeats on the phone, then cold-restarts to HOME. Harbor: pending on firmware 23.0.1 (checklist). Eden: not started

## Gate 3: OpenPak integration (phase 3; desktop, then phone)

### Privacy and logging (first, before any OpenPak data)
- [ ] **G3.PRV.1** [D P] With debug logging at maximum, run setup, NA link, friends, an invitation, pctl pairing and the eShop; grep the console log and logcat: no uid, account, NA or BaaS id, token, nickname, PIN, pairing code, typed text or OAuth callback URL. Harbor: NOT MET, known offenders listed (BsdTap hex dumps on by default, t17 host trace, friends NSA ids, session ids; services.md §5). Eden: not started
- [ ] **G3.PRV.2** [D P] The upstream log sites in services.md §5 are gone: ovln 0x80 hex dump, uid in acc, friend, am, ns and olsc, typed text at INFO in swkbd, web-browser URLs, shop URL, device nickname, host SSIDs, web_service. Harbor: not applicable (Eden-specific list). Eden: not started
- [ ] **G3.PRV.3** [D] A release build with `SSLKEYLOGFILE` set writes no TLS secrets to the log. Harbor: unverified. Eden: not started
- [ ] **G3.PRV.4** [D] The settings dump and unimplemented-command reports contain no OpenPak settings and no raw command words carrying uids. Harbor: unverified. Eden: not started
- [ ] **G3.PRV.5** [D P] Tokens and the device-account secret are stored through the secret store (Keystore AES-256-GCM, non-exportable key; libsecret), never as plain JSON; no plaintext fallback; `allowBackup` off. Harbor: Keystore backend code only, never run on a device; device-account secret is plain text (checklist M4, services.md §3.1). Eden: not started
- [ ] **G3.PRV.6** [D P] Unknown or guessed contracts go through `LOG_UNKNOWN_CONTRACT` once per call and appear in the docs list. Harbor: practice only (qlaunch-probe). Eden: not started

### Boot connection
- [ ] **G3.BOOT.1** [D P] First boot with OpenPak on does the site check, fetches the network profile, keeps the console-facing CA only when its SHA-256 matches the profile, and writes `openpak/server.json`. Harbor: verified on PC; Android code only, not run on device (first-run, checklist M4). Eden: not started
- [ ] **G3.BOOT.2** [D P] A failed connect boots offline and retries on the next boot; the signed ceiling is fetched at boot and refreshed every 6 h; a 404 at boot drops those families until the refresh. Harbor: unverified (first-run, plan §6). Eden: not started
- [ ] **G3.BOOT.3** [D P] Data files use Harbor's names inside openpak-client: `openpak/server.json`, `ca.pem`, `network-profile.json`, `network-ceiling*`. Harbor: verified on PC (first-run). Eden: not started

### TLS, DNS and sockets
- [ ] **G3.NET.1** [D P] Guest TLS always verifies: a chain trusted by the host or rooted at the OpenPak CA passes; a wrong host name or an unknown CA fails; the guest's verify option and CA list are ignored. Harbor: positive case verified on PC (web applet TLS 1.3); negative cases unverified (checklist M4). Eden: not started
- [ ] **G3.NET.2** [D P] System certificate 1033 is the OpenPak CA and CaCertificateId 0xFFFFFFFE returns the trusted set within 127 entries; the web applet's handshake succeeds. Harbor: verified on PC (qlaunch-probe). Eden: not started
- [ ] **G3.NET.3** [D P] Peek, DoNotCloseSocket and ALPN (offered versus negotiated) work: Stardew (1033), Moving Out 2 (Peek) and a web applet page connect. Harbor: earlier Ryujinx port (ledger §2); not recorded on the HOME-session build. Eden: not started
- [ ] **G3.NET.4** [D P] DNS resolves in order: nsd substitution (`%`, account names to their BaaS hosts, `accounts.nintendo.com` unchanged), profile redirect, fail-closed for every ceiling family, then host DNS; a ceiling-family name without a redirect fails and never reaches real Nintendo (ban risk 2181-4008). Harbor: redirect verified on PC; fail-closed implemented, no negative test recorded (qlaunch-probe, services.md §3.2). Eden: not started
- [ ] **G3.NET.5** [D P] Firmware 23.x resolver commands 16, 100, 102, 104 answer with the layout confirmed against the dnsmitm capture; names resolve through them. Harbor: implemented from the friends marshallers, disagrees with the capture's guess, unverified (services.md §3.2). Eden: not started
- [ ] **G3.NET.6** [D P] A full session capture shows traffic only to the OpenPak edge, the OpenPak site and (on request) the driver catalogue; nothing to Nintendo, Eden or third parties. Harbor: unverified. Eden: not started
- [ ] **G3.NET.7** [D P] The connection test in Internet Settings reports a NAT type; nncs1-% and nncs2-% reach a real resolver, never the redirect. Harbor: desktop NAT check landed in v0.2.2; HOME-session NetConnect test unverified (HANDOVER desktop section). Eden: not started
- [ ] **G3.NET.8** [D P] Online titles connect and play against a second endpoint: Stardew, Outbound, Moving Out 2, Risk of Rain 2 (type-0 sockets), Terraria (parked recv). Harbor: Ryujinx fork is the behaviour reference (ledger §6); not recorded on the HOME-session build. Eden: not started
- [ ] **G3.NET.9** [D P] Several firmware processes online at once do not collide or run out of sockets (per-process fd tables); the bsd rewrite carries a provenance note per function and no source-available-derived code. Harbor: not applicable, its poll code is the tainted lineage (`3ab97fbe7`, `081d5cd88`). Eden: not started
- [ ] **G3.NET.10** [D] lp2p GetGroupInfo returns InvalidState outside a group and the ldn monitors answer. Harbor: unverified (services.md §3.2). Eden: not started

### Accounts: Nintendo Account link
- [ ] **G3.ACC.1** [D P] "Sign In and Link" in setup and HOME's add user show the firmware Login applet (program `0100000000001042`, AppletId 0x36) with OpenPak's sign-in page (password or QR) from nx-baas; the callback is accepted and the user is created with the account's nickname and picture, linked. Harbor: Approve input verified on phone; full fresh link NEVER PROVED (owner retest pending); PC route blocked at IAdministrator 290 (checklist, account-browser-input). Eden: not started
- [ ] **G3.ACC.2** [D P] A fresh registration returns through ns 2366 (copied signalled event plus IAsyncResult with Get, Cancel, GetErrorContext) with no crash, and the profile appears in the firmware. Harbor: NEVER PROVED, regression replay only; golden trace first (checklist, account-browser-input). Eden: not started
- [ ] **G3.ACC.3** [D P] After linking, the post-link qlaunch path runs: ns 4105 does linked-account work, ns 935/936/4008/4058/4084/4089/4096 answer under logged unknown contracts, no abort. Harbor: offline 4105 verified on PC; linked 4105 work not implemented; post-link path NEVER PROVED (firmware-23, plan §6). Eden: not started
- [ ] **G3.ACC.4** [D P] The linked profile persists across restart and across rerunning setup; link state is derived at each boot from the server. Harbor: verified on phone (owner, persistence only; checklist). Eden: not started
- [ ] **G3.ACC.5** [D P] IAdministrator 250 is true only for the active linked user, 251 on a linked user returns 2124-0140, 252 works, and 120, 136 (0x270 struct, birthday YYYY-MM-DD) and 143 agree with 250. Harbor: 250, 120, 136 verified on PC (connected HOME, web applet); 251/252 unit tests only (qlaunch-probe, NintendoAccountOAuthTests). Eden: not started
- [ ] **G3.ACC.6** [D] Settings, Users, Nintendo Account ("Check Using This System") opens without aborting on IAdministrator 290. Harbor: NOT IMPLEMENTED, open blocker; RE the 23.0.1 account module first (account-browser-input). Eden: not started
- [ ] **G3.ACC.7** [D P] dauth device and edge tokens reach their consumers (BaaS, nim:shp, pctl) with each consumer's client id. Harbor: verified on PC for shop tokens (qlaunch-probe). Eden: not started
- [ ] **G3.ACC.8** [D P] A linked login adopts the account's nickname and a 256x256 baseline picture; HOME shows the avatar, not a white circle. Harbor: verified on PC (services.md §3.1). Eden: not started
- [ ] **G3.ACC.9** [D P] The Login page's own input and the inline keyboard are both interactive: touch and controller A press Approve, and text typed into a page field through the inline keyboard reaches the page; keyboard appear and hide, and quick-settings and full-screen keyboard transitions, behave. Harbor: Approve verified on phone; keyboard transitions need more device coverage (account-browser-input). Eden: not started
- [ ] **G3.ACC.10** [D P] Two linked users keep separate sessions: no cross-profile tokens or ids, sign-out ends only that session. Harbor: unverified (checklist M4). Eden: not started

### Friends, presence, push, ovln
- [ ] **G3.SOC.1** [D P] MyPage's Friend List shows OpenPak friends (nickname and picture) for the linked open user. Harbor: verified on PC (qlaunch-probe signed-in run). Eden: not started
- [ ] **G3.SOC.2** [D P] Add Friend by code, request, accept, remove and block work and the lists update on both endpoints; a favourite set to false stays false after a reboot. Harbor: unverified in a HOME session (desktop fork v0.2.x; acceptance J1, J2, J6). Eden: not started
- [ ] **G3.SOC.3** [D P] Presence publishes on title start, exit and change (INACTIVE, ONLINE, PLAYING); a friend sees PLAYING with the title; HOME never counts as a played title. Harbor: HOME exclusion verified on PC; publish end to end unverified (qlaunch-probe, acceptance J3). Eden: not started
- [ ] **G3.SOC.4** [D P] The Penne push connection opens at sign-in and reconnects with backoff after loss; on Android background behaviour is documented (no foreground service). Harbor: verified on PC ("Push connection open"); phone unverified (qlaunch-probe, services.md §3.3). Eden: not started
- [ ] **G3.SOC.5** [D P] ovln works as psc's agent: named sources, per-sender queues, one receiver per source, global FIFO, empty receive returns 2025-0012 and overlayDisp does not abort; qlaunch's tag 23 reply path works. Harbor: verified on PC (OvlnAgentTests, invite chain). Eden: not started
- [ ] **G3.SOC.6** [D P] "<friend> is now online" shows top right when a friend offline for 30 minutes or more signs in, gated by the account notification settings and requiring friend:v 10102 and the picture from 10110. Harbor: NEVER PROVED visibly; chain reaches overlayDisp (overlay-applet). Eden: not started
- [ ] **G3.SOC.7** [D P] An invitation sent while HOME is up shows a pop-up for 8 s with sender name and picture; HOME while it is up opens MyPage on that invitation. Harbor: NEVER PROVED visibly; simulated push verified on PC up to the sender lookup (overlay-applet). Eden: not started
- [ ] **G3.SOC.8** [D P] Join on the invitation launches the title (CreateApplication, 121, 180, 130, Start, 101) and the game pops it via IApplicationFunctions 141 ("Friend invitation handed to the application"). Harbor: NEVER PROVED; golden trace first (overlay-applet, qlaunch-probe). Eden: not started
- [ ] **G3.SOC.9** [D P] With the title already running (Outbound), the pop-up shows over the game; HOME then Join pushes the invitation through IApplicationAccessor 180 and the game pops it. Harbor: NEVER PROVED (overlay-applet). Eden: not started
- [ ] **G3.SOC.10** [D P] The pop-up is actionable only when HOME's writer lock is free and no blocking scene exists; otherwise it shows without the HOME shortcut and the invitation stays reachable from the user page. Harbor: unverified, lock accessors may not satisfy qlaunch's check (overlay-applet). Eden: not started
- [ ] **G3.SOC.11** [D P] qlaunch's resume calls friends 20900 and the once-per-user summary (ovln 7001) appears. Harbor: unverified, trigger inferred from the command name (overlay-applet). Eden: not started
- [ ] **G3.SOC.12** [D P] npns:s 161/162/201/202 and npns:u 5/8 answer so qlaunch's Friends "Update" and overlayDisp do not abort. Harbor: verified on PC (qlaunch-probe). Eden: not started
- [ ] **G3.SOC.13** [D P] A friend-request pop-up (ovln 7003) shows on a new request. Harbor: not implemented (services.md §3.3). Eden: not started
- [ ] **G3.SOC.14** [D P] The emulator-closable rows of the console acceptance checklist pass (relationship parser and 300 bound, preferences, Penne framing, event carrier, ack, SubscribeTopic, REST shapes, invitation read state and expiry, presence); the console-only rows (sleep/wake, silent RF loss, store install and launch, latency) are not ticked from emulator evidence. Harbor: unverified, all gates pending (acceptance §11). Eden: not started

### News and BCAT
- [ ] **G3.NEWS.1** [D P] The lock screen shows Featured News tiles and the News icon opens Featured and Latest with OpenPak titles, text, images, channel names and dates; an article opens. Harbor: verified on PC, 33 articles (qlaunch-probe, news server v0.2.1). Eden: not started
- [ ] **G3.NEWS.2** [D P] Find Channels lists OpenPak channels, Follow Channel works, a channel page lists articles and one opens; INewsService 30500 fills the caller's transfer memory and 30201 answers true. Harbor: verified on PC (qlaunch-probe). Eden: not started
- [ ] **G3.NEWS.3** [D P] News comes only from the OpenPak server: no built-in or fabricated news, no contact with Eden, Forgejo or GitHub. Harbor: verified by construction; no capture recorded. Eden: not started
- [ ] **G3.NEWS.4** [D P] The news store persists; an article whose size changes in the index is replaced; qlaunch's WHERE and ORDER BY strings (N_IF, N_SWITCH, N_RAND, N_PMATCH; unknown token gives 2125-0203) evaluate; GetTopicList (30101) after closing News does not abort. Harbor: verified on PC (qlaunch-probe, services.md §3.4). Eden: not started
- [ ] **G3.NEWS.5** [D P] News works on firmware 23.0.1 and the new 23.x command 50100 is logged as an unknown contract. Harbor: unverified on 23.0.1 in Harbor (services.md §3.4). Eden: not started
- [ ] **G3.NEWS.6** [D P] Launching a BCAT title fetches its delivery cache into its BCAT save before it runs; 20301 returns the suspension object; the system application's cache (`0100000000001056`) is created on first use. Harbor: unverified in a HOME session (desktop fork a909bfd3b; qlaunch-probe). Eden: not started

### Parental controls
- [ ] **G3.PCTL.1** [D P] Pairing from setup ("use the smartphone app") and HOME settings: the console shows a code, a test parent account enters it, the console shows paired with owners and owner picture. Harbor: NEVER PROVED, "Untested on Harbor"; golden trace first (qlaunch-probe, PctlPairingTests only). Eden: not started
- [ ] **G3.PCTL.2** [D P] Unpairing (owners DELETE, parent-side unlink) clears the pairing and the overlay shows the unlink notice. Harbor: NEVER PROVED (services.md §3.4). Eden: not started
- [ ] **G3.PCTL.3** [D P] The parent's setting syncs after pairing, 5 s after a change push and every 10 minutes: PIN, rating, free communication and play-timer block are applied and the sync event fires. Harbor: NEVER PROVED; apply logic unit-tested (qlaunch-probe, PctlPairingTests). Eden: not started
- [ ] **G3.PCTL.4** [D P] A restricted action asks for the firmware PIN (no host PIN applet); a wrong PIN is rejected (2142-0120), none set 0132, bad format 0131; Eden's accept-any-PIN host applet is disabled. Harbor: NEVER PROVED (qlaunch-probe). Eden: not started
- [ ] **G3.PCTL.5** [D P] A title above the allowed age, or unrated, is refused at launch (2142-0103, NACP RatingAge by organisation, IARC fallback). Harbor: NEVER PROVED (qlaunch-probe). Eden: not started
- [ ] **G3.PCTL.6** [D P] 2142-0234 is treated as a JSON parse abort, not a restriction verdict: a malformed server answer does not lock the console. Harbor: NEVER PROVED (services.md §3.4). Eden: not started
- [ ] **G3.PCTL.7** [D P] HOME's parental-controls refresh on the sync event reads 1031, 1006, 145601, 1455, 1458, 1454, 1459 without aborting; 1454 is an s64 and 145601 is registered once. Harbor: implemented, unverified (qlaunch-probe, services.md §3.4). Eden: not started
- [ ] **G3.PCTL.8** [D P] The overlay shows the changed-settings notice (4000) when the parent changes a setting. Harbor: unverified (`b36ac8efb`). Eden: not started
- [ ] **G3.PCTL.9** [D P] Play-timer commands 1451 to 1460 answer as Harbor's (in memory, no enforcement); enforcement is not part of parity. Harbor: in memory, not enforced (services.md §3.4). Eden: not started

### eShop, Online and web
- [ ] **G3.SHOP.1** [D P] The eShop icon starts the firmware web program (`0100000000001042`) and renders the OpenPak Shop; closing returns to HOME with no freeze, five times in a row. Harbor: verified on PC, freeze fixed (qlaunch-probe, `13d34f6bf`); phone unverified. Eden: not started
- [ ] **G3.SHOP.2** [D P] The Online icon renders the OpenPak Online page. Harbor: verified on PC (qlaunch-probe). Eden: not started
- [ ] **G3.SHOP.3** [D] systemWeb runs on Eden with the corrected program ids and LLE; one desktop run before committing, then measured on the phone. Harbor: verified on PC; nobody has run it on Eden (plan §6 risk 4). Eden: not started
- [ ] **G3.SHOP.4** [D P] nim:shp 1 and 3 return dauth device and edge tokens through IAsyncValue; ns:ec 3 completes with "no OpenPak rights", 8 and 10 prefetch, 9 and 11 hand over the BaaS id_token; ns:vm 3100 answers state 3 and 3101 completes. Harbor: verified on PC (qlaunch-probe, services.md §3.5). Eden: not started
- [ ] **G3.SHOP.5** [D P] Process winding (Lhub and Shop push context, reserve, unwind) returns to the same page. Harbor: NEVER PROVED beyond PushContext (qlaunch-probe). Eden: not started
- [ ] **G3.SHOP.6** [D P] Virtual Game Cards opens its page (ns 4042). Harbor: not implemented, stops the session (qlaunch-probe unknown contracts). Eden: not started
- [ ] **G3.SHOP.7** [D P] Purchase, download and install from the shop are not required for parity; the shop browses and closes cleanly (store install is console-only). Harbor: page-driven commands not implemented (qlaunch-probe; acceptance J8). Eden: not started
- [ ] **G3.SHOP.8** [D P] The ns unknown-contract set (1, 45, 49, 300, 518, 935, 1508, 2366, 2367, 4008, 4058, 4084, 4089, 4096; 607; 701) answers with Harbor's firmware-traced shapes (4022 and 4088 events never signalled, 4023 `(bool, 0x50 record)`, 936 `(s32, 0x80 record)`) and qlaunch does not spin. Harbor: verified on PC for those hit (services.md §3.5). Eden: not started
- [ ] **G3.SHOP.9** [D P] The offline web program (`…100F`) opens an offline page. Harbor: id mapped, unverified (qlaunch-probe). Eden: not started

### Saves and reports
- [ ] **G3.SAV.1** [D P] Save Data Cloud uploads and downloads a title's save through the firmware's own olsc path (the console protocol). Harbor: NEVER PROVED, olsc answered only; SaveArchive was never on the HOME launch path (plan §5.10). Eden: not started
- [ ] **G3.SAV.2** [D P] olsc 209 and the 23.x command 1001 answer and qlaunch's olsc sweep does not abort. Harbor: verified on PC for the sweep (services.md §3.5). Eden: not started
- [ ] **G3.SAV.3** [D P] erpt accepts and discards the firmware's reports (Error applet report creation) and writes no files. Harbor: verified on PC (`3a4bf842a`). Eden: not started
- [ ] **G3.SAV.4** [D P] A fatal produces a consent-gated OpenPak crash report with no host dialog; with consent off nothing is sent. Harbor: desktop fork only; HOME path unverified (plan §5.11). Eden: not started
- [ ] **G3.SAV.5** [D P] The "Captured" overlay appears after a capture (ovln 8001 to 8004). Harbor: implemented, unverified (`b36ac8efb`). Eden: not started
- [ ] **G3.SAV.6** [D P] prepo accepts 20102/20103 and no play reports carrying user ids are written. Harbor: verified on PC for acceptance (services.md §3.5). Eden: not started

### Phone repeat
- [ ] **G3.PH.1** [P M] With tobagin's permission, every Gate 3 item that is not desktop-only repeats on the phone. Harbor: phone mostly unverified; see items. Eden: not started

## Gate 4: host UX (phase 4)

### Wizard
- [ ] **G4.WIZ.1** [D P M] Startup gate: an incomplete setup or missing keys, firmware or qlaunch (`0100000000001000`, not MiiEdit) opens the wizard; otherwise the app goes straight to HOME with no game list. Harbor: verified on PC in flow tests; phone gate unverified (first-run). Eden: not started
- [ ] **G4.WIZ.2** [D P M] Pages in order: Welcome, Device check, Keys, Firmware, Games, DLC (skippable), Updates (skippable), Graphics driver (Adreno only), Ready with "Start Harbor". Harbor: verified on PC off-screen; Android walk pending (first-run, applet-startup). Eden: not started
- [ ] **G4.WIZ.3** [D P M] Device check rows: processor (arm64 or fail), memory (under 4 GB fails, 4 or 6 GB warns), Vulkan (none or under 1.1 fails), driver (non-Adreno warns), system (under API 30 warns), storage (under 4 GB warns); a failed row disables Next, Back works. Harbor: logic verified on PC; Android values unread on a device in Harbor (first-run). Eden: not started
- [ ] **G4.WIZ.4** [D P M] Keys: picking `prod.keys` stages and verifies it (header_key) before install; a bad file never replaces a good one. Harbor: verified in tests with fake pickers; real SAF picker unverified (first-run). Eden: not started
- [ ] **G4.WIZ.5** [D P M] Firmware: a zip, xci or NCA folder is verified, the version shown, anything but 23.0.1 warns and may still install, install shows progress; a copied import file is deleted after install. Harbor: verified in tests on PC; real picker and xci on phone unverified (first-run). Eden: not started
- [ ] **G4.WIZ.6** [P M] Games, DLC and updates folders are picked as SAF tree URIs with no all-files permission, including removable storage; the games appear as HOME tiles and DLC and updates apply. Harbor: not applicable, Harbor uses all-files access (first-run; plan §3). Eden: not started
- [ ] **G4.WIZ.7** [D] On desktop the folder pages take paths and the same HOME library appears. Harbor: verified on PC with the owner's folders (checklist M1.5). Eden: not started
- [ ] **G4.WIZ.8** [P M] After DLC and updates picks, B Back and A Next stay fully on screen after every picker return, system bars included. Harbor: layout fixed, passes PC tests, phone unverified (first-run). Eden: not started
- [ ] **G4.WIZ.9** [D P M] Nothing is written to the record until Next succeeds on its page; resume opens at the first incomplete page judged from the files; "Set up again" reopens on the saved record. Harbor: verified in flow tests on PC (first-run). Eden: not started
- [ ] **G4.WIZ.10** [P M] Look: 1280x720 canvas scaled uniformly (widgets no larger than the firmware's), neutral Basic White or charcoal Basic Black, blue values, mint focus ring only after a key or pad press, one page per step with progress dots, fixed primary action, A/B/X footer as touch targets. Harbor: renders verified on PC; light palette verified on phone; firmware-scale fix pending on phone (wizard-presentation, applet-startup). Eden: not started
- [ ] **G4.WIZ.11** [P M] Input: touch (a real-length press), D-pad, arrows, A/B/X by position, Plus confirm, Minus back, held direction repeat; a physical controller drives every page. Harbor: PC verified; phone controller pending (controller-support); Borealis touch and D-pad verified on phone, gamepad untested (borealis.md). Eden: not started
- [ ] **G4.WIZ.12** [P M] The horn cue is 180 ms, one semitone higher per visible page, lower on Back, no pitch gap when the driver page is hidden, silent on a failed pick, stopped on pause, audio focus released before firmware audio, and a sound failure never blocks setup. Harbor: PC verified (PCM, SDL3 dummy); phone pending (wizard-presentation). Eden: not started
- [ ] **G4.WIZ.13** [P M] The activity is landscape-only and both system bars stay hidden through setup, picker returns and the hand-off to firmware setup. Harbor: pending on phone (wizard-presentation). Eden: not started
- [ ] **G4.WIZ.14** [D] The desktop wizard runs before the Qt application and hands over; `--setup` reruns it. Harbor: unverified, launcher never started (first-run "Not run"). Eden: not started
- [ ] **G4.WIZ.15** [P M] Borealis inside Eden's APK: after the wizard finishes, SDL3 audio still works in the console (option A), else option B is built. Harbor: not applicable (plan §3). Eden: not started
- [ ] **G4.WIZ.16** [D P M] No Nintendo system fonts or Pokemon art ship; fonts are Noto Sans and Material Icons; the CJK fallback load is gone. Harbor: not applicable (own assets, wizard-presentation). Eden: not started
- [ ] **G4.WIZ.17** [P] A fresh install reaches HOME using only touch, and again using only a controller. Harbor: not done (Gate 4 definition). Eden: not started
- [ ] **G4.WIZ.18** [D] A fresh desktop install reaches HOME using only keyboard, mouse and pad. Harbor: not done (Gate 4 definition). Eden: not started

### Console mode and Harbor menu
- [ ] **G4.CON.1** [P M] After readiness the app boots qlaunch; Back opens the Harbor menu and never reaches a game list; Exit closes the app. Harbor: unverified on phone (first-run, core-integration §6). Eden: not started
- [ ] **G4.CON.2** [D] Qt console mode scans the library hidden, applies the console preset (every applet LLE, handheld, overlay hides with a pad) and launches qlaunch; menu and status bars are hidden. Harbor: unverified, desktop launcher never started (first-run). Eden: not started
- [ ] **G4.CON.3** [D P M] The Harbor menu has Resume, Detach or Attach Joy-Con, Restart, Graphics driver, Show or Hide FPS, Log threads and Set up again; the stopped screen offers Start again and Set up again. Harbor: implemented, unverified on phone (first-run, host.md). Eden: not started
- [ ] **G4.CON.4** [P M] Restart re-selects qlaunch (not the last game) and relaunches the console cleanly. Harbor: unverified (core-integration §6). Eden: not started
- [ ] **G4.CON.5** [D P M] The host theme follows the console's colour set, also while stopped, and before boot follows the phone. Harbor: light verified on phone; Basic Black switching and restart persistence pending (applet-startup). Eden: not started
- [ ] **G4.CON.6** [D P M] No host substitutes exist in console mode: no Android keyboard or web applet, no PIN applet, no built-in news, no updater, no web service or rooms, no Discord presence, no lobby, no game list. Harbor: by design (plan §5.8). Eden: not started

### Controllers and touch
- [ ] **G4.PAD.1** [P M] The touch overlay has A, B, X, Y, L, R, ZL, ZR, +, -, HOME, capture, both stick presses, d-pad and both sticks; the stick recentres where touched; an 8-way d-pad slides between directions; sliding from one button to another moves the press; a finger on no control is the console touch screen. Harbor: PC tests; on phone only show, fade and Approve touch verified (touch-controller §6, applet-startup). Eden: not started
- [ ] **G4.PAD.2** [P M] Edit mode drags controls, resizes one or all, changes opacity 20 to 100 %, resets and saves per orientation; the show/hide toggle works; haptics follow the system touch-feedback setting. Harbor: PC tests; device unverified (touch-controller §7). Eden: not started
- [ ] **G4.PAD.3** [P M] The pad fades to 15 % over 400 ms after three seconds idle or holding buttons, restores on touch (the touch still acts), and stays visible while editing. Harbor: verified on phone (applet-startup). Eden: not started
- [ ] **G4.PAD.4** [P M] The pad hides as soon as any controller is connected (also idle, in edit mode, present before boot); touching the screen keeps it hidden and feeds guest touch; the last disconnect restores it; keyboards and virtual devices do not count. Harbor: PC tests and bridge; phone saw only Kunai discovery and reconnect (controller-support, account-browser-input). Eden: not started
- [ ] **G4.PAD.5** [P M] Face buttons map by position (right A, bottom B, top X, left Y) on the Kunai, Pro Controller, Joy-Con and Xbox-style pads, and the first pad binds to player 1 with no setup step. Harbor: unit tests; hardware pending, hid-nintendo versus hid-generic order unknown (controller-support). Eden: not started
- [ ] **G4.PAD.6** [P M] Sticks, triggers (hysteresis 0.5 and 0.4, dead zones), stick clicks, shoulders, Plus, Minus, HOME and capture reach the guest; split Joy-Con halves and several devices act as one handheld pad and removing one keeps the others. Harbor: unit tests and 23 bridge assertions; hardware pending (controller-support). Eden: not started
- [ ] **G4.PAD.7** [P M] Host panels navigate with D-pad, hat and left stick (immediate, repeat after 350 ms then every 110 ms), A or Plus confirms, B or Minus goes back, and no UI press arrives in the guest as a stuck button afterwards. Harbor: PC tests; hardware pending (controller-support). Eden: not started
- [ ] **G4.PAD.8** [P M] Plus+Minus opens the Harbor menu from the console without leaking the chord to the guest, including the Joy-Con detach action. Harbor: PC bridge; hardware pending (controller-support). Eden: not started
- [ ] **G4.PAD.9** [P M] Disconnecting while holding releases the input and shows the pad; reconnecting idle hides it at once; with two devices disconnecting one keeps the pad hidden; backgrounding while holding and reopening leaves nothing stuck. Harbor: pending on phone (controller-support). Eden: not started
- [ ] **G4.PAD.10** [P M] The capture button on pad and overlay takes a capture. Harbor: unverified, did nothing as of 2026-10-04 (touch-controller §5). Eden: not started
- [ ] **G4.PAD.11** [D] Mouse acts as the console touch screen with an explicit mapping and focus loss releases held input. Harbor: unverified, mapping not documented (README requires it). Eden: not started
- [ ] **G4.PAD.12** [D] Desktop keys: F8 HOME (short and long), F7 Joy-Con, F6 dock, F4 capture, F5 power hold, F10 force-terminate, F11 fullscreen, F12 thread dump; Eden's clashing defaults are remapped. Harbor: verified on PC (`WindowBase.cs`, qlaunch-probe). Eden: not started
- [ ] **G4.PAD.13** [D] Keyboard and desktop pads drive firmware setup and HOME. Harbor: verified on PC (qlaunch-probe). Eden: not started

### GPU drivers
- [ ] **G4.GPU.1** [P M] The Graphics driver page, menu entry and saved choice exist only when the GPU is Adreno; elsewhere no page, no entry, a saved choice ignored. Harbor: PC tests; Android read unverified (gpu-drivers). Eden: not started
- [ ] **G4.GPU.2** [P M] Options are the phone's driver (recommended), installed drivers, "Install a driver file" (.zip) and Skip; installing selects the driver; the choice persists. Harbor: verified on PC tests (gpu-drivers). Eden: not started
- [ ] **G4.GPU.3** [P M] "Browse GitHub drivers" lists MrPurple666, whitebelyash, K11MCH1 and StevenMXZ releases with title, publisher, channel, date, filename and size, with no request before it is opened. Harbor: verified on PC live (gpu-drivers). Eden: not started
- [ ] **G4.GPU.4** [P M] The list filters by arm64-v8a, Adreno and API 28 or higher plus family rules (Gen8 only on 8xx, Qualcomm needs an explicit family claim, 710/720/722 only with explicit notes, K11MCH1 R8 excluded); on API 31 the live catalogue gave 16 choices without Qualcomm v863.1, on API 34 17 with it; each package's minApi is checked before extraction and again at load. Harbor: verified on PC (gpu-drivers); Android unverified. Eden: not started
- [ ] **G4.GPU.5** [P M] Downloads are HTTPS from the four repositories, 32 MiB bounded, exact length, SHA-256 verified when GitHub gives a digest, with 60 s and 3 min deadlines; X cancels by touch, key or pad; navigation is locked while busy; a failure keeps the previous selection. Harbor: verified on PC (gpu-drivers). Eden: not started
- [ ] **G4.GPU.6** [P M] A chosen driver loads (log shows the custom driver and a Turnip device); a failed load logs "could not be loaded" and the session runs on the stock driver. Harbor: unverified on phone (gpu-drivers). Eden: not started
- [ ] **G4.GPU.7** [P M] A driver change takes effect after a process restart. Harbor: same limit (gpu-drivers). Eden: not started
- [ ] **G4.GPU.8** [P] The firmware language list renders correctly on Turnip and on the stock driver, settling whether the stock driver is the cause. Harbor: pending, bug reproduced on stock only (gpu-drivers). Eden: not started
- [ ] **G4.GPU.9** [P] ARMS and Kirby's Dream Buffet run on the phone with the latest MrPurple Turnip, including gameplay. Harbor: PC verified; phone pending (game-launch-crashes, checklist). Eden: not started

### Identity and packaging
- [ ] **G4.ID.1** [D P M] The name Harbor, lighthouse icon, adaptive and monochrome launcher icons and strings show everywhere; no Eden branding and no Nintendo mark in user-visible text. Harbor: partly done (identity, wizard-presentation). Eden: not started
- [ ] **G4.ID.2** [D] The desktop binary is `harbor` with data under an XDG `harbor` directory and a desktop entry and icons. Harbor: not done, data directory still shared with Ryujinx (identity). Eden: not started
- [ ] **G4.ID.3** [D P M] Tokens survive a restart through the secret store; an invalidated Keystore key is handled; sign-out removes them; no plaintext fallback. Harbor: code only, not run on a device (checklist M4). Eden: not started
- [ ] **G4.ID.4** [D P M] Licence notices are present and shown: AGPL-3.0-only for new files, GPL notices kept, MIT notices (`NOTICE.md`), Borealis Apache-2.0, nanovg and SDL zlib, fonts OFL. Harbor: verified in repository (identity). Eden: not started
- [ ] **G4.ID.5** [P M] Both flavours install and run: legacy on API 31, mainline on API 33+, with the same features. Harbor: single flavour (plan §5.2). Eden: not started

## Gate 5: parity and cut-over (phase 5)

### Device matrix and games
- [ ] **G5.DEV.1** [D P M] Every item in Gates 2 to 4 is run on desktop Linux, the ROG Phone 3 (legacy) and the API 33+ device (device TBD), with results recorded per device. Harbor: phone coverage partial (checklist M5). Eden: not started
- [ ] **G5.DEV.2** [P] Phone specifics hold: stock Adreno 650 driver, Turnip choice, 4 KiB pages, ASUS Key Mapping off for the Kunai, PIN-locked screen not bypassed. Harbor: partly verified (gpu-drivers, controller-support). Eden: not started
- [ ] **G5.GAME.1** [D P M] ARMS, Kirby's Dream Buffet and Slime Rancher launch, render, take input, return to HOME, resume, close and relaunch. Harbor: verified on PC; phone pending (game-launch-crashes). Eden: not started
- [ ] **G5.GAME.2** [D P M] Outbound name entry shows no loop and its online play reaches a session. Harbor: name entry owner-confirmed 2026-10-05; online unverified on this build (`b093e284b`). Eden: not started

### Performance and lifecycle
- [ ] **G5.PERF.1** [P M] A sustained gameplay run records frame times, memory pressure and thermal behaviour, no slower than the Ryujinx Harbor on the same title. Harbor: not measured; tobagin judged Eden much faster on the ROG Phone 3 (plan §1). Eden: not started
- [ ] **G5.LIFE.1** [P M] Background and foreground with held input, surface recreation, memory pressure, process death and driver failure leave a running console recoverable and saves coherent. Harbor: Home and reopen verified on the diagnostic host only; the rest unverified (vulkan-probe, checklist M3). Eden: not started
- [ ] **G5.LIFE.2** [P M] Twenty cold launches give no SIGSEGV at signal-handler setup and no audio-callback abort. Harbor: one intermittent SIGSEGV seen, race fixed, cause unconfirmed, final cold boot unverified (account-browser-input, launch-crash). Eden: not started
- [ ] **G5.LIFE.3** [D P M] Stop and restart of the console in one process works without restarting the app. Harbor: unverified (checklist M2). Eden: not started

### Command coverage
- [ ] **G5.CMD.1** [D] Every one of Harbor's 687 added IPC command ids has an Eden handler or a recorded decision (237 were missing at plan time). Harbor: not applicable. Eden: not started
- [ ] **G5.CMD.2** [D] Every entry on Harbor's open unknown-contract list is answered as Harbor does or resolved by RE, and none is marked done on a guess. Harbor: list in qlaunch-probe. Eden: not started

### Release and cut-over
- [ ] **G5.REL.1** [D P M] A `v*.*.*` tag builds desktop and both Android flavours in CI with pinned dependencies and recorded checksums. Harbor: not done (checklist M5). Eden: not started
- [ ] **G5.REL.2** [D P M] After each planned upstream rebase the Gate 2 sequence is rerun before anything else. Harbor: not applicable. Eden: not started
- [ ] **G5.REL.3** [P M] At cut-over the id becomes `org.openpak.harbor`, signed with a new release key kept outside the repository (the current debug-signed APK cannot be updated). Harbor: debug key today (plan §5.3). Eden: not started
- [ ] **G5.REL.4** [P] Before the Ryujinx Harbor is touched, saves are exported or both apps stay installed; nothing is uninstalled without tobagin's say-so. Harbor: not applicable (plan §5.4). Eden: not started
- [ ] **G5.REL.5** [D] openpak-client is published or vendored before any release. Harbor: not applicable (plan §5.12). Eden: not started
- [ ] **G5.REL.6** [D P M] The console acceptance run (J1 to J7) repeats with the Eden Harbor as the second endpoint. Harbor: pending (acceptance). Eden: not started
- [ ] **G5.REL.7** [D P M] tobagin validates the checklist item by item and says so before the Ryujinx Harbor is retired. Harbor: not applicable. Eden: not started

## Not carried over on purpose

- Host substitutes: Android keyboard and web host applets, the host PIN applet, built-in news, Eden updater, web service and rooms, Discord presence, multiplayer lobby, game list, `REQUEST_INSTALL_PACKAGES`, the all-files permission (SAF tree URIs instead), the `genshinSpoof` and `chromeOS` flavours, the Lossless DLL page.
- No data migration from the Ryujinx Harbor: layouts differ (`bis/` versus `nand/`); the owner reruns firmware setup; the linked profile and saves are lost unless exported first.
- The old OpenPak Eden fork's host UI (OpenPak windows, toasts, replacement MyPage, invitation dialogs), its identity model tied to the host's selected profile, and its `save_sync` host cloud save; Harbor's `SaveArchive`.
- Upstream Eden's TLS verify-off, wide DNS blocklist and tracing hooks.
- The an earlier Ryujinx fork poll code (source-available lineage): sockets are rewritten clean-room.
- Game patches: only the existing Crash Team Racing key-swap IPS; nothing else.
- Things Harbor never had, so parity does not require them: play-timer enforcement, other overlay senders (downloads, volume, input devices), presence-change notifications to friends listeners, real sleep, rumble, gyro, controller remapping, separate multiplayer pads, purchase and download in the shop, iOS.
- Harbor's own privacy offenders (services.md §5) are fixed in Eden, not ported.
