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

## High-accuracy follow-up candidate

The RC3 device run still shows the artifacts with GPU accuracy confirmed High
and RADV active. No unsupported render-target-format warning recurred. Removing
that warning did not resolve the visual defect.

The log also records repeated NCE faults on a GPU-cached page. The old recovery
made the host page writable after 64 faults even when GPU tracking remained
active. The follow-up uses the existing memory-instruction interpreter for that
case, retaining tracked Memory reads/writes; uncached stale host protection keeps
its previous recovery. An unsupported instruction is reported through the normal
failed-fault path instead of granting untracked access. This changes recovery
behavior and needs a device retest. It does not fix the unexplained retained cache
reference itself.

High accuracy now honors NCE's preserve-GPU-writes request before invalidation,
including textures as well as buffers. Previously this required the separate,
default-off nce_invalidation_gpu_readback option and considered buffers only.
This is a correctness candidate, not confirmation of the artifact cause.

Shader buffer-tracking failures still use the existing fallback. Their warning
now identifies shader stage and IR opcode. No evidence yet justifies replacing
that fallback or changing driver features. A shader loaded from disk cache may
not be recompiled, so absence of a new warning alone does not demonstrate a fix.

See [firmware service startup](firmware-service-startup.md) for the independent
service failures and their investigation. Acceptance requires the same Splatoon
3 scene on S25 FE, High accuracy and the same RADV driver, with a new screenshot,
NCE/renderer logs, and verification that npns no longer dies at BGTC command 200.

Validation: `scripts/harbor/test-nce-interpreter.sh` exercises the actual NCE
interpreter/decoder against instrumented software-memory callbacks under ASan and
UBSan. Scalar stores across a page boundary, SIMD pair stores, loads, and rejection
of unsupported instructions pass. This does not simulate live GPU cache ownership
or the phone's signal delivery; those require the device acceptance run.

## RC8 shader fallback bounds candidate

The Vulkan global-memory fallback used the aligned host binding base for both
bounds comparisons, although the guest descriptor size starts at the original
address. For a guest base `0x1234000060`, size `0x180` and alignment `0x100`,
this admitted padding below the guest base and rejected the last `0x60` valid
bytes. Loads outside the test return zero; writes are discarded. The host buffer
binding already includes the alignment padding, so offsets still subtract the
aligned base while bounds now use the original guest base and its end.

`scripts/harbor/test-spirv-ssbo.sh` compiles the actual SPIR-V emitter and checks
all six generated load/store helpers with aligned and unaligned addresses above
4 GiB. Its 72 boundary checks pass with the change and fail with the previous
emitter. This establishes a shader bug, not that Splatoon 3 exercises this path
in the corrupted scene. RC8 device acceptance is pending. No driver features,
services or accuracy settings change in this candidate.
