<!-- SPDX-FileCopyrightText: 2026 tobagin -->
<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Firmware service startup investigation

The S25 FE RC3 High-accuracy log contains 532 warning/error occurrences with 151
unique messages, including parameter variations. These are observations, not 151
independently diagnosed bugs.

## Services reported as not registered

| Services | Guest firmware owner | Observation |
| --- | --- | --- |
| friend:u, friend:v, friend:m, friend:s | friends, 010000000000000E | Probed during boot; HLE intentionally yields these names to installed firmware. |
| bcat:u, bcat:s | bcat, 010000000000000C | Boot deferrals; later BCAT network activity is present. |
| prepo:u, prepo:s | bcat, 010000000000000C | Boot deferrals; later prepo settings and task activity are present. |
| olsc:s | olsc, 010000000000003E | Boot deferrals; registration timing needs explicit logging. |
| npns:s | npns, 010000000000002F | Boot deferrals, followed by a separate fatal at about 25 seconds. |

All not-registered warnings in this capture occur before 7.1 seconds. This alone
does not prove every provider later registered successfully. The follow-up logs
guest service registrations with owner program IDs, elapsed time from the first
pending lookup, and retry counts. Lookups that keep retrying still warn every 30 seconds; session shutdown also
reports unresolved names even if no further retry occurred.
Access checks now precede port lookup: firmware clients probe lower-privilege
names deliberately, and access-denied probes must not be counted as missing
providers. NPDM diagnostics distinguish expected firmware-owned names from names
with no configured provider. No substitute service is registered over guest
firmware as part of this change.

## Confirmed contract mismatches

- ARP reader commands 1, 2 and 3 changed in HOS 10. Harbor's old table interpreted
  modern command 3 process IDs as title IDs for a control-property lookup. The new
  table follows the installed firmware version and implements instance lookup and
  command 6 (GetMicroApplicationInstanceId). Harbor uses process IDs for its
  application instance IDs, resolving metadata through each live process's program
  ID. Process properties carry ProgramIndex and follow the HOS 17 size change.
  The legacy reader table remains available for older firmware. Modern ARP writer
  registration/update interfaces are outside this correction and remain incomplete.
- BGTC task command 200 is EnableStayHalfAwake. The npns client wrapper at
  NSO offset 0x4DF70 invokes vtable slot 0x98 with no input payload and aborts on
  failure; the captured fatal immediately follows Harbor's unimplemented response.
  The follow-up accepts the enable request on the task object and exposes its
  state to command 103. Harbor still reports full-awake operation; this does not
  implement console hardware sleep or wake-on-WLAN.

Contracts: [Glue services](https://switchbrew.org/wiki/Glue_services).
Firmware binary excerpts remain in local investigation scratch, not in this repo.

## Remaining observations

- The OpenPak duplicate-device-identity rejection clears later in the capture;
  it does not establish a persistent identity failure.
- SSL command 36 is GetSessionTicket (HOS 20+), per
  [SSL services](https://switchbrew.org/wiki/SSL_services). Its response format
  and backend session-ticket handling still need investigation. NIFM commands
  43/57 also remain unimplemented. The installed npns client wrappers at NSO
  offsets 0x8F1C0 and 0x8F2E0 both marshal one 32-bit input through the same IPC
  helper; timeout validation and wake behavior need further tracing. NIFM 43 is
  documented as SetWowlDelayedWakeTime and 57 as SetWowlTcpKeepAliveTimeout:
  [Network Interface services](https://switchbrew.org/wiki/Network_Interface_services).
- Account resource-cache refresh command 132 remains unimplemented; it needs a
  real asynchronous refresh with account availability/error propagation.
- Missing NACP/control data and base RomFS on system programs are not evidence
  that Splatoon 3 content is damaged. Their loader paths need classification by
  program type before changing severity or inventing metadata.
- The absent optional TAS directory, audio underruns, power-state acknowledgment
  timeouts, stubbed kernel info and the remaining hardware-facing service calls
  have not been corrected by this patch.
- The shader fallback and the root cause of the retained NCE cache protection
  remain under investigation. Neither is proven to cause the rendering artifacts.

All changed Android translation units compile. Device acceptance remains pending.
