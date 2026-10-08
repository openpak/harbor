// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/ns/async_result.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/ns/ns_types.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/service.h"

namespace Service::NS {

/// The version HOME shows for an application: the update that will be applied when the title
/// starts (the newest enabled one in the external content folders, the frontend's game folders, or
/// the NAND/SD card), else the base title's own version. Updates and DLC in the user's folders are
/// part of the installed content, as on a console after the downloads finished.
[[nodiscard]] u32 GetInstalledApplicationVersion(Core::System& system, u64 application_id);

/// The HOME library: one record per application found in the content providers, most recently
/// played or touched first.
[[nodiscard]] std::vector<ApplicationRecord> ListLibraryApplicationRecords(Core::System& system);

/// Signals the record update event of every IApplicationManagerInterface, so HOME lists again.
void NotifyApplicationRecordsChanged();

class IApplicationManagerInterface final : public ServiceFramework<IApplicationManagerInterface> {
public:
    explicit IApplicationManagerInterface(Core::System& system_);
    ~IApplicationManagerInterface() override;

    void SignalRecordUpdate();

private:
    bool RemoveInstalledContent(u64 application_id);

public:

    Result UnregisterNetworkServiceAccountWithUserSaveDataDeletion(Common::UUID user_id);
    Result GetApplicationControlData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                     Out<u32> out_actual_size,
                                     ApplicationControlSource application_control_source,
                                     u64 application_id);
    Result GetApplicationDesiredLanguage(Out<ApplicationLanguage> out_desired_language,
                                         u32 supported_languages);
    Result ConvertApplicationLanguageToLanguageCode(Out<u64> out_language_code,
                                                    ApplicationLanguage application_language);
    Result ListApplicationRecord(OutArray<ApplicationRecord, BufferAttr_HipcMapAlias> out_records,
                                 Out<s32> out_count, s32 offset);
    Result GetApplicationRecordUpdateSystemEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetGameCardMountFailureEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetGameCardWakenReadyEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result IsGameCardApplicationRunning(Out<bool> out_is_running);
    Result Unknown936(Out<UnknownCountedRecord> out_record);
    Result IsAnyApplicationEntityInstalled(Out<bool> out_is_any_application_entity_installed);
    Result GetApplicationViewDeprecated(
        OutArray<ApplicationViewV19, BufferAttr_HipcMapAlias> out_application_views,
        InArray<u64, BufferAttr_HipcMapAlias> application_ids);
    Result GetApplicationViewWithPromotionInfo(
        OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
        Out<u32> out_count,
        InArray<u64, BufferAttr_HipcMapAlias> application_ids);
    Result GetApplicationView(
        OutArray<ApplicationViewV20, BufferAttr_HipcMapAlias> out_application_views,
        InArray<u64, BufferAttr_HipcMapAlias> application_ids);
    Result GetApplicationRightsOnClient(
        OutArray<ApplicationRightsOnClient, BufferAttr_HipcMapAlias> out_rights, Out<u32> out_count,
        u32 flags, u64 application_id, Uid account_id);
    Result CheckSdCardMountStatus();
    Result GetSdCardMountStatusChangedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetTotalSpaceSize(Out<s64> out_total_space_size, FileSys::StorageId storage_id);
    Result GetFreeSpaceSize(Out<s64> out_free_space_size, FileSys::StorageId storage_id);
    Result GetGameCardUpdateDetectionEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result ResumeAll();
    Result IsQualificationTransitionSupportedByProcessId(Out<bool> out_is_supported,
                                                         u64 process_id);
    Result DeleteApplicationEntity(u64 application_id);
    Result DeleteApplicationCompletely(u64 application_id);
    Result GetStorageSize(Out<s64> out_total_space_size, Out<s64> out_free_space_size,
                          FileSys::StorageId storage_id);
    Result TouchApplication(u64 application_id);
    Result IsApplicationUpdateRequested(Out<bool> out_update_required, Out<u32> out_update_version,
                                        u64 application_id);
    Result CheckApplicationLaunchVersion(u64 application_id);
    Result GetApplicationTerminateResult(Out<Result> out_result, u64 application_id);
    Result GetApplicationLogoData(Out<s64> out_size,
                                  OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                  u64 application_id,
                                  InBuffer<BufferAttr_HipcMapAlias> logo_path_buffer);
    Result Unknown4022(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result Unknown4023(Out<UnknownTryPopRecord> out_record);
    Result Unknown4042(OutInterface<IAsyncResult> out_interface,
                       OutCopyHandle<Kernel::KReadableEvent> out_event,
                       u64 arg1, u64 arg2);
    Result Unknown4053();
    Result Unknown4105();

    // Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base). The emulated unit's SD
    // card, game card and shell state never change: these events exist for the HOME menu to wait
    // on and never signal.
    Result GenerateApplicationRecordCount(Out<u64> out_count);
    Result GetGameCardAttachmentEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetSdCardRemovedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result GetApplicationShellEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result Unknown518(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result Unknown1508(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result Unknown935();
    Result Unknown2366(OutInterface<IAsyncResult> out_interface,
                       OutCopyHandle<Kernel::KReadableEvent> out_event, Common::UUID user_id);
    Result Unknown4008(OutInterface<IAsyncResult> out_interface,
                       OutCopyHandle<Kernel::KReadableEvent> out_event, Common::UUID user_id,
                       u64 work_buffer_size,
                       InCopyHandle<Kernel::KTransferMemory> work_buffer);
    Result Unknown4058();
    Result Unknown4084();
    Result Unknown4096();
    Result Unknown4089(Out<UnknownTryPopRecord> out_record);

    Result RequestDownloadApplicationControlDataInBackground(u64 control_source,
                                                             u64 application_id);

    void PushApplicationRecord(HLERequestContext& ctx);
    void ListApplicationTitle(HLERequestContext& ctx);
    void ListApplicationIcon(HLERequestContext& ctx);

private:
    KernelHelpers::ServiceContext service_context;
    Event record_update_system_event;
    Event sd_card_mount_status_event;
    Event gamecard_update_detection_event;
    Event gamecard_mount_status_event;
    Event gamecard_mount_failure_event;
    Event gamecard_waken_ready_event;
    Event unknown_event;
    Event gamecard_attachment_event;
    Event sd_card_removed_event;
    Event application_shell_event;
    Event unknown518_event;
    Event unknown1508_event;
    Event async_completed_event;
    Event unknown4008_event;
};

} // namespace Service::NS
