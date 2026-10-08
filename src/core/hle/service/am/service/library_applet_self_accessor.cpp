// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "common/redact.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/service/library_applet_self_accessor.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/erpt/erpt.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/ns/application_manager_interface.h"
#include "core/hle/service/ns/service_getter_interface.h"
#include "core/hle/service/set/system_settings_server.h"
#include "core/hle/service/sm/sm.h"

namespace Service::AM {

namespace {

AppletIdentityInfo GetCallerIdentity(Applet& applet) {
    if (const auto caller_applet = applet.caller_applet.lock(); caller_applet) {
        // TODO: is this actually the application ID?
        return {
            .applet_id = caller_applet->applet_id,
            .application_id = caller_applet->program_id,
        };
    } else {
        return {
            .applet_id = AppletId::QLaunch,
            .application_id = 0x0100000000001000ull,
        };
    }
}

// The root of the caller chain: the application (or system applet) that started the first library
// applet. myPage builds an invitation's application id from it. Behaviour follows the Ryujinx
// Harbor (MIT-licensed Ryujinx base).
AppletIdentityInfo GetMainIdentity(Applet& applet) {
    std::shared_ptr<Applet> main = applet.caller_applet.lock();
    if (!main) {
        return GetCallerIdentity(applet);
    }
    while (const auto caller = main->caller_applet.lock()) {
        main = caller;
    }
    return {
        .applet_id = main->applet_id,
        .application_id = main->program_id,
    };
}

} // namespace

ILibraryAppletSelfAccessor::ILibraryAppletSelfAccessor(Core::System& system_,
                                                       std::shared_ptr<Applet> applet)
    : ServiceFramework{system_, "ILibraryAppletSelfAccessor"}, m_applet{std::move(applet)},
      m_broker{m_applet->caller_applet_broker} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&ILibraryAppletSelfAccessor::PopInData>, "PopInData"},
        {1, D<&ILibraryAppletSelfAccessor::PushOutData>, "PushOutData"},
        {2, D<&ILibraryAppletSelfAccessor::PopInteractiveInData>, "PopInteractiveInData"},
        {3, D<&ILibraryAppletSelfAccessor::PushInteractiveOutData>, "PushInteractiveOutData"},
        {5, D<&ILibraryAppletSelfAccessor::GetPopInDataEvent>, "GetPopInDataEvent"},
        {6, D<&ILibraryAppletSelfAccessor::GetPopInteractiveInDataEvent>, "GetPopInteractiveInDataEvent"},
        {10, D<&ILibraryAppletSelfAccessor::ExitProcessAndReturn>, "ExitProcessAndReturn"},
        {11, D<&ILibraryAppletSelfAccessor::GetLibraryAppletInfo>, "GetLibraryAppletInfo"},
        {12, D<&ILibraryAppletSelfAccessor::GetMainAppletIdentityInfo>, "GetMainAppletIdentityInfo"},
        {13, D<&ILibraryAppletSelfAccessor::CanUseApplicationCore>, "CanUseApplicationCore"},
        {14, D<&ILibraryAppletSelfAccessor::GetCallerAppletIdentityInfo>, "GetCallerAppletIdentityInfo"},
        {15, D<&ILibraryAppletSelfAccessor::GetMainAppletApplicationControlProperty>, "GetMainAppletApplicationControlProperty"},
        {16, D<&ILibraryAppletSelfAccessor::GetMainAppletStorageId>, "GetMainAppletStorageId"},
        {17, D<&ILibraryAppletSelfAccessor::GetCallerAppletIdentityInfoStack>, "GetCallerAppletIdentityInfoStack"},
        {18, D<&ILibraryAppletSelfAccessor::GetNextReturnDestinationAppletIdentityInfo>, "GetNextReturnDestinationAppletIdentityInfo"},
        {19, D<&ILibraryAppletSelfAccessor::GetDesirableKeyboardLayout>, "GetDesirableKeyboardLayout"},
        {20, D<&ILibraryAppletSelfAccessor::PopExtraStorage>, "PopExtraStorage"},
        {25, D<&ILibraryAppletSelfAccessor::GetPopExtraStorageEvent>, "GetPopExtraStorageEvent"},
        {30, D<&ILibraryAppletSelfAccessor::UnpopInData>, "UnpopInData"},
        {31, D<&ILibraryAppletSelfAccessor::UnpopExtraStorage>, "UnpopExtraStorage"},
        {40, D<&ILibraryAppletSelfAccessor::GetIndirectLayerProducerHandle>, "GetIndirectLayerProducerHandle"},
        {50, D<&ILibraryAppletSelfAccessor::ReportVisibleError>, "ReportVisibleError"},
        {51, D<&ILibraryAppletSelfAccessor::ReportVisibleErrorWithErrorContext>, "ReportVisibleErrorWithErrorContext"},
        {60, D<&ILibraryAppletSelfAccessor::GetMainAppletApplicationDesiredLanguage>, "GetMainAppletApplicationDesiredLanguage"},
        {70, D<&ILibraryAppletSelfAccessor::GetCurrentApplicationId>, "GetCurrentApplicationId"},
        {80, D<&ILibraryAppletSelfAccessor::RequestExitToSelf>, "RequestExitToSelf"},
        {90, D<&ILibraryAppletSelfAccessor::CreateApplicationAndPushAndRequestToLaunch>, "CreateApplicationAndPushAndRequestToLaunch"},
        {100, D<&ILibraryAppletSelfAccessor::CreateGameMovieTrimmer>, "CreateGameMovieTrimmer"},
        {101, D<&ILibraryAppletSelfAccessor::ReserveResourceForMovieOperation>, "ReserveResourceForMovieOperation"},
        {102, D<&ILibraryAppletSelfAccessor::UnreserveResourceForMovieOperation>, "UnreserveResourceForMovieOperation"},
        {110, D<&ILibraryAppletSelfAccessor::GetMainAppletAvailableUsers>, "GetMainAppletAvailableUsers"},
        {120, nullptr, "GetLaunchStorageInfoForDebug"},
        {130, D<&ILibraryAppletSelfAccessor::GetGpuErrorDetectedSystemEvent>, "GetGpuErrorDetectedSystemEvent"},
        {140, nullptr, "SetApplicationMemoryReservation"},
        {150, D<&ILibraryAppletSelfAccessor::ShouldSetGpuTimeSliceManually>, "ShouldSetGpuTimeSliceManually"},
        {160, D<&ILibraryAppletSelfAccessor::GetLibraryAppletInfoEx>, "GetLibraryAppletInfoEx"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

ILibraryAppletSelfAccessor::~ILibraryAppletSelfAccessor() = default;

Result ILibraryAppletSelfAccessor::PopInData(Out<SharedPointer<IStorage>> out_storage) {
    LOG_INFO(Service_AM, "called");
    R_RETURN(m_broker->GetInData().Pop(system.Kernel(), out_storage));
}

Result ILibraryAppletSelfAccessor::PushOutData(SharedPointer<IStorage> storage) {
    LOG_INFO(Service_AM, "called");
    m_broker->GetOutData().Push(system.Kernel(), storage);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::PopInteractiveInData(Out<SharedPointer<IStorage>> out_storage) {
    LOG_INFO(Service_AM, "called");
    R_RETURN(m_broker->GetInteractiveInData().Pop(system.Kernel(), out_storage));
}

Result ILibraryAppletSelfAccessor::PushInteractiveOutData(SharedPointer<IStorage> storage) {
    LOG_INFO(Service_AM, "called");
    m_broker->GetInteractiveOutData().Push(system.Kernel(), storage);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetPopInDataEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_AM, "called");
    *out_event = m_broker->GetInData().GetEvent();
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetPopInteractiveInDataEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_AM, "called");
    *out_event = m_broker->GetInteractiveInData().GetEvent();
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetLibraryAppletInfo(
    Out<LibraryAppletInfo> out_library_applet_info) {
    LOG_INFO(Service_AM, "called");
    *out_library_applet_info = {
        .applet_id = m_applet->applet_id,
        .library_applet_mode = m_applet->library_applet_mode,
    };
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetMainAppletIdentityInfo(
    Out<AppletIdentityInfo> out_identity_info) {
    *out_identity_info = GetMainIdentity(*m_applet);
    LOG_INFO(Service_AM, "called, applet_id={} program_id={:016X}",
             static_cast<u32>(out_identity_info->applet_id), out_identity_info->application_id);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::CanUseApplicationCore(Out<bool> out_can_use_application_core) {
    // TODO: This appears to read the NPDM from state and check the core mask of the applet.
    LOG_UNKNOWN_CONTRACT(Service_AM, "ILibraryAppletSelfAccessor 13 CanUseApplicationCore: answered false (the NPDM core mask is not read)");
    *out_can_use_application_core = false;
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetMainAppletApplicationControlProperty(
    OutLargeData<std::array<u8, 0x4000>, BufferAttr_HipcMapAlias> out_nacp) {
    LOG_DEBUG(Service_AM, "called");

    const auto application = GetMainIdentity(*m_applet);
    std::vector<u8> nacp;
    const auto result =
        system.GetARPManager().GetControlProperty(&nacp, application.application_id);

    if (R_SUCCEEDED(result)) {
        std::memcpy(out_nacp->data(), nacp.data(), (std::min)(nacp.size(), out_nacp->size()));
    }

    R_RETURN(result);
}

Result ILibraryAppletSelfAccessor::GetMainAppletStorageId(Out<FileSys::StorageId> out_storage_id) {
    // Where the main applet's program is installed. Titles from the host's game folders are
    // answered as user NAND, as the Ryujinx Harbor answers applications (MIT-licensed Ryujinx base).
    const auto main = GetMainIdentity(*m_applet);
    const auto slot = system.GetContentProviderUnion().GetSlotForEntry(
        main.application_id, FileSys::ContentRecordType::Program);
    FileSys::StorageId storage_id = FileSys::StorageId::NandUser;
    if (slot == FileSys::ContentProviderUnionSlot::SysNAND) {
        storage_id = FileSys::StorageId::NandSystem;
    } else if (slot == FileSys::ContentProviderUnionSlot::SDMC) {
        storage_id = FileSys::StorageId::SdCard;
    }
    *out_storage_id = storage_id;
    LOG_DEBUG(Service_AM, "called, program_id={:016X} storage_id={}", main.application_id,
              static_cast<u32>(storage_id));
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::ExitProcessAndReturn() {
    LOG_INFO(Service_AM, "called");

    if (const auto caller_applet = m_applet->caller_applet.lock(); caller_applet) {
        {
            std::scoped_lock lk{m_applet->lock};
            m_applet->process_exiting = true;
        }
        m_applet->process->Terminate();
    } else {
        system.GetUserChannel() = m_applet->user_channel_launch_parameter;
        system.ExecuteProgram(0);
    }

    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetCallerAppletIdentityInfo(
    Out<AppletIdentityInfo> out_identity_info) {
    LOG_INFO(Service_AM, "called");
    *out_identity_info = GetCallerIdentity(*m_applet);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetCallerAppletIdentityInfoStack(
    Out<s32> out_count, OutArray<AppletIdentityInfo, BufferAttr_HipcMapAlias> out_identity_info) {
    LOG_INFO(Service_AM, "called");

    std::shared_ptr<Applet> applet = m_applet;
    *out_count = 0;

    do {
        if (*out_count >= static_cast<s32>(out_identity_info.size())) {
            break;
        }
        out_identity_info[(*out_count)++] = GetCallerIdentity(*applet);
    } while ((applet = applet->caller_applet.lock()));

    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetDesirableKeyboardLayout(Out<u32> out_desirable_layout) {
    // The layout the caller set with ISelfController 20.
    if (const auto caller = m_applet->caller_applet.lock(); caller) {
        std::scoped_lock lk{caller->lock};
        if (caller->desirable_keyboard_layout) {
            *out_desirable_layout = *caller->desirable_keyboard_layout;
            LOG_INFO(Service_AM, "called, the caller's layout {}", *out_desirable_layout);
            R_SUCCEED();
        }
    }

    // A console answers an error when the caller set none (which one is not documented) and the
    // applet then picks its own default. Answering 0 (Japanese) made the web applets' inline
    // keyboard type Shift+2 as '"'; the system keyboard layout is what the user chose.
    Set::KeyboardLayout layout{};
    const auto set_sys =
        system.ServiceManager().GetService<Set::ISystemSettingsServer>("set:sys", true);
    if (set_sys) {
        (void)set_sys->GetKeyboardLayout(Out<Set::KeyboardLayout>{&layout});
    }
    *out_desirable_layout = static_cast<u32>(layout);

    // The software keyboard asks on every key press: warn once.
    static std::atomic_bool warned{};
    if (!warned.exchange(true)) {
        LOG_WARNING(Service_AM,
                    "unknown contract: ILibraryAppletSelfAccessor 19 with no layout from the "
                    "caller; returning the system keyboard layout {}",
                    *out_desirable_layout);
    }
    R_SUCCEED();
}

// An applet showed the user an error: am files a Visible erpt report for it (firmware 23.0.1 am
// 0x7314c, through erpt:c 5): ErrorInfo with the code and AbortFlag/CreateProcessFailureFlag
// false, and the main applet's ApplicationInfo, so Settings > Support > Error History lists it.
// A code outside 2000-2999/0-9999 is filed as 2999-9999 with the raw pair in InvalidErrorCode,
// as the firmware formats codes. The error context's fields are not parsed into the report.
static void FileVisibleError(Core::System& system, Applet& applet, ErrorCode error_code) {
    ERPT::Context error_info;
    error_info.category = ERPT::Category_ErrorInfo;
    error_info.Set(116, false); // AbortFlag
    error_info.Set(152, false); // CreateProcessFailureFlag
    std::string code;
    if (error_code.category >= 2000 && error_code.category < 3000 && error_code.number < 10000) {
        code = fmt::format("{:04}-{:04}", error_code.category, error_code.number);
    } else {
        code = "2999-9999";
        error_info.Set(681, fmt::format("{}-{}", error_code.category, error_code.number));
    }
    const auto main = GetMainIdentity(applet);
    const std::vector<ERPT::Context> additional{ERPT::MakeApplicationInfo(
        system, main.application_id, static_cast<s32>(main.applet_id))};
    LOG_WARNING(Service_AM, "am: applet {} reports visible error {}",
                static_cast<u32>(applet.applet_id), code);
    ERPT::CreateHostReport(system, ERPT::ReportType_Visible, code, &error_info, additional);
}

Result ILibraryAppletSelfAccessor::ReportVisibleError(ErrorCode error_code) {
    FileVisibleError(system, *m_applet, error_code);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::ReportVisibleErrorWithErrorContext(
    ErrorCode error_code, InLargeData<ErrorContext, BufferAttr_HipcMapAlias> error_context) {
    FileVisibleError(system, *m_applet, error_code);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::UnpopInData(SharedPointer<IStorage> storage) {
    LOG_INFO(Service_AM, "called");
    m_broker->GetInData().Unpop(system.Kernel(), storage);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetMainAppletApplicationDesiredLanguage(
    Out<u64> out_desired_language) {
    // FIXME: this is copied from IApplicationFunctions::GetDesiredLanguage
    // FIXME: all of this stuff belongs to ns
    auto identity = GetCallerIdentity(*m_applet);

    // TODO(bunnei): This should be configurable
    LOG_DEBUG(Service_AM, "called");

    // Get supported languages from NACP, if possible
    // Default to 0 (all languages supported)
    u32 supported_languages = 0;

    const auto res = FileSys::PatchManager::GetMetadataFromBaseOrUpdate(system, identity.application_id);

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
    u64 language_code{};
    R_TRY(app_man->ConvertApplicationLanguageToLanguageCode(&language_code, desired_language));

    LOG_DEBUG(Service_AM, "got desired_language={:016X}", language_code);

    *out_desired_language = language_code;
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetCurrentApplicationId(Out<u64> out_application_id) {
    // The running application's id, 0 when none runs (switchbrew; the Ryujinx Harbor answers
    // the same).
    const u64 program_id = system.GetApplicationProcessProgramID();
    *out_application_id = program_id == 0 ? 0 : FileSys::GetBaseTitleID(program_id);
    LOG_DEBUG(Service_AM, "called, application_id={:016X}", *out_application_id);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetMainAppletAvailableUsers(
    Out<bool> out_can_select_any_user, Out<s32> out_users_count,
    OutArray<Common::UUID, BufferAttr_HipcMapAlias> out_users) {
    const auto& manager = system.GetProfileManager();

    *out_can_select_any_user = false;
    *out_users_count = -1;

    LOG_INFO(Service_AM, "called");

    if (manager.GetUserCount() > 0) {
        *out_can_select_any_user = true;
        *out_users_count = static_cast<s32>(manager.GetUserCount());

        const auto users = manager.GetAllUsers();
        for (size_t i = 0; i < users.size() && i < out_users.size(); i++) {
            out_users[i] = users[i];
        }
    }

    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::ShouldSetGpuTimeSliceManually(
    Out<bool> out_should_set_gpu_time_slice_manually) {
    // False: this system has no GPU time slicing.
    LOG_DEBUG(Service_AM, "called");
    *out_should_set_gpu_time_slice_manually = false;
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetLibraryAppletInfoEx(
    Out<LibraryAppletInfo> out_library_applet_info) {
    LOG_INFO(Service_AM, "called");
    *out_library_applet_info = {
        .applet_id = m_applet->applet_id,
        .library_applet_mode = m_applet->library_applet_mode,
    };
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetNextReturnDestinationAppletIdentityInfo(
    Out<AppletIdentityInfo> out_identity_info) {
    LOG_WARNING(Service_AM, "unknown contract: ILibraryAppletSelfAccessor 18 answered with the "
                            "caller");
    *out_identity_info = GetCallerIdentity(*m_applet);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::PopExtraStorage(Out<SharedPointer<IStorage>> out_storage) {
    LOG_DEBUG(Service_AM, "called");
    R_RETURN(m_broker->GetExtraStorage().Pop(system.Kernel(), out_storage.Get()));
}

Result ILibraryAppletSelfAccessor::GetPopExtraStorageEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_DEBUG(Service_AM, "called");
    *out_event = m_broker->GetExtraStorage().GetEvent();
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::UnpopExtraStorage(SharedPointer<IStorage> storage) {
    LOG_DEBUG(Service_AM, "called");
    m_broker->GetExtraStorage().Unpop(system.Kernel(), storage);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetIndirectLayerProducerHandle(Out<u64> out_handle) {
    LOG_WARNING(Service_AM, "unknown contract: ILibraryAppletSelfAccessor 40 -> 0 (no indirect "
                            "layer for applet processes)");
    *out_handle = 0;
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::RequestExitToSelf() {
    LOG_INFO(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    m_applet->lifecycle_manager.RequestExit(system.Kernel());
    m_applet->UpdateSuspensionStateLocked(true);
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::CreateApplicationAndPushAndRequestToLaunch(
    u64 application_id, SharedPointer<IStorage> storage) {
    LOG_WARNING(Service_AM, "unknown contract: ILibraryAppletSelfAccessor 90 ({:016x}) -> "
                            "NotImplemented",
                application_id);
    R_THROW(ResultNotImplemented);
}

Result ILibraryAppletSelfAccessor::CreateGameMovieTrimmer() {
    LOG_WARNING(Service_AM, "unknown contract: ILibraryAppletSelfAccessor 100 -> NotImplemented "
                            "(no grc)");
    R_THROW(ResultNotImplemented);
}

Result ILibraryAppletSelfAccessor::ReserveResourceForMovieOperation() {
    LOG_DEBUG(Service_AM, "called");
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::UnreserveResourceForMovieOperation() {
    LOG_DEBUG(Service_AM, "called");
    R_SUCCEED();
}

Result ILibraryAppletSelfAccessor::GetGpuErrorDetectedSystemEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_WARNING(Service_AM, "unknown contract: ILibraryAppletSelfAccessor 130, an event that "
                            "never signals");
    *out_event = m_applet->gpu_error_detected_event.GetHandle();
    R_SUCCEED();
}

} // namespace Service::AM
