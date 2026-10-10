// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/openpak/olsc_transfer.h"

#include <algorithm>
#include <cstring>
#include <random>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/logging.h"

namespace OpenPak::Olsc::Transfer {

namespace {

constexpr const char* Api = "/api/console/v3";

/// The console's error reading (0x1dedb0): a 4-digit "code" first, then the HTTP status.
Error ErrorOf(const HttpResponse& response, std::string* detail) {
    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (json.is_object()) {
        const auto code = json.find("code");
        if (code != json.end() && code->is_string()) {
            const std::string text = code->get<std::string>();
            *detail = fmt::format("server code {}", text);
            return Error::Server;
        }
    }
    if (response.status >= 200 && response.status < 300) {
        return Error::None;
    }
    *detail = fmt::format("HTTP {}", response.status);
    return Error::Http;
}

std::string CodeOf(const HttpResponse& response) {
    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (json.is_object()) {
        const auto code = json.find("code");
        if (code != json.end() && code->is_string()) {
            return code->get<std::string>();
        }
    }
    return {};
}

struct Component {
    std::uint64_t id = 0;
    std::uint32_t index = 0;
    std::string status;
    std::int64_t size = 0;
    std::string put_url;
    std::string get_url;
};

std::optional<Component> ParseComponent(const nlohmann::json& item) {
    if (!item.is_object() || !item.contains("id") || !item.contains("index") ||
        !item["id"].is_number_unsigned() || !item["index"].is_number_unsigned()) {
        return std::nullopt;
    }
    Component component;
    component.id = item["id"].get<std::uint64_t>();
    component.index = item["index"].get<std::uint32_t>();
    component.status = item.value("status", std::string{});
    if (const auto size = item.find("archive_size"); size != item.end() && size->is_number()) {
        component.size = size->get<std::int64_t>();
    }
    component.put_url = item.value("put_url", std::string{});
    component.get_url = item.value("get_url", std::string{});
    return component;
}

struct Archive {
    std::uint64_t id = 0;
    std::string status;
    int num_of_partitions = 0;
    std::uint64_t series_id = 0;
    std::uint64_t commit_id = 0;
    std::vector<Component> components;
};

std::optional<Archive> ParseArchive(const std::string& body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    const auto archive = json.is_object() ? json.find("save_data_archive") : json.end();
    if (archive == json.end() || !archive->is_object() || !archive->contains("id") ||
        !(*archive)["id"].is_number_unsigned()) {
        return std::nullopt;
    }
    Archive out;
    out.id = (*archive)["id"].get<std::uint64_t>();
    out.status = archive->value("status", std::string{});
    if (const auto count = archive->find("num_of_partitions");
        count != archive->end() && count->is_number_integer()) {
        out.num_of_partitions = count->get<int>();
    }
    if (const auto series = archive->find("series_id");
        series != archive->end() && series->is_number_unsigned()) {
        out.series_id = series->get<std::uint64_t>();
    }
    if (const auto digest = archive->find("encoded_digest"); digest != archive->end() &&
                                                            digest->is_string()) {
        if (const auto bytes = FromBase64Url(digest->get<std::string>()); bytes && bytes->size() >= 8) {
            std::memcpy(&out.commit_id, bytes->data(), 8);
        }
    }
    if (const auto list = archive->find("component_files");
        list != archive->end() && list->is_array()) {
        for (const auto& item : *list) {
            if (auto component = ParseComponent(item)) {
                out.components.push_back(std::move(*component));
            }
        }
    }
    return out;
}

std::optional<Component> ParseComponentReply(const std::string& body) {
    const auto json = nlohmann::json::parse(body, nullptr, false);
    const auto component = json.is_object() ? json.find("component_file") : json.end();
    if (component == json.end()) {
        return std::nullopt;
    }
    return ParseComponent(*component);
}

/// X-SDA-ID, the archive a request is about (0x1e7d10 sends it when the caller names one).
Headers Sda(std::uint64_t archive_id) {
    return {{"X-SDA-ID", std::to_string(archive_id)}};
}

Outcome Fail(Error error, std::string detail, std::uint64_t archive_id = 0) {
    LOG_WARNING(Service_OLSC, "save data cloud transfer: {} (2179-{:04})", detail,
                static_cast<std::uint32_t>(error));
    return {error, archive_id, std::move(detail)};
}

/// The policy check both directions make (olsc-transfer.md §2.2 step 2).
std::optional<Outcome> CheckPolicy(Rest& rest, std::uint64_t application_id,
                                   std::uint32_t launch_version) {
    // ?dtoken= is the dauth device token for olsc's client; without one the console never
    // sends the request (0x1f4ac0).
    const std::string token = rest.DeviceToken();
    if (token.empty()) {
        return Fail(Error::Http, "policy: no device token");
    }
    const auto reply = rest.Request(
        "GET", PolicyHost,
        fmt::format("/api/console/v2/application_policy/{:016x}/{:05}?dtoken={}", application_id,
                    launch_version, token),
        {});
    std::string detail;
    if (ErrorOf(reply, &detail) != Error::None) {
        return Fail(Error::PolicyForbidden, "policy: " + detail);
    }
    const auto json = nlohmann::json::parse(reply.body, nullptr, false);
    if (!json.is_object() || json.value("policy_type", std::string{}) != "ALL_OK") {
        return Fail(Error::PolicyForbidden, "policy is not ALL_OK");
    }
    return std::nullopt;
}

Aad MakeAad(std::uint64_t application_id, std::uint64_t archive_id) {
    Aad aad{};
    std::memcpy(aad.data(), &application_id, 8);
    std::memcpy(aad.data() + 8, &archive_id, 8);
    return aad;
}

/// The archive's key seed package for this task's challenge (0x20f860 → 0x1e73c0).
std::optional<Outcome> LoadKeySeedPackage(Rest& rest, Fs& fs, const std::string& archive_path,
                                          std::uint64_t archive_id) {
    const auto challenge = fs.Challenge();
    const auto generated = rest.Request(
        "POST", StorageHost, archive_path + "/generate_key_seed_package",
        fmt::format(R"({{"encoded_challenge": "{}"}})", Base64Url(challenge)), Sda(archive_id));
    std::string detail;
    if (ErrorOf(generated, &detail) != Error::None) {
        return Fail(Error::Http, "generate_key_seed_package: " + detail, archive_id);
    }
    const auto json = nlohmann::json::parse(generated.body, nullptr, false);
    const auto package =
        json.is_object() ? FromBase64Url(json.value("encoded_key_seed_package", std::string{}))
                         : std::nullopt;
    if (!package || package->size() != 0x200 || !fs.SetKeySeedPackage(*package)) {
        return Fail(Error::Fs, "key seed package refused", archive_id);
    }
    return std::nullopt;
}

/// The archive's `meta` component, the 8 KiB initial data (0x20bb10).
std::optional<Outcome> ReadMeta(Rest& rest, const Archive& archive, Bytes* out) {
    const auto meta = std::find_if(archive.components.begin(), archive.components.end(),
                                   [](const Component& c) { return c.index == 0; });
    if (meta == archive.components.end() || meta->size != 0x2000) {
        return Fail(Error::MetaWrongSize, "meta component not 8 KiB", archive.id);
    }
    const auto readable = rest.Request(
        "POST", StorageHost, fmt::format("{}/component_files/{}/signed_uri", Api, meta->id),
        R"({"permissions": ["read"]})", Sda(archive.id));
    const auto meta_url = ParseComponentReply(readable.body);
    if (!meta_url || meta_url->get_url.empty()) {
        return Fail(Error::MetaWithoutUrl, "meta without get_url", archive.id);
    }
    const auto bytes = rest.Blob("GET", meta_url->get_url, {});
    if (bytes.body.size() != 0x2000) {
        return Fail(Error::MetaWrongSize, "meta not 8 KiB", archive.id);
    }
    out->assign(bytes.body.begin(), bytes.body.end());
    return std::nullopt;
}

} // namespace

std::string Base64Url(std::span<const std::uint8_t> data) {
    static constexpr char Alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = data[i] << 16 | data[i + 1] << 8 | data[i + 2];
        out += Alphabet[v >> 18 & 63];
        out += Alphabet[v >> 12 & 63];
        out += Alphabet[v >> 6 & 63];
        out += Alphabet[v & 63];
    }
    if (data.size() - i == 1) {
        const std::uint32_t v = data[i] << 16;
        out += Alphabet[v >> 18 & 63];
        out += Alphabet[v >> 12 & 63];
    } else if (data.size() - i == 2) {
        const std::uint32_t v = data[i] << 16 | data[i + 1] << 8;
        out += Alphabet[v >> 18 & 63];
        out += Alphabet[v >> 12 & 63];
        out += Alphabet[v >> 6 & 63];
    }
    return out;
}

std::optional<Bytes> FromBase64Url(std::string_view text) {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    Bytes out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) {
            return std::nullopt;
        }
        buffer = buffer << 6 | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(buffer >> bits & 0xff));
        }
    }
    return out;
}

std::array<std::uint8_t, 16> Md5(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 16> out{};
    unsigned int length = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_md5(), nullptr);
    return out;
}

std::string StartUploadBody(const LocalSave& save, std::uint64_t series_id) {
    // encoded_digest: the commit id, little-endian, in 32 zero bytes (0x1ec2f0).
    std::array<std::uint8_t, 32> digest{};
    std::memcpy(digest.data(), &save.commit_id, 8);
    return fmt::format(
        R"({{"application_id":"{:016x}","datatype":0,"data_size":{},"saved_at_as_unixtime":{},)"
        R"("launch_required_version":{},"series_id":{},"encoded_digest":"{}",)"
        R"("has_thumbnail":false,"desc":null,"auto_backup":false,"acd_index":{}}})",
        save.application_id, save.data_size, save.saved_at, save.launch_required_version,
        series_id, Base64Url(digest), save.acd_index);
}

Outcome Upload(Rest& rest, Fs& fs, const LocalSave& save, std::uint64_t series_id,
               Progress* progress) {
    if (auto failed = CheckPolicy(rest, save.application_id, save.launch_required_version)) {
        return *failed;
    }

    // The cloud's records for the title: one still uploading blocks a new upload, a fixed one
    // is the base of a delta upload.
    const auto list =
        rest.Request("GET", StorageHost,
                     fmt::format("{}/save_data_archives?datatype=0&application_id={:016x}", Api,
                                 save.application_id),
                     {});
    std::string detail;
    if (ErrorOf(list, &detail) != Error::None) {
        return Fail(Error::Http, "archive list: " + detail);
    }
    const auto records = nlohmann::json::parse(list.body, nullptr, false);
    std::optional<std::uint64_t> base_id;
    if (records.is_object() && records.contains("save_data_archives") &&
        records["save_data_archives"].is_array()) {
        for (const auto& record : records["save_data_archives"]) {
            const auto status = record.value("status", std::string{});
            if (status == "uploading") {
                return Fail(Error::AlreadyUploading, "an archive of the title is uploading");
            }
            if (status == "fixed" && record.contains("id") && record["id"].is_number_unsigned()) {
                // ponytail: the newest fixed record is the base (the console keeps two records;
                // the server lists one fixed archive per title).
                base_id = std::max(base_id.value_or(0), record["id"].get<std::uint64_t>());
            }
        }
    }

    std::unique_ptr<Exporter> exporter;
    std::optional<Archive> archive;
    std::optional<Archive> base;
    if (base_id) {
        // Delta upload (§2.3): the base archive with its components, its key seed, its meta.
        const std::string base_path = fmt::format("{}/save_data_archives/{}", Api, *base_id);
        const auto got = rest.Request("GET", StorageHost, base_path, {}, Sda(*base_id));
        base = CodeOf(got) == "0001" ? std::nullopt : ParseArchive(got.body);
        if (!base) {
            return Fail(Error::SeriesBaseMissing, "the base archive is gone", *base_id);
        }
        if (auto failed = LoadKeySeedPackage(rest, fs, base_path, base->id)) {
            return *failed;
        }
        Bytes meta;
        if (auto failed = ReadMeta(rest, *base, &meta)) {
            return *failed;
        }
        exporter = fs.OpenDiffExporter(meta);
        if (!exporter) {
            return Fail(Error::Fs, "the save could not be diff-exported", base->id);
        }
        const auto aad = exporter->ImportAad();
        const auto expected = MakeAad(save.application_id, base->id);
        if (!std::equal(aad.begin(), aad.begin() + 16, expected.begin())) {
            return Fail(Error::AadMismatch, "base initial data AAD", base->id);
        }
        const auto started = rest.Request("POST", StorageHost, base_path + "/start_upload",
                                          StartUploadBody(save, base->series_id), Sda(base->id));
        if (CodeOf(started) == "0010") {
            return Fail(Error::AlreadyUploading, "start_upload: already uploading");
        }
        if (ErrorOf(started, &detail) != Error::None) {
            return Fail(Error::Http, "start_upload: " + detail, base->id);
        }
        archive = ParseArchive(started.body);
        if (!archive) {
            return Fail(Error::ArchiveIncomplete, "start_upload reply has no archive");
        }
        // No SetDivisionCount: the initial data carries the base's.
        if (archive->num_of_partitions != base->num_of_partitions) {
            return Fail(Error::PartitionCountDiffers, "num_of_partitions differs from the base",
                        archive->id);
        }
    } else {
        if (series_id == 0) {
            std::random_device random;
            series_id = (std::uint64_t{random()} << 32 | random()) & 0x7fffffffffffffffULL;
        }
        const auto started = rest.Request("POST", StorageHost,
                                          fmt::format("{}/save_data_archives/start_upload", Api),
                                          StartUploadBody(save, series_id));
        if (CodeOf(started) == "0010") {
            return Fail(Error::AlreadyUploading, "start_upload: already uploading");
        }
        if (ErrorOf(started, &detail) != Error::None) {
            return Fail(Error::Http, "start_upload: " + detail);
        }
        archive = ParseArchive(started.body);
        if (!archive) {
            return Fail(Error::ArchiveIncomplete, "start_upload reply has no archive");
        }
        // A full upload creates every component itself; the reply's are not read (0x1edbf0).
        archive->components.clear();
        exporter = fs.OpenFullExporter();
        if (!exporter) {
            return Fail(Error::Fs, "the save could not be exported", archive->id);
        }
        if (!exporter->SetDivisionCount(archive->num_of_partitions)) {
            return Fail(Error::Fs,
                        fmt::format("SetDivisionCount({}) refused", archive->num_of_partitions),
                        archive->id);
        }
    }
    exporter->SetAad(MakeAad(save.application_id, archive->id));

    const auto ids = exporter->ChunkIds();
    if (progress) {
        progress->total = 0;
        progress->done = 0;
        for (const auto id : ids) {
            if (const auto chunk = exporter->Chunk(id)) {
                progress->total += chunk->size();
            }
        }
    }
    const std::string archive_path = fmt::format("{}/save_data_archives/{}", Api, archive->id);
    const auto sda = Sda(archive->id);
    unsigned counter = 0;
    for (const auto id : ids) {
        // X-Ignorable: counter=<chunk index> (0x1eed30); "2000" is tolerated.
        auto extend_headers = sda;
        extend_headers.emplace_back("X-Ignorable", fmt::format("counter={}", counter++));
        const auto extended = rest.Request("POST", StorageHost,
                                           archive_path + "/extend_upload_timeout", {},
                                           extend_headers);
        if (ErrorOf(extended, &detail) != Error::None && CodeOf(extended) != "2000") {
            return Fail(Error::Http, "extend_upload_timeout: " + detail, archive->id);
        }

        // The component of this index (0x20f7f0): a delta's start_upload listed the base's;
        // absent ones are created (`meta` iff chunk 0).
        std::optional<Component> component;
        if (const auto listed =
                std::find_if(archive->components.begin(), archive->components.end(),
                             [id](const Component& c) { return c.index == id; });
            listed != archive->components.end()) {
            component = *listed;
        } else {
            const auto created = rest.Request(
                "POST", StorageHost, archive_path + "/component_files/create",
                fmt::format(R"({{"index": {},"datatype": "{}"}})", id, id == 0 ? "meta" : "save"),
                sda);
            if (ErrorOf(created, &detail) != Error::None) {
                return Fail(Error::Http, "component create: " + detail, archive->id);
            }
            component = ParseComponentReply(created.body);
        }
        if (!component) {
            return Fail(Error::ArchiveIncomplete, "component create reply", archive->id);
        }
        const std::string component_path =
            fmt::format("{}/component_files/{}", Api, component->id);
        if (component->status == "fixed") {
            return Fail(Error::ComponentAlreadyFixed, "component already fixed", archive->id);
        }
        const auto signed_uri =
            component->status == "hand_over"
                ? rest.Request("POST", StorageHost, component_path + "/update", {}, sda)
                : rest.Request("POST", StorageHost, component_path + "/signed_uri",
                               R"({"permissions": ["write"]})", sda);
        if (ErrorOf(signed_uri, &detail) != Error::None) {
            return Fail(Error::Http, "signed_uri: " + detail, archive->id);
        }
        const auto writable = ParseComponentReply(signed_uri.body);
        if (!writable || writable->put_url.rfind("https://", 0) != 0) {
            return Fail(Error::ArchiveIncomplete, "component without put_url", archive->id);
        }

        const auto bytes = exporter->Chunk(id);
        if (!bytes) {
            return Fail(Error::Fs, fmt::format("chunk {:#x} unreadable", id), archive->id);
        }
        const auto put = rest.Blob("PUT", writable->put_url, *bytes);
        if (put.status < 200 || put.status >= 300) {
            return Fail(Error::Http, fmt::format("blob PUT: HTTP {}", put.status), archive->id);
        }
        const auto finished = rest.Request(
            "POST", StorageHost, component_path + "/finish_upload",
            fmt::format(R"({{"archive_size": {},"encoded_archive_digest": "{}"}})", bytes->size(),
                        Base64Url(Md5(*bytes))),
            sda);
        if (ErrorOf(finished, &detail) != Error::None) {
            return Fail(Error::Http, "component finish_upload: " + detail, archive->id);
        }
        if (progress) {
            progress->done += bytes->size();
        }
    }

    const auto mac = exporter->InitialDataMac();
    if (!mac) {
        return Fail(Error::Fs, "no initial-data MAC", archive->id);
    }
    // A full upload sends its new key seed (0x1ee2d0); a delta keeps the base's and sends the
    // MAC alone (0x1ee5a0).
    const std::string finish_body =
        base ? fmt::format(R"({{"encoded_mac": "{}"}})", Base64Url(*mac))
             : fmt::format(R"({{"encoded_key_seed": "{}","encoded_mac": "{}"}})",
                           Base64Url(exporter->KeySeed()), Base64Url(*mac));
    const auto finished =
        rest.Request("POST", StorageHost, archive_path + "/finish_upload", finish_body, sda);
    if (ErrorOf(finished, &detail) != Error::None) {
        return Fail(Error::Http, "finish_upload: " + detail, archive->id);
    }
    exporter->Finalize();

    const auto check = rest.Request("GET", StorageHost, archive_path, {}, sda);
    if (CodeOf(check) == "0001" || !ParseArchive(check.body)) {
        return Fail(Error::ArchiveMissingAfterFinish, "archive missing after finish", archive->id);
    }
    LOG_INFO(Service_OLSC, "save data cloud: uploaded {} chunk(s){}", ids.size(),
             base ? " (delta)" : "");
    return {Error::None, archive->id, {}, base ? base->series_id : series_id, save.commit_id};
}

Outcome Download(Rest& rest, Fs& fs, std::uint64_t application_id, std::uint64_t archive_id,
                 std::uint32_t launch_version, Progress* progress) {
    const std::string archive_path = fmt::format("{}/save_data_archives/{}", Api, archive_id);
    const auto sda = Sda(archive_id);
    const auto got = rest.Request("GET", StorageHost, archive_path, {}, sda);
    std::string detail;
    if (CodeOf(got) == "0001") {
        return Fail(Error::ArchiveGone, "archive gone", archive_id);
    }
    if (ErrorOf(got, &detail) != Error::None) {
        return Fail(Error::Http, "archive: " + detail, archive_id);
    }
    const auto archive = ParseArchive(got.body);
    if (!archive) {
        return Fail(Error::ArchiveIncomplete, "archive reply", archive_id);
    }
    if (auto failed = CheckPolicy(rest, application_id, launch_version)) {
        return *failed;
    }
    if (auto failed = LoadKeySeedPackage(rest, fs, archive_path, archive_id)) {
        return *failed;
    }
    Bytes meta;
    if (auto failed = ReadMeta(rest, *archive, &meta)) {
        return *failed;
    }
    auto importer = fs.OpenImporter(meta);
    if (!importer) {
        return Fail(Error::Fs, "the archive could not be opened for import", archive_id);
    }
    const auto aad = importer->ImportAad();
    const auto expected = MakeAad(application_id, archive_id);
    if (!std::equal(aad.begin(), aad.begin() + 16, expected.begin())) {
        return Fail(Error::AadMismatch, "initial data AAD", archive_id);
    }

    const auto ids = importer->ChunkIds();
    const auto started =
        rest.Request("POST", StorageHost, archive_path + "/start_download", {}, sda);
    if (ErrorOf(started, &detail) != Error::None) {
        return Fail(Error::Http, "start_download: " + detail, archive_id);
    }
    const auto downloading = ParseArchive(started.body);
    if (!downloading) {
        return Fail(Error::ArchiveIncomplete, "start_download reply", archive_id);
    }
    if (progress) {
        std::uint64_t total = 0;
        for (const auto& component : downloading->components) {
            total += static_cast<std::uint64_t>(std::max<std::int64_t>(component.size, 0));
        }
        progress->done = 0;
        progress->total = total;
    }
    for (const auto id : ids) {
        const auto component =
            std::find_if(downloading->components.begin(), downloading->components.end(),
                         [id](const Component& c) { return c.index == id; });
        if (component == downloading->components.end() || component->get_url.empty()) {
            return Fail(Error::ChunkWithoutComponent, fmt::format("chunk {:#x}", id), archive_id);
        }
        const auto blob = rest.Blob("GET", component->get_url, {});
        if (blob.status < 200 || blob.status >= 300) {
            return Fail(Error::Http, fmt::format("blob GET: HTTP {}", blob.status), archive_id);
        }
        const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(blob.body.data()), blob.body.size()};
        if (!importer->Push(id, bytes)) {
            return Fail(Error::Fs, fmt::format("chunk {:#x} refused", id), archive_id);
        }
        if (progress) {
            progress->done += bytes.size();
        }
    }
    const auto finished =
        rest.Request("POST", StorageHost, archive_path + "/finish_download", {}, sda);
    if (ErrorOf(finished, &detail) != Error::None) {
        return Fail(Error::Http, "finish_download: " + detail, archive_id);
    }
    if (!importer->Finalize()) {
        return Fail(Error::Fs, "the save could not be written", archive_id);
    }
    LOG_INFO(Service_OLSC, "save data cloud: downloaded {} chunk(s)", ids.size());
    return {Error::None, archive_id, {}, archive->series_id, archive->commit_id};
}

} // namespace OpenPak::Olsc::Transfer
