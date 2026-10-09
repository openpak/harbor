// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// erpt as firmware 23.0.1's erpt sysmodule (010000000000002B) answers it: contexts submitted by
// category, reports built from them and kept in erpt's system save on the NAND, and the erpt:r
// sessions System Settings > Support > Error History reads them through. Layouts, checks and
// limits come from firmware 23.0.1 (erpt, qlaunch, the error applet and am); reports never leave
// this console (no upload). Private fields (serial numbers, addresses, SSIDs, URLs) are never
// stored, and logs name only the error code.

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/redact.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/erpt/erpt.h"
#include "core/hle/service/erpt/report_builder.h"
#include "core/hle/service/erpt/report_store.h"
#include "core/hle/service/glue/time/static.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/psc/time/system_clock.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"

namespace Service::ERPT {

namespace {

constexpr Result ResultInvalidArgument{ErrorModule::ERPT, 7};
constexpr Result ResultNotFound{ErrorModule::ERPT, 8};
constexpr Result ResultFieldCategoryMismatch{ErrorModule::ERPT, 9};
constexpr Result ResultFieldTypeMismatch{ErrorModule::ERPT, 10};
constexpr Result ResultAlreadyExists{ErrorModule::ERPT, 11};
constexpr Result ResultRequiredContextMissing{ErrorModule::ERPT, 14};
constexpr Result ResultRequiredFieldMissing{ErrorModule::ERPT, 15};
constexpr Result ResultThrottled{ErrorModule::ERPT, 17};
constexpr Result ResultArrayFieldTooLarge{ErrorModule::ERPT, 18};

constexpr u64 ErptSaveId = 0x80000000000000D1;
constexpr std::size_t ReportListSize = 8 + ReportStore::MaxReports * sizeof(ReportInfo);
constexpr auto InvisibleThrottleWindow = std::chrono::seconds{3};

// Fields erpt and its callers set (firmware 23.0.1 ids).
constexpr u32 FieldErrorCode = 10;
constexpr u32 FieldOccurrenceTimestamp = 12;
constexpr u32 FieldReportIdentifier = 13;
constexpr u32 FieldAbortFlag = 116;
constexpr u32 FieldReportVisibilityFlag = 117;
constexpr u32 FieldOccurrenceTimestampNet = 119;
constexpr u32 FieldCrashReportFlag = 751;

Result ToResult(ErptError error) {
    switch (error) {
    case ErptError::None:
        return ResultSuccess;
    case ErptError::NotFound:
        return ResultNotFound;
    case ErptError::FieldCategoryMismatch:
        return ResultFieldCategoryMismatch;
    case ErptError::FieldTypeMismatch:
        return ResultFieldTypeMismatch;
    case ErptError::RequiredContextMissing:
        return ResultRequiredContextMissing;
    case ErptError::RequiredFieldMissing:
        return ResultRequiredFieldMissing;
    case ErptError::ArrayFieldTooLarge:
        return ResultArrayFieldTooLarge;
    case ErptError::InvalidArgument:
    default:
        return ResultInvalidArgument;
    }
}

struct ReportIdRaw {
    ReportId id;
};
static_assert(sizeof(ReportIdRaw) == 0x14);

// nn::erpt::StorageUsageStatistics.
struct StorageUsageStatistics {
    std::array<u8, 0x10> journal_uuid;
    u32 used_storage_size;
    INSERT_PADDING_BYTES_NOINIT(4);
    s64 max_report_size;
    std::array<u32, 2> report_count;
    std::array<u32, 2> transmitted_count;
    std::array<u32, 2> untransmitted_count;
};
static_assert(sizeof(StorageUsageStatistics) == 0x38);

// The erpt sysmodule's state: the stored context per category, the report store and the
// IManager events to signal when the store changes.
class ErptManager {
public:
    void Initialize(Core::System& system_) {
        std::scoped_lock lk{mutex};
        system = &system_;
        contexts.clear();
        last_invisible.clear();
        listeners.clear();
        const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
                         fmt::format("system/save/{:016X}", ErptSaveId);
        store = std::make_unique<ReportStore>(dir);
        store->Load();
        LOG_INFO(Service_ERPT, "erpt: {} reports in the store",
                 store->List(ReportType_Any).size());
    }

    ReportStore& Store() {
        return *store;
    }

    /// One slot per category; a context with no fields clears its category.
    void Submit(Context context) {
        std::scoped_lock lk{mutex};
        if (context.fields.empty()) {
            contexts.erase(context.category);
        } else {
            contexts[context.category] = std::move(context);
        }
    }

    Result CreateReport(u32 type, Result result, Context error_info,
                        const std::vector<Context>& additional,
                        std::optional<ReportId> requested_id) {
        R_UNLESS(type <= ReportType_Invisible, ResultInvalidArgument);
        R_TRY(ToResult(CheckReportContext(error_info)));
        const auto error_code = std::get<std::string>(error_info.Get(FieldErrorCode)->value);

        // Invisible reports of one error code are kept at most once per window, unless they are
        // crash reports.
        const bool crash = [&] {
            const auto* flag = error_info.Get(FieldCrashReportFlag);
            return flag != nullptr && std::get<bool>(flag->value);
        }();
        if (type == ReportType_Invisible && !crash) {
            std::scoped_lock lk{mutex};
            const auto now = std::chrono::steady_clock::now();
            const auto it = last_invisible.find(error_code);
            if (it != last_invisible.end() && now - it->second < InvisibleThrottleWindow) {
                R_THROW(ResultThrottled);
            }
            last_invisible[error_code] = now;
        }

        if (error_info.Get(FieldAbortFlag) == nullptr) {
            error_info.Set(FieldAbortFlag, false);
        }

        ReportInfo info{};
        info.type = type;
        if (requested_id) {
            info.id = *requested_id;
        } else {
            const auto uuid = Common::UUID::MakeRandom();
            std::memcpy(info.id.data(), uuid.uuid.data(), 16);
        }
        info.timestamp_user = UserClockNow();
        info.timestamp_network = NetworkClockNow();

        Context automatic;
        automatic.category = Category_ErrorInfoAuto;
        automatic.Set(FieldReportIdentifier, ReportIdentifierString(info.id));
        automatic.Set(FieldOccurrenceTimestamp, std::int64_t{info.timestamp_user});
        automatic.Set(FieldOccurrenceTimestampNet, std::int64_t{info.timestamp_network});
        automatic.Set(FieldReportVisibilityFlag, type == ReportType_Visible);

        std::vector<std::uint8_t> body;
        {
            std::scoped_lock lk{mutex};
            std::vector<const Context*> parts{&automatic, &error_info};
            for (const auto& context : additional) {
                parts.push_back(&context);
            }
            for (const auto& [category, context] : contexts) {
                if (category != Category_ErrorInfo && category != Category_ErrorInfoAuto) {
                    parts.push_back(&context);
                }
            }
            body = BuildReportBody(parts);
        }

        R_UNLESS(store->Add(info, body), ResultAlreadyExists);
        LOG_INFO(Service_ERPT, "erpt: {} report {} stored (result {:#x}, {} bytes)",
                 type == ReportType_Visible ? "visible" : "invisible", error_code, result.raw,
                 body.size());
        NotifyListeners();
        R_SUCCEED();
    }

    u64 AddListener(std::function<void()> listener) {
        std::scoped_lock lk{mutex};
        listeners.emplace(++next_listener, std::move(listener));
        return next_listener;
    }

    void RemoveListener(u64 id) {
        std::scoped_lock lk{mutex};
        listeners.erase(id);
    }

    void NotifyListeners() {
        std::vector<std::function<void()>> copy;
        {
            std::scoped_lock lk{mutex};
            for (const auto& [_, listener] : listeners) {
                copy.push_back(listener);
            }
        }
        for (const auto& listener : copy) {
            listener();
        }
    }

private:
    std::shared_ptr<PSC::Time::SystemClock> Clock(bool network) {
        if (system == nullptr) {
            return nullptr;
        }
        auto static_service =
            system->ServiceManager().GetService<Glue::Time::StaticService>("time:u", true);
        if (!static_service) {
            return nullptr;
        }
        std::shared_ptr<PSC::Time::SystemClock> clock;
        const auto result = network ? static_service->GetStandardNetworkSystemClock(&clock)
                                    : static_service->GetStandardUserSystemClock(&clock);
        return R_SUCCEEDED(result) ? clock : nullptr;
    }

    s64 UserClockNow() {
        s64 time{};
        if (const auto clock = Clock(false); !clock || R_FAILED(clock->GetCurrentTime(&time))) {
            time = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        }
        return time;
    }

    // 0 when the network clock has no time, as erpt writes it.
    s64 NetworkClockNow() {
        s64 time{};
        if (const auto clock = Clock(true); !clock || R_FAILED(clock->GetCurrentTime(&time))) {
            return 0;
        }
        return time;
    }

    std::mutex mutex;
    Core::System* system{};
    std::unique_ptr<ReportStore> store;
    std::map<u32, Context> contexts;
    std::map<std::string, std::chrono::steady_clock::time_point> last_invisible;
    std::map<u64, std::function<void()>> listeners;
    u64 next_listener{};
};

ErptManager& GetManager() {
    static ErptManager manager;
    return manager;
}

Result ParseCallerContext(std::span<const u8> entry, std::span<const u8> data,
                          std::span<const u8> meta, Context& out) {
    R_UNLESS(entry.size() == ContextEntrySize, ResultInvalidArgument);
    R_UNLESS(data.size() <= MaxArrayBufferSize, ResultInvalidArgument);
    R_UNLESS((meta.size() & ~std::size_t{0x20}) == 0, ResultInvalidArgument);
    R_RETURN(ToResult(ParseContext(entry, data, out)));
}

// A MultipleCategoryContext: CategoryEntry {u32 category, u32 field count, u32 array bytes} per
// category, the FieldEntry records and the array bytes of every category one after another.
// The array start of a field counts from its own category's bytes (not established).
Result ParseMultipleContexts(std::span<const u8> categories, std::span<const u8> fields,
                            std::span<const u8> arrays, std::vector<Context>& out) {
    std::size_t field_at = 0;
    std::size_t array_at = 0;
    for (std::size_t i = 0; i + 12 <= categories.size(); i += 12) {
        u32 category{}, field_count{}, array_size{};
        std::memcpy(&category, categories.data() + i, 4);
        std::memcpy(&field_count, categories.data() + i + 4, 4);
        std::memcpy(&array_size, categories.data() + i + 8, 4);
        R_UNLESS(field_count <= MaxFieldsPerContext && array_size <= MaxArrayBufferSize,
                 ResultInvalidArgument);
        R_UNLESS(field_at + field_count * 0x10 <= fields.size() &&
                     array_at + array_size <= arrays.size(),
                 ResultInvalidArgument);
        Context context;
        R_TRY(ToResult(ParseFields(category, fields.subspan(field_at, field_count * 0x10),
                                   field_count, arrays.subspan(array_at, array_size), context)));
        field_at += field_count * 0x10;
        array_at += array_size;
        out.push_back(std::move(context));
    }
    R_SUCCEED();
}

} // namespace

Context MakeApplicationInfo(Core::System& system, u64 program_id, s32 applet_id) {
    Context info;
    info.category = Category_ApplicationInfo;
    info.Set(43, fmt::format("{:016x}", program_id)); // ApplicationID
    info.Set(682, std::int64_t{applet_id});           // AppletId
    std::vector<u8> nacp_data;
    if (program_id != 0 &&
        R_SUCCEEDED(system.GetARPManager().GetControlProperty(&nacp_data, program_id)) &&
        nacp_data.size() >= sizeof(FileSys::RawNACP)) {
        const FileSys::NACP nacp{std::make_shared<FileSys::VectorVfsFile>(std::move(nacp_data))};
        info.Set(44, nacp.GetApplicationName()); // ApplicationTitle
        info.Set(45, nacp.GetVersionString());   // ApplicationVersion
    }
    return info;
}

bool CreateHostReport(Core::System& system, u32 report_type, const std::string& error_code,
                      const Context* extra_error_info, const std::vector<Context>& additional) {
    Context error_info;
    if (extra_error_info != nullptr) {
        error_info = *extra_error_info;
    }
    error_info.category = Category_ErrorInfo;
    error_info.Set(FieldErrorCode, error_code);
    return R_SUCCEEDED(
        GetManager().CreateReport(report_type, ResultSuccess, error_info, additional, {}));
}

class ErrorReportContext final : public ServiceFramework<ErrorReportContext> {
public:
    explicit ErrorReportContext(Core::System& system_) : ServiceFramework{system_, "erpt:c"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&ErrorReportContext::SubmitContext>, "SubmitContext"},
            {1, D<&ErrorReportContext::CreateReportV0>, "CreateReportV0"},
            {2, D<&ErrorReportContext::SetInitialLaunchSettingsCompletionTime>, "SetInitialLaunchSettingsCompletionTime"},
            {3, D<&ErrorReportContext::ClearInitialLaunchSettingsCompletionTime>, "ClearInitialLaunchSettingsCompletionTime"},
            {5, D<&ErrorReportContext::CreateReportWithAdditionalContext>, "CreateReportWithAdditionalContext"},
            {6, D<&ErrorReportContext::SubmitMultipleContext>, "SubmitMultipleContext"},
            {7, D<&ErrorReportContext::RegisterRunningApplicationInfo>, "RegisterRunningApplicationInfo"},
            {8, D<&ErrorReportContext::UnregisterRunningApplicationInfo>, "UnregisterRunningApplicationInfo"},
            {10, D<&ErrorReportContext::CreateReportWithAttachments>, "CreateReportWithAttachments"},
            {11, D<&ErrorReportContext::CreateReportV1>, "CreateReportV1"},
            {12, D<&ErrorReportContext::CreateReport>, "CreateReport"},
            {14, D<&ErrorReportContext::CreateReportWithSpecifiedReportId>, "CreateReportWithSpecifiedReportId"},
            {15, D<&ErrorReportContext::SubmitAttachment>, "SubmitAttachment"},
            {20, D<&ErrorReportContext::RegisterRunningApplet>, "RegisterRunningApplet"},
            {21, D<&ErrorReportContext::UnregisterRunningApplet>, "UnregisterRunningApplet"},
            {22, D<&ErrorReportContext::UpdateAppletSuspendedDuration>, "UpdateAppletSuspendedDuration"},
            {30, D<&ErrorReportContext::InvalidateForcedShutdownDetection>, "InvalidateForcedShutdownDetection"},
            {40, D<&ErrorReportContext::WaitForReportCreation>, "WaitForReportCreation"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    using InA = InBuffer<BufferAttr_HipcMapAlias>;

    Result SubmitContext(InA context_entry, InA field_list) {
        Context context;
        R_UNLESS(context_entry.size() == ContextEntrySize &&
                     field_list.size() <= MaxArrayBufferSize,
                 ResultInvalidArgument);
        R_TRY(ToResult(ParseContext(context_entry, field_list, context)));
        LOG_DEBUG(Service_ERPT, "called, category={} fields={}", context.category,
                  context.fields.size());
        GetManager().Submit(std::move(context));
        R_SUCCEED();
    }

    Result CreateReportV0(u32 report_type, InA context_entry, InA data, InA meta) {
        R_RETURN(Create(report_type, ResultSuccess, context_entry, data, meta, {}, {}));
    }

    Result CreateReportV1(u32 report_type, Result result, InA context_entry, InA data, InA meta) {
        R_RETURN(Create(report_type, result, context_entry, data, meta, {}, {}));
    }

    Result CreateReport(u32 report_type, Result result, u32 flags, InA context_entry, InA data,
                        InA meta) {
        R_RETURN(Create(report_type, result, context_entry, data, meta, {}, {}));
    }

    Result CreateReportWithAttachments(u32 report_type, Result result, u32 flags,
                                       InA context_entry, InA data, InA attachment_ids) {
        // Attachments are not kept: the report is stored without them.
        R_UNLESS(attachment_ids.size() / 0x14 <= 10, ResultInvalidArgument);
        R_RETURN(Create(report_type, result, context_entry, data, {}, {}, {}));
    }

    // Raw data in alignment order: the 20-byte id, then type, Result and flags.
    Result CreateReportWithSpecifiedReportId(ReportIdRaw report_id, u32 report_type, Result result,
                                             u32 flags, InA context_entry, InA data, InA meta,
                                             InA attachment_ids) {
        R_UNLESS(attachment_ids.size() / 0x14 <= 10, ResultInvalidArgument);
        R_RETURN(Create(report_type, result, context_entry, data, meta, {}, report_id.id));
    }

    Result CreateReportWithAdditionalContext(u32 report_type, Result result, u32 flags,
                                             InA context_entry, InA data, InA meta,
                                             InA category_entries, InA field_entries,
                                             InA array_data) {
        std::vector<Context> additional;
        R_TRY(ParseMultipleContexts(category_entries, field_entries, array_data, additional));
        R_RETURN(Create(report_type, result, context_entry, data, meta, additional, {}));
    }

    Result SubmitMultipleContext(InA category_entries, InA field_entries, InA array_data) {
        std::vector<Context> contexts;
        R_TRY(ParseMultipleContexts(category_entries, field_entries, array_data, contexts));
        for (auto& context : contexts) {
            GetManager().Submit(std::move(context));
        }
        R_SUCCEED();
    }

    Result SubmitAttachment(Out<ReportIdRaw> out_attachment_id, u32 flags, InA name, InA data) {
        // Attachments (syslogs, dumps) are not kept; the caller gets an id for its report.
        const auto uuid = Common::UUID::MakeRandom();
        *out_attachment_id = {};
        std::memcpy(out_attachment_id->id.data(), uuid.uuid.data(), 16);
        LOG_DEBUG(Service_ERPT, "attachment of {} bytes not kept", data.size());
        R_SUCCEED();
    }

    // The power-on, launch and applet times feed fields of the automatic context that Error
    // History never shows; they are accepted and not kept.
    Result SetInitialLaunchSettingsCompletionTime(std::array<u8, 0x20> time_point) {
        LOG_DEBUG(Service_ERPT, "called");
        R_SUCCEED();
    }

    Result ClearInitialLaunchSettingsCompletionTime() {
        LOG_DEBUG(Service_ERPT, "called");
        R_SUCCEED();
    }

    Result RegisterRunningApplicationInfo(u64 application_id, u64 program_id) {
        LOG_DEBUG(Service_ERPT, "called, application_id={:016X}", application_id);
        R_SUCCEED();
    }

    Result UnregisterRunningApplicationInfo() {
        LOG_DEBUG(Service_ERPT, "called");
        R_SUCCEED();
    }

    Result RegisterRunningApplet(u64 program_id) {
        LOG_DEBUG(Service_ERPT, "called, program_id={:016X}", program_id);
        R_SUCCEED();
    }

    Result UnregisterRunningApplet(u64 program_id) {
        LOG_DEBUG(Service_ERPT, "called, program_id={:016X}", program_id);
        R_SUCCEED();
    }

    Result UpdateAppletSuspendedDuration(u64 program_id, s64 duration) {
        LOG_DEBUG(Service_ERPT, "called, program_id={:016X}", program_id);
        R_SUCCEED();
    }

    Result InvalidateForcedShutdownDetection() {
        LOG_DEBUG(Service_ERPT, "called");
        R_SUCCEED();
    }

    Result WaitForReportCreation() {
        // Reports are written before CreateReport answers.
        LOG_DEBUG(Service_ERPT, "called");
        R_SUCCEED();
    }

    Result Create(u32 report_type, Result result, std::span<const u8> context_entry,
                  std::span<const u8> data, std::span<const u8> meta,
                  const std::vector<Context>& additional, std::optional<ReportId> report_id) {
        Context error_info;
        R_TRY(ParseCallerContext(context_entry, data, meta, error_info));
        const auto result_of_create =
            GetManager().CreateReport(report_type, result, std::move(error_info), additional,
                                      report_id);
        if (R_FAILED(result_of_create)) {
            LOG_WARNING(Service_ERPT, "report not stored: {:#x}", result_of_create.raw);
        }
        R_RETURN(result_of_create);
    }
};

class IReport final : public ServiceFramework<IReport> {
public:
    explicit IReport(Core::System& system_) : ServiceFramework{system_, "IReport"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IReport::Open>, "Open"},
            {1, D<&IReport::Read>, "Read"},
            {2, D<&IReport::SetFlags>, "SetFlags"},
            {3, D<&IReport::GetFlags>, "GetFlags"},
            {4, D<&IReport::Close>, "Close"},
            {5, D<&IReport::GetSize>, "GetSize"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    Result Open(ReportIdRaw report_id) {
        R_UNLESS(GetManager().Store().Find(report_id.id).has_value(), ResultNotFound);
        id = report_id.id;
        position = 0;
        R_SUCCEED();
    }

    Result Read(Out<u32> out_read, OutBuffer<BufferAttr_HipcMapAlias> out_data) {
        R_UNLESS(id.has_value(), ResultInvalidArgument);
        const auto body = GetManager().Store().ReadBody(*id);
        R_UNLESS(body.has_value(), ResultNotFound);
        const std::size_t left = body->size() > position ? body->size() - position : 0;
        const std::size_t count = std::min(left, out_data.size());
        std::memcpy(out_data.data(), body->data() + position, count);
        position += count;
        *out_read = static_cast<u32>(count);
        R_SUCCEED();
    }

    Result SetFlags(u32 flags) {
        R_UNLESS(id.has_value(), ResultInvalidArgument);
        R_UNLESS(GetManager().Store().SetFlags(*id, flags), ResultNotFound);
        R_SUCCEED();
    }

    Result GetFlags(Out<u32> out_flags) {
        R_UNLESS(id.has_value(), ResultInvalidArgument);
        const auto info = GetManager().Store().Find(*id);
        R_UNLESS(info.has_value(), ResultNotFound);
        *out_flags = info->flags | info->flags_set;
        R_SUCCEED();
    }

    Result Close() {
        id.reset();
        position = 0;
        R_SUCCEED();
    }

    Result GetSize(Out<s64> out_size) {
        R_UNLESS(id.has_value(), ResultInvalidArgument);
        const auto info = GetManager().Store().Find(*id);
        R_UNLESS(info.has_value(), ResultNotFound);
        *out_size = info->size;
        R_SUCCEED();
    }

    std::optional<ReportId> id;
    std::size_t position{};
};

class IManager final : public ServiceFramework<IManager> {
public:
    explicit IManager(Core::System& system_)
        : ServiceFramework{system_, "IManager"}, service_context{system_, "erpt:IManager"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IManager::GetReportList>, "GetReportList"},
            {1, D<&IManager::GetEvent>, "GetEvent"},
            {2, D<&IManager::CleanupReports>, "CleanupReports"},
            {3, D<&IManager::DeleteReport>, "DeleteReport"},
            {4, D<&IManager::GetStorageUsageStatistics>, "GetStorageUsageStatistics"},
            {6, D<&IManager::GetAttachmentList>, "GetAttachmentList"},
            {7, D<&IManager::PopNotifiableErrorCodes>, "PopNotifiableErrorCodes"},
            {10, D<&IManager::GetReportSizeMax>, "GetReportSizeMax"},
        };
        // clang-format on
        RegisterHandlers(functions);

        event = service_context.CreateEvent("erpt:IManager:Event");
        listener = GetManager().AddListener([this] { event->Signal(system.Kernel()); });
    }

    ~IManager() override {
        GetManager().RemoveListener(listener);
        service_context.CloseEvent(event);
    }

private:
    Result GetReportList(OutBuffer<BufferAttr_HipcMapAlias> out_list, u32 report_type) {
        R_UNLESS(out_list.size() == ReportListSize, ResultInvalidArgument);
        const auto reports = GetManager().Store().List(report_type);
        const u32 count = static_cast<u32>(std::min(reports.size(), ReportStore::MaxReports));
        std::memset(out_list.data(), 0, out_list.size());
        std::memcpy(out_list.data(), &count, sizeof(count));
        std::memcpy(out_list.data() + 8, reports.data(), count * sizeof(ReportInfo));
        LOG_DEBUG(Service_ERPT, "called, type={} count={}", report_type, count);
        R_SUCCEED();
    }

    Result GetEvent(OutCopyHandle<Kernel::KReadableEvent> out_event) {
        *out_event = &event->GetReadableEvent();
        R_SUCCEED();
    }

    Result CleanupReports() {
        LOG_INFO(Service_ERPT, "erpt: every report deleted");
        GetManager().Store().Clear();
        GetManager().NotifyListeners();
        R_SUCCEED();
    }

    Result DeleteReport(ReportIdRaw report_id) {
        R_UNLESS(GetManager().Store().Delete(report_id.id), ResultNotFound);
        GetManager().NotifyListeners();
        R_SUCCEED();
    }

    Result GetStorageUsageStatistics(Out<StorageUsageStatistics> out_statistics, bool unknown) {
        auto& store = GetManager().Store();
        StorageUsageStatistics stats{};
        stats.used_storage_size = static_cast<u32>(store.UsedSize());
        stats.max_report_size = static_cast<s64>(MaxArrayBufferSize);
        for (const auto& info : store.List(ReportType_Any)) {
            const auto t = info.type & 1;
            ++stats.report_count[t];
            ++(((info.flags | info.flags_set) & 1) != 0 ? stats.transmitted_count[t]
                                                        : stats.untransmitted_count[t]);
        }
        *out_statistics = stats;
        LOG_UNKNOWN_CONTRACT(Service_ERPT,
                             "IManager 4 bool argument {} (meaning not established); the "
                             "statistics ignore it",
                             unknown);
        R_SUCCEED();
    }

    Result GetAttachmentList(Out<u32> out_count, ReportIdRaw report_id,
                             OutBuffer<BufferAttr_HipcMapAlias> out_list) {
        // No attachment is kept.
        *out_count = 0;
        R_SUCCEED();
    }

    Result PopNotifiableErrorCodes(
        OutLargeData<std::array<u8, 0x880>, BufferAttr_HipcAutoSelect> out_codes) {
        LOG_UNKNOWN_CONTRACT(Service_ERPT,
                             "IManager 7 PopNotifiableErrorCodes answered with no error codes");
        out_codes->fill(0);
        R_SUCCEED();
    }

    Result GetReportSizeMax(Out<u32> out_size) {
        LOG_UNKNOWN_CONTRACT(Service_ERPT,
                             "IManager 10 GetReportSizeMax answered with the array buffer "
                             "limit {:#x}",
                             MaxArrayBufferSize);
        *out_size = static_cast<u32>(MaxArrayBufferSize);
        R_SUCCEED();
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
    u64 listener{};
};

class ErrorReportSession final : public ServiceFramework<ErrorReportSession> {
public:
    explicit ErrorReportSession(Core::System& system_) : ServiceFramework{system_, "erpt:r"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&ErrorReportSession::OpenReport>, "OpenReport"},
            {1, D<&ErrorReportSession::OpenManager>, "OpenManager"},
            {2, D<&ErrorReportSession::OpenAttachment>, "OpenAttachment"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    Result OpenReport(OutInterface<IReport> out_report) {
        *out_report = std::make_shared<IReport>(system);
        R_SUCCEED();
    }

    Result OpenManager(OutInterface<IManager> out_manager) {
        *out_manager = std::make_shared<IManager>(system);
        R_SUCCEED();
    }

    // No attachment is ever kept, so an attachment opens as a report would and finds nothing.
    Result OpenAttachment(OutInterface<IReport> out_attachment) {
        *out_attachment = std::make_shared<IReport>(system);
        R_SUCCEED();
    }
};

void LoopProcess(Core::System& system) {
    GetManager().Initialize(system);

    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("erpt:c", std::make_shared<ErrorReportContext>(system));
    server_manager->RegisterNamedService("erpt:r", std::make_shared<ErrorReportSession>(system));

    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::ERPT
