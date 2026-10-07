// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <filesystem>
#include <memory>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/hle/service/bcat/backend/backend.h"
#include "core/hle/service/bcat/bcat.h"
#include "core/hle/service/bcat/news/news_shared.h"
#include "core/hle/service/bcat/news/service_creator.h"
#include "core/hle/service/bcat/openpak_fetch.h"
#include "core/hle/service/bcat/service_creator.h"
#include "core/hle/service/server_manager.h"

namespace Service::BCAT {

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("bcat:a", std::make_shared<IServiceCreator>(system, "bcat:a"), 32);
    server_manager->RegisterNamedService("bcat:m", std::make_shared<IServiceCreator>(system, "bcat:m"), 32);
    server_manager->RegisterNamedService("bcat:u", std::make_shared<IServiceCreator>(system, "bcat:u"), 32);
    server_manager->RegisterNamedService("bcat:s", std::make_shared<IServiceCreator>(system, "bcat:s"), 32);

    // Until the OpenPak side installs a fetcher, a directory of files can stand in for it (self-checks
    // and desktop runs without a server): HARBOR_OPENPAK_STUB_DIR, or <user dir>/openpak/stub. See
    // FileFetcher.
    if (!OpenPakFetcherIsStub()) {
        std::filesystem::path stub;
        if (const char* env = std::getenv("HARBOR_OPENPAK_STUB_DIR"); env != nullptr && *env != '\0') {
            stub = env;
        } else if (const auto dir =
                       Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "openpak" / "stub";
                   std::filesystem::is_directory(dir)) {
            stub = dir;
        }
        if (!stub.empty()) {
            LOG_WARNING(Service_BCAT, "OpenPak fetches come from files, not a server");
            SetOpenPakFetcher(std::make_shared<FileFetcher>(stub));
        }
    }

    // One store, two events and one fill worker behind all five news ports.
    const auto news = std::make_shared<News::NewsShared>(
        system, Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "openpak" / "news");

    server_manager->RegisterNamedService("news:a", std::make_shared<News::IServiceCreator>(system, news, 0xffffffff, "news:a"), 32);
    server_manager->RegisterNamedService("news:p", std::make_shared<News::IServiceCreator>(system, news, 0x1, "news:p"), 32);
    server_manager->RegisterNamedService("news:c", std::make_shared<News::IServiceCreator>(system, news, 0x2, "news:c"), 32);
    server_manager->RegisterNamedService("news:v", std::make_shared<News::IServiceCreator>(system, news, 0x4, "news:v"), 32);
    server_manager->RegisterNamedService("news:m", std::make_shared<News::IServiceCreator>(system, news, 0xd, "news:m"), 32);

    news->StartFill();

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::BCAT
