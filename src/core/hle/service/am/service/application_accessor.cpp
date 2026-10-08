// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "core/file_sys/common_funcs.h"
#include "core/core.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/application_accessor.h"
#include "core/hle/service/am/service/library_applet_accessor.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/glue/glue_manager.h"

namespace Service::AM {

IApplicationAccessor::IApplicationAccessor(Core::System& system_, std::shared_ptr<Applet> applet,
                                           WindowSystem& window_system)
    : ServiceFramework{system_, "IApplicationAccessor"}, m_window_system(window_system),
      m_applet(std::move(applet)) {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IApplicationAccessor::GetAppletStateChangedEvent>, "GetAppletStateChangedEvent"},
        {1, D<&IApplicationAccessor::IsCompleted>, "IsCompleted"},
        {10, D<&IApplicationAccessor::Start>, "Start"},
        {20, D<&IApplicationAccessor::RequestExit>, "RequestExit"},
        {25, D<&IApplicationAccessor::Terminate>, "Terminate"},
        {30, D<&IApplicationAccessor::GetResult>, "GetResult"},
        {101, D<&IApplicationAccessor::RequestForApplicationToGetForeground>, "RequestForApplicationToGetForeground"},
        {110, D<&IApplicationAccessor::TerminateAllLibraryApplets>, "TerminateAllLibraryApplets"},
        {111, D<&IApplicationAccessor::AreAnyLibraryAppletsLeft>, "AreAnyLibraryAppletsLeft"},
        {112, D<&IApplicationAccessor::GetCurrentLibraryApplet>, "GetCurrentLibraryApplet"},
        {120, D<&IApplicationAccessor::GetApplicationId>, "GetApplicationId"},
        {121, D<&IApplicationAccessor::PushLaunchParameter>, "PushLaunchParameter"},
        {122, D<&IApplicationAccessor::GetApplicationControlProperty>, "GetApplicationControlProperty"},
        {123, D<&IApplicationAccessor::GetApplicationLaunchProperty>, "GetApplicationLaunchProperty"},
        {124, D<&IApplicationAccessor::GetApplicationLaunchRequestInfo>, "GetApplicationLaunchRequestInfo"},
        {130, D<&IApplicationAccessor::SetUsers>, "SetUsers"},
        {131, D<&IApplicationAccessor::CheckRightsEnvironmentAvailable>, "CheckRightsEnvironmentAvailable"},
        {132, D<&IApplicationAccessor::GetNsRightsEnvironmentHandle>, "GetNsRightsEnvironmentHandle"},
        {140, D<&IApplicationAccessor::GetDesirableUids>, "GetDesirableUids"},
        {150, D<&IApplicationAccessor::ReportApplicationExitTimeout>, "ReportApplicationExitTimeout"},
        {160, D<&IApplicationAccessor::SetApplicationAttribute>, "SetApplicationAttribute"},
        {170, D<&IApplicationAccessor::HasSaveDataAccessPermission>, "HasSaveDataAccessPermission"},
        {180, D<&IApplicationAccessor::PushToFriendInvitationStorageChannel>, "PushToFriendInvitationStorageChannel"},
        {190, nullptr, "PushToNotificationStorageChannel"},
        {200, nullptr, "RequestApplicationSoftReset"},
        {201, nullptr, "RestartApplicationTimer"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IApplicationAccessor::~IApplicationAccessor() = default;

Result IApplicationAccessor::Start() {
    LOG_INFO(Service_AM, "called");
    m_applet->process->Run();
    R_SUCCEED();
}

Result IApplicationAccessor::RequestExit() {
    LOG_INFO(Service_AM, "called");

    std::scoped_lock lk{m_applet->lock};
    if (m_applet->exit_locked) {
        m_applet->lifecycle_manager.RequestExit(system.Kernel());
        m_applet->UpdateSuspensionStateLocked(true);
    } else {
        m_applet->process->Terminate();
    }

    R_SUCCEED();
}

Result IApplicationAccessor::Terminate() {
    LOG_INFO(Service_AM, "called");
    m_applet->process->Terminate();
    R_SUCCEED();
}

Result IApplicationAccessor::GetResult() {
    LOG_INFO(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    R_RETURN(m_applet->terminate_result);
}

Result IApplicationAccessor::GetAppletStateChangedEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_AM, "called");
    *out_event = m_applet->state_changed_event.GetHandle();
    R_SUCCEED();
}

Result IApplicationAccessor::PushLaunchParameter(LaunchParameterKind kind,
                                                 SharedPointer<IStorage> storage) {
    LOG_INFO(Service_AM, "called, kind={}", kind);

    const auto data = storage ? storage->GetData() : std::vector<u8>{};

    switch (kind) {
    case LaunchParameterKind::UserChannel: {
        // Popped by the application's IApplicationFunctions 1 PopLaunchParameter.
        std::scoped_lock lk{m_applet->lock};
        m_applet->user_channel_launch_parameter.push_back(data);
        R_SUCCEED();
    }
    case LaunchParameterKind::AccountPreselectedUser: {
        // A LaunchParameterAccountPreselectedUser: magic, then the selected user at 0x8.
        constexpr u32 PreselectedUserMagic = 0xC79497CA;
        u32 magic{};
        if (data.size() >= sizeof(magic)) {
            std::memcpy(&magic, data.data(), sizeof(magic));
        }
        if (data.size() >= 0x18 && magic == PreselectedUserMagic) {
            Common::UUID uuid;
            std::memcpy(&uuid, data.data() + 0x8, sizeof(uuid));
            m_last_preselected_user = uuid;
        }
        std::scoped_lock lk{m_applet->lock};
        m_applet->preselected_user_launch_parameter.push_back(data);
        R_SUCCEED();
    }
    default:
        LOG_WARNING(Service_AM, "unknown launch parameter kind {}", kind);
        R_THROW(ResultInvalidParameters);
    }
}

Result IApplicationAccessor::GetApplicationControlProperty(
    OutBuffer<BufferAttr_HipcMapAlias> out_control_property) {
    LOG_INFO(Service_AM, "called");

    std::vector<u8> nacp;
    R_TRY(system.GetARPManager().GetControlProperty(&nacp, m_applet->program_id));

    std::memcpy(out_control_property.data(), nacp.data(),
                (std::min)(out_control_property.size(), nacp.size()));

    R_SUCCEED();
}

Result IApplicationAccessor::SetUsers(bool enable,
                                      InArray<Common::UUID, BufferAttr_HipcMapAlias> user_ids) {
    LOG_INFO(Service_AM, "called, enable={} user_id_count={}", enable, user_ids.size());
    R_SUCCEED();
}

Result IApplicationAccessor::GetCurrentLibraryApplet(
    Out<SharedPointer<ILibraryAppletAccessor>> out_accessor) {
    LOG_INFO(Service_AM, "(STUBBED) called");
    *out_accessor = nullptr;
    R_SUCCEED();
}

Result IApplicationAccessor::RequestForApplicationToGetForeground() {
    LOG_INFO(Service_AM, "called");
    m_window_system.RequestApplicationToGetForeground();
    R_SUCCEED();
}

Result IApplicationAccessor::CheckRightsEnvironmentAvailable(Out<bool> out_is_available) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_is_available = true;
    R_SUCCEED();
}

Result IApplicationAccessor::GetNsRightsEnvironmentHandle(Out<u64> out_handle) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_handle = 0xdeadbeef;
    R_SUCCEED();
}

Result IApplicationAccessor::ReportApplicationExitTimeout() {
    LOG_ERROR(Service_AM, "called");
    R_SUCCEED();
}

bool IApplicationAccessor::IsSystemApplication() const {
    return m_applet->applet_id == AppletId::Starter;
}

Result IApplicationAccessor::IsCompleted(Out<bool> out_is_completed) {
    LOG_DEBUG(Service_AM, "called");
    std::scoped_lock lk{m_applet->lock};
    *out_is_completed = m_applet->is_completed;
    R_SUCCEED();
}

Result IApplicationAccessor::TerminateAllLibraryApplets() {
    LOG_INFO(Service_AM, "called");
    std::list<std::shared_ptr<Applet>> children;
    {
        std::scoped_lock lk{m_applet->lock};
        children = m_applet->child_applets;
    }
    for (const auto& child : children) {
        std::scoped_lock lk{child->lock};
        child->terminate_result = ResultLibraryAppletTerminated;
        child->process->Terminate();
    }
    m_window_system.RequestUpdate();
    R_SUCCEED();
}

Result IApplicationAccessor::AreAnyLibraryAppletsLeft(Out<bool> out_are_any_left) {
    std::scoped_lock lk{m_applet->lock};
    *out_are_any_left = !m_applet->child_applets.empty();
    LOG_DEBUG(Service_AM, "called, any_left={}", *out_are_any_left);
    R_SUCCEED();
}

Result IApplicationAccessor::GetApplicationId(Out<u64> out_application_id) {
    LOG_DEBUG(Service_AM, "called");
    // The application's id, also after ExecuteProgram moved it to another of its programs.
    std::scoped_lock lk{m_applet->lock};
    *out_application_id = FileSys::GetBaseTitleID(m_applet->program_id);
    R_SUCCEED();
}

Result IApplicationAccessor::GetApplicationLaunchProperty(
    OutBuffer<BufferAttr_HipcMapAlias> out_launch_property) {
    if (IsSystemApplication()) {
        // switchbrew says it fails for a system application; the result is undocumented.
        LOG_WARNING(Service_AM, "unknown contract: IApplicationAccessor 123 for a system "
                                "application; answered NotAvailable");
        R_THROW(ResultNoDataInChannel);
    }
    R_UNLESS(out_launch_property.size() >= sizeof(Glue::ApplicationLaunchProperty),
             ResultInvalidParameters);

    Glue::ApplicationLaunchProperty property{};
    if (R_FAILED(system.GetARPManager().GetLaunchProperty(&property, m_applet->program_id))) {
        // What the loader registers for a title: version 0, base on BuiltInUser, no patch.
        property = {
            .title_id = m_applet->program_id,
            .version = 0,
            .base_game_storage_id = FileSys::StorageId::NandUser,
            .update_storage_id = FileSys::StorageId::None,
            .program_index = 0,
            .reserved = 0,
        };
    }
    std::memcpy(out_launch_property.data(), &property, sizeof(property));
    LOG_DEBUG(Service_AM, "called, application_id={:016x}", m_applet->program_id);
    R_SUCCEED();
}

Result IApplicationAccessor::GetApplicationLaunchRequestInfo(Out<u32> out_unknown0,
                                                             Out<u32> out_unknown1,
                                                             Out<u64> out_unknown2) {
    // switchbrew: CreateApplication's default is two u32 3s and zeros; CreateSystemApplication's
    // is all zero.
    const u32 value = IsSystemApplication() ? 0 : 3;
    LOG_DEBUG(Service_AM, "called, value={}", value);
    *out_unknown0 = value;
    *out_unknown1 = value;
    *out_unknown2 = 0;
    R_SUCCEED();
}

Result IApplicationAccessor::GetDesirableUids(
    Out<s32> out_count, OutArray<Common::UUID, BufferAttr_HipcMapAlias> out_uids) {
    // qlaunch only uses one user id; its source is not known. Answered with the pushed
    // preselected user, as the Ryujinx Harbor does.
    LOG_WARNING(Service_AM, "unknown contract: IApplicationAccessor 140 GetDesirableUids "
                            "(the pushed preselected user, if any)");
    *out_count = 0;
    if (m_last_preselected_user && !out_uids.empty()) {
        out_uids[0] = *m_last_preselected_user;
        *out_count = 1;
    }
    R_SUCCEED();
}

Result IApplicationAccessor::SetApplicationAttribute(
    InLargeData<AppletAttribute, BufferAttr_HipcMapAlias> attribute) {
    LOG_DEBUG(Service_AM, "called (not used)");
    R_SUCCEED();
}

Result IApplicationAccessor::HasSaveDataAccessPermission(Out<bool> out_has_permission,
                                                         u64 application_id) {
    *out_has_permission = application_id == FileSys::GetBaseTitleID(m_applet->program_id);
    if (!*out_has_permission) {
        // switchbrew documents only the own-id case.
        LOG_WARNING(Service_AM, "unknown contract: IApplicationAccessor 170 for another "
                                "application id -> false");
    }
    R_SUCCEED();
}

Result IApplicationAccessor::PushToFriendInvitationStorageChannel(
    SharedPointer<IStorage> storage) {
    R_UNLESS(storage != nullptr, ResultInvalidParameters);
    LOG_INFO(Service_AM, "called");
    {
        std::scoped_lock lk{m_applet->lock};
        // TryPopFromFriendInvitationStorageChannel takes from the back: keep the oldest there.
        m_applet->friend_invitation_storage_channel.insert(
            m_applet->friend_invitation_storage_channel.begin(), storage->GetData());
    }
    m_applet->friend_invitation_storage_channel_event.Signal(system.Kernel());
    R_SUCCEED();
}

} // namespace Service::AM
