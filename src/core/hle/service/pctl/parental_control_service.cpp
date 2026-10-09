// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// [Harbor] Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base): its
// IParentalControlService over the parent's setting, the PIN and pairing with OpenPak's moon.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <span>
#include <string>

#include "common/logging.h"
#include "common/redact.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/pctl/moon.h"
#include "core/hle/service/pctl/parental_control_service.h"
#include "core/hle/service/pctl/pctl_pairing.h"
#include "core/hle/service/pctl/pctl_results.h"
#include "core/hle/service/pctl/pctl_setting.h"
#include "core/hle/service/set/settings_server.h"
#include "core/hle/service/set/system_settings_server.h"

namespace Service::PCTL {

namespace {

void PushResult(HLERequestContext& ctx, Result result) {
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

std::uint32_t Region() {
    return static_cast<std::uint32_t>(Settings::values.region_index.GetValue());
}

std::string Language() {
    return Moon::LanguageName(static_cast<std::uint64_t>(Set::GetLanguageCodeFromIndex(
        static_cast<std::size_t>(Settings::values.language_index.GetValue()))));
}

Rules::Restriction CurrentRestriction(const Pairing::Restrictions& restrictions) {
    return Rules::Current(restrictions.safety_level, restrictions.custom, Region());
}

int RatingOrganization(const Pairing::Restrictions& restrictions) {
    return restrictions.rating_organization >= 0
               ? restrictions.rating_organization
               : Rules::DefaultOrganization(Region(), Language());
}

/// The free-communication gate (.text 0x40a30): a title that does not flag free communication,
/// an unlocked console, no PIN, or free communication not restricted all pass; then an exempt
/// title, then (CUSTOM only) a title the parent allowed in the free-communication list.
Result FreeCommunicationCheck(u64 application_id, bool flagged) {
    const auto restrictions = Pairing::Current().InForce();
    if (!flagged || restrictions.temporarily_unlocked || !restrictions.Enabled() ||
        !CurrentRestriction(restrictions).free_communication_restricted) {
        return ResultSuccess;
    }
    if (restrictions.exempt_applications.contains(application_id)) {
        return ResultSuccess;
    }
    for (const auto& [id, restricted] : restrictions.free_communication_applications) {
        if (restrictions.safety_level == 1 && id == application_id && !restricted) {
            return ResultSuccess;
        }
    }
    return ResultNoFreeCommunication;
}

/// The launch gate's rating half (.text 0x40b40): an unlocked console, no PIN or an exempt title
/// pass; otherwise the title's rating ages against the allowed age.
Result RatingCheck(u64 application_id, std::span<const std::int8_t> ratings) {
    const auto restrictions = Pairing::Current().InForce();
    if (restrictions.temporarily_unlocked || !restrictions.Enabled() ||
        restrictions.exempt_applications.contains(application_id)) {
        return ResultSuccess;
    }
    if (Rules::RatingPermits(CurrentRestriction(restrictions).rating_age,
                             RatingOrganization(restrictions), ratings)) {
        return ResultSuccess;
    }
    LOG_INFO(Service_PCTL, "pctl: a title is over the allowed age rating");
    return ResultRestrictedByRating;
}

/// The rating-age buffer the launch gates take (X descriptor; a map one is read too), at most
/// 0x20 signed bytes, one per organisation.
std::vector<std::int8_t> ReadRatings(HLERequestContext& ctx) {
    if (!ctx.CanReadBuffer(0)) {
        return {};
    }
    const auto buffer = ctx.ReadBuffer(0);
    const auto size = std::min<std::size_t>(buffer.size(), 0x20);
    std::vector<std::int8_t> ratings(size);
    std::memcpy(ratings.data(), buffer.data(), size);
    return ratings;
}

/// A NUL-terminated string from the X buffer (a PIN or a pairing code): never logged.
std::string ReadString(HLERequestContext& ctx, std::size_t max) {
    if (!ctx.CanReadBuffer(0)) {
        return {};
    }
    const auto buffer = ctx.ReadBuffer(0);
    const auto size = std::min(buffer.size(), max);
    const auto* data = reinterpret_cast<const char*>(buffer.data());
    return std::string(data, strnlen(data, size));
}

/// A string into the C buffer: at most 32 bytes, NUL-terminated when it fits. Returns the length.
u32 WriteString(HLERequestContext& ctx, std::string_view text) {
    if (!ctx.CanWriteBuffer() || ctx.GetWriteBufferSize() == 0) {
        return 0;
    }
    const std::size_t size = ctx.GetWriteBufferSize();
    const std::size_t length = std::min({text.size(), Moon::NicknameBytes, size});
    std::vector<u8> output(std::min(size, length + 1));
    std::memcpy(output.data(), text.data(), length);
    ctx.WriteBuffer(output.data(), output.size());
    return static_cast<u32>(length);
}

/// No PIN is 2142-0132; one that is not the PIN, 2142-0120.
Result CheckPin(const std::string& typed) {
    const auto pin = Pairing::Current().InForce().pin;
    if (pin.empty()) {
        return ResultPinNotSet;
    }
    return typed == pin ? ResultSuccess : ResultWrongPin;
}

u32 RestrictedFeatures() {
    const auto restrictions = Pairing::Current().InForce();
    if (!restrictions.Enabled()) {
        return 0;
    }
    const auto current = CurrentRestriction(restrictions);
    return (current.rating_age != 0 ? 1u : 0u) | (current.sns_post_restricted ? 2u : 0u) |
           (current.free_communication_restricted ? 4u : 0u);
}

// System Settings enters its parental-controls page (after the PIN, where one is set) and leaves it
// again; the state is the console's, shared by every pctl session.
std::atomic<bool> restricted_system_settings_entered{false};

// Play time is counted while the timer runs (StartPlayTimer/StopPlayTimer) and starts over each
// local day. The limit of the day's regulation is applied to the remaining time; the bedtime alarm,
// the alarm and suspension events and the event upload are not (pctl's watcher is not reproduced).
struct PlayClock {
    std::mutex mutex;
    bool running = false;
    std::chrono::steady_clock::time_point started{};
    std::chrono::nanoseconds played_today{};
    int day = -1; ///< days since the epoch, local
};

PlayClock& Clock() {
    static PlayClock clock;
    return clock;
}

std::tm LocalNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local;
}

std::chrono::nanoseconds PlayedToday() {
    auto& clock = Clock();
    std::lock_guard lock{clock.mutex};
    const std::tm local = LocalNow();
    const int today = local.tm_year * 400 + local.tm_yday;
    const auto now = std::chrono::steady_clock::now();
    if (today != clock.day) {
        clock.day = today;
        clock.played_today = {};
        clock.started = now;
    }
    return clock.running ? clock.played_today + (now - clock.started) : clock.played_today;
}

std::optional<int> TodaysLimit(const PlayTimerBlock& timer) {
    return Rules::TodaysLimit(timer, LocalNow().tm_wday);
}

} // namespace

IParentalControlService::IParentalControlService(Core::System& system_, Capability capability_,
                                                 u64 program_id_,
                                                 std::shared_ptr<OpenPak::AsyncWorker> worker_)
    : ServiceFramework{system_, "IParentalControlService"}, capability{capability_},
      program_id{program_id_}, worker{std::move(worker_)},
      service_context{system_, "IParentalControlService"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, &IParentalControlService::Initialize, "Initialize"},
        {1001, &IParentalControlService::CheckFreeCommunicationPermission, "CheckFreeCommunicationPermission"},
        {1002, &IParentalControlService::ConfirmLaunchApplicationPermissionOld, "ConfirmLaunchApplicationPermission"},
        {1003, &IParentalControlService::ConfirmResumeApplicationPermission, "ConfirmResumeApplicationPermission"},
        {1004, &IParentalControlService::ConfirmSnsPostPermission, "ConfirmSnsPostPermission"},
        {1005, &IParentalControlService::ConfirmSystemSettingsPermission, "ConfirmSystemSettingsPermission"},
        {1006, &IParentalControlService::IsRestrictionTemporaryUnlocked, "IsRestrictionTemporaryUnlocked"},
        {1007, &IParentalControlService::RevertRestrictionTemporaryUnlocked, "RevertRestrictionTemporaryUnlocked"},
        {1008, &IParentalControlService::EnterRestrictedSystemSettings, "EnterRestrictedSystemSettings"},
        {1009, &IParentalControlService::LeaveRestrictedSystemSettings, "LeaveRestrictedSystemSettings"},
        {1010, &IParentalControlService::IsRestrictedSystemSettingsEntered, "IsRestrictedSystemSettingsEntered"},
        {1011, &IParentalControlService::LeaveRestrictedSystemSettings, "RevertRestrictedSystemSettingsEntered"},
        {1012, &IParentalControlService::GetRestrictedFeatures, "GetRestrictedFeatures"},
        {1013, &IParentalControlService::ConfirmStereoVisionPermission, "ConfirmStereoVisionPermission"},
        {1014, nullptr, "ConfirmPlayableApplicationVideoOld"},
        {1015, nullptr, "ConfirmPlayableApplicationVideo"},
        {1016, nullptr, "ConfirmShowNewsPermission"},
        {1017, &IParentalControlService::EndFreeCommunication, "EndFreeCommunication"},
        {1018, &IParentalControlService::IsFreeCommunicationAvailable, "IsFreeCommunicationAvailable"},
        {1019, &IParentalControlService::ConfirmLaunchApplicationPermission, "ConfirmLaunchApplicationPermission"},
        {1020, nullptr, "ConfirmLaunchSharedApplicationPermission"}, //20.0.0+
        {1021, nullptr, "TryBeginFreeCommunicationForStreamPlay"}, //21.0.0+
        {1022, nullptr, "EndFreeCommunicationForStreamPlay"}, //21.0.0+
        {1023, &IParentalControlService::GetRestrictedFeatureFlagSet, "GetRestrictedFeatureFlagSet"}, // 23.0.0+
        {1031, &IParentalControlService::IsRestrictionEnabled, "IsRestrictionEnabled"},
        {1032, &IParentalControlService::GetSafetyLevel, "GetSafetyLevel"},
        {1033, &IParentalControlService::SetSafetyLevel, "SetSafetyLevel"},
        {1034, &IParentalControlService::GetSafetyLevelSettings, "GetSafetyLevelSettings"},
        {1035, &IParentalControlService::GetCurrentSettings, "GetCurrentSettings"},
        {1036, &IParentalControlService::SetCustomSafetyLevelSettings, "SetCustomSafetyLevelSettings"},
        {1037, &IParentalControlService::GetDefaultRatingOrganization, "GetDefaultRatingOrganization"},
        {1038, &IParentalControlService::SetDefaultRatingOrganization, "SetDefaultRatingOrganization"},
        {1039, &IParentalControlService::GetFreeCommunicationApplicationListCount, "GetFreeCommunicationApplicationListCount"},
        {1042, &IParentalControlService::AddToFreeCommunicationApplicationList, "AddToFreeCommunicationApplicationList"},
        {1043, &IParentalControlService::DeleteSettings, "DeleteSettings"},
        {1044, &IParentalControlService::GetFreeCommunicationApplicationList, "GetFreeCommunicationApplicationList"},
        {1045, &IParentalControlService::UpdateFreeCommunicationApplicationList, "UpdateFreeCommunicationApplicationList"},
        {1046, nullptr, "DisableFeaturesForReset"},
        {1047, nullptr, "NotifyApplicationDownloadStarted"},
        {1048, nullptr, "NotifyNetworkProfileCreated"},
        {1049, nullptr, "ResetFreeCommunicationApplicationList"},
        {1050, &IParentalControlService::AddToFreeCommunicationApplicationListWithFlag, "AddToFreeCommunicationApplicationList"}, //20.0.0+
        {1051, nullptr, "NotifyApplicationDownloadStarted"}, //20.0.0+
        {1061, &IParentalControlService::ConfirmStereoVisionRestrictionConfigurable, "ConfirmStereoVisionRestrictionConfigurable"},
        {1062, &IParentalControlService::GetStereoVisionRestriction, "GetStereoVisionRestriction"},
        {1063, &IParentalControlService::SetStereoVisionRestriction, "SetStereoVisionRestriction"},
        {1064, &IParentalControlService::ResetConfirmedStereoVisionPermission, "ResetConfirmedStereoVisionPermission"},
        {1065, &IParentalControlService::IsStereoVisionPermitted, "IsStereoVisionPermitted"},
        {1201, &IParentalControlService::UnlockRestrictionTemporarily, "UnlockRestrictionTemporarily"},
        {1202, &IParentalControlService::UnlockSystemSettingsRestriction, "UnlockSystemSettingsRestriction"},
        {1203, &IParentalControlService::SetPinCode, "SetPinCode"},
        {1204, &IParentalControlService::GenerateInquiryCode, "GenerateInquiryCode"},
        {1205, &IParentalControlService::CheckMasterKey, "CheckMasterKey"},
        {1206, &IParentalControlService::GetPinCodeLength, "GetPinCodeLength"},
        {1207, &IParentalControlService::GetPinCodeChangedEvent, "GetPinCodeChangedEvent"},
        {1208, &IParentalControlService::GetPinCode, "GetPinCode"},
        {1209, &IParentalControlService::UnlockSystemSettingsRestrictionWithReason, "UnlockSystemSettingsRestriction"}, // 22.0.0+
        {1403, &IParentalControlService::IsPairingActive, "IsPairingActive"},
        {1406, &IParentalControlService::GetSettingsLastUpdated, "GetSettingsLastUpdated"},
        {1411, &IParentalControlService::GetPairingAccountInfo, "GetPairingAccountInfo"},
        {1412, &IParentalControlService::GetAllPairingAccountInfos, "GetAllPairingAccountInfos"}, // 23.0.0+
        {1421, &IParentalControlService::GetAccountNickname, "GetAccountNickname"},
        {1424, &IParentalControlService::GetAccountState, "GetAccountState"},
        {1425, nullptr, "RequestPostEvents"},
        {1426, nullptr, "GetPostEventInterval"},
        {1427, nullptr, "SetPostEventInterval"},
        {1432, &IParentalControlService::GetSynchronizationEvent, "GetSynchronizationEvent"},
        {1451, &IParentalControlService::StartPlayTimer, "StartPlayTimer"},
        {1452, &IParentalControlService::StopPlayTimer, "StopPlayTimer"},
        {1453, &IParentalControlService::IsPlayTimerEnabled, "IsPlayTimerEnabled"},
        {1454, &IParentalControlService::GetPlayTimerRemainingTime, "GetPlayTimerRemainingTime"},
        {1455, &IParentalControlService::IsRestrictedByPlayTimer, "IsRestrictedByPlayTimer"},
        {1456, &IParentalControlService::GetPlayTimerSettingsOld, "GetPlayTimerSettingsOld"},
        {1457, &IParentalControlService::GetPlayTimerEventToRequestSuspension, "GetPlayTimerEventToRequestSuspension"},
        {1458, &IParentalControlService::IsPlayTimerAlarmDisabled, "IsPlayTimerAlarmDisabled"},
        {1459, &IParentalControlService::GetPlayTimerRemainingTimeDisplayInfo, "GetPlayTimerRemainingTimeDisplayInfo"},
        {1460, &IParentalControlService::GetWatcherStatusDisplayInfo, "GetWatcherStatusDisplayInfo"}, // 23.0.0+
        {1471, &IParentalControlService::NotifyWrongPinCodeInputManyTimes, "NotifyWrongPinCodeInputManyTimes"},
        {1472, nullptr, "CancelNetworkRequest"},
        {1473, &IParentalControlService::GetUnlinkedEvent, "GetUnlinkedEvent"},
        {1474, nullptr, "ClearUnlinkedEvent"},
        {1475, &IParentalControlService::GetExtendedPlayTimerEvent, "GetExtendedPlayTimerEvent"}, // 18.0.0+
        {1601, nullptr, "DisableAllFeatures"},
        {1602, nullptr, "PostEnableAllFeatures"},
        {1603, nullptr, "IsAllFeaturesDisabled"},
        {1901, nullptr, "DeleteFromFreeCommunicationApplicationListForDebug"},
        {1902, nullptr, "ClearFreeCommunicationApplicationListForDebug"},
        {1903, nullptr, "GetExemptApplicationListCountForDebug"},
        {1904, nullptr, "GetExemptApplicationListForDebug"},
        {1905, nullptr, "UpdateExemptApplicationListForDebug"},
        {1906, nullptr, "AddToExemptApplicationListForDebug"},
        {1907, nullptr, "DeleteFromExemptApplicationListForDebug"},
        {1908, nullptr, "ClearExemptApplicationListForDebug"},
        {1941, nullptr, "DeletePairing"},
        {1951, nullptr, "SetPlayTimerSettingsForDebug"},
        {1952, nullptr, "GetPlayTimerSpentTimeForTest"},
        {1953, nullptr, "SetPlayTimerAlarmDisabledForDebug"},
        {1954, nullptr, "IsBedtimeAlarmEnabled"}, // 18.0.0+
        {1955, nullptr, "GetBedtimeAlarmTime"}, // 18.0.0+
        {1956, nullptr, "GetBedtimeAlarmTimeHour"}, // 18.0.0+
        {1957, nullptr, "GetBedtimeAlarmTimeMinute"}, // 18.0.0+
        {2001, &IParentalControlService::RequestPairingAsync, "RequestPairingAsync"},
        {2002, &IParentalControlService::FinishRequestPairing, "FinishRequestPairing"},
        {2003, &IParentalControlService::AuthorizePairingAsyncOld, "AuthorizePairingAsync"},
        {2004, &IParentalControlService::FinishAuthorizePairing, "FinishAuthorizePairing"},
        {2005, &IParentalControlService::RetrievePairingInfoAsync, "RetrievePairingInfoAsync"},
        {2006, &IParentalControlService::FinishRetrievePairingInfo, "FinishRetrievePairingInfo"},
        {2007, &IParentalControlService::UnlinkPairingAsync, "UnlinkPairingAsync"},
        {2008, &IParentalControlService::FinishUnlinkPairing, "FinishUnlinkPairing"},
        {2009, &IParentalControlService::GetAccountMiiImageAsync, "GetAccountMiiImageAsync"},
        {2010, &IParentalControlService::FinishGetAccountMiiImage, "FinishGetAccountMiiImage"},
        {2011, &IParentalControlService::GetAccountMiiImageContentTypeAsync, "GetAccountMiiImageContentTypeAsync"},
        {2012, &IParentalControlService::FinishGetAccountMiiImageContentType, "FinishGetAccountMiiImageContentType"},
        {2013, &IParentalControlService::SynchronizeParentalControlSettingsAsync, "SynchronizeParentalControlSettingsAsync"},
        {2014, &IParentalControlService::FinishSynchronizeParentalControlSettings, "FinishSynchronizeParentalControlSettings"},
        {2015, &IParentalControlService::FinishSynchronizeParentalControlSettingsWithLastUpdated, "FinishSynchronizeParentalControlSettingsWithLastUpdated"},
        {2016, nullptr, "RequestUpdateExemptionListAsync"}, //5.0.0+
        {2017, &IParentalControlService::AuthorizePairingAsync, "AuthorizePairingAsync"}, //19.0.0+
        {2019, nullptr, "RequestUpdateDeviceUsersBackground"}, //19.0.0+
        {2021, nullptr, "RequestCopyPairingAsync"}, //20.0.0+
        {2022, nullptr, "FinishRequestCopyPairing"}, //20.0.0+
        {2023, nullptr, "IsFromPairingActiveDevice"}, //20.0.0+
        {2024, nullptr, "RollbackCopyPairing"}, //21.0.0+
        {2025, &IParentalControlService::DownloadAllAccountMiiImagesAsync, "DownloadAllAccountMiiImagesAsync"}, // 23.0.0+
        {2026, &IParentalControlService::FinishDownloadAllAccountMiiImages, "FinishDownloadAllAccountMiiImages"}, // 23.0.0+
        {2027, &IParentalControlService::GetPairingAccountChangedEvent, "GetPairingAccountChangedEvent"}, // 23.0.0+
        {3001, nullptr, "GetErrorContextChangedEvent"}, //20.0.0+
        {9407, &IParentalControlService::PrepareMigrationWithEmulatedOldSaveDataForDebug, "PrepareMigrationWithEmulatedOldSaveDataForDebug"}, // 23.0.0+
        {145601, &IParentalControlService::GetPlayTimerSettings, "GetPlayTimerSettings"}, // 18.0.0+
        {195101, nullptr, "SetPlayTimerSettingsForDebug"}, //18.0.0+
    };
    // clang-format on
    RegisterHandlers(functions);

    for (auto& event : slot_events) {
        event = service_context.CreateEvent("IParentalControlService:Async");
    }
    synchronization_event = service_context.CreateEvent("IParentalControlService:Synchronization");
    pin_code_changed_event = service_context.CreateEvent("IParentalControlService:PinCodeChanged");
    unlinked_event = service_context.CreateEvent("IParentalControlService:Unlinked");
    request_suspension_event =
        service_context.CreateEvent("IParentalControlService:RequestSuspension");
    extended_play_timer_event =
        service_context.CreateEvent("IParentalControlService:ExtendedPlayTimer");
    pairing_account_changed_event =
        service_context.CreateEvent("IParentalControlService:PairingAccountChanged");

    // The synchronization event fires when the setting in force changes; qlaunch re-reads its
    // parental-controls state then (IsRestrictionEnabled, the temporary unlock, the play timer).
    synchronization_host_event = worker->MakeHostEvent(synchronization_event);
    pin_code_changed_host_event = worker->MakeHostEvent(pin_code_changed_event);
    // The unlinked event (GetUnlinkedEvent) fires when the pairing goes, from this console or the
    // parent's side; Settings > Parental Controls redraws as unpaired on it.
    unlinked_host_event = worker->MakeHostEvent(unlinked_event);
    listener_id = Pairing::AddListener(
        [sync = synchronization_host_event, pin = pin_code_changed_host_event,
         unlinked = unlinked_host_event](bool pairing_gone) {
            sync->Raise();
            pin->Raise();
            if (pairing_gone) {
                unlinked->Raise();
            }
        });
}

IParentalControlService::~IParentalControlService() {
    Pairing::RemoveListener(listener_id);
    synchronization_host_event->Detach();
    pin_code_changed_host_event->Detach();
    unlinked_host_event->Detach();
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i]) {
            tasks[i]->Detach();
        }
        service_context.CloseEvent(slot_events[i]);
    }
    service_context.CloseEvent(synchronization_event);
    service_context.CloseEvent(pin_code_changed_event);
    service_context.CloseEvent(unlinked_event);
    service_context.CloseEvent(request_suspension_event);
    service_context.CloseEvent(extended_play_timer_event);
    service_context.CloseEvent(pairing_account_changed_event);
}

bool IParentalControlService::Has(u32 bits) const {
    return (static_cast<u32>(capability) & bits) != 0;
}

// The pairing commands need permission bit 0x8, as each of pctl's impls tests first; pctl:s and
// pctl:a carry it.
bool IParentalControlService::CanPair() const {
    return Has(0x8);
}

void IParentalControlService::PushEvent(HLERequestContext& ctx, Kernel::KEvent* event) {
    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, event->GetReadableEvent());
}

// ---------------------------------------------------------------------------------------------
// Initialize and the restriction commands

Result IParentalControlService::InitializeImpl() {
    if (False(capability & (Capability::Application | Capability::System))) {
        LOG_ERROR(Service_PCTL, "Invalid capability! capability={:X}", capability);
        return ResultNoCapability;
    }
    if (program_id != 0) {
        const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                       system.GetContentProvider()};
        const auto control = pm.GetControlMetadata();
        if (control.first) {
            application_info = ApplicationInfo{
                .application_id = program_id,
                .age_rating = control.first->GetRatingAge(),
                .parental_control_flag = control.first->GetParentalControlFlag(),
                .capability = capability,
            };
        }
    }
    return ResultSuccess;
}

void IParentalControlService::Initialize(HLERequestContext& ctx) {
    LOG_DEBUG(Service_PCTL, "called");
    PushResult(ctx, InitializeImpl());
}

void IParentalControlService::CheckFreeCommunicationPermission(HLERequestContext& ctx) {
    const Result result = FreeCommunicationCheck(application_info.application_id,
                                                 (application_info.parental_control_flag & 1) != 0);
    if (result.IsSuccess()) {
        free_communication = true;
    }
    PushResult(ctx, result);
}

void IParentalControlService::IsFreeCommunicationAvailable(HLERequestContext& ctx) {
    PushResult(ctx, FreeCommunicationCheck(application_info.application_id,
                                           (application_info.parental_control_flag & 1) != 0));
}

void IParentalControlService::EndFreeCommunication(HLERequestContext& ctx) {
    free_communication = false;
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::ConfirmLaunchApplicationPermissionOld(HLERequestContext& ctx) {
    // (bool free_communication, u64 application_id, buffer<s8 rating_age[], 0x9>)
    IPC::RequestParser rp{ctx};
    rp.Skip(2, false);
    const auto application_id = rp.Pop<u64>();
    PushResult(ctx, RatingCheck(application_id, ReadRatings(ctx)));
}

void IParentalControlService::ConfirmResumeApplicationPermission(HLERequestContext& ctx) {
    // qlaunch's proxy (0x855e6c0): a bool at +0 and the id at +8. pctl runs its resume variant of
    // the launch gate (.text 0x40dd0), whose extra play-timer inputs are not traced; the rating
    // half is applied.
    IPC::RequestParser rp{ctx};
    rp.Skip(2, false);
    const auto application_id = rp.Pop<u64>();
    PushResult(ctx, RatingCheck(application_id, ReadRatings(ctx)));
}

void IParentalControlService::ConfirmLaunchApplicationPermission(HLERequestContext& ctx) {
    // 1019 (20.0.0+), qlaunch's proxy (0x855ea90): a byte at +0 (only logged by pctl), a bool at
    // +1, the id at +8. pctl clears the free-communication flag here (.text 0x40dc0), so only the
    // rating is checked.
    IPC::RequestParser rp{ctx};
    rp.Skip(2, false);
    const auto application_id = rp.Pop<u64>();
    PushResult(ctx, RatingCheck(application_id, ReadRatings(ctx)));
}

void IParentalControlService::ConfirmSnsPostPermission(HLERequestContext& ctx) {
    const auto restrictions = Pairing::Current().InForce();
    if (restrictions.temporarily_unlocked || !restrictions.Enabled() ||
        !CurrentRestriction(restrictions).sns_post_restricted) {
        PushResult(ctx, ResultSuccess);
        return;
    }
    LOG_UNKNOWN_CONTRACT(Service_PCTL,
                         "pctl 1004: SNS posting restricted, answered 2142-0101 as before");
    PushResult(ctx, ResultNoFreeCommunication);
}

void IParentalControlService::ConfirmSystemSettingsPermission(HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::IsRestrictionTemporaryUnlocked(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(Pairing::Current().InForce().temporarily_unlocked);
}

void IParentalControlService::RevertRestrictionTemporaryUnlocked(HLERequestContext& ctx) {
    Pairing::Current().SetTemporarilyUnlocked(false);
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::EnterRestrictedSystemSettings(HLERequestContext& ctx) {
    restricted_system_settings_entered = true;
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::LeaveRestrictedSystemSettings(HLERequestContext& ctx) {
    restricted_system_settings_entered = false;
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::IsRestrictedSystemSettingsEntered(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(restricted_system_settings_entered.load());
}

void IParentalControlService::GetRestrictedFeatures(HLERequestContext& ctx) {
    // bit 0 age rating, bit 1 SNS posting, bit 2 free communication (.text 0x41750).
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(RestrictedFeatures());
}

void IParentalControlService::GetRestrictedFeatureFlagSet(HLERequestContext& ctx) {
    // pctl 23.0.1 (impl 0x12d550) answers 1012's value without 1012's & 0xf; today both are the
    // same. Needs permission bit 0x100.
    if (!Has(0x100)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(RestrictedFeatures());
}

void IParentalControlService::ConfirmStereoVisionPermission(HLERequestContext& ctx) {
    // .text 0x41110: an unlocked console may; otherwise a PIN and the restriction refuse.
    const auto restrictions = Pairing::Current().InForce();
    const bool denied = !restrictions.temporarily_unlocked && restrictions.Enabled() &&
                        restrictions.stereo_vision_restricted;
    PushResult(ctx, denied ? ResultStereoVisionRestricted : ResultSuccess);
}

void IParentalControlService::IsStereoVisionPermitted(HLERequestContext& ctx) {
    const auto restrictions = Pairing::Current().InForce();
    const bool denied = !restrictions.temporarily_unlocked && restrictions.Enabled() &&
                        restrictions.stereo_vision_restricted;
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(denied ? ResultStereoVisionRestricted : ResultSuccess);
    rb.Push(!denied);
}

void IParentalControlService::IsRestrictionEnabled(HLERequestContext& ctx) {
    if (!Has(0x140)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(Pairing::Current().InForce().Enabled());
}

void IParentalControlService::GetSafetyLevel(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(Pairing::Current().InForce().safety_level));
}

void IParentalControlService::GetSafetyLevelSettings(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto level = rp.Pop<u32>();
    if (level > 4) {
        PushResult(ctx, ResultInvalidArgument);
        return;
    }
    const auto restrictions = Pairing::Current().InForce();
    const auto settings =
        level == 1 ? restrictions.custom : Rules::Preset(static_cast<int>(level), Region());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw(RestrictionSettings{settings.rating_age, settings.sns_post_restricted,
                                   settings.free_communication_restricted});
}

void IParentalControlService::GetCurrentSettings(HLERequestContext& ctx) {
    const auto current = CurrentRestriction(Pairing::Current().InForce());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw(RestrictionSettings{current.rating_age, current.sns_post_restricted,
                                   current.free_communication_restricted});
}

void IParentalControlService::GetDefaultRatingOrganization(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(RatingOrganization(Pairing::Current().InForce())));
}

void IParentalControlService::GetFreeCommunicationApplicationListCount(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<s32>(Pairing::Current().InForce().free_communication_applications.size()));
}

void IParentalControlService::GetFreeCommunicationApplicationList(HLERequestContext& ctx) {
    // (s32 offset, buffer<FreeCommunicationApplicationInfo, 6>) -> s32. An entry is 0x10 bytes:
    // the title id, then whether it is restricted at +8 (pctl's copy-out, .text 0x3cea0).
    IPC::RequestParser rp{ctx};
    const auto offset = std::max(rp.Pop<s32>(), 0);
    const auto list = Pairing::Current().InForce().free_communication_applications;
    const std::size_t capacity = ctx.CanWriteBuffer() ? ctx.GetWriteBufferSize() / 0x10 : 0;
    std::vector<u8> out;
    s32 count = 0;
    for (std::size_t i = static_cast<std::size_t>(offset);
         i < list.size() && static_cast<std::size_t>(count) < capacity; ++i, ++count) {
        std::array<u8, 0x10> entry{};
        std::memcpy(entry.data(), &list[i].first, sizeof(u64));
        entry[8] = list[i].second ? 1 : 0;
        out.insert(out.end(), entry.begin(), entry.end());
    }
    if (!out.empty()) {
        ctx.WriteBuffer(out.data(), out.size());
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(count);
}

// ---------------------------------------------------------------------------------------------
// The console's own setters (System Settings > Parental Controls without a smart device). Each
// impl tests permission bit 0x4 first (pctl:s and pctl:a carry it), then its arguments; 22.5.0
// impls at .text 0x2cff0, 0x2d170, 0x2d230, 0x2d440, 0x2d480, 0x2d3f0, 0x2d270. The Ryujinx Harbor
// has no setters; these contracts are read from the firmware.

void IParentalControlService::SetSafetyLevel(HLERequestContext& ctx) {
    if (!Has(0x4)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto level = rp.Pop<u32>();
    if (level > 4) {
        PushResult(ctx, ResultInvalidArgument);
        return;
    }
    Pairing::Current().SetSafetyLevel(static_cast<u8>(level),
                                      Rules::DefaultOrganization(Region(), Language()));
    LOG_INFO(Service_PCTL, "pctl: safety level set to {}", SafetyLevelName(static_cast<u8>(level)));
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::SetCustomSafetyLevelSettings(HLERequestContext& ctx) {
    if (!Has(0x4)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto settings = rp.PopRaw<RestrictionSettings>();
    Pairing::Current().SetCustomSettings(
        {settings.rating_age, settings.sns_post_restriction != 0,
         settings.free_communication_restriction != 0});
    LOG_INFO(Service_PCTL, "pctl: custom restriction settings set (age {})", settings.rating_age);
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::SetDefaultRatingOrganization(HLERequestContext& ctx) {
    if (!Has(0x4)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto organization = rp.Pop<u32>();
    // pctl keeps one byte and checks nothing (.text 0x41820).
    Pairing::Current().SetDefaultRatingOrganization(static_cast<u8>(organization));
    LOG_INFO(Service_PCTL, "pctl: default rating organization set to {}",
             static_cast<u8>(organization));
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::AddToFreeCommunicationApplicationList(HLERequestContext& ctx) {
    // 1042 (u64 application_id): listed with the flag clear.
    if (!Has(0x4)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto application_id = rp.Pop<u64>();
    if (!Pairing::Current().AddFreeCommunicationApplication(application_id, false)) {
        LOG_WARNING(Service_PCTL, "pctl: the free-communication list is full");
    }
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::AddToFreeCommunicationApplicationListWithFlag(
    HLERequestContext& ctx) {
    // 1050 (u64 application_id, bool): the same with the flag given.
    if (!Has(0x4)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto application_id = rp.Pop<u64>();
    const auto restricted = rp.Pop<bool>();
    if (!Pairing::Current().AddFreeCommunicationApplication(application_id, restricted)) {
        LOG_WARNING(Service_PCTL, "pctl: the free-communication list is full");
    }
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::UpdateFreeCommunicationApplicationList(HLERequestContext& ctx) {
    // (buffer<FreeCommunicationApplicationInfo, 5>): 0x10-byte entries, the title id and at +8
    // whether it is restricted, each added or changed (.text 0x42140 over 0x3cec0).
    if (!Has(0x4)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    std::vector<std::pair<std::uint64_t, bool>> entries;
    if (ctx.CanReadBuffer(0)) {
        const auto buffer = ctx.ReadBuffer(0);
        for (std::size_t offset = 0; offset + 0x10 <= buffer.size(); offset += 0x10) {
            std::uint64_t id;
            std::memcpy(&id, buffer.data() + offset, sizeof(id));
            entries.emplace_back(id, buffer[offset + 8] != 0);
        }
    }
    if (!entries.empty()) {
        Pairing::Current().UpdateFreeCommunicationApplications(entries);
    }
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::DeleteSettings(HLERequestContext& ctx) {
    // Needs permission bit 0x4 or 0x40 (.text 0x2d270). pctl resets its savedata and tears the
    // pairing down; here the setting, the PIN and any pairing are dropped on this console only.
    if (!Has(0x44)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    LOG_UNKNOWN_CONTRACT(Service_PCTL,
                         "pctl 1043 DeleteSettings: local reset only; the server is not told of "
                         "a pairing dropped with it");
    Pairing::Current().DeleteSettings();
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::ConfirmStereoVisionRestrictionConfigurable(HLERequestContext& ctx) {
    PushResult(ctx, Has(0x2) ? ResultSuccess : ResultPermissionDenied);
}

void IParentalControlService::GetStereoVisionRestriction(HLERequestContext& ctx) {
    if (!Has(0x200)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(Pairing::Current().InForce().stereo_vision_restricted);
}

void IParentalControlService::SetStereoVisionRestriction(HLERequestContext& ctx) {
    if (!Has(0x200)) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    Pairing::Current().SetStereoVisionRestricted(rp.Pop<bool>());
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::ResetConfirmedStereoVisionPermission(HLERequestContext& ctx) {
    PushResult(ctx, ResultSuccess);
}

// ---------------------------------------------------------------------------------------------
// The PIN (pctl .text 0x41550, 0x41b10, 0x41c90). It arrives NUL-terminated in an X buffer and is
// never logged.

void IParentalControlService::UnlockRestrictionTemporarily(HLERequestContext& ctx) {
    const Result result = CheckPin(ReadString(ctx, 0x10));
    if (result.IsSuccess()) {
        Pairing::Current().SetTemporarilyUnlocked(true);
    }
    PushResult(ctx, result);
}

void IParentalControlService::UnlockSystemSettingsRestriction(HLERequestContext& ctx) {
    PushResult(ctx, CheckPin(ReadString(ctx, 0x10)));
}

void IParentalControlService::UnlockSystemSettingsRestrictionWithReason(HLERequestContext& ctx) {
    // (u8 reason, buffer<char, 0x9> pin): the byte only goes into pctl's event log (.text 0x2dc14).
    PushResult(ctx, CheckPin(ReadString(ctx, 0x10)));
}

void IParentalControlService::SetPinCode(HLERequestContext& ctx) {
    const std::string pin = ReadString(ctx, 0x10);
    // Empty clears it; otherwise four to eight digits (.text 0x41b58).
    if (!pin.empty() &&
        (pin.size() < 4 || pin.size() > 8 ||
         !std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; }))) {
        PushResult(ctx, ResultInvalidArgument);
        return;
    }
    // A PIN set on a paired console reaches the server with the next sync receipt.
    Pairing::Current().SetPin(pin);
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::GenerateInquiryCode(HLERequestContext& ctx) {
    // The current format (pctl .text 0x63a20) is not traced; this is the documented 3.0.1 shape,
    // "11" and eight random digits. Without Nintendo's master-key data no key can match it (1205).
    LOG_UNKNOWN_CONTRACT(Service_PCTL, "pctl 1204 inquiry code format");
    std::array<char, 0x20> code{};
    std::mt19937 random{std::random_device{}()};
    const auto digits = std::uniform_int_distribution<u32>{0, 99'999'999}(random);
    std::snprintf(code.data(), code.size(), "11%08u", digits);
    IPC::ResponseBuilder rb{ctx, 2 + 8};
    rb.Push(ResultSuccess);
    rb.PushRaw(code);
}

void IParentalControlService::CheckMasterKey(HLERequestContext& ctx) {
    LOG_UNKNOWN_CONTRACT(Service_PCTL, "pctl 1205 master key (Nintendo's key data); no match");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(false);
}

void IParentalControlService::GetPinCodeLength(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<s32>(Pairing::Current().InForce().pin.size()));
}

void IParentalControlService::NotifyWrongPinCodeInputManyTimes(HLERequestContext& ctx) {
    // The firmware's PIN applet calls this after repeated wrong entries; leaving it out made the
    // applet assert on its first wrong PIN. pctl records an event for the parent app to see; with
    // no events upload here nothing is kept.
    LOG_UNKNOWN_CONTRACT(Service_PCTL, "pctl 1471 NotifyWrongPinCodeInputManyTimes: no event is "
                                       "recorded or uploaded; answered success");
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::GetPinCodeChangedEvent(HLERequestContext& ctx) {
    PushEvent(ctx, pin_code_changed_event);
}

void IParentalControlService::GetPinCode(HLERequestContext& ctx) {
    const u32 length = WriteString(ctx, Pairing::Current().InForce().pin);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(length);
}

// ---------------------------------------------------------------------------------------------
// Pairing state

void IParentalControlService::IsPairingActive(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(Pairing::Current().IsPairingActive());
}

void IParentalControlService::GetSettingsLastUpdated(HLERequestContext& ctx) {
    const auto updated = Pairing::Current().SettingsLastUpdated();
    if (updated == 0) {
        PushResult(ctx, ResultSettingsNeverSynchronized);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(updated);
}

void IParentalControlService::GetPairingAccountInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto info = rp.PopRaw<PairingInfoBase>();
    auto& pairing = Pairing::Current();
    if (info.state == 0) {
        PushResult(ctx, ResultPairingUnavailable);
        return;
    }
    if (info.device_id != pairing.FoundDevice()) {
        PushResult(ctx, ResultPairingAccountMismatch);
        return;
    }
    const auto owner = pairing.FirstOwner();
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(PairingAccountInfoBase{info.device_id, owner ? owner->nintendo_account_id : 0});
}

void IParentalControlService::GetAllPairingAccountInfos(HLERequestContext& ctx) {
    // 23.0.1 (impl 0x12e830): 1411 for every owner, min(capacity, owners) entries; a capacity of
    // six or more is 2142-0131, state 0 is 2142-0135, another device or no owner 2142-0222.
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto info = rp.PopRaw<PairingInfoBase>();
    auto& pairing = Pairing::Current();
    const auto owners = pairing.Owners();
    const std::size_t capacity = ctx.CanWriteBuffer() ? ctx.GetWriteBufferSize() / 0x10 : 0;
    if (info.state == 0) {
        PushResult(ctx, ResultPairingUnavailable);
        return;
    }
    if (capacity > Moon::MaxOwners) {
        PushResult(ctx, ResultInvalidArgument);
        return;
    }
    if (info.device_id != pairing.FoundDevice() || owners.empty()) {
        PushResult(ctx, ResultPairingAccountMismatch);
        return;
    }
    const std::size_t count = std::min(capacity, owners.size());
    std::vector<PairingAccountInfoBase> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back({info.device_id, owners[i].nintendo_account_id});
    }
    if (!out.empty()) {
        ctx.WriteBuffer(out.data(), out.size() * sizeof(PairingAccountInfoBase));
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u8>(count));
}

void IParentalControlService::GetAccountNickname(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account = rp.PopRaw<PairingAccountInfoBase>();
    auto& pairing = Pairing::Current();
    const auto owner = pairing.FirstOwner();
    if (!owner || account.device_id != pairing.FoundDevice()) {
        PushResult(ctx, ResultPairingAccountMismatch);
        return;
    }
    // pctl copies at most 32 bytes and answers the string's length (.text 0x2e44c).
    const u32 length = WriteString(ctx, owner->nickname);
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(length);
}

void IParentalControlService::GetAccountState(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto account = rp.PopRaw<PairingAccountInfoBase>();
    auto& pairing = Pairing::Current();
    const auto owner = pairing.FirstOwner();
    if (!owner || account.device_id != pairing.FoundDevice()) {
        PushResult(ctx, ResultPairingAccountMismatch);
        return;
    }
    // 0 for an owner with a nickname, 1 for one without (.text 0x2e4fc). What 1 means is not
    // named.
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(owner->nickname.empty() ? 1u : 0u);
}

void IParentalControlService::GetSynchronizationEvent(HLERequestContext& ctx) {
    PushEvent(ctx, synchronization_event);
}

// ---------------------------------------------------------------------------------------------
// The play timer, for parity: counted, never enforced.

void IParentalControlService::StartPlayTimer(HLERequestContext& ctx) {
    PlayedToday();
    auto& clock = Clock();
    {
        std::lock_guard lock{clock.mutex};
        if (!clock.running) {
            clock.running = true;
            clock.started = std::chrono::steady_clock::now();
        }
    }
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::StopPlayTimer(HLERequestContext& ctx) {
    const auto played = PlayedToday();
    auto& clock = Clock();
    {
        std::lock_guard lock{clock.mutex};
        clock.played_today = played;
        clock.running = false;
    }
    PushResult(ctx, ResultSuccess);
}

void IParentalControlService::IsPlayTimerEnabled(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(Pairing::Current().InForce().play_timer[0] != 0);
}

void IParentalControlService::GetPlayTimerRemainingTime(HLERequestContext& ctx) {
    // -> nn::TimeSpan (s64 nanoseconds; qlaunch divides by 10^9). With no limit today: 0x7fffffff.
    const auto limit = TodaysLimit(Pairing::Current().InForce().play_timer);
    s64 remaining = 0x7fffffff;
    if (limit) {
        const auto left = std::chrono::minutes{*limit} - PlayedToday();
        remaining = std::max<s64>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count());
    }
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(remaining);
}

void IParentalControlService::IsRestrictedByPlayTimer(HLERequestContext& ctx) {
    // Inferred (as in the Ryujinx Harbor): restricted once today's limit is used up under
    // FORCED_TERMINATION (+1 == 1), unless unlocked.
    const auto restrictions = Pairing::Current().InForce();
    const auto limit = TodaysLimit(restrictions.play_timer);
    const bool restricted = limit && restrictions.play_timer[1] == 1 &&
                            !restrictions.temporarily_unlocked &&
                            PlayedToday() >= std::chrono::minutes{*limit};
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(restricted);
}

void IParentalControlService::GetPlayTimerSettingsOld(HLERequestContext& ctx) {
    PlayTimerSettingsOld old{};
    const auto timer = Pairing::Current().InForce().play_timer;
    std::memcpy(&old, timer.data(), sizeof(old));
    IPC::ResponseBuilder rb{ctx, 2 + sizeof(PlayTimerSettingsOld) / 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(old);
}

void IParentalControlService::GetPlayTimerSettings(HLERequestContext& ctx) {
    // 0x44 bytes since 21.0.0 (layout in pctl_setting.h).
    PlayTimerSettings settings{};
    const auto timer = Pairing::Current().InForce().play_timer;
    std::memcpy(&settings, timer.data(), sizeof(settings));
    IPC::ResponseBuilder rb{ctx, 2 + sizeof(PlayTimerSettings) / 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(settings);
}

void IParentalControlService::GetPlayTimerEventToRequestSuspension(HLERequestContext& ctx) {
    PushEvent(ctx, request_suspension_event);
}

void IParentalControlService::IsPlayTimerAlarmDisabled(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(false);
}

void IParentalControlService::GetPlayTimerRemainingTimeDisplayInfo(HLERequestContext& ctx) {
    PushDisplayInfo(ctx, false, true);
}

void IParentalControlService::GetWatcherStatusDisplayInfo(HLERequestContext& ctx) {
    // 23.0.1 (impl 0x12f0a0), (bool ignore_temporary_unlock). Needs bits 0x8 and 0x100.
    if ((static_cast<u32>(capability) & 0x108) != 0x108) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    PushDisplayInfo(ctx, rp.Pop<u8>() != 0, false);
}

void IParentalControlService::PushDisplayInfo(HLERequestContext& ctx, bool ignore_temporary_unlock,
                                              bool for_1459) {
    const auto restrictions = Pairing::Current().InForce();
    const std::tm local = LocalNow();
    const Rules::PlayTimerNow now{
        .day_of_week = local.tm_wday,
        .hour = local.tm_hour,
        .minute = local.tm_min,
        .second = local.tm_sec,
        .posix = static_cast<std::int64_t>(std::time(nullptr)),
        .played_ns = PlayedToday().count(),
    };
    const auto display =
        Rules::DisplayInfo(restrictions.play_timer, restrictions.Enabled(),
                           restrictions.temporarily_unlocked, ignore_temporary_unlock, now, for_1459);
    PlayTimerRemainingTimeDisplayInfo info{};
    info.state = static_cast<PlayTimerDisplayState>(display.state);
    info.bedtime = display.bedtime;
    info.remaining_time = display.remaining_ns;
    IPC::ResponseBuilder rb{ctx, 2 + sizeof(PlayTimerRemainingTimeDisplayInfo) / 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(info);
}

void IParentalControlService::GetUnlinkedEvent(HLERequestContext& ctx) {
    PushEvent(ctx, unlinked_event);
}

void IParentalControlService::GetExtendedPlayTimerEvent(HLERequestContext& ctx) {
    PushEvent(ctx, extended_play_timer_event);
}

// ---------------------------------------------------------------------------------------------
// The pairing commands' async half

void IParentalControlService::StartAsync(HLERequestContext& ctx, const char* what, Work work,
                                         u32 extra_word, bool extra) {
    const AsyncData data = slots.Begin();
    // Starting over a slot still running drops that work.
    if (auto& old = tasks[data.index]) {
        old->Detach();
    }
    auto* event = slot_events[data.index];
    event->Clear(system.Kernel());
    tasks[data.index] = worker->Run(event, ResultServerUnreachable,
                                    [work = std::move(work), what](std::stop_token, std::vector<u8>&) {
                                        const Result result = work();
                                        LOG_DEBUG(Service_PCTL, "pctl: {} done", what);
                                        return result;
                                    });
    IPC::ResponseBuilder rb{ctx, extra ? 5u : 4u, 1};
    rb.Push(ResultSuccess);
    rb.PushRaw(data);
    if (extra) {
        rb.Push(extra_word);
    }
    rb.PushCopyObjects(ctx, event->GetReadableEvent());
}

void IParentalControlService::AnswerNothing(HLERequestContext& ctx, bool extra, u32 extra_word) {
    // qlaunch's wrappers read handle zero as "done, no Finish" (RetrievePairingInfo 0x855c8e4,
    // UnlinkPairing 0x855ca18, Synchronize 0x855cb8c).
    IPC::ResponseBuilder rb{ctx, extra ? 5u : 4u, 1};
    rb.Push(ResultSuccess);
    rb.PushRaw(AsyncData{});
    if (extra) {
        rb.Push(extra_word);
    }
    rb.PushCopyObjects(ctx, static_cast<Kernel::KReadableEvent*>(nullptr));
}

Result IParentalControlService::FinishAsync(const AsyncData& data) {
    const auto slot = slots.Find(data);
    if (!slot || !tasks[*slot]) {
        return ResultInvalidAsyncData;
    }
    auto& task = tasks[*slot];
    if (!task->IsDone()) {
        // pctl's Finish takes the work's result; the guest asks once the event fired. A Finish
        // before that is answered without waiting (an IPC thread never waits on the network) and
        // the slot stays live.
        LOG_UNKNOWN_CONTRACT(Service_PCTL,
                             "pctl Finish before the work is done; answered 2142-0221, slot kept");
        return ResultInvalidAsyncData;
    }
    const Result result = task->GetResult();
    task->Detach();
    task.reset();
    slots.Release(*slot);
    return result;
}

void IParentalControlService::RequestPairingAsync(HLERequestContext& ctx) {
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    // NUL-terminated, at most 15 characters (qlaunch's 16-byte copy of the typed text). Never
    // logged.
    const std::string code = ReadString(ctx, 0x100);
    LOG_INFO(Service_PCTL, "pctl: pairing with a {}-character code", code.size());
    StartAsync(ctx, "RequestPairing",
               [code] { return PctlResult(Pairing::Current().RequestPairing(code)); });
}

void IParentalControlService::FinishRequestPairing(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const Result result = FinishAsync(rp.PopRaw<AsyncData>());
    if (result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(PairingInfoBase{Pairing::Current().FoundDevice(), Pairing::StateFound});
}

void IParentalControlService::AuthorizePairingAsyncOld(HLERequestContext& ctx) {
    // pctl runs it as 2017 with an owner id of zero (.text 0x2faf0).
    IPC::RequestParser rp{ctx};
    AuthorizePairing(ctx, rp.PopRaw<PairingInfoBase>());
}

void IParentalControlService::AuthorizePairingAsync(HLERequestContext& ctx) {
    // pctl hands the chosen owner's id to its authorize executor (.text 0x2fa44); the hardware
    // activate body carries no owner, so it is not sent.
    IPC::RequestParser rp{ctx};
    const auto info = rp.PopRaw<PairingInfoBase>();
    LOG_UNKNOWN_CONTRACT(Service_PCTL, "pctl 2017's owner argument is not sent");
    AuthorizePairing(ctx, info);
}

void IParentalControlService::AuthorizePairing(HLERequestContext& ctx,
                                               const PairingInfoBase& info) {
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    if (info.device_id != Pairing::Current().FoundDevice() || info.state != Pairing::StateFound) {
        PushResult(ctx, ResultInvalidPairingInfo);
        return;
    }

    Pairing::DeviceFacts facts;
    facts.language = Language();
    facts.region = std::string{Moon::RegionName(Region())};
    Set::FirmwareVersionFormat firmware{};
    if (Set::GetFirmwareVersionImpl(firmware, system, Set::GetFirmwareVersionType::Version2)
            .IsSuccess()) {
        facts.displayed_version =
            std::string(firmware.display_version.data(),
                        strnlen(firmware.display_version.data(), firmware.display_version.size()));
        facts.internal_version = (u32{firmware.major} << 16) | (u32{firmware.minor} << 8) |
                                 u32{firmware.micro};
    }
    const u64 device = info.device_id;
    StartAsync(ctx, "AuthorizePairing", [device, facts] {
        return PctlResult(Pairing::Current().Authorize(device, facts));
    });
}

void IParentalControlService::FinishAuthorizePairing(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const Result result = FinishAsync(rp.PopRaw<AsyncData>());
    if (result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(PairingInfoBase{Pairing::Current().PairedDevice(), Pairing::StateActive});
}

void IParentalControlService::RetrievePairingInfoAsync(HLERequestContext& ctx) {
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    if (!Pairing::Current().IsPairingActive()) {
        AnswerNothing(ctx);
        return;
    }
    StartAsync(ctx, "RetrievePairingInfo",
               [] { return PctlResult(Pairing::Current().Retrieve()); });
}

void IParentalControlService::FinishRetrievePairingInfo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const Result result = FinishAsync(rp.PopRaw<AsyncData>());
    if (result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    auto& pairing = Pairing::Current();
    // No owner left is a parent who unlinked from their side: pctl forgets the pairing
    // (.text 0x30004).
    if (pairing.Owners().empty()) {
        pairing.ForgetPairing();
        PushResult(ctx, ResultPairingUnlinked);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(PairingInfoBase{pairing.PairedDevice(), Pairing::StateActive});
}

void IParentalControlService::UnlinkPairingAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const bool force = rp.Pop<bool>();
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    if (!Pairing::Current().IsPairingActive()) {
        AnswerNothing(ctx);
        return;
    }
    StartAsync(ctx, "UnlinkPairing",
               [force] { return PctlResult(Pairing::Current().Unlink(force)); });
}

void IParentalControlService::FinishUnlinkPairing(HLERequestContext& ctx) {
    // (bool force, AsyncData at +4).
    IPC::RequestParser rp{ctx};
    rp.Skip(1, false);
    PushResult(ctx, FinishAsync(rp.PopRaw<AsyncData>()));
}

// 2009-2012, as pctl 23.0.1 answers them (impl 0x08531180/0x08531370, 0x085314F0/0x085316E0;
// research/g3pctl-mii/notes.md): a picture already in the icon cache is answered at once (AsyncData
// zero, no event, the full size whatever the buffer, min(buffer, size) bytes copied); otherwise the
// download is async work and Finish answers the same way from the cache. qlaunch's setup page asks
// with an empty buffer first for the size, then again with a buffer of that size.
void IParentalControlService::GetAccountMiiImageAsync(HLERequestContext& ctx) {
    // (PairingAccountInfoBase, buffer<bytes, 6>) -> (AsyncData, u32 size, handle<copy>).
    StartMii(ctx, "GetAccountMiiImage", false);
}

void IParentalControlService::GetAccountMiiImageContentTypeAsync(HLERequestContext& ctx) {
    // (PairingAccountInfoBase, buffer<char, 0xa>) -> (AsyncData, u32 length, handle<copy>).
    StartMii(ctx, "GetAccountMiiImageContentType", true);
}

void IParentalControlService::StartMii(HLERequestContext& ctx, const char* what,
                                       bool content_type) {
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    const auto account = rp.PopRaw<PairingAccountInfoBase>();
    auto& pairing = Pairing::Current();
    const auto owners = pairing.Owners();
    const bool known = std::any_of(owners.begin(), owners.end(), [&](const Moon::Owner& o) {
        return o.nintendo_account_id == account.nintendo_account_id;
    });
    if (account.device_id != pairing.FoundDevice() || !known) {
        PushResult(ctx, ResultPairingAccountMismatch);
        return;
    }
    mii_account = account.nintendo_account_id;
    if (const auto cached = pairing.Mii(account.nintendo_account_id)) {
        const u32 size = content_type ? WriteString(ctx, cached->second)
                                      : WriteMii(ctx, cached->first);
        AnswerNothing(ctx, true, size);
        return;
    }
    const u64 id = account.nintendo_account_id;
    StartAsync(ctx, what, [id] { return PctlResult(Pairing::Current().FetchMii(id)); }, 0, true);
}

u32 IParentalControlService::WriteMii(HLERequestContext& ctx, const std::vector<u8>& image) {
    const std::size_t room = ctx.CanWriteBuffer() ? ctx.GetWriteBufferSize() : 0;
    if (const std::size_t copied = std::min(image.size(), room); copied > 0) {
        ctx.WriteBuffer(image.data(), copied);
    }
    return static_cast<u32>(image.size());
}

void IParentalControlService::FinishGetAccountMiiImage(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const Result result = FinishAsync(rp.PopRaw<AsyncData>());
    if (result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    const auto cached = Pairing::Current().Mii(mii_account);
    const u32 size = WriteMii(ctx, cached ? cached->first : std::vector<u8>{});
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(size);
}

void IParentalControlService::FinishGetAccountMiiImageContentType(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const Result result = FinishAsync(rp.PopRaw<AsyncData>());
    if (result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    // The stored HTTP Content-Type: u32 its length, the text in the type-C buffer (at most 0x20).
    const auto cached = Pairing::Current().Mii(mii_account);
    const u32 length = WriteString(ctx, cached ? cached->second : std::string{});
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(length);
}

void IParentalControlService::SynchronizeParentalControlSettingsAsync(HLERequestContext& ctx) {
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    if (!Pairing::Current().IsPairingActive()) {
        AnswerNothing(ctx);
        return;
    }
    // A changed setting signals the synchronization event through the pairing's listeners.
    StartAsync(ctx, "SynchronizeParentalControlSettings",
               [] { return PctlResult(Pairing::Current().Synchronize()); });
}

void IParentalControlService::FinishSynchronizeParentalControlSettings(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    PushResult(ctx, FinishAsync(rp.PopRaw<AsyncData>()));
}

void IParentalControlService::FinishSynchronizeParentalControlSettingsWithLastUpdated(
    HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const Result result = FinishAsync(rp.PopRaw<AsyncData>());
    if (result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(Pairing::Current().SettingsLastUpdated());
}

void IParentalControlService::DownloadAllAccountMiiImagesAsync(HLERequestContext& ctx) {
    // 23.0.1 (impl 0x130e60): each entry must be an owner of the paired device; one icon download
    // is queued per owner whose cached icon is not current, and with nothing to download AsyncData
    // zero and no event. qlaunch's setup page runs it before asking for a picture (2009).
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    const auto buffer = ctx.CanReadBuffer(0) ? ctx.ReadBuffer(0) : std::span<const u8>{};
    const std::size_t count = buffer.size() / sizeof(PairingAccountInfoBase);
    if (count > Moon::MaxOwners) {
        PushResult(ctx, ResultInvalidArgument);
        return;
    }
    auto& pairing = Pairing::Current();
    const auto owners = pairing.Owners();
    std::vector<u64> missing;
    for (std::size_t i = 0; i < count; ++i) {
        PairingAccountInfoBase account;
        std::memcpy(&account, buffer.data() + i * sizeof(account), sizeof(account));
        const bool known = std::any_of(owners.begin(), owners.end(), [&](const Moon::Owner& o) {
            return o.nintendo_account_id == account.nintendo_account_id;
        });
        if (account.device_id != pairing.FoundDevice() || !known) {
            PushResult(ctx, ResultPairingAccountMismatch);
            return;
        }
        if (!pairing.Mii(account.nintendo_account_id)) {
            missing.push_back(account.nintendo_account_id);
        }
    }
    if (missing.empty()) {
        AnswerNothing(ctx);
        return;
    }
    StartAsync(ctx, "DownloadAllAccountMiiImages", [missing] {
        Moon::Description result = Moon::Success;
        for (const u64 id : missing) {
            if (const auto fetched = Pairing::Current().FetchMii(id); fetched != Moon::Success) {
                result = fetched;
            }
        }
        return PctlResult(result);
    });
}

void IParentalControlService::FinishDownloadAllAccountMiiImages(HLERequestContext& ctx) {
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    IPC::RequestParser rp{ctx};
    PushResult(ctx, FinishAsync(rp.PopRaw<AsyncData>()));
}

void IParentalControlService::GetPairingAccountChangedEvent(HLERequestContext& ctx) {
    // pctl 23.0.1 signals it after an owners re-read stored a different owner set (0x150490);
    // Harbor re-reads the owners only when pairing, so it is never signalled.
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    PushEvent(ctx, pairing_account_changed_event);
}

void IParentalControlService::PrepareMigrationWithEmulatedOldSaveDataForDebug(
    HLERequestContext& ctx) {
    // Debug: pctl 23.0.1 rewrites its save data in an old layout. Harbor keeps none in that form.
    if (!CanPair()) {
        PushResult(ctx, ResultPermissionDenied);
        return;
    }
    LOG_WARNING(Service_PCTL, "pctl 9407: debug save-data migration ignored");
    PushResult(ctx, ResultSuccess);
}

} // namespace Service::PCTL
