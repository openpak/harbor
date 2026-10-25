# Deploy: harbor

## Connecting to OpenPak

On first launch Harbor's setup wizard walks through a device check, your keys, firmware, games, optional DLC and
updates, an Android graphics driver choice, an OpenPak account page, and an opt-in crash report question.

The account page offers Sign in, Create account, or Later. Choosing Later keeps Harbor offline and nothing is
linked. If you sign in or create an account, the console's first user is linked to it. Accounts with two-step
verification cannot finish sign-in on this page yet; choose Later and link a user in the console's settings.
Create an account at https://openpak.org.

You must supply everything copyrighted yourself, from your own console:

- `prod.keys` dumped from your Switch.
- Firmware dumped from your Switch, as a zip or folder of NCA files. Firmware 23.0.1 is the tested version on
  desktop and 22.5.0 on Android.
- Your own legally dumped games, plus any updates and DLC.

OpenPak provides no keys, firmware or games, and never will. Harbor never downloads any.

## Downloads

Releases are published at https://github.com/openpak/harbor/releases. The current release is v0.3.2, marked as a
pre-release, with a Linux x86_64 archive and two Android ARM64 APKs (legacy for Android 10 and newer, and
mainline). Builds are for testing and are signed with a test key. Only the legacy APK has been run on a device.

There are no Windows or macOS releases. A manual compile-check workflow exists for both, but nothing is built or
published from it, so treat those platforms as unreleased.

## Building from source

Harbor follows Eden's build system. Start with the General Build Guide (`docs/Build.md`), the dependency guide
(`docs/Deps.md`) and, for Android, `docs/build/Android.md`. A typical Linux desktop configuration:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF \
          -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF \
          -DENABLE_QT_TRANSLATION=OFF
    cmake --build build

The desktop executable is `harbor`. Android builds come from Gradle in `src/android`, with the `legacy` and
`mainline` flavors (for example `assembleLegacyRelease`). You need NDK 27 or newer. OpenPak's own code lives in
`src/core/openpak` and `src/harbor_setup`. Some build targets, file names and assets still carry Eden names.
