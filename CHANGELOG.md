# Changelog — Harbor (OpenPak Switch emulator, based on Eden)

Only OpenPak's changes are listed; everything else is upstream Eden. `git log` stays the
source of truth. Tags are `vX.Y.Z`; rc tags never build.

## v0.3.5 — 2026-11-04

- Account: authorization request shared with the title, Nintendo Account fingerprint, open
  context returns the application manager
- nsd/set: NAS service settings answered empty and logged; nim told to skip application
  authentication on shop access
- OpenPak connect retried on a fresh socket, also for timeouts; brightness persists

## v0.3.4 — 2026-11-03

- Account id_token and LoadOpenContext fixes

## v0.3.3 — 2026-11-02

- HOME-menu emulator on real firmware modules; OpenPak network profile and sign-in
