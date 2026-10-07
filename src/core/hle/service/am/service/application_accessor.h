// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "common/uuid.h"
#include "core/hle/service/am/am_types.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"

namespace Service::AM {

struct Applet;
class ILibraryAppletAccessor;
class IStorage;
class WindowSystem;

class IApplicationAccessor final : public ServiceFramework<IApplicationAccessor> {
public:
    explicit IApplicationAccessor(Core::System& system_, std::shared_ptr<Applet> applet,
                                  WindowSystem& window_system);
    ~IApplicationAccessor() override;

private:
    Result Start();
    Result RequestExit();
    Result Terminate();
    Result GetResult();
    Result GetAppletStateChangedEvent(OutCopyHandle<Kernel::KReadableEvent> out_event);
    Result PushLaunchParameter(LaunchParameterKind kind, SharedPointer<IStorage> storage);
    Result GetApplicationControlProperty(OutBuffer<BufferAttr_HipcMapAlias> out_control_property);
    Result SetUsers(bool enable, InArray<Common::UUID, BufferAttr_HipcMapAlias> user_ids);
    Result GetCurrentLibraryApplet(Out<SharedPointer<ILibraryAppletAccessor>> out_accessor);
    Result RequestForApplicationToGetForeground();
    Result CheckRightsEnvironmentAvailable(Out<bool> out_is_available);
    Result GetNsRightsEnvironmentHandle(Out<u64> out_handle);
    Result ReportApplicationExitTimeout();
    Result IsCompleted(Out<bool> out_is_completed);
    Result TerminateAllLibraryApplets();
    Result AreAnyLibraryAppletsLeft(Out<bool> out_are_any_left);
    Result GetApplicationId(Out<u64> out_application_id);
    Result GetApplicationLaunchProperty(OutBuffer<BufferAttr_HipcMapAlias> out_launch_property);
    Result GetApplicationLaunchRequestInfo(Out<u32> out_unknown0, Out<u32> out_unknown1,
                                           Out<u64> out_unknown2);
    Result GetDesirableUids(Out<s32> out_count,
                            OutArray<Common::UUID, BufferAttr_HipcMapAlias> out_uids);
    Result SetApplicationAttribute(InLargeData<AppletAttribute, BufferAttr_HipcMapAlias> attribute);
    Result HasSaveDataAccessPermission(Out<bool> out_has_permission, u64 application_id);
    Result PushToFriendInvitationStorageChannel(SharedPointer<IStorage> storage);

    bool IsSystemApplication() const;

    WindowSystem& m_window_system;
    const std::shared_ptr<Applet> m_applet;
    std::optional<Common::UUID> m_last_preselected_user{};
};

} // namespace Service::AM
