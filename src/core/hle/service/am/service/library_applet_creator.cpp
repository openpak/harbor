// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/library_applet_storage.h"
#include "core/hle/service/am/process_creation.h"
#include "core/hle/service/am/service/library_applet_accessor.h"
#include "core/hle/service/am/service/library_applet_creator.h"

#include "core/hle/api_version.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/am/window_system.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/sm/sm.h"

namespace Service::AM {

namespace {

bool ShouldCreateGuestApplet(Core::System& system, AppletId applet_id) {
    // A HOME session runs every library applet from the firmware, as a console does; the host
    // (frontend) applets remain for game sessions and as a fallback when a program cannot load.
    if (system.GetAppletManager().IsHomeSession()) {
        return true;
    }

#define X(Name, name)                                                                              \
    if (applet_id == AppletId::Name &&                                                             \
        Settings::values.name##_applet_mode.GetValue() != Settings::AppletMode::LLE) {             \
        return false;                                                                              \
    }

    X(Cabinet, cabinet)
    X(Controller, controller)
    X(DataErase, data_erase)
    X(Error, error)
    X(NetConnect, net_connect)
    X(ProfileSelect, player_select)
    X(SoftwareKeyboard, swkbd)
    X(MiiEdit, mii_edit)
    X(Web, web)
    X(Shop, shop)
    X(PhotoViewer, photo_viewer)
    X(OfflineWeb, offline_web)
    X(LoginShare, login_share)
    X(WebAuth, wifi_web_auth)
    X(MyPage, my_page)

#undef X

    return true;
}

bool IsProgramInstalled(Core::System& system, AppletProgramId program_id) {
    return system.GetContentProviderUnion().HasEntry(static_cast<u64>(program_id),
                                                     FileSys::ContentRecordType::Program);
}

// Firmware 23.0.1's own applet table: am's (applet id, program id) pairs, read from its rodata
// (am 0100000000000023, at 0x15794c). Used in a HOME session when the firmware has the program.
// Shop (0x14) is not in it; the Shop is the system web program like Login, as the Ryujinx Harbor
// found on 22.x.
AppletProgramId AppletIdToFirmware23ProgramId(AppletId applet_id) {
    struct Entry {
        u32 applet_id;
        u64 program_id;
    };
    static constexpr Entry table[] = {
        {0x0A, 0x0100000000001001}, {0x0B, 0x0100000000001002}, {0x0C, 0x0100000000001003},
        {0x0D, 0x0100000000001004}, {0x0E, 0x0100000000001005}, {0x0F, 0x0100000000001006},
        {0x10, 0x0100000000001007}, {0x11, 0x0100000000001008}, {0x12, 0x0100000000001009},
        {0x15, 0x010000000000100D}, {0x16, 0x010000000000100E}, {0x1A, 0x0100000000001013},
        {0x1B, 0x010000000000101A}, {0x1C, 0x010000000000101C}, {0x1D, 0x010000000000101D},
        {0x1F, 0x010070000E3C0000}, {0x20, 0x010086000E49C000}, {0x1E, 0x0100000000001020},
        {0x22, 0x0100000000001007}, {0x64, 0x0100000000001048}, {0x50, 0x0100000000001007},
        {0x51, 0x0100000000001001}, {0x52, 0x0100000000001001}, {0x66, 0x0100000000001054},
        {0x03, 0x0100000000001000}, {0x04, 0x0100000000001012}, {0x13, 0x0100000000001043},
        {0x17, 0x010000000000100F}, {0x18, 0x0100000000001042}, {0x19, 0x0100000000001043},
        {0x32, 0x010000000000100F}, {0x33, 0x010000000000100F}, {0x35, 0x0100000000001042},
        {0x36, 0x0100000000001042}, {0x37, 0x0100000000001042}, {0x21, 0x0100000000001038},
        {0x14, 0x0100000000001042},
    };
    for (const auto& entry : table) {
        if (entry.applet_id == static_cast<u32>(applet_id)) {
            return static_cast<AppletProgramId>(entry.program_id);
        }
    }
    return static_cast<AppletProgramId>(0);
}

AppletProgramId AppletIdToProgramId(AppletId applet_id) {
    switch (applet_id) {
    case AppletId::OverlayDisplay:
        return AppletProgramId::OverlayDisplay;
    case AppletId::QLaunch:
        return AppletProgramId::QLaunch;
    case AppletId::Starter:
        return AppletProgramId::Starter;
    case AppletId::Auth:
        return AppletProgramId::Auth;
    case AppletId::Cabinet:
        return AppletProgramId::Cabinet;
    case AppletId::Controller:
        return AppletProgramId::Controller;
    case AppletId::DataErase:
        return AppletProgramId::DataErase;
    case AppletId::Error:
        return AppletProgramId::Error;
    case AppletId::NetConnect:
        return AppletProgramId::NetConnect;
    case AppletId::ProfileSelect:
        return AppletProgramId::ProfileSelect;
    case AppletId::SoftwareKeyboard:
        return AppletProgramId::SoftwareKeyboard;
    case AppletId::MiiEdit:
        return AppletProgramId::MiiEdit;
    case AppletId::Web:
        return AppletProgramId::Web;
    case AppletId::Shop:
        return AppletProgramId::Shop;
    case AppletId::PhotoViewer:
        return AppletProgramId::PhotoViewer;
    case AppletId::Settings:
        return AppletProgramId::Settings;
    case AppletId::OfflineWeb:
        return AppletProgramId::OfflineWeb;
    case AppletId::LoginShare:
        return AppletProgramId::LoginShare;
    case AppletId::WebAuth:
        return AppletProgramId::WebAuth;
    case AppletId::MyPage:
        return AppletProgramId::MyPage;
    default:
        return static_cast<AppletProgramId>(0);
    }
}

std::shared_ptr<ILibraryAppletAccessor> CreateGuestApplet(Core::System& system,
                                                          WindowSystem& window_system,
                                                          std::shared_ptr<Applet> caller_applet,
                                                          AppletId applet_id,
                                                          LibraryAppletMode mode) {
    auto program_id = static_cast<u64>(AppletIdToProgramId(applet_id));
    if (system.GetAppletManager().IsHomeSession()) {
        const auto firmware_id = AppletIdToFirmware23ProgramId(applet_id);
        if (firmware_id != static_cast<AppletProgramId>(0) &&
            IsProgramInstalled(system, firmware_id)) {
            program_id = static_cast<u64>(firmware_id);
        }
    }
    if (program_id == 0) {
        // Unknown applet
        LOG_WARNING(Service_AM, "no firmware program for applet_id={}", applet_id);
        return {};
    }

    auto process = CreateProcess(system, program_id, 1, HLE::ApiVersion::HOS_VERSION_MAJOR);
    if (process) {
        const auto applet = std::make_shared<Applet>(system, std::move(process), false);
        applet->program_id = program_id;
        applet->applet_id = applet_id;
        applet->type = AppletType::LibraryApplet;
        applet->library_applet_mode = mode;
        applet->window_visible = mode != LibraryAppletMode::AllForegroundInitiallyHidden;

        auto broker = std::make_shared<AppletDataBroker>(system);
        applet->caller_applet = caller_applet;
        applet->caller_applet_broker = broker;
        {
            std::scoped_lock lk{caller_applet->lock};
            caller_applet->child_applets.push_back(applet);
        }
        (void)window_system.TrackApplet(applet, false);
        LOG_INFO(Service_AM,
                 "am: library applet {} ({:016x}) created as pid {} ({}) for pid {}, Applet pool "
                 "{} MiB free",
                 applet_id, program_id, applet->aruid.pid, mode, caller_applet->aruid.pid,
                 system.Kernel().MemoryManager().GetFreeSize(Kernel::KMemoryManager::Pool::Applet) /
                     (1024 * 1024));
        return std::make_shared<ILibraryAppletAccessor>(system, broker, applet);
    }
    // Couldn't initialize the guest process
    LOG_WARNING(Service_AM, "library applet {} ({:016x}) could not be loaded; using the host applet",
                applet_id, program_id);
    return {};
}

std::shared_ptr<ILibraryAppletAccessor> CreateFrontendApplet(Core::System& system,
                                                             WindowSystem& window_system,
                                                             std::shared_ptr<Applet> caller_applet,
                                                             AppletId applet_id,
                                                             LibraryAppletMode mode) {
    const auto program_id = static_cast<u64>(AppletIdToProgramId(applet_id));

    auto process = std::make_unique<Process>(system);
    auto applet = std::make_shared<Applet>(system, std::move(process), false);
    applet->program_id = program_id;
    applet->applet_id = applet_id;
    applet->type = AppletType::LibraryApplet;
    applet->library_applet_mode = mode;

    auto storage = std::make_shared<AppletDataBroker>(system);
    applet->caller_applet = caller_applet;
    applet->caller_applet_broker = storage;
    applet->frontend = system.GetFrontendAppletHolder().GetApplet(applet, applet_id, mode);
    {
        std::scoped_lock lk{caller_applet->lock};
        caller_applet->child_applets.push_back(applet);
    }
    return std::make_shared<ILibraryAppletAccessor>(system, storage, applet);
}

} // namespace

namespace {
// A HOME session runs every library applet as the firmware's own process; a host-drawn stand-in
// (a host keyboard, a host PIN entry) must never appear there, so a guest applet that cannot be
// created fails the guest's request instead.
Result CreateAppletForCaller(std::shared_ptr<ILibraryAppletAccessor>* out, Core::System& system,
                             WindowSystem& window_system, std::shared_ptr<Applet> caller,
                             AppletId applet_id, LibraryAppletMode mode) {
    std::shared_ptr<ILibraryAppletAccessor> library_applet;
    if (ShouldCreateGuestApplet(system, applet_id)) {
        library_applet = CreateGuestApplet(system, window_system, caller, applet_id, mode);
    }
    if (!library_applet && system.GetAppletManager().IsHomeSession()) {
        LOG_ERROR(Service_AM, "am: library applet {} could not be created; no host applet in a "
                              "HOME session, the caller gets AppletLaunchFailed",
                  applet_id);
        R_THROW(ResultAppletLaunchFailed);
    }
    if (!library_applet) {
        library_applet = CreateFrontendApplet(system, window_system, caller, applet_id, mode);
    }
    if (!library_applet) {
        LOG_ERROR(Service_AM, "Applet doesn't exist! applet_id={}", applet_id);
        R_THROW(ResultUnknown);
    }
    *out = std::move(library_applet);
    R_SUCCEED();
}
} // namespace

ILibraryAppletCreator::ILibraryAppletCreator(Core::System& system_, std::shared_ptr<Applet> applet,
                                             WindowSystem& window_system)
    : ServiceFramework{system_, "ILibraryAppletCreator"},
      m_window_system{window_system}, m_applet{std::move(applet)} {
    static const FunctionInfo functions[] = {
        {0, D<&ILibraryAppletCreator::CreateLibraryApplet>, "CreateLibraryApplet"},
        {1, nullptr, "TerminateAllLibraryApplets"},
        {2, nullptr, "AreAnyLibraryAppletsLeft"},
        {3, D<&ILibraryAppletCreator::CreateLibraryAppletEx>, "CreateLibraryAppletEx"},
        {10, D<&ILibraryAppletCreator::CreateStorage>, "CreateStorage"},
        {11, D<&ILibraryAppletCreator::CreateTransferMemoryStorage>, "CreateTransferMemoryStorage"},
        {12, D<&ILibraryAppletCreator::CreateHandleStorage>, "CreateHandleStorage"},
    };
    RegisterHandlers(functions);
}

ILibraryAppletCreator::~ILibraryAppletCreator() = default;

Result ILibraryAppletCreator::CreateLibraryApplet(
    Out<SharedPointer<ILibraryAppletAccessor>> out_library_applet_accessor, AppletId applet_id,
    LibraryAppletMode library_applet_mode) {
    LOG_DEBUG(Service_AM, "called with applet_id={} applet_mode={}", applet_id,
              library_applet_mode);

    std::shared_ptr<ILibraryAppletAccessor> library_applet;
    R_TRY(CreateAppletForCaller(&library_applet, system, m_window_system, m_applet, applet_id,
                                library_applet_mode));

    // Applet is created, can now be launched.
    m_applet->library_applet_launchable_event.Signal(system.Kernel());
    *out_library_applet_accessor = library_applet;
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateLibraryAppletEx(
    Out<SharedPointer<ILibraryAppletAccessor>> out_library_applet_accessor, AppletId applet_id,
    LibraryAppletMode library_applet_mode, u64 thread_id) {
    LOG_DEBUG(Service_AM, "called with applet_id={} applet_mode={} thread_id={}", applet_id,
              library_applet_mode, thread_id);

    std::shared_ptr<ILibraryAppletAccessor> library_applet;
    R_TRY(CreateAppletForCaller(&library_applet, system, m_window_system, m_applet, applet_id,
                                library_applet_mode));

    // Applet is created, can now be launched.
    m_applet->library_applet_launchable_event.Signal(system.Kernel());
    *out_library_applet_accessor = library_applet;
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateStorage(Out<SharedPointer<IStorage>> out_storage, s64 size) {
    LOG_DEBUG(Service_AM, "called, size={}", size);

    if (size <= 0) {
        LOG_ERROR(Service_AM, "size is less than or equal to 0");
        R_THROW(ResultUnknown);
    }

    *out_storage = std::make_shared<IStorage>(system, AM::CreateStorage(std::vector<u8>(size)));
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateTransferMemoryStorage(
    Out<SharedPointer<IStorage>> out_storage, bool is_writable, s64 size,
    InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle) {
    LOG_DEBUG(Service_AM, "called, is_writable={} size={}", is_writable, size);

    if (size <= 0) {
        LOG_ERROR(Service_AM, "size is less than or equal to 0");
        R_THROW(ResultUnknown);
    }

    if (!transfer_memory_handle) {
        LOG_ERROR(Service_AM, "transfer_memory_handle is null");
        R_THROW(ResultUnknown);
    }

    *out_storage = std::make_shared<IStorage>(
        system, AM::CreateTransferMemoryStorage(system.Kernel(), transfer_memory_handle->GetOwner()->GetMemory(),
                                                transfer_memory_handle.Get(), is_writable, size));
    R_SUCCEED();
}

Result ILibraryAppletCreator::CreateHandleStorage(
    Out<SharedPointer<IStorage>> out_storage, s64 size,
    InCopyHandle<Kernel::KTransferMemory> transfer_memory_handle) {
    LOG_DEBUG(Service_AM, "called, size={}", size);

    if (size <= 0) {
        LOG_ERROR(Service_AM, "size is less than or equal to 0");
        R_THROW(ResultUnknown);
    }

    if (!transfer_memory_handle) {
        LOG_ERROR(Service_AM, "transfer_memory_handle is null");
        R_THROW(ResultUnknown);
    }

    *out_storage = std::make_shared<IStorage>(
        system, AM::CreateHandleStorage(system.Kernel(), transfer_memory_handle->GetOwner()->GetMemory(),
                                        transfer_memory_handle.Get(), size));
    R_SUCCEED();
}

} // namespace Service::AM
