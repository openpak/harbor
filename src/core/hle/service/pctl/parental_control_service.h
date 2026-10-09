// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator
// Project// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/pctl/pctl_async.h"
#include "core/hle/service/pctl/pctl_types.h"
#include "core/hle/service/service.h"
#include "core/openpak/async_worker.h"

namespace Kernel {
class KEvent;
}

namespace Service::PCTL {

// [Harbor] The restriction commands answer from the parent's setting and the PIN (pctl_pairing),
// with nothing restricted before pairing; the pairing commands talk to OpenPak's nx-pctl on an
// OpenPak::AsyncWorker task, never on the IPC thread. Behaviour follows the Ryujinx Harbor
// (MIT-licensed Ryujinx base).
class IParentalControlService final : public ServiceFramework<IParentalControlService> {
public:
    explicit IParentalControlService(Core::System& system_, Capability capability_,
                                     u64 program_id_,
                                     std::shared_ptr<OpenPak::AsyncWorker> worker_);
    ~IParentalControlService() override;

private:
    bool Has(u32 bits) const;
    bool CanPair() const;

    void Initialize(HLERequestContext& ctx);
    Result InitializeImpl();

    // Restrictions
    void CheckFreeCommunicationPermission(HLERequestContext& ctx);
    void IsFreeCommunicationAvailable(HLERequestContext& ctx);
    void EndFreeCommunication(HLERequestContext& ctx);
    void ConfirmLaunchApplicationPermissionOld(HLERequestContext& ctx);
    void ConfirmResumeApplicationPermission(HLERequestContext& ctx);
    void ConfirmLaunchApplicationPermission(HLERequestContext& ctx);
    void ConfirmSnsPostPermission(HLERequestContext& ctx);
    void ConfirmSystemSettingsPermission(HLERequestContext& ctx);
    void IsRestrictionTemporaryUnlocked(HLERequestContext& ctx);
    void RevertRestrictionTemporaryUnlocked(HLERequestContext& ctx);
    void EnterRestrictedSystemSettings(HLERequestContext& ctx);
    void LeaveRestrictedSystemSettings(HLERequestContext& ctx);
    void IsRestrictedSystemSettingsEntered(HLERequestContext& ctx);
    void GetRestrictedFeatures(HLERequestContext& ctx);
    void GetRestrictedFeatureFlagSet(HLERequestContext& ctx);
    void ConfirmStereoVisionPermission(HLERequestContext& ctx);
    void IsRestrictionEnabled(HLERequestContext& ctx);
    void GetSafetyLevel(HLERequestContext& ctx);
    void GetSafetyLevelSettings(HLERequestContext& ctx);
    void GetCurrentSettings(HLERequestContext& ctx);
    void GetDefaultRatingOrganization(HLERequestContext& ctx);
    void SetSafetyLevel(HLERequestContext& ctx);
    void SetCustomSafetyLevelSettings(HLERequestContext& ctx);
    void SetDefaultRatingOrganization(HLERequestContext& ctx);
    void AddToFreeCommunicationApplicationList(HLERequestContext& ctx);
    void AddToFreeCommunicationApplicationListWithFlag(HLERequestContext& ctx);
    void UpdateFreeCommunicationApplicationList(HLERequestContext& ctx);
    void DeleteSettings(HLERequestContext& ctx);
    void GetFreeCommunicationApplicationListCount(HLERequestContext& ctx);
    void GetFreeCommunicationApplicationList(HLERequestContext& ctx);
    void ConfirmStereoVisionRestrictionConfigurable(HLERequestContext& ctx);
    void GetStereoVisionRestriction(HLERequestContext& ctx);
    void SetStereoVisionRestriction(HLERequestContext& ctx);
    void ResetConfirmedStereoVisionPermission(HLERequestContext& ctx);
    void IsStereoVisionPermitted(HLERequestContext& ctx);

    // PIN
    void UnlockRestrictionTemporarily(HLERequestContext& ctx);
    void UnlockSystemSettingsRestriction(HLERequestContext& ctx);
    void UnlockSystemSettingsRestrictionWithReason(HLERequestContext& ctx);
    void SetPinCode(HLERequestContext& ctx);
    void GenerateInquiryCode(HLERequestContext& ctx);
    void CheckMasterKey(HLERequestContext& ctx);
    void GetPinCodeLength(HLERequestContext& ctx);
    void NotifyWrongPinCodeInputManyTimes(HLERequestContext& ctx);
    void GetPinCodeChangedEvent(HLERequestContext& ctx);
    void GetPinCode(HLERequestContext& ctx);

    // Pairing state
    void IsPairingActive(HLERequestContext& ctx);
    void GetSettingsLastUpdated(HLERequestContext& ctx);
    void GetPairingAccountInfo(HLERequestContext& ctx);
    void GetAllPairingAccountInfos(HLERequestContext& ctx);
    void GetAccountNickname(HLERequestContext& ctx);
    void GetAccountState(HLERequestContext& ctx);
    void GetSynchronizationEvent(HLERequestContext& ctx);

    // Play timer (parity; no enforcement)
    void StartPlayTimer(HLERequestContext& ctx);
    void StopPlayTimer(HLERequestContext& ctx);
    void IsPlayTimerEnabled(HLERequestContext& ctx);
    void GetPlayTimerRemainingTime(HLERequestContext& ctx);
    void IsRestrictedByPlayTimer(HLERequestContext& ctx);
    void GetPlayTimerSettingsOld(HLERequestContext& ctx);
    void GetPlayTimerSettings(HLERequestContext& ctx);
    void GetPlayTimerEventToRequestSuspension(HLERequestContext& ctx);
    void IsPlayTimerAlarmDisabled(HLERequestContext& ctx);
    void GetPlayTimerRemainingTimeDisplayInfo(HLERequestContext& ctx);
    void GetWatcherStatusDisplayInfo(HLERequestContext& ctx);
    void GetUnlinkedEvent(HLERequestContext& ctx);
    void GetExtendedPlayTimerEvent(HLERequestContext& ctx);

    // Pairing (async)
    void RequestPairingAsync(HLERequestContext& ctx);
    void FinishRequestPairing(HLERequestContext& ctx);
    void AuthorizePairingAsyncOld(HLERequestContext& ctx);
    void AuthorizePairingAsync(HLERequestContext& ctx);
    void FinishAuthorizePairing(HLERequestContext& ctx);
    void RetrievePairingInfoAsync(HLERequestContext& ctx);
    void FinishRetrievePairingInfo(HLERequestContext& ctx);
    void UnlinkPairingAsync(HLERequestContext& ctx);
    void FinishUnlinkPairing(HLERequestContext& ctx);
    void GetAccountMiiImageAsync(HLERequestContext& ctx);
    void FinishGetAccountMiiImage(HLERequestContext& ctx);
    void GetAccountMiiImageContentTypeAsync(HLERequestContext& ctx);
    void FinishGetAccountMiiImageContentType(HLERequestContext& ctx);
    void SynchronizeParentalControlSettingsAsync(HLERequestContext& ctx);
    void FinishSynchronizeParentalControlSettings(HLERequestContext& ctx);
    void FinishSynchronizeParentalControlSettingsWithLastUpdated(HLERequestContext& ctx);
    void DownloadAllAccountMiiImagesAsync(HLERequestContext& ctx);
    void FinishDownloadAllAccountMiiImages(HLERequestContext& ctx);
    void GetPairingAccountChangedEvent(HLERequestContext& ctx);
    void PrepareMigrationWithEmulatedOldSaveDataForDebug(HLERequestContext& ctx);

    using Work = std::function<Result()>;
    /// Start `work` in the next slot: AsyncData and a copy handle to the slot's event.
    void StartAsync(HLERequestContext& ctx, const char* what, Work work, u32 extra_word = 0,
                    bool extra = false);
    /// Nothing to do (not paired): AsyncData zero and handle zero, success.
    void AnswerNothing(HLERequestContext& ctx, bool extra = false, u32 extra_word = 0);
    /// 2009/2011: the owner's picture (or its media type) from the cache at once, else async.
    void PushDisplayInfo(HLERequestContext& ctx, bool ignore_temporary_unlock, bool for_1459);
    void StartMii(HLERequestContext& ctx, const char* what, bool content_type);
    /// The picture into the output buffer, min(buffer, picture); answers the picture's full size.
    u32 WriteMii(HLERequestContext& ctx, const std::vector<u8>& image);
    /// Take the work `data` names back: its result, or 2142-0221.
    Result FinishAsync(const AsyncData& data);
    void PushEvent(HLERequestContext& ctx, Kernel::KEvent* event);
    void AuthorizePairing(HLERequestContext& ctx, const PairingInfoBase& info);

    Capability capability{};
    u64 program_id{};
    ApplicationInfo application_info{};
    bool free_communication{};

    std::shared_ptr<OpenPak::AsyncWorker> worker;
    KernelHelpers::ServiceContext service_context;

    AsyncSlots slots;
    /// The owner the last 2009/2011 asked about, for its Finish (2010/2012).
    u64 mii_account{};
    std::array<std::shared_ptr<OpenPak::AsyncTask>, AsyncSlots::Count> tasks{};
    std::array<Kernel::KEvent*, AsyncSlots::Count> slot_events{};

    Kernel::KEvent* synchronization_event{};
    Kernel::KEvent* pin_code_changed_event{};
    Kernel::KEvent* unlinked_event{};
    Kernel::KEvent* request_suspension_event{};
    Kernel::KEvent* extended_play_timer_event{};
    Kernel::KEvent* pairing_account_changed_event{};
    std::shared_ptr<OpenPak::HostEvent> synchronization_host_event;
    std::shared_ptr<OpenPak::HostEvent> pin_code_changed_host_event;
    std::shared_ptr<OpenPak::HostEvent> unlinked_host_event;
    int listener_id{};
};

} // namespace Service::PCTL
