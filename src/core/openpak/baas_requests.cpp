// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived from openpak-client 3fd9ba7 (baas_requests.cpp).

// Friend requests on the route a console's friends module takes: the inbox and outbox boxes
// (§A.4.2) polled beside the invitation inbox, and the send/answer/read writes (§A.4.1, §A.4.4)
// the guest's 30xxx commands fire at. The URLs and bodies are pure functions of their arguments,
// ported from Ryujinx.OpenPak/OpenPakBaas.Requests.cs; the calls that use them are the request
// half of Ryujinx's OpenPakSession.Requests.cs and OpenPakSession.Invitations.cs.

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "core/openpak/baas.h"
#include "core/openpak/baas_detail.h"
#include "common/logging.h"
#include "core/openpak/session.h"

namespace OpenPak::Baas {

namespace {

using detail::Dump;
using detail::Hex16;

std::string RequestBoxPath(const std::string& user_id, bool inbox, int offset, int count) {
    return fmt::format("/2.0.0/users/{}/friend_requests/{}"
                       "?offset={}&count={}&sort=createdAt:desc&filter.state.$eq=PENDING",
                       user_id, inbox ? "inbox" : "outbox", offset, count);
}

std::string RequestPath(std::uint64_t request_id) {
    return fmt::format("/2.0.0/friend_requests/{}", Hex16(request_id));
}

/// A GET the way the reference's session makes one for the request boxes: anything but a 2xx
/// with a JSON body is a failure, and the caller keeps what it had.
std::optional<Json> GetOrFail(const std::string& path, std::string& why) {
    const Reply reply = detail::Send("GET", BaasHost, path, {}, {});

    if (!reply.Success()) {
        why = fmt::format("{} returned {}: {}", path, reply.status, reply.body);
        return std::nullopt;
    }

    Json document = Json::parse(reply.body, nullptr, false);
    if (document.is_discarded()) {
        why = fmt::format("{} answered with something that is not JSON", path);
        return std::nullopt;
    }

    return document;
}

/// A write whose reply is the request object (§A.4.4), or nothing with the reason in `why`.
std::optional<Json> WriteOrFail(const char* method, const std::string& path,
                                const char* content_type, const std::string& body,
                                std::string& why) {
    const Reply reply = detail::Send(method, BaasHost, path, content_type, body);

    if (!reply.Success()) {
        // The body carries the reason; a bare status code has cost days on this chain before.
        why = fmt::format("{} returned {}: {}", path, reply.status, reply.body);
        return std::nullopt;
    }

    Json document = Json::parse(reply.body, nullptr, false);
    if (document.is_discarded()) {
        why = fmt::format("{} answered with something that is not JSON", path);
        return std::nullopt;
    }

    return document;
}

/// The sender-only half every variant starts with.
Json SendHead(const std::string& channel, std::uint64_t sender_id, std::uint64_t receiver_id) {
    Json body = Json::object();
    body["channels"] = Json::array({channel});
    body["senderId"] = Hex16(sender_id);
    body["receiverId"] = Hex16(receiver_id);
    return body;
}

} // namespace

// ---- the calls, as pure functions ----

std::string RequestBoxUrl(const std::string& host, const std::string& user_id, bool inbox,
                          int offset, int count) {
    return fmt::format("https://{}{}", host, RequestBoxPath(user_id, inbox, offset, count));
}

std::string RequestUrl(const std::string& host, std::uint64_t request_id) {
    return fmt::format("https://{}{}", host, RequestPath(request_id));
}

std::string AnswerRequestBody(const std::string& state) {
    if (state != "CANCELED" && state != "AUTHORIZED" && state != "REJECTED") {
        throw std::invalid_argument(
            fmt::format("A request answer is CANCELED, AUTHORIZED or REJECTED, not '{}'.", state));
    }

    return Dump(Json::array({Json{{"op", "replace"}, {"path", "/state"}, {"value", state}}}));
}

std::string ReadRequestBody() {
    return Dump(
        Json::array({Json{{"op", "add"}, {"path", "/extras/receiver/read"}, {"value", true}}}));
}

std::string SendRequestBody(const std::string& channel, std::uint64_t sender_id,
                            std::uint64_t receiver_id) {
    Json body = SendHead(channel, sender_id, receiver_id);
    body["extras"] = Json{{"sender", Json{{"route:sender", "me"}}}};
    return Dump(body);
}

std::string SendRequestBody(const std::string& channel, std::uint64_t sender_id,
                            std::uint64_t receiver_id, std::uint64_t application_id,
                            std::uint8_t acd_index, std::uint64_t presence_group_id,
                            const std::string& target_name, const std::string& target_language,
                            const std::string& own_name, const std::string& own_language) {
    Json sender = Json::object();
    sender["route:sender"] = "me";
    sender["route:name"] = target_name;
    sender["route:name:language"] = target_language;

    Json both = Json::object();
    both["route:appInfo:appId"] = Hex16(application_id);
    both["route:appInfo:acdIndex"] = acd_index;
    both["route:appInfo:presenceGroupId"] = Hex16(presence_group_id);
    both["route:name"] = own_name;
    both["route:name:language"] = own_language;

    Json body = SendHead(channel, sender_id, receiver_id);
    body["extras"] = Json::object();
    body["extras"]["sender"] = std::move(sender);
    body["extras"]["senderAndReceiver"] = std::move(both);
    return Dump(body);
}

std::string SendRequestCatalogBody(const std::string& channel, std::uint64_t sender_id,
                                   std::uint64_t receiver_id, const std::string& catalog_id,
                                   const std::string& target_name,
                                   const std::string& target_language, const std::string& own_name,
                                   const std::string& own_language) {
    Json sender = Json::object();
    sender["route:sender"] = "me";
    sender["route:name"] = target_name;
    sender["route:name:language"] = target_language;

    Json both = Json::object();
    both["route:candidate:catalogId"] = catalog_id;
    both["route:name"] = own_name;
    both["route:name:language"] = own_language;

    Json body = SendHead(channel, sender_id, receiver_id);
    body["extras"] = Json::object();
    body["extras"]["sender"] = std::move(sender);
    body["extras"]["senderAndReceiver"] = std::move(both);
    return Dump(body);
}

std::string SendRequestNnidBody(const std::string& channel, std::uint64_t sender_id,
                                std::uint64_t receiver_id, const std::string& mii_name,
                                const std::string& mii_image_url_param) {
    Json sender = Json::object();
    sender["route:sender"] = "me";
    sender["route:nnid:miiName"] = mii_name;
    sender["route:nnid:miiImageUrlParam"] = mii_image_url_param;

    Json both = Json::object();
    both["route:nnid:miiName"] = mii_name;
    both["route:nnid:miiImageUrlParam"] = mii_image_url_param;

    Json body = SendHead(channel, sender_id, receiver_id);
    body["extras"] = Json::object();
    body["extras"]["sender"] = std::move(sender);
    body["extras"]["senderAndReceiver"] = std::move(both);
    return Dump(body);
}

bool TryCatalogId(std::string_view catalog_id, std::uint64_t& hi, std::uint64_t& lo) {
    hi = 0;
    lo = 0;

    return catalog_id.size() == 32 && TryHexText(catalog_id.substr(0, 16), hi) &&
           TryHexText(catalog_id.substr(16, 16), lo);
}

// ---- the calls, made ----

void RefreshFriendRequests() {
    const std::string user_id = UserId();
    if (user_id.empty()) {
        return;
    }

    std::string why;
    const std::optional<Json> inbox = GetOrFail(RequestBoxPath(user_id, true, 0, 100), why);
    const std::optional<Json> outbox =
        inbox ? GetOrFail(RequestBoxPath(user_id, false, 0, 100), why) : std::nullopt;

    if (!inbox || !outbox) {
        LOG_DEBUG(WebService, "[OpenPak] Friend request boxes: {}", why);
        return;
    }

    SetFriendRequests(ParseRequestList(*inbox, true), ParseRequestList(*outbox, false));
}

void AnswerFriendRequest(std::uint64_t request_id, const std::string& state) {
    // A write the guest fired is worth a sign-in, as the reference refreshes one first.
    Session::Ensure({});

    if (!Ready()) {
        return;
    }

    std::string why;
    std::optional<Json> reply;

    try {
        reply = WriteOrFail("PATCH", RequestPath(request_id), detail::JsonPatch,
                            AnswerRequestBody(state), why);
    } catch (const std::invalid_argument& e) {
        why = e.what();
    }

    if (!reply) {
        LOG_WARNING(WebService, "[OpenPak] Answering a friend request with {}: {}", state, why);
        return;
    }

    // The reply is the request object (§A.4.4); an AUTHORIZED one is what tells the console to
    // sync at once, which the refresh below does either way.
    ParseRequest(*reply, state != "CANCELED");

    RefreshFriendRequests();
}

void ReadFriendRequest(std::uint64_t request_id) {
    // A write the guest fired is worth a sign-in, as the reference refreshes one first.
    Session::Ensure({});

    if (!Ready()) {
        return;
    }

    std::string why;
    const std::optional<Json> reply =
        WriteOrFail("PATCH", RequestPath(request_id), detail::JsonPatch, ReadRequestBody(), why);

    if (!reply) {
        LOG_WARNING(WebService, "[OpenPak] Marking a friend request read: {}", why);
        return;
    }

    ParseRequest(*reply, true);

    RefreshFriendRequests();
}

void SendFriendRequest(const FriendRequestSend& send) {
    // A write the guest fired is worth a sign-in, as the reference refreshes one first.
    Session::Ensure({});

    const std::string user_id = UserId();
    if (user_id.empty()) {
        return;
    }

    const std::string receiver = ReceiverId(send.target_id);

    std::uint64_t sender_id = 0;
    std::uint64_t receiver_id = 0;
    if (!TryHexText(user_id, sender_id) || !TryHexText(receiver, receiver_id)) {
        LOG_WARNING(WebService, "[OpenPak] Friend request not sent: the receiver has no BAAS user");
        return;
    }

    // One variant per send command (§A.4.1): the catalog and NNID routes each carry their own
    // extras, and everything else is the plain or the app route.
    std::string body;
    if (!send.catalog_id.empty()) {
        body = SendRequestCatalogBody(send.channel, sender_id, receiver_id, send.catalog_id,
                                      send.target_name, send.target_language, send.own_name,
                                      send.own_language);
    } else if (!send.mii_name.empty()) {
        body = SendRequestNnidBody(send.channel, sender_id, receiver_id, send.mii_name,
                                   send.mii_image_url_param);
    } else if (send.application_id == 0) {
        body = SendRequestBody(send.channel, sender_id, receiver_id);
    } else {
        body = SendRequestBody(send.channel, sender_id, receiver_id, send.application_id,
                               send.acd_index, send.presence_group_id, send.target_name,
                               send.target_language, send.own_name, send.own_language);
    }

    // The module refuses a body it cannot fit in its own buffer before it sends it.
    if (body.size() > 0x400) {
        LOG_WARNING(WebService, "[OpenPak] Friend request not sent: the body is over 0x400 bytes");
        return;
    }

    std::string why;
    const std::optional<Json> reply =
        WriteOrFail("POST", "/2.0.0/friend_requests", detail::JsonType, body, why);

    if (!reply) {
        LOG_WARNING(WebService, "[OpenPak] Friend request not sent: {}", why);
        return;
    }

    const std::optional<Request> sent = ParseRequest(*reply, false);
    if (sent && sent->state == 3) {
        RefreshFriendRequests();
    }
}

std::string ReceiverId(std::uint64_t network_service_account_id) {
    if (FindFriend(network_service_account_id)) {
        return Hex16(network_service_account_id);
    }

    return Hex16(network_service_account_id);
}

} // namespace OpenPak::Baas
