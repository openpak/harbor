// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <map>
#include <memory>
#include <mutex>

#include "core/hle/service/bcat/backend/backend.h"
#include "core/openpak/async_worker.h"

namespace Service::BCAT {

// Fills a title's delivery cache (the BCAT save the title reads through bcat:u) from OpenPak's news
// service (OpenPak::Bcat::Fetch: sha256-checked, kept for offline starts). A console's bcat daemon downloads in the
// background and a title's RequestSyncDeliveryCache waits on it; here the download starts when the
// title's BCAT services are created (Prepare) and a sync waits for it. Fetching happens on a worker
// thread, never on an IPC thread. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
class OpenPakBcatBackend final : public NullBcatBackend {
public:
    explicit OpenPakBcatBackend(Core::System& system, DirectoryGetter getter);
    ~OpenPakBcatBackend() override;

    bool Synchronize(Kernel::KernelCore& kernel, TitleIDVersion title,
                     ProgressServiceBackend& progress) override;
    bool SynchronizeDirectory(Kernel::KernelCore& kernel, TitleIDVersion title, std::string name,
                              ProgressServiceBackend& progress) override;
    bool Clear(Kernel::KernelCore& kernel, u64 title_id) override;
    void Prepare(Kernel::KernelCore& kernel, u64 title_id) override;

private:
    struct Deliveries;
    std::shared_ptr<Deliveries> deliveries;
};

} // namespace Service::BCAT
