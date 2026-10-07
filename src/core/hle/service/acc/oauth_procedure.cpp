// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <random>

#include "common/fs/file.h"
#include "common/logging.h"
#include "common/redact.h"
#include "common/settings.h"
#include "core/constants.h"
#include "core/core.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/nintendo_account_oauth.h"
#include "core/hle/service/acc/oauth_procedure.h"
#include "core/hle/service/acc/oauth_procedure_rules.h"
#include "core/hle/service/acc/openpak_async.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/session.h"

namespace Service::Account {

namespace oauth = NintendoAccountOAuth;

namespace detail {

/// What a procedure knows, shared with the work it starts (which may outlive the IPC object).
struct ProcedureState {
    explicit ProcedureState(ProcedureRules::Kind kind_) : kind{kind_} {}

    /// The rules' view of this procedure. Call with `mutex` held.
    ProcedureRules::Machine Machine() const {
        return {.kind = kind, .stage = stage, .has_code = !code.empty(), .registered = registered};
    }

    const ProcedureRules::Kind kind;
    std::mutex mutex;
    ProcedureRules::Stage stage = ProcedureRules::Stage::Fresh;
    std::string verifier;
    std::string state;
    std::string code;
    /// The user the account is linked to: the existing user for a linkage, the one the
    /// registration is going to create for a registration.
    std::string uid;
    std::optional<OpenPakAccount::NintendoAccountLink> link;
    bool registered = false;
    /// The IPC object is gone: a registration that never registered drops its device account.
    bool abandoned = false;
};

} // namespace detail

namespace {

using detail::ProcedureState;
using ProcedureRules::Kind;
using ProcedureRules::Stage;

constexpr std::size_t NicknameSize = 0x21;
// acc:su 211 maps this much of the caller's transfer memory as the procedure's work area.
constexpr u32 MinimumWorkMemorySize = 0x22000;

struct Suspended {
    std::string verifier;
    std::string state;
};

std::mutex sessions_mutex;
std::map<std::pair<Kind, ProcedureSessionId>, Suspended> sessions;

ProcedureSessionId NewSessionId() {
    std::random_device random;
    const auto draw = [&random] { return (u64{random()} << 32) | random(); };
    return ProcedureSessionId{draw() | 1, draw()};
}

void PushError(HLERequestContext& ctx, Result result) {
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

/// A fixed-size string into a receive-list buffer, zero padded.
void WriteFixed(HLERequestContext& ctx, VAddr address, std::size_t capacity, std::size_t size,
                std::string_view text) {
    const auto buffer = ProcedureRules::FixedString(text, capacity, size);
    ctx.GetMemory().WriteBlock(address, buffer.data(), buffer.size());
}

/// GetRequest's two 0x1A pointer outputs: RequestUrl (0x1000) and CallbackUri (0x100). Written by
/// hand, one per receive-list entry; a single entry large enough for both takes them back to back.
Result WriteRequest(HLERequestContext& ctx, ProcedureState& state, int theme) {
    using ProcedureRules::CallbackUriSize;
    using ProcedureRules::RequestLayout;
    using ProcedureRules::RequestUrlSize;

    std::lock_guard lock{state.mutex};
    if (!ProcedureRules::CanGetRequest(state.Machine())) {
        return ResultInvalidProcedureState;
    }

    const auto& c = ctx.BufferDescriptorC();
    std::vector<std::size_t> sizes;
    for (const auto& entry : c) {
        sizes.push_back(entry.Size());
    }
    const RequestLayout layout = ProcedureRules::PlanRequest(sizes);
    if (layout == RequestLayout::Refused) {
        LOG_WARNING(Service_ACC, "acc: GetRequest with {} receive-list entries, two expected",
                    c.size());
        return ResultInvalidBuffer;
    }

    state.verifier = oauth::NewVerifier();
    state.state = oauth::NewState();
    const std::string url = oauth::RequestUrl(state.verifier, state.state, theme);

    if (layout == RequestLayout::TwoEntries) {
        WriteFixed(ctx, c[0].Address(), RequestUrlSize, c[0].Size(), url);
        WriteFixed(ctx, c[1].Address(), CallbackUriSize, c[1].Size(), oauth::RedirectUri);
    } else {
        LOG_UNKNOWN_CONTRACT(Service_ACC,
                             "acc GetRequest: one receive-list entry, URL and callback written "
                             "back to back");
        WriteFixed(ctx, c[0].Address(), RequestUrlSize, RequestUrlSize, url);
        WriteFixed(ctx, c[0].Address() + RequestUrlSize, CallbackUriSize, CallbackUriSize,
                   oauth::RedirectUri);
    }

    LOG_INFO(Service_ACC, "acc: OAuth GetRequest, theme {}", oauth::ThemeName(theme));
    return ResultSuccess;
}

Result SuspendInto(ProcedureState& state, ProcedureSessionId& out) {
    out = NewSessionId();
    std::lock_guard lock{sessions_mutex};
    sessions[{state.kind, out}] = Suspended{state.verifier, state.state};
    return ResultSuccess;
}

/// Takes a suspended session's verifier and state; the stage is the one a resume starts at.
Result ResumeFrom(ProcedureState& state, const ProcedureSessionId& id) {
    Suspended suspended;
    {
        std::lock_guard lock{sessions_mutex};
        const auto it = sessions.find({state.kind, id});
        if (it == sessions.end()) {
            // The module's result for an unknown session id (0x0860f080) is not read.
            LOG_UNKNOWN_CONTRACT(Service_ACC,
                                 "acc resume of an unknown procedure session, refused with "
                                 "InvalidArgument");
            return ResultInvalidArgument;
        }
        suspended = std::move(it->second);
        sessions.erase(it);
    }
    std::lock_guard lock{state.mutex};
    state.verifier = std::move(suspended.verifier);
    state.state = std::move(suspended.state);
    state.stage = ProcedureRules::AfterResume(state.kind);
    return ResultSuccess;
}

/// The other users on this console, as the OpenPak session names them.
std::vector<std::string> OtherUsers(const ProfileManager& profile_manager) {
    std::vector<std::string> out;
    for (const auto& user : profile_manager.GetAllUsers()) {
        if (user.IsValid()) {
            out.push_back(OpenPakAccount::UidString(user));
        }
    }
    return out;
}

/// ApplyResponseAsync: the callback check here, the network work on the worker.
void StartApply(HLERequestContext& ctx, Core::System& system,
                const std::shared_ptr<OpenPak::AsyncWorker>& worker,
                const std::shared_ptr<ProcedureState>& state, bool profile_required, std::vector<std::string> others,
                std::function<void(const OpenPakAccount::NintendoAccountLink&)> applied) {
    if (!ctx.CanReadBuffer(0) || ctx.GetReadBufferSize(0) == 0) {
        // The command stub refuses an empty buffer before the module sees it.
        PushError(ctx, ResultSfInvalidArgument);
        return;
    }
    const auto buffer = ctx.ReadBuffer(0);
    const std::string response(reinterpret_cast<const char*>(buffer.data()),
                               strnlen(reinterpret_cast<const char*>(buffer.data()),
                                       buffer.size()));

    std::string code;
    std::string verifier;
    std::string uid;
    {
        std::lock_guard lock{state->mutex};
        if (!ProcedureRules::CanApply(state->Machine())) {
            PushError(ctx, ResultInvalidProcedureState);
            return;
        }
        if (oauth::ParseResponse(response, state->state, code) != oauth::ParseError::None) {
            LOG_WARNING(Service_ACC, "acc: ApplyResponse refused the callback URL");
            PushError(ctx, ResultOAuthResponseMismatch);
            return;
        }
        state->code = code;
        verifier = state->verifier;
        if (state->uid.empty()) {
            state->uid = OpenPakAccount::UidString(Common::UUID::MakeRandom());
        }
        uid = state->uid;
    }

    auto work = [state, code, verifier, uid, profile_required, others = std::move(others),
                 applied = std::move(applied)](std::stop_token stop, std::vector<u8>&) {
        OpenPakAccount::NintendoAccountLink link;
        Result result = OpenPakAccount::ApplyCode(stop, code, verifier, uid, others, link);
        if (result.IsSuccess() && profile_required && link.nickname.empty()) {
            result = ResultNetworkServiceAccountProfileUnavailable;
        }

        bool abandoned;
        {
            std::lock_guard lock{state->mutex};
            abandoned = state->abandoned;
            if (result.IsSuccess() && !abandoned) {
                state->link = link;
                state->stage = ProcedureRules::AfterApply(state->Machine(), true);
            }
        }
        if (result.IsSuccess() && !abandoned && applied) {
            applied(link);
        }
        if ((abandoned || result.IsError()) && state->kind == Kind::Registration) {
            // Nobody will register this user: the device account made for it goes with it.
            OpenPak::Session::ForgetProfile(uid);
        }
        LOG_INFO(Service_ACC, "acc: OAuth response applied -> {}",
                 result.IsSuccess() ? "success" : "failure");
        return result;
    };

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<OpenPakAsyncContext>(ctx, system, worker, std::move(work));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// IOAuthProcedureForUserRegistration (acc:su 211/212/213/214): a new user from an account.

IOAuthProcedureForUserRegistration::IOAuthProcedureForUserRegistration(
    Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_)
    : ServiceFramework{system_, "IOAuthProcedureForUserRegistration"},
      worker{std::move(worker_)},
      state{std::make_shared<ProcedureState>(Kind::Registration)},
      profile_manager{system_.GetProfileManager()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IOAuthProcedureForUserRegistration::PrepareAsync, "PrepareAsync"},
        {1, &IOAuthProcedureForUserRegistration::GetRequest, "GetRequest"},
        {2, &IOAuthProcedureForUserRegistration::ApplyResponse, "ApplyResponse"},
        {3, &IOAuthProcedureForUserRegistration::ApplyResponseAsync, "ApplyResponseAsync"},
        {10, &IOAuthProcedureForUserRegistration::Suspend, "Suspend"},
        {100, &IOAuthProcedureForUserRegistration::GetAccountId, "GetAccountId"},
        {101, &IOAuthProcedureForUserRegistration::GetLinkedNintendoAccountId, "GetLinkedNintendoAccountId"},
        {102, &IOAuthProcedureForUserRegistration::GetNickname, "GetNickname"},
        {103, &IOAuthProcedureForUserRegistration::GetProfileImage, "GetProfileImage"},
        {104, &IOAuthProcedureForUserRegistration::GetProfileLargeImage, "GetProfileLargeImage"}, // 18.0.0+
        {110, &IOAuthProcedureForUserRegistration::RegisterUserAsync, "RegisterUserAsync"},
        {111, &IOAuthProcedureForUserRegistration::GetUid, "GetUid"},
        {200, &IOAuthProcedureForUserRegistration::ApplyResponseForUserCreationAsync, "ApplyResponseForUserCreationAsync"}, // 17.0.0+
        {205, nullptr, "SuspendAfterApplyResponse"}, // 17.0.0+
        {210, &IOAuthProcedureForUserRegistration::IsProfileAvailable, "IsProfileAvailable"}, // 17.0.0+
        {220, &IOAuthProcedureForUserRegistration::RegisterUserAsync, "RegisterUserAsyncWithoutProfile"}, // 17.0.0+
        {221, &IOAuthProcedureForUserRegistration::RegisterUserWithProfileAsync, "RegisterUserWithProfileAsync"}, // 17.0.0+
        {230, &IOAuthProcedureForUserRegistration::RegisterUserWithProfileAsync, "RegisterUserWithLargeImageProfileAsync"} // 18.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

IOAuthProcedureForUserRegistration::~IOAuthProcedureForUserRegistration() {
    std::string forget;
    {
        std::lock_guard lock{state->mutex};
        state->abandoned = true;
        if (state->stage == Stage::Applied && !state->registered) {
            forget = state->uid;
        }
    }
    if (!forget.empty()) {
        // Applied and never registered: the device account made for it is dropped.
        OpenPak::Session::ForgetProfile(forget);
    }
}

Result IOAuthProcedureForUserRegistration::Resume(const ProcedureSessionId& id) {
    return ResumeFrom(*state, id);
}

void IOAuthProcedureForUserRegistration::PrepareAsync(HLERequestContext& ctx) {
    // The module's worker only checks the stage (0x085a1e40); nothing goes over the network.
    bool ready;
    {
        std::lock_guard lock{state->mutex};
        ready = ProcedureRules::PrepareSucceeds(state->Machine());
    }
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<OpenPakAsyncContext>(
        ctx, system, worker,
        OpenPakAsyncContext::Done(ready ? ResultSuccess : ResultInvalidProcedureState));
}

void IOAuthProcedureForUserRegistration::GetRequest(HLERequestContext& ctx) {
    PushError(ctx, WriteRequest(ctx, *state, oauth::RegistrationTheme));
}

void IOAuthProcedureForUserRegistration::ApplyResponse(HLERequestContext& ctx) {
    PushError(ctx, ResultNotSupported);
}

void IOAuthProcedureForUserRegistration::ApplyResponseAsync(HLERequestContext& ctx) {
    Apply(ctx, true);
}

void IOAuthProcedureForUserRegistration::ApplyResponseForUserCreationAsync(
    HLERequestContext& ctx) {
    // As 3, but an account with no profile is allowed.
    Apply(ctx, false);
}

void IOAuthProcedureForUserRegistration::Apply(HLERequestContext& ctx, bool profile_required) {
    StartApply(ctx, system, worker, state, profile_required, OtherUsers(profile_manager), {});
}

void IOAuthProcedureForUserRegistration::Suspend(HLERequestContext& ctx) {
    {
        std::lock_guard lock{state->mutex};
        if (!ProcedureRules::CanSuspend(state->Machine())) {
            PushError(ctx, ResultInvalidProcedureState);
            return;
        }
        state->stage = ProcedureRules::AfterSuspend(state->Machine());
    }
    ProcedureSessionId id;
    SuspendInto(*state, id);
    LOG_INFO(Service_ACC, "acc: user registration procedure suspended");

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(id);
}

void IOAuthProcedureForUserRegistration::GetAccountId(HLERequestContext& ctx) {
    std::lock_guard lock{state->mutex};
    if (!ProcedureRules::CanReadApplied(state->Machine())) {
        PushError(ctx, ResultInvalidProcedureState);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(state->link->network_service_account_id);
}

void IOAuthProcedureForUserRegistration::GetLinkedNintendoAccountId(HLERequestContext& ctx) {
    std::lock_guard lock{state->mutex};
    if (!ProcedureRules::CanReadApplied(state->Machine())) {
        PushError(ctx, ResultInvalidProcedureState);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(state->link->nintendo_account_id);
}

void IOAuthProcedureForUserRegistration::GetNickname(HLERequestContext& ctx) {
    std::string nickname;
    {
        std::lock_guard lock{state->mutex};
        if (!ProcedureRules::CanReadApplied(state->Machine())) {
            PushError(ctx, ResultInvalidProcedureState);
            return;
        }
        nickname = state->link->nickname;
    }
    if (!ctx.CanWriteBuffer() || ctx.GetWriteBufferSize() < NicknameSize) {
        PushError(ctx, ResultInvalidBufferSize);
        return;
    }
    std::array<u8, NicknameSize> out{};
    std::memcpy(out.data(), nickname.data(), std::min(nickname.size(), NicknameSize - 1));
    ctx.WriteBuffer(out);
    PushError(ctx, ResultSuccess);
}

void IOAuthProcedureForUserRegistration::GetProfileImage(HLERequestContext& ctx) {
    std::vector<u8> image;
    {
        std::lock_guard lock{state->mutex};
        if (!ProcedureRules::CanReadApplied(state->Machine())) {
            PushError(ctx, ResultInvalidProcedureState);
            return;
        }
        image = state->link->image;
    }
    if (image.empty()) {
        image.assign(Core::Constants::ACCOUNT_BACKUP_JPEG.begin(),
                     Core::Constants::ACCOUNT_BACKUP_JPEG.end());
    }
    if (ctx.CanWriteBuffer() && ctx.GetWriteBufferSize() > 0) {
        ctx.WriteBuffer(image.data(), std::min(image.size(), ctx.GetWriteBufferSize()));
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(image.size()));
}

void IOAuthProcedureForUserRegistration::GetProfileLargeImage(HLERequestContext& ctx) {
    // Stubbed in the module since 18.0.0.
    PushError(ctx, ResultNotSupported);
}

void IOAuthProcedureForUserRegistration::RegisterUserAsync(HLERequestContext& ctx) {
    // Also 220, RegisterUserAsyncWithoutProfile: what the module leaves out of the profile there
    // (0x08618f20) is not read, so it registers as 110 does.
    std::string nickname;
    std::vector<u8> image;
    {
        std::lock_guard lock{state->mutex};
        if (state->link) {
            nickname = state->link->nickname;
            image = state->link->image;
        }
    }
    Register(ctx, std::move(nickname), std::move(image));
}

void IOAuthProcedureForUserRegistration::RegisterUserWithProfileAsync(HLERequestContext& ctx) {
    // Also 230: (nn::account::Nickname, buffer<bytes, 5> image).
    IPC::RequestParser rp{ctx};
    const auto raw = rp.PopRaw<std::array<char, NicknameSize>>();
    std::string nickname(raw.data(), strnlen(raw.data(), raw.size()));

    std::vector<u8> image;
    if (ctx.CanReadBuffer(0) && ctx.GetReadBufferSize(0) > 0) {
        const auto data = ctx.ReadBuffer(0);
        image.assign(data.begin(), data.end());
    }
    if (image.empty()) {
        std::lock_guard lock{state->mutex};
        if (state->link) {
            image = state->link->image;
        }
    }
    Register(ctx, std::move(nickname), std::move(image));
}

/// The user is created in the call, as the module does (0x08618ca0), and keeps the device account
/// the response federated; the work after it is that user's sign-in.
void IOAuthProcedureForUserRegistration::Register(HLERequestContext& ctx, std::string nickname,
                                                  std::vector<u8> image) {
    std::string uid_text;
    {
        std::lock_guard lock{state->mutex};
        if (!ProcedureRules::CanRegister(state->Machine())) {
            PushError(ctx, ResultInvalidProcedureState);
            return;
        }
        uid_text = state->uid;
    }
    const Common::UUID uid{uid_text};

    const ProfileUsername username = OpenPakAccount::ToUsername(nickname);
    if (const Result added = profile_manager.AddUser({
            .user_uuid = uid,
            .username = username,
            .creation_time = 0,
            .data = {},
            .is_open = false,
        });
        added.IsError()) {
        LOG_WARNING(Service_ACC, "acc: the registered user could not be added");
        PushError(ctx, added);
        return;
    }

    if (!image.empty()) {
        const auto path = OpenPakAccount::ProfileImagePath(uid);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        Common::FS::IOFile file(path, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::BinaryFile);
        if (!file.IsOpen() || file.Write(image) != image.size()) {
            LOG_WARNING(Service_ACC, "acc: the registered user's picture was not saved");
        }
    }

    // As acc:su 201: saved, and the open user on a console that has none.
    profile_manager.WriteUserSaveFile();
    if (profile_manager.GetLastOpenedUser().IsInvalid()) {
        if (const auto index = profile_manager.GetUserIndex(uid)) {
            Settings::values.current_user = static_cast<s32>(*index);
        }
        profile_manager.OpenUser(uid);
    }
    registered_uid = uid;
    {
        std::lock_guard lock{state->mutex};
        state->registered = true;
    }

    LOG_INFO(Service_ACC, "acc: user registered from a Nintendo Account, {} user(s)",
             profile_manager.GetUserCount());

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<OpenPakAsyncContext>(
        ctx, system, worker, [uid_text](std::stop_token, std::vector<u8>&) {
            // The federation signed this user in already; this refreshes nothing unless the
            // token went stale meanwhile.
            OpenPak::Session::Ensure(uid_text);
            return ResultSuccess;
        });
}

void IOAuthProcedureForUserRegistration::GetUid(HLERequestContext& ctx) {
    if (registered_uid.IsInvalid()) {
        PushError(ctx, ResultInvalidProcedureState);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(registered_uid);
}

void IOAuthProcedureForUserRegistration::IsProfileAvailable(HLERequestContext& ctx) {
    std::lock_guard lock{state->mutex};
    if (!ProcedureRules::CanReadApplied(state->Machine())) {
        PushError(ctx, ResultInvalidProcedureState);
        return;
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(!state->link->nickname.empty());
}

// ---------------------------------------------------------------------------------------------
// IOAuthProcedureForNintendoAccountLinkage (IAdministrator 251/252): link an existing user.

IOAuthProcedureForNintendoAccountLinkage::IOAuthProcedureForNintendoAccountLinkage(
    Core::System& system_, std::shared_ptr<OpenPak::AsyncWorker> worker_, Common::UUID user_id_)
    : ServiceFramework{system_, "IOAuthProcedureForNintendoAccountLinkage"},
      worker{std::move(worker_)},
      state{std::make_shared<ProcedureState>(Kind::Linkage)}, user_id{user_id_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IOAuthProcedureForNintendoAccountLinkage::PrepareAsync, "PrepareAsync"},
        {1, &IOAuthProcedureForNintendoAccountLinkage::GetRequest, "GetRequest"},
        {2, &IOAuthProcedureForNintendoAccountLinkage::ApplyResponse, "ApplyResponse"},
        {3, &IOAuthProcedureForNintendoAccountLinkage::ApplyResponseAsync, "ApplyResponseAsync"},
        {10, &IOAuthProcedureForNintendoAccountLinkage::Suspend, "Suspend"},
        {100, &IOAuthProcedureForNintendoAccountLinkage::GetRequestWithTheme, "GetRequestWithTheme"},
        {101, &IOAuthProcedureForNintendoAccountLinkage::IsNetworkServiceAccountReplaced, "IsNetworkServiceAccountReplaced"},
        {199, nullptr, "GetUrlForIntroductionOfExtraMembership"}, // 2.0.0 - 5.1.0
        {200, nullptr, "ApplyAsyncWithAuthorizedToken"},
    };
    // clang-format on

    RegisterHandlers(functions);
    state->uid = OpenPakAccount::UidString(user_id);
}

IOAuthProcedureForNintendoAccountLinkage::~IOAuthProcedureForNintendoAccountLinkage() {
    std::lock_guard lock{state->mutex};
    state->abandoned = true;
}

Result IOAuthProcedureForNintendoAccountLinkage::Resume(const ProcedureSessionId& id) {
    return ResumeFrom(*state, id);
}

void IOAuthProcedureForNintendoAccountLinkage::PrepareAsync(HLERequestContext& ctx) {
    // Not supported for a linkage (the module answers 2124-0011).
    PushError(ctx, ResultNotSupported);
}

void IOAuthProcedureForNintendoAccountLinkage::GetRequest(HLERequestContext& ctx) {
    PushError(ctx, WriteRequest(ctx, *state, oauth::LinkageTheme));
}

void IOAuthProcedureForNintendoAccountLinkage::GetRequestWithTheme(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto theme = rp.Pop<u32>();
    if (theme > static_cast<u32>(oauth::MaxRequestTheme)) {
        PushError(ctx, ResultInvalidTheme);
        return;
    }
    PushError(ctx, WriteRequest(ctx, *state, static_cast<int>(theme)));
}

void IOAuthProcedureForNintendoAccountLinkage::ApplyResponse(HLERequestContext& ctx) {
    PushError(ctx, ResultNotSupported);
}

void IOAuthProcedureForNintendoAccountLinkage::ApplyResponseAsync(HLERequestContext& ctx) {
    const Common::UUID uid = user_id;
    // The module syncs the user's profile from the account after the federation (0x085f9360).
    StartApply(ctx, system, worker, state, false,
               OtherUsers(system.GetProfileManager()),
               [uid](const OpenPakAccount::NintendoAccountLink& link) {
                   OpenPakAccount::QueueAdoption(uid, link.nickname, link.image);
               });
}

void IOAuthProcedureForNintendoAccountLinkage::Suspend(HLERequestContext& ctx) {
    {
        std::lock_guard lock{state->mutex};
        if (!ProcedureRules::CanSuspend(state->Machine())) {
            PushError(ctx, ResultInvalidProcedureState);
            return;
        }
        state->stage = ProcedureRules::AfterSuspend(state->Machine());
    }
    ProcedureSessionId id;
    SuspendInto(*state, id);
    LOG_INFO(Service_ACC, "acc: linkage procedure suspended");

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(id);
}

void IOAuthProcedureForNintendoAccountLinkage::IsNetworkServiceAccountReplaced(
    HLERequestContext& ctx) {
    // No stage check in the module (byte +0x264).
    bool replaced;
    {
        std::lock_guard lock{state->mutex};
        replaced = state->link && state->link->replaced;
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(replaced);
}

} // namespace Service::Account
