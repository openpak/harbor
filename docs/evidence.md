# Evidence: harbor

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
