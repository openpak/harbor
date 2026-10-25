# Xclipse custom Vulkan drivers on Android

Harbor recognises Samsung Xclipse 530, 540, 550, 920, 940, 950 and 960 on
Android 14+ ARM64. Both Samsung's Xclipse device names and RADV's GPU codenames
are recognised. This enables driver selection; it is not a claim that games
have been verified on each phone.

Setup, HOME, per-game settings and the Android driver manager can select an
imported RADV Xclipse ZIP or the system driver. GitHub browsing on Xclipse lists
JimVulkan/radv-xclipse releases from 1.7 onward, the first release supporting
all seven models. Wayland packages are excluded; use the regular Android ZIP.
Adreno keeps its existing publishers and Freedreno controls.

1. Open the graphics driver page and browse GitHub drivers, or import the regular
   ZIP from https://github.com/JimVulkan/radv-xclipse/releases.
2. Select RADV Xclipse and restart Harbor. Clear the test game's shader cache
   after changing drivers.
3. Verify the actual driver in Harbor's GPU-driver checks. For RADV diagnostics,
   run `adb shell setprop debug.radv_xclipse_log 1`, launch the game, and look for
   the `[BUILD] Mesa` line in logcat. Turn diagnostics off with
   `adb shell setprop debug.radv_xclipse_log 0`.
4. Compare with the system driver and retain logs under workspace `scratch/`.

The upstream v1.8.0 notes report untested ray tracing on the 940 and broken ray
tracing on the 950/960, including possible phone crashes on the 960. After a GPU
hang, reboot before retesting; use the system driver if the issue repeats.

Sources: https://github.com/JimVulkan/radv-xclipse/releases/tag/v1.8.0

The setup regression suite passes all 126 checks, including every supported Xclipse model,
RADV codenames, Android/ABI gates, package-family checks and Android-only downloads.
The Android legacy release APK also builds successfully.
Device acceptance is pending. No Xclipse phone was used to validate this change.
