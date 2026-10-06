# Borealis for the Harbor setup wizard: evaluation and proof of concept

2026-10-06. Workspace: `scratch/harbor-eden/research/borealis/`. Nothing in ryujinx-android, eden, harbor-eden or
docs was changed. No phone or adb was used. Every PoC window ran on a hidden special workspace and was closed.

## Recommendation (short)

**Use Borealis, specifically the XITRIX fork, vendored at a pinned commit, as the one shared wizard for desktop,
Android and later iOS.** It meets every hard requirement. Each one has a stated condition, and the Android ones still
need checking on a phone (below). The owner wants one implementation for all platforms, and that settles a cost
comparison that is otherwise roughly even.

Conditions:
- Replace Borealis' bundled fonts. It ships Nintendo's system fonts.
- Pin SDL3 to Eden's exact version.
- Run the wizard once per process.
- Apply three small fixes (scaling, a focus route, an Android build define), described below.

Fallback: native Kotlin plus Qt wizards. Switch to it if the phone test shows that `SDLActivity` and Eden's
activities cannot share the process or the SDL copy (§6 option B also fails), or that controllers mis-map.

## 1. Fork choice — evidence

| Fork | Last commit (default branch) | Commits since 2025-10-06 | Backends | Android | iOS | Notes |
|---|---|---|---|---|---|---|
| natinusala/borealis (original) | 2021-04-05 | 0 | GLFW (Switch/PC) | no | no | dead |
| xfangfang/borealis (`wiliwili`, used by wiliwili) | 2026-04-25 | 6 | GLFW, **SDL2** | SDL2 + GLES2 (wiki) | SDL2 + GLES (wiki), not shipped by wiliwili | wiliwili v1.6.0 assets: Switch, PS4, PSV, Linux, macOS, Windows. No Android or iOS builds |
| dragonflylee/borealis (`switchfin`, child of xfangfang) | 2026-09-22 | 17 | SDL2/GLFW | switchfin ships arm64/armv7 APKs | – | SDL2 |
| **XITRIX/borealis** (`moonlight_wiliwili`, used by Moonlight-Switch) | **2026-07-26** (`8c1f1093`) | **69** | **SDL3** (2026-07-10 "SDL3 backend", "SDL3: Android"), SDL2, GLFW, native Metal (2026-04), D3D11 (2026-05), Deko3D | **SDL3 + GLES2/3** (vendored SDL 3.4.10, shared `libSDL3.so`) | **SDL3 + GLES3 or Metal**; iOS/tvOS/visionOS targets in CMake | Moonlight-Switch v1.5.0 ships `moonlight-app-release.apk`. Its README offers iOS/tvOS/macOS through TestFlight and credits Borealis for the iOS port |

**Choice: XITRIX/borealis.** It is the only maintained fork on SDL3, which is the SDL Eden uses on desktop and on
Android ("[android] Replace Oboe audio with SDL3 audio", `aa4e494c0`). It is also the only fork with a shipped iOS
path. The xfangfang/dragonflylee forks are SDL2 on Android. Their `org.libsdl.app` Java glue would clash class for
class with Eden's SDL3 glue in one APK. That rules them out unless they are ported.

Risks:
- Bus factor is one: nearly all recent commits are by XITRIX, and the repo has 37 stars.
- The head commit's Android build is broken (rumble symbols clash because `PLATFORM_ANDROID` is never defined).
  The PoC works around this.

Mitigation: vendor it as a subrepo at a pinned commit and own the patches.

## 2. The checks

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Maintained fork, runs on Android, path to iOS | **PASS** (XITRIX) | Table above. The PoC APK builds with SDL 3.4.10 + GLES2 for arm64. iOS: SDL3 + GLES3/Metal CMake targets, and Moonlight-Switch ships them through TestFlight |
| 2 | Coexistence with Eden Kotlin (Android) and Qt (desktop) | **PASS with conditions**; device behaviour **UNKNOWN** | See §6. Eden statically links SDL3 into `libyuzu-android.so` (bundled `sdl3-ci` 3.4.14 on Android, `release-3.4.8` source on desktop) and compiles SDL's Java glue from that same tree (`build.gradle.kts` srcDir `externals/generated/sdl/java`). SDL's `SDLActivity` refuses to start on a C/Java version mismatch (`SDLActivity.java:393-399`). There can be only one `org.libsdl.app` in the dex. So the wizard must use **the same SDL3 version** as Eden. **Two SDL copies in one process** would bind the Java natives (`Java_org_libsdl_app_*`) to whichever library loads first, so they are not allowed. Either one SDL copy (option A) or a separate process (option B). Borealis keeps static state and its demo `System.exit`s in `onDestroy`, so one wizard run per process |
| 3 | Input: controller + touch, A confirm, B back, X skip, Y secondary; Xbox vs Nintendo; landscape | **PASS** for keyboard (run on desktop). Controller: **PASS by code**, no pad on the desktop and untested on a phone. Touch: **UNKNOWN** (not exercised) | Keyboard, run: arrows move focus and scroll; Enter = A (with a passing check it exits 0, "next page: Keys"); Esc/Backspace = B; X logs "nothing to skip"; Y switches the theme; A is refused while a check fails. Controller: Borealis maps SDL3's positional buttons as Xbox semantics (`SDL_BUTTONS_MAPPING`, south = A). `Application::setSwapInputKeys(true)` swaps A/B and X/Y, which gives the Switch's by-position layout: right A, bottom B, top X, left Y. The swap is global; there is no per-controller label detection. An "Xbox labels" option would just be the swap turned off. The Joy-Con `Generic.kl` caveat from first-run.md applies to SDL's Android path too. Touch: `TapGestureRecognizer` on rows, the action button and the footer hints. Borealis hides the focus ring in touch mode, which matches the design ("ring only after key/pad"). Landscape: `sensorLandscape` in the manifest. The 1280x720 canvas scales uniformly in any window (see the scaling note) |
| 4 | Themes: light and the Switch charcoal dark | **PASS** | Both palettes copied from `FirstRunView.axaml`. They switch live with Y. Screenshots below |
| 5 | Licences vs AGPL-3.0-only | **PASS after removing assets**; **FAIL as shipped** | Borealis' `resources/font/switch_font.ttf` is "nintendo_udsg-r_std_003" and `switch_icons.ttf` is "NintendoExt003": Nintendo system fonts. `resources/img/pokemon` is Nintendo IP. The PoC ships neither: Noto Sans (OFL-1.1) loads as `font/font.ttf` (Borealis' `USER_FONT_PATH`), and button glyphs are drawn with nanovg. Everything else is permissive and AGPL-compatible (table §3) |
| 6 | APK size and startup cost | **Measured** (size, desktop startup, phone up to the r1 abort); first frame on the phone **pending** (r2) | r2 APK 6,176,280 B, ~2.3 MB gzip-compressed. Desktop first frame 0.68–0.99 s. Phone (r1, owner's run): activity in 231 ms, GL context ~0.67 s after process start. Details §5 |

Design-fidelity findings (all handled in the PoC, none blocking):
- **Scaling**: this fork scales by width only (`contentWidth = 1280`, so a 2340x1080 phone gets a 1280x591 canvas).
  `harbor::fitCanvas()` caps `setMaximumUIScale(min(w/1280, h/720))` on every resize, which gives the design's uniform
  fit with paper at the sides.
- **Focus**: a NATURAL `ScrollingFrame` keeps D-pad focus inside it, so the central action could not be reached.
  Fixed with custom navigation routes.
- **Focus ring**: the default ring pulses. The `SIMPLE_HIGHLIGHT` build option gives the static 3-unit mint ring.
  The lit row uses the theme's `brls/highlight/background`.
- **Pickers**: SDL3 offers file pickers on desktop (portal) and Android (SAF `ACTION_OPEN_DOCUMENT`).
  `SDL_ShowOpenFolderDialog` is unsupported on Android (`src/dialog/android`), so the games/DLC/updates folder picks
  need a small JNI `ACTION_OPEN_DOCUMENT_TREE` + all-files-permission bridge.
- **Accessibility**: Borealis has no TalkBack support (native views would). This is a soft cost.

## 3. Licence table

| Component | Licence | Used by PoC | Note |
|---|---|---|---|
| borealis (XITRIX fork) | Apache-2.0 | yes | Compatible with AGPL-3.0 (one-way, GPLv3 family). NOTICE kept (`poc/NOTICE`) |
| Borealis `resources/font/*.ttf` | Nintendo proprietary | **no** | Must never ship; replaced by Noto Sans |
| Borealis `resources/img/pokemon`, `img/sys` | Nintendo IP / unclear | **no** | Demo only |
| Material Icons (`resources/material`, Google 2015) | Apache-2.0 | **yes** (since r2) | Borealis loads it unconditionally when it creates the window; shipped with its LICENSE.txt |
| Borealis `i18n/en-US/hints.json` | Apache-2.0 | yes | |
| SDL 3.4.10 (+ Java glue, HIDAPI) | zlib | yes | Must equal Eden's SDL version in a shared APK |
| nanovg, fontstash | zlib | yes | |
| stb_image, stb_truetype | public domain / MIT | yes | |
| Yoga | MIT | yes | |
| {fmt} 12.1.0 | MIT | yes | |
| tweeny | MIT | yes | |
| tinyxml2 | zlib | yes | |
| libretro-common | MIT | yes | |
| libromfs | MIT | yes (Android) | Host generator built separately |
| glad (generated GL loader) | public domain / Apache-2.0 (Khronos spec) | yes | |
| nlohmann/json | MIT | headers | |
| GLFW | zlib | no (SDL3 used) | |
| switch-libpulsar | MIT | no | Switch only |
| Noto Sans Regular 2.015 | OFL-1.1 | yes | `poc/resources/font/OFL.txt` |
| PoC code | AGPL-3.0-only | – | `poc/LICENSE` |

## 4. Proof of concept

Source: `research/borealis/poc/` (local git repo, one commit `8e35022` authored by tobagin, not pushed).
- `src/device_check.cpp`: `DeviceCheck.cs` thresholds and strings.
- `src/device_probe.cpp`: the probes.
- `src/setup_page.cpp`: the page.
- `src/main.cpp`: options, timing.
- `android/`: Gradle project.

Borealis checkout: `research/borealis/src/borealis-xitrix` (`8c1f1093`, SDL submodule `ba6ec4d`). Also cloned for
comparison: `src/borealis-xfangfang`.

The page follows the current Harbor design, the `wizard-visual-20261006` renders:
- header with emblem (vector from `HarborIcon.axaml`), "Device check" and 7 step dots;
- lead line;
- rows with verdict disc, title, reason and blue value;
- warn/fail strip;
- central Next button, greyed while a check fails;
- footer hints `(Y) Dark/Light` (PoC-only theme switch), `(B) Back`, `(A) Next`.

The 1280x720 canvas scales uniformly. `--sample phone|warn|blocked|desktop` reproduces the design's states. Warn and
blocked were checked visually: the strip scrolls into view, Next is disabled and A is refused.

Which checks are real:
- **Desktop (run):**
  - Processor: build architecture, the same source as Harbor's process architecture.
  - Memory: `sysconf` physical pages.
  - Storage: `statvfs` of `~/.config/Ryujinx`.
  - With `--vulkan`, a real Vulkan probe through `dlopen("libvulkan.so.1")` reported "Quadro P1000, 1.4".
  - Without the flag the desktop shows only processor, memory and storage, as the design's desktop variant does.
- **Android (built, not run):** all six rows read real sources:
  - `ro.product.cpu.abilist` (= `Build.SUPPORTED_ABIS`);
  - `sysconf` memory (= `MemoryInfo.totalMem`);
  - Vulkan 1.0 instance, first device;
  - `ro.hardware.vulkan`/`ro.hardware.egl` = adreno;
  - `ro.build.version.sdk`/`release`;
  - `statvfs` of the app's internal storage.
- An arm64-only APK cannot install on an armeabi-v7a phone, so the Processor fail row can only ever be seen with
  `--sample`.

**Desktop: ran, yes.** Screenshots (own window only, `grim -T <stableId>`):
- `research/borealis/shots/device-check-light.png`
- `research/borealis/shots/device-check-dark.png`

Logs: `research/borealis/run-*.log`. Build: `research/borealis/build-desktop/harbor_setup`, 2.7 MB (2.2 MB stripped),
plus 0.6 MB of resources, linking the system SDL3 3.4.16.

**Android r1 on the phone (owner's run, ROG Phone 3, API 31; log `research/borealis/phone/logcat-all.txt`, pid 14263):**
- Install OK. Cold start 231 ms to the activity.
- SDL3 is up. GLES 3.2 runs on the stock Adreno 650 driver. Orientation LandscapeLeft/Right, surface 2340x1080.
- The Vulkan probe ran before the window opened (Adreno Vulkan driver loaded, 22.944–22.950).
- It then **aborted on SDLThread about 0.55 s in**: `std::invalid_argument: Invalid romfs resource path for
  'harbor_setup' : material/MaterialIcons-Regular.ttf`.
- Cause: Borealis' `FontLoader::loadMaterialFromResources()` calls `romfs::get` without a catch. libromfs throws for
  a missing path, and the PoC had not shipped the font.
- The desktop build never showed this because it loads from files (`access()` check, no throw).

**r2 fix (rebuilt through the lock, minSdk 29):**
- `poc/resources/material/MaterialIcons-Regular.ttf` (Apache-2.0, with LICENSE.txt) is now shipped.
- `poc/CMakeLists.txt` refuses to configure unless every resource Borealis loads unconditionally exists:
  `font/font.ttf`, `material/MaterialIcons-Regular.ttf`, `i18n/en-US/hints.json`.
- `main.cpp` fixes the locale to en-US. It wraps Borealis in a `try` so a missing resource ends with a logged
  `harbor: setup stopped: …` instead of SIGABRT. Check results and startup timing now go through Borealis' logger,
  so they reach logcat (tag `borealis`).
- Verified:
  - `libharbor_setup.so` now embeds `font/font.ttf`, `font/OFL.txt`, `i18n/en-US/hints.json`,
    `material/LICENSE.txt` and `material/MaterialIcons-Regular.ttf` (3,682,704 → 3,826,928 B).
  - A desktop build with `-DUSE_LIBROMFS=ON` (`research/borealis/build-desktop-romfs`) loads everything through the
    same `@res/` path as Android. It ran, drew the page and exited 0 on A.

**`@res`/romfs audit** (every `romfs::get`/`romfs::list` in the fork):
- **Throws if missing:**
  - Material font (`core/font.cpp`, fixed by shipping it);
  - `romfs::get` of i18n files (only paths returned by `romfs::list`, so safe);
  - XML views (`view.cpp`, `box.cpp`) and `Image` (`image.cpp`): the PoC uses neither; they must ship whatever XML or
    images a page names;
  - GLFW window icon and Switch cursor: not built.
- **Caught:** `DesktopFontLoader::loadFont` (`font/font.ttf`, `font/icon.ttf`, `font/switch_icons.ttf`).
  - `font/icon.ttf` is deliberately absent. It is Borealis' button-glyph font, and its only shipped implementation is
    Nintendo's NintendoExt003. The page draws its A/B/X/Y glyphs with nanovg and uses no Borealis view that needs it.
  - The two "Cannot find custom icon / Failed to load internal icons" warnings are therefore expected and harmless.
  - If Borealis' own hint bar is ever used, it needs a free replacement font mapping the same private-use code
    points.
- **`romfs::list("i18n/<locale>")`:** returns empty for a missing locale and logs an error. The fixed en-US locale
  avoids that.

**"Using zh-Hans font: /system/fonts/NotoSansCJK-Regular.ttc"** is intended Borealis behaviour, not a locale
choice:
- On Android, `DesktopFontLoader` always adds the system Noto CJK font as a fallback after the regular font, for
  Chinese glyphs, whatever the locale (the log shows "Set app locale: en-US" before it).
- On the phone it took about 25 ms. Note that nanovg's fontstash loads the whole font file into memory (no
  `BRLS_FONTSTASH_STREAM`), and that .ttc is tens of MB.
- For an English-only wizard it is unnecessary. Harbor should patch it out of the vendored fork (or enable
  `BRLS_FONTSTASH_STREAM`). The PoC leaves it, so the fork is used unmodified.

**Android: r2 APK built, not yet run on the phone.**
- **r2** path: `research/borealis/apk/harbor-setup-poc-arm64-r2.apk`.
- Size: **6,176,280 bytes**. SHA-256 **`63da9c2b1e9fe419b604f34e39b8c99e6b62844cc7e30adb0e0b543e9441868d`**.
- r1 (aborts on the phone, kept for the record): `harbor-setup-poc-arm64.apk`, 6,032,056 B, SHA-256
  `65c96df6…8044ee5cac`.
- Package `org.openpak.harbor.setuppoc`, **minSdk 29** (installs on Eden's legacy flavour devices, API 29+),
  targetSdk 36, arm64-v8a only, `sensorLandscape`.
- Release build with R8, signed v2 with the build's own debug key. `apksigner verify` passes, and 16 KiB zipalign
  passes.
- Contents (r2, stored uncompressed for page-aligned loading): `libharbor_setup.so` 3.83 MB (Borealis, nanovg, Yoga, fmt
  and the 0.6 MB text font plus the 0.13 MB icon font in libromfs), `libSDL3.so` 2.23 MB, `classes.dex` 93 KB.
- Toolchain: AGP 8.9.1 and Gradle 8.13, the same as Eden; NDK 28.2; CMake 3.31.6.
- GRADLE_USER_HOME and ANDROID_USER_HOME are on the pool (`research/borealis/gradle-home`, 618 MB). Logs:
  `research/borealis/android-build.log` (r1) and `android-build-2.log` (r2).
- Fork bug hit: Android link failure, worked around in `poc/CMakeLists.txt` with `-DPLATFORM_ANDROID`.

All heavy builds ran through `heavy.sh`: emulator check, then `flock /tmp/openpak-build.lock nice -n 19`. One build
waited about 3 minutes while Ryujinx was running.

## 5. Size and startup

- **APK**: 6.2 MB standalone (2.3 MB compressed download).
- **Inside Eden with option A** (one SDL, §6), the added cost is about the 3.7 MB setup library minus its share of
  fmt/SDL, roughly +3 MB uncompressed, of which 0.6 MB is the font. Option B adds the full ~6 MB.
- **Desktop startup**, timestamped in `main`, six runs:
  - first frame 682 / 790 / 822 / 866 / 877 / 990 ms after `main`;
  - before `main`: 10–110 ms;
  - device probe: 0.1 ms;
  - building the page: 1.4–3.3 ms.
  - Nearly all the time is Borealis/SDL window and input initialisation. The log shows about 0.6–0.7 s between "GL
    Version" and "joystick num". It is the same with `SDL_JOYSTICK_HIDAPI=0`, and it was not attributed further.
- **Android, measured by the owner on the ROG Phone 3 with r1:**
  - cold start 231 ms to the activity;
  - both libraries load in about 3 ms (22.883–22.886);
  - `SDL_main` starts 128 ms after process start;
  - the GL context is ready at +0.67 s. It waits about 0.5 s for the window to gain focus and resize to full screen
    (22.958 → 23.465);
  - input init 16 ms;
  - CJK fallback font 25 ms.
  - First frame not reached (r1 aborted). Expect it around 0.7 s after process start; to be confirmed with r2.

## 6. Integration design

Common to both platforms:
- The wizard owns `<data>/harbor-setup.json`, the record from first-run.md: `keys_present`, `firmware_version`,
  `game_dirs`, `dlc_dirs`, `update_dirs`, `gpu_driver`, `completed`.
- It installs keys and firmware through Eden's own code: `frontend_common::FirmwareManager::InstallKeys` /
  `VerifyFirmware`, and the Android `NativeLibrary.installKeys` / `installFileToNand` equivalents.
- It writes `completed: true` last.
- The emulator side then boots HOME with those folders.
- "Set up again" relaunches the wizard. Because Borealis' static state cannot be rebuilt, every run is a fresh
  process or a process restart.

**Android, option A (recommended, one SDL):**
- Build Borealis into `libyuzu-android.so` from Eden's CMake. Borealis accepts an existing `SDL3::SDL3` target, so it
  uses Eden's bundled SDL3. One SDL copy and one `org.libsdl.app`. Add Borealis' `PlatformUtils`/`BorealisHandler`
  to that package, or patch the JNI class name. Also add libromfs and the font.
- `HarborSetupActivity extends SDLActivity` with `getLibraries() = {"yuzu-android"}`.
- A tiny Kotlin launcher activity (or `MainActivity.onCreate`) reads the record and readiness:
  `NativeLibrary.areKeysPresent`, `isFirmwareAvailable`, HOME present.
  - If anything is missing, it starts `HarborSetupActivity` and finishes.
  - On A at Ready, the native side writes the record and returns from `SDL_main`. The activity's `onDestroy` then
    starts Eden's console activity (`EmulationActivity` with the HOME menu) instead of `System.exit`.
- "Set up again" schedules the setup activity and restarts the process.
- Must be checked on the phone: SDL's activity teardown (`nativeQuit`) must not leave SDL unusable for Eden's SDL3
  audio, which starts later at emulation start.

**Android, option B (fallback, separate process):**
- `HarborSetupActivity` with `android:process=":setup"` and its own `libharbor_setup.so`, which statically contains
  Borealis and SDL built from **exactly Eden's SDL version**.
- Installs go to the main process through a bound `HarborInstallService` (Messenger/AIDL) calling `NativeLibrary`.
- The wizard writes the record, starts the console activity in the main process, and `System.exit`s.
- Cost: +~6 MB APK, plus IPC glue.

**Desktop (Qt):**
- No embedding: Borealis owns its SDL window, GL context and main loop, and cannot sit inside a QWidget.
- Run it **in the same `eden` executable before `QApplication` is created**:
  1. `main()` checks the record and readiness.
  2. If setup is needed (or `--setup` is passed), it runs the Borealis loop with Eden's SDL3 (already linked for input
     and audio) and calls `frontend_common`/core directly for keys and firmware.
  3. It tears SDL video down and continues into Qt.
  4. Qt boots HOME with the record's folders.
- "Set up again" in the Qt menu re-execs `eden --setup`.
- A separate `harbor-setup` binary sharing the same SDL3 shared library works too. It would need its own link to
  `frontend_common`/core for verification.

**iOS later:** the same C++ wizard on SDL3 with GLES3 or Metal, through Borealis' `PLATFORM_IOS`, run before the
console view.

## 7. Effort estimate (one developer, working days)

| Work | Borealis, shared | Native Kotlin + Qt |
|---|---|---|
| Build integration (vendor fork, Eden CMake on Android/desktop, romfs, font, the three patches) | 2–3 | – |
| Flow, record and resume logic (port of `FirstRunViewModel`/pages) | 3–4 (once, C++) | 4–6 (Kotlin + C++/Qt, twice) |
| 8 pages in the Switch look, scaling, focus, sound cue | 5–7 (once) | Android 6–8 (Eden's `SetupFragment`/`SetupPage` exist but are Material-styled) + Qt 5–7 |
| Pickers and permissions (SDL dialogs; Android folder-tree JNI + all-files permission) | 3–5 | 2–3 (SAF/QFileDialog native) |
| Keys, firmware and driver install/download hooks | 3–4 | 2–3 (Eden already has `NativeLibrary`/`frontend_common` and the driver manager) |
| Hand-off and "set up again", both platforms | 2–3 | 1–2 |
| Device testing (phone, controllers, desktop) | 3–5 | 3–5 |
| **Total now** | **21–31** | **23–34** |
| iOS later | +3–5 (packaging, Metal/GLES check) | +7–10 (a third UI) |
| Ongoing | one UI, own a single-maintainer fork | two (later three) UIs to keep identical |

## 8. Recommendation

No hard requirement fails, and the owner wants one implementation. So: **Borealis (XITRIX fork, pinned and vendored
as a subrepo, with the Harbor patches carried locally) for the entire wizard on desktop, Android and later iOS.**

- Android: integrate with option A (in `libyuzu-android.so`, one SDL); desktop: the in-binary pre-Qt run.
- Ship only free fonts (Noto/Inter), never Borealis' `resources/font` or `img`.
- Next step: install r2 on the ROG Phone 3 (API 31, legacy flavour). r1 already confirmed install, SDL3, GLES 3.2
  on Adreno 650 and landscape. Check:
  - first-frame time;
  - immersive full screen;
  - touch on rows and hints;
  - a Pro Controller and a Joy-Con against the by-position mapping;
  - the six real probe values.
- Fall back to native Kotlin + Qt only if:
  - the phone shows a blocking problem, such as `SDLActivity` and Eden's SDL audio not surviving each other in one
    process when option B is also rejected, or wrong controller mapping that SDL hints cannot fix; or
  - the fork stops being maintained before Harbor can carry it.

## Phone session r2 — ROG Phone 3 (ASUS_I003D, Android 12/API 31, Adreno 650, stock driver), 2026-10-06

Run over USB with the owner's permission; r2 APK sha256 63da9c2b…868d.

- Install and launch: PASS. `am start -W` cold 314 ms / warm 150 ms to the activity; inside the
  process: probe 9 ms, Borealis init + window 532–549 ms, first frame 545–562 ms after main.
- Rendering: PASS. GLES 3.2 on Adreno 650, landscape 2340×1080 full screen with no system bars,
  1280×720 canvas scaled ×1.5 (content 1560×720), crisp text, light and dark themes.
- Checks: all six read real device values (64-bit ARM, 12 GB, Vulkan 1.1 Adreno 650,
  "Phone's or Turnip", Android 12, 255 GB free).
- D-pad navigation (KEYCODE_DPAD_DOWN): PASS — focus moves Processor → Memory.
- Touch: PASS with real-length taps. `input tap` (down/up within one frame) is ignored by
  Borealis' gesture recogniser; a 120 ms press (`input touchscreen swipe x y x y 120`) works:
  the "Y Dark" hint toggles the theme live and Next activates (the PoC then exits as designed).
  Touch hides the focus ring (Borealis touch mode), as expected.
- Gamepad: UNTESTED — no controller was connected. `adb input keyevent KEYCODE_BUTTON_Y` did not
  toggle the theme; injected keyevents do not come from a gamepad source, so this is not
  evidence either way. A real Kunai/Pro/Joy-Con session is still needed.
- One harmless log line per start: BLASTBufferQueue rejects the first buffer sized for portrait
  (1001×2214) before the landscape resize.
