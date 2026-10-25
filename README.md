# Harbor Emulator

OpenPak's Harbor Emulator.

| | |
| --- | --- |
| Kind | Emulator |
| Console | Nintendo Switch |
| Platforms | Linux x86_64, Android ARM64 |
| Download | [Releases](https://github.com/openpak/harbor/releases) |
| Based on | [Eden](https://git.eden-emu.dev/eden-emu/eden), GPL-3.0-or-later |
| Licence | As upstream, see [LICENSE.txt](LICENSE.txt) |

## What it does

- Lets you play Switch games online through OpenPak's replacement services, with an OpenPak account, friends, invitations, news and cloud saves.
- Runs the firmware's own HOME menu, setup and applets from a firmware dump you supply, and ten of the firmware's system modules as guest processes.
- Is built on Eden, which descends from yuzu. Harbor is maintained by OpenPak and is not an official Eden release.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF \
      -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF \
      -DENABLE_QT_TRANSLATION=OFF
cmake --build build
```

Android builds come from Gradle in `src/android`. See [deploy](docs/deploy.md).

## Docs

- [Protocol](docs/protocol.md)
- [Evidence](docs/evidence.md)
- [Deploy](docs/deploy.md)
- [Notes](docs/notes.md)
- Other files already in `docs/`:
  - [Build](docs/Build.md)
  - [Caveats](docs/Caveats.md)
  - [CODEOWNERS](docs/CODEOWNERS)
  - [CPMUtil](docs/CPMUtil.md)
  - [CrossCompile](docs/CrossCompile.md)
  - [Debug](docs/Debug.md)
  - [Deps](docs/Deps.md)
  - [DesignOverview](docs/DesignOverview.md)
  - [Development](docs/Development.md)
  - [DriverBugs](docs/DriverBugs.md)
  - [HosKernel](docs/HosKernel.md)
  - [NvidiaGpu](docs/NvidiaGpu.md)
  - [Options](docs/Options.md)
  - [README](docs/README.md)
  - [Settings](docs/Settings.md)
  - [creator-1.png](docs/img/creator-1.png)
  - Folders: [build](docs/build/), [dynarmic](docs/dynarmic/), [harbor](docs/harbor/), [policies](docs/policies/), [user](docs/user/)

## Licence

Harbor keeps the upstream licence notices. The Eden code is GPL-3.0-or-later, see [LICENSE.txt](LICENSE.txt) and the [LICENSES](LICENSES) directory. Files that carry Harbor's own header are AGPL-3.0-only.
