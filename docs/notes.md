# Notes: harbor

<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

## Overview

Harbor is OpenPak's Nintendo Switch emulator, built on Eden (which descends from yuzu). It is an independent
project with its own name, direction and OpenPak integration. Its purpose is to let you play Switch games online
through OpenPak's replacement services, with an OpenPak account, friends, invitations, news and cloud saves.

Eden remains our upstream base and we keep merging applicable upstream work. Harbor is maintained by OpenPak
and is not an official Eden release.

**Status:** Alpha (online play verified on named titles, see below).

## Not affiliated

Harbor is an independent project. It is not endorsed by the Eden or yuzu projects, and it is not affiliated with
Nintendo. Nintendo Switch and all other trademarks belong to their respective owners.

## Local multiplayer

Local multiplayer over the home network is under development; see
[setup and the unverified device test cases](harbor/local-play.md). Mixed
Harbor/Switch Local Wireless requires a compatible `ldn_mitm` module on the Switch.

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
