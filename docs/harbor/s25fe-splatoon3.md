<!-- SPDX-FileCopyrightText: 2026 tobagin -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Splatoon 3 on S25 FE: investigation

Observed 2026-10-25 on Xclipse 940. Samsung proprietary Vulkan 25.3.3 runs
were terminated by Android low-memory killer. Android exit-info and logcat
agree on LOW_MEMORY/SIGKILL; this is not evidence of a native driver crash.
The driver advertised about 5 GiB of budget even while system available RAM
was below 768 MiB. The game was at 0.75x handheld resolution.

The owner clarified that RADV produces artifacts, while Samsung's driver
crashes. A later RADV 26.99.99 (MAGELLAN CARL) run was verified and a screen
capture showed stretched 3D geometry and horizontal corruption with readable
UI. Harbor repeatedly logged render-target format 0. This does not yet
identify the rendering bug or prove that the driver causes it.

Candidate changes:

- On integrated Android GPUs with memory-budget reporting, constrain the cache
  budget using current MemAvailable and current GPU usage, retaining 512 MiB
  for the OS. The sample is cached for one second. An unavailable reading keeps
  the existing driver budget. This encourages earlier existing cache cleanup;
  it cannot prevent all Android kills or reclaim live guest memory.
- Include caller function/file/line in unsupported render-target-format
  diagnostics. Do not guess a valid format for 0 or suppress the warning.

Budget arithmetic checks cover reserve, zero availability, the driver ceiling
and overflow. Changed Android ARM64 translation units compile. No candidate
APK has been installed and no artifact fix is claimed. The next distributed
build must use a tag, as requested by the owner. Acceptance needs a repeated
same-scene run to measure memory/exit behavior and capture the diagnostic
caller. Keep driver, resolution, cache and accuracy changes separate so the
comparison remains useful.

## RC2 clear-path correction

RC1 diagnostics locate format 0 calls in RasterizerVulkan::Clear. Framebuffer's
HasAspectColorBit used rt_map for unbound slots, whose zero initialization aliases
the first image. RC2 first checks the render-pass key for an actual color binding.
This prevents a clear of an absent slot from being issued against image zero.
Regression checks cover sparse bindings, depth-only, all-bound and out-of-range
slots. Artifact resolution is pending a same-scene test on the S25 FE.
