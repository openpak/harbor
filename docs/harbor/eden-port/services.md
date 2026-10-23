# Harbor on Eden — phase 0: OpenPak network services map

Written 2026-10-06. This is analysis only: nothing was built or run, and no original repository was changed.

**Sources**
- Harbor (Ryujinx): `emulators/ryujinx-android`, branch `feature/android-port` at `ee683886b`. Its upstream merge base is `475615f04`.
- Fresh Eden: `emulators/harbor-eden`, branch `harbor` at `10bcd2d84`.
- Old OpenPak Eden fork: `emulators/eden`, branch `openpak-playability-column`, `d88ed1bd7f`. It carries 135 commits on `5f142c7926`.
- Shared C++ client: `openpak-client` `main`, `3fd9ba7`.

**Path shorthand**
- `R:` = `ryujinx-android/src/Ryujinx.HLE/HOS/Services/`
- `RH:` = `ryujinx-android/src/Ryujinx.Horizon/`
- `RO:` = `ryujinx-android/src/Ryujinx.OpenPak/`
- `E:` = `harbor-eden/src/core/hle/service/`
- `OPC:` = `openpak-client/client/openpak/`

**Labels**
- "Guess" marks an inference that nobody has measured.
- Effort is in engineer-days, written as `d`. Confidence is H, M or L.

**Throwaway evidence:** `scratch/harbor-eden/research/`.
- `eden-merge/` is a repo built on alternates. Its branches are `fork`, `up` and `base`. The trial-merge tree is `2fec0477dd`.
- `eden-pick/` is a worktree on branch `pick`. It holds the 22 commits that cherry-picked cleanly onto `10bcd2d84`.

Neither directory is pushed anywhere, and both can be deleted.

---

## 1. Verdict on the old OpenPak Eden fork

**Do not start from the fork, and do not merge it.** Start from fresh upstream `10bcd2d84`. Cherry-pick about 25 commits from the list below, and re-write about 10 more from their descriptions. Ignore everything else.

### Why not start from it

1. **More than half of it is host UI that Harbor's model forbids.** About 7,100 lines changed under `src/core` and `src/common`, plus the Qt and Android layers. That includes:
   - the `src/yuzu/openpak_host.*` account window, toasts, setup wizard, startup profile picker and sign-in dialog;
   - the Android `OpenPakFragment` and `OpenPakUi`;
   - the Qt MyPage friend picker that replaces the firmware MyPage;
   - the Join/Ignore invitation dialog;
   - the Online and Playability game-list columns.

   In Harbor the firmware does all of this: starter, PlayerSelect, the Login applet, MyPage, overlayDisp and qlaunch.
2. **The fork's identity model conflicts with Harbor's.** In `acc.cpp`, `OpenPakSignedIn()` (lines 56–101) ties the OpenPak identity to `Settings::values.current_user`. It signs in synchronously on the IPC thread, through `openpak::client::session`, using a website bearer that the host sign-in dialog supplied. Harbor's users come from firmware setup, and they are linked through acc:su 211–214 and the Login applet.
3. **Licence lineage.** Some socket commits in the old fork had unclear provenance and were not carried over. Ryujinx master removed the code in question in `154744710`. These commits are `73da2f25b7`, `497320b975` and `fd1b755fc4`, plus `03d14d62d1` and `fa69cbe39f` by lineage. They cannot ship in an AGPL Harbor and must be rewritten clean-room.
4. **None of the 2026-09-28 parity-pass commits was ever run against a title** (internal research notes, "State on 2026-09-28"). Only the 09-13 to 09-17 work, Stardew online, was verified live.
5. **The base is close.** Only 74 upstream commits separate `5f142c7926` from `10bcd2d84`, so cherry-picking is cheap. Starting from the fork would mean reverting far more than you would carry.

### Trial results against 10bcd2d84

**Whole-fork merge** (`git merge-tree fork up`): 7 files conflict.
- `ssl/ssl.cpp`: 7 hunks, about 800 lines. Upstream moved ISslConnection and ISslContext to `D<>` CMIF serialisation in `8c1194474e`, and removed SecureTransport and SChannel in `ee73920d28`.
- `ssl_backend_openssl.cpp`: 1 hunk, `SetHostName(const char*)` against the fork's deferred host name.
- `sockets.cpp`: 1 hunk, the new `max_sessions` arguments from `8a22f1845b`.
- `patch_manager.cpp`: 2 hunks, `GetModificationLoadRoot()`.
- `am/frontend/applets.cpp`: 1 hunk, `AppletId::None` against the fork's MyPage.
- `src/yuzu/main_window.cpp` and the Android JNI `CMakeLists.txt`.

**Selective cherry-pick** of 38 candidate commits, in fork order, onto `up`, without their dropped predecessors:
- **22 applied cleanly** (2,682 lines across 25 files; see `research/eden-pick`).
- **16 conflicted:**
  - **4 are trivial.** The submodule gitlink or OPENPAK.md/host files are only touched in `09f68c5377`, `cdca940531` (core hunks applied) and `b901520d0c`. In `3c60035c49` the trivial part is the gitlink.
  - **12 are real:**
    - ssl ×5: `c584e950bf`, `194f46f1cb`, `f72a21cff2`, `bcb4f1dd44`, plus the ssl part of `1f76d48e3a`;
    - sockets/sfdnsres/nsd ×5: `80801b1919`, `d187a8c910`, `fb99c24f47`, `dc770c6d2d`, `9542a5e612`;
    - nifm ×1: `08cc59e11b`;
    - patch_manager ×1: `5b6a6135b7`.
  - Most of the sockets, sfdnsres and nifm conflicts come from dropping `77131d4747`, `1af8a689f7`, `73da2f25b7` and `497320b975`, which they were built on. Do not resolve these textually. Re-express them inside the bsd and resolver rewrite.

**Expected conflict cost:**
- ssl rewrite onto `D<>`: 1–1.5 d, plus 0.5 d of title tests.
- Resolver and nsd onto upstream: 1 d.
- nifm: 0.3 d.
- patch_manager: 0.2 d.
- Submodule and `CMakeLists` wiring: 0.3 d.
- **Total: about 3.5–4 d of mechanical work**, separate from the bsd rewrite.

### Carry: cherry-pick as is, or with small edits

Each line gives the hash, the one-line reason, and any edit needed.

**Accounts**
- `42c1f8607e`: acc LoadIdTokenCacheDeprecated (3) goes to the same path as 4. Re-point the token source to the acc-owned session (see §2).
- `229a534b81`: IAuthorizationRequest 0/10/19/20/21/22 and application-side 150. Same shape as `R:Account/Acc/AccountService/IAuthorizationRequest.cs`. Re-point the token source.
- `fb22fb4fd0`: acc resource cache NSA id, licence kind 170 answers Subscribed, acc:u0 stubs 6/99/102/103/111/120/130. Re-point the token source.
- `e9ed8153fe`: nifm SetNetworkProfileId succeeds and GetAppletInfo answers three words. Upstream still pushes `1`.

**Friends and npns**
- `ddbcea330f`: `friend/openpak_friends.cpp` (2,202 lines). It has Harbor's FriendService command set, permission by port name, an INotificationService queue, and guest events signalled from a session-owned thread. Remove the id logging (§5). The core of the social port.
- `fb2cd49363`, `feba677913`: own-profile resolution; the completion event follows the work; GetProfileImageUrl. These match Harbor `c5052b1fb`, `de8916462` and `201c5c451`.
- `58f387cef7`: GetProfileList reports unresolved ids. Change it to log counts only.
- `b901520d0c`: the friend block/unblock hunk (30400–30403). Its BCAT hunk goes with bcat, below.
- `09f68c5377`: npns:u subscriptions. Take the gitlink once.
- `7fb9883f4b`: npns 162/201/202 prevent qlaunch's Friends "Update" fatal (2010-0212). The pctl 1475 and prepo stubs are superseded later by the pctl port. These are Citron-derived stubs; Citron is GPL, so they are fine.

**BCAT**
- `535ce7beb7`: bcat name validation and `current_dir`. Upstream still has all three bugs.
- `a497ae63fd`: the delivery-cache save is created on demand (Splatoon 3's endless retry). Check that `save/bcat/<tid>` is the same directory the BIS path in `bis_factory.cpp:139` reads. This is a guess-level risk.
- `b901520d0c` (BCAT hunk): fills the cache through `openpak::bcat::Fetch`. Harbor does the same kind of host fetch, at launch.

**am**
- `cdca940531` (core hunks only): `AppletManager::PushFriendInvitation` and a FIFO 141 pop with the event re-armed. Use it to implement IApplicationAccessor 180, which is nullptr upstream. Drop the Qt prompt.
- `94789d409e`: error-applet word order, and NetConnect exits. This only matters on the host-fallback path.

**sm (core area)**
- `13baceaea8`, `4373aa192f`: the pointer buffer is at least 0xF000 unless the guest set it. NPLN gRPC needs this.

**Sockets, nifm, ldn and the network profile**
- `4b8a20f8a2`: the setsockopt failure log names the option.
- `af04e01d61`: a socket whose Initialize failed is not returned as a valid descriptor.
- `7277268fcd`: the SOCK_CLOEXEC strip only.
- `ae862ed29c`: socket type 0 is DGRAM.
- `d187a8c910`: getifaddrs via sysctl NET_RT_IFLISTL, from OpenPak's own Ryujinx `faf7575ce`, which is fine. One-hunk conflict.
- `dc770c6d2d`: SOCK_NONBLOCK applied, a UDP receive buffer, timeval timeouts ("as Citron", GPL). One-hunk conflict.
- `2e602ee3ba`, `6b24a920b9`: nifm Submit is accepted and both events are signalled; a request on hold is accepted once the host has an address.
- `08cc59e11b`: nifm SetDefaultIpSetting and the two ForTest switches. Two-hunk conflict.
- `6f50a26a02`: ldn drops short packets and ignores another session's SyncNetwork.
- `3c60035c49`, `1f76d48e3a` (core parts): the network profile and signed ceiling come from `openpak::NetworkProfile`. Move the fetch out of acc's `call_once` and into host boot.

**SSL** (re-express as `D<>` handlers on upstream's file; do not apply the text)
- `194f46f1cb`: guest TLS verified against the OpenPak CA, with IP SAN support. This is the core trust code.
- `c584e950bf`: DoNotCloseSocket. Upstream's logic is still inverted.
- `f72a21cff2`: Peek. Upstream's `8c1194474e` Peek is broken: `*out_size` is 0 and the error label says SSL_write_ex.
- `bcb4f1dd44`: the guest's verify option is kept, the duplicate socket is closed, ssl:s CreateContext really creates a context, and the Citron stubs.

**Resolver**
- `9542a5e612`, plus Harbor `711e9152f`: sfdnsres and nsd resolve the way Ryujinx does. Also apply 711e9152f: `accounts.nintendo.com` is no longer mapped to BaaS. Keep the blocklist fail-closed for every ceiling family (§5 risk 4).
- `80801b1919` (resolver part): address lengths. Rewrite its `server_manager` deferral wake inside the bsd rewrite.
- `1af8a689f7` (parts): nsd `%` substitution, `use_nsd_resolve`, the split between the ALPN offered list and the negotiated name, and the CA placed where the console store expects a root. Drop its poll and eventfd parts.

**File system**
- `5b6a6135b7`: the Crash Team Racing Demonware key-swap IPS, the one sanctioned game-patch exception. Harbor carries the same patch (`ryujinx-android/src/Ryujinx.HLE/HOS/OpenPakBuiltinPatches.cs`). Two-hunk conflict in `patch_manager.cpp`. The owner should reconfirm it, because this task's brief says "console only for CA trust".

**Core**
- `912d991a1d`: presence learns the running title and its NACP presence group. **Rewrite the hook point.** `System::Impl::Load` only ever sees qlaunch in a HOME session, so the hook must move to am application process start and exit (core area).
- `085cf4cd7e` (parts): `HLERequestContext::ReadBufferB` and the gdb stub bound to loopback. Drop its `OPENPAK_TRACE_GUEST_FAULT` and thread-creation log.

### Rewrite clean-room from behaviour descriptions

These must never be re-read while writing the new code.
- `73da2f25b7`, `497320b975`, `fd1b755fc4`: unclear-provenance lineage (deferral, eventfd drain, feigned sockopts, the 5 ms datagram slice, 2 s sync connect).
- `03d14d62d1`: select parked on eventfds, one fd table, `bsd:a` not a user service. Upstream registers `bsd:a` as a user service with `max_sessions` 0x17; check which is right.
- `fa69cbe39f`, `fb99c24f47`: the parked datagram recv and the 1 ms heartbeat. These are built on the deferral lineage.
- **Spec for the rewrite:**
  - the `154744710` commit message;
  - `emulators/ryujinx/OPENPAK.md` (bsd paragraphs);
  - internal research notes §2 "Sockets" and §3;
  - Ryujinx master's `IClient.cs`, `ManagedSocketPollManager.cs` and `ServerBase.cs` deferral (`cd462aa69`, which is OpenPak's own).

  **Never use `feature/android-port`'s IClient.** It still carries `3ab97fbe7` and `081d5cd88`.

### Drop

- **CI and docs:** every `.github`/`ci:` and `docs:` commit, `50e6f53ff4` and `21417881cd`.
- **Qt and Android host UI:**
  - `5aff73bd8f`, `ecc26c0e0f`, `5b85e37a36`, `bc29db7bc0`
  - `59814e404d` (keep only the idea of linking openpak-client into core)
  - `e6becedf4d`, `ceb0138834`, `b0bd162cde`, `5c7dab39ca`
  - `376d02abfe`, `2438bef8a7`, `b88e445995`, `26c13aaa98`, `4a258f74d2`
  - `9e8416cb2e`, `2cbcb85625`, `e701cd545a`, `03b0e9e003`, `bfce138ff8`, `1e2c462b02`
  - `dc8377cdfd`, `2ecb995c35`, `3a7d249877`, `e2faa90c79` (crash reports through a host dialog)
  - `41697391b2`, `e2e8adbb4b`, `5c17526534`, `dbad72d55e`, `e98d2462d2`, `509f860c26`
  - `cd0e96f32f`, `d7f9739b02`, `b59438e029`, `94adcd10e2`, `18c8f05879`, `82e4c0a44e`
  - `552451529a`, `d88ed1bd7f`, `8d514b55c7`
  - all of the `openpak-client` gitlink bumps (take one current gitlink instead)
- **Host MyPage replacement:** `6a6bbbe577`. harbor-eden already defaults `my_page_applet_mode` to LLE (`common/settings.h:169`).
- **Unsafe or diagnostic:**
  - `daee120d9d`: stops verifying certificates while the redirect is on.
  - `7bfccb5811`: `EDEN_SSL_TRACE`, plaintext logging.
  - `32423ebc7a`, `a346f05a25`: thread-98 tracing.
  - `77131d4747`: a grab-bag, superseded.
  - `b2bbdb48fd`: upstream now has Pending.
- **Privacy:** `3719853697` (thumbnail URL per friend id) and `ff48010c71` (raises id logs to the default level).
- **Breaks "IPC never waits on the network":** `c3d08447c2`, a blocking HTTP fetch inside GetProfileList. Redo it asynchronously if it is still needed.
- **Game code patch:** `02671249a1` (Super Mario Bros. Wonder instruction changes). `2c330c7608` already reverted it.
- **Upstream already has it:** `7d3fba2ead` (`3266c20e3` upstream).

---

## 2. Should openpak-client be reused?

**Yes, as the core-only library. Build it with `-DOPENPAK_BUILD_QT=OFF` and link `openpak::client`. Do not use the `qt/` half.**

### Licence

`openpak-client` is GPL-2.0-or-later throughout (111 SPDX headers). 34 files keep a "citron Emulator Project" copyright line because they were extracted from Citron's GPL-2.0-or-later code. All of this is compatible with an AGPL-3.0 combination through GPL-3.0 §13. Eden's own files are GPL-2.0-or-later and GPL-3.0-or-later.

Two consequences:
1. The Eden-derived files stay GPL-3.0-or-later, so "Harbor is AGPL-3.0-only" holds for Harbor's new files and for the combined work, not for every file. The svc-eshop and svc-social research reached the same reading; it is not legal advice.
2. The repository is **private** today: the fork's CI fetches it with `OPENPAK_MODULE_TOKEN`. AGPL and GPL distribution of Harbor binaries requires the corresponding source to be available, so before any release it must be made public, or its files must be vendored into the Harbor tree.

### Reuse as is (pure protocol, no host UI)

| Module | Harbor equivalent | Use in Harbor-on-Eden |
|---|---|---|
| `network_ceiling.cpp`, `network_profile.h`, `network_profile_fetch.cpp` | `RO:OpenPakCeiling.cs`, `RO:OpenPakNetworkProfile.cs` | Ed25519 ceiling with the same pinned key, profile fetch/validate/store, `RedirectFor` with Harbor's precedence (never → overrides → exact → suffixes), `EnsureCA` pinned by SHA-256 |
| `friend_structs.*`, `baas.h`, `baas_parsing.cpp`, `baas_requests.cpp`, `baas_blocks.cpp`, `friends_cache.*`, `presence_state.*` | `RO:OpenPakBaas*.cs`, `RO:OpenPakPresence.cs`, `RH:Sdk/Friends/Detail/OpenPakFriends.Structs.cs` | Friends REST, the Five inbox, blocks, requests, the guest struct layouts (with tests in `tests/friend_structs_test.cpp`) |
| `push.*` | `R:Account/OpenPak/OpenPakSession.Push.cs`, `RO:PenneFrames.cs` | Penne registration, login ticket, frontline frames. Add a `(kind, body)` hook for ovln and pctl |
| `bcat.*` | `RO:OpenPakBcat.cs` | Title delivery-cache fetch |
| `nat_check.*` | `RO:OpenPakNatCheck.cs` | Optional host-side diagnostic |
| `session.*` (only the dauth, aauth and BaaS application-token, users, login and federation chain, plus the per-server client certificate) | `R:Account/OpenPak/OpenPakSession*.cs`, `OpenPakServer.cs` | The device-account chain. Usable once it is changed as listed below |
| `platform.*`, `log.h`, `safe_entry_path.h`, `zip_store.*` | — | Directories, X-OpenPak-Client header, log sink |

### Do not use for Harbor

- `qt/` entirely: the account window, prompts, sign-in, toasts, friend picker and chat. The firmware applets do this UI.
- `my_page.*`: the host MyPage substitute.
- `api.*` website-bearer sign-in (`SignIn`, `LinkWithPassword`) and `account.*`: the website identity model. Harbor links through acc:su 211–214 and the Login applet instead.
- `save_sync.*`, `restore_swap.h`: host cloud saves. These are not in Harbor's HOME model; olsc is the firmware path, and that is a separate decision.
- `compatibility.*`, `playability_list.inc`, `population.*`, `crash_reports.*` (host dialog).

### Changes openpak-client needs before Harbor uses it

Estimated 4–6 d. They land in openpak-client itself so that Citron and the other forks keep building.

1. **Identity per firmware user.**
   - `Platform::SetProfileSource` must answer the uid acc has open (from `ProfileManager::GetOpenUsers`), not `Settings::current_user`.
   - `session::Configure` must become per user, or be wrapped by an acc-owned `HarborSession` keyed by uid.
   - Drop the website auto-link path (`LinkFromAccountLockedAsync` equivalent) from console boot.
2. **Nintendo Account OAuth half.** Missing today:
   - the code→token exchange (`/connect/1.0.0/api/token`) and `users/me`;
   - federation of a user that does not exist yet (registration 110/221);
   - edge tokens for nim:shp (vendor "akamai");
   - the dauth client for pctl.

   Port these from `R:Account/Acc/AccountService/NintendoAccountOAuth.cs` and `OpenPakSession.NintendoAccount.cs` / `.Dauth.cs`.
3. **Async, never on IPC threads.** `session::Ensure()` is blocking, and the fork called it from the acc IPC thread. Expose futures or callbacks so that Eden's `IAsyncContext` workers can complete and signal.
4. **Penne hook.** `push.cpp` only refreshes caches. Add the kind/body callback (Harbor's `RaisePushReceived`) for:
   - FriendsOverlayNotifier: `friend_invitation_received`, `presence_updated`;
   - pctl: `DID_CHANGE_PARENTAL_CONTROL_SETTING`, `DID_UNLINK_NX`.

   Also expose `PenneId()` for the pctl `notificationToken`.
5. **Privacy scrub** (list in §5): `session.cpp:646` (nickname or device id), `session.cpp:397` (raw response bodies), `session.cpp:1275/1282` (paths contain user ids), `api.cpp:517` (pid and username), `baas_requests.cpp:127` (friend code), `baas_requests.cpp:286/311/334` (request and target ids).
6. **File layout.**
   - openpak-client writes `openpak_network_profile.json/.etag`, `openpak_network_ceiling.json/.version` and `openpak/ca.pem`.
   - Harbor writes `openpak/server.json`, `ca.pem`, `network-profile.json`, `network-ceiling.json` and `network-ceiling-version`.

   Pick one with the host area. Harbor's alpha may break, so the recommendation is to adopt Harbor's names inside openpak-client behind a `Platform` option and skip any migration.
7. **Secrets.** The device-account JSON (`device-<server>-<profile>.json`, holding id and password) is plain text in both Harbor and openpak-client. Under Harbor's model it is the real credential. Put it behind a host secret-store hook: Android Keystore through JNI, libsecret on desktop.

---

## 3. Feature table

**Eden status** describes `10bcd2d84` as it is, without the fork:
- **has**: works as Harbor needs.
- **partial**: the IPC shell exists, but behaviour or layout is wrong or stubbed.
- **missing**: nullptr or absent.

**Verdict:**
- **reuse**: an Eden upstream or fork commit.
- **reuse-OPC**: openpak-client.
- **port**: translate Harbor's C# to C++.
- **rewrite**: new code, written from behaviour descriptions.

### 3.1 Accounts (acc, dauth, ns account hooks)

| Harbor feature | Ryujinx file(s) | Eden file(s) | Eden status | Verdict | Effort (conf.) | Risks |
|---|---|---|---|---|---|---|
| **Console mode.** No default user; starter creates users. acc:su 200 makes a nameless user, 201 saves and opens it, 206 = 201. GetLastOpenedUser and TrySelectUserWithoutInteraction answer the invalid uid when nobody is open | `R:Account/Acc/AccountManager.cs` (`createDefaultUser`, `bf7dc177f`), `IAccountServiceForAdministrator.cs` | `E:acc/profile_manager.cpp:53-72` (ctor creates the "Eden" user and `OpenUser(*GetUser(current))` dereferences an empty optional with zero users), `acc.cpp:1029` (200 names it "Eden"), 206 nullptr; **two ProfileManager instances** (`core/core.cpp:114/491` and `acc.cpp:1641`) | missing | rewrite the ProfileManager boot path; port 200/201/206; one shared instance | 2–3 (M) | Frontend and `launch_params.cpp:101-109` assume at least one user; touches core (§4) |
| **acc:su 211–214, 230.** NA registration procedure (211 Create, 212 Resume, 213/214 the same handlers), 230 AuthenticateServiceAsync. IOAuthProcedure 0/1/2/3/10, 100–104, 110/111, 200, 210, 220/221/230 and the Linkage variant | `R:Account/Acc/IAccountServiceForAdministrator.cs` (`628786c47`, `66537b878`), `AccountService/IOAuthProcedure.cs` (484 lines) | `E:acc/acc.cpp:1306-1308` (nullptr), 213/214 absent; procedure tables `:800-826`, `:307-327` all nullptr | missing | port (contract: `scratch/ryujinx-android/account-oauth/notes.md` §2–4) | 5–7 (M-L) | Two hand-written 0x1A pointer outputs in GetRequest (0x1000 URL, 0x100 callback); needs a real async worker (Eden's `IAsyncContext` has none); 205/215 unserved in Harbor too |
| **NA OAuth against nx-baas.** PKCE S256, Login applet → `/connect/1.0.0/authorize`, callback parse, dauth, `/connect/1.0.0/api/token`, `users/me`, BaaS app token, `/1.0.0/users`, `/1.0.0/federation` | `R:Account/Acc/AccountService/NintendoAccountOAuth.cs`, `R:Account/OpenPak/OpenPakSession.NintendoAccount.cs`, `.Dauth.cs`, `OpenPakServer.cs` | none upstream; OPC `session.cpp:277-589` has dauth, app token, users, login and federation | missing | reuse-OPC (BaaS chain) and port (NA exchange, users/me, pre-user federation) | 3–4 (M) | OPC's global session model (§2 item 1) |
| **IAdministrator (acc:su 250).** 250 IsLinkedWithNintendoAccount (true only for the active, linked uid), 251 (2124-0140 when already linked), 252; inherits 0/1/2/3/4/120/136/143 | `R:Account/Acc/AccountService/IAdministrator.cs` | `E:acc/acc.cpp:176-245`, 250 always false, rest nullptr or absent | partial | port 250–252 with the 120/136 consistency rule | 2 (M) | qlaunch's linked path after 250 = true is unmeasured on any emulator |
| **IAdministrator 290** (switchbrew: GetRequestForNintendoAccountUserResourceView). It blocks Settings → Users → Nintendo Account on PC | not implemented (open blocker, `docs/android-port/account-browser-input.md:60-68`) | nullptr | missing | RE first, then rewrite | 1.5–3 (L) | Guess: GetRequest-shaped (0x1000/0x100 URL buffers). RE the 23.0.1 account module before answering anything |
| **IManagerForApplication / ForSystemService.** 1 NSA id; 2 EnsureIdTokenCacheAsync; 3/4 LoadIdTokenCache (size checked); 120/136 NA id + 0x270 NasUserBase; 143 = (u32 Subscribed, u32 0, u64 0); app-side 150 and 170 | `R:Account/Acc/AccountService/ManagerServer.cs`, `IManagerForSystemService.cs`, `IAuthorizationRequest.cs` | `E:acc/acc.cpp:74-148` (system: stubs, 143 has the wrong layout and says not subscribed), `:683-778` (application: 4 writes zeros, 150/170 nullptr) | partial | reuse fork `42c1f8607e`, `229a534b81`, `fb22fb4fd0` (re-pointed); port 120/136/143 | 2–3 (M-H) | 136 birthday must be YYYY-MM-DD or the answer is "cache not found"; HOME's NA token goes through ns:ec 8/10, not system 150 |
| **dauth device and edge tokens** for consumers: NA exchange, BaaS, nim:shp, pctl | `R:Account/OpenPak/OpenPakSession.Dauth.cs`, `OpenPakServer.cs:203` (per-server client certificate) | `E:acc` DAUTH services nullptr; OPC has device tokens only | missing | reuse-OPC and add edge tokens and client ids | 1 (M) | — |
| **Profile persistence and NA link state.** Nickname and avatar adopted on a linked login (256×256 baseline JPEG; progressive JPEGs re-encoded, otherwise qlaunch shows a white circle); link state lives server-side (federation), derived at each boot | `R:Account/Acc/AccountManager.cs:45-58,175-195`, `ProfileServer.cs`, `system/Profiles.json`, `openpak/device-<server>-<profile>.json` | `E:acc/profile_manager.cpp` (`profiles.dat` + `<uuid>.jpg`), IProfileEditor 100/101/30 implemented | has (base) | reuse Eden storage; port device-account store and adoption | 1.5–2 (M) | Device-account secret is plain text in both |
| **ns 2366** (uid in → copied event + IAsyncResult; firmware calls nim 164) and **ns 4105** (offline success; linked work warns) | `R:Ns/IApplicationManagerInterface.cs:328-370` | `E:ns/application_manager_interface.cpp:347` (2366 nullptr), `:479/885` (4105 stub, ok); `ns/async_result.cpp` Get and GetErrorContext nullptr | partial | port 2366 and implement IAsyncResult 0/2 | 0.5–1 (H) | Fresh registration through 2366 is not proven on Harbor either (owner retest pending) |

### 3.2 Network profile, TLS, DNS, nifm, bsd

| Harbor feature | Ryujinx file(s) | Eden file(s) | Eden status | Verdict | Effort (conf.) | Risks |
|---|---|---|---|---|---|---|
| **Signed ceiling + network profile** (host boot and a 6 h refresh) | `RO:OpenPakCeiling.cs`, `RO:OpenPakNetworkProfile.cs` | none | missing | reuse-OPC; fork `3c60035c49`/`1f76d48e3a` core parts | 1.5 (H) | File names (§2 item 6); httplib needs CA roots on Android for openpak.org |
| **Pinned console-facing CA** (from the profile, SHA-256 pinned) | `RO:OpenPakNetworkProfile.cs:485-516`, `OpenPakConfig.CaPath` | none | missing | reuse-OPC `EnsureCA` | 0.5 (H) | — |
| **Console server address** (`openpak/server.json`: website + console server; the profile's `server_address` wins) | `RO:OpenPakHostSetup.cs:21`, `OpenPakConfig.ResolvedConsoleServer` | none (fork used `openpak_server_ip` settings and env vars) | missing | port (small); the host frontend owns writing it | 0.5–1 (H) | Must be written by the host wizard before core boot (§4) |
| **Guest TLS trust.** Accept a host-trusted chain or one rooted at the OpenPak CA; never excuse a name mismatch; ignore the guest's verify option and CA list | `R:Ssl/SslService/SslManagedSocketConnection.cs:141-152`, `R:Account/OpenPak/OpenPakServer.cs:182-201` | `E:ssl/ssl.cpp:219-224` (upstream forces verify off), `ssl_backend_openssl.cpp:125` (bundled store reloaded on every Init) | partial (unsafe) | reuse fork `194f46f1cb`, re-expressed on `D<>`; adopt Harbor's model: always verify, OpenPak CA added after `LoadCaCertStore` | 1–1.5 (M) | Decision needed: Harbor ignores the guest's verify option; fork and ledger honour it. Recommendation in §5 risk 6 |
| **System cert 1033 = OpenPak CA**; id `0xFFFFFFFE` = "trusted only" (the web applet uses it) | `R:Ssl/OpenPakCertificate.cs`, `BuiltInCertificateManager.cs:99,177` (`cf76f16fd`) | `E:ssl/cert_store.cpp` (only All = -1); fork `CertStore::ApplyOpenPakCertificate` | partial | reuse fork + port the 0xFFFFFFFE filter | 0.5 (H) | — |
| **Other ssl:** Peek, DoNotCloseSocket, ALPN offered vs negotiated, ssl:s CreateContext, the 24/25/28–35 stubs, ISslContext 6/7/9–13 | `R:Ssl/SslService/ISslConnection.cs`, `SslManagedSocketConnection.cs` | `E:ssl/ssl.cpp`, `ssl_backend_openssl.cpp:228-233` (broken Peek); `SetRenegotiationMode` changes the global `ssl_ctx`; `SetNextAlpnProto` stores nothing | partial | reuse fork `f72a21cff2`, `c584e950bf`, `bcb4f1dd44`, `1af8a689f7` (ALPN), re-expressed | 1–1.5 (M), included in the conflict cost above | Test Stardew (1033), Moving Out 2 (Peek), a web applet page |
| **sfdnsres redirect** (cmds 2/6/10/12): nsd substitution → profile redirect → fail-closed for ceiling families → host DNS. Optional Atmosphère hosts file plus 65000 reload | `R:Sockets/Sfdnsres/IResolver.cs`, `Proxy/DnsMitmResolver.cs`, `R:Account/OpenPak/OpenPakHosts.cs` | `E:sockets/sfdnsres.cpp:56-97` (wide substring blocklist, no redirect, no hosts file) | missing | reuse fork `9542a5e612` + `RedirectFor`; no hosts file needed | 1–1.5 (M) | Fail-open on unredirected Nintendo names risks a ban (2181-4008); the fork narrowed the blocklist |
| **Firmware 23.x resolver:** 16 ReportTelemetry, 100 QueryAddrInfoRequestWithOptions (u32 eventfd, config version, option count, four in-buffers; answer stored under a handle, eventfd written), 102 Fetch, 104 Cancel. 101/103 are the name-info pair | `R:Sockets/Sfdnsres/IResolver.cs:275-405` (`463557412`); capture in `scratch/home/openpak-firmware-audit-23.0.x/dnsmitm/` | none; needs a real bsd `EventFd` (`bsd.cpp:508` is a stub) and lookup across services | missing | port | 1.5–2 (M-L) | Harbor's layout (from the friends client marshallers) disagrees with the dnsmitm FINDINGS AMS-patch guess (handle at raw offset 0). Check against `dnsmitm/capture/dns_mitm_raw.log` first |
| **nsd Resolve/ResolveEx (20/21):** `%` substitution, the account names mapped to their BaaS hosts, except `accounts.nintendo.com` (the Login applet's page) | `R:Sockets/Nsd/*` (`FqdnResolver.cs`, `711e9152f`) | `E:sockets/nsd.cpp:67-72` stub | missing | reuse fork `9542a5e612`/`1af8a689f7` + `711e9152f` | 0.5 (H) | — |
| **nifm for qlaunch/NetConnect:** one wired link always, a stored profile with a stable UUID, empty scan, Submit signals both events, Cancel → Free and signals | `R:Nifm/StaticService/IGeneralService.cs`, `IRequest.cs`, `IScanRequest.cs` | `E:nifm/nifm.cpp` (Wi-Fi with real host SSIDs; Submit → OnHold without signalling at `:386`; Cancel does nothing; 26/28/29/30 missing) | partial | reuse fork `2e602ee3ba`, `6b24a920b9`, `08cc59e11b`, `e9ed8153fe`; port wired-only, empty scan, Cancel | 1.5–2 (M) | Guess: Harbor's IRequest setter ids 7/8/9 may be shifted by one against switchbrew and Eden; check before copying |
| **bsd behaviours:** deferred poll, a zero-timeout probe answers 0, datagram sockets never POLLHUP, 5 ms slice while a datagram socket is open, select parked on eventfd, parked datagram recv, real eventfd counter, INET6 (v4-mapped), getifaddrs sysctl, type 0 = DGRAM, SO_REUSEADDR on UDP bind, 1 MiB UDP receive buffer, 2 s synchronous connect to the OpenPak server only | Ryujinx **master** `R:Sockets/Bsd/IClient.cs`, `Impl/ManagedSocketPollManager.cs`, `InterfaceList.cs`, `ServerBase.cs` deferral (not the copies on `feature/android-port`) | `E:sockets/bsd.cpp` (no INET6, stub EventFd/Select, no Sysctl/Ioctl/RecvMMsg/SendMMsg); **fd table is `static inline std::array<…,128>` shared by all processes** (`bsd.h:44,182`) | partial | **rewrite clean-room**; carry only the clean fork commits listed in §1 | 6–10 (L-M) | Licence lineage; per-process fd tables needed once qlaunch, applets, systemWeb and a game share bsd (guess); `bsd:a` user flag |
| **ldn:** lp2p GetGroupInfo returns InvalidState outside a group; the ldn:m/lp2p:m monitors | `R:Ldn/IMonitorService.cs`, `Lp2p/ISfMonitorService.cs` | `E:ldn/sf_monitor_service.cpp:31` (success with zeros) | partial | port one line + fork `6f50a26a02` | 0.3 (H) | — |

### 3.3 Social: friends, presence, invitations, push, ovln, npns

| Harbor feature | Ryujinx file(s) | Eden file(s) | Eden status | Verdict | Effort (conf.) | Risks |
|---|---|---|---|---|---|---|
| **friend:* command table** (about 107 commands, permission from the port name) served from OpenPak caches for the open, linked user | `RH:Sdk/Friends/Detail/Ipc/FriendService.cs`, `OpenPakFriends*.cs`, `Detail/*Impl.cs` | `E:friend/friend.cpp:26-140` (about 20 empty stubs, the rest nullptr) | partial | reuse fork `ddbcea330f` + `fb2cd49363`, `feba677913`, `58f387cef7`, friend hunk of `b901520d0c`, on OPC `friend_structs`/`baas` | 1.5 (H) | Profile source must be the firmware user (§2 item 1) |
| Harbor-only friends additions: 10110 GetFriendProfileImage (JPEG cache), 10502 = 10500, 10503 → 2121-0092 (fw 23), 20900 → ovln 7001 | same | none (the fork's 10110 returns size 0) | missing | port; add a picture cache to OPC | 1.5 (M-H) | Without 10110/10102 overlayDisp silently drops every pop-up |
| **Presence publish/read** (10600/10601/10610 → REST PATCH on change; INACTIVE/ONLINE/PLAYING) | `RO:OpenPakPresence.cs`, `R:Account/OpenPak/OpenPakSession.Invitations.cs:267` | stubs | missing | reuse-OPC `presence_state` + fork `912d991a1d`, re-hooked at am app start/exit | 0.5 (H) | fw23 31000/31001 presence override: caller unknown, missing in Harbor too |
| **Invitations, receive and send** (22000–22003, 22010, 30900/30901, 30910/30911; Five inbox) | `FriendService.cs:2277-2346`, `OpenPakSession.Invitations.cs`, `RO:OpenPakBaas.cs` | 22010 = 0, the rest nullptr | missing | reuse fork (hand-written 30900/30901, because Eden's serializer misreads X+A buffers) + OPC | 1 (M-H) | Never exercised through the firmware MyPage on Eden |
| **Accept → launch into the game.** myPage → SAMS 32 → qlaunch → IApplicationAccessor 180 → the title pops `[Uid][app data]` with 141 | `R:Am/FriendInvitationChannel.cs`, `IApplicationAccessor.cs:482`, `IApplicationFunctions.cs:641-660` | `E:am/service/application_accessor.cpp:49` (180 nullptr), `application_functions.cpp:507-523` (pops newest first, event never cleared) | partial | new 180 handler + fork `cdca940531` core hunk | 1 (M) | The path for a title that is not running depends on am CreateApplication (core area) |
| **INotificationService** (FriendListUpdate / NewFriendRequest queue, collapsed at 100 entries) | `RH:Sdk/Friends/Detail/Ipc/NotificationEventHandler.cs` | `E:friend/friend.cpp:356-460` (never filled) | partial | reuse fork `ddbcea330f` | 0.5 (H) | Type 2 (one friend's presence) is missing in Harbor too |
| **Penne push** (penne_ids, login ticket, empty-body frontline POST, length-prefixed FlatBuffers frames), downlink only | `R:Account/OpenPak/OpenPakSession.Push.cs`, `RO:PenneFrames.cs` | none | missing | reuse-OPC `push.cpp` + kind/body hook + PenneId + `OPENPAK_SIMULATE_PUSH_DIR` test hook | 1 (H) | Android background: no foreground service; reconnect backoff 2–300 s (guess) |
| **npns:** npns:s 1/2/5/7/8/26/161/162/201/202, npns:u 1/2/5/7/8/21/23/25/26/101/102 (events never signal) | `R:Npns/INpnsSystem.cs`, `INpnsUser.cs` | `E:npns/npns.cpp` (npns:s 2/5/8/103/106/161; npns:u 5/8) | partial | reuse fork `09f68c5377`, `7fb9883f4b` + port the remainder | 0.5 (H) | Without 162/201/202 qlaunch's Friends "Update" is fatal; overlayDisp needs npns:u 8 and 5 |
| **ovln agent (OvlnAgent):** sources, per-sender queues, one receiver per source, one global FIFO, send options, 2025-xxxx results; empty receive = 2025-0012 (0x1819) | `RH:Ovln/OvlnAgent.cs`, `Ovln/Ipc/{Receiver,Sender,ReceiverService,SenderService}.cs`, `RH:Sdk/Ovln/*`; tests `Ryujinx.Tests/HLE/OvlnAgentTests.cs` | `E:psc/ovln/*` **wrong** (details below the table) | partial (broken) | **rewrite** (port OvlnAgent + tests) | 2 (H) | overlayDisp aborts on any other empty-receive result; layouts must be exact (hand-write Send/ReceiveWithTick if `D<>` reorders them) |
| **FriendsOverlayNotifier** (details below the table) | `RH:Friends/FriendsOverlayNotifier.cs`, `RH:Ovln/OverlayNotificationSender.cs` | none | missing | port; add 7003 (friend request), which Harbor lacks | 1.75 (M-H) | 7001's trigger is inferred; created_at must be within 60 s of the network clock |
| **notif:** 520, 1000, 1010, 1500, 1510 | `R:Notification/INotificationServices.cs` | `E:glue/notif.cpp:217-226` (1010, 1500 nullptr) | partial | port | 0.25 (H) | — |

**What is wrong with Eden's ovln** (`E:psc/ovln/*`):
- `SourceName` is `char[0x16]`; it must be 0x10.
- Each IReceiver keeps a private source map, so senders can never reach it.
- `Send` only logs.
- The argument order is wrong: the real layout is u64 option at +0, then the 0x80-byte message at +8.
- `OpenSender` misreads the input: a 0x10 name, then a u64 whose low u32 is the queue length.
- Receive returns 0x90 bytes instead of 0x88.
- An empty receive returns `ResultUnknown`.
- Tick is always 0.
- 1 GetUnreceivedMessageCount is nullptr.

**What FriendsOverlayNotifier does:** one sender on source "overlay", queue length 16, option 1.
- **7004 (invitation)**, 0x38-byte payload, on a push. It prefetches the sender's picture and bumps the 22010 count.
- **7002 (friend online)**, 0x20-byte payload, sent on an offline→online transition, gated by set:sys AccountNotificationSettings +0x14 (3, or 2 for favourites).
- **7001 (summary)** on friends 20900.

qlaunch answers SAMS 29 with **tag 23** (0x40 bytes) through `ovln:snd`, so the guest-facing sender path is load-bearing too.

### 3.4 News, BCAT, parental controls

| Harbor feature | Ryujinx file(s) | Eden file(s) | Eden status | Verdict | Effort (conf.) | Risks |
|---|---|---|---|---|---|---|
| **qlaunch News** (details below the table) | `R:News/{IServiceCreator,INewsService,INewsDatabaseService,INewsDataService,IDownloadContext,IEventHolder,NewsStore,NewsQuery}.cs`, filled at boot from `Switch.cs:122` | `E:bcat/news/*` (registered in `bcat/bcat.cpp:23-27`) | partial (skeleton) and **conflicting** (details below) | **delete `builtin_news.*`**; keep the Eden IPC shells, msgpack code and record structs (layout fixed); port NewsStore, NewsQuery and IDownloadContext | 5–6 (M) | Record layout partly guessed; 30201's meaning; 50100 (new in 23.x) not yet seen; subscriptions not persisted (Harbor gap) |
| **Title BCAT delivery cache** (host fetch at launch into the real BCAT save), 20301 suspension object, system-app cache created at 5 MiB on first use | `R:Account/OpenPak/OpenPakBcatDelivery.cs`, `RO:OpenPakBcat.cs`, `RH:Bcat/Ipc/ServiceCreator/BcatService.cs`, `DeliveryTaskSuspensionService.cs` | `E:bcat/*` (`NullBcatBackend`, 20301 nullptr, name-validation bugs) | partial | reuse fork `535ce7beb7`, `a497ae63fd`, BCAT hunk of `b901520d0c`; port 20301 | 2–3 (M-H) | Fork save path versus BIS path (guess) |
| **pctl pairing with moon** (details below the table) | `R:Pctl/Pairing/{Moon,PctlPairing,PctlAsync}.cs`, `R:Pctl/IParentalControlService.cs`; tests `Ryujinx.Tests/HLE/PctlPairingTests.cs` | `E:pctl/parental_control_service.cpp:124-147` (2001–2024 nullptr; 2025–2027, 1412 absent) | missing | port, translating the tests alongside | 6–8 (M) | **Harbor's pairing has not been tested on Harbor**: port from golden traces, not from untested code (§5 risk 2) |
| **Setting sync and restriction state.** PIN (`unlockCode`), rating (NACP RatingAge, IARC fallback, 2142-0103), free communication, the 0x44-byte play-timer settings block, PIN commands 1201–1209, sync event 1432; sync after 5 s, then every 10 min | `R:Pctl/Pairing/PctlSetting.cs`, `IParentalControlService.cs` | `E:pctl/*` (session-local `pin_code` that nothing sets; 1201–1205/1207/1208 nullptr; 1003/1019 logged stubs) | partial | rewrite (port PctlSetting); **disable Eden's host `Auth` applet**, which accepts every PIN (`core/frontend/applets/general.cpp:18-31`, wired at `am/frontend/applets.cpp:228`) | 4–5 (M) | Inferred parts (1455, play-timer byte 0); setters 1033/1036/1038/1042/1045 missing in both |
| **pctl push:** `DID_CHANGE_PARENTAL_CONTROL_SETTING` triggers SyncSoon; `DID_UNLINK_NX` goes to ovln 4002; `notificationToken` = Penne id | `R:Account/OpenPak/OpenPakSession.Push.cs:231-241`, `Moon.cs:283-331` | none | missing | port as a hook on the OPC push | 1 (M), after Penne | Unproven on hardware |
| **Play timer** (1451–1460) | `IParentalControlService.cs` (in-memory, not per title, no enforcement) | `E:pctl/*` stubs; 1454 returns s32 where Harbor uses s64; 145601 is registered twice (`:140`, `:148`) | partial | port for parity (1.5 d, M); **real enforcement** (am suspension, the 4001/4003–4005 overlays, persistence, `/events` upload) is new design work | 1.5 (M) + 5–8 (L) | The parity ledger's "pctl 1454 agree" row is stale |
| **pctl overlay** 4000 (changed-settings mask) and 4002 (unlink) | `R:Pctl/Pairing/PctlOverlayNotifier.cs` | none | missing | port once ovln exists | 0.5–1 (M) | Depends on the ovln rewrite and on am starting overlayDisp |
| **set:sys PctlReadyFlag** 156/157 | `R:Settings/ISystemSettingsServer.cs:939-961` | `E:set/system_settings_server.cpp:274-275` nullptr | missing | port | 0.25 (M) | 8-byte layout |

**qlaunch News, Harbor's implementation:**
- news:a/c/m/p/v are registered.
- Implemented commands: 10100 PostLocalNews (stored), 20100, 30100, 30101, 30110 (16 MiB quota), 30200 = false, 30201 = true, 30500 IDownloadContext, 40100, 40101, 40200, 40201.
- INewsDatabaseService 0/1/3/4/1000 runs a SQLite subset for WHERE and ORDER BY.
- INewsDataService 0/1/2/3/1001.
- The store fills from `/openpak/v1/local/*`.

**Eden's News implementation:**
- `news_service.cpp`:
  - 30110 is wired to the wrong handler.
  - 30201 and 30210 answer false, which makes qlaunch say a system update is required.
  - 30300, 30500 and 40100 are nullptr.
- The database ignores WHERE.
- The data service falls back to the first article when a lookup misses.
- `news_storage.h` has no decoration_type at 0x60.
- **`builtin_news.cpp` fabricates host news and downloads from eden-emu.dev and Forgejo/GitHub.** That is a host substitute and contacts a non-OpenPak network.

**pctl pairing, Harbor's implementation:**
- moon endpoints:
  - `GET /moon/v1/devices?filter…pairingCode`
  - `POST …/activate`
  - `GET …/owners`
  - `DELETE …/owners/<na>`
  - `GET …/parental_control_setting` (If-None-Match)
  - the receipt `POST`
- Error mapping 2142-2001..2017. 2142-0234 is a JSON parse abort, not a restriction verdict.
- Async objects have two slots per session; a bad AsyncData returns 2142-0221.
- Pairing commands 2001–2015, 2017 and 2025–2027.
- The dauth client is pctl's own.
- State is kept in `openpak/pctl-<server>.json`.

**News firmware patch.** `scratch/home/openpak-firmware-audit-23.0.x/news-patches/FINDINGS.md` describes two 8-byte bcat-sysmodule "no-dauth" patches for the real console. Harbor applies none of them. Harbor-on-Eden must stay HLE for news and bcat (no dauth or container crypto involved), so neither these nor the 22.5 bcat RSA-modulus swap is needed. Running the real bcat sysmodule (`010000000000000C`) would bring them back.

They are **console** firmware patches beyond CA trust, which is a second exception besides the CTR key swap. That is worth an owner decision for the console side; it is not needed for Harbor.

### 3.5 eShop, ns, olsc, set:sys, erpt, prepo, caps

| Harbor feature | Ryujinx file(s) | Eden file(s) | Eden status | Verdict | Effort (conf.) | Risks |
|---|---|---|---|---|---|---|
| **Shop, Lhub, Login and Share as the real firmware web program.** On 22.x and later: Shop/Lhub(0x35)/Login(0x36)/Share(0x37)/Whitelisted → `0x0100000000001042`, Web/LibAppletAuth → `…1043`, offline web → `…100F`. The host browser is only a logged fallback outside HOME | `R:Am/AppletAE/AllSystemAppletProxiesService/SystemAppletProxy/ILibraryAppletCreator.cs:21-46` (`_firmwareApplets`) | `E:am/am_types.h:111-112` (pre-22 ids `…100A`/`…100B`, which exit at once on 22.x); `am/service/library_applet_creator.cpp:55-99` (no Lhub/Login/Share); `common/settings.h:157-168` (web/shop/login_share/wifi_web_auth default to HLE) | partial (wrong) | port the program-id table; force LLE in HOME mode; make the fallback a logged refusal, not Eden's host `WebBrowser` | 1 (H) + systemWeb bring-up 3–10 (L) | Also missing: fsp-srv 21 DeleteSaveDataFileSystem (`filesystem/fsp/fsp_srv.cpp:67`), `ns:sweb` (`ns/ns.cpp:83-89`), ssl 0xFFFFFFFE. Core area owns the applet launch |
| **nim:shp** 1 RequestCachedDeviceAuthenticationToken, 3 RequestCachedEdgeToken (IAsyncValue 1/2/3); ec async answers `{}`. The shop pages talk to nx-shop themselves from systemWeb | `R:Nim/IShopServiceManager.cs`, `IShopServiceAsync.cs`, `IShopServiceAccessor.cs` | `E:nim/nim.cpp:473-475` (1 and 3 nullptr); ec `{}` already matches (`:31-36`) | partial | port + OPC edge token (§3.1) | 1 (M-H) | — |
| **ns:ec / VGC / rights** (command table below) | `R:Ns/IECommerceInterface.cs`, `IDynamicRightsInterface.cs`, `IVulnerabilityManagerInterface.cs`, `IApplicationManagerInterface.cs` | `E:ns/ecommerce_interface.cpp:12-18` (0–6 only), `ns/async_result.cpp:15-17` (Get nullptr), `ns/ns.cpp:69-73` (3100 zeros), `application_manager_interface.cpp:871` (4042) | partial | port; **keep Harbor's firmware-traced shapes where they differ** | 2.5 (M) | Eden's zero 3100 likely makes systemWeb error (guess); 4042 unresolved in both |
| **ns:am "unknown contract" set:** 1, 45, 49, 300, 518, 935, 1508, 2366, 2367, 4008, 4058, 4084, 4089, 4096; IContentManagement 607; IDownloadTask 701 (details below the table) | `R:Ns/IApplicationManagerInterface.cs:255-401`, `IContentManagementInterface.cs`, `IDownloadTaskInterface.cs` | all nullptr in Eden | missing / conflicting | port, with one `LOG_UNKNOWN_CONTRACT` macro and a list in docs | 1.5 (H) | Eden's signalled 4022 may make qlaunch spin (guess) |
| **ns:ro control data** 23 (fw23) and 20 (used by overlayDisp's friend pop-up) | `R:Ns/IReadOnlyApplicationControlDataInterface.cs` | `E:ns/read_only_application_control_data_interface.cpp:143` (23 ok), 20 missing | partial | reuse 23; port 20 | 0.25 (H) | — |
| **HOME library population** | `R:Ns/HomeMenuScanner.cs`, `HomeMenuLibrary.cs` | `E:ns` ListApplicationRecord + Android `native.cpp:220-272` provider | has (mostly) | reuse; add 1 GenerateApplicationRecordCount and record-update event signalling | 0.5 (M) | Core and frontend boundary |
| **olsc** (qlaunch's command set) | `R:Olsc/*` | `E:olsc/*` (all but 209 and 1001) | has (mostly) | port 209 (answered like 200, unknown contract) and 1001 (23.x, done at once) | 0.5 (H) | — |
| **set:sys persistence** (key list below the table) | `R:Settings/SystemSettingsStore.cs`, `ISystemSettingsServer.cs` | `E:set/system_settings_server.cpp:1487-1530` (persists to `settings.dat` in NAND saves `8000…0050/52/53/54`) | has, with **three first-boot blockers** (below the table) | reuse Eden storage; remove the three blockers behind a Harbor flag; port 156/157/171/202/207–210 | 1–1.5 (M) | Coordinate with core (starter) and frontend (no host region/language override) |
| **erpt accept-and-discard:** 0, 1, 2, 5, 6, 7, 8, 10, 11, 12, 14, 20, 21, 22, 30, 40 (no files written) | `R:Erpt/IContext.cs` | `E:erpt/erpt.cpp:23-39` (0/1/11/12 + old 5) | partial | port | 0.5 (H) | — |
| **prepo:** 20102/20103 | `RH:Prepo/Ipc/PrepoService.cs` | `E:prepo/prepo.cpp` (has these; lacks 40100/40101, 10500) | has (mostly) | reuse + fork's 10500 stub | 0.25 (H) | `core/reporter.cpp` writes play reports with user_id when `reporting_services` is on; keep it off |
| **capsrv → "Captured" overlay** (renderer frame saved via CaptureManager; 96×54 RGBA thumbnail for caps:a 301; ovln "vi" 8004 then 8001/8002/8003) | `R:Caps/OverlayScreenShot.cs`, `IAlbumAccessorService.cs` | `E:caps/*` (capture button only sends an applet message, `am/button_poller.cpp:64-115`; caps:c 1001 nullptr; caps:a 0/6/15–17/100/301/302/501 nullptr, 110/120/140/50012 absent); AlbumManager and `caps_su` CaptureAndSaveScreenshot are reusable | partial | port the notify path and the caps:a commands | 2–3 (M) | Depends on ovln; capture button belongs to core and hid |

**ns:ec / VGC / rights commands:**

| Command | Harbor | Eden |
|---|---|---|
| ns:ec 3 RequestSyncRights | completes with a "no OpenPak rights" log | nullptr |
| ns:ec 8/10 | NA-id-token prefetch | absent |
| ns:ec 9/11 | hand over the BaaS id_token | absent |
| IAsyncResult 0/2 (Get, GetErrorContext) | worker task, real results | nullptr |
| 2050 | one entry, only for library titles | always one entry |
| ns:vm 3100 | `{3, 0, 1, 0}` (systemWeb's web-auth check needs state 3) | zeros |
| ns:vm 3101 | completes | nullptr |
| 4042 (Virtual Game Cards) | open in Harbor | stub returning IAsyncResult on a shared event |

**ns:am shape conflicts** (Harbor's firmware-traced shape against Eden's):

| Command | Harbor | Eden |
|---|---|---|
| 4022/4088 | event never signalled | event signalled |
| 4023 | `(bool, 0x50-byte record)` | u64 |
| 936 | `(s32, 0x80 record)` | u64 |
| IContentManagement 71 | real total/free | zeros |

**Keys Harbor persists in set:sys** (one JSON, `system/openpak-system-settings.json`, written on every Set):
- language, region
- account settings flags, notification and account-notification settings
- lock screen, color set, applet launch flags
- battery %, auto update, push mode on sleep, panel CRC
- hearing protection, error-report share permission
- initial launch flags and timestamp
- sleep, TV settings, primary album storage
- device nickname, keyboard layout
- pctl ready flag
- field testing

**Eden's three first-boot blockers:**
1. The constructor pre-accepts the EULA on every boot (`system_settings_server.cpp:356-364`).
2. The default initial-launch flags are 0x10001 (`setting_formats/system_settings.cpp:40-43`), so starter never runs.
3. Region is overwritten from host settings on every construction (`:353`).

### Effort totals

Ranges come from the per-area research.

| Area | Engineer-days | Confidence |
|---|---|---|
| Accounts (acc, dauth, ns 2366, privacy scrub, tests) | 20–28 | M |
| Network (profile/CA/TLS/DNS/fw23 resolver/nifm/ldn + bsd clean-room 6–10) | 18–27 | M (bsd L-M) |
| Social (friends, push, ovln, notifier, invitations, npns, notif) | 15–18 | M |
| News, BCAT, pctl parity | 21–25 | M |
| eShop/system services (excluding systemWeb bring-up) | 12–16 | M |
| openpak-client changes (§2) | 4–6 | M |
| Fork cherry-pick and conflict resolution (§1) | 3.5–4 | M-H |
| **Subtotal** | **about 94–124** | |
| Low-confidence extras: systemWeb bring-up 3–10, play-timer enforcement 5–8, IAdministrator 290 RE 1.5–3 | 9.5–21 | L |

Some reuse is double-counted between areas, for example the OPC session in accounts and in friends. The realistic subtotal is about 85–110 d.

---

## 4. Phase 3 order and parallelism

### Wave 0: shared foundations

Run these serially, one owner. Wave 0 touches core and am/acc, and every later stream builds on it.
1. **Vendor openpak-client** (client target only) and agree the data-directory contract with the host area:
   - `Platform::SetDirectories`, `SetClient("harbor", …)`, `SetProfileSource`;
   - the `server.json`/`ca.pem`/`network-profile.json`/`network-ceiling*` files written by the host wizard before `Core::System::Load`.
2. **One ProfileManager and console mode.** Share one instance between `core.cpp` and `acc.cpp`; no default user; zero-user boot safe (frontend, `launch_params`, Android JNI). Meets the core area at starter launch (`IApplicationCreator` 10) and the frontend area at the profile UI.
3. **Async infrastructure.**
   - An `AsyncWorker` for `IAsyncContext`/`IAsyncResult`/`IAsyncValue` (worker thread, KEvent signal, cancel).
   - A **session-owned signalling thread** for host→guest events. The fork found that host threads signalling get a stale dummy KThread across sessions.

   Used by acc, ns, nim, pctl, friends and news.
4. **Logging policy.** A redaction helper and an `LOG_UNKNOWN_CONTRACT` macro, plus the §5 scrub of existing Eden lines. It is cheap and must come before any OpenPak data flows.
5. **OvlnAgent rewrite** (`E:psc/ovln/*`), with tests ported from `OvlnAgentTests.cs`. It is independent of everything else and unblocks friends, pctl, caps and am overlays. Meets the core area: am must start overlayDisp (`applet_manager.cpp:268`, `enable_overlay`) and implement IAppletCommonFunctions 52/60–62.

### Wave 1: parallel, separate engineers, mostly disjoint files

| Stream | Files | Depends on | Meets |
|---|---|---|---|
| **N1 TLS + resolver + nifm** | `E:ssl/*`, `E:sockets/{sfdnsres,nsd}.*`, `E:nifm/*`, `E:ldn/*` | W0.1 | frontend (CA and profile files); bsd eventfd for fw23 100/102 (stub interface first) |
| **N2 bsd clean-room** | `E:sockets/bsd.*`, `sockets_translate.*`, `core/internal_network/*`, `server_manager` deferral | none. **Must be done by someone who has not read the tainted diffs or `feature/android-port`'s IClient** | core (per-process fd tables, deferral in `server_manager.cpp`) |
| **A acc identity** | `E:acc/*`, `ns/application_manager_interface.cpp` (2366/4105), `ns/async_result.cpp`, OPC session/NA OAuth | W0.1–3 | core (starter, PlayerSelect, Login applet 0x36 program id), frontend (secret store) |
| **D News** | `E:bcat/news/*` (delete `builtin_news.*`), `bcat/*` BCAT fixes | W0.1, W0.3 | none beyond the host fetch |
| **S system services** | `E:set/*` (blockers, 156/157 …), `erpt`, `prepo`, `olsc`, `ns` unknown-contract set, `ns:ro` 20, `ns:vm` 3100/3101, `glue/notif.cpp`, `npns` | W0.4 | core (starter and initial-launch flags; ns max_sessions = 5 from `8a22f1845b` with qlaunch, overlayDisp, systemWeb, starter and a game at once, guess) |

### Wave 2: needs A and W0.5

| Stream | Files | Depends on | Meets |
|---|---|---|---|
| **F friends + push + invitations** | `E:friend/*` (cherry-pick `ddbcea330f` and its follow-ups), OPC push hook, FriendsOverlayNotifier, `am/service/application_accessor.cpp` 180, `application_functions.cpp` 140/141 | A (BaaS token, uid mapping), W0.5 | core (IApplicationAccessor 180, general-channel FIFO at `core.cpp:999`, CreateApplication path for invitations) |
| **P pctl** | `E:pctl/*`, set:sys 156/157, disable host `Auth` applet | A (dauth), F (Penne hook), W0.5 | core (am suspension for play-timer enforcement, a later design) |
| **C caps notify** | `E:caps/*` | W0.5 | core and hid (capture button), vi (frame grab) |
| **W systemWeb / eShop / Lhub / Login** | `E:am/am_types.h` ids, `library_applet_creator.cpp` policy, `nim/nim.cpp` 1/3, `ns/ecommerce_interface.cpp` 3/8–11, fsp 21, `ns:sweb` | N1 (TLS 0xFFFFFFFE, redirect), A (NA token), S (ns:vm) | core owns guest applet launch and memory pools. **Measure first** (one desktop run with corrected ids), then commit |

**Strict order:** W0.2 → A (users must exist before linking) → F/P. A's IAdministrator 250 and its 120/136 consistency gate everything linked in qlaunch.

**Independent of A:** N1, N2, D, S and W0.5 can run fully in parallel.

**File overlap to manage:** `E:ns/application_manager_interface.cpp` is touched by A (2366/4105) and S (unknown-contract set). Give S that file and let A add 2366 through S, or sequence them.

---

## 5. Top risks and unknowns

1. **Licence lineage (high impact, certain).**
   - bsd code of unclear provenance exists in:
     - the old fork: `73da2f25b7`, `497320b975`, `fd1b755fc4`, and by lineage `03d14d62d1` and `fa69cbe39f`;
     - Harbor's own `feature/android-port`: `3ab97fbe7`, `081d5cd88` (the `154744710` cherry-pick is still pending there);
     - Citron's online path, so Citron-sourced bsd code has unknown provenance.

   *De-risk:* N2 is written clean-room by someone who has not read those diffs, from the spec listed in §1. Add a per-function provenance note. Decide `154744710` for `feature/android-port` separately (owner).
2. **Porting behaviour Harbor never proved.** These are unverified on Harbor itself:
   - pctl pairing, sync and push ("untested on Harbor");
   - the ovln pop-up and Join path;
   - fresh NA registration through ns 2366 (owner retest pending);
   - linked-account ns 4105 work;
   - qlaunch's path after IAdministrator 250 = true.

   *De-risk:* before porting each, run it once on Harbor with a test account. Save IPC traces as golden files, then diff Eden's replies against them.
3. **systemWeb on Eden (largest unknown).** WebKit NRO, memory pool, ro/ldr, CFI, Android performance; neither Eden nor Citron runs it on 22.x or later. Shop, Lhub, the Login applet (and so NA linking) all need it.

   *De-risk:* one desktop run with the corrected program ids, LLE mode and guest internet on, adding one missing command per run. Measure on the phone before committing to stream W.
4. **Fail-open DNS → real Nintendo contact.** An unredirected ceiling-family name resolving through host DNS risks a ban (2181-4008). The fork narrowed the blocklist, and Harbor's hosts block fails open when the console server does not resolve (`online-home-plan.md` D3).

   *De-risk:* fail closed for every ceiling family whenever OpenPak is on, with unit tests over the families. `nncs2-*` stays a profile override, never collapsed onto the server.
5. **Async and threading in Eden.** Eden's `IAsyncContext` has no worker. Host threads signalling guest events bind a stale dummy KThread across sessions. The fork's acc signs in on the IPC thread.

   *De-risk:* do W0.3 first. Test two emulation sessions back to back, and test that IPC never waits on the network.
6. **TLS trust model decision.**
   - Upstream Eden forces verification off (unsafe).
   - The fork honours the guest's verify option.
   - Harbor always verifies host-side and accepts the OpenPak CA.

   Recommend Harbor's model: always verify, never accept an unverified certificate for ceiling families. Honouring verify-none for third-party hosts is an owner call.

   *De-risk:* test Stardew (1033), the web applet (0xFFFFFFFE), Moving Out 2 (Peek) and one verify-none title.
7. **ovln layouts and empty-receive result.** overlayDisp aborts on anything but 2025-0012. Eden's serializer may reorder raw fields.

   *De-risk:* make the empty-receive and 0x88 ReceiveWithTick unit tests pass before any sender is enabled. Hand-write Send and ReceiveWithTick if needed (as the fork did for friends 30900/30901).
8. **Unknown 23.x contracts:**
   - IAdministrator 290 (blocks PC account portal);
   - news 50100 and 30300;
   - pctl 2016/2019/2021–2024/3001 and 1954–1957;
   - ns 4042 (Virtual Game Cards);
   - friends 31000/31001;
   - the layout of the fw23 resolver 100/102/104.

   *De-risk:* RE in the 23.0.1 modules (scratch Ghidra projects exist). Parse `dnsmitm/capture/dns_mitm_raw.log` offline for 100–104. Never answer a guessed contract as done; log it under `LOG_UNKNOWN_CONTRACT`.
9. **Eden's global 128-entry bsd fd table** (`bsd.h:44,182`) with many guest processes. Guess: it collides or fills once qlaunch, applets, systemWeb and a game share it, and fw23 eventfd signalling needs a per-process lookup.

   *De-risk:* count peak fds in a Harbor web-applet run; key the tables by client process in N2.
10. **First-boot hacks in Eden** (EULA pre-accept, initial-launch default 0x10001, host region override) bypass starter.

    *De-risk:* remove all three behind a Harbor flag in S; test a fresh NAND to HOME.
11. **openpak-client is private.** AGPL/GPL distribution needs public source.

    *De-risk:* publish it, or vendor it into Harbor, before the first Harbor-on-Eden release.
12. **Secrets at rest.** These are in plain text today:
    - the device-account JSON (in Harbor and openpak-client);
    - `pctl-<server>.json`, which holds the PIN, owner NA ids, nicknames and the device id.

    *De-risk:* host secret-store hook (Android Keystore through JNI) in W0.1 and the host area's scope.
13. **ns max_sessions = 5** (upstream `8a22f1845b`) with several firmware clients. Guess.

    *De-risk:* count ns sessions in the first qlaunch + overlayDisp + shop + game run.
14. **BCAT save path** (fork `save/bcat/<tid>` against the BIS `GetBCATDirectory`). Guess.

    *De-risk:* one BCAT title (parity ledger §6 item 6).

### Privacy: logging that breaks Harbor's rule

Harbor's rule: never log account ids, tokens, names, nicknames, PINs, pairing codes or typed text. Fix all of the following in wave 0.4.

**Eden upstream**
- **acc:**
  - `acc/acc.cpp:539` and `:572`: nickname plus uid.
  - `acc/acc.cpp:421, 438, 445, 885, 916, 1033, 1044, 1055, 1057, 1072, 1084, 1095, 1125, 1138, 1150`: uid.
  - `acc/profile_manager.cpp:390`: uid.
- **friend:** `friend/friend.cpp:205, 219, 239, 265` (WARNING) and `:320, :487`: uid.
- **ovln:**
  - `psc/ovln/sender.cpp:28`: hex-dumps every 0x80 notification (uid, friend, invitation and group ids). **Fix first.**
  - `psc/ovln/sender_service.cpp:24`: dumps the raw OpenSender data.
- **am:**
  - `am/service/application_functions.cpp:158, 237, 252`: uid.
  - `am/frontend/applet_general.cpp:22-37`: hex-dumps stubbed applet input.
- **Typed text:**
  - `core/frontend/applets/software_keyboard.cpp:118-121`: inline typed text at INFO.
  - `am/frontend/applet_software_keyboard.cpp:244, 297, 366, 373, 396, 406`.
- **URLs:** `am/frontend/applet_web_browser.cpp:380, 465, 600`. These log `last_url` and the external URL, which on this flow would include the OAuth callback with code and state.
- **ns:**
  - `ns/application_manager_interface.cpp:492` (uid), `:753` (account id at WARNING on every rights query).
  - `ns/query_service.cpp:63` (account id).
- **olsc:** `olsc/daemon_controller.cpp:38, 57, 64, 71, 83, 90, 103` and `olsc_service_for_system_service.cpp:121`: uid.
- **set:** `set/system_settings_server.cpp:1192`: device nickname.
- **nim:** `nim/nim.cpp:135`: shop Prepare URL.
- **TLS secrets:** `ssl/ssl_backend_openssl.cpp:310-321` KeyLogCallback writes TLS secrets to the log at DEBUG when `SSLKEYLOGFILE` is set. Compile it out of release builds.
- **Host network names:** `nifm/nifm.cpp:685` and `internal_network/wifi_scanner.cpp:85` log host SSIDs (location data).
- **Unimplemented-command report:** `service.cpp:70-76` dumps raw command words, which can contain uids, into reports.
- **web_service:** `web_backend.cpp:85/116/123/131/136` (URLs; **136 logs the response body**), `verify_user_jwt.cpp:52`, `announce_room_json.cpp:36` (nickname).
- **Settings dump:** `common/settings.cpp:116-128` LogSettings dumps every setting except `eden_token`, including any OpenPak setting the port adds. Mark new OpenPak settings as non-logged.

**Old fork (do not carry)**
- `EDEN_SSL_TRACE` (`ssl.cpp:72-96, 271, 330-348`): plaintext requests, including Authorization and Cookie headers.
- `bsd.cpp` around line 2283: the user's public IP from the NAT check.
- `openpak_friends.cpp:917, 920, 957`: thumbnail URL and presence appField.
- `openpak_friends.cpp:714–2147` (many lines): ids at WARNING.

**openpak-client:** see §2 item 5.

**Harbor itself (fix there, don't port)**
- `R:Sockets/Bsd/Impl/ManagedSocket.cs:418-470`: BsdTap hex dumps are on by default.
- `R:Ssl/SslService/ISslConnection.cs:54-56`: t17service hosts are traced without any env var.
- `RH:Sdk/Friends/Detail/Ipc/FriendService.cs:427, 1022`: NSA ids and uid.
- `R:Account/OpenPak/OpenPakSession.cs:472, 1086, 1223`: device account id, sender id, profile id.
- `R:Account/Acc/ApplicationServiceServer.cs:104` and `IAccountServiceForAdministrator.cs:269`: uid.

---

## Meeting points with the other phase-0 areas

**Core area (kernel, am/applets, hid, vi)**
- **acc ↔ starter:**
  - `IApplicationCreator` 10;
  - acc:su 200/201/205/206, 52, 401/405/410;
  - IProfileEditor 100/101/30;
  - 230/211/213 with transfer memory → Login applet 0x36 → `0x0100000000001042`;
  - one ProfileManager.
- **ovln ↔ overlayDisp:**
  - am starts overlayDisp;
  - IAppletCommonFunctions 52/60–62 (nullptr at `applet_common_functions.cpp:29-32`);
  - ICommonStateGetter 20 PushToGeneralChannel;
  - the general channel must be FIFO (`core.cpp:999` pops `back()`);
  - set:sys 75 and 31;
  - npns:u 8 and 5;
  - friend:v 10102/10110/20109.
- **Invitations:** IApplicationAccessor 180 and IApplicationFunctions 140/141 with a per-applet channel filled before Start.
- **Presence:** an am application start/exit hook replaces `System::Impl::Load` (`912d991a1d`).
- **Web programs:** LLE program-id table, memory pools, fsp-srv 21.
- **Caps:** capture button → caps / ovln "vi".
- **Play-timer enforcement:** am suspension (future design).
- **sm:** pointer buffer (`13baceaea8`, `4373aa192f`).
- **bsd:** per-process fd tables and `server_manager` deferral.

**Host area**
- **Files written before boot:** the OpenPak data directory (`server.json`, `ca.pem`, `network-profile.json`, `network-ceiling*`) by the first-run wizard, and the 6 h ceiling/profile refresh on the host.
- **Secret store:** Android Keystore through JNI, plus libsecret on desktop, for device accounts and pctl state.
- **Settings location:** decide whether to migrate the owner's phone data (Eden `settings.dat` against Harbor JSON) or break it, since the alpha may break.
- **Applet modes:** force LLE in HOME mode and hide the toggles.
- **First-boot settings:** no host language/region override of set:sys; initial-launch flags.
- **Lifecycle:** Android pause/resume policy for the Penne push.
- **No host UI** for anything firmware does: sign-in, friends, invitations, MyPage, toasts and the PIN applet.
