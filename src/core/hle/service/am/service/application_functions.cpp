// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>

#include <openssl/evp.h>


#include "common/hex_util.h"
#include "common/redact.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/savedata_factory.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/result.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/application_functions.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/ns/application_manager_interface.h"
#include "core/hle/service/ns/service_getter_interface.h"
#include "core/hle/service/sm/sm.h"
#include "core/launch_timestamp_cache.h"
#include "frontend_common/play_time_manager.h"

namespace Service::AM {

namespace {

// pdm: a user that is not on the console (2178-0101).
constexpr Result ResultPdmUserNotFound{ErrorModule::PDM, 101};
// am: an id outside the caller's play log white list (2128-0500).
constexpr Result ResultObjectInvalid{ErrorModule::AM, 500};

std::unique_ptr<FileSys::RawNACP> ReadControlProperty(Core::System& system, u64 program_id) {
    std::vector<u8> nacp;
    if (R_FAILED(system.GetARPManager().GetControlProperty(&nacp, program_id))) {
        return nullptr;
    }
    auto raw = std::make_unique<FileSys::RawNACP>();
    std::memcpy(raw.get(), nacp.data(), (std::min)(sizeof(*raw), nacp.size()));
    return raw;
}

// What pdm:qry answers for the system (QueryApplicationPlayStatisticsForSystem), filtered by the
// caller's play log capability as am does before it asks pdm: None answers the caller's own
// title only, WhiteList refuses an id outside the list, All answers every id. Behaviour follows
// the Ryujinx Harbor (MIT-licensed Ryujinx base), QueryPlayStatisticsManager.cs.
Result QueryPlayStatistics(Core::System& system, u64 caller_program_id, s32& out_entries,
                           std::span<ApplicationPlayStatistics> out_stats,
                           std::span<const u64> application_ids) {
    const auto nacp = ReadControlProperty(system, caller_program_id);
    const auto capability =
        nacp ? nacp->play_log_query_capability : FileSys::PlayLogQueryCapability::None;
    const u64 own_id = FileSys::GetBaseTitleID(caller_program_id);

    if (capability == FileSys::PlayLogQueryCapability::WhiteList) {
        std::array<u64, 16> allowed{};
        std::memcpy(allowed.data(), nacp->play_log_queryable_application_id.data(),
                    sizeof(allowed));
        for (const u64 id : application_ids) {
            if (std::find(allowed.begin(), allowed.end(), id) == allowed.end()) {
                LOG_INFO(Service_AM, "play statistics of {:016X} refused: not in the white list",
                         id);
                R_THROW(ResultObjectInvalid);
            }
        }
    }

    // The frontend's PlayTimeManager owns the play time file; this reads what it last saved.
    const auto play_times = PlayTime::PlayTimeManager::ReadSaved();
    s32 written = 0;
    for (const u64 id : application_ids) {
        if (static_cast<size_t>(written) >= out_stats.size()) {
            break;
        }
        if (capability == FileSys::PlayLogQueryCapability::None && id != own_id) {
            continue;
        }
        out_stats[written++] = {
            .application_id = id,
            .play_time_ns = (play_times.contains(id) ? play_times.at(id) : 0) * 1'000'000'000ULL,
            .launch_count = Core::LaunchTimestampCache::GetLaunchCount(id),
        };
    }
    out_entries = written;
    LOG_DEBUG(Service_AM, "play statistics: {} of {} ids answered (capability {})", written,
              application_ids.size(), static_cast<u32>(capability));
    R_SUCCEED();
}

FileSys::PatchManager::Metadata GetApplicationMetadata(Core::System& system, u64 program_id) {
    auto metadata = FileSys::PatchManager::GetMetadataFromBaseOrUpdate(system, program_id);
    if (metadata.first != nullptr) {
        return metadata;
    }

    const auto application_id = FileSys::GetBaseTitleID(program_id);
    if (application_id == program_id) {
        return metadata;
    }

    LOG_DEBUG(Service_AM,
              "no metadata for program_id={:016X}, falling back to application_id={:016X}",
              program_id, application_id);
    return FileSys::PatchManager::GetMetadataFromBaseOrUpdate(system, application_id);
}

} // Anonymous namespace

IApplicationFunctions::IApplicationFunctions(Core::System& system_, std::shared_ptr<Applet> applet)
    : ServiceFramework{system_, "IApplicationFunctions"}, m_applet{std::move(applet)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {1, D<&IApplicationFunctions::PopLaunchParameter>, "PopLaunchParameter"},
        {10, nullptr, "CreateApplicationAndPushAndRequestToStart"},
        {11, nullptr, "CreateApplicationAndPushAndRequestToStartForQuest"},
        {12, D<&IApplicationFunctions::CreateApplicationAndRequestToStart>, "CreateApplicationAndRequestToStart"},
        {13, nullptr, "CreateApplicationAndRequestToStartForQuest"},
        {14, nullptr, "CreateApplicationWithAttributeAndPushAndRequestToStartForQuest"},
        {15, nullptr, "CreateApplicationWithAttributeAndRequestToStartForQuest"},
        {20, D<&IApplicationFunctions::EnsureSaveData>, "EnsureSaveData"},
        {21, D<&IApplicationFunctions::GetDesiredLanguage>, "GetDesiredLanguage"},
        {22, D<&IApplicationFunctions::SetTerminateResult>, "SetTerminateResult"},
        {23, D<&IApplicationFunctions::GetDisplayVersion>, "GetDisplayVersion"},
        {24, nullptr, "GetLaunchStorageInfoForDebug"},
        {25, D<&IApplicationFunctions::ExtendSaveData>, "ExtendSaveData"},
        {26, D<&IApplicationFunctions::GetSaveDataSize>, "GetSaveDataSize"},
        {27, D<&IApplicationFunctions::CreateCacheStorage>, "CreateCacheStorage"},
        {28, D<&IApplicationFunctions::GetSaveDataSizeMax>, "GetSaveDataSizeMax"},
        {29, D<&IApplicationFunctions::GetCacheStorageMax>, "GetCacheStorageMax"},
        {30, D<&IApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed>, "BeginBlockingHomeButtonShortAndLongPressed"},
        {31, D<&IApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed>, "EndBlockingHomeButtonShortAndLongPressed"},
        {32, D<&IApplicationFunctions::BeginBlockingHomeButton>, "BeginBlockingHomeButton"},
        {33, D<&IApplicationFunctions::EndBlockingHomeButton>, "EndBlockingHomeButton"},
        {34, nullptr, "SelectApplicationLicense"},
        {35, nullptr, "GetDeviceSaveDataSizeMax"},
        {36, nullptr, "GetLimitedApplicationLicense"},
        {37, nullptr, "GetLimitedApplicationLicenseUpgradableEvent"},
        {40, D<&IApplicationFunctions::NotifyRunning>, "NotifyRunning"},
        {50, D<&IApplicationFunctions::GetPseudoDeviceId>, "GetPseudoDeviceId"},
        {60, D<&IApplicationFunctions::SetMediaPlaybackStateForApplication>, "SetMediaPlaybackStateForApplication"},
        {65, D<&IApplicationFunctions::IsGamePlayRecordingSupported>, "IsGamePlayRecordingSupported"},
        {66, D<&IApplicationFunctions::InitializeGamePlayRecording>, "InitializeGamePlayRecording"},
        {67, D<&IApplicationFunctions::SetGamePlayRecordingState>, "SetGamePlayRecordingState"},
        {68, nullptr, "RequestFlushGamePlayingMovieForDebug"},
        {70, nullptr, "RequestToShutdown"},
        {71, nullptr, "RequestToReboot"},
        {72, nullptr, "RequestToSleep"},
        {80, nullptr, "ExitAndRequestToShowThanksMessage"},
        {90, D<&IApplicationFunctions::EnableApplicationCrashReport>, "EnableApplicationCrashReport"},
        {100, D<&IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer>, "InitializeApplicationCopyrightFrameBuffer"},
        {101, D<&IApplicationFunctions::SetApplicationCopyrightImage>, "SetApplicationCopyrightImage"},
        {102, D<&IApplicationFunctions::SetApplicationCopyrightVisibility>, "SetApplicationCopyrightVisibility"},
        {110, D<&IApplicationFunctions::QueryApplicationPlayStatistics>, "QueryApplicationPlayStatistics"},
        {111, D<&IApplicationFunctions::QueryApplicationPlayStatisticsByUid>, "QueryApplicationPlayStatisticsByUid"},
        {120, D<&IApplicationFunctions::ExecuteProgram>, "ExecuteProgram"},
        {121, D<&IApplicationFunctions::ClearUserChannel>, "ClearUserChannel"},
        {122, D<&IApplicationFunctions::UnpopToUserChannel>, "UnpopToUserChannel"},
        {123, D<&IApplicationFunctions::GetPreviousProgramIndex>, "GetPreviousProgramIndex"},
        {124, nullptr, "EnableApplicationAllThreadDumpOnCrash"},
        {130, D<&IApplicationFunctions::GetGpuErrorDetectedSystemEvent>, "GetGpuErrorDetectedSystemEvent"},
        {131, nullptr, "SetDelayTimeToAbortOnGpuError"},
        {140, D<&IApplicationFunctions::GetFriendInvitationStorageChannelEvent>, "GetFriendInvitationStorageChannelEvent"},
        {141, D<&IApplicationFunctions::TryPopFromFriendInvitationStorageChannel>, "TryPopFromFriendInvitationStorageChannel"},
        {150, D<&IApplicationFunctions::GetNotificationStorageChannelEvent>, "GetNotificationStorageChannelEvent"},
        {151, nullptr, "TryPopFromNotificationStorageChannel"},
        {160, D<&IApplicationFunctions::GetHealthWarningDisappearedSystemEvent>, "GetHealthWarningDisappearedSystemEvent"},
        {170, nullptr, "SetHdcpAuthenticationActivated"},
        {180, nullptr, "GetLaunchRequiredVersion"},
        {181, nullptr, "UpgradeLaunchRequiredVersion"},
        {190, nullptr, "SendServerMaintenanceOverlayNotification"},
        {200, nullptr, "GetLastApplicationExitReason"},
        {210, D<&IApplicationFunctions::GetUnknownEvent210>, "Unknown210"},
        {220, nullptr, "Unknown220"}, // [20.0.0+]
        {300, nullptr, "CreateMovieWriter"}, // [19.0.0+]
        {310, nullptr, "Unknown310"}, // [20.0.0+]
        {320, nullptr, "Unknown320"}, // [20.0.0+]
        {330, D<&IApplicationFunctions::Unknown330>, "Unknown330"}, // [20.0.0+]
        {500, nullptr, "StartContinuousRecordingFlushForDebug"},
        {1000, nullptr, "CreateMovieMaker"},
        {1001, D<&IApplicationFunctions::PrepareForJit>, "PrepareForJit"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationFunctions::~IApplicationFunctions() = default;

Result IApplicationFunctions::PopLaunchParameter(Out<SharedPointer<IStorage>> out_storage,
                                                 LaunchParameterKind launch_parameter_kind) {
    LOG_INFO(Service_AM, "called, kind={}", launch_parameter_kind);

    std::scoped_lock lk{m_applet->lock};

    auto& channel = launch_parameter_kind == LaunchParameterKind::UserChannel
                        ? m_applet->user_channel_launch_parameter
                        : m_applet->preselected_user_launch_parameter;

    if (channel.empty()) {
        LOG_WARNING(Service_AM, "Attempted to pop parameter {} but none was found!",
                    launch_parameter_kind);
        R_THROW(AM::ResultNoDataInChannel);
    }

    auto data = channel.back();
    channel.pop_back();

    *out_storage = std::make_shared<IStorage>(system, std::move(data));
    R_SUCCEED();
}

Result IApplicationFunctions::EnsureSaveData(Out<u64> out_size, Common::UUID user_id) {
    LOG_INFO(Service_AM, "called, uid={}", Common::Redact(user_id));

    FileSys::SaveDataAttribute attribute{};
    attribute.program_id = m_applet->program_id;
    attribute.user_id = user_id.AsU128();
    attribute.type = FileSys::SaveDataType::Account;

    FileSys::VirtualDir save_data{};
    R_TRY(system.GetFileSystemController().OpenSaveDataController()->CreateSaveData(
        &save_data, FileSys::SaveDataSpaceId::User, attribute));

    // [OpenPak] A console provisions the title's BCAT delivery cache here too. Splatoon 3 asks
    // for it at startup and, refused, retried every ~500 ms forever. Titles whose NACP declares
    // no delivery cache are unaffected.
    const auto metadata = GetApplicationMetadata(system, m_applet->program_id);
    if (metadata.first != nullptr && metadata.first->GetBCATDeliveryCacheStorageSize() > 0) {
        FileSys::SaveDataAttribute bcat_attribute{};
        bcat_attribute.program_id = m_applet->program_id;
        bcat_attribute.type = FileSys::SaveDataType::Bcat;

        FileSys::VirtualDir bcat_save_data{};
        system.GetFileSystemController().OpenSaveDataController()->CreateSaveData(
            &bcat_save_data, FileSys::SaveDataSpaceId::User, bcat_attribute);
    }

    *out_size = 0;
    R_SUCCEED();
}

Result IApplicationFunctions::GetDesiredLanguage(Out<u64> out_language_code) {
    // FIXME: all of this stuff belongs to ns
    // TODO(bunnei): This should be configurable
    LOG_DEBUG(Service_AM, "called");

    // Get supported languages from NACP, if possible
    // Default to 0 (all languages supported)
    u32 supported_languages = 0;

    const auto res = GetApplicationMetadata(system, m_applet->program_id);
    if (res.first != nullptr) {
        supported_languages = res.first->GetSupportedLanguages();
    }

    // Call IApplicationManagerInterface implementation.
    auto& service_manager = system.ServiceManager();
    auto ns_am2 = service_manager.GetService<NS::IServiceGetterInterface>("ns:am2");

    std::shared_ptr<NS::IApplicationManagerInterface> app_man;
    R_TRY(ns_am2->GetApplicationManagerInterface(&app_man));

    // Get desired application language
    NS::ApplicationLanguage desired_language{};
    R_TRY(app_man->GetApplicationDesiredLanguage(&desired_language, supported_languages));

    // Convert to settings language code.
    R_TRY(app_man->ConvertApplicationLanguageToLanguageCode(out_language_code, desired_language));

    LOG_DEBUG(Service_AM, "got desired_language={:016X}", *out_language_code);
    R_SUCCEED();
}

Result IApplicationFunctions::SetTerminateResult(Result terminate_result) {
    // Kept per applet: it is what the application's accessor reports when it ends.
    LOG_INFO(Service_AM, "called, result={:#x} ({:04}-{:04})",
             terminate_result.GetInnerValue(),
             static_cast<u32>(terminate_result.GetModule()) + 2000,
             terminate_result.GetDescription());

    std::scoped_lock lk{m_applet->lock};
    m_applet->terminate_result = terminate_result;

    R_SUCCEED();
}

Result IApplicationFunctions::GetDisplayVersion(Out<DisplayVersion> out_display_version) {
    LOG_DEBUG(Service_AM, "called");

    const auto res = GetApplicationMetadata(system, m_applet->program_id);
    if (res.first != nullptr) {
        const auto& version = res.first->GetVersionString();
        std::memcpy(out_display_version->string.data(), version.data(),
                    (std::min)(version.size(), out_display_version->string.size()));
    } else {
        static constexpr char default_version[]{"1.0.0"};
        std::memcpy(out_display_version->string.data(), default_version, sizeof(default_version));
    }

    out_display_version->string[out_display_version->string.size() - 1] = '\0';
    R_SUCCEED();
}

Result IApplicationFunctions::ExtendSaveData(Out<u64> out_required_size, FileSys::SaveDataType type,
                                             Common::UUID user_id, u64 normal_size,
                                             u64 journal_size) {
    LOG_DEBUG(Service_AM, "called with type={} user_id={} normal={:#x} journal={:#x}",
              static_cast<u8>(type), Common::Redact(user_id), normal_size, journal_size);

    system.GetFileSystemController().OpenSaveDataController()->WriteSaveDataSize(
        type, m_applet->program_id, user_id.AsU128(), {normal_size, journal_size});

    // The following value is used to indicate the amount of space remaining on failure
    // due to running out of space. Since we always succeed, this should be 0.
    *out_required_size = 0;

    R_SUCCEED();
}

Result IApplicationFunctions::GetSaveDataSize(Out<u64> out_normal_size, Out<u64> out_journal_size,
                                              FileSys::SaveDataType type, Common::UUID user_id) {
    LOG_DEBUG(Service_AM, "called with type={} user_id={}", type, Common::Redact(user_id));

    const auto size = system.GetFileSystemController().OpenSaveDataController()->ReadSaveDataSize(
        type, m_applet->program_id, user_id.AsU128());

    *out_normal_size = size.normal;
    *out_journal_size = size.journal;
    R_SUCCEED();
}

Result IApplicationFunctions::CreateCacheStorage(Out<u32> out_target_media,
                                                 Out<u64> out_required_size, u16 index,
                                                 u64 normal_size, u64 journal_size) {
    LOG_DEBUG(Service_AM, "called with index={} size={:#x} journal_size={:#x}", index,
              normal_size, journal_size);

    // Within the title's cache storage limits (its control property), as am checks before it
    // asks fs; the storage is made on the console's user storage.
    const auto nacp = ReadControlProperty(system, m_applet->program_id);
    if (nacp && (index > nacp->cache_storage_max_index ||
                 normal_size + journal_size > nacp->cache_storage_data_and_journal_max_size)) {
        LOG_UNKNOWN_CONTRACT(Service_AM,
                             "IApplicationFunctions 27 index {} size {:#x}+{:#x} beyond the "
                             "control property's limits (index {}, {:#x}); answered "
                             "InvalidParameters",
                             index, normal_size, journal_size, nacp->cache_storage_max_index,
                             nacp->cache_storage_data_and_journal_max_size);
        R_THROW(ResultInvalidParameters);
    }

    const auto attribute = FileSys::SaveDataAttribute::Make(
        FileSys::GetBaseTitleID(m_applet->program_id), FileSys::SaveDataType::Cache, {}, 0, index,
        FileSys::SaveDataRank::Primary);
    auto controller = system.GetFileSystemController().OpenSaveDataController();
    if (!controller->SaveDataExists(FileSys::SaveDataSpaceId::User, attribute)) {
        FileSys::VirtualDir dir;
        R_TRY(controller->CreateSaveData(&dir, FileSys::SaveDataSpaceId::User, attribute));
    }

    *out_target_media = 1; // CacheStorageTargetMedia::Nand
    *out_required_size = 0;
    R_SUCCEED();
}

Result IApplicationFunctions::GetSaveDataSizeMax(Out<u64> out_max_normal_size,
                                                 Out<u64> out_max_journal_size) {
    // The account save data's maximum from the control property, never below its initial size.
    const auto nacp = ReadControlProperty(system, m_applet->program_id);
    if (nacp == nullptr) {
        *out_max_normal_size = 0;
        *out_max_journal_size = 0;
        LOG_WARNING(Service_AM, "no control property for {:016X}", m_applet->program_id);
        R_SUCCEED();
    }
    *out_max_normal_size = (std::max)(nacp->user_account_save_data_max_size,
                                      nacp->user_account_save_data_size);
    *out_max_journal_size = (std::max)(nacp->user_account_save_data_max_journal_size,
                                       nacp->user_account_save_data_journal_size);
    LOG_DEBUG(Service_AM, "called, max={:#x} journal={:#x}", *out_max_normal_size,
              *out_max_journal_size);
    R_SUCCEED();
}

Result IApplicationFunctions::GetCacheStorageMax(Out<u32> out_cache_storage_index_max,
                                                 Out<u64> out_max_journal_size) {
    LOG_DEBUG(Service_AM, "called");

    std::vector<u8> nacp;
    R_TRY(system.GetARPManager().GetControlProperty(&nacp, m_applet->program_id));

    auto raw_nacp = std::make_unique<FileSys::RawNACP>();
    std::memcpy(raw_nacp.get(), nacp.data(), (std::min)(sizeof(*raw_nacp), nacp.size()));

    *out_cache_storage_index_max = static_cast<u32>(raw_nacp->cache_storage_max_index);
    *out_max_journal_size = static_cast<u64>(raw_nacp->cache_storage_data_and_journal_max_size);

    R_SUCCEED();
}

Result IApplicationFunctions::BeginBlockingHomeButtonShortAndLongPressed(s64 unused) {
    // The window system holds HOME's short and long presses back from this application.
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_long_pressed_blocked = true;
    m_applet->home_button_short_pressed_blocked = true;

    R_SUCCEED();
}

Result IApplicationFunctions::EndBlockingHomeButtonShortAndLongPressed() {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_long_pressed_blocked = false;
    m_applet->home_button_short_pressed_blocked = false;

    R_SUCCEED();
}

Result IApplicationFunctions::BeginBlockingHomeButton(s64 timeout_ns) {
    // Short presses only, as am does (it sets the short flag and keeps the time); the block lasts
    // until EndBlockingHomeButton. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx
    // base). What the time changes on a console is not established.
    LOG_UNKNOWN_CONTRACT(Service_AM,
                         "IApplicationFunctions 32 BeginBlockingHomeButton time {} ns recorded, "
                         "the block lasts until 33",
                         timeout_ns);

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_short_pressed_blocked = true;

    R_SUCCEED();
}

Result IApplicationFunctions::EndBlockingHomeButton() {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->home_button_short_pressed_blocked = false;

    R_SUCCEED();
}

Result IApplicationFunctions::NotifyRunning(Out<bool> out_became_running) {
    // The application is running from its first call on.
    LOG_DEBUG(Service_AM, "called");
    *out_became_running = true;
    R_SUCCEED();
}

Result IApplicationFunctions::GetPseudoDeviceId(Out<Common::UUID> out_pseudo_device_id) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "IApplicationFunctions 50 GetPseudoDeviceId: derived from the title seed alone; a console also mixes in a per-console secret");
    R_UNLESS(out_pseudo_device_id != nullptr, ResultUnknown);

    // This should be hashed with the device specific hash
    // for now this will do
    const auto res = FileSys::PatchManager::GetMetadataFromBaseOrUpdate(system, m_applet->program_id);
    R_UNLESS(res.first != nullptr, ResultUnknown);
    std::array<u8, EVP_MAX_MD_SIZE> hash;
    unsigned int hash_len = 0;
    auto const seed = res.first->raw.seed_for_pseudo_device_id;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    auto const algorithm = EVP_sha1();
    EVP_DigestInit_ex(ctx, algorithm, nullptr);
    EVP_DigestUpdate(ctx, &seed, sizeof(seed));
    EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
    EVP_MD_CTX_free(ctx);
    *out_pseudo_device_id = Common::UUID::MakeRFC4122V5(std::span<u8, 16>{hash.begin(), hash.begin() + 16});
    R_SUCCEED();
}

Result IApplicationFunctions::IsGamePlayRecordingSupported(
    Out<bool> out_is_game_play_recording_supported) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "IApplicationFunctions 65 IsGamePlayRecordingSupported: not emulated: gameplay recording; answered false");
    *out_is_game_play_recording_supported = false;
    R_SUCCEED();
}

Result IApplicationFunctions::InitializeGamePlayRecording(
    u64 transfer_memory_size, InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "IApplicationFunctions 66 InitializeGamePlayRecording: not emulated: gameplay recording; the memory is accepted and unused");
    R_SUCCEED();
}

Result IApplicationFunctions::SetMediaPlaybackStateForApplication(bool enabled) {
    // Kept per applet (it holds the idle timer back on a console).
    LOG_DEBUG(Service_AM, "called, enabled={}", enabled);
    std::scoped_lock lk{m_applet->lock};
    m_applet->media_playback_state = enabled;
    R_SUCCEED();
}

Result IApplicationFunctions::SetGamePlayRecordingState(
    GamePlayRecordingState game_play_recording_state) {
    // Kept per applet; nothing is recorded here.
    LOG_DEBUG(Service_AM, "called, state={}", game_play_recording_state);

    std::scoped_lock lk{m_applet->lock};
    m_applet->game_play_recording_state = game_play_recording_state;

    R_SUCCEED();
}

Result IApplicationFunctions::EnableApplicationCrashReport(bool enabled) {
    // Kept per applet.
    LOG_DEBUG(Service_AM, "called, enabled={}", enabled);

    std::scoped_lock lk{m_applet->lock};
    m_applet->application_crash_report_enabled = enabled;

    R_SUCCEED();
}

Result IApplicationFunctions::InitializeApplicationCopyrightFrameBuffer(
    s32 width, s32 height, u64 transfer_memory_size,
    InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "IApplicationFunctions 100 InitializeApplicationCopyrightFrameBuffer: not emulated: the copyright overlay for captures; accepted");
    R_SUCCEED();
}

Result IApplicationFunctions::SetApplicationCopyrightImage(
    s32 x, s32 y, s32 width, s32 height, WindowOriginMode window_origin_mode,
    InBuffer<BufferAttr_HipcMapTransferAllowsNonSecure | BufferAttr_HipcMapAlias> image_data) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "IApplicationFunctions 101 SetApplicationCopyrightImage: not emulated: the copyright overlay for captures; image dropped");
    R_SUCCEED();
}

Result IApplicationFunctions::SetApplicationCopyrightVisibility(bool visible) {
    LOG_UNKNOWN_CONTRACT(Service_AM, "IApplicationFunctions 102 SetApplicationCopyrightVisibility: not emulated: the copyright overlay for captures; visible={}", visible);
    R_SUCCEED();
}

Result IApplicationFunctions::QueryApplicationPlayStatistics(
    Out<s32> out_entries,
    OutArray<ApplicationPlayStatistics, BufferAttr_HipcMapAlias> out_play_statistics,
    InArray<u64, BufferAttr_HipcMapAlias> application_ids) {
    *out_entries = 0;
    R_RETURN(QueryPlayStatistics(system, m_applet->program_id, *out_entries,
                                 {out_play_statistics.data(), out_play_statistics.size()},
                                 {application_ids.data(), application_ids.size()}));
}

Result IApplicationFunctions::QueryApplicationPlayStatisticsByUid(
    Out<s32> out_entries,
    OutArray<ApplicationPlayStatistics, BufferAttr_HipcMapAlias> out_play_statistics,
    Common::UUID user_id, InArray<u64, BufferAttr_HipcMapAlias> application_ids) {
    *out_entries = 0;
    R_UNLESS(system.GetProfileManager().UserExists(user_id), ResultPdmUserNotFound);
    // Play time is kept per console here, not per user: every user sees the same totals.
    R_RETURN(QueryPlayStatistics(system, m_applet->program_id, *out_entries,
                                 {out_play_statistics.data(), out_play_statistics.size()},
                                 {application_ids.data(), application_ids.size()}));
}

Result IApplicationFunctions::ExecuteProgram(ProgramSpecifyKind kind, u64 value) {
    LOG_INFO(Service_AM, "called, kind={}, value={}", kind, value);
    R_UNLESS(kind == ProgramSpecifyKind::ExecuteProgram ||
                 kind == ProgramSpecifyKind::RestartProgram,
             ResultInvalidParameters);

    if (system.GetAppletManager().IsHomeSession()) {
        R_RETURN(ExecuteProgramInHomeSession(kind, value));
    }

    // Copy user channel ownership into the system so that it will be preserved
    system.GetUserChannel() = m_applet->user_channel_launch_parameter;
    system.ExecuteProgram(value);
    R_SUCCEED();
}

// In a HOME session the application's slot stays and HOME is not told the application ended: its
// process ends and the window system loads the other program of the same application in its place
// (the winding slot's relaunch), with the user channel kept and GetPreviousProgramIndex answering
// the index it came from.
Result IApplicationFunctions::ExecuteProgramInHomeSession(ProgramSpecifyKind kind, u64 value) {
    std::unique_lock lk{m_applet->lock};
    const u64 current_id = m_applet->program_id;
    const u64 base_id = FileSys::GetBaseTitleID(current_id);
    const u64 current_index = current_id - base_id;
    const u64 next_index = kind == ProgramSpecifyKind::RestartProgram ? current_index : value;
    const u64 next_id = FileSys::GetBaseTitleIDWithProgramIndex(current_id, next_index);

    R_UNLESS(next_index < 0x10, ResultInvalidParameters);
    R_UNLESS(system.GetContentProviderUnion().GetEntryRaw(next_id,
                                                          FileSys::ContentRecordType::Program) !=
                 nullptr,
             ResultApplicationRecordNotFound);

    LOG_INFO(Service_AM, "am: application pid {} executes program {} ({:016x})",
             m_applet->aruid.pid, next_index, next_id);

    m_applet->previous_program_index = static_cast<s32>(current_index);
    m_applet->program_id = next_id;
    m_applet->is_winding = true;
    m_applet->unwind_after_reserved = true;
    m_applet->process_exiting = true;
    m_applet->display_layer_manager.SetWindowVisibility(false);
    lk.unlock();

    m_applet->process->Terminate();
    R_SUCCEED();
}

// https://switchbrew.org/wiki/Applet_Manager_services#CreateApplicationAndRequestToStart
Result IApplicationFunctions::CreateApplicationAndRequestToStart(u64 application_id) {
    LOG_INFO(Service_AM, "called, application_id={:016X} current_program_id={:016X}", application_id, m_applet->program_id);

    if (application_id == 0 ||
        FileSys::GetBaseTitleID(application_id) == FileSys::GetBaseTitleID(m_applet->program_id)) {
        const auto program_index = application_id == 0
                                       ? 0
                                       : application_id - FileSys::GetBaseTitleID(application_id);

        system.GetUserChannel() = m_applet->user_channel_launch_parameter;
        system.ExecuteProgram(program_index);
        R_SUCCEED();
    }

    LOG_ERROR(Service_AM, "Launching a different application ({:016X}) is not implemented!", application_id);
    R_THROW(ResultUnknown);
}

Result IApplicationFunctions::ClearUserChannel() {
    LOG_DEBUG(Service_AM, "called");
    m_applet->user_channel_launch_parameter.clear();
    R_SUCCEED();
}

Result IApplicationFunctions::UnpopToUserChannel(SharedPointer<IStorage> storage) {
    LOG_DEBUG(Service_AM, "called");
    m_applet->user_channel_launch_parameter.push_back(storage->GetData());
    R_SUCCEED();
}

Result IApplicationFunctions::GetPreviousProgramIndex(Out<s32> out_previous_program_index) {
    std::scoped_lock lk{m_applet->lock};
    *out_previous_program_index = m_applet->previous_program_index;
    LOG_DEBUG(Service_AM, "called, previous_program_index={}", *out_previous_program_index);
    R_SUCCEED();
}

Result IApplicationFunctions::GetGpuErrorDetectedSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    // Never signalled: no GPU error detection here.
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->gpu_error_detected_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::GetFriendInvitationStorageChannelEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->friend_invitation_storage_channel_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::TryPopFromFriendInvitationStorageChannel(
    Out<SharedPointer<IStorage>> out_storage) {
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};

    auto& channel = m_applet->friend_invitation_storage_channel;

    if (channel.empty()) {
        return AM::ResultNoDataInChannel;
    }

    auto data = channel.back();
    channel.pop_back();
    if (channel.empty()) {
        m_applet->friend_invitation_storage_channel_event.Clear(system.Kernel());
    }

    *out_storage = std::make_shared<IStorage>(system, std::move(data));
    R_SUCCEED();
}

Result IApplicationFunctions::GetNotificationStorageChannelEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->notification_storage_channel_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::GetHealthWarningDisappearedSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->health_warning_disappeared_system_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::GetUnknownEvent210(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_applet->unknown_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationFunctions::Unknown330(Out<u8> out) {
    LOG_DEBUG(Service_AM, "called");
    *out = 0;
    R_SUCCEED();
}

Result IApplicationFunctions::PrepareForJit() {
    // Kept per applet; the jit service needs nothing more.
    LOG_DEBUG(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    m_applet->jit_service_launched = true;

    R_SUCCEED();
}

} // namespace Service::AM
