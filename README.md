<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

# Harbor

Harbor is OpenPak's Nintendo Switch emulator, built on Eden (which descends from yuzu). It is an independent
project with its own name, direction and OpenPak integration. Its purpose is to let you play Switch games online
through OpenPak's replacement services, with an OpenPak account, friends, invitations, news and cloud saves.

Eden remains our upstream base and we keep merging applicable upstream work. Harbor is maintained by OpenPak
and is not an official Eden release.

**Status:** Alpha (online play verified on named titles, see below).

## Not affiliated

Harbor is an independent project. It is not endorsed by the Eden or yuzu projects, and it is not affiliated with
Nintendo. Nintendo Switch and all other trademarks belong to their respective owners.

## What works today

- Linux desktop: the firmware's own HOME menu, setup and applets boot on firmware 23.0.1 and games launch.
  This is the best-tested path.
- Android (legacy build, ROG Phone 3, Android 12): firmware 22.5.0 setup and applets run. Firmware 23.0.1 on a
  phone is still pending. Windows and macOS are not tested (see Downloads).
- Harbor runs ten of the firmware's own system modules (friends, news, parental controls and others) as guest
  processes instead of emulating them. News was fetched and followed on both desktop and phone.
- Online play checked against OpenPak servers, with the retail Switch on the other side:
  - Full Metal Furies (version 00050000): two-player co-op between Harbor and a Switch. Creating and joining a
    session, NAT punch-through and a clean leave all worked. Three or four players and a reverse host are untested.
  - PlateUp! 1.5.2: the Switch hosts and Harbor connects to the session. Sustained restaurant gameplay and Harbor
    hosting are not verified.
  - Go-Go Town!: the Switch hosts, Harbor joins, both players play together. Reverse hosting is untested.
  - Minecraft: Nintendo Switch Edition 1.0.17: worked in both directions between a Switch and Harbor on Android,
    as reported by the maintainer. No independent trace was kept.
- Heroes of Hammerwatch and Monopoly are served and were tested on two machines, not recorded as Harbor.
- Animal Crossing: New Horizons island restore through the game's recovery flow worked on Android, as reported
  by the maintainer. Restoring onto a physical Switch is not yet tested.
- Not verified: everything not listed above. A game that launches is not evidence that it works online.

Local multiplayer over the home network is under development; see
[setup and the unverified device test cases](docs/harbor/local-play.md). Mixed
Harbor/Switch Local Wireless requires a compatible `ldn_mitm` module on the Switch.

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

## Known limitations

- GPU drivers: on a ROG Phone 3 (Adreno 650) Mainline Turnip v32 crashes Nobody Saves the World shortly after
  launch, while Turnip v26.3.0-R5 boots it with visual artifacts. An unresolved Turnip VulkanWorker fault and a
  Vulkan out-of-memory case in Mario Golf are also recorded. Try another driver before reporting a bug. Eden's
  list of vendor driver bugs is in `docs/DriverBugs.md`.
- The BCAT key patch that lets news articles download has to be installed by hand today. Harbor does not yet
  ship it on every platform.
- News received during a session only shows after the next boot. The log stops at 100 MiB.
- The friend page shows "Offline" without a duration, and push notifications from the news service are not yet
  confirmed to reach the Switch side.
- The inherited user handbook (`docs/user`) is Eden's text and is not yet rewritten for Harbor.

The OpenPak forks of Ryujinx and Citron are retired; Harbor is the Switch emulator to use.

## Contributing and support

Report bugs and request features at https://github.com/openpak/harbor/issues. Report security problems
privately to the OpenPak maintainers through the contact on https://openpak.org, not in a public issue.

The most useful help is testing real titles on your own console, firmware and keys, with a note of the Harbor
version, platform and GPU driver, and what you saw, plus driver crash reports from varied GPUs. `CONTRIBUTING.md`
is inherited from Eden and still describes Eden's channels and donation links, which do not apply to Harbor. Send Harbor changes and issues here, not to Eden.

## License and acknowledgements

Harbor preserves the upstream licensing and copyright notices. The Eden codebase is GPL-3.0-or-later, with yuzu
code under GPL-2.0-or-later, and bundled components keep their own terms. See `LICENSE.txt` (GNU GPL version 3),
the `LICENSES` directory and the notices in each source file. Files that carry Harbor's own header are
AGPL-3.0-only.

Thanks to the Eden and yuzu contributors and to the other projects credited in the inherited source. Renaming the
fork does not change that provenance. Upstream Eden is at https://git.eden-emu.dev/eden-emu/eden.
