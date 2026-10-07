// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
//
// psc's ovln notification agent: sources by name, senders with a fixed-length queue each, and at
// most one receiver per source. A message waits in its sender's queue until a receiver that
// attached the sender's source takes it; across sources and senders the oldest goes first (one
// global sequence number). The receiver's event is signalled when its unreceived total leaves 0
// and cleared when it returns to 0.
//
// Senders (ovln:snd) and receivers (ovln:rcv) share one agent. It depends only on the standard
// library so it can be checked on its own: see ovln_agent_selfcheck.cpp.

#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/hle/service/psc/ovln/ovln_types.h"

namespace Service::PSC {

/// What the agent answers; the services map these to ovln results (module 25).
enum class OvlnStatus {
    Success,
    QueueFullRejected, // 25-0002, 0x419: send policy 3 on a full queue
    InvalidArgument,   // 25-0003, 0x619: queue length 0, unknown send option, a source not held
    QueueFull,         // 25-0010, 0x1419: the sender's queue is full (send policy 0)
    SourceAlreadyAttached, // 25-0011, 0x1619: another receiver already holds this source
    NoMessage,         // 25-0012, 0x1819: nothing to receive
};

class OvlnAgent {
public:
    struct Source;

    struct Entry {
        OverlayNotification notification;
        u64 sequence;
        u64 tick;
    };

    struct SenderState {
        std::shared_ptr<Source> source;
        std::size_t queue_length{};
        std::list<Entry> queue;
        bool closed{};
    };

    struct ReceiverState {
        /// signal: signals the receive event; clear: clears it. Both run under the agent's lock.
        ReceiverState(std::function<void()> signal_, std::function<void()> clear_)
            : signal{std::move(signal_)}, clear{std::move(clear_)} {}

        std::vector<std::shared_ptr<Source>> sources;
        std::size_t total{};
        std::function<void()> signal;
        std::function<void()> clear;
    };

    struct Source {
        SourceName name{};
        std::vector<std::shared_ptr<SenderState>> senders;
        ReceiverState* receiver{};
        std::size_t unreceived{};
    };

    /// A new emulation session: whatever the last one left (senders, receivers, messages) is gone.
    void Reset() {
        std::scoped_lock lk{mutex};
        for (auto& [_, source] : sources) {
            for (auto& sender : source->senders) {
                sender->closed = true;
                sender->queue.clear();
            }
            source->receiver = nullptr;
        }
        sources.clear();
    }

    /// ISenderService 0 OpenSender: a sender on `name` with room for `queue_length` messages.
    OvlnStatus OpenSender(const SourceName& name, u32 queue_length,
                          std::shared_ptr<SenderState>& out_sender) {
        if (queue_length == 0) {
            return OvlnStatus::InvalidArgument;
        }
        std::scoped_lock lk{mutex};
        auto source = GetOrCreateSourceLocked(name);
        auto sender = std::make_shared<SenderState>();
        sender->source = source;
        sender->queue_length = queue_length;
        source->senders.push_back(sender);
        out_sender = std::move(sender);
        return OvlnStatus::Success;
    }

    /// ISender 0 Send. The option's low byte places the message (1 at the tail, 0 at the head); its
    /// second byte decides what a full queue does (0 fail, 1 evict the head, 2 evict the tail, 3
    /// refuse).
    OvlnStatus Send(SenderState& sender, u64 option, const OverlayNotification& notification,
                    u64 tick) {
        std::scoped_lock lk{mutex};
        // The counter advances on every call, failed ones included.
        u64 sequence = ++sequence_counter;

        if (sender.closed) {
            return OvlnStatus::InvalidArgument;
        }

        const u32 placement = static_cast<u32>(option & 0xff);
        const u32 when_full = static_cast<u32>((option >> 8) & 0xff);
        if (placement > 1) {
            return OvlnStatus::InvalidArgument;
        }

        bool evicted = false;
        if (sender.queue.size() >= sender.queue_length) {
            switch (when_full) {
            case 0:
                return OvlnStatus::QueueFull;
            case 1:
                // The new message takes the evicted head's place in the global order.
                sequence = sender.queue.front().sequence;
                sender.queue.pop_front();
                evicted = true;
                break;
            case 2:
                sender.queue.pop_back();
                evicted = true;
                break;
            case 3:
                return OvlnStatus::QueueFullRejected;
            default:
                return OvlnStatus::InvalidArgument;
            }
        }

        if (placement == 1) {
            sender.queue.push_back({notification, sequence, tick});
        } else {
            // At the head it jumps the queue with the current head's sequence number.
            const u64 head_sequence = sender.queue.empty() ? sequence : sender.queue.front().sequence;
            sender.queue.push_front({notification, head_sequence, tick});
        }

        if (evicted) {
            // An eviction leaves the counts as they were and signals nothing new.
            return OvlnStatus::Success;
        }

        auto& source = *sender.source;
        source.unreceived++;
        if (source.receiver != nullptr && source.receiver->total++ == 0) {
            source.receiver->signal();
        }
        return OvlnStatus::Success;
    }

    /// Host-side send for HLE modules that notify the overlay without a guest sender (on a console
    /// they open ovln:snd like any other sender). The source gets one host sender with a queue of
    /// one, which is reopened after a new session closed it. The option places and bounds the
    /// message as in the guest Send: 0x100 (what the SDK's default sender uses) makes a new message
    /// replace a queued one, 1 appends and fails on a full queue.
    /// @param source - The source name ("overlay" for every firmware sender).
    /// @param option - The send option.
    /// @param message - The 0x80 byte message; see MakeNotification for the tag/size/payload layout.
    /// @param tick - The guest tick reported by ReceiveWithTick (0 when not known).
    /// @return Whether the message was queued.
    bool Send(std::string_view source, u64 option, std::span<const u8, sizeof(OverlayNotification)> message,
              u64 tick = 0) {
        SourceName name{};
        std::copy_n(source.begin(), std::min(source.size(), name.name.size()), name.name.begin());

        std::shared_ptr<SenderState> sender;
        {
            std::scoped_lock lk{host_mutex};
            auto& slot = host_senders[name];
            if (!slot || IsClosed(*slot)) {
                if (OpenSender(name, 1, slot) != OvlnStatus::Success) {
                    return false;
                }
            }
            sender = slot;
        }
        OverlayNotification notification;
        std::memcpy(notification.data(), message.data(), sizeof(notification));
        return Send(*sender, option, notification, tick) == OvlnStatus::Success;
    }

    /// ISender 1 GetUnreceivedMessageCount: what is still in this sender's own queue.
    u32 GetUnreceivedCount(const SenderState& sender) {
        std::scoped_lock lk{mutex};
        return static_cast<u32>(sender.queue.size());
    }

    /// Closing a sender drops what it still had queued.
    void CloseSender(SenderState& sender) {
        std::scoped_lock lk{mutex};
        if (sender.closed) {
            return;
        }
        sender.closed = true;

        auto source = sender.source;
        const std::size_t dropped = sender.queue.size();
        sender.queue.clear();
        std::erase_if(source->senders, [&](const auto& s) { return s.get() == &sender; });
        source->unreceived -= dropped;

        if (source->receiver != nullptr && dropped > 0) {
            source->receiver->total -= dropped;
            if (source->receiver->total == 0) {
                source->receiver->clear();
            }
        }
        RemoveIfUnusedLocked(source);
    }

    /// IReceiver 0 AddSource: one receiver per source; what was sent before it attached is
    /// receivable at once.
    OvlnStatus AddSource(ReceiverState& receiver, const SourceName& name) {
        std::scoped_lock lk{mutex};
        auto source = GetOrCreateSourceLocked(name);
        if (source->receiver != nullptr) {
            return OvlnStatus::SourceAlreadyAttached;
        }
        source->receiver = &receiver;
        receiver.sources.push_back(source);

        const bool signal = receiver.total == 0 && source->unreceived > 0;
        receiver.total += source->unreceived;
        if (signal) {
            receiver.signal();
        }
        return OvlnStatus::Success;
    }

    /// IReceiver 1 RemoveSource: only a source this receiver holds.
    OvlnStatus RemoveSource(ReceiverState& receiver, const SourceName& name) {
        std::scoped_lock lk{mutex};
        const auto it = sources.find(name);
        if (it == sources.end() || it->second->receiver != &receiver) {
            return OvlnStatus::InvalidArgument;
        }
        auto source = it->second;
        source->receiver = nullptr;
        receiver.total -= source->unreceived;
        if (receiver.total == 0) {
            receiver.clear();
        }
        std::erase(receiver.sources, source);
        RemoveIfUnusedLocked(source);
        return OvlnStatus::Success;
    }

    /// IReceiver 3/4: the oldest message across this receiver's sources, or NoMessage.
    OvlnStatus Receive(ReceiverState& receiver, OverlayNotification& out_notification,
                       u64& out_tick) {
        std::scoped_lock lk{mutex};
        SenderState* oldest = nullptr;
        for (auto& source : receiver.sources) {
            for (auto& sender : source->senders) {
                if (!sender->queue.empty() &&
                    (oldest == nullptr ||
                     sender->queue.front().sequence < oldest->queue.front().sequence)) {
                    oldest = sender.get();
                }
            }
        }

        if (oldest == nullptr) {
            out_notification = {};
            out_tick = 0;
            return OvlnStatus::NoMessage;
        }

        const Entry entry = oldest->queue.front();
        oldest->queue.pop_front();
        oldest->source->unreceived--;
        if (--receiver.total == 0) {
            receiver.clear();
        }
        out_notification = entry.notification;
        out_tick = entry.tick;
        return OvlnStatus::Success;
    }

    /// Closing a receiver only detaches its sources; their messages stay for the next one.
    void CloseReceiver(ReceiverState& receiver) {
        std::scoped_lock lk{mutex};
        const auto held = std::move(receiver.sources);
        receiver.sources.clear();
        for (const auto& source : held) {
            source->receiver = nullptr;
            RemoveIfUnusedLocked(source);
        }
        receiver.total = 0;
    }

private:
    std::shared_ptr<Source> GetOrCreateSourceLocked(const SourceName& name) {
        auto& source = sources[name];
        if (!source) {
            source = std::make_shared<Source>();
            source->name = name;
        }
        return source;
    }

    void RemoveIfUnusedLocked(const std::shared_ptr<Source>& source) {
        if (source->senders.empty() && source->receiver == nullptr) {
            const SourceName name = source->name;
            sources.erase(name);
        }
    }

    bool IsClosed(const SenderState& sender) {
        std::scoped_lock lk{mutex};
        return sender.closed;
    }

    std::mutex host_mutex;
    std::map<SourceName, std::shared_ptr<SenderState>> host_senders;

    std::mutex mutex;
    std::map<SourceName, std::shared_ptr<Source>> sources;
    u64 sequence_counter{};
};

/// The one agent ovln:snd and ovln:rcv share.
inline OvlnAgent& GetOvlnAgent() {
    static OvlnAgent agent;
    return agent;
}

} // namespace Service::PSC
