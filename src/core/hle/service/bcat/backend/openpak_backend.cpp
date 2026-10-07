// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/backend/openpak_backend.h"

#include <functional>
#include <thread>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/bcat/backend/openpak_bcat.h"
#include "core/hle/service/bcat/bcat_result.h"
#include "core/openpak/bcat.h"

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::BCAT {
namespace {

using OpenPak::Bcat::Outcome;

// A cache OpenPak filled carries this marker, so a title whose data is no longer published gets its
// cache emptied, and one OpenPak never touched is left alone.
constexpr std::string_view Marker = ".openpak";

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
void WriteDelivery(const FileSys::VirtualDir& root, const OpenPak::Bcat::Result& dataset,
                   u64 title_id) {
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

// The per-title downloads of this run. The state is shared with the downloading threads only by a weak
// reference, so a thread that outlives the backend finds it gone and does not touch the session.
struct OpenPakBcatBackend::Deliveries {
    struct Job {
        bool started{};
        bool finished{};
        std::vector<std::function<void()>> waiting;
    };

    explicit Deliveries(Core::System& system) : worker{system, "bcat:OpenPak"} {}

    // Progress is reported from the worker's signalling thread, the one host thread allowed to touch
    // this session's kernel objects.
    OpenPak::AsyncWorker worker;
    std::mutex mutex;
    std::map<u64, Job> jobs;

    // Start the title's download unless this run has one, and have `on_done` called (on the
    // signalling thread) when it ends, at once if it already has. Titles delivered to in this run are
    // not fetched again, unless the service could not be asked.
    static void Request(const std::shared_ptr<Deliveries>& self, u64 title_id,
                        FileSys::VirtualDir root, std::function<void()> on_done) {
        bool start = false;
        bool run_now = false;
        {
            std::scoped_lock lock{self->mutex};
            Job& job = self->jobs[title_id];
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
            self->worker.Post(std::move(on_done));
            return;
        }
        if (!start) {
            return;
        }

        // The fetch blocks (website and file downloads): a thread of its own.
        std::thread([weak = std::weak_ptr<Deliveries>{self}, title_id, root = std::move(root)] {
            const auto dataset = OpenPak::Bcat::Fetch(title_id);
            WriteDelivery(root, dataset, title_id);

            LOG_INFO(Service_BCAT, "{}",
                     dataset.outcome == Outcome::Dataset
                         ? "[OpenPak] Delivered " + std::to_string(dataset.files.size()) +
                               " BCAT file(s)"
                         : (dataset.outcome == Outcome::None ? "[OpenPak] No BCAT data is published"
                                                             : "[OpenPak] BCAT data not reachable"));

            const auto self = weak.lock();
            if (!self) {
                return;
            }

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
                self->worker.Post(std::move(callback));
            }
        }).detach();
    }

    void Reset(u64 title_id) {
        std::scoped_lock lock{mutex};
        jobs.erase(title_id);
    }
};

OpenPakBcatBackend::OpenPakBcatBackend(Core::System& system, DirectoryGetter getter)
    : NullBcatBackend(std::move(getter)), deliveries{std::make_shared<Deliveries>(system)} {}

OpenPakBcatBackend::~OpenPakBcatBackend() = default;

bool OpenPakBcatBackend::Synchronize(Kernel::KernelCore& kernel, TitleIDVersion title,
                                     ProgressServiceBackend& progress) {
    progress.StartConnecting(kernel);

    // The progress object may be gone when the download ends; the callback asks first.
    const auto liveness = progress.GetLiveness();
    Deliveries::Request(deliveries, title.title_id, dir_getter(title.title_id),
                        [&kernel, liveness] {
                            std::scoped_lock lock{liveness->mutex};
                            if (liveness->target != nullptr) {
                                liveness->target->FinishDownload(kernel, ResultSuccess);
                            }
                        });
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
    deliveries->Reset(title_id);
    return true;
}

void OpenPakBcatBackend::Prepare(Kernel::KernelCore& kernel, u64 title_id) {
    // System modules ask the service for a cache too; only applications have data published.
    constexpr u64 FirstApplicationId = 0x0100000000010000ULL;
    if (title_id < FirstApplicationId) {
        return;
    }
    Deliveries::Request(deliveries, title_id, dir_getter(title_id), {});
}

} // namespace Service::BCAT
