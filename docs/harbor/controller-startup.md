# Android controller startup crash — 2026-10-13

A ROG 3 crash at 10:27:46 local time enters NPad::ControllerUpdate from the
Android virtual gamepad motion callback, before usable applet resources exist.
The fault is a null-pointer dereference at address 0x188. NPad registers
callbacks in its constructor, whereas SetNpadExternals installs the applet
resource afterward. The callback previously dereferenced that resource even
for Motion, which its switch ignored after dereferencing.

ControllerUpdate now discards events it does not handle before touching
resources. Connected, Disconnected and Battery updates also return while the
applet resource is unwired. Periodic updates still consume motion/button data.
This addresses the observed startup path, not every possible controller
lifecycle race. No callback locking or teardown semantics were changed.

Validation: the changed npad.cpp object compiled for Android ARM64/API 29 using
the existing legacy CMake tree under the Harbor build lock. The log is in the
workspace scratch/ryujinx-android/rog3-nobody-20261013/hid-android-compile.log.
No APK containing this guard has been packaged or installed. Preserve the
existing, unrelated shader work when preparing an isolated APK build.

The earlier crash at 10:27:33 is a separate VulkanWorker fault inside the
selected custom driver, libvulkan_freedreno.so. The following launch also
reported GPU device loss. Controller guarding does not resolve those failures.
Raw captures remain outside the repository in the same scratch directory.
