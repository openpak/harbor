// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <quazip.h>
#include <quazipfile.h>

#include "common/httplib.h"
#include "common/logging.h"
#include "core/openpak/platform.h"
#include "yuzu/harbor_test_drivers.h"

namespace HarborDesktop {

namespace {

namespace fs = std::filesystem;
using namespace HarborSetup;

constexpr std::size_t MaxMeta = 64 * 1024;

struct Meta {
    std::string name, version, vendor, library;
    int min_api = 0;
    std::string error;
};

/// meta.json of an AdrenoTools package, wherever it is in the zip.
Meta ReadMeta(const fs::path& zip_path) {
    QuaZip zip(QString::fromStdString(zip_path.string()));
    if (!zip.open(QuaZip::mdUnzip)) {
        return {.error = "The driver package is not a zip file."};
    }
    for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
        const QString name = zip.getCurrentFileName();
        if (name != QStringLiteral("meta.json") && !name.endsWith(QStringLiteral("/meta.json"))) {
            continue;
        }
        QuaZipFile file(&zip);
        if (!file.open(QIODevice::ReadOnly)) {
            break;
        }
        const QByteArray data = file.read(MaxMeta + 1);
        if (data.size() > static_cast<qsizetype>(MaxMeta)) {
            return {.error = "The driver's meta.json is too large."};
        }
        const auto json = nlohmann::json::parse(data.toStdString(), nullptr, false);
        if (!json.is_object()) {
            return {.error = "The driver's meta.json cannot be read."};
        }
        Meta meta;
        meta.name = json.value("name", "");
        meta.version = json.value("driverVersion", "");
        meta.vendor = json.value("vendor", "");
        meta.library = json.value("libraryName", "");
        if (json.contains("minApi") && json["minApi"].is_number_integer()) {
            meta.min_api = json["minApi"].get<int>();
        }
        if (meta.library.empty()) {
            meta.error = "The driver's meta.json names no library.";
        }
        return meta;
    }
    return {.error = "The driver package has no meta.json."};
}

std::pair<std::string, std::string> SplitUrl(const std::string& url) {
    const std::size_t path_at = url.find('/', url.find("://") + 3);
    if (path_at == std::string::npos) {
        return {url, "/"};
    }
    return {url.substr(0, path_at), url.substr(path_at)};
}

class TestStore final : public Host {
public:
    explicit TestStore(fs::path dir_) : dir{std::move(dir_)} {
        std::error_code ec;
        fs::create_directories(dir / "staged", ec);
        device.abis = std::vector<std::string>{"arm64-v8a", "armeabi-v7a"};
        device.vulkan_major = 1;
        device.vulkan_minor = 1;
        device.gpu = "Adreno (TM) 650";
        device.adreno = true;
        device.android_api = 31;
        device.android_release = "12";
        LOG_INFO(Frontend, "harbor: test driver store in {} (Adreno 650, API 31)", dir.string());
    }

    bool IsAndroid() const override {
        return false;
    }
    DeviceInfo Probe() override {
        return device;
    }
    void PickFile(const std::string&, const std::vector<std::string>&, PickCallback done) override {
        done(std::nullopt);
    }
    void PickFolder(const std::string&, PickCallback done) override {
        done(std::nullopt);
    }
    bool HasDriverStore() const override {
        return true;
    }

    std::vector<InstalledDriver> InstalledDrivers() override {
        std::vector<InstalledDriver> out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".zip") {
                continue;
            }
            const auto meta = ReadMeta(entry.path());
            std::string detail = meta.version;
            if (!meta.vendor.empty()) {
                detail += (detail.empty() ? "" : " · ") + meta.vendor;
            }
            out.push_back({entry.path().filename().string(),
                           meta.name.empty() ? entry.path().stem().string() : meta.name, detail});
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        return out;
    }

    std::string CurrentDriver() override {
        std::ifstream in(dir / "selected");
        std::string id;
        std::getline(in, id);
        return fs::exists(dir / id) ? id : std::string{};
    }

    DriverPackage InspectDriver(const std::string& location) override {
        const fs::path source(location);
        const auto meta = ReadMeta(source);
        if (!meta.error.empty()) {
            return {.error = meta.error};
        }
        // Stored under the package's own name, as Eden's store keeps it.
        std::string name;
        for (const char c : meta.name.empty() ? source.stem().string() : meta.name) {
            name += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_';
        }
        const fs::path staged = dir / "staged" / (name + ".zip");
        std::error_code ec;
        fs::copy_file(source, staged, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return {.error = "The driver package could not be copied: " + ec.message()};
        }
        return {.staged = staged.string(),
                .name = meta.name,
                .version = meta.version,
                .vendor = meta.vendor,
                .library_name = meta.library,
                .min_api = meta.min_api};
    }

    std::optional<InstalledDriver> CommitDriver(const DriverPackage& package, std::string* error) override {
        const fs::path staged(package.staged);
        std::error_code ec;
        fs::rename(staged, dir / staged.filename(), ec);
        if (ec) {
            if (error) {
                *error = "The driver could not be installed: " + ec.message();
            }
            return std::nullopt;
        }
        return InstalledDriver{staged.filename().string(), package.name,
                               package.version + (package.vendor.empty() ? "" : " · " + package.vendor)};
    }

    void DiscardDriver(const DriverPackage& package) override {
        std::error_code ec;
        fs::remove(package.staged, ec);
    }

    bool SelectDriver(const std::string& id, std::string*) override {
        std::ofstream(dir / "selected") << id << '\n';
        LOG_INFO(Frontend, "harbor: test driver store: {} chosen", id.empty() ? "the phone's own" : id);
        return true;
    }

    bool RemoveDriver(const std::string& id, std::string*) override {
        std::error_code ec;
        return id != CurrentDriver() && fs::remove(dir / fs::path(id).filename(), ec);
    }

    std::optional<std::string> HttpGet(const std::string& url, std::uint64_t max_bytes,
                                       const std::atomic_bool& cancel, std::string* error) override {
        std::string body;
        int status = 0;
        const auto fail = [&](std::string why) -> std::optional<std::string> {
            if (error) {
                *error = std::move(why);
            }
            return std::nullopt;
        };
        if (!Fetch(url, {{"Accept", "application/vnd.github+json"}}, cancel, &status,
                   [&](const char* data, std::size_t length) {
                       body.append(data, length);
                       return body.size() <= max_bytes;
                   })) {
            return fail(body.size() > max_bytes ? "The answer is too large." : "The request failed.");
        }
        if (status == 403 || status == 429) {
            return fail("GitHub's request limit was reached. Try again later.");
        }
        if (status < 200 || status > 299) {
            return fail("GitHub answered " + std::to_string(status) + ".");
        }
        return body;
    }

    DownloadResult Download(const std::string& url, std::uint64_t max_bytes,
                            const std::function<void(std::uint64_t)>& progress,
                            const std::atomic_bool& cancel) override {
        const fs::path path = dir / "staged" / "download.part";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> sha(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        EVP_DigestInit_ex(sha.get(), EVP_sha256(), nullptr);
        std::uint64_t total = 0;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
        const bool ok = Fetch(url, {}, cancel, &status, [&](const char* data, std::size_t length) {
            total += length;
            if (total > max_bytes || std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            out.write(data, static_cast<std::streamsize>(length));
            EVP_DigestUpdate(sha.get(), data, length);
            progress(total);
            return true;
        });
        out.close();
        if (!ok || status < 200 || status > 299) {
            std::error_code ec;
            fs::remove(path, ec);
            if (cancel) {
                return {.error = "Cancelled."};
            }
            if (total > max_bytes) {
                return {.error = "The download is larger than the release says."};
            }
            return {.error = status ? "GitHub answered " + std::to_string(status) + "." : "The download failed."};
        }
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_length = 0;
        EVP_DigestFinal_ex(sha.get(), digest, &digest_length);
        std::string hex;
        for (unsigned int i = 0; i < digest_length; i++) {
            hex += fmt::format("{:02x}", digest[i]);
        }
        return {.path = path.string(), .sha256 = hex, .size = total};
    }

private:
    /// GET with redirects followed by hand (GitHub's downloads move to another host), the body to
    /// `receive` only for the final answer.
    bool Fetch(std::string url, httplib::Headers headers, const std::atomic_bool& cancel, int* status,
               const std::function<bool(const char*, std::size_t)>& receive) {
        headers.emplace("User-Agent", "Harbor");
        for (int hop = 0; hop < 5; hop++) {
            if (!url.starts_with("https://")) {
                return false;
            }
            const auto [base, path] = SplitUrl(url);
            httplib::Client client(base);
            client.set_connection_timeout(20);
            client.set_read_timeout(60);
            OpenPak::Platform::ApplySystemCa(client);
            std::string location;
            const auto result = client.Get(
                path, headers,
                [&](const httplib::Response& response) {
                    *status = response.status;
                    if (response.status >= 300 && response.status < 400) {
                        location = response.get_header_value("Location");
                    }
                    return !cancel;
                },
                [&](const char* data, std::size_t length) {
                    if (cancel) {
                        return false;
                    }
                    return location.empty() ? receive(data, length) : true;
                });
            if (!result) {
                return false;
            }
            if (location.empty()) {
                return true;
            }
            url = location;
        }
        return false;
    }

    fs::path dir;
    DeviceInfo device;
};

} // namespace

HarborSetup::Host* TestDriverStore() {
    static const std::unique_ptr<TestStore> store = [] {
        const char* dir = std::getenv("HARBOR_TEST_DRIVER_STORE");
        return dir && *dir ? std::make_unique<TestStore>(dir) : nullptr;
    }();
    return store.get();
}

} // namespace HarborDesktop
