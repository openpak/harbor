# Harbor core on Eden: phase 0 map (am / kernel / hid / vi / ns-launch / audio)

Date: 2026-10-06. Analysis only; nothing was built or run.

- Harbor-Ryujinx: `emulators/ryujinx-android`, branch `feature/android-port`, HEAD `ee683886b`. The
  diff is measured from the branch's starting revision `c93a0e2b06e` (from `next-session.md`). From
  there to HEAD, 238 core files change, +23,560 lines (`git diff --stat c93a0e2b06e feature/android-port`
  over `src/Ryujinx.HLE`, `Ryujinx.Horizon`, `Ryujinx.Audio`, `Graphics.*`, `Memory`, `Cpu`).
- Eden: `emulators/harbor-eden`, branch `harbor`, `10bcd2d84`.
- Paths below are relative to the repository root unless they are absolute. Ryujinx `Am/…` means
  `src/Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/…`. Eden `am/…` means
  `src/core/hle/service/am/…`.
- **(guess)** marks an inference from reading code that no run has confirmed.
- Ledger tooling and output: `scratch/harbor-eden/research/ledger/{ledger.py,harbor-added.tsv,ledger.tsv}`.

---

## 1. How Eden runs qlaunch and applets today

**Boot.** The Qt action `action_Launch_QLaunch` (`src/yuzu/main_window.cpp:1579`, also `-qlaunch` and
desktop shortcuts in `qt_common/util/game.cpp:514`) and the Android `GamesFragment.launchQLaunch()`
(`src/android/.../ui/GamesFragment.kt:485`, behind the `ENABLE_QLAUNCH_BUTTON` setting) both load
qlaunch 0100000000001000 from the installed system NAND (BIS) contents. They call
`LaunchFirmwareApplet`, then `AppletManager::CreateAndInsertByFrontendAppletParameters`, then
`AppletManager::SetWindowSystem` (`am/applet_manager.cpp`).

`SetWindowSystem`:

- builds the qlaunch `Applet`;
- pushes a fake caller's CommonArguments (`PushInShowQlaunch`);
- pushes a preselected-user launch parameter, with `ASSERT` that a valid user exists;
- calls `TrackApplet(home menu)` and `RequestHomeMenuToGetForeground`, then `process->Run()`.

overlayDisp 010000000000100C is created and run first, **before** qlaunch, but only when
`Settings::values.enable_overlay` is set. That setting defaults to false (`common/settings.h:171`).

**Everything is LLE multi-process.** Each applet is a real guest `KProcess`, wrapped in
`Service::Process` (`hle/service/os/process.cpp`). Each process has its own `Core::Memory` page table
and its own CPU interfaces (`hle/kernel/k_process.cpp:1306-1327`). NCE (Android) and fastmem are used
only for the process in the Application pool (`k_process.cpp:1310`, `core/memory.cpp:50-57`); qlaunch,
overlay, starter and library applets run on dynarmic with page-table access. Services are HLE host
threads, one `ServerManager` per module.

**am is the window-system design inherited from yuzu.** It has these parts:

- `WindowSystem` (`am/window_system.cpp`);
- `Applet` (`am/applet.h`), which holds:
  - its lifecycle, `hid_registration` and `display_layer_manager`;
  - its caller/child tree;
  - winding state (`is_winding`, `reserved_applet`);
- `LifecycleManager` (`am/lifecycle_manager.cpp`), whose messages come from state: requested versus
  acknowledged focus, plus flags. They are coalesced, not queued;
- `EventObserver` (`am/event_observer.cpp`): a jthread that MultiWaits on every applet process and
  calls `WindowSystem::Update()`.

On each wake-up, `Update()` prunes terminated applets. It pushes `ApplicationExited` to the HOME menu
and moves the foreground back to it. It then honours `LockForeground`, which first terminates the HOME
menu's children with `ResultLibraryAppletTerminated`. Finally, `UpdateAppletStateLocked` recomputes, for
the overlay, HOME and application roots recursively:

- activity state (ForegroundVisible/Obscured, Background…);
- suspension (`Process::Suspend` → `SetActivity`);
- visibility;
- HID enable (`HidRegistration::EnableAppletToGetInput` → `hid_core` `EnablePadInput`/`EnableTouchScreen`
  per aruid);
- layer z-index (`AppletZIndex`: Background 0, Foreground 1, ForegroundVisible 2, Overlay 3).

Library applets go through `ILibraryAppletCreator`. It creates a guest process when that applet's
`*_applet_mode` is LLE, and otherwise falls back to a frontend (HLE) applet
(`am/service/library_applet_creator.cpp:15-40,105-160`). The LLE defaults are cabinet, error,
net_connect, player_select, mii_edit, photo_viewer, offline_web and my_page. The HLE defaults are
controller, swkbd, web, shop, login_share, wifi_web_auth and data_erase.

| Area | What Eden already does |
|---|---|
| Applications | `IApplicationCreator` 0 `CreateApplication` and 10 `CreateSystemApplication` (the starter, `AppletId::Starter`) create not-started processes (`am/service/application_creator.cpp`); `IApplicationAccessor` starts, exits and foregrounds them |
| Library applets | the data broker exists |
| Winding | `IProcessWindingController` plus `WindowSystem::RestartAppletProcessLocked` (`0c2894eab` "(re)winding application") |
| Display | system buffer sharing and capture slots (`vi/shared_buffer_manager.cpp`, from the **Secure** pool); a z-sorted, blended multi-layer compositor (`nvnflinger/hardware_composer.cpp`) |
| HID | per-aruid shared memory (`hid_core/resources/applet_resource.cpp`, `AruidIndexMax` 0x20) |
| Vsync | one vsync event per display session |
| Firmware 23 surface | kernel TLS+0x110 thread handle; GetInfo 37/38 returning 0; ZBIC NSO decompression for 22.0+ (`815325cce`); firmware constants 23.0.0 (`e7a96c790`); ns 4105, 23 and 4042, am 240/230/350/610/611/170 as stubs |

Eden's own history shows active qlaunch work: `5d150cac5` "Fixes the latest qlaunch regressions on
Android and adds cmd23 for launching games on FW 23", `bfba95fd6` "stubs for qlaunch fw23", `54cd5fb8e`
"Improve multi-process, display layers, dynamic shader cache and rework overlay" and `86eae5cc4` "Fix
qlaunch crash on second launch". Going by those messages (not verified here), qlaunch on firmware 23
boots and launches games on desktop and Android. The overlay is opt-in.

**What Eden does not do today on Harbor's path:**

- **It never runs the first-time setup.** The defaults are initial launch flags `0x10001`
  (`set/setting_formats/system_settings.cpp:40`) and an automatic "Eden" user when there are none
  (`acc/profile_manager.cpp:57`).
- **The starter gets the wrong focus messages.** It is tracked with `is_application = true`
  (`application_creator.cpp:82`), so `LifecycleManager` sends it `FocusStateChanged`. Harbor proved that
  the starter needs ChangeIntoForeground/ChangeIntoBackground.
- **It refuses the starter's audio.** It rejects audren REV16+ (`audio_core/common/feature_support.h:19`,
  `CurrentRevision = 15`); the starter needs REV18.
- **Several Harbor fixes are missing:** the held foreground message, input exclusion, held-button
  masking, HOME routing, sm access lists, and the firmware 22.x web applet program ids.

**Single biggest architectural difference.** Harbor's `AppletManager.cs`, a single 1917-line file, is
imperative: it pushes explicit messages from a queue and special-cases each hard-won behaviour (held
`ChangeIntoForeground`, the starter's message type, input recipient lists). Eden's am is a **state
machine that recomputes everything from the applet tree on every event**, with messages *derived* from
requested versus acknowledged state. So Harbor's behaviours cannot be copied as code: each must be
re-expressed as a state rule inside `WindowSystem::UpdateAppletStateLocked` and `LifecycleManager`. For
example, a completed child whose accessor is still open keeps "obscuring" its caller.

The good news follows from the same design. The pieces Harbor had to build in Ryujinx already exist in
Eden, as the real-am design: per-process contexts, a process-exit observer, the foreground-owner
compositor, multi-layer blending, per-aruid HID memory and a per-process CPU backend. The port is
mostly **behaviour deltas on Eden's am, hid_core and vi**, not new subsystems.

---

## 2. Feature table

Effort is in engineer-days for one engineer who knows the codebase, followed by confidence:
H = high, M = medium, L = low. "has" means Eden's code covers it, though it may be unverified with
firmware 23.0.1.

### 2a. Kernel, processes and memory

| Harbor feature | Ryujinx file(s) (commit) | Eden file(s) | Eden status | Verdict | Effort | Risk notes |
|---|---|---|---|---|---|---|
| Several guest processes at once; create-not-start; exit gives back pages, sessions, layers | `HOS/Kernel/Process/KProcess.cs`, `KernelContext.cs` `ProcessExited`, `Kernel/Memory/KPageTableBase.cs` `ReleaseAllPages`, `Services/ServerBase.cs` (`338f33cb8`, `df382a4d3`, `d99d9e0df`) | `hle/kernel/k_process.cpp`, `hle/service/os/process.cpp`, `am/process_creation.cpp`, `am/event_observer.cpp`, `am/process_holder.cpp` | has | reuse Eden | 0.5 (H), verification only | An accessor holds `shared_ptr<Applet>` and so keeps the `Process`. Confirm that pool usage returns after each applet (Harbor logged pool free sizes; add the same log). |
| Memory pools per NPDM (qlaunch/applets in the Applet pool, game in Application) and the resource limit | `Loaders/Processes/ProcessLoaderHelper.cs`, `Kernel/Common/KSystemControl.cs` | `k_process.cpp:1193` `LoadFromMetadata` (pool from `GetPoolPartition`), `hle/kernel/board/nintendo/nx/k_system_control.cpp:115-171` | has | reuse | 0.5 (H) | The pool sizes change with Eden's `memory_layout_mode`. Keep 4 GB (Application 3285 MiB, Applet 507 MiB) for retail parity: ARMS and Kirby compute their heap from it (`game-launch-crashes.md`). |
| VI framebuffer pages from the secure pool, non-contiguous (games had run out of memory) | `Services/Vi/SharedBufferManager.cs` (`463557412`) | `vi/shared_buffer_manager.cpp:33-60` `AllocateSharedBufferMemory` (`Pool::Secure`, page group) | has (Harbor ported this *from* Eden) | reuse | 0 | none |
| Firmware 22/23 kernel quirks: TLS+0x110 current thread handle; GetInfo 36-38 (shadow stack 0,0) | `KThread.SetTlsThreadHandle`, `Syscall.cs`, `InfoType.cs` (`93e57ecb0`, `463557412`) | `k_process.cpp:1014`, `svc/svc_thread.cpp:81`, `svc/svc_info.cpp:213` (Unknown37/38 → 0) | has | reuse | 0 | Eden returns 0 for 37 (AddressSpaceSize?) where Harbor returns the real size. qlaunch only reads 37/38 to skip an empty region (**guess** that this is harmless). |
| svcBreak dumps its Result/TLS; IPC non-success logging; F12 dump of every guest thread's stack | `Syscall.cs` Break, `IpcService.cs`, headless F12 (`574e3a336`, `8fb3ef91f`) | Eden: GDB stub, `LOG_*`; no thread-dump hotkey | partial | port logic | 1 (H) | This was the single most useful bring-up tool (memory note "Harbor hangs"). Port it before the boot sweep. |
| CPU backend per process. Harbor: HostTracked fallback for applets on Android after host-VA exhaustion | `HOS/ArmProcessContextFactory.cs` (`463557412`, `applet-startup.md`) | `k_process.cpp:1306-1327` (NCE only `IsApplication()`), `core/memory.cpp:50-64` (fastmem only for the application; Android `heap_tracker`), `src/core/device_memory.cpp:12` (one 2^39 arena) | has (different design) | reuse | 0 port; 2-3 (L) phone measurement | Applets use one shared arena and never fastmem, so Harbor's VA exhaustion cannot occur. Open question: is dynarmic page-table mode fast enough for qlaunch and the web applet on the phone? Harbor's yardstick: NetConnect creation to GPU in 2.76 s, PlayerSelect 2.54 s. |

### 2b. am: applet manager and lifecycle

| Harbor feature | Ryujinx file(s) (commit) | Eden file(s) | Eden status | Verdict | Effort | Risk notes |
|---|---|---|---|---|---|---|
| Per-applet am contexts, roles, stable aruid=pid | `HOS/SystemState/AppletManager.cs` (`338f33cb8`) | `am/window_system.cpp`, `am/applet.h`, `am/applet.cpp:22` (aruid = pid) | has | reuse | 0 | none |
| `ISystemAppletProxy`, `ILibraryAppletProxy`, `IOverlayAppletProxy`, appletAE 100/110/200/201/300/350/450/460 | `Am/AppletAE/IAllSystemAppletProxiesService.cs`, `ISystemAppletProxy.cs`, `IOverlayAppletProxy.cs` | `am/service/all_system_applet_proxies_service.cpp`, `system_applet_proxy.cpp`, `overlay_applet_proxy.cpp` | has; 450/460 return no object | port the 2 objects | 0.25 (H) | Harbor returns objects (`ISystemProcessCommonFunctions` → `IApplicationObserver`). Not called on the setup path so far. |
| HOME → title: `IApplicationCreator` 0; `IApplicationAccessor` 0/10/20/25/30/101/121/122/130; exit → `ApplicationExited` | `SystemAppletProxy/IApplicationCreator.cs`, `IApplicationAccessor.cs` (`d99d9e0df`) | `am/service/application_creator.cpp:24-70`, `application_accessor.cpp`, `window_system.cpp` `PruneTerminatedAppletsLocked` | has; 10 of 23 Harbor accessor commands are nullptr: 1, 110, 111, 120, 123, 124, 140, 160, 170, 180 | port logic | 1-1.5 (H) | `TrackApplet` **ASSERTs** `m_application == nullptr` (`window_system.cpp:66`). Harbor returns `AppletLaunchFailed`; replace the asserts. 121 only accepts kind 2 (UserChannel kind 1 → ResultUnknown). |
| Starter via `CreateSystemApplication` (AppletId 4, system buffer sharing, application-shaped foreground and suspension) | `IApplicationCreator.cs` 10, `AppletManager.CreateApplication` `IsSystemApplication` (`1076779fc`) | `application_creator.cpp:66-95`, `display_layer_manager.cpp:105` (sharing denied only to `AppletId::Application`) | has | reuse | 0 | Uses `CreateProcess(…, 1, HOS_VERSION_MAJOR)`, a key-generation gate; fine for 23.0.1 (**guess**). |
| **Starter receives ChangeIntoForeground/Background, never FocusStateChanged.** Its SAMS hand-offs to qlaunch (parental controls and two more steps) wait for 2 then 1 | `AppletManager.FocusMessage` (`46ff794ef`); RE `scratch/ryujinx-android/starter-re/` | `am/lifecycle_manager.cpp:44-62` (`m_is_application` selects 15 versus 1/2) and `UpdateRequestedFocusState`; starter built with `is_application=true` | **missing** | port logic: split `LifecycleManager`'s flag into "application suspension/focus rules" and "uses FocusStateChanged"; set the second to false for `AppletId::Starter` | 0.5-1 (H) | Without it, setup stalls at the hand-offs. Cheap to verify on the first starter run. |
| **qlaunch's ChangeIntoForeground after a library applet exits waits until qlaunch has released that applet's accessor** (1 s fallback); otherwise qlaunch's 0x29 handler deadlocks | `AppletManager.CompleteLibraryAppletLocked`, `ReleaseLibraryApplet`, `ReleaseHeldForegroundMessage` (`13d34f6bf`) | `window_system.cpp:281-302` (child unlinked as soon as its process terminates), `am/service/library_applet_accessor.cpp:88` (`~ILibraryAppletAccessor` defaulted) | **missing** | port logic: count "completed child whose accessor is alive" in `has_obscuring_child_applets` (or a per-caller hold flag); clear it in `~ILibraryAppletAccessor` plus a 1 s CoreTiming event; then `RequestUpdate` | 1.5-2 (M) | Timing-dependent deadlock (seen with eShop). Holders exclude application callers, as Harbor's do. |
| Library applets as firmware processes; HOME session forces LLE; 22.x program ids (Web/WifiWebAuth 0x…1043; Shop/LoginShare/Lhub/Login/Share 0x…1042; OfflineWeb 0x…100F) | `ILibraryAppletCreator.cs:21-47` (`f68f5b5b6`, `03a30e391`, `dfde80c3e`), `Types/AppletId.cs` | `library_applet_creator.cpp:15-95`, `am/am_types.h:76-120` (`AppletProgramId` is pre-22: Web 0x100A, Shop 0x100B, LoginShare 0x1010, WebAuth 0x1011; no 0x32/0x33/0x36/0x37 ids; Lhub 0x35 → program 0) | partial | port the table plus a "HOME session ⇒ all LLE" policy | 1 (H) | Each applet switched to LLE then surfaces its own missing commands: swkbd, Controller, the web family. This meets the services area for the web/eShop pages. |
| Library applet accessor and self-accessor commands | `LibraryAppletCreator/ILibraryAppletAccessor.cs`, `LibraryAppletProxy/ILibraryAppletSelfAccessor.cs` | `library_applet_accessor.cpp`, `library_applet_self_accessor.cpp` | partial: accessor 50, 102 and self 18, 20, 25, 31, 40, 80, 90, 100-102, 130 are nullptr | port | 1.5 (M) | 18/19/13 are unknown contracts in Harbor too (logged). |
| Partial-foreground applets draw over their caller; the caller stays on screen | `SurfaceFlinger.SetPartialOwner` (`de9f5ed33`), `AppletManager.ShowsOverCaller` | `window_system.cpp:470-515` (only AllForeground modes obscure; caller stays visible), `display_layer_manager.cpp:28` (blending for Partial*), `nvnflinger/hardware_composer.cpp` z-sort | has | reuse, plus raise a partial child's z above its caller | 0.5 (M) | Caller and partial child both get z=ForegroundVisible(2). `stable_sort` then keeps layer-stack order (child created later, so it draws on top). Fragile: set an explicit z. |
| Winding and unwinding (Lhub/Shop push context and reserve) | `IProcessWindingController.cs`, `AppletManager.WindAndDoReserved`/`Unwind` (`475e72c2a`) | `am/service/process_winding_controller.cpp`, `window_system.cpp:270-302,329-350` | has | reuse | 0.5-1 (M) verify | Not exercised past `PushContext` in Harbor either. |
| Suspend the title behind HOME and resume it (focus-handling modes, `Resume` message) | `AppletManager.UpdateSuspensionLocked` (`54af17b16`), `ISelfController` 13/14/16 (`ea9a5b3b1`) | `am/applet.cpp:32-57`, `lifecycle_manager.cpp:216-302` (`IsRunnable`) | has | reuse | 0 | none |
| Mute a suspended title's audio renderer | `AudioRendererManager.SetSuspended` (`54af17b16`) | `audio_core/renderer/system_manager.cpp`: no per-aruid suspend (**guess**) | missing | port | 1 (M) | Without it, Harbor heard a paused game keep mixing its last voices. |
| HOME short/long press routing and blocking | `AppletManager.HomeButtonShortPress`/`LongPress`/`PowerButtonMiddlePress`/`OverlayInputChanged` (`d99d9e0df`, `b36ac8efb`), `HomeButtonGesture` | `am/button_poller.cpp` (400/800 ms; Handheld+Player1 only), `window_system.cpp:216-266` `SendButtonAppletMessageLocked` | partial | port Harbor's routing (see the notes) | 1.5-2 (M) | Harbor's rules: a short press goes to the HOME menu, or to the foreground applet that set `ICommonStateGetter` 100; a watching overlay gets it first and keeps it if it set `IOverlayFunctions` 20; a blocked press makes the "HOME cannot be used" overlay notice. The long press goes to the overlay only; `IGlobalStateController` 12 sets the long-press time; a middle press on power goes to the overlay. Eden instead sends HOME to HOME menu, overlay **and application**, and reads 100 as "blocked = !enabled". Meets the frontend area (host HOME key) and the ovln area (am notifier 2000/8101). |
| Overlay applet beside qlaunch; quick-settings input hand-off | `AppletManager.StartOverlayApplet` (`b73812db3`, `b36ac8efb`), `OverlayAppletProxy/IOverlayFunctions.cs` | `applet_manager.cpp` `SetWindowSystem` (behind `enable_overlay`, launched **before** qlaunch), `window_system.cpp` `DoesOverlayTakeInputLocked`, z Overlay=3; `am/service/overlay_functions.cpp` | has (core); partial (commands) | reuse, plus enable it in the HOME session and port the commands | 1.5 (M) | `IOverlayFunctions` has 8 of the 22 commands Harbor added. Missing: 3, 5, 6, 10, 11 (power menu → shutdown/reboot), 30, 40, 41, 50, 60, 71, 75, 90, 101. The launch order differs from Harbor's (**guess**: harmless, because Eden binds fs per session). Meets the ovln area. |
| Lock accessors; HOME writer lock (pop-up actionability) | `ILockAccessor.cs`, `ICommonStateGetter` 30-32, `IHomeMenuFunctions` (`fbb4ba533`) | `am/service/lock_accessor.cpp`; `home_menu_functions.cpp:23-24` (30/31 nullptr) | partial | port 30/31 | 0.5 (M) | qlaunch's invitation-to-myPage shortcut checks the writer lock (`overlay-applet.md`). |
| General channel (overlay ↔ qlaunch SAMS messages) | `AppletManager.PushToGeneralChannel` | `home_menu_functions.cpp:20-21`, `system.TryPopGeneralChannel` | has | reuse | 0 | none |
| Reboot/shutdown from qlaunch or setup relaunches the console | `IGlobalStateController` 3/4, power-request marker (`1b82d27a4`) | `am/service/global_state_controller.cpp:34-44` (`system.Exit()`) | partial | port a core "restart requested" flag; the frontend area relaunches | 0.5 (H) core | The EULA path asks for a reboot. |
| Firmware 20-23 am commands (unknown contracts) | `IAppletCommonFunctions` 350, `ICommonStateGetter` 610/611, `ISelfController` 230/240, `ILibraryAppletAccessor` 170 (`463557412`) | `applet_common_functions.cpp:43`, `common_state_getter.cpp:78-79`, `self_controller.cpp:418-430`, `library_applet_accessor.cpp:231` | has (stubs) | port the validation | 0.5 (H) | Contract gaps. 240: Eden lacks am 23.0.1's "value > 3 → `am::InvalidParameters` (0x3f480)". 230: Eden reads one u32 and writes a u16; Harbor observed two inputs and no output. Audit every "has" against `qlaunch-probe.md`'s unknown-contract list. |

### 2c. hid

| Harbor feature | Ryujinx file(s) (commit) | Eden file(s) | Eden status | Verdict | Effort | Risk notes |
|---|---|---|---|---|---|---|
| Per-aruid HID shared memory | `Services/Hid/Hid.cs`, `AppletResourceMemory.cs` (`b093e284b`) | `hid_core/resources/applet_resource.cpp`, `shared_memory_holder.cpp` | has | reuse | 0 | none |
| **Only the input-focus applet chain gets input.** A caller behind a full-screen applet or quick settings gets none; inline helpers (Login with a partial swkbd) and their caller share input | `AppletManager.GetInputRecipients`/`ApplyInputFocusLocked`, `HidInputFocus.cs` (`b093e284b`, `5fe8d9da8`) | `window_system.cpp:486-490` `needs_hid_input = is_foreground && window_visible && !overlay_takes_input`, applied to every applet in the foreground chain, obscured or not | partial (overlay exclusion right; full-screen exclusion missing; inline sharing works by accident) | port logic into `UpdateAppletStateLocked`: obscured ⇒ no input; a partial child keeps its caller interactive | 1 (M) | This is the Outbound name-entry bug class (Unity replayed presses made in the keyboard). |
| Applets without focus read neutral entries (sampling numbers still advance) | `NpadDevices.cs`, `TouchDevice.cs` (`b093e284b`) | `hid_core/resources/npad/npad.cpp:528` (`if (!enable_pad_input) continue;`, so the lifo freezes on its last state); `touch_screen/touch_screen_resource.cpp:552-559` (writes denied entries) | partial (touch has it, npad does not) | port: write a neutral `NPadGenericState` per lifo for disabled aruids | 1 (M) | A lifo frozen while a button was down may read as a stuck button (**guess**). hid_core is shared with every game: regression-test single-game boots. |
| A button or touch already held when an aruid gains input is hidden from it until released | `AppletResourceMemory.HeldButtons/TouchHeld`, `Npads.HoldPressedButtons` (`b093e284b`) | none | missing | port into `NPad::OnUpdate` / `TouchScreenResource` keyed by aruid | 1-1.5 (M) | The button that closes an applet otherwise arrives as a new press for its caller. |
| Supported npad styles/players per aruid; SystemExt style for system aruids, dropped when a game has focus | `NpadDevices.ApplyAruidSettings`, `SystemStyle` (`e4e7b47ca`, `7feafabf6`, `b37e7e6f8`) | `npad/npad_resource.cpp` (per-aruid style sets), `abstracted_pad/abstract_button_handler.cpp:152` (system_ext from `GetActiveData()`), `hid_system_server.cpp:515` (hid:sys 322, guest-driven) | has (guest-driven) | reuse; verify `GetActiveData()` follows am's focus | 0.5-1 (M) | Setting SystemExt for a game aborts Slime Rancher (`GetVibrationDeviceHandles` asserts). Eden only sets it on an aruid's own call (**guess**: correct). |
| hid:sys commands the Controllers applet and HOME use | `IHidSystemServer.cs` (`662437d6e`) | `hid/hid_system_server.cpp` | has: 34 of 35 (514 missing) | reuse; port 514 | 0.25 (H) | 514's output is an unknown contract. |

### 2d. vi, nvnflinger, display and capture

| Harbor feature | Ryujinx file(s) (commit) | Eden file(s) | Eden status | Verdict | Effort | Risk notes |
|---|---|---|---|---|---|---|
| One vsync event per display session (fixed applets reading input one run in three) | `IApplicationDisplayService` 5202, `Horizon.CreateVsyncEvent` (`f147b6c80`) | `vi/application_display_service.cpp:253-265`, `vi/vsync_manager.h` | has | reuse | 0 | none |
| Layers for qlaunch, overlay, applets and game; show the foreground owner; drain hidden layers; blend the overlay and partial layers | `SurfaceFlinger.cs` `SetForegroundOwner`/`SetOverlayOwner`/`SetPartialOwner`, `Gpu/Window.cs`, Vulkan `Window.DrawOverlay` (`338f33cb8`, `b73812db3`, `de9f5ed33`) | `nvnflinger/hardware_composer.cpp` (acquires every layer, composites visible ones z-sorted with blending), `am/display_layer_manager.cpp` (`SetWindowVisibility`, `SetOverlayZIndex`, layer-stack masks) | has (Eden ahead: N layers, OpenGL and Vulkan) | reuse | 0 | Harbor composes only one extra layer, Vulkan only. |
| System buffer sharing (IndirectDisplay/SharedBuffer) for non-application applets; 3+ sessions (qlaunch, starter, starter's applet) | `Vi/SharedBufferManager.cs` `MaxSessions = 3`, per-session slot base (`748c57f5b`, `463557412`) | `vi/shared_buffer_manager.cpp:178-216,330-335` (`SharedBufferMaxSessions = 2`; `presentation_slot_base` is always 0, so every session renders into pool slots 3-4) | partial | port per-session slot bases and MaxSessions ≥ 3 (4 if the overlay uses fbshare) | 1 (M) | **(guess)** A backgrounded fbshare client (qlaunch behind the starter) can overwrite the front applet's frame: flicker or wrong frames during setup. Slot count is limited to 16 (`static_assert`). |
| Capture buffers / thumbnails (`IDisplayController` 22-27, own-layer screenshot) | `IDisplayController.cs` (Harbor: "not written", ponytail) | `display_layer_manager.cpp` `WriteAppletCaptureBuffer`, `shared_buffer_manager.cpp:521` (real GPU capture), `display_controller.cpp` (0-4, 6, 9-18, 21, 28 nullptr) | has (ahead of Harbor) / partial commands | reuse; port 6/21/28 | 0.5 (M) | none |
| Capture button: picture to album, 96×54 thumbnail, ovln 8001-8004; caps:a for PhotoViewer | `Caps/OverlayScreenShot.cs`, `Caps/IAlbumAccessorService.cs` (`b36ac8efb`, `f68f5b5b6`) | `caps/*`, `window_system.cpp` (DetectShortPressingCaptureButton to applets) | partial: caps:a has 6 of 19 Harbor commands | port | 2 (M) | Missing caps:a commands are in appendix A. Meets the ovln area (8001-8004). |

### 2e. fs, ncm, sm and ns launch plumbing

| Harbor feature | Ryujinx file(s) (commit) | Eden file(s) | Eden status | Verdict | Effort | Risk notes |
|---|---|---|---|---|---|---|
| Firmware titles from the user's installed firmware; HOME library from the user's games | `ContentManager.cs`, `Ns/HomeMenuLibrary.cs`, `HomeMenuScanner.cs` (`4d11c9973`) | `file_sys/registered_cache` via `system.GetContentProviderUnion()`, `ns/application_manager_interface.cpp:565` `ListApplicationRecord`, `ns/read_only_application_control_data_interface.cpp` | has | reuse | 0 | Games must be in Eden's content providers (NAND/SD install, or the frontend's manual provider). The frontend area's concern. |
| fs client per process | `LibHacHorizonManager.GetClientFor`, `IFileSystemProxy` 1 (step 9) | `filesystem/fsp/fsp_srv.cpp` `SetCurrentProcess` per session | has | reuse | 0 | none |
| fsp 210/212 on firmware 23 | `IFileSystemProxy.cs` 210/212 (`463557412`) | `fsp_srv.cpp:121` maps **210 → SetCurrentProcess** (`e7a96c790`) | **conflict** | settle by RE, then port | 0.5 (M) | Harbor: 210 is `OpenDataStorageByCurrentProcess` (an IStorage out), from the 23.0.1 fs client in overlayDisp `+0x13adf0`. If Harbor is right, Eden returns no object and firmware-23 applets cannot mount their RomFs. |
| fsp 21: a program deletes a system save it owns (the web applet clears 0x8000000000001093) | `IFileSystemProxy.cs` 21 (`cf76f16fd`) | `fsp_srv.cpp:67` nullptr | missing | port | 0.5 (H) | none |
| sm honours each firmware program's NPDM service access list (2021-0008 outside it) | `Services/Sm/IUserInterface.cs` (`df382a4d3`) | `hle/service/sm/sm.cpp`: no access check | missing | port: parse ACI0 SAC at load; enforce for program ids 0100000000000000-010000000000FFFF only | 1-1.5 (M) | Without it MyPage dies with 2121-0090 (it opens `friend:u`), and setup hides three pages ("link a Nintendo Account", "Additional users", "To Parents and Guardians"). |
| ns launch path: views, control data 19/23, 1704/1706, rights, terminate result, plus firmware 23's 4105, 2366, 4008… | `Ns/IApplicationManagerInterface.cs`, `IReadOnlyApplicationControlDataInterface.cs` (`d95030381`, `463557412`, `5fe8d9da8`) | `ns/application_manager_interface.cpp` (4105 stub, 4042 present, 2366 nullptr), `read_only_application_control_data_interface.cpp:145` (23 = ControlData3) | partial: 21 of 34 Harbor ids | port the 13 missing (appendix A) | 1.5 (M) | 2366's purpose belongs to the acc area (account registration). 4105 linked-account work is unknown in both. |
| New console = first-time setup: initial launch flags 0, no default user, and the setup's writes persist | `Settings/ISystemSettingsServer.cs`, `SystemSettingsStore.cs`, `AccountManager.cs` (`a2def06f2`, `bf7dc177f`, `5c104e31e`) | `set/setting_formats/system_settings.cpp:40` (0x10001), `set/system_settings_server.cpp` (file-backed, `SetSaveNeeded`), `acc/profile_manager.cpp:57` ("Eden" user), `am/applet_manager.cpp` (ASSERT on the current user; preselected user pushed to qlaunch) | partial | port a "console mode" switch: flags 0 on a new data dir, no automatic user, no preselected user when there is none | 1-1.5 (H) | Without it the starter never runs. The user-creation commands themselves exist (acc:su 200/201/205; the acc area covers OAuth). |
| set:sys commands the sweep needs | `ISystemSettingsServer.cs` | `set/system_settings_server.cpp` | 43 of 51 (156, 157, 171, 202, 207-210 missing) | port | 0.5 (H) | 156/157 PctlReadyFlag layout is an unknown contract. |
| Assorted system modules qlaunch sweeps (lbl, btm:sys, ldn/lp2p monitors, nfc:sys, notif:s, erpt:c) | `Horizon/Lbl`, `BluetoothManager/*`, `Ldn/*`, `Nfc/*`, `Notification/*`, `Erpt/IContext.cs` | `lbl/lbl.cpp` 17/19, `btm/btm_system_core.cpp` 12/24, `ldn/*` all, `nfc/nfc.cpp` ok, notif:s (`glue/notif.cpp`) 2/4, `erpt/erpt.cpp` 5/16 | partial | port | 2 (M) | erpt must accept the Error applet's report creation (`3a4bf842a`). |

### 2f. Audio

| Harbor feature | Ryujinx file(s) (commit) | Eden file(s) | Eden status | Verdict | Effort | Risk notes |
|---|---|---|---|---|---|---|
| audren REV16 (22.5.0 starter) and REV17/18 (23.0.1 starter) accepted through the existing layouts | `Ryujinx.Audio/Renderer/Server/BehaviourInfo.cs`, `AudioRendererManager.cs` (`463557412`) | `audio_core/common/feature_support.h:19,114` (`CurrentRevision = 15`); checks in `audio_render_manager.cpp:35`, `renderer/system.cpp:110`, `behavior/info_updater.cpp:587` | **missing** | port the compatibility acceptance | 0.5 (H) | Without it the starter dies with 2153-0002. REV17/18 DSP changes are undocumented in both (L). |
| audctl: system volume and mute, applied as the output gain | `Horizon/Audio/Ipc/AudioController.cs`, `ConsoleOutputVolume.cs` (`90255efa8`) | `audio/audio_controller.cpp` (`"audctl"`; the 18 Harbor-added ids are registered, 5000 included) | has (commands) | reuse; port the gain hookup later | 0.5 (L) | none |

### 2g. Per-command ledger summary

Method: take every IPC command id added on `feature/android-port` (`[CommandCmif(n)]` / `[CmifCommand(n)]`
lines in the diff from `c93a0e2b06e`), map each interface to Eden's class, and parse that class's
`FunctionInfo` table. "Eden handler" means the id is registered with a non-null handler; that handler
may be a logged stub or use a different contract. Classes that share a name across modules (am and
audctl `IAudioController`, the `IStorage` classes) are merged, which can overstate "handler" slightly.
This is a grep-level count.

| Area | Harbor-added ids | Eden handler | Eden `nullptr` | Absent in Eden | Missing in total |
|---|---|---|---|---|---|
| core (am, hid, vi, fs, ns-launch, set, lbl, btm, ldn, nfc, notif, erpt, caps, audctl) | 457 | 335 | 111 | 11 | **122** |
| OpenPak services (acc, pctl, friends, news/bcat, nifm, nim, npns, ns:ec, olsc, ovln, sfdnsres, ngc, prepo) — services area | 230 | 115 | 78 | 37 | **115** |

The core gap concentrates in a few interfaces. In am, `ILibraryAppletSelfAccessor` (11), `IOverlayFunctions` (14)
and `IApplicationAccessor` (10). Elsewhere: ns `IApplicationManagerInterface` (13), caps:a (13), btm:sys (12) and
erpt (11). The services gap is dominated by pctl (43) and the acc OAuth procedures (17). Full lists are in appendix A;
the per-row table is `scratch/harbor-eden/research/ledger/ledger.tsv`.

---

## 3. Recommended port order for phase 2

**Gate.** On desktop, firmware 23.0.1, new data directory:

1. qlaunch and overlayDisp boot, and qlaunch starts the starter (flags 0).
2. Walk the setup: language, region, EULA, handheld/TV, Internet (NetConnect LLE, wired), time zone,
   user creation (PlayerSelect LLE plus swkbd LLE), skip the account-link and parental-control steps,
   "Setup is complete!".
3. HOME renders with the overlay live.
4. A cold reboot boots straight to HOME (0x10001 persisted).

**Serialized track A — am/applet core. One owner: `am/*.cpp|h`, `window_system.*`, `lifecycle_manager.*`,
`applet_manager.*`, `library_applet_*`. In order:**

1. **HOME-session mode switch** in `AppletManager::SetWindowSystem`, plus `set` and `acc` defaults:
   - a new data dir gets initial launch flags 0 and no automatic user;
   - remove the user ASSERT, and push no preselected user when there is none;
   - enable the overlay in HOME mode;
   - force LLE for every applet in HOME mode;
   - load the 22.x/23.x `AppletProgramId` table.
   (1.5 d)
2. **`LifecycleManager` flag split** so the starter gets 1/2 messages (0.5-1 d). Replace the `TrackApplet`
   and `CreateApplication` ASSERTs with result codes (0.25 d).
3. **Boot sweep, one missing command per run** (Harbor's discipline; keep `use_auto_stub` off so
   `UNIMPLEMENTED_MSG` is loud): starter, NetConnect, PlayerSelect, swkbd, Error and their am
   self-accessor commands. Every service fix lands through track F. (3-5 d, L)
4. **Hold qlaunch's ChangeIntoForeground until the accessor is released** (1.5-2 d). Not strictly on the
   setup path, but the first HOME → applet → HOME round trip after the gate needs it.
5. **am side of input focus**: the recipient chain in `UpdateAppletStateLocked` (1 d). Integrates with B.
6. After the gate, still phase 2: HOME short/long and power routing, `IGlobalStateController` 12,
   `IOverlayFunctions` 22.x commands, the reboot flag, accessor/self-accessor commands, and audio mute
   for a suspended title.

**Parallel tracks (independent files):**

- **B — hid_core** (`npad.cpp`, `touch_screen_resource.cpp`, `applet_resource.*`): neutral npad entries
  for disabled aruids, a per-aruid held-input mask, and checking that `GetActiveData()` matches focus.
  Single-game regression tests. 2.5-3.5 d. Lands before A5.
- **C — vi** (`shared_buffer_manager.*`, `display_layer_manager.cpp`): per-session presentation slot
  bases with MaxSessions ≥ 3; explicit z for a partial child; `IDisplayController` 6/21/28. 1.5 d.
  Must land before A3 reaches PlayerSelect.
- **D — audio_core** (`feature_support.h`): accept REV16-18. 0.5 d. **Must land before A3**, or the
  starter dies at about 14 s.
- **E — kernel/fs/sm**:
  1. svcBreak logging and an F12-style guest thread dump (1 d; first, it pays back during A3);
  2. settle the fsp 210/212 contract by reading the 23.0.1 fs dispatch, then port (0.5 d);
  3. fsp 21 (0.5 d);
  4. sm ACI0 enforcement (1-1.5 d; needed so setup shows the account-link page and MyPage works).
- **F — service sweep** (set:sys, ns, lbl, btm, erpt, notif, caps): one file per service, any number of
  people. Commands land in the order A3's runs surface them. About 4 d in total.
- **G — OpenPak services**: the services area (acc OAuth, pctl pairing, friends/ovln senders, ns 2366).
  Not on the gate path, as long as setup takes "Later" on the account link.

Rough gate estimate: about 15-20 engineer-days (M/L). Full core parity with Harbor, every row in §2:
about 35-45 engineer-days (L), excluding the services area and phone performance work.

---

## 4. Top risks and unknowns

1. **Re-expressing Harbor's fixes inside Eden's state-derived am model** (held foreground message,
   starter messages, input chain, HOME routing). The behaviours are proven; their translation into
   `UpdateAppletStateLocked` and coalesced lifecycle messages is not.
   - *De-risk:* keep each rule behind a HOME-session flag. Write regression scripts for Harbor's known
     sequences (eShop open and close with no freeze; Album B→HOME; starter parental hand-off; the
     Outbound keyboard name entry). Reuse Harbor's log lines (`am: foreground pid …`) in Eden so logs
     can be diffed side by side.
2. **Applet speed on Android without fastmem or NCE.** Every applet runs on dynarmic page tables. No
   phone evidence exists yet for Eden.
   - *De-risk:* before porting, run stock Eden's qlaunch button on the ROG Phone 3 with firmware 23.0.1
     and time NetConnect and PlayerSelect from creation to first frame. Compare with Harbor's
     2.76 s/2.54 s. This is a measurement, not a build; the owner runs it.
3. **The fsp 210 contract conflict** (Eden: SetCurrentProcess; Harbor RE: OpenDataStorageByCurrentProcess).
   - *De-risk:* disassemble the 23.0.1 fs server dispatch for 210 (Harbor gives the client address in
     overlayDisp `+0x13adf0`), or watch overlayDisp's RomFs mount on the first Eden boot with the overlay
     enabled.
4. **Shared fbshare presentation slots (guess).** All sessions use slots 3-4 in Eden. Starter plus
   PlayerSelect plus a backgrounded qlaunch may tear or show the wrong frames.
   - *De-risk:* log the slot per Present during the setup's user-creation page. Port Harbor's slot bases
     before A3 reaches PlayerSelect.
5. **REV17/18 audio renderer semantics are unknown.** Harbor accepts them through the REV15 layouts with
   size validation, and setup works.
   - *De-risk:* compare starter's `RequestUpdate` section sizes against `info_updater.cpp`'s validations.
     Mark the acceptance as compatibility, not support.
6. **hid_core changes affect every game.** The neutral entries and held mask live in shared npad and
   touch code.
   - *De-risk:* touch only aruids that have had input disabled at least once. Keep a standalone-game
     smoke list: Slime Rancher (SystemExt assert), ARMS, Kirby (heap budget).
7. **Contract drift where Eden "has" a handler** (§2 ledger: 335 "has" in core). Examples: 230 (u16 out
   versus two inputs), 240 (no validation), `ICommonStateGetter` 100 semantics, 450/460 with no object,
   accessor 121 refusing UserChannel.
   - *De-risk:* a 2-day audit of every Eden handler that Harbor documented in `qlaunch-probe.md` (the
     shapes table and "Open unknown contracts"), before the boot sweep relies on them.
8. **Upstream churn.** Eden lands weekly qlaunch, firmware-23 and am changes in exactly these files
   (`5d150cac5`, `bfba95fd6`, `07f40d5ca`, `54cd5fb8e`).
   - *De-risk:* pin `10bcd2d84`. Put Harbor deltas in small hooks (a HOME-session policy object rather
     than scattered edits). Schedule rebases deliberately. Never upstream.
9. **Licence.** Eden is GPL-3.0-or-later (yuzu parts GPL-2.0-or-later); Harbor is AGPL-3.0-only.
   GPLv3 §13 permits combining with AGPLv3, and Harbor's own Ryujinx-side logic is the owner's code,
   with Ryujinx itself MIT. **(guess, not legal advice)**
   - *De-risk:* the owner confirms the combined-work licence before phase 2 code lands.
10. **Boot ordering and the remaining asserts.** Eden starts overlayDisp before qlaunch; Harbor starts it
    after. Asserts on a second application, or on a missing user, kill the session instead of producing
    a firmware error.
    - *De-risk:* covered by A1/A2. Watch the first boot's pid order and fs bindings.
11. **Where this area meets the others** (interfaces to agree on early):
    - **hid:** am ↔ hid is `HidRegistration::EnableAppletToGetInput(pad, touch)` →
      `ResourceManager::EnablePadInput`/`EnableTouchScreen`. It needs a "recipients" extension for the
      held mask.
    - **ovln:**
      - Overlay power menu → `IOverlayFunctions` 10/11.
      - The am overlay notifier (2000 OvlHomeNg, 8101) → Eden psc `ISender` (services area).
      - Capture → caps plus ovln 8001-8004.
    - **friends:** `IApplicationAccessor` 180 ↔ friends invitations.
    - **acc:** ns 2366/4105 and acc:su 211-214.
    - **frontends:** the host HOME key and long-press source (`ButtonPoller` watches only the Handheld
      and Player1 emulated controllers); reboot relaunch.

---

## Appendix A. Command ids Harbor added that Eden lacks (nullptr or absent), by interface

Core area:

| Interface (Eden class) | Missing ids |
|---|---|
| `IApplicationAccessor` | 1, 110, 111, 120, 123, 124, 140, 160, 170, 180 |
| `ILibraryAppletAccessor` | 50, 102 |
| `ILibraryAppletSelfAccessor` | 18, 20, 25, 31, 40, 80, 90, 100, 101, 102, 130 |
| `IOverlayFunctions` | 3, 5, 6, 10, 11, 30, 40, 41, 50, 60, 71, 75, 90, 101 |
| `IAppletCommonFunctions` | 52, 60, 61, 62, 100 |
| `ICommonStateGetter` | 40 (also 15, an overlayDisp sleep-lock wait, unknown contract in Harbor) |
| `ICradleFirmwareUpdater` | 4, 5 |
| `IDisplayController` | 6, 21, 28 |
| `IGlobalStateController` | 9, 11, 12, 13 |
| `IHomeMenuFunctions` | 30, 31 (writer lock accessors; from Eden's table, not the diff) |
| `ISystemProcessCommonFunctions` (appletAE 450 object) | 1 |
| `IHidSystemServer` | 514 |
| `IApplicationRootService` (vi:u) | 1 `GetDisplayServiceWithProxyNameExchange` (gives the starter its system display service) |
| `FSP_SRV` | 21, 212 (and 210, see the conflict in §2e) |
| `IApplicationManagerInterface` (ns) | 1, 45, 49, 300, 518, 935, 1508, 2366, 4008, 4058, 4084, 4089, 4096 |
| `IContentManagementInterface` | 607 |
| `IDownloadTaskInterface` | 701 |
| `IReadOnlyApplicationControlDataInterface` | 3, 20 |
| `IVulnerabilityManagerInterface` (ns:vm) | 3101 |
| `ISystemSettingsServer` | 156, 157, 171, 202, 207, 208, 209, 210 |
| `LBL` | 4, 5 |
| `IBtmSystemCore` | 2, 3, 8, 9, 10, 11, 12, 15, 16, 18, 19, 21 |
| `IAlbumAccessorService` (caps:a) | 0, 6, 15, 16, 17, 100, 110, 120, 140, 301, 302, 501, 50012 |
| `ErrorReportContext` (erpt:c) | 2, 6, 7, 8, 10, 14, 20, 21, 22, 30, 40 |
| notif:s | 1010, 1500 |

Services area:

| Interface | Missing ids |
|---|---|
| acc `IAdministrator` | 251, 252 |
| acc `IManagerForSystemService` | 120, 136 |
| acc OAuth procedures | 0, 1, 2, 3, 10, 100-104, 110, 111, 200, 210, 220, 221, 230 |
| acc `IProfileEditor` | 30 |
| acc:su | 206, 211-214, 230, 405, 410 |
| news `IDownloadContext` | 0, 2, 3 |
| news `IEventHolder` | 0 |
| news `INewsService` | 30500, 40100 |
| nifm `IRequest` | 7, 8 |
| nim:shp | 1, 2, 3 |
| npns:s | 1, 7, 26, 162, 201, 202 |
| ns:ec | 0-3, 8-11 |
| olsc | 209, 1001, 10000 |
| olsc `IAsyncLatestVersionPlayablePlatformsRequest` | 0-3, 100 |
| pctl | 1007-1009, 1011, 1012, 1023, 1034, 1037, 1044, 1201-1205, 1207-1209, 1406, 1411, 1412, 1421, 1424, 1475, 2001-2017, 2025-2027, 9407 |
| sfdnsres | 16, 100, 102, 104 |
| bcat | 20301 |
| bcat `DeliveryTaskSuspension` | 0 |
| ovln `ISender` | 1 |
| friends | 10502, 10503 |

## Appendix B. Firmware 23.0.1 items from `firmware-23.md` / `qlaunch-probe.md`, checked against Eden

| Item | Eden |
|---|---|
| GetInfo shadow-stack queries (37/38) | stubbed 0 (`svc_info.cpp:213`) — ok |
| audren REV18 | refused (CurrentRevision 15) — **port** |
| am `ISelfController` 240 | registered, no validation — port the >3 → InvalidParameters check |
| ns 4105 offline handler | registered stub — ok for offline; linked-account work unknown in both |
| ns control data 23 | registered, routed to ControlData3 — ok (same approach as Harbor) |
| ns 2366 (copied event plus `IAsyncResult`) | nullptr — port (accounts area: account registration path) |
| fsp 210/212 | conflicting/missing — settle by RE |
| qlaunch 23.0.1 hash | `e3b0b627…adb1` (`firmware-23.md`); boot to HOME proven on Harbor PC only |
