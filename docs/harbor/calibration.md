# Harbor calibration (PRODINFO / set:cal)

Every Harbor NAND has its own calibration binary, the way every Switch has its own PRODINFO
partition. Code: `src/core/harbor_calibration/` (`cal0.*` = format and generator, `calibration.*`
= per-NAND storage and the device key), `src/core/hle/service/set/factory_settings_server.cpp`
(`set:cal`), `set:sys` GetSerialNumber / GetBatteryLot. Self-check:
`src/core/harbor_calibration/check/calibration_check.cpp` (build line in its header).

## Who reads set:cal on firmware 23.0.1

Modules whose NPDM grants `set:cal` (firmware audit, `all/<tid>/exefs/main.npdm`): settings (09,
the server itself), bluetooth (0B), ptm (10), hid (13), wlan (16), nvnflinger (1C, NPDM only), am
(23), ssl (24), nim (25), pctl (2E), es (33), omm (45), and the FS KIP. **No applet holds it**:
qlaunch, starter, the settings scene, MyPage, the shop and every other applet have no `set:cal`
in their NPDM. In Harbor all of those system modules are HLE, so a HOME run makes no `set:cal`
call at all; every run log on this machine (am-*, acc-*, social-*, olsc-*, shop-*, ...) has
no unimplemented `set:cal` line. The consumers are therefore Harbor's own HLE code, through the
C++ API (`Harbor::Calibration::Get()`), and `set:cal` itself is answered for completeness.

qlaunch's System Settings shows the serial under System → Serial Information (between Zoom and
Important Information), with the battery lot: the scene calls `set:sys` GetSerialNumber (68) and
GetBatteryLot (67) when it opens, and both answer from the calibration. In 23.0.1 qlaunch the
`set:sys` proxy for command 68 is the vtable slot at +0x220 (thunk 0x31dbe0); the aborting wrapper
`nn::settings::system::GetSerialNumber` (0x31b1b0) is called from the Serial Information scene
(0x99c0e0) and from two other places (0x499368, 0x665288) not reached in a plain HOME session.

## The binary

`<nand>/calibration/prodinfo.bin`, 0x8000 bytes, the documented CAL0 layout (switchbrew
"Calibration"; block offsets cross-checked with Atmosphere's `amsmitm_prodinfo_utils.cpp` and
Eden's own PRODINFO reader at 0x3890). Not at `<nand>/PRODINFO`: Eden's key manager scans the NAND
root for a file of that name and would XTS-decrypt it as a real console's.

Header: magic `CAL0`, version 7, body size 0x7FC0, model 1, CRC16 (Atmosphere's nibble table,
init 0x55AA) over 0x00..0x1E, SHA-256 of the body. Every block below ends in its CRC16, or in a
SHA-256 for the three hashed blocks (TLS certificate over its stored size, random number, game
card certificate).

| Block | Offset | Harbor fills |
|---|---|---|
| ConfigurationId1 | 0x0040 | `MP_00_01_00_00` |
| WlanCountryCodes | 0x0080 | count 1, `R1` |
| WlanMacAddress | 0x0210 | random, locally administered unicast (`x2`/`x6`/`xA`/`xE` first octet) |
| BdAddress | 0x0220 | random, same rule |
| Accelerometer/Gyroscope offset, scale | 0x0230-0x024F | offsets 0, scales 0x4000 |
| SerialNumber | 0x0250 | `HBW` + 10 random digits + check digit (weights 1/3, the console rule) |
| EccB233DeviceCertificate | 0x0480 | Nintendo format, sig type 0x10005, issuer `Root-OP00000001-HD00000001`, key type 2, name `NX%016llX-0` (device id), sect233r1 point |
| SslCertificateSize / SslCertificate | 0x0AD0 / 0x0AE0 | DER X.509, RSA-2048, CN "OpenPak Harbor device", self-signed |
| RandomNumber | 0x1300 | 0x1000 random bytes |
| Rsa2048ETicketCertificate | 0x2A90 | Nintendo format, ECDSA-signed shape, key type 1, same issuer and name |
| BatteryLot | 0x2CE0 | `BHACHZZAD` + letter + 10 digits + check digit |
| RegionCode, ProductModel | 0x3510, 0x3740 | 1, 1 (as set:sys GetProductModel answers) |
| ExtendedEccB233DeviceKey | 0x3770 | the scalar, wrapped |
| ExtendedRsa2048ETicketKey | 0x3890 | p, q, e, wrapped |
| ExtendedSslKey | 0x3AE0 | p, q, e, wrapped |
| ExtendedRsa2048DeviceKey / Rsa2048DeviceCertificate | 0x3D70 / 0x3FC0 | p, q, e wrapped / Nintendo format as above |
| everything else in the table (speaker, LCD, colours, Type-C, six-axis type, battery version, game card certificate, legacy SSL key) | | zero, with a valid check |

Amiibo and game-card keys are not made (no NFC write path, no card slot).

**Device id**: `0x7E << 56 | 56 random bits`. A console's device id is its 56-bit fuse id (top
byte 0), so no Harbor id equals a console's, whatever prefix a service puts in front.
**Serial**: consoles always start with `X`; `HBW` cannot collide and still passes a "14
alphanumerics with a check digit" test.

## Keys

- **Device key** (16 bytes, the stand-in for the key a Switch keeps in its SoC): random per
  install, kept only in the host password store under `harbor-device-key-<8 bytes of
  SHA-256(device id)>`, never on disk, never logged.
- Private keys in the binary are wrapped with AES-128-GCM under
  `HMAC-SHA256(device key, "harbor-cal0-key-wrap")[0..16]`, 12-byte nonce in a 16-byte field, the
  block offset as AAD. A console's PRODINFO is likewise useless without its SoC.
- `Calibration::DeriveDeviceKey(label)` = `HMAC-SHA256(device key, label)[0..16]`: the device-bound
  input for Harbor's own HLE code. The fs save-transfer stream should take its device-unique key
  from here (one label per purpose, e.g. `fs-save-data-transfer`).

## Life cycle

- First boot of a NAND with no binary (set:cal is constructed at boot): generate, store the
  device key, write the file. Log: `calibration: made this NAND's own (32768 bytes, serial
  present, device key in the password store)`.
- Later boots: verify every check, find the device key, open the ECC key. Log: `calibration:
  loaded (32768 bytes, checks pass, device key present)`.
- Binary fails a check: kept as `prodinfo.bin.invalid`, a new one is made.
- Keys are never replaced on their own (2026-10-09, after a phone start that ran before the
  password store was installed replaced a console's key pairs). `Calibration::Keys()`:
  - `StoreUnavailable`: the frontend has not decided the password store yet
    (`OpenPak::SecretStoreDecided()`), or the store could not be read. Nothing is made or
    replaced; the binary's identity is used, and the key is looked up again once the store is
    decided (or after 30 s for a read error). A NAND with no binary waits too.
  - `NotOnThisHost`: the store answered, and the key is absent or not this console's. The console
    runs but does not sign in to OpenPak (no self-signed stand-in either); the frontend says so at
    console start and points to Restore console or Make this a new console.
  - `BackupPending`: a restored NAND carrying its sealed key; Restore console asks the passphrase.
- Every frontend start decides the store first: Qt calls `InstallSecretStore` at window start;
  Android's start goes through `HarborStartActivity` (which installs it before OpenPak), and an
  `EmulationActivity` that Android recreates on its own after the process died is sent there
  instead of booting the console directly (the start path behind the phone's run).
- A copied NAND is the same console (same as restoring a console's NAND backup). See decisions.

## Certificates (owner approved an OpenPak device CA, 2026-10-08)

Nothing in Harbor verifies a device certificate against Nintendo's CA: es, ssl, nim, dauth are
HLE and none checks its own console's certificate; tickets are verified against the ticket
signer, not the device. Only servers verify device certificates. Until OpenPak signs them, the
signatures are zero (`Cal0::IsSigned`) until the console enrols (below).

Proposed server side (nx-baas or a small `devices` service; keys owner-local like the ceiling
key, never generated by agents):

1. **Two CA keys.** `HD00000001`: an ECC **sect233r1** key that signs Nintendo-format
   certificates (ECDSA-SHA256, sig type 0x10005) under a root `Root-OP00000001`; the formats are
   then byte-identical to a console's. Plus an X.509 "OpenPak Harbor Device CA" for the TLS
   client certificate.
2. **Enrolment** `POST /harbor/v1/devices` (TLS, after the console's first OpenPak contact):
   device id, serial, the three unsigned certificate bodies, the TLS public key; proof of
   possession: an ECDSA signature by the ECC device key and RSA signatures by the eTicket, device
   and TLS keys over a server nonce. Server checks: id top byte 0x7E, serial `HBW` + valid check
   digit, id and serial unique (a second enrolment of the same id with other keys is refused
   unless the owner re-keys it), rate limit. Returns the four signatures; Harbor writes them
   into the blocks and re-seals.
3. **One live console per device id** (owner decision 2026-10-08). Enrolment refuses a device id
   that is already enrolled with other keys while that console is live (seen within the server's
   activity window, e.g. 30 days, or never retired), and answers HTTP 409 with
   `{"error":"device_id_in_use","action":"make_new_console"}`. Harbor then tells the user that
   another console uses this identity (a copied NAND) and offers Harbor menu › Make this a new
   console. The same id with the same keys is the same console (re-enrolment after a restore on
   another host with its device key restored): answer the existing certificates. A console made
   new retires its old enrolment by enrolling its new id; the old id frees once its window lapses.
   dauth applies the same rule to a TLS client certificate of a known device id with different
   keys: 409, same body. A NAND copied on the same host keeps the same keys too, so keys alone
   cannot tell the copy apart: two concurrent sessions (push connections or heartbeats from
   different source addresses or session ids) for one device id get the same 409 on the second.
4. **Verifiers**: dauth binds the device to the TLS client certificate chaining to the device CA
   (and accepts consoles' Nintendo-chained certificates, public material only); BaaS device
   accounts and Penne/friends/play reports use the device id from that certificate; the shop
   (ecs/eTicket) issues personalised tickets to the eTicket RSA certificate; `saves` binds a
   key-seed package to the requesting device's certificate and challenge.

### As built (2026-10-09; nx-baas v0.51.0, `harbor_devices.go`; Harbor `enrolment*.cpp`)

The endpoints live in nx-baas on the **dauth host** (`dauth-lp1.ndas.srv.nintendo.net`), the one
name where nx-baas asks for the TLS client certificate:

| Request | Answer |
|---|---|
| `POST /harbor/v1/devices/challenge` | `{"nonce": b64(32 bytes), "expires_in": 300}`, single use |
| `POST /harbor/v1/devices` `{nonce, device_id (16 hex), serial, ecc_certificate, eticket_certificate, device_certificate, tls_certificate (the self-signed DER), proofs: {ecc, eticket, device, tls}}` | 200 the four certificates, signed (full bytes, base64), plus `tls_ca_certificate`; 400 `invalid_device` / `invalid_proof` / `invalid_nonce`; 429; 409 `device_id_in_use` |
| `POST /harbor/v1/devices/live` (every 30 s while signed in) | 204; 409 when another session of the id is live |
| `POST /harbor/v1/devices/release` (GoOffline) | 204 |

Every proof signs `"OpenPak Harbor device enrolment v1\0" ‖ nonce ‖ device id (8 bytes BE)`:
ECDSA-SHA256 (DER) by the B233 key, RSASSA-PKCS1-v1_5-SHA256 by the three RSA keys. The server
answers whole certificates; Harbor takes them only when each Nintendo-format one is its own body
with a signature added and the X.509 one carries its own TLS key (`Cal0::ApplySignedCertificates`),
re-seals `prodinfo.bin`, and rewrites the per-server `device-*.pem` from it. With no CA keys the
server answers 503 `enrolment_unavailable`; Harbor logs it and asks again after 6 h.

- **Liveness**: a different-keys enrolment is refused while the enrolled console was seen within
  30 days; a second console session (`X-OpenPak-Console-Session`, random per Harbor process,
  sent to the dauth host only) is refused while another was heard from within 90 s. A clean exit
  releases the session (`BootConnection::Stop`), so a restart is not refused; after a crash the
  menu's note says to wait out the 90 s.
- **dauth** names an enrolled console by the id it had before it enrolled (`cert-<8 bytes>` of the
  certificate it presented when enrolling), so pctl pairings, shop links and device accounts made
  earlier carry on, with the console's own `HBW` serial. A device CA certificate that is no longer
  its id's is refused (409). **BaaS** binds a device account to the enrolled console that first
  logs in with it; another device is then refused (`invalid_device_account`).
- **Harbor**: on a 409 (dauth, live, enrolment) the session notes it once and the Borealis menu
  opens at Harbor › This console (`MenuPage::Console`) with a note above "Make this a new
  console…"; the next accepted answer clears it. `MakeNewConsole` clears it too.
- **CA key files** (nx-baas env, mounted read-only like saves' KSP keys):
  `NX_DEVICE_CA_ECC_KEY_FILE` (HD00000001, sect233r1, PEM SEC 1 or PKCS#8),
  `NX_DEVICE_CA_TLS_CERT_FILE` (the X.509 CA certificate, PEM, CA:TRUE, keyCertSign) and
  `NX_DEVICE_CA_TLS_KEY_FILE` (its key, PEM; RSA ≥ 2048 or ECDSA P-256/P-384). All three or none.
- **Checks**: `check/calibration_check.cpp` (proofs, taking signed certificates, refusals);
  `check/enrolment_check.sh <nx-baas> <work dir>` (the wire against a local nx-baas with
  throwaway keys: enrol, dauth, second session 409, other keys 409, restored console accepted,
  no keys inert); nx-baas `harbor_devices_test.go` and `internal/b233`.

## Console backups and new consoles (owner decisions 2026-10-08)

- **Back up console** (Harbor menu, Qt and Android): the emulated console stops, the device key is
  sealed into `<nand>/calibration/device-key.hbk` with a passphrase the user sets (scrypt N 2^15,
  r 8, p 1 to AES-256-GCM; the device id is associated data, so a sealed key opens only on its own
  console), the NAND is zipped, and the sealed key leaves the live NAND. The passphrase is never
  stored. Android's existing "Export user data" zip does not carry the key; the Harbor menu's does.
- **Restore console**: the backup unzips into `nand.restore`, must hold `calibration/prodinfo.bin`,
  then replaces the NAND. If this host's password store has no device key for it, the binary
  waits (`keys pending`: no new key pairs are made while a sealed key is present) and the
  passphrase is asked; the right one puts the key in this host's store and the restored console
  is the same console. Cancelled: the console stays locked until a restore with the passphrase or
  "Make this a new console".
- **Make this a new console**: after a confirmation the console stops; every OpenPak credential in
  the old namespace (the namespace keeps an index of its entries), the old device key and the
  per-server TLS files are dropped, this process forgets its signed-in users, and a new
  calibration is made. Firmware users and saves stay; each user links again.

## Save-data transfer

A console's transfer key depends on its own device key, so no two devices share it, console or
Harbor; archives move between devices because the server issues each importer a key-seed package
for its own challenge (research/olsc-re/fs-transfer.md §5, §6.4). Harbor behaves like one more
console: its fs uses `DeriveDeviceKey(...)` where a console uses its device-unique key, and the
OpenPak `saves` server (signing with the OpenPak modulus, §6.4) bridges consoles and Harbors. What
stays open is §7.3: matching nnSdk's exact derivation, which only matters for a console reading
an archive *without* the server's package.

## Consumers wired

- fs `OpenBISPartitionStorage(CalibrationBinary)`: this binary in a HOME session, never a dumped
  PRODINFO (fsp-srv `OpenBisStorage` itself is still unimplemented, so no guest reaches it yet).
- spl `GetConfig(DeviceId)`, nsd `GetDeviceId` (unknown contract: 16 hex digits), ldn station MAC,
  wlan:lcl / wlan:soc `GetMacAddress`: from `Harbor::Calibration::Get()`.
- OpenPak TLS: `Harbor::InstallClientIdentity()` runs in core before the services start; a server
  this install has no certificate for gets the calibration's TLS certificate and key.
- Not wired: btm `GetHostDeviceProperty` / btdrv `GetAdapterProperties` (23.x layout not
  established, no caller seen); ethc `GetMacAddress` (a wired adapter, not in CAL0); the starter's
  Internet pages show "System MAC address: ---" and "LAN MAC address: ---", fed by a nifm call
  not yet identified.
- Password store: every OpenPak secret key (device accounts, Penne, pctl, website account) is
  kept as `console-<8 bytes of SHA-256("harbor-console:" + device id)>-<key>`
  (`Harbor::InstallSecretNamespace`, set before any secret is read); an older global entry moves
  under the namespace on its first read. The device key itself stays host-wide. A NAND copied
  together with `calibration/prodinfo.bin` is the same console and shares the namespace; remove
  the binary from the copy to make it a console of its own.
- set:sys `GetMiiAuthorId` already is a per-install random UUID in the system settings save.
