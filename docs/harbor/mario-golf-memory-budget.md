# Mario Golf Vulkan allocation crash — 2026-10-13

ROG3 Android crash at 21:49 local was SIGABRT on the GPU thread: uncaught
VK_ERROR_OUT_OF_DEVICE_MEMORY in TextureCache::JoinImages during a Fermi2D blit.
The same root cause has not been established for the earlier PC slowdown.
Strikers later exited with SIGSEGV, with invalid texture-descriptor assertions
and no usable native backtrace; it is not a confirmed repeat of this failure.

The integrated GPU calculation previously allowed physical heap size to override
a smaller driver budget. It now bounds the limit by available budget with headroom.
Integrated-GPU budget changes update texture cleanup limits each frame. Cleanup
threshold floors cannot exceed a small heap's budget. Failed image allocations
log dimensions and numerical budget/usage; no account or player data is logged.
Driver selection and user graphics settings are preserved.

Small-budget, exhausted-budget, large-heap and shrinking-budget standalone checks
passed. Catch2 regression cases are in src/tests/video_core/memory_budget.cpp;
the full Catch2 target was not run. Android legacy ARM64 release build passed.
The signed test APK was installed in place on ROG3 with adb install -r; no data
cleared and no game launched by the agent. Original APK is retained privately in
scratch/mario-golf-retest-20261013/harbor-before-memory-fix.apk for rollback.
Candidate SHA-256: 4ab7bd15cb484004f2d28449043d4f5535a68aa2b5ff911c442da23412eedcc2.
Artifact: scratch/harbor-eden/artifacts/harbor-eden-legacy-2df397d240.apk.
This includes existing checkout changes. Real-game acceptance remains pending;
do not call the allocation failure resolved solely from these checks.
