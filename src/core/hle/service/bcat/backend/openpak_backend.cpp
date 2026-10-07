// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/backend/openpak_backend.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/bcat/backend/openpak_bcat.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/hle/service/bcat/openpak_fetch.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::BCAT {
namespace {

namespace fs = std::filesystem;

// A cache OpenPak filled carries this marker, so a title whose data is no longer published gets its
// cache emptied, and one OpenPak never touched is left alone.
constexpr std::string_view Marker = ".openpak";

enum class Outcome {
    // The service holds a dataset for this title and it is in its window.
    Dataset,
    // The service answered that there is nothing to deliver.
    None,
    // The service could not be asked; the cached copy, if any, stands.
    Unreachable,
};

struct Dataset {
    Outcome outcome{Outcome::Unreachable};
    std::vector<BcatFile> files;
};

std::string TitleHex(u64 title_id) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(title_id));
    return hex;
}

fs::path CacheDirectory(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "openpak" / "bcat" /
           TitleHex(title_id);
}

std::optional<std::string> ReadText(const fs::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void Forget(u64 title_id) {
    std::error_code ec;
    fs::remove_all(CacheDirectory(title_id), ec);
}

void Store(u64 title_id, const std::string& manifest, const std::vector<BcatFile>& files) {
    const auto directory = CacheDirectory(title_id);
    Forget(title_id);

    std::error_code ec;
    for (const auto& file : files) {
        fs::create_directories(directory / "files" / file.directory, ec);
        std::ofstream stream{directory / "files" / file.directory / file.name,
                             std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(file.data.data()),
                     static_cast<std::streamsize>(file.data.size()));
    }
    fs::create_directories(directory, ec);
    std::ofstream{directory / "manifest.json", std::ios::trunc} << manifest;
}

// The last dataset fetched, while it is inside its validity window.
std::optional<std::vector<BcatFile>> Cached(u64 title_id) {
    const auto directory = CacheDirectory(title_id);

    try {
        const auto manifest = ReadText(directory / "manifest.json");
        if (!manifest || !BcatInWindow(*manifest, std::chrono::system_clock::now())) {
            return std::nullopt;
        }

        std::vector<BcatFile> files;
        for (const auto& entry : BcatManifestEntries(*manifest)) {
            BcatFile file;
            if (!SplitBcatPath(entry.path, file.directory, file.name)) {
                continue;
            }
            const auto data = ReadText(directory / "files" / file.directory / file.name);
            if (!data) {
                return std::nullopt;
            }
            file.data.assign(data->begin(), data->end());
            files.push_back(std::move(file));
        }
        return files;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// What this title should find in its delivery cache: the service's dataset, or the cached one when
// the service cannot be reached. A dataset is taken whole or not at all: one file failing its sha256
// leaves the last good copy in place.
Dataset FetchDataset(u64 title_id) {
    const auto unreachable = [&] {
        if (auto cached = Cached(title_id)) {
            return Dataset{Outcome::Dataset, std::move(*cached)};
        }
        return Dataset{Outcome::Unreachable, {}};
    };

    const auto fetcher = GetOpenPakFetcher();
    if (!fetcher) {
        return unreachable();
    }

    try {
        const auto response =
            fetcher->Get(FetchOrigin::Website, "/api/emulator/v1/bcat/titles/" + TitleHex(title_id));
        if (!response.reached) {
            return unreachable();
        }
        if (response.status == 404) {
            Forget(title_id);
            return {Outcome::None, {}};
        }
        if (response.status != 200) {
            LOG_INFO(Service_BCAT, "[OpenPak] BCAT data for {:016x} not fetched ({})", title_id,
                     response.status);
            return unreachable();
        }

        const std::string manifest(response.body.begin(), response.body.end());
        if (!BcatInWindow(manifest, std::chrono::system_clock::now())) {
            Forget(title_id);
            return {Outcome::None, {}};
        }

        Dataset dataset{Outcome::Dataset, {}};
        for (const auto& entry : BcatManifestEntries(manifest)) {
            BcatFile file;
            if (!SplitBcatPath(entry.path, file.directory, file.name)) {
                LOG_WARNING(Service_BCAT,
                            "[OpenPak] {:016x}: a manifest path is not <directory>/<file> within 31 "
                            "characters each; not delivered",
                            title_id);
                continue;
            }

            // Harbor reaches OpenPak's website only: an absolute URL is reduced to its path.
            std::string path = entry.url;
            if (const size_t scheme = path.find("://"); scheme != std::string::npos) {
                const size_t slash = path.find('/', scheme + 3);
                path = slash == std::string::npos ? "/" : path.substr(slash);
            }

            const auto data = fetcher->Get(FetchOrigin::Website, path);
            if (!data.reached || data.status != 200) {
                throw std::runtime_error("a file was not served");
            }
            std::string expected = entry.sha256;
            std::transform(expected.begin(), expected.end(), expected.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            if (Sha256Hex(data.body) != expected) {
                throw std::runtime_error("a file does not match its sha256");
            }

            file.data = data.body;
            dataset.files.push_back(std::move(file));
        }

        Store(title_id, manifest, dataset.files);
        return dataset;
    } catch (const std::exception& e) {
        LOG_INFO(Service_BCAT, "[OpenPak] BCAT data for {:016x} not fetched: {}", title_id, e.what());
        return unreachable();
    }
}

void EmptyCache(const FileSys::VirtualDir& root) {
    for (const auto& directory : root->GetSubdirectories()) {
        root->DeleteSubdirectoryRecursive(directory->GetName());
    }
    root->DeleteFile(Marker);
}

// Replace what the delivery cache holds with the dataset's files. With none, a cache OpenPak never
// wrote to is left alone; one it did write (it has the marker) is emptied, since the service no
// longer publishes what it holds. The layout is <directory>/<file>, as IDeliveryCacheStorageService
// reads it.
void WriteDelivery(const FileSys::VirtualDir& root, const Dataset& dataset, u64 title_id) {
    if (root == nullptr) {
        return;
    }
    if (dataset.outcome == Outcome::Unreachable ||
        (dataset.files.empty() && root->GetFile(Marker) == nullptr)) {
        return;
    }

    EmptyCache(root);

    for (const auto& group : GroupBcatFiles(dataset.files)) {
        const auto directory = root->CreateSubdirectory(group.directory);
        for (const auto& file : group.files) {
            const auto out = directory != nullptr ? directory->CreateFile(file.name) : nullptr;
            if (out == nullptr || !out->Resize(file.data.size()) ||
                out->WriteBytes(file.data) != file.data.size()) {
                LOG_WARNING(Service_BCAT, "[OpenPak] Could not write a BCAT file for {:016x}", title_id);
            }
        }
    }

    if (!dataset.files.empty()) {
        root->CreateFile(Marker);
    }
}

} // namespace

// The per-title downloads of this run. A worker thread keeps the state alive past the backend.
struct OpenPakBcatBackend::Deliveries {
    struct Job {
        bool started{};
        bool finished{};
        std::vector<std::function<void()>> waiting;
    };

    std::mutex mutex;
    std::map<u64, Job> jobs;

    // Start the title's download unless this run has one, and have `on_done` called when it ends
    // (at once if it already has). Titles delivered to in this run are not fetched again, unless the
    // service could not be asked.
    void Request(u64 title_id, FileSys::VirtualDir root, std::function<void()> on_done,
                 const std::shared_ptr<Deliveries>& self) {
        bool start = false;
        bool run_now = false;
        {
            std::scoped_lock lock{mutex};
            Job& job = jobs[title_id];
            if (!job.started) {
                job.started = true;
                job.finished = false;
                start = true;
            }
            if (on_done) {
                if (job.finished) {
                    run_now = true;
                } else {
                    job.waiting.push_back(std::move(on_done));
                }
            }
        }

        if (run_now) {
            on_done();
            return;
        }
        if (!start) {
            return;
        }

        std::thread([self, title_id, root = std::move(root)] {
            const Dataset dataset = FetchDataset(title_id);
            WriteDelivery(root, dataset, title_id);

            LOG_INFO(Service_BCAT, "{}",
                     dataset.outcome == Outcome::Dataset
                         ? "[OpenPak] Delivered " + std::to_string(dataset.files.size()) +
                               " BCAT file(s)"
                         : (dataset.outcome == Outcome::None ? "[OpenPak] No BCAT data is published"
                                                             : "[OpenPak] BCAT data not reachable"));

            std::vector<std::function<void()>> waiting;
            {
                std::scoped_lock lock{self->mutex};
                Job& job = self->jobs[title_id];
                job.finished = true;
                // Not asked: the next call tries again.
                if (dataset.outcome == Outcome::Unreachable) {
                    job.started = false;
                }
                waiting = std::move(job.waiting);
                job.waiting.clear();
            }
            for (auto& callback : waiting) {
                callback();
            }
        }).detach();
    }

    void Reset(u64 title_id) {
        std::scoped_lock lock{mutex};
        jobs.erase(title_id);
    }
};

OpenPakBcatBackend::OpenPakBcatBackend(DirectoryGetter getter)
    : NullBcatBackend(std::move(getter)), deliveries{std::make_shared<Deliveries>()} {}

OpenPakBcatBackend::~OpenPakBcatBackend() = default;

bool OpenPakBcatBackend::Synchronize(Kernel::KernelCore& kernel, TitleIDVersion title,
                                     ProgressServiceBackend& progress) {
    progress.StartConnecting(kernel);

    // The progress object may be gone when the download ends; the worker asks first.
    const auto liveness = progress.GetLiveness();
    deliveries->Request(
        title.title_id, dir_getter(title.title_id),
        [&kernel, liveness] {
            std::scoped_lock lock{liveness->mutex};
            if (liveness->target != nullptr) {
                liveness->target->FinishDownload(kernel, ResultSuccess);
            }
        },
        deliveries);
    return true;
}

bool OpenPakBcatBackend::SynchronizeDirectory(Kernel::KernelCore& kernel, TitleIDVersion title,
                                              std::string name, ProgressServiceBackend& progress) {
    return Synchronize(kernel, title, progress);
}

bool OpenPakBcatBackend::Clear(Kernel::KernelCore& kernel, u64 title_id) {
    LOG_DEBUG(Service_BCAT, "called, title_id={:016X}", title_id);

    if (const auto root = dir_getter(title_id)) {
        EmptyCache(root);
    }
    Forget(title_id);
    deliveries->Reset(title_id);
    return true;
}

void OpenPakBcatBackend::Prepare(Kernel::KernelCore& kernel, u64 title_id) {
    // System modules ask the service for a cache too; only applications have data published.
    constexpr u64 FirstApplicationId = 0x0100000000010000ULL;
    if (title_id < FirstApplicationId) {
        return;
    }
    deliveries->Request(title_id, dir_getter(title_id), {}, deliveries);
}

} // namespace Service::BCAT
