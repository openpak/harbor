# Harbor on Eden: host and frontend map (phase 0)

Written 2026-10-06. Analysis only: nothing was built, run or changed.

Sources read:
- Harbor: `emulators/ryujinx-android`, branch `feature/android-port`. Commits up to 5fe8d9da8 plus the docs commit after it.
- Eden: `emulators/harbor-eden`, branch `harbor` = upstream `10bcd2d84` (2026-10-05).
- The old OpenPak Eden fork: `emulators/eden`, branch `openpak-playability-column`, `d88ed1bd7f`.

All repository paths below are relative to those roots: `R/` = ryujinx-android, `E/` = harbor-eden, `F/` = old fork.

Owner decision: nothing from this work is ever sent upstream to Eden.

Labels used throughout:
- **Guess** means I inferred it but did not verify it.
- **Interfaces** names the area the work touches: map-core (am/hid/vi/ns), map-services (OpenPak services and openpak-client), Borealis (the wizard evaluation).

---

## 1. How Eden's frontends are built

### 1.1 Android (`E/src/android`)

**Gradle**
- `E/src/android/settings.gradle.kts` includes `:app` and a project `:Eden` that points at the repository root.
- `app/build.gradle.kts`:
  - AGP 8.9.1, Gradle 8.13, Kotlin 1.9.20.
  - `namespace = "org.yuzu.yuzu_emu"`, `applicationId = "dev.eden.eden_emulator"`.
  - minSdk 24, raised to 33 for `mainline`. targetSdk 36. NDK `28.2.13676358`, CMake `3.31.6`. Only `arm64-v8a` is built.
  - Flavours: `mainline`, `genshinSpoof` (**applicationId `com.miHoYo.Yuanshen`**, delete it), `legacy` (`dev.legacy.eden_emulator`, `-DYUZU_LEGACY=ON`, minSdk 29) and `chromeOS`.
  - Build types: `release`, `relWithDebInfo` (`.relWithDebInfo` suffix, the default) and `debug` (`.debug` suffix).
  - `externalNativeBuild` runs the root `CMakeLists.txt` with `-DENABLE_QT=0 -DYUZU_USE_CPM=ON -DCPMUTIL_FORCE_BUNDLED=ON -DYUZU_USE_BUNDLED_FFMPEG=ON`. Extra arguments come in through `-PYUZU_ANDROID_ARGS`.
  - `jniLibs.useLegacyPackaging = true`: native libraries are extracted to disk, which adrenotools needs.
  - Signing reads the `ANDROID_KEYSTORE_FILE`, `ANDROID_KEYSTORE_PASS` and `ANDROID_KEY_ALIAS` environment variables. Otherwise it uses the committed public `app/debug.keystore`.

**Process entry**
- `YuzuApplication.onCreate` does, in order:
  1. `DirectoryInitialization.start()`. This sets the user directory to `getExternalFilesDir(null)`, calls `NativeLibrary.setAppDirectory`, `NativeConfig.initializeGlobalConfig()`, `NativeLibrary.initializeSystem(false)` and `reloadProfiles()`.
  2. `GpuDriverHelper.initializeFreedrenoConfigEarly()`.
  3. `GpuDriverHelper.initializeDriverParameters()`, which loads the GPU driver once per process.
  4. `NativeInput.reloadInputDevices()`.
  5. `ControllerNavigationGlobalHook.install(this)`.

**Activities**
- `ui/main/MainActivity.kt` is the launcher, with a splash screen.
  - Its NavHost graph is `res/navigation/home_navigation.xml`, starting at `gamesFragment`.
  - When the `FirstApplicationLaunch` shared preference is true, it routes to `firstTimeSetupFragment`, which is `fragments/SetupFragment.kt`.
  - The game list is `ui/GamesFragment.kt`.
  - Settings live in `features/settings/ui/SettingsActivity` and `SettingsFragmentPresenter.kt`.
- `activities/EmulationActivity.kt` is the game screen.
  - It is `singleTop` and goes immersive in `enableFullscreenImmersive()`.
  - It implements `InputManager.InputDeviceListener`.
  - It hosts `fragments/EmulationFragment.kt` (2,611 lines). That fragment holds the surface, the `EmulationState` thread, the in-game drawer (`res/menu/menu_in_game.xml`: pause, quick overlay, settings, quick settings, controls, multiplayer, amiibo, overlay controls, lock drawer, exit) and the performance overlay.

**JNI boundary**
- Kotlin `external fun`s live in four places:
  - `NativeLibrary.kt`: about 110 functions, including `run`, `surfaceChanged`, `pause`, `unpause`, `stop`, `initializeGpuDriver`, `installKeys`, `isFirmwareAvailable`, `firmwareVersion`, `getAppletLaunchPath`, `setCurrentAppletId`, `addGameFolderFileToFilesystemProvider` and the user/profile calls.
  - `features/input/NativeInput.kt`: gamepad button/axis/motion events, touch, overlay button/joystick, `registerController`.
  - `utils/NativeConfig.kt`: typed get/set by key, game directories, external content directories, overlay data, input settings.
  - `utils/NativeFreedrenoConfig.kt` and `NativePostProcessing.kt`.
- The native side is `app/src/main/jni`:
  - `native.cpp` (2,337 lines) holds the singleton `EmulationSession`: `InitializeGpuDriver`, `InitializeSystem`, `InitializeEmulation`, `RunEmulation`, `SetAppletId` and the content-provider configuration.
  - `native_input.cpp`, `native_config.cpp` and `android_config.cpp`/`android_settings.h` (Android-only settings such as `driver_path`, `theme`, `enable_qlaunch_button`).
  - `emu_window/`, which is the `ANativeWindow` render target.
- JNI class and method IDs are cached in `E/src/common/android/id_cache.cpp`. It hard-codes the `org/yuzu/yuzu_emu/...` class paths nine times.

**Settings**
- Core settings are `Settings::values` (`E/src/common/settings.h`). Android-only settings are `AndroidSettings::values`.
- Both are stored in `<user>/config/config.ini` through `AndroidConfig` (`frontend_common/config.cpp`).
- UI-only flags such as `FirstApplicationLaunch` and the overlay preferences are Android `SharedPreferences` (`features/settings/model/Settings.kt`).
- Per-game configuration is a separate ini file per title.

**Driver manager**
- `utils/GpuDriverHelper.kt` keeps driver zips in `<user>/gpu_drivers/`. It extracts the chosen one into `filesDir/gpu_driver/`.
- The choice is the setting `driver_path`, which is a path to a zip. There is also a per-game override.
- `GpuDriverMetadata.kt` reads `meta.json` (`minApi`, `libraryName`). `installCustomDriver` refuses a package when `minApi > SDK_INT`.
- `fragments/DriverManagerFragment.kt` and `model/DriverViewModel.kt` are the manager screen. `fragments/DriverFetcherFragment.kt` downloads from GitHub. Its repositories are MrPurple666/purple-turnip, crueter/GameHub-8Elite-Drivers, K11MCH1/AdrenoToolsDrivers, Weab-chan/freedreno_turnip-CI and whitebelyash/freedreno_turnip-CI. It does no checksum check.
- Native loading is `EmulationSession::InitializeGpuDriver` → `adrenotools_open_libvulkan(...)`, which falls back to the system driver.
- libadrenotools (Eden's fork `eden-emulator/libadrenotools 8ba23b42d7`) and liblinkernsbypass are built from source through CPM.
- Freedreno/Turnip environment variables can be tuned in `NativeFreedrenoConfig` and `FreedrenoSettingsFragment`.

**Input overlay**
- The overlay is `overlay/InputOverlay.kt` (1,378 lines), from the yuzu lineage. Its parts are `InputOverlayDrawableButton`, `...Dpad` and `...Joystick`, and `overlay/model/OverlayControl.kt`, which includes `BUTTON_HOME` and `BUTTON_CAPTURE` with landscape, portrait and foldable positions. Layout data is in `NativeConfig.get/setOverlayControlData`.
- Events go through `NativeInput.onOverlayButtonEvent` and `onOverlayJoystickEvent` into `input_common/drivers/virtual_gamepad.cpp`. The port is chosen by `IsHandheldOnly()`.
- Auto-hide exists (`ENABLE_INPUT_OVERLAY_AUTO_HIDE`, `INPUT_OVERLAY_AUTO_HIDE` seconds).
- Hide-on-controller exists too: `HIDE_OVERLAY_ON_CONTROLLER_INPUT`, with `EmulationFragment.onPhysicalControllerStateChanged` driven by `EmulationActivity.onInputDeviceAdded/Removed`.

**Controller mapping**
- `utils/InputHandler.kt` decides what counts as a physical gamepad (`isPhysicalGameController`). It sends key and axis events as `NativeInput.onGamePadButtonEvent(guid, port, keycode, …)` to `E/src/input_common/drivers/android.cpp`.
- Mappings are per player, built from `Common::ParamPackage`.
- `Android::GetButtonMappingForDevice` builds the auto-map:
  - By **label**, Android `BUTTON_A` → Switch A.
  - By **position** for the vendor IDs in `flipped_ab_vids` (sony, nintendo, razer, redmagic, backbone, xbox) and `flipped_xy_vids` (`android.h:141-144`). ASUS (`0b05`) is **not** in either list.
- Auto-mapping is a manual action in input settings (`auto_mapping_device`, `SettingsFragmentPresenter.kt:858`).
- **Guess:** a fresh install does not bind a newly connected pad to player 1 without that step. Verify this on the device.
- Controller navigation of the Android UI is `utils/ControllerNavigationGlobalHook.kt`. It wraps every activity's `Window.Callback`, and the `INVERT_CONFIRM_BACK_CONTROLLER_BUTTONS` setting maps confirm/back. Core `Settings::values.controller_navigation` is the same idea for applets.

**Existing qlaunch path**
- `GamesFragment.launchQLaunch()` does three things:
  1. `NativeLibrary.getAppletLaunchPath(0x0100000000001000)` gives the qlaunch NCA path.
  2. `setCurrentAppletId(AppletInfo.QLaunch = 0x03)`.
  3. It navigates to `EmulationActivity` with a `Game(path = <NCA>)`.
- The button behind this is hidden unless `enable_qlaunch_button` is set and firmware is present.
- `InitializeEmulation` passes `FrontendAppletParameters{applet_id, launch_type}` to `System::Load`. It registers `RegisterApplicationChangedCallback` (a title launched from qlaunch reloads the disk shader cache) and `RegisterExecuteProgramCallback`.

### 1.2 Qt desktop (`E/src/yuzu`, `E/src/qt_common`, `E/src/frontend_common`)

**Entry**
- `yuzu/main.cpp` creates `MainWindow` (`yuzu/main_window.cpp`, 4,825 lines). The output binary is `eden` (`yuzu/CMakeLists.txt:242`).
- Command-line parsing is shared with the CLI: `Core::ParseLaunchParams` in `E/src/core/launch_params.cpp`.
  - `-qlaunch` sets `launch_qlaunch`, which calls `MainWindow::LaunchFirmwareApplet(QLaunch)`.
  - `-setup` launches the Starter.
  - `-hlaunch` and `-f` (fullscreen) also exist.
  - Handled in `main_window.cpp:518-548`.
- `LaunchFirmwareApplet` (`main_window.cpp:3915`) first calls `FirmwareManager::VerifyFirmware`, then boots the NCA from BIS.

**Shared helpers**
- `frontend_common/firmware_manager.*`: `InstallKeys`, `VerifyFirmware`, `CheckFirmwarePresence` (it checks MiiEdit, not qlaunch), `GetFirmwareVersion`. Firmware *installation* is not here.
- Firmware installation is Qt-only in `qt_common/util/content.cpp` (`InstallFirmware`, `InstallFirmwareZip`). The Android equivalent is in Kotlin: `utils/InstallableActions.processFirmware`, which unzips into the cache and copies to `nand/system/Contents/registered/`.
- Also in `frontend_common`: `config.cpp` (the shared ini reader/writer), `data_manager`, `play_time_manager`, `mod_manager`, `update_checker`.

**Game list and content**
- `qt_common/game_list/worker.cpp` scans the game directories. As it scans, it feeds files into the `FrontendManual` content provider (`AddEntriesFromContainer`).
- Android's `GameHelper.getGames()` does the same through `addGameFolderFileToFilesystemProvider`.
- This is what lets qlaunch and ns see the library (interface: map-core).

**Hotkeys**
- `qt_common/config/uisettings.cpp:33`, `default_hotkeys` (a fixed array of 33), currently binds:

  | Key | Action |
  |---|---|
  | F4 | pause |
  | F5 | stop |
  | F6 | restart |
  | F8 | change adapting filter |
  | F9 | GPU mode |
  | F10 | docked mode |
  | F11 | fullscreen |

- Each hotkey also has a controller chord (Home+X and so on), handled through `hotkeys.cpp`.
- Keyboard pad defaults are `QtConfig::default_buttons` (`qt_common/config/qt_config.cpp:12`). Home and Screenshot are 0 (unbound), and A is `C`; Harbor uses A = `Z`.

**Hardware buttons**
- HOME short/long and capture are polled in the **core**: `core/hle/service/am/button_poller.cpp` reads `GetHomeButtons()`/`GetCaptureButtons()` of handheld and player 1.
- `window_system.h` has empty `OnCaptureButtonPressed` and `OnPowerButtonPressed` (interface: map-core).

**Data location**
- `common/fs/path_util.cpp` uses:
  - `$XDG_DATA_HOME/eden`, `$XDG_CONFIG_HOME/eden` (`qt-config.ini`) and `$XDG_CACHE_HOME/eden`;
  - or `./user` when running portable.
- The directory name is `EDEN_DIR` in `common/fs/fs_paths.h`.
- Ryujinx's data directory is detected as a legacy path, and `common/fs/ryujinx_compat.cpp` links saves.

**eden-cli (`E/src/yuzu_cmd`)**: an SDL3 window that needs a file path. It has no `-qlaunch` and no host keys, so it is not a good console host.

### 1.3 The old OpenPak Eden fork (`F/`): what can be reused

The fork has 133 owner commits.

**For the host**
- `F/externals/openpak-client`: the C++ OpenPak client (session, friends, push, saves, network profile/ceiling). It is shared with Citron. Interface: map-services.
- `F/src/android/app/src/main/jni/openpak_native.cpp`: about 1,300 lines, one JSON-dispatch JNI entry, `Java_org_yuzu_yuzu_1emu_utils_OpenPak_*`. Worth keeping as the Kotlin↔openpak-client bridge pattern.
- `utils/OpenPak.kt`: initialisation in `YuzuApplication`.
- The fork stores tokens through `openpak::Platform::WriteFile`, as plaintext JSON. There is no Keystore.
- The X-OpenPak-Client naming (e701cd545a) and the crash-report consent (e2faa90c79) can be reused.

**Not for the console UI** (qlaunch replaces these screens)
- The fork's own Android OpenPak screens: `OpenPakFragment.kt` and `OpenPakUi.kt`, about 2,200 lines (2ecb995c35).
- The Qt OpenPak menu and windows: 2cbcb85625, 4a258f74d2, b59438e029 (shoulder buttons turn pages, a controller drives OpenPak windows). Only their controller-navigation idea transfers.

**Not present in the fork**: Android controller navigation work beyond Eden's own, Keystore, and console mode.

**Build state**: the fork's build outputs (`.cache`, `build-openpak`, `src/android/app/.cxx`, `build`, `artifacts`; about 10 GB) were deleted by someone else while this survey ran. `/home` went from 1.2 GB to 32 GB free. No warm native caches remain. `~/.gradle` (6.9 GB) is still there.

---

## 2. Feature table

`R` and `E` paths are as above.

- Status: **has** = present in Eden as needed; **partial** = present but needs changes; **missing**.
- Verdict:
  - **reuse** = use Eden's code with configuration or small edits;
  - **port** = carry Harbor's behaviour or code over;
  - **rewrite** = new code.
- Effort is in engineer-days. Confidence: H/M/L.

| Harbor feature | Ryujinx file(s) | Eden file(s) | Eden status | Verdict | Effort (conf.) | Risks |
|---|---|---|---|---|---|---|
| Startup gate: no completed setup or missing keys/firmware/qlaunch → wizard, else console | `R/src/Ryujinx.UI.Mobile/App.axaml.cs` (`ShowStartAsync`), `R/src/Ryujinx.Common/Configuration/HarborSetup.cs`, `R/src/Ryujinx.HLE/FileSystem/ConsoleInstaller.cs` (`CheckReady`) | `MainActivity.kt` (`FirstApplicationLaunch`), `SetupFragment.kt`, `NativeLibrary.areKeysPresent/isFirmwareAvailable/getAppletLaunchPath` | partial | rewrite (small) | 2 (M) | `CheckFirmwarePresence` looks for MiiEdit, not qlaunch; check qlaunch `0x0100000000001000` explicitly |
| Device check page: ABI, RAM, Vulkan, Adreno, API, storage | `R/src/Ryujinx.UI.Mobile/FirstRun/DeviceCheck.cs`, `AndroidHarborPlatform.Device` | `NativeLibrary.getGpuModel/getVulkanApiVersion`, `utils/MemoryUtil.kt`, `GpuDriverHelper.isAdrenoGpu` | partial (values exist, no page) | port | 1 (H) | Eden `mainline` minSdk 33 vs Harbor 29 (see risks) |
| Keys page: pick `prod.keys`, verify, install, never replace a good file with a bad one | `R/.../FirstRunViewModel.cs`, `ConsoleInstaller.InstallKeys` | `frontend_common/firmware_manager.cpp InstallKeys`, `NativeLibrary.installKeys`, `<user>/keys/` | has (install) / partial (staging) | reuse + port the staging | 1 (H) | Eden copies `prod.keys` and `title.keys` from the source folder; SAF stream must be copied to a temp file first |
| Firmware page: zip/xci/NCA folder, version check (warn unless 23.0.1), progress | `ConsoleInstaller.VerifyFirmwarePackage/InstallFirmware`, `FirstRunPages.cs` | Qt `qt_common/util/content.cpp InstallFirmware*`; Android `InstallableActions.processFirmware`; `FirmwareManager::GetFirmwareVersion` | partial (zip/folder; no xci; split by frontend) | rewrite into `frontend_common` (one `FirmwareManager::InstallFirmware(src, progress)` for both + JNI) | 3 (M) | xci firmware extraction is not in Eden's frontends (guess); interface: map-core for 23.0.1 boot |
| Games, DLC and updates folders | `HarborSetup.GameDirs/AutoloadDirs`, `R/src/Ryujinx.HLE/HOS/Services/Ns/HomeMenuScanner.cs` | Android `NativeConfig.setGameDirs`, `setExternalContentDirs` (`Settings::values.external_content_dirs`, `ext_content_from_game_dirs`); Qt `UISettings` game dirs; `GameHelper.getGames` feeding `FrontendManual` | has (storage) / partial (library registration happens only during a game-list scan) | reuse + headless scan before boot | 2 (M) | qlaunch tiles depend on what ns reads from `FrontendManual`. Interface: map-core (ns application records, icons, DLC/update merge) |
| Graphics driver page + GitHub catalogue (MrPurple, whitebelyash, K11MCH1, StevenMXZ; progress, X cancel, SHA-256, size cap, minApi, family filter, R8 exclusion) | `R/src/Ryujinx.UI.Mobile/FirstRun/GpuDriverCatalog.cs`, `GpuDrivers.cs`, `R/src/Ryujinx.Android/GpuDriverLoader.cs`, `docs/android-port/gpu-drivers.md` | `fragments/DriverFetcherFragment.kt`, `features/fetcher/*`, `model/DriverViewModel.kt`, `utils/GpuDriverHelper.kt`, `GpuDriverMetadata.kt`, `jni/native.cpp InitializeGpuDriver` | partial (fetcher exists, no checksum, different repositories, no family filter) | reuse loader/store; port catalogue rules into the Kotlin fetcher | 4 (M) | Eden loads the driver once at `Application.onCreate`, so a change needs a process restart (Harbor has the same limit) |
| "Ready" → boot the HOME menu | `App.axaml.cs ShowConsole`, `R/src/Ryujinx.Android/EmulationSession.cs Boot` | `GamesFragment.launchQLaunch()`, `EmulationActivity`, `EmulationSession::InitializeEmulation` | has (as a side feature) | reuse; make it the main path (§3) | 3 (M) | **Guess:** whether Eden's am keeps qlaunch resident while a title runs (interface: map-core) |
| Wizard look: 1280×720 canvas, Basic White/Black palette, mint focus, one page per step, progress dots, A/B/X footer, fixed primary action | `R/src/Ryujinx.UI.Mobile/FirstRun/FirstRunView.axaml(.cs)`, `FirstRunPages.cs`, `HarborIcon.axaml`; renders in `scratch/ryujinx-android/wizard-visual-20261006/renders/` | none (`SetupFragment` is Material pages) | missing | rewrite (Kotlin views on a scaled 1280×720 `FrameLayout`, or Borealis) | 10–15 Kotlin + Qt twin 5–8 (M) | Two UI stacks (Kotlin + Qt) unless the Borealis area's single C++ UI works on both. Interface: Borealis |
| Setup horn cue (180 ms, semitone per page), audio focus released before firmware audio | `R/src/Ryujinx.UI.Mobile/FirstRun/SetupChime.cs`, `R/src/Ryujinx.Android/SetupAudio.cs`, `HarborAudioFocus.cs`, `R/src/Harbor.Desktop/SetupAudio.cs` | none | missing | port (SoundPool on Android, a QSoundEffect/SDL3 equivalent on Qt) | 1 (H) | none of note |
| Landscape-only and hidden system bars throughout | `MainActivity` `SensorLandscape`, `App.RefreshSystemBars` | `EmulationActivity.enableFullscreenImmersive`; MainActivity is not locked | partial | reuse + manifest `screenOrientation` | 0.5 (H) | Picker return re-shows the bars; reapply on `onWindowFocusChanged` as Harbor does |
| Theme follows the console's Basic White/Black (before boot: the phone's) | `EmulationSession.ConsoleDarkTheme`, `IHarborPlatform.ConsoleDarkTheme` | `utils/ThemeHelper.kt` (Material, `theme_mode`) | missing | port | 1.5 (M) | Needs a native getter for set:sys `color_set_id`. Interface: map-core |
| Touch overlay: yuzu-derived controls, Harbor art and tokens, sliding between buttons, 8-way D-pad, edit toolbar, show/hide toggle, haptics | `R/src/Ryujinx.UI.Mobile/Pad/PadOverlay*.cs`, `PadLayout.cs`, `R/src/Ryujinx.Android/HarborPadView.cs`, `TouchController.cs` | `overlay/InputOverlay*.kt`, `OverlayControl.kt`, `OverlayScaleDialog.kt` | has (same yuzu base, Eden's art and editing) | reuse first; port Harbor's art and sliding later | 0.5 reuse / 6 full port (M) | Harbor's overlay is C#; porting means re-expressing it in Kotlin (it is the same lineage, so not a licence issue) |
| Joy-Con detach/attach: rail button during firmware setup, menu item, F7 | `EmulationSession.JoyConAttached`, `HarborPadView.SetRailLabel`, `R/src/Ryujinx/Headless/Windows/WindowBase.cs` F7 | `NativeConfig.getInputSettings` (`PlayerInput` type); no toggle | missing | rewrite: JNI `setPlayer1Style(handheld/dual)` + `HIDCore().ReloadInputDevices()` | 1.5 (M) | Interface: map-core/hid. Eden's `IsHandheldOnly()` decides the overlay port |
| Physical controllers: discovery, USB/BT, positional face buttons for every pad, Kunai VID 0b05/PID 7904, split Joy-Con halves as one pad, held-input release on pause | `R/src/Ryujinx.Android/PhysicalGamepad.cs`, `R/src/Ryujinx.UI.Mobile/Pad/PhysicalPadState.cs`, `ControllerButtons.cs` | `utils/InputHandler.kt`, `features/input/YuzuInputDevice.kt`, `input_common/drivers/android.cpp` | partial (label mapping except listed vendors; manual auto-map) | port the policy: positional by default, auto-bind the first pad to player 1 with no setup step | 3–4 (M) | ASUS is missing from `flipped_*_vids`; hid-nintendo vs hid-generic order on phones is unknown (Harbor docs say the same) |
| Overlay hidden while any controller is connected, restored on last disconnect | `PadOverlay.GamepadConnected` | `EmulationFragment.onPhysicalControllerStateChanged`, `HIDE_OVERLAY_ON_CONTROLLER_INPUT` | has | reuse, default the setting on | 0.5 (H) | Eden's version also requires `SHOW_INPUT_OVERLAY`; check that touch does not bring the overlay back |
| Plus+Minus opens the Harbor menu | `PhysicalGamepad.OnKey` chord | none (Back opens the drawer) | missing | rewrite (chord in `EmulationActivity.dispatchKeyEvent`, swallowed before the guest) | 1 (H) | The chord must not leak to the guest; Eden's Qt uses `L+Plus+Minus` for stop |
| Controller navigation of host panels: D-pad/hat/left stick, 350/110 ms repeat, A/Plus confirm, B/Minus back | `R/src/Ryujinx.UI.Mobile/Pad/GamepadNavigation.cs`, `ControllerPanel.cs` | `utils/ControllerNavigationGlobalHook.kt` | partial | reuse the hook + port the repeat/positional rules | 2 (M) | Android's default `BUTTON_A` = select puts A on the bottom button of a Nintendo-style pad |
| Harbor menu: Resume, Detach/Attach Joy-Con, Restart, Graphics driver, Show/Hide FPS, Log threads, Set up again; stopped screen with "Start again" | `App.axaml.cs:190-320`, `IHarborPlatform` | `menu_in_game.xml`, `EmulationFragment` drawer, `QuickSettings.kt` | partial (game-oriented drawer) | rewrite the menu contents; keep the drawer component | 2 (M) | Exit must close the app, not return to a game list |
| Show FPS | `EmulationSession.ShowPerformance` | `EmulationFragment` `showStatsOverlayText` (`SHOW_PERFORMANCE_OVERLAY`); Qt "Toggle Performance Overlay" Ctrl+V | has | reuse | 0.5 (H) | none |
| Log guest threads (Android menu, desktop F12) | `R/src/Ryujinx.HLE/HOS/Horizon.cs LogGuestThreads` | `core/arm/arm_interface.h LogBacktrace`, `KProcess::GetThreadList` | partial (pieces) | rewrite `System::LogGuestThreads()` + JNI + hotkey | 1.5 (M) | Interface: map-core |
| Desktop host keys: F8 HOME (short/long), F7 Joy-Con, F6 dock, F11 fullscreen, F12 thread dump, plus F4 capture, F5 power hold, F10 force-terminate | `R/src/Ryujinx/Headless/Windows/WindowBase.cs UpdateSystemKeys` (lines 501-620), `Horizon.UpdateHomeButton/UpdateCaptureButton/SimulatePowerButtonMiddlePress/ForceTerminateApplication/ChangeDockedModeState` | `qt_common/config/uisettings.cpp default_hotkeys`, `qt_config.cpp default_buttons`, `MainWindow::OnToggleDockedMode`, `ToggleFullscreen`, `core/hle/service/am/button_poller.cpp` | partial (dock/fullscreen exist on other keys; HOME/capture via the core poller) | reuse + rebind: Home=F8 and Screenshot=F4 in `default_buttons`; move dock to F6; new hotkeys for F7/F12/F10/F5 | 2.5 (M) | Clashes with Eden's defaults (F6 restart, F8 filter, F10 dock, F4 pause, F5 stop). Power/capture handlers are empty in `window_system.h` (interface: map-core) |
| Desktop launcher (wizard window, then the emulator with `--home-menu …`) | `R/src/Harbor.Desktop/Program.cs` | `main.cpp`, `LaunchParams.launch_qlaunch` | partial | rewrite as console mode inside `MainWindow` (§3.2) | 3 (M) | The game-list scan must still run hidden |
| Android audio (AAudio CFI crash fixed by a native callback bridge) | `R/src/Ryujinx.Audio.Backends.AAudio/Native/aaudio_bridge.c`, `R/scripts/build-aaudio-bridge.sh` | `audio_core/sink/cubeb_sink.cpp`, `sdl3_sink.cpp`; cubeb `mozilla/cubeb fa02160712` | has | reuse (the CFI issue was Mono-specific and does not apply to native callbacks) | 0.5 validation (M) | **Guess:** cubeb picks its AAudio backend on API ≥ 26. Headphone route changes are untested on both |
| Android Keystore secret storage for OpenPak tokens (AES-256-GCM, non-exportable key, ciphertext in the no-backup directory) | `R/src/Ryujinx.Android/KeystoreSecretBackend.cs`, `MainApplication.OnCreate` | none; the fork's openpak-client writes plaintext JSON (`F/externals/openpak-client/client/openpak/account.cpp:116`) | missing | port (Kotlin `KeystoreSecrets` + JNI callbacks behind an openpak-client `Platform` secret hook) | 3 (M) | Interface: map-services. Eden manifest has `allowBackup=true`; set false or exclude secrets |
| All-files access (`MANAGE_EXTERNAL_STORAGE`) for folders read by path | `R/src/Ryujinx.Android/MainActivity.EnsureAllFilesAccessAsync`, `AndroidManifest.xml` | `common/fs/fs_android.*` (`IsContentUri`, `OpenContentUri`), `DocumentsTree.kt`; manifest already declares `MANAGE_EXTERNAL_STORAGE` | has (SAF URIs work in the core) | reuse SAF; drop the all-files prompt | 0.5 (M) | SAF reads through JNI may be slower for big NSP/XCI (guess); keys and firmware go in through SAF streams |
| Signal handler / fault forwarding (Mono chaining race fixed 2026-10-06) | `R/src/Ryujinx.Cpu/Signal/UnixSignalHandlerRegistration.cs`, `NativeSignalHandler.cs` | `common/signal_chain.cpp` (libc `sigaction` directly), `core/arm/nce/arm_nce.cpp`, dynarmic `exception_handler_posix.cpp` | has (native, chains the old handlers) | reuse | 0.5 validation (M) | NCE + ART interaction on the ROG Phone 3 is unverified for Harbor's workload |
| 16 KiB page support / alignment | `core-integration.md` §2, `zipalign -P 16` checks | NDK r28 defaults to 16 KiB ELF alignment (guess for every target); `useLegacyPackaging=true` | likely has | verify | 1 (M) | Prebuilt `.so`s (FFmpeg CI build, downloaded validation layer) need `llvm-readelf -l` checks |
| Identity: app id `org.openpak.harbor`, label Harbor, lighthouse icon, coexistence with Eden | `R/docs/harbor/identity.md`, `harbor-icon.svg`, `R/scripts/generate-harbor-icon.py`, `R/src/Ryujinx.Android/Resources/*` | `app/build.gradle.kts`, `AndroidManifest.xml`, `res/mipmap-*`, `dist/dev.eden_emu.eden.*`, `common/fs/fs_paths.h EDEN_DIR` | missing (Eden branding) | rewrite (§4.3) | 2.5 (H) | Signing-key continuity with the current Harbor APK; GPL/AGPL notices |
| OpenPak host connection at boot (`server.json`, pinned CA, offline fallback) | `R/src/Ryujinx.OpenPak/OpenPakHostSetup.cs`, `R/src/Ryujinx.HLE/HOS/Services/Account/OpenPak/OpenPakBoot.cs` | `F/` openpak-client network profile | missing in upstream | port (interface: map-services) | n/a here | Must run before `InitializeEmulation` |
| Logging to logcat with a tag | `R/src/Ryujinx.Android/LogcatLogTarget.cs` | `jni/native_log.cpp`, `utils/Log.kt` | has | reuse | 0 | none |
| iOS | design goal only (`identity.md`: sideload + JIT) | only `OS::IOS` detection in `core/game_settings.cpp:99`; no iOS frontend, no Xcode project | missing | out of scope | n/a | Neither base has a validated iOS path |

Totals for the host and frontend, excluding core/services work and the wizard UI choice:
- About 30–40 engineer-days for Android plus Qt console mode, controllers, driver catalogue, identity and secrets.
- Add 15–23 for a native Kotlin + Qt wizard. If Borealis gives one C++ wizard for both, use the Borealis estimate instead.
- Confidence: M.

---

## 3. Console-mode design sketch

Goal: after the wizard, both apps boot straight into qlaunch. There is no game list, and the Harbor menu is the only host UI.

### 3.1 Android: minimal changes

1. **Launcher routing.** Add `HarborStartActivity`, or change the `MainActivity.onCreate` branch at `MainActivity.kt:106`. The process already ran `DirectoryInitialization.start()` and the driver load in `YuzuApplication`.
   - Off the UI thread, compute readiness:
     - `NativeLibrary.areKeysPresent()`;
     - `NativeLibrary.getAppletLaunchPath(0x0100000000001000).isNotEmpty()` (qlaunch present; don't rely on `CheckFirmwarePresence`, which checks MiiEdit);
     - `firmwareVersion()`;
     - and the setup record (§3.3) `completed == true`.
   - If not ready, start the wizard: a Kotlin `HarborSetupActivity`, or the Borealis activity. Wait for its result, then re-check.
   - If ready, go to step 2.
2. **Hidden library registration.** Run `GameHelper.getGames()` without showing `GamesFragment`. It already calls `clearFilesystemProvider()` and `addGameFolderFileToFilesystemProvider()` for every file under the game directories and the external content directories. This is Harbor's `HomeMenuScanner.Register` equivalent. Interface: map-core confirms ns/qlaunch read `FrontendManual` for tiles, icons, DLC and updates.
3. **Console preset.** Apply it before every boot with `NativeConfig.setX` + `NativeLibrary.applySettings()`:
   - every `*_applet_mode` in `common/settings.h:141-170` = `LLE`. Eden defaults `swkbd`, `web`, `controller`, `shop`, `login_share`, `wifi_web_auth` and `data_erase` to HLE; Harbor runs the firmware's own applets;
   - `use_docked_mode` = Handheld;
   - `HIDE_OVERLAY_ON_CONTROLLER_INPUT` = true;
   - `INVERT_CONFIRM_BACK_CONTROLLER_BUTTONS` per the positional policy.
   - In `EmulationSession::InitializeEmulation`, stop registering the Android host keyboard and web applets when in console mode (`SetFrontendAppletSet`, `native.cpp:300-311`).
   - Interface: map-core decides which applets truly need LLE.
4. **Boot.** Call `NativeLibrary.setCurrentAppletId(0x03)` and start `EmulationActivity` with `Game(title="HOME", path=<qlaunch NCA>)`. This is exactly `GamesFragment.launchQLaunch()`, called from the launcher. Then `finish()` the launcher so Back cannot reach a list.
   - Pin `EmulationActivity` to `sensorLandscape`.
   - Make it the task root: `excludeFromRecents` on the launcher and `clearTask` flags.
5. **EmulationActivity in console mode.**
   - Back and Plus+Minus open the Harbor menu (replace `menu_in_game.xml`).
   - "Exit" calls `finishAndRemoveTask()`.
   - Emulation stopping (the `OnEmulationStopped` callback) shows a stopped panel with "Start again" and "Set up again".
   - `RunEmulation` resets `m_applet_id` to Application on exit (`native.cpp:440`), so the restart path must call `setCurrentAppletId(0x03)` again.
6. **Remove or hide** for this build: `GamesFragment`, `HomeSettingsFragment` entries, `AppletLauncherFragment`, the updater (`ENABLE_UPDATE_CHECKER` stays OFF, drop `REQUEST_INSTALL_PACKAGES`), the multiplayer and lobby UI (`NetPlayDialog`, `LobbyBrowser`), the Lossless DLL page, and the `genshinSpoof` flavour. Keep `SettingsActivity` behind the Harbor menu's "Advanced" for driver and debug knobs.
7. **Interfaces.**
   - The HOME button from the overlay or a pad is a hid button read by `ButtonPoller`; no JNI is needed.
   - The capture/power hooks are core work (map-core).
   - Theme-follow needs a `getConsoleDarkTheme()` JNI reading set:sys (map-core).

### 3.2 Qt desktop: minimal changes

1. Add a `bool harbor_console` launch parameter (`--console`, in `core/launch_params.*`), defaulted on in the Harbor build. Alternatively make it implicit when `harbor-setup.json` is complete.
2. In `MainWindow` (`main_window.cpp:518-548`), console mode does the following:
   - Run the game-list worker without showing `game_list`. Its `AddEntriesFromContainer` registration is what matters. Keep `game_list` and `game_list_placeholder` hidden.
   - Apply the console preset (as on Android).
   - Call `LaunchFirmwareApplet(u64(AppletProgramId::QLaunch), std::nullopt)` once the scan finishes.
   - Go fullscreen if requested.
   - `OnEmulationStopped` (`main_window.cpp:2128`) shows a small stopped page ("Start again" / "Set up again" / "Quit") instead of `game_list->show()`.
   - Hide the menu bar and status bar by default, as a console would; the Harbor menu is a popup on a hotkey.
3. Hotkeys: edit `default_hotkeys` and `default_buttons` (§2 row). `Shortcut` arrays are fixed-size: change `std::array<Shortcut, 33>` in `uisettings.h:247` too.
4. The wizard runs before `MainWindow` when the record is incomplete or `--setup` is passed. It is a separate `QDialog`/window, or the Borealis program run as a child process the way `Harbor.Desktop` runs the emulator today. On "Start Harbor" it writes the contract (§3.3) and either returns into `MainWindow` or `exec`s `harbor --console`.

### 3.3 Hand-off contract for a separate wizard (Kotlin, Qt or Borealis)

The wizard owns no emulator state beyond these files. Everything is relative to Eden's user directory:
- Android: `getExternalFilesDir(null)` = `/storage/emulated/0/Android/data/org.openpak.harbor/files`.
- Desktop: `$XDG_DATA_HOME/harbor` after `EDEN_DIR` is renamed, with config in `$XDG_CONFIG_HOME/harbor`.

| Item | Wizard writes | Where Eden expects it | Who reads it |
|---|---|---|---|
| Record | `<user>/harbor-setup.json`, same schema as today: `keys_present`, `firmware_version`, `game_dirs`, `dlc_dirs`, `update_dirs`, `gpu_driver`, `completed`, with null = page not passed. Written through a `.tmp` + rename | new file; replaces Android's `FirstApplicationLaunch` preference | launcher gate (both) |
| Keys | `FirmwareManager::InstallKeys(dir, "keys")` (JNI `installKeys`) on a staged copy of the picked file | `<user>/keys/prod.keys` (+ `title.keys`); `areKeysPresent()` / `reloadKeys()` | core `KeyManager` |
| Firmware | `FirmwareManager::InstallFirmware(src, progress)` (new, moved from `qt_common/util/content.cpp` and `InstallableActions.kt`); verify version 23.0.1 and that qlaunch `0x0100000000001000` exists | `<user>/nand/system/Contents/registered/*.nca` (NAND directory from `nand_directory` in config) | BIS content provider |
| Game folders | Android `NativeConfig.setGameDirs(GameDir(uri, deepScan))` with SAF tree URIs; Qt `UISettings::values.game_dirs` | `config.ini` / `qt-config.ini`, `[Paths]` game dirs | game-list scan → `FrontendManual` |
| DLC + update folders | `Settings::values.external_content_dirs` (Android `NativeConfig.setExternalContentDirs`; Qt via the shared config); keep `ext_content_from_game_dirs=true` | `config.ini` array `external_content_dirs` | `ConfigureFilesystemProvider` / patch manager |
| GPU driver | Copy the zip into `<user>/gpu_drivers/`, set `driver_path` = that zip, `""` = system driver | Android-only setting `driver_path` (`android_settings.h:54`); extracted to `filesDir/gpu_driver/` at the next process start | `GpuDriverHelper.initializeDriverParameters` |
| Console preset | nothing; the launcher applies it every boot (§3.1 step 3) | `Settings::values` | core |
| Language/region/theme | nothing; the firmware's own first-time setup (Starter) asks | system save data in NAND | firmware |
| OpenPak | nothing (as today); the host connects at boot and writes `<user>/openpak/server.json` + `ca.pem` | interface: map-services | openpak-client |
| Exit code / intent result | Android: `RESULT_OK` from the wizard activity; desktop: exit 0 = boot console, other = quit | n/a | launcher |

Rule carried over from Harbor: nothing is written to the record until that page's Next succeeds. Resume judges from the files themselves, not only from the record.

A Borealis wizard needs C++ access to `FirmwareManager` and the config writer. Linking it against `frontend_common` and `core` keeps a single implementation; driving the wizard through JNI on Android is the alternative. Interface: Borealis evaluation.

---

## 4. Build notes

### 4.1 Where things land and why the checkout must move

These paths are hard-wired inside the source tree:

- The root `CMakeLists.txt:13` sets `set(CPM_SOURCE_CACHE ${CMAKE_SOURCE_DIR}/.cache/cpm)` as a **normal variable**. That shadows any `-DCPM_SOURCE_CACHE=…` cache entry and the environment variable. The CPM cache (about 1 GB) always ends up in `<source>/.cache/cpm` unless that line is patched.
- The Android validation layer is downloaded into `<source>/src/android/app/src/main/jniLibs/<abi>/` (`CMakeLists.txt:285-300`). Disable it with `-DYUZU_DOWNLOAD_ANDROID_VVL=OFF`.
- Gradle native intermediates (`src/android/app/.cxx`, 5.7 GB in the old fork) and `src/android/app/build` (3.2 GB) are in-tree. The APK/AAB copies go to `<source>/artifacts`.
- `emulators/` is on `/home`: 32 GB free now, 1.2 GB at the start of this survey.

So **build from a clone on the pool**, not from `emulators/harbor-eden`:

```sh
SCR=~/REPOS/Openpak/scratch/harbor-eden
git clone ~/REPOS/Openpak/emulators/harbor-eden "$SCR/src"   # local clone; objects copied (~116 MB); branch harbor
# or make the pool clone the canonical working copy and keep emulators/harbor-eden as a pointer
mkdir -p "$SCR/tmp" "$SCR/build" "$SCR/gradle-home"
```

The alternative, if the checkout has to stay on `/home`, is to patch `CMakeLists.txt:13` to `if(NOT DEFINED CPM_SOURCE_CACHE)` and set `android.buildDir` and `externalNativeBuild.cmake.buildStagingDirectory` to the pool. That touches more files and gains nothing.

### 4.2 Desktop (Linux, Qt)

Installed already: `qt6-base` 6.11.2, `qt6-svg`, `qt6-tools`, `cmake` 4.4.3, `ninja`, `sdl3` 3.4.16, `boost` 1.92, `glslang`. `ccache` is not installed. The full Arch list is in `E/docs/Deps.md:156`; check `qt6-multimedia`, `qt6-charts`, `catch2`, `enet`, `libzip`, `spirv-tools`, `vulkan-utility-libraries` and `libusb`.

```sh
cd "$SCR/src"
export TMPDIR="$SCR/tmp"
cmake -S . -B "$SCR/build/desktop" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DYUZU_TESTS=OFF -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF \
      -DYUZU_USE_BUNDLED_QT=OFF -DENABLE_QT_TRANSLATION=OFF
# CPM cache -> $SCR/src/.cache/cpm (pool), build tree -> $SCR/build/desktop (pool)
flock /tmp/openpak-build.lock nice -n 19 cmake --build "$SCR/build/desktop" -j12
# binary: $SCR/build/desktop/bin/eden  (bin/harbor after the rename)
```

- If GCC 16 warnings stop the build, add `-DENABLE_WERROR=OFF` (default ON, `CMakeLists.txt:83`). The old fork's log shows warnings but a successful link.
- Follow the existing workspace rules: refuse to build while an emulator runs, as `scratch/parity-build/build-eden.sh` does with `pgrep`, and keep one build at a time with `flock /tmp/openpak-build.lock`. Only the lock file sits in `/tmp`.

### 4.3 Android

Installed already: SDK `~/Android/Sdk` with NDK `28.2.13676358` and CMake `3.31.6` (exactly what gradle asks for), and JDK 21 (`/usr/lib/jvm/java-21-openjdk`; the old fork used it, and AGP 8.9 needs 17 or newer).

```sh
cd "$SCR/src"
export TMPDIR="$SCR/tmp"
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk PATH=/usr/lib/jvm/java-21-openjdk/bin:$PATH
export ANDROID_HOME=~/Android/Sdk ANDROID_SDK_ROOT=~/Android/Sdk
export GRADLE_USER_HOME="$SCR/gradle-home"          # writable cache on the pool
export GRADLE_RO_DEP_CACHE=~/.gradle/caches   # reuse the 6.9 GB modules-2 read-only (AGP 8.9.1 already there)
export NUM_JOBS=8
flock /tmp/openpak-build.lock nice -n 19 .ci/android/build.sh -t standard -b Release -DYUZU_DOWNLOAD_ANDROID_VVL=OFF
# -> $SCR/src/artifacts/app-mainline-release.apk (+ .aab); intermediates in $SCR/src/src/android/app/{.cxx,build}
```

- `GRADLE_RO_DEP_CACHE` must point at the directory that contains `modules-2`, which here is `~/.gradle/caches`. Gradle never writes to it.
- The Gradle 8.13 distribution (about 130 MB) downloads once into `$SCR/gradle-home/wrapper`. A copy is already in `~/.gradle/wrapper/dists/gradle-8.13-bin`; copy that folder over to skip the download.
- Pin `-Dorg.gradle.daemon=false` if daemons outliving the build lock become a problem. The memory note about `VBCSCompiler` holding the lock is the C# equivalent.
- `.ci/android/build.sh` always builds both the APK and the AAB (`copy…Outputs` depends on `bundle…`). For quicker loops, call `./gradlew assembleMainlineRelease -PYUZU_ANDROID_ARGS="…"` from `src/android` with the same environment.

**Build times**
- Recorded (old fork, incremental, `scratch/parity-build/*.log`): Android successful runs took 1 m 38 s – 4 m 02 s; failed runs took 1 m 36 s – 4 m 05 s. The desktop incremental relinked 6–453 objects.
- No cold build time is recorded anywhere. **Guess:** cold desktop 25–45 min at `-j12`; cold Android 40–70 min at 8 jobs.
- Disk per tree: about 1 GB CPM cache plus about 9 GB Android intermediates, plus the desktop tree. Keep about 20 GB free on the pool per checkout.

### 4.4 applicationId and package rename to `org.openpak.harbor`

1. In `src/android/app/build.gradle.kts`:
   - `defaultConfig.applicationId = "org.openpak.harbor"`.
   - Delete the `genshinSpoof` flavour, because it impersonates another app's package. Delete `legacy`/`chromeOS`, or rename them, for example `org.openpak.harbor.legacy`. `legacy` (`-DYUZU_LEGACY=ON`, minSdk 29) is the one built for Snapdragon 865-class GPUs, and the ROG Phone 3 is an 865+: decide whether Harbor ships that flavour.
   - Set `appNameBase` to "Harbor" and the `resValue` `app_name`/`app_name_suffixed` to "Harbor".
   - Remove the `play {}` publisher block.
   - `debug` and `relWithDebInfo` add `.debug`/`.relWithDebInfo` suffixes, which install side by side.
2. **Keep `namespace = "org.yuzu.yuzu_emu"`** and the Kotlin packages. JNI symbol names (`Java_org_yuzu_yuzu_1emu_*` throughout `jni/*.cpp`) and `common/android/id_cache.cpp` (9 `FindClass` paths) depend on them. Renaming is mechanical but large; do it later, if ever.
3. `AndroidManifest.xml`:
   - Change the action `dev.eden.eden_emulator.LAUNCH_WITH_CUSTOM_CONFIG`, together with `CustomSettingsHandler.kt:30`, to `org.openpak.harbor.…`.
   - Drop `REQUEST_INSTALL_PACKAGES`.
   - Set `allowBackup="false"` or exclude the secret files.
   - Set the console activity to `screenOrientation="sensorLandscape"`.
   - The provider authorities already use `${applicationId}`, so they won't collide with an installed Eden.
4. Strings: 20 "Eden" mentions in `res/values/strings.xml`, plus the translations.
5. Icons: replace `res/mipmap-*/ic_launcher*`, `ic_launcher_foreground/background/monochrome` and `drawable/tv_banner`. Generate them from `R/docs/harbor/harbor-icon.svg` by adapting `R/scripts/generate-harbor-icon.py` to write Android vector/PNG resources. For desktop, `dist/dev.eden_emu.eden.{desktop,metainfo.xml,svg}` become `org.openpak.harbor.*`.
6. Desktop: `common/fs/fs_paths.h` `EDEN_DIR "eden"` becomes `"harbor"`. `yuzu/CMakeLists.txt:242` `OUTPUT_NAME "eden"` becomes `"harbor"`. Update the window title and About text.
7. Signing: the installed Harbor (`org.openpak.harbor`, versionCode 2) is signed with a debug key. Very likely that is the .NET for Android debug keystore under `~/.local/share/Xamarin/Mono for Android/` (only its presence was checked, not its contents). An Eden-based APK with the same id installs over it **only** with the same key: pass it through `ANDROID_KEYSTORE_FILE`/`…_PASS`/`ANDROID_KEY_ALIAS`. Eden's `autoVersion` code (about 3×10⁷) is greater than 2, so the upgrade itself is allowed.
   - The data layouts differ (Ryujinx `bis/` vs Eden `nand/`), so nothing carries over except what is migrated deliberately.
   - Suggestion: use `org.openpak.harbor.eden` during the port and switch ids at cut-over.
   - The committed `app/debug.keystore` is the public Android debug key; never ship with it.

---

## 5. Top risks and unknowns, with de-risking steps

1. **qlaunch as a long-lived host process in Eden's am.**
   - Eden can *launch* qlaunch (Android button, Qt `-qlaunch`). Whether title launch, return to HOME and resume from qlaunch work in one session decides whether console mode is even possible.
   - Check: map-core's verdict first. Then, on desktop, run `eden -qlaunch` against an isolated data directory with firmware 23.0.1 and walk HOME → tile → HOME → resume.
2. **Library visibility to qlaunch.**
   - Tiles come only from what the frontend scan registers in `FrontendManual`. The Android scan is tied to `GamesViewModel`.
   - Check: call `GameHelper.getGames()` headlessly before boot and confirm the tiles, DLC and updates (map-core for the ns side).
3. **Two UI stacks for the wizard and host menu** (Kotlin and Qt). Harbor has one C# UI.
   - Check: wait for the Borealis evaluation. If Borealis renders on Android inside the same process with input from Eden's Kotlin controller path, one wizard serves both. If not, budget 15–23 days for Kotlin plus Qt.
4. **Controller semantics drift.** Eden maps by label except for listed vendors, ASUS is not listed, and auto-mapping is a manual step.
   - Check: a unit-level port of Harbor's `ControllerButtons.FromAndroid` table into `android.cpp`, plus a first-pad-to-player-1 auto-bind. Then repeat Harbor's Kunai procedure (`R/docs/android-port/controller-support.md` §Phone validation) on the ROG Phone 3.
5. **minSdk.** Eden `mainline` needs API 33; Harbor targets API 29+. The ROG Phone 3 runs Android 12 (API 31), so **mainline will not install on the primary test phone**. Only `legacy` (minSdk 29) will.
   - Check: build the `legacy` flavour first, or lower `mainline` minSdk and audit API-33-only calls (for example `POST_NOTIFICATIONS`).
6. **Signing and data continuity** with the installed Harbor. The key and layout mismatch is covered in §4.4.
   - Check: decide the test id now; write a one-off Harbor→Eden migration only for saves and profiles if the owner wants it. `common/fs/ryujinx_compat.cpp` is a starting point on desktop.
7. **HLE frontend applets bypassing firmware applets.** Eden registers Android keyboard and web host applets and defaults several applet modes to HLE, which would replace the firmware's own swkbd and web pages that Harbor relies on (OpenPak sign-in page).
   - Check: the console preset in §3.1 step 3, confirmed by map-core and map-services against the sign-in flow.
8. **Driver change needs a process restart** (load at `Application.onCreate`), and the catalogue rules differ (no checksum, different repositories).
   - Check: keep Harbor's rule ("used from the next start"), add a "Restart Harbor" action that kills the process, and port the `GpuDriverCatalog` filters and SHA-256 checks.
9. **Build disk pressure.** The cache paths are hard-wired into the source tree (§4.1).
   - Check: build only from the pool clone. Verify `df` before the first cold build: about 20 GB per tree.
10. **16 KiB devices.**
    - Check: after the first APK, run `llvm-readelf -l` on every `lib/arm64-v8a/*.so` (LOAD align `0x4000`) and `zipalign -c -P 16 4`. This matters most for the FFmpeg CI prebuilt and adrenotools hooks.
11. **Upstream drift.** Eden moves fast (the `harbor` branch is from 2026-10-05).
    - Check: keep Harbor changes in a few well-bounded files (launcher, console preset, hotkeys, identity) so rebases stay cheap. Never push upstream (owner decision above).
12. **iOS.** Eden has no iOS frontend, and nothing here moves Harbor closer to it.
    - Check: keep it a non-goal for phase 1. **Guess:** a JIT-sideload iOS port would need a new Metal/MoltenVK host either way. MoltenVK is already in `cpmfile.json` for macOS.
