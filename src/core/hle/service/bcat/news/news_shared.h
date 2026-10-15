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
#include "core/openpak/async_worker.h"

namespace Core {
class System;
}

namespace Kernel {
class KEvent;
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

    // Signal newly arrived from any thread: the signal itself happens on the worker's signalling
    // thread, the one host thread allowed to touch this session's kernel objects.
    void SignalNewlyArrived();
    // The same for the overwrite event, which now signals when a channel's articles are removed.
    void SignalOverwrite();

    // The session-owned worker for what blocks on the network: download contexts run on it and
    // the fill posts its signal through it.
    std::unique_ptr<OpenPak::AsyncWorker> worker;

    // Fill the store from OpenPak's news service on a worker thread. Does nothing while a fill runs or
    // while there is no fetcher.
    void StartFill();

    // Subscription status of a topic: 0 forget/unknown, 1 unsubscribed, 2 subscribed, 3 auto. The
    // statuses are the user's choice and are kept in <news dir>/subscriptions.txt; only a followed
    // topic (2 or 3) is filled or kept in the store.
    u32 GetSubscription(const std::string& topic);
    void SetSubscription(const std::string& topic, u32 status);
    void ClearSubscriptions();
    bool IsFollowed(const std::string& topic);

private:
    void Fill(std::stop_token stop);
    void FillOnce(std::stop_token stop);

    KernelHelpers::ServiceContext service_context;
    std::mutex subscription_mutex;
    std::map<std::string, u32> subscriptions;
    std::atomic<bool> filling{false};
    std::atomic<bool> refill{false}; // a follow arrived while a fill ran: fill once more
    std::mutex fill_mutex;           // orders StartFill against a fill that is finishing
    std::jthread fill_thread;
    std::filesystem::path subscriptions_path;
    void SaveSubscriptions();
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
