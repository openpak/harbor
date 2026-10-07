# Harbor on Eden: log lines that break the privacy rule

Harbor's rule: never log account ids, uids, tokens, names, nicknames, PINs, pairing codes or typed
text. This list is services.md §5 checked against `harbor` at `721349fbc4` (2026-10-07). Line
numbers are for that commit. Each owner fixes their own files.

**Helpers** (in `src/common/redact.h`):
- `Common::Redact(x)` formats as `<redacted>`, whatever `x` is (a `UUID`, a `u64`, a string).
- `Common::RedactLength(text)` formats as `<redacted, N bytes>`.
- `Common::RedactUrl(url)` keeps the scheme and host and drops the rest: `https://host/...`.
- `LOG_UNKNOWN_CONTRACT(Class, "service cmd ...", args...)` logs a warning that starts with
  `unknown contract: `.

The usual fix is to drop the value or wrap it, for example
`LOG_DEBUG(Service_ACC, "called user_id={}", Common::Redact(user_id))`. A line that only says
"called" loses nothing.

## acc and ProfileManager (am stream: user registration, ProfileManager; A stream: the rest of acc)

| File:line | What it logs |
|---|---|
| `core/hle/service/acc/acc.cpp:1033, 1044` | uuid (INFO): acc:su 200/201, am stream (fixed on `p2/am`, `cd2eadddbd`) |
| `core/hle/service/acc/acc.cpp:1084` | user_id: acc:su 205 GetProfileEditor, am stream (fixed on `p2/am`, `cd2eadddbd`) |
| `core/hle/service/acc/profile_manager.cpp:421` | uuid: am stream (fixed on `p2/am`, `cd2eadddbd`) |

Resolved on `p3/acc`: `acc.cpp:539, 572` (IProfileEditor Store/StoreWithImage username and uuid),
`421, 438, 445` (IProfile), `885, 916` (GetUserExistence, GetProfile), `1055, 1057` (DeleteUser),
`1072` (SetUserPosition), `1095` (GetBaasAccountAdministrator), `1125, 1138, 1150` (thumbnails,
GetBaasAccountManagerForSystemService). The new acc code (Nintendo Account procedures, BAAS
managers, `openpak_account.cpp`) logs no uid, nickname, token, NA id or URL past its host.

## friend (F stream)

| File:line | What it logs |
|---|---|
| `core/hle/service/friend/friend.cpp:206` | uuid and pid (WARNING) |
| `core/hle/service/friend/friend.cpp:245, 265, 291` | uuid (WARNING) |
| `core/hle/service/friend/friend.cpp:346, 513` | uuid |

## am (am stream)

| File:line | What it logs |
|---|---|
| `core/hle/service/am/service/application_functions.cpp:158` | uid (INFO) |
| `core/hle/service/am/service/application_functions.cpp:251, 266` | user_id |
| `core/hle/service/am/frontend/applet_general.cpp:27-29, 34-36` | hex dump of stubbed applet input (it can hold uids and typed text) |
| `core/hle/service/am/frontend/applet_software_keyboard.cpp:244` | initial text |
| `core/hle/service/am/frontend/applet_software_keyboard.cpp:297` | text check message |
| `core/hle/service/am/frontend/applet_software_keyboard.cpp:366, 373, 396, 406` | submitted text |
| `core/hle/service/am/frontend/applet_web_browser.cpp:380, 465` | `last_url`. On the Nintendo Account flow this is the OAuth callback with the code and state. Use `RedactUrl` |
| `core/hle/service/am/frontend/applet_web_browser.cpp:600` | external URL (INFO). Use `RedactUrl` |

## ns, set, ovln (svc stream)

| File:line | What it logs |
|---|---|
| `core/hle/service/ns/application_manager_interface.cpp:495` | user_id |
| `core/hle/service/ns/application_manager_interface.cpp:760` | account id, at WARNING on every rights query |
| `core/hle/service/ns/query_service.cpp:63` | account id (WARNING) |
| `core/hle/service/set/system_settings_server.cpp:1185` | device nickname (INFO) |

Resolved: `psc/ovln/sender.cpp` and `sender_service.cpp`. The OvlnAgent rewrite logs only the tag, the size and the source name.

## olsc (S stream; svc, if olsc stays with it)

| File:line | What it logs |
|---|---|
| `core/hle/service/olsc/daemon_controller.cpp:38, 48, 57, 64, 71, 83, 90, 103` | user_id (INFO) |
| `core/hle/service/olsc/olsc_service_for_system_service.cpp:121` | uuid (WARNING) |
| `core/hle/service/olsc/olsc_service_for_application.cpp:58` | account id (WARNING) |

## nim (A stream)

Resolved on `p3/acc`: `nim.cpp:135`, the shop Prepare URL, now goes through `RedactUrl`.

## ssl, nifm, internal_network (net stream)

| File:line | What it logs |
|---|---|
| `core/internal_network/wifi_scanner.cpp:85` | host Wi-Fi SSIDs (Windows build only). Unreachable in Harbor: nothing calls `ScanWifiNetworks` since nifm became wired-only. The file is not the net stream's; drop the line when it is next touched |

Resolved on `p3/net`:
- `ssl/ssl_backend_openssl.cpp`: `KeyLogCallback` and the `SSLKEYLOGFILE` handling are compiled only
  without `NDEBUG`, so a release build has neither. The secrets go to the file, never to the log.
- `nifm/nifm.cpp`: the Wi-Fi scan is gone (wired only, an empty scan), and with it the host SSID line.
- `ssl`: the host names a connection verifies against are logged (they are server names, not user data);
  no request, header or certificate content is.

## Core, frontend and common (coordinator)

| File:line | What it logs |
|---|---|
| `core/hle/service/service.cpp:64-76` | `ReportUnimplementedFunction` puts the raw command words, which can hold uids, into the log and the report. Keep the command id, drop the words (or keep them at TRACE only) |
| `core/frontend/applets/software_keyboard.cpp:118-121` | inline input text (INFO) |
| `core/frontend/applets/software_keyboard.cpp:40, 85` | keyboard parameters, including guide, header and initial texts (INFO). Check before keeping |
| `common/settings.cpp:116-137` | `LogSettings` dumps every setting except `eden_token`. Any OpenPak setting that is added must be hidden the same way |
| `web_service/web_backend.cpp:85, 116, 123, 131` | request URLs |
| `web_service/web_backend.cpp:136` | the response body |
| `web_service/announce_room_json.cpp:36` | a room member's nickname |

`web_service` is not built for Harbor (`ENABLE_WEB_SERVICE=OFF`), so its lines are low priority.

## Not to carry from the old fork

- `EDEN_SSL_TRACE` traced plaintext requests, including the Authorization and Cookie headers.
- `bsd.cpp` near line 2283 logged the user's public IP from the NAT check.
- `openpak_friends.cpp` logged ids, thumbnail URLs and the presence appField.

## The OpenPak layer (`src/core/openpak`, core stream): done

The lines services.md §2 item 5 lists were scrubbed as the code was ported:
- the sign-in line logs no nickname and no device id;
- failed responses log the status and the body size, not the body;
- `SendAs` and the friends transport log the host, not the path (paths carry user ids);
- the friend-request lines log no friend code, request id or target id;
- marking an invitation read logs no invitation id.

The website sign-in lines (`api.cpp`) were not ported.
