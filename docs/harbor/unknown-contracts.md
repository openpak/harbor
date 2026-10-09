# Unknown contracts, Eden side

Every place where Harbor answers a command whose layout or meaning is not established by firmware
reverse engineering or the Ryujinx Harbor's evidence, or where it accepts a command whose console
effect is not emulated. Each such site logs `unknown contract: ...` (the `LOG_UNKNOWN_CONTRACT`
macro in `src/common/redact.h`) on every call, and none may be declared complete (phase 2 rules,
G2.AM.19, G3.PRV.6).

- The logged text says what is answered and why. Texts that contain "not emulated:" are commands
  whose meaning is known but whose console effect this system does not have (dock firmware,
  gameplay recording, the copyright overlay, system zoom, the backlight); the others are guesses
  (a value, a layout or a result that is not established).
- The table below the marker is generated from the source:
  `scripts/harbor/unknown-contracts.py --ryujinx <ryujinx-android checkout> --write`
  (read-only use of the Ryujinx Harbor checkout). Regenerate it after adding or removing a site.
  Sites whose "unknown contract: " prefix is a runtime argument (hid:sys 548/551 share one log line)
  are not picked up; hid:sys 551 is one.
- am has no "(STUBBED)" log left (2026-10-08): every former stub either answers as a console does
  (state kept per applet and answered back; no such hardware here, such as GPU time slices or a
  CPU boost) and logs at debug, or logs as an unknown contract with its reason.
- acc and ns, established from 23.0.1 and no longer unknown (2026-10-09): acc IAdministrator 141
  (0x0859F2B0: refuses 2124-0200 without a BAAS registration, then 2124-0220 without a linked
  Nintendo Account, before the refresh); IManagerForApplication 130/136 (0x1fa5c0:
  NasUserBaseForApplication 0x68 as an auto-select buffer, a work buffer of at least 0x800, the
  Nintendo Account id out); acc:su 112 (0x19ac70: 395 when the save's thumbnail header is not one
  110 wrote, which on Harbor is always). Still unknown: acc 142 (answered as 141, the seconds
  threshold ignored); ns ROACD 4 (the candidates' layout is established, the output type is
  inferred); ns 936 (logged once, then at debug: qlaunch polls it every second).

## Reconciliation with the Ryujinx Harbor

The Ryujinx Harbor's list is its `Unknown contract:` log sites (`feature/android-port`) and the
"Open unknown contracts" section of `docs/android-port/qlaunch-probe.md`. Items of that list with no
Eden-side site, and what Eden does instead (2026-10-08):

| Ryujinx Harbor item | Eden | Owner |
|---|---|---|
| acc: resume of an unknown sign-in session; IOAuthProcedureForUserRegistration 220; acc:su 215 | acc's OAuth procedures (`acc/oauth_procedure*.cpp`) are a separate port; not reviewed here | acc |
| IProfileEditor 30 (0x10 zero bytes) | no IProfileEditor 30 handler found | acc |
| ILibraryAppletAccessor 10 / IApplicationAccessor 10 Start on a started process (success, no restart) | Start runs the process again with no check | am |
| IProcessWindingController 11 with no reserved applet (NotAvailable) | Eden answers the caller applet's accessor (a different answer, not logged) | am |
| IProcessWindingController 22 on an empty stack (NotAvailable) | now the same, logged | done |
| IOverlayFunctions 70 (no effect) | now logged the same way | done |
| IOverlayFunctions 10/11 (power menu, not implemented there) | implemented by the power stream (shutdown and reboot sequences) | power |
| IOverlayFunctions 40/41 while a title runs (logo not served) | logged the same way | done |
| IApplicationAccessor commands on a system application (NotAvailable) | not distinguished | am |
| appletAE 300 OpenOverlayAppletProxy from a pid am did not launch | served without a log | am |
| ICommonStateGetter 15 (overlayDisp's sleep-lock wait) | implemented by the sleep stream (GetSleepSequenceIdleEvent) | done |
| ICommonStateGetter 20 general-channel messages 8 and 0x1C | not reviewed | am |
| ns IApplicationManagerInterface 4022, 4053, 4088 | "(STUBBED)" logs, not unknown contracts | ns |
| ns IContentManagementInterface 71 (taken as GetStorageSize) | "(STUBBED)" log, not an unknown contract | ns |
| ns 4096, 935, 936, 4008, 4023, 4058, 4084, 4089, 4042, ns:vm 3100, ns:ec | logged on Eden (see the table); 4008 now answers an IAsyncValue as 23.0.1 does; 4074 and ROACD 28 now answer as 23.0.1 ns does (logged) | done |
| caps:a 18 GetAppletProgramIdTable and other caps:a commands | caps:a 17/110/120 logged; 18 answered without a log | caps |
| pctl 1459 layout; 1406/2015 time report; 1455; 2017's owner argument; transport failure 2142-2003; picture cut to fit; 2012 string shape | 1204, 1205, 2012, 2017 and 2142-2003 logged on Eden; 1406, 1455, 1459 not | pctl |
| notif:s 1500/1510 layout | implemented with a debug log | glue |
| set:sys 157 SetPctlReadyFlag layout (8 bytes kept as written) | 156 logged; 157 not | set |
| ssl CaCertificateId 0xFFFFFFFE | not reviewed | ssl |
| friends 20900 as the 7001 trigger | not reviewed | friend |
| INewsService 30101 GetTopicList | not reviewed | news |
| nifm 25 size | now logged (answers u32 0) | done |
| olsc 209 | logged on Eden as "olsc:s 209" | done |
| audctl 5000 (a new session there) | logged; Eden answers the same audctl object | done |
| set:sys 0/57 rejections | logged on Eden | done |

<!-- generated:start -->
165 Eden-side sites (generated by scripts/harbor/unknown-contracts.py).

| Area | Site | What is logged (answer and reason) |
|---|---|---|
| common | `src/common/redact.h:14` | ns:am2 4042 answered success with no data |
| acc | `src/core/hle/service/acc/acc.cpp:322` | acc IAdministrator 111, nothing to invalidate |
| acc | `src/core/hle/service/acc/acc.cpp:328` | acc IAdministrator 113, answering 0 |
| acc | `src/core/hle/service/acc/acc.cpp:335` | acc IAdministrator 400, answering false |
| acc | `src/core/hle/service/acc/acc.cpp:342` | acc IAdministrator 401, an async context already done |
| acc | `src/core/hle/service/acc/acc.cpp:467` | acc IAdministrator 142 ({} s), answered as 141 |
| acc | `src/core/hle/service/acc/acc.cpp:1761` | acc:su 230 dauth failure answered as InternetRequestDenied |
| acc | `src/core/hle/service/acc/oauth_procedure.cpp:129` | acc GetRequest: one receive-list entry, URL and callback written back to back |
| acc | `src/core/hle/service/acc/oauth_procedure.cpp:156` | acc resume of an unknown procedure session, refused with InvalidArgument |
| am | `src/core/hle/service/am/frontend/applet_general.cpp:222` | library applet {} has no host frontend; it answers 0x1000 zero bytes and exits |
| am | `src/core/hle/service/am/frontend/applet_software_keyboard.cpp:941` | swkbd UnsetCustomizedDictionaries: answered done (dictionaries are not used by the host keyboard) |
| am | `src/core/hle/service/am/frontend/applet_web_browser.cpp:554` | web applet shop shim: no host frontend; closed as by the end button |
| am | `src/core/hle/service/am/frontend/applet_web_browser.cpp:559` | web applet login shim: no host frontend; closed as by the end button |
| am | `src/core/hle/service/am/frontend/applet_web_browser.cpp:597` | web applet share shim: no host frontend; closed as by the end button |
| am | `src/core/hle/service/am/frontend/applet_web_browser.cpp:614` | web applet wifi shim: no host frontend; closed as by the end button |
| am | `src/core/hle/service/am/frontend/applet_web_browser.cpp:619` | web applet lobby shim: no host frontend; closed as by the end button |
| am | `src/core/hle/service/am/frontend/applet_web_browser.cpp:626` | web applet lhub shim: no host frontend; closed as by the end button |
| am | `src/core/hle/service/am/service/applet_common_functions.cpp:80` | IAppletCommonFunctions 42 SetDisplayMagnification: not emulated: the system zoom {},{} {}x{} is kept, not composited |
| am | `src/core/hle/service/am/service/applet_common_functions.cpp:113` | IAppletCommonFunctions 350 argument={:#x}; returning 0 |
| am | `src/core/hle/service/am/service/application_accessor.cpp:201` | IApplicationAccessor 132 GetNsRightsEnvironmentHandle: answered 0xdeadbeef (no ns rights environment exists) |
| am | `src/core/hle/service/am/service/application_accessor.cpp:257` | IApplicationAccessor 123 for a system application; answered NotAvailable |
| am | `src/core/hle/service/am/service/application_accessor.cpp:298` | IApplicationAccessor 140 GetDesirableUids (the pushed preselected user, if any) |
| am | `src/core/hle/service/am/service/application_accessor.cpp:319` | IApplicationAccessor 170 for another application id -> false |
| am | `src/core/hle/service/am/service/application_functions.cpp:360` | IApplicationFunctions 27 index {} size {:#x}+{:#x} beyond the control property's limits (index {}, {:#x}); answered InvalidParameters |
| am | `src/core/hle/service/am/service/application_functions.cpp:443` | IApplicationFunctions 32 BeginBlockingHomeButton time {} ns recorded, the block lasts until 33 |
| am | `src/core/hle/service/am/service/application_functions.cpp:471` | IApplicationFunctions 50 GetPseudoDeviceId: derived from the title seed alone; a console also mixes in a per-console secret |
| am | `src/core/hle/service/am/service/application_functions.cpp:493` | IApplicationFunctions 65 IsGamePlayRecordingSupported: not emulated: gameplay recording; answered false |
| am | `src/core/hle/service/am/service/application_functions.cpp:500` | IApplicationFunctions 66 InitializeGamePlayRecording: not emulated: gameplay recording; the memory is accepted and unused |
| am | `src/core/hle/service/am/service/application_functions.cpp:536` | IApplicationFunctions 100 InitializeApplicationCopyrightFrameBuffer: not emulated: the copyright overlay for captures; accepted |
| am | `src/core/hle/service/am/service/application_functions.cpp:543` | IApplicationFunctions 101 SetApplicationCopyrightImage: not emulated: the copyright overlay for captures; image dropped |
| am | `src/core/hle/service/am/service/application_functions.cpp:548` | IApplicationFunctions 102 SetApplicationCopyrightVisibility: not emulated: the copyright overlay for captures; visible={} |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:307` | ICommonStateGetter 52 SetLcdBacklighOffEnabled: not emulated: the backlight is not switched; enabled={} |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:358` | ICommonStateGetter 68 GetBuiltInDisplayType: answered 0 (the values are not established) |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:409` | ICommonStateGetter 200 GetOperationModeSystemInfo: answered 0 (the meaning is not established) |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:438` | ICommonStateGetter 900 SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled: kept per applet, not acted on at ExecuteProgram |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:475` | ICommonStateGetter 40 GetCradleFwVersion -> zeros |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:485` | ICommonStateGetter 610 argument={:#x}; returning success |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:491` | ICommonStateGetter 611 argument={:#x}; returning success |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:518` | ICommonStateGetter 1003 GetVrLaboGoggleViewport: answered the full 1280x720 screen (not established) |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:527` | ICommonStateGetter 1004 GetPanelPhysicalSizeForSpecificTitle: answered 137.25 x 77.2 mm (not established) |
| am | `src/core/hle/service/am/service/common_state_getter.cpp:534` | ICommonStateGetter 1005 GetPanelResolutionForSpecificTitle: answered 1280x720 (not established) |
| am | `src/core/hle/service/am/service/cradle_firmware_updater.cpp:31` | ICradleFirmwareUpdater 0 StartUpdate: not emulated: no dock firmware to update |
| am | `src/core/hle/service/am/service/cradle_firmware_updater.cpp:36` | ICradleFirmwareUpdater 1 FinishUpdate: not emulated: no dock firmware to update |
| am | `src/core/hle/service/am/service/cradle_firmware_updater.cpp:41` | ICradleFirmwareUpdater 2 GetCradleDeviceInfo: not emulated: no dock; answered zeros |
| am | `src/core/hle/service/am/service/cradle_firmware_updater.cpp:48` | ICradleFirmwareUpdater 3 GetCradleDeviceInfoChangeEvent: not emulated: no dock; the event is never signalled |
| am | `src/core/hle/service/am/service/display_controller.cpp:62` | IDisplayController 5 GetLastForegroundCaptureImageEx: not emulated: no frame copy kept; answered not written, black |
| am | `src/core/hle/service/am/service/display_controller.cpp:85` | IDisplayController 28 TakeScreenShotOfOwnLayerEx: not emulated: the own layer is not copied into capture buffer {} |
| am | `src/core/hle/service/am/service/display_controller.cpp:94` | IDisplayController 7 GetCallerAppletCaptureImageEx: not emulated: no frame copy kept; answered not written, black |
| am | `src/core/hle/service/am/service/display_controller.cpp:102` | IDisplayController 8 TakeScreenShotOfOwnLayer: not emulated: the own layer is not copied into the capture buffer |
| am | `src/core/hle/service/am/service/library_applet_accessor.cpp:158` | ILibraryAppletAccessor 90 Unknown90: answered success with no effect (the command is not documented) |
| am | `src/core/hle/service/am/service/library_applet_accessor.cpp:233` | ILibraryAppletAccessor 160 GetIndirectLayerConsumerHandle: answered 0xdeadbeef (indirect layers are not emulated) |
| am | `src/core/hle/service/am/service/library_applet_accessor.cpp:253` | ILibraryAppletAccessor 170; returning an event that never signals |
| am | `src/core/hle/service/am/service/library_applet_accessor.cpp:261` | ILibraryAppletAccessor 50 ({}); accepted, not applied |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:175` | ILibraryAppletSelfAccessor 13 CanUseApplicationCore: answered false (the NPDM core mask is not read) |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:280` | ILibraryAppletSelfAccessor 19 with no layout from the caller; returning the system keyboard layout {} |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:422` | ILibraryAppletSelfAccessor 18 answered with the caller |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:447` | ILibraryAppletSelfAccessor 40 -> 0 (no indirect layer for applet processes) |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:463` | ILibraryAppletSelfAccessor 90 ({:016x}) -> NotImplemented |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:470` | ILibraryAppletSelfAccessor 100 -> NotImplemented (no grc) |
| am | `src/core/hle/service/am/service/library_applet_self_accessor.cpp:487` | ILibraryAppletSelfAccessor 130, an event that never signals |
| am | `src/core/hle/service/am/service/overlay_functions.cpp:191` | IOverlayFunctions 40 while a title runs; the logo is not served |
| am | `src/core/hle/service/am/service/overlay_functions.cpp:200` | IOverlayFunctions 41 while a title runs; the movie is not served |
| am | `src/core/hle/service/am/service/overlay_functions.cpp:212` | IOverlayFunctions 60 ({}) recorded only |
| am | `src/core/hle/service/am/service/overlay_functions.cpp:217` | IOverlayFunctions 71 has no effect here |
| am | `src/core/hle/service/am/service/overlay_functions.cpp:223` | IOverlayFunctions 75 answered with the firmware's own failure (0xc80) |
| am | `src/core/hle/service/am/service/overlay_functions.cpp:239` | IOverlayFunctions 70: accepted, no effect here (the command is not documented) |
| am | `src/core/hle/service/am/service/process_winding_controller.cpp:84` | IProcessWindingController 22 PopContext on an empty stack: answered NotAvailable |
| am | `src/core/hle/service/am/service/self_controller.cpp:241` | ISelfController 19 SetAlbumImageOrientation: kept per applet; captures are not rotated (orientation={}) |
| am | `src/core/hle/service/am/service/self_controller.cpp:290` | ISelfController 50 SetHandlesRequestToDisplay: accepted, enable={}: the request-to-display flow is not emulated |
| am | `src/core/hle/service/am/service/self_controller.cpp:295` | ISelfController 51 ApproveToDisplay: accepted: the request-to-display flow is not emulated |
| am | `src/core/hle/service/am/service/self_controller.cpp:386` | ISelfController 67 IsIlluminanceAvailable: answered false: no ambient light sensor here |
| am | `src/core/hle/service/am/service/self_controller.cpp:458` | ISelfController 230 ({}, {}); returning success |
| am | `src/core/hle/service/am/sleep_sequence.cpp:181` | am sleep lock timeout: pid {} did not release the sleep lock in {} s; the console sleeps anyway |
| am | `src/core/hle/service/am/sleep_sequence.cpp:284` | am AutoPowerDown routing (sent to the HOME menu only) |
| am | `src/core/hle/service/am/sleep_sequence.cpp:314` | am DetectShortPressingPowerButton routing (sent to the HOME menu only) |
| audio | `src/core/hle/service/audio/audio_controller.cpp:238` | audctl 5000: answered this audctl object again |
| bcat | `src/core/hle/service/bcat/news/news_service.cpp:138` | INewsService 30201 ({}, {}); returning true |
| bcat | `src/core/hle/service/bcat/news/news_service.cpp:180` | news 50100; returning success with no output |
| caps | `src/core/hle/service/caps/caps_a.cpp:117` | caps:a 6 GetAlbumUsage, storage={} |
| caps | `src/core/hle/service/caps/caps_a.cpp:124` | caps:a 15 GetAlbumUsage3, storage={} |
| caps | `src/core/hle/service/caps/caps_a.cpp:137` | caps:a 17 GetAlbumUsage16, storage={}, flags={}, buffer {:#x} bytes; zero-filled |
| caps | `src/core/hle/service/caps/caps_a.cpp:155` | caps:a 110, out buffer {:#x} bytes; returning 0 entries |
| caps | `src/core/hle/service/caps/caps_a.cpp:165` | caps:a 120, out buffer {:#x} bytes; returning 0 entries |
| caps | `src/core/hle/service/caps/caps_c.cpp:53` | caps:c 1001 RequestTakingScreenShot |
| erpt | `src/core/hle/service/erpt/erpt.cpp:643` | IManager 4 bool argument {} (meaning not established); the statistics ignore it |
| erpt | `src/core/hle/service/erpt/erpt.cpp:659` | IManager 7 PopNotifiableErrorCodes answered with no error codes |
| erpt | `src/core/hle/service/erpt/erpt.cpp:666` | IManager 10 GetReportSizeMax answered with the array buffer limit {:#x} |
| fatal | `src/core/hle/service/fatal/fatal.cpp:144` | fatal {} filed as a visible erpt report with FatalFlag (the module that files it on a console is not established) |
| hid | `src/core/hle/service/hid/hid_system_server.cpp:727` | hid:sys 514 SendVibrationNotificationPattern pattern={:#x} unknown={:#x}; no output, no rumble |
| nifm | `src/core/hle/service/nifm/nifm.cpp:881` | nifm IGeneralService 25 GetSsidListVersion: answered u32 0 |
| nim | `src/core/hle/service/nim/nim.cpp:511` | nim IAsyncValue Get before the value is ready, answered as a failed token request |
| npns | `src/core/hle/service/npns/npns.cpp:249` | npns:u 23 DestroyToken(u64 {:#x}, pid {}): nothing stored, success |
| npns | `src/core/hle/service/npns/npns.cpp:255` | npns:u 25 QueryIsTokenValid: input not established, answered true |
| npns | `src/core/hle/service/npns/npns.cpp:262` | npns:u 26 ListenToMyApplicationId(u64 {:#x}, pid {}): recorded only |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:959` | IApplicationManagerInterface 936; returning 0 and an empty 0x80-byte record (logged once; qlaunch polls it every second) |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1070` | ns 5 DeleteApplicationCompletely failure Result (the code a console shows for a failed delete); answering fs 2002-6400 |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1305` | IApplicationManagerInterface 4023; returning false and an empty 0x50-byte record |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1315` | IApplicationManagerInterface 4042 (Virtual Game Cards), arg1={:016X} arg2={:016X}; IAsyncResult on the shared event |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1358` | IApplicationManagerInterface 518; returning an event that never signals |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1366` | IApplicationManagerInterface 1508; returning an event that never signals |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1375` | IApplicationManagerInterface 935; returning success |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1386` | IApplicationManagerInterface 2366; Nintendo network-install work is unavailable, completed unchanged |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1407` | IApplicationManagerInterface 4008; completed unchanged, work buffer {:#x} bytes{} (no transfer memory) |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1423` | IApplicationManagerInterface 4074: accepted, no account content state refresh queued (the key's meaning is not established) |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1435` | IApplicationManagerInterface 905 RequestApplicationUpdate {:016X} ({}): not emulated: no update download is forced |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1443` | IApplicationManagerInterface 4058; returning success |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1448` | IApplicationManagerInterface 4084; returning success |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1453` | IApplicationManagerInterface 4096; returning success |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1459` | IApplicationManagerInterface 4089; returning false and an empty 0x50-byte record |
| ns | `src/core/hle/service/ns/application_manager_interface.cpp:1467` | ns:am2 4105; returning success, no linked-account work queued |
| ns | `src/core/hle/service/ns/ecommerce_interface.cpp:159` | ns:ec 0 RequestLinkDevice: no OpenPak device link, completes |
| ns | `src/core/hle/service/ns/ecommerce_interface.cpp:169` | ns:ec 4 RequestUnlinkDevice: no OpenPak device link, completes |
| ns | `src/core/hle/service/ns/ecommerce_interface.cpp:178` | ns:ec 1 RequestCleanupAllPreInstalledApplications: nothing pre-installed, completes |
| ns | `src/core/hle/service/ns/ecommerce_interface.cpp:188` | ns:ec 2 RequestCleanupPreInstalledApplication {:016X}: nothing pre-installed, completes |
| ns | `src/core/hle/service/ns/ecommerce_interface.cpp:282` | ns:ec LoadPrefetched u64 (token life left, ns): answered 1 h |
| ns | `src/core/hle/service/ns/ns.cpp:82` | ns:vm 3100 field meanings; reporting state 3 |
| ns | `src/core/hle/service/ns/read_only_application_control_data_interface.cpp:241` | IReadOnlyApplicationControlDataInterface 4, {} candidate(s) |
| ns | `src/core/hle/service/ns/read_only_application_control_data_interface.cpp:358` | IReadOnlyApplicationControlDataInterface 28 for {:016X}, flag {}: answered success, no control-data download queued (the flag's meaning is not established) |
| ns | `src/core/hle/service/ns/read_only_application_control_data_interface.cpp:372` | IReadOnlyApplicationControlDataInterface 20 for {:016X} (source {}, flags {}/{}, extra {:#x}); answered as 19 |
| olsc | `src/core/hle/service/olsc/olsc_service_for_system_service.cpp:154` | olsc:s 209 answered like 200 (two zero bytes), app={:016X} |
| olsc | `src/core/hle/service/olsc/olsc_service_for_system_service.cpp:241` | olsc 1001 request 100 answered u32 1 (NX only) |
| olsc | `src/core/hle/service/olsc/remote_storage_controller.cpp:67` | olsc IAsyncResult 0 asked before done, answered 2179-1 |
| olsc | `src/core/hle/service/olsc/remote_storage_controller.cpp:181` | olsc RSC 12 first byte {} not read |
| olsc | `src/core/hle/service/olsc/remote_storage_controller.cpp:197` | olsc cache update: no list fetched, answered success |
| olsc | `src/core/hle/service/olsc/remote_storage_controller.cpp:220` | olsc RSC 13 notification token: no npns token, answered success |
| olsc | `src/core/hle/service/olsc/transfer_task_list_controller.cpp:200` | olsc TTLC 25 query not read, answered not found |
| omm | `src/core/hle/service/omm/policy_manager_system.cpp:51` | idle:sys 3 SetHandlingContext from a guest; ignored |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:490` | pctl 1004: SNS posting restricted, answered 2142-0101 as before |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:748` | pctl 1043 DeleteSettings: local reset only; the server is not told of a pairing dropped with it |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:821` | pctl 1204 inquiry code format |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:832` | pctl 1205 master key (Nintendo's key data); no match |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:848` | pctl 1471 NotifyWrongPinCodeInputManyTimes: no event is recorded or uploaded; answered success |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:1162` | pctl Finish before the work is done; answered 2142-0221, slot kept |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:1209` | pctl 2017's owner argument is not sent |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:1339` | pctl 2010: a {}-byte picture in a {}-byte buffer; cut |
| pctl | `src/core/hle/service/pctl/parental_control_service.cpp:1363` | pctl 2012 answers the picture's HTTP media type, nickname-style |
| pctl | `src/core/hle/service/pctl/pctl_pairing.cpp:434` | pctl moon {} could not reach the server; answered 2142-2003 |
| pctl | `src/core/hle/service/pctl/pctl_pairing.cpp:837` | pctl: the owner has no picture URL; answered an empty picture |
| set | `src/core/hle/service/set/factory_settings_server.cpp:193` | set:cal GetSslKey (size={}); the wrapped block |
| set | `src/core/hle/service/set/factory_settings_server.cpp:207` | set:cal GetEciDeviceKey; the wrapped block |
| set | `src/core/hle/service/set/factory_settings_server.cpp:213` | set:cal GetEticketDeviceKey (size={}); the wrapped block |
| set | `src/core/hle/service/set/factory_settings_server.cpp:234` | set:cal GetEciDeviceKey2 (size={}); the wrapped block |
| set | `src/core/hle/service/set/system_settings_server.cpp:485` | set:sys SetLanguageCode(0x{:016X}) is not a known language code; not stored, answered LanguageOutOfRange |
| set | `src/core/hle/service/set/system_settings_server.cpp:764` | set:sys GetSettingsItemValueSize {}!{} missing, answered size 0 |
| set | `src/core/hle/service/set/system_settings_server.cpp:784` | set:sys GetSettingsItemValue {}!{} missing, answered size 0 |
| set | `src/core/hle/service/set/system_settings_server.cpp:996` | set:sys SetRegionCode({}) is outside the known region codes; not stored, answered Success |
| sockets | `src/core/hle/service/sockets/nsd.cpp:140` | nsd GetDeviceId (buffer {} bytes); 16 hex digits |
| sockets | `src/core/hle/service/sockets/sfdnsres.cpp:720` | sfdnsres 102 for unknown query {}; answered EAI_FAIL |
| sockets | `src/core/hle/service/sockets/sfdnsres.cpp:725` | sfdnsres 102 before query {} completed; answered EAI_AGAIN |
| sockets | `src/core/hle/service/sockets/sfdnsres.cpp:770` | sfdnsres 101 QueryNameInfoRequestWithOptions, answered EAI_NONAME at fetch |
| sockets | `src/core/hle/service/sockets/sfdnsres.cpp:795` | sfdnsres 103 FetchNameInfoRequestWithOptions, answered EAI_NONAME |
| ssl | `src/core/hle/service/ssl/cert_store.cpp:120` | ssl GetCertificates id 0xFFFFFFFE, answered with the EnabledTrusted certificates |
| ssl | `src/core/hle/service/ssl/ssl.cpp:477` | ssl ISslConnection 24 GetVerifyCertErrors, answered no errors |
| ssl | `src/core/hle/service/ssl/ssl.cpp:507` | ssl ISslConnection 28 SetDtlsSocketDescriptor |
| ssl | `src/core/hle/service/ssl/ssl.cpp:513` | ssl ISslConnection 29 GetDtlsHandshakeTimeout |
| ssl | `src/core/hle/service/ssl/ssl.cpp:519` | ssl ISslConnection 30 SetPrivateOption option={} value={} |
| ssl | `src/core/hle/service/ssl/ssl.cpp:526` | ssl ISslConnection 31 SetSrtpCiphers |
| ssl | `src/core/hle/service/ssl/ssl.cpp:531` | ssl ISslConnection 32 GetSrtpCipher |
| ssl | `src/core/hle/service/ssl/ssl.cpp:537` | ssl ISslConnection 33 ExportKeyingMaterial |
| ssl | `src/core/hle/service/ssl/ssl.cpp:543` | ssl ISslConnection 34 SetIoTimeout {} |
| ssl | `src/core/hle/service/ssl/ssl.cpp:549` | ssl ISslConnection 35 GetIoTimeout |
| ssl | `src/core/hle/service/ssl/ssl.cpp:720` | ssl ISslContext 12 ImportClientCertKeyPki |
| ssl | `src/core/hle/service/ssl/ssl.cpp:726` | ssl ISslContext 13 GeneratePrivateKeyAndCert |
| ssl | `src/core/hle/service/ssl/ssl.cpp:732` | ssl ISslContextForSystem 14 CreateConnectionEx, answered as CreateConnection |
| ssl | `src/core/hle/service/ssl/ssl.cpp:941` | ssl:s 103 VerifySignature, answered success |

46 Ryujinx Harbor sites; 16 have no Eden-side site with the same interface and command (see the notes above for each):

| Ryujinx site | Logged there |
|---|---|
| `Ryujinx.HLE/HOS/Services/Account/Acc/AccountService/IOAuthProcedure.cs:96` | acc: no suspended {} for that session id; refused with InvalidArgument |
| `Ryujinx.HLE/HOS/Services/Account/Acc/AccountService/IProfileEditor.cs:59` | IProfileEditor 30; returning 0x10 zero bytes |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/LibraryAppletCreator/ILibraryAppletAccessor.cs:174` | ILibraryAppletAccessor 10 Start on pid {}, already started; answered Success without starting again |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/LibraryAppletProxy/ILibraryAppletSelfAccessor.cs:73` | ILibraryAppletSelfAccessor({}) {} {} -> {} |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/LibraryAppletProxy/IProcessWindingController.cs:66` | IProcessWindingController({}) 11 OpenCallingLibraryApplet with no reserved applet -> NotAvailable |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/OverlayAppletProxy/IOverlayFunctions.cs:190` | IOverlayFunctions {} while a title runs; the logo is not served |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/SystemAppletProxy/IApplicationAccessor.cs:80` | IApplicationAccessor 10 Start on pid {}, already started; answered Success without starting again |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/AllSystemAppletProxiesService/SystemAppletProxy/IApplicationAccessor.cs:346` | IApplicationAccessor({}) {} on a system application; swb says it fails, the result is undocumented; answered NotAvailable |
| `Ryujinx.HLE/HOS/Services/Am/AppletAE/IAllSystemAppletProxiesService.cs:53` | OpenOverlayAppletProxy from pid {}, which am did not launch as the overlay applet; served |
| `Ryujinx.HLE/HOS/Services/Caps/IAlbumAccessorService.cs:150` | caps:a 18 GetAppletProgramIdTable; wrote PhotoViewer and OverlayDisp ids: {} |
| `Ryujinx.HLE/HOS/Services/Caps/IAlbumAccessorService.cs:199` | caps:a {} (raw 0x{} bytes in, {} out buffer(s)); returning 0 entries |
| `Ryujinx.HLE/HOS/Services/Ns/IApplicationManagerInterface.cs:238` | IApplicationManagerInterface {}; returning an event that never signals |
| `Ryujinx.HLE/HOS/Services/Ns/IApplicationManagerInterface.cs:246` | IApplicationManagerInterface {}; returning false and an empty 0x50-byte record |
| `Ryujinx.HLE/HOS/Services/Ns/IApplicationManagerInterface.cs:318` | IApplicationManagerInterface {}; returning success |
| `Ryujinx.HLE/HOS/Services/Olsc/IOlscServiceForSystemService.cs:59` | IOlscServiceForSystemService 209 for {}; returning two zero bytes |
| `Ryujinx.HLE/HOS/Services/Pctl/Pairing/PctlAsync.cs:94` | pctl {} could not reach the server ({}); answered 2142-2003 |
<!-- generated:end -->
