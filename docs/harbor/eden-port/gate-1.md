# Gate 1 — repo, builds, first runs (2026-10-06)

## Builds

| Build | Time (cold) | Artifact |
|---|---|---|
| Desktop (Linux, Qt) | about 16 min at -j12, plus a 42 s relink | `build/desktop/bin/harbor`, 43.7 MB |
| Android legacy (minSdk 29) | 13 m 33 s | `harbor-eden-legacy-3cdca9cd1c.apk`, 21.3 MB, sha256 `5670d39b…6a76` |
| Android mainline (minSdk 33) | 13 m 21 s | `harbor-eden-mainline-3cdca9cd1c.apk`, 21.0 MB, sha256 `e3d7b29d…ec7e` |

- Both APKs pass `apksigner verify` and `zipalign -c -P 16`, and every native library has 16 KiB LOAD alignment.
- They are signed with Eden's public debug key, which is for test builds only.
- The Android native build is limited to 6 compiles and 2 links, because an unlimited run ran out of memory.

## Desktop: stock Eden `-qlaunch`, firmware 23.0.1, isolated data directory

Run from `scratch/harbor-eden/run/stock-qlaunch`: a portable `user/` directory, the firmware copied
from the owner's 23.0.1 zip, and the keys symlinked from `~/.switch`.

- qlaunch loads and renders the lock screen in about 3 s.
- The News panel shows **Eden's own release news** ("Eden v0.2.1"…), fetched from Eden's servers.
  This confirms the plan's finding that the built-in news must be deleted.
- The log shows one unimplemented command on the boot path: ns `IApplicationManagerInterface` 935
  (in the ledger). There are also two shader-environment and texture-format asserts in video_core.
- Input to a window on a hidden workspace did not reach the guest, and the frame counter stopped
  while hidden, so the walk past the lock screen is left to phase 2's driven runs.

Host problems found on the way, which phase 2 or 4 must fix:
- `-qlaunch` is swallowed by `getopt_long` on Linux (it is parsed as `-q -l -a …`). Workaround:
  `harbor -- -qlaunch`. Fix: a real `--qlaunch` long option, or console mode as the default.
- On first start Eden offers to **migrate another emulator's data** (Citron, yuzu, Ryujinx; the
  Ryujinx path can link saves). Harbor must never offer this. Remove the prompt in phase 1
  follow-up or phase 4.
- A "Wayland Detected!" prompt, defaulting to "Use X11", appears on every start until it is hidden.
  Remove it for Harbor.
- The Qt organisation and application names are still "eden".

## Phone: legacy APK on the ROG Phone 3 (Android 12, stock Adreno 650 driver 512.502)

- It installs beside the current Harbor as "Harbor" (`org.openpak.harbor.eden`); the existing Harbor
  is untouched. The owner completed Eden's own onboarding (keys, firmware 23.0.1, game folder).
- Eden's HOME button (`enable_qlaunch_button`, set over adb) boots qlaunch:

  | Event | Time from the tap |
  |---|---|
  | Lock screen | about 5 s, 60 FPS |
  | HOME with the bottom row | about 9 s, 44 FPS while compiling shaders |
  | Crash | about 19 s |

- The crash: on the **stock Qualcomm driver**, three graphics pipelines fail to build
  (`VK_ERROR_UNKNOWN`), then `vkCmdDrawIndexed` segfaults inside `vulkan.adreno.so` on the
  VulkanWorker thread. The next run needs Turnip (the owner's MrPurple build). Harbor's wizard
  already steers Adreno 6xx users to Turnip; the Eden Harbor must as well.
- Log: `scratch/harbor-eden/run/phone/stock-qlaunch-crash-29852.txt`.

## Still open for gate 1

- Rerun the phone timing on Turnip, including NetConnect and PlayerSelect against Harbor's 2.76 s and
  2.54 s (risk 2 in the plan).
- An API 33+ device for the mainline APK.
