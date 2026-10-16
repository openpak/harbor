<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

# Harbor

Harbor is OpenPak's Nintendo Switch emulator, built on the Eden codebase.
It is an independent project with its own name, direction and OpenPak
integration. This marks our departure from maintaining an OpenPak-branded
Eden build: the project is now Harbor.

**Eden remains our upstream base.** We acknowledge and preserve the work of
Eden, yuzu and their contributors, and can continue integrating applicable
upstream improvements. Harbor is maintained by OpenPak; it is not an official
Eden release and is not presented as endorsed by the Eden project.

## OpenPak direction

Harbor is the active Switch emulator for OpenPak development and online
compatibility work. Its direction is integration with OpenPak's accounts,
friends and replacement online services. Support varies by title and feature;
a successful emulator launch does not establish online compatibility.

The OpenPak Citron and Ryujinx forks, including Ryujinx Android, are retired.
Do not use those forks for current OpenPak development, builds or testing.

## Source and development

- Harbor source and issue tracking: [openpak/harbor](https://github.com/openpak/harbor).
- Upstream base: [Eden](https://git.eden-emu.dev/eden-emu/eden).
- Workspace checkout: `emulators/harbor`.
- Build instructions: [General Build Guide](docs/Build.md).
- Inherited user documentation: [User Handbook](docs/user/README.md).
- Contribution guidance: [CONTRIBUTING.md](CONTRIBUTING.md).

Some inherited build targets, filenames, assets and documentation still use
Eden identifiers. These reflect the codebase's origin; the OpenPak project is
Harbor. Follow local handoff documents for the current implementation status.
Send Harbor-specific reports and changes to Harbor's repository.

## Releases

Harbor releases belong to [openpak/harbor](https://github.com/openpak/harbor/releases).
Eden's releases are upstream Eden builds and should not be described as Harbor
releases. Refer to the Harbor repository for available builds.

## License and acknowledgements

Harbor preserves the upstream licensing and copyright notices. See
[LICENSE.txt](LICENSE.txt), [LICENSES](LICENSES) and individual source-file
notices for the applicable terms. The Eden codebase is GPL-3.0-or-later;
components retain their own license terms.

Thanks to the Eden and yuzu contributors and the other projects credited in
the inherited source. Renaming the fork does not change that provenance.
