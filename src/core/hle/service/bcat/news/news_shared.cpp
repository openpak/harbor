// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/news/news_shared.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/bcat/openpak_fetch.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

std::mutex active_mutex;
NewsShared* active{};

std::vector<std::string> SplitSegments(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            parts.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

} // namespace

std::string EscapeDataString(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                c == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
            out += hex;
        }
    }
    return out;
}

std::string LocalRoute(std::string_view url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string_view::npos ? std::string_view::npos
                                                          : url.find('/', scheme + 3);
    if (start == std::string_view::npos) {
        return {};
    }

    std::string_view path = url.substr(start);
    path = path.substr(0, path.find('?'));
    const auto parts = SplitSegments(path, '/');

    const auto is = [&](size_t index, std::string_view value) {
        return parts.size() > index && parts[index] == value;
    };

    if (parts.size() == 5 && is(0, "api") && is(1, "nx") && is(2, "v1") && is(3, "topics") &&
        is(4, "catalog")) {
        return "/openpak/v1/local/catalog";
    }
    if (parts.size() == 6 && is(0, "api") && is(1, "nx") && is(2, "v1") && is(3, "topics") &&
        (is(5, "detail") || is(5, "icon") || is(5, "archives"))) {
        return "/openpak/v1/local/" + parts[4] + "/" + parts[5];
    }
    if (parts.size() == 6 && is(0, "api") && is(1, "nx") && is(2, "v2") && is(3, "topics") &&
        is(5, "online_archives")) {
        return "/openpak/v1/local/" + parts[4] + "/archives";
    }
    if (parts.size() == 7 && is(0, "api") && is(1, "nx") && is(2, "v1") && is(3, "topics") &&
        is(6, "article")) {
        return "/openpak/v1/local/" + parts[4] + "/news/" + parts[5];
    }
    if (parts.size() == 7 && is(0, "openpak") && is(1, "v1") && is(2, "topics") && is(4, "news") &&
        is(6, "article")) {
        return "/openpak/v1/local/" + parts[3] + "/news/" + parts[5];
    }
    if (parts.size() == 7 && is(0, "openpak") && is(1, "v1") && is(2, "topics") && is(4, "news") &&
        is(6, "summary")) {
        return "/openpak/v1/local/" + parts[3] + "/news/" + parts[5] + "/summary";
    }
    return {};
}

NewsShared::NewsShared(Core::System& system_, std::filesystem::path directory)
    : system{system_}, store{directory}, worker{std::make_unique<OpenPak::AsyncWorker>(system_, "news:OpenPak")},
      service_context{system_, "News"}, subscriptions_path{directory / "subscriptions.txt"} {
    newly_arrived = service_context.CreateEvent("News::NewlyArrived");
    overwrite = service_context.CreateEvent("News::Overwrite");

    // The user's own choices. A first run has no file: the two channels every OpenPak install
    // follows are seeded once, and after that only the user changes the list (an unfollow sticks,
    // and a channel the server adds later is not followed on the user's behalf).
    bool existed = false;
    {
        std::ifstream in{subscriptions_path};
        existed = in.good();
        std::string line;
        while (std::getline(in, line)) {
            const auto space = line.find(' ');
            if (space == std::string::npos) {
                continue;
            }
            try {
                subscriptions[line.substr(space + 1)] = static_cast<u32>(std::stoul(line.substr(0, space)));
            } catch (const std::exception&) {
            }
        }
    }
    if (!existed) {
        subscriptions["openpak_news"] = 2;
        subscriptions["openpak_games"] = 2;
        SaveSubscriptions();
    }
    // Articles of channels nobody follows leave the store (the Nintendo ones an earlier server index
    // put there, channels unfollowed while another build ran).
    std::vector<std::string> followed;
    for (const auto& [topic, status] : subscriptions) {
        if (status >= 2) {
            followed.push_back(topic);
        }
    }
    const int dropped = store.RemoveTopicsNotIn(followed);
    if (dropped > 0) {
        LOG_INFO(Service_BCAT, "[OpenPak] News: removed {} article(s) of channels not followed", dropped);
    }

    std::scoped_lock lock{active_mutex};
    active = this;
}

NewsShared::~NewsShared() {
    {
        std::scoped_lock lock{active_mutex};
        if (active == this) {
            active = nullptr;
        }
    }

    fill_thread.request_stop();
    if (fill_thread.joinable()) {
        fill_thread.join();
    }

    // Nothing signals the events after this.
    worker.reset();

    service_context.CloseEvent(newly_arrived);
    service_context.CloseEvent(overwrite);
}

void NewsShared::SignalNewlyArrived() {
    worker->Post([this] { newly_arrived->Signal(system.Kernel()); });
}

void NewsShared::SignalOverwrite() {
    worker->Post([this] { overwrite->Signal(system.Kernel()); });
}

void NewsShared::SaveSubscriptions() {
    // Callers hold subscription_mutex, or are the constructor.
    std::error_code ec;
    std::filesystem::create_directories(subscriptions_path.parent_path(), ec);
    const auto temporary = std::filesystem::path{subscriptions_path}.concat(".new");
    {
        std::ofstream out{temporary, std::ios::trunc};
        for (const auto& [topic, status] : subscriptions) {
            out << status << ' ' << topic << '\n';
        }
    }
    std::filesystem::rename(temporary, subscriptions_path, ec);
}

bool NewsShared::IsFollowed(const std::string& topic) {
    return GetSubscription(topic) >= 2;
}

u32 NewsShared::GetSubscription(const std::string& topic) {
    std::scoped_lock lock{subscription_mutex};
    const auto it = subscriptions.find(topic);
    return it == subscriptions.end() ? 0 : it->second;
}

void NewsShared::SetSubscription(const std::string& topic, u32 status) {
    {
        std::scoped_lock lock{subscription_mutex};
        subscriptions[topic] = status;
        SaveSubscriptions();
    }
    LOG_INFO(Service_BCAT, "[OpenPak] News: subscription of '{}' set to {}", topic, status);
    // Unfollowing removes the channel's articles, as the console's News module does, and tells the
    // applet through the overwrite event.
    if (status < 2 && store.RemoveTopic(topic) > 0) {
        SignalOverwrite();
    }
}

void NewsShared::ClearSubscriptions() {
    {
        std::scoped_lock lock{subscription_mutex};
        subscriptions.clear();
        SaveSubscriptions();
    }
    LOG_INFO(Service_BCAT, "[OpenPak] News: every subscription cleared");
    if (store.RemoveTopicsNotIn({}) > 0) {
        SignalOverwrite();
    }
}

void NewsShared::StartFill() {
    if (filling.exchange(true)) {
        return;
    }

    // The previous worker has finished (filling was false), so this joins at once.
    fill_thread = std::jthread([this](std::stop_token stop) { Fill(std::move(stop)); });
}

// GET the local index ("<topic> <news_id> <bytes>" per line), then every article not stored yet or
// changed in size; signals newly arrived when anything was added.
void NewsShared::Fill(std::stop_token stop) {
    const auto fetcher = BCAT::GetOpenPakFetcher();
    int added = 0;

    // The index may be asked for before OpenPak is up (its configuration is applied by the
    // frontend) or while the server is away: ask again every so often, for a while.
    constexpr int Attempts = 20;
    constexpr auto Pause = std::chrono::seconds{15};
    BCAT::FetchResponse response;
    for (int attempt = 0; attempt < Attempts && !stop.stop_requested(); ++attempt) {
        response = fetcher->Get("/openpak/v1/local/index");
        if (response.reached && response.status == 200) {
            break;
        }
        if (attempt == 0) {
            LOG_WARNING(Service_BCAT, "[OpenPak] News index not fetched ({}); retrying",
                        response.reached ? response.status : 0);
        }
        std::mutex wait_mutex;
        std::condition_variable_any wake;
        std::unique_lock lock{wait_mutex};
        wake.wait_for(lock, stop, Pause, [] { return false; });
    }
    if (stop.stop_requested() || !response.reached || response.status != 200) {
        LOG_WARNING(Service_BCAT, "[OpenPak] News index not fetched ({})",
                    response.reached ? response.status : 0);
        filling = false;
        return;
    }

    std::istringstream index{std::string(response.body.begin(), response.body.end())};
    std::string line;
    while (!stop.stop_requested() && std::getline(index, line)) {
        std::istringstream fields{line};
        std::string topic;
        std::string id_text;
        std::string size_text;
        std::string extra;
        if (!(fields >> topic >> id_text >> size_text) || (fields >> extra)) {
            if (!line.empty() && line != "\r") {
                LOG_WARNING(Service_BCAT, "[OpenPak] News index line skipped");
            }
            continue;
        }
        // The user's preferences: nothing is fetched for a channel they do not follow.
        if (!IsFollowed(topic)) {
            continue;
        }

        unsigned long long id{};
        long long size{};
        try {
            size_t used = 0;
            id = std::stoull(id_text, &used);
            if (used != id_text.size()) {
                throw std::invalid_argument("id");
            }
            size = std::stoll(size_text, &used);
            if (used != size_text.size()) {
                throw std::invalid_argument("size");
            }
        } catch (const std::exception&) {
            LOG_WARNING(Service_BCAT, "[OpenPak] News index line skipped");
            continue;
        }

        char news_id[32];
        std::snprintf(news_id, sizeof(news_id), "LS%020llu", id);

        // Stored already, unless the server's copy has another size: a rebuilt article replaces it.
        if (store.StoredSize(news_id) == size) {
            continue;
        }

        const auto article = fetcher->Get(
            "/openpak/v1/local/" + EscapeDataString(topic) + "/news/" + std::to_string(id));
        if (!article.reached || article.status != 200) {
            LOG_WARNING(Service_BCAT, "[OpenPak] News article not fetched ({})",
                        article.reached ? article.status : 0);
            continue;
        }
        if (static_cast<long long>(article.body.size()) != size) {
            LOG_WARNING(Service_BCAT, "[OpenPak] News article is {} bytes, the index says {}; skipped",
                        article.body.size(), size);
            continue;
        }

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (store.Post(article.body, now)) {
            ++added;
        }
    }

    if (added > 0) {
        SignalNewlyArrived();
    }
    LOG_INFO(Service_BCAT, "[OpenPak] News: {} new article(s)", added);
    filling = false;
}

void RequestNewsFill() {
    std::scoped_lock lock{active_mutex};
    if (active != nullptr) {
        active->StartFill();
    }
}

} // namespace Service::News
