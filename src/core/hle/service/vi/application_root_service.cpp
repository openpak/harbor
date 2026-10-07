// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/vi/application_display_service.h"
#include "core/hle/service/vi/application_root_service.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/service_creator.h"
#include "core/hle/service/vi/vi.h"
#include "core/hle/service/vi/vi_types.h"

namespace Service::VI {

IApplicationRootService::IApplicationRootService(Core::System& system_,
                                                 std::shared_ptr<Container> container)
    : ServiceFramework{system_, "vi:u"}, m_container{std::move(container)} {
    static const FunctionInfo functions[] = {
        {0, C<&IApplicationRootService::GetDisplayService>, "GetDisplayService"},
        {1, C<&IApplicationRootService::GetDisplayServiceWithProxyNameExchange>, "GetDisplayServiceWithProxyNameExchange"},
    };
    RegisterHandlers(functions);
}

IApplicationRootService::~IApplicationRootService() = default;

Result IApplicationRootService::GetDisplayService(
    Out<SharedPointer<IApplicationDisplayService>> out_application_display_service, Policy policy) {
    LOG_DEBUG(Service_VI, "called");
    R_RETURN(GetApplicationDisplayService(out_application_display_service, system, m_container,
                                          Permission::User, policy));
}

// How a program holding only vi:u (the first-time setup, a system application) gets the
// system-level display service for its shared layer: the name is exchanged for the rights am
// registered for it. There is no name registry here; every caller is a firmware program am
// launched. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
Result IApplicationRootService::GetDisplayServiceWithProxyNameExchange(
    Out<SharedPointer<IApplicationDisplayService>> out_application_display_service, Policy policy,
    u64 proxy_name) {
    LOG_DEBUG(Service_VI, "called, policy={}", policy);
    R_RETURN(GetApplicationDisplayService(out_application_display_service, system, m_container,
                                          Permission::System, policy));
}

} // namespace Service::VI
