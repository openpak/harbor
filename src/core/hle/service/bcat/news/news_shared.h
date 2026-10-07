// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/common_types.h"
#include "core/hle/service/bcat/news/news_storage.h"
#include "core/hle/service/kernel_helpers.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
}

namespace Service::BCAT {
class OpenPakFetcher;
}

namespace Service::News {

// What the five news:* ports and every object they create share: the article store, the two events
// HOME waits on, the topic subscriptions, and the worker that fills the store from OpenPak.
class NewsShared {
public:
    NewsShared(Core::System& system_, std::filesystem::path directory);
    ~NewsShared();

    Core::System& system;
    NewsStorage store;

    // Signalled when news arrives (newly arrived) or is replaced (overwrite). The store signals newly
    // arrived on PostLocalNews and the OpenPak fill; nothing is downloaded over an existing article,
    // so overwrite never signals.
    Kernel::KEvent* newly_arrived{};
    Kernel::KEvent* overwrite{};

    void SignalNewlyArrived();

    // Fill the store from OpenPak's news service on a worker thread. Does nothing while a fill runs or
    // while there is no fetcher.
    void StartFill();

    u32 GetSubscription(const std::string& topic);
    void SetSubscription(const std::string& topic, u32 status);
    void ClearSubscriptions();

private:
    void Fill(std::stop_token stop);

    KernelHelpers::ServiceContext service_context;
    std::mutex subscription_mutex;
    std::map<std::string, u32> subscriptions;
    std::atomic<bool> filling{false};
    std::jthread fill_thread;
};

// Ask the running news service to fill its store again. For the OpenPak side to call once its
// configuration is up, since the first fill at boot finds no fetcher while OpenPak is off.
void RequestNewsFill();

// The route on the news edge that serves, decoded, what the firmware asks for at `url` as a signed and
// encrypted container (INewsService 30500), or empty for a URL with no decoded counterpart.
std::string LocalRoute(std::string_view url);

// Percent-encode like Uri.EscapeDataString.
std::string EscapeDataString(std::string_view text);

} // namespace Service::News
