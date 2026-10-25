<!-- SPDX-FileCopyrightText: 2026 tobagin -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Local multiplayer over the home network

Status: implementation under test. Protocol tests are synthetic; real Switch/Android
lobby discovery and gameplay are not yet verified. Do not describe this as supported
until the acceptance checks below have been run.

When a game opens Local Wireless without Harbor being connected to an emulator room,
Harbor uses the home network. It broadcasts discovery and keeps session membership
on TCP using the `ldn_mitm` protocol. The game sees the host's real LAN address;
gameplay sockets use the existing host network backend. There is no OpenPak relay
or matchmaking request for this discovery/session transport.

If Harbor is already connected to an emulator room when Local Wireless initializes,
it keeps the existing room transport. Leave the room and exit/re-enter the game's
local mode to switch to the home network. Room and home-network traffic are not mixed.
OpenPak's online-service redirect setting is independent of this choice.

## Devices

Use separate devices on the same IPv4 subnet. Avoid a guest Wi-Fi network with
client isolation. Each installation needs its own identity and each game needs a
compatible update. The implementation supports one local-wireless session per host
IP address, up to seven joining devices (subject to the game's smaller limit).
Allow inbound UDP and TCP port 11452 plus the game's own gameplay ports on desktops.
Android enables broadcast/multicast reception while the emulation activity is visible
and releases the Wi-Fi lock when the activity pauses.

- Harbor hosting or joining Harbor: both use this build and leave emulator rooms.
- Switch hosting or joining Harbor: install and enable an `ldn_mitm` release matching
  that Switch's Atmosphère version, then reboot. Both devices use the game's Local
  Wireless option. Harbor itself does not need a sysmodule.
- Switch hosting or joining Switch: two ordinary Switch local-wireless sessions can
  communicate natively. For the same home-network mode used with Harbor, enable
  compatible `ldn_mitm` on both. A native local-wireless peer cannot see a LAN-bridged peer.

Get the Switch module from its [upstream releases](https://github.com/spacemeowx2/ldn_mitm/releases)
and check the [Atmosphère version table](https://github.com/spacemeowx2/ldn_mitm#version-table).
It is a separate installation, not included in Harbor's APK. Do not assume a version
from another console or install a mismatched binary.

For the owner-reported Atmosphère 1.12.1, upstream nightly
[run 40157473777](https://github.com/spacemeowx2/ldn_mitm/actions/runs/40157473777)
was prepared as a test candidate. Its log pins Atmosphère libraries to
[cf48d23](https://github.com/Atmosphere-NX/Atmosphere-libs/commit/cf48d23375cf36742876f4b014971b2ebabc6dc8),
which declares version 1.12.1. Its original `sdcard` artifact SHA256 is
`6f9fbdf3701cea69e93db1fa535098a8fb1c8e37f5797cbb5ec68b2fad8a02c1`;
archive integrity and NSP/NRO headers were checked. It has not been boot-tested
on the owner's Switch. The tagged release table only lists through 1.11.1.

## ARMS

Use the game's **Local** mode with the bridge. Host on the Switch first, then reverse
roles. Match the game update on both devices.

ARMS also has a distinct native **LAN Play** mode, introduced in update 1.1. At the
main menu hold the left-stick button and press L + R (not ZL/ZR); Local changes to
LAN Play. This mode uses ordinary sockets rather than the local-wireless transport
added here, so test and report it separately. Harbor's left-stick click must be mapped.
[Nintendo's instructions](https://www.nintendo.com/en-gb/Support/Nintendo-Switch/How-to-Use-the-LAN-Play-Feature-of-ARMS-1516623.html)
explain lobby creation and joining.

## Acceptance still required

Run each case with matching game versions, first with ARMS:

- [ ] Harbor host, Harbor join.
- [ ] Switch host with `ldn_mitm`, Harbor join.
- [ ] Harbor host, Switch join with `ldn_mitm`.
- [ ] Switch host and Switch join, both using `ldn_mitm`.

For each case, record both builds, firmware/module versions and network connection;
verify discovery, joining, sustained gameplay, leaving, host closure and rejoining.
Repeat with OpenPak enabled. Keep native Switch-to-Switch Local Wireless and ARMS
native LAN Play results separate from bridge results. Seeing the lobby is insufficient.

Harbor logs `Local wireless transport: home LAN (ldn_mitm)` when this mode starts.
If a port cannot be opened, initialization returns a connection error and logs the
host error rather than silently presenting an empty room list. Host connection failures
or missing admission updates also return failure rather than success.

## Why not just boot the real firmware's ldn module?

The guest module still needs WLAN scanning, connection, beacons and frame transfer.
Harbor's `wlan.cpp` currently leaves those handlers unimplemented; its real-firmware
selector deliberately defaults to logic modules without hardware dependencies.
Selecting `ldn` alone does not implement that lower layer or grant Android access to
the Switch's radio protocol. A real-module WLAN backend is separate future work;
this change makes no claim that it has been implemented or tested.

## Validation and protocol provenance

Run `scripts/harbor/test-local-wireless.sh` from the workspace or checkout. It compiles
a standalone test into the workspace scratch directory and uses loopback sockets.
It checks independent golden wire frames, malformed/compressed input bounds,
UDP reception, both TCP roles, rejoining, fragmented/coalesced frames, EOF, refusal,
port conflicts and shutdown releasing ports before destruction.
These checks do not execute a game or the physical Switch's module.

Wire facts were checked against `spacemeowx2/ldn_mitm` revision
`9a2639eff5b2e60065be3dfae69c46de6c3d7648`:

- [lan_protocol.hpp](https://github.com/spacemeowx2/ldn_mitm/blob/9a2639eff5b2e60065be3dfae69c46de6c3d7648/ldn_mitm/source/lan_protocol.hpp):
  12-byte little-endian header, magic 0x11451400, opcodes Scan=0, ScanResp=1,
  Connect=2, SyncNetwork=3.
- [lan_protocol.cpp](https://github.com/spacemeowx2/ldn_mitm/blob/9a2639eff5b2e60065be3dfae69c46de6c3d7648/ldn_mitm/source/lan_protocol.cpp):
  optional zero-run encoding; a zero and count represent count+1 zeros. Uncompressed
  transmission is supported. Header fields store encoded and decoded lengths.
- [lan_discovery.hpp](https://github.com/spacemeowx2/ldn_mitm/blob/9a2639eff5b2e60065be3dfae69c46de6c3d7648/ldn_mitm/source/lan_discovery.hpp)
  and [lan_discovery.cpp](https://github.com/spacemeowx2/ldn_mitm/blob/9a2639eff5b2e60065be3dfae69c46de6c3d7648/ldn_mitm/source/lan_discovery.cpp):
  UDP scan/reply and TCP admission/synchronization on port 11452; NodeInfo on connect,
  NetworkInfo on scan response/sync; TCP closure signals departure.
- [ldn_types.hpp](https://github.com/spacemeowx2/ldn_mitm/blob/9a2639eff5b2e60065be3dfae69c46de6c3d7648/ldn_mitm/source/ldn_types.hpp):
  the node's IPv4 field is a little-endian integer; wire structures match Harbor's
  existing 0x40-byte NodeInfo and 0x480-byte NetworkInfo.

The transport and decoder are independently written from these protocol facts; no
upstream implementation is copied or vendored. Harbor sends uncompressed frames,
bounds decode/output sizes, serializes peer IO and closes sessions on TCP EOF.
