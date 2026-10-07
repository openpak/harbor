// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: GPL-2.0-or-later

// The friends sysmodule's IFriendService and INotificationService on OpenPak's friend graph,
// command by command as the Ryujinx Harbor's FriendService.cs serves them: the port name picks the
// permission bits (viewer for 20xxx/22xxx, manager for 30xxx, system for 4xxxx), reads come from
// the core/openpak caches only, and every write is fire and forget on the friends worker, so no
// network call ever runs on a guest's IPC thread.
//
// Only the user acc has open, signed in and linked, gets OpenPak's data. Any other user a title
// names, and every user while nobody is, gets the answers Eden's stubs gave: a console that is
// simply not online.

#pragma once

#include <memory>
#include <string_view>

#include "common/common_types.h"
#include "common/uuid.h"

namespace Core {
class System;
}

namespace Service {
class ServiceFrameworkBase;
}

namespace Service::Friend {

class FriendsSession;

/// The IFriendService a port hands out; the port's name picks its permission bits.
std::shared_ptr<ServiceFrameworkBase> CreateFriendService(Core::System& system,
                                                         std::shared_ptr<FriendsSession> session,
                                                         std::string_view port_name);

/// The INotificationService a port hands out for one (valid) user.
std::shared_ptr<ServiceFrameworkBase> CreateNotificationService(
    Core::System& system, std::shared_ptr<FriendsSession> session, std::string_view port_name,
    const Common::UUID& user);

/// am: an application process (never the HOME menu or an applet) started. Presence publishes it
/// with the presence group its NACP declares. Cheap; never touches the network.
void OnApplicationStarted(Core::System& system, u64 program_id);

/// am: the application exited; presence goes back to INACTIVE.
void OnApplicationExited();

} // namespace Service::Friend
