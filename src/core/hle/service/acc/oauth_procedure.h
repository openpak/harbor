// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// nn::account::nas::IOAuthProcedureForUserRegistration (acc:su 211-214) and
// IOAuthProcedureForNintendoAccountLinkage (IAdministrator 251/252), as the 22.5.0 account module
// serves them (scratch/ryujinx-android/account-oauth/notes.md): GetRequest hands out the sign-in
// URL and the redirect the firmware's Login applet stops at; ApplyResponseAsync takes the URL it
// stopped at and does the network work against OpenPak on a worker thread.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "common/uuid.h"
#include "core/hle/service/acc/openpak_account.h"
#include "core/hle/service/service.h"

namespace OpenPak {
class AsyncWorker;
}

namespace Service::Account {

class ProfileManager;

/// A session id (nn::account::detail::Uuid) as Suspend writes it.
struct ProcedureSessionId {
    u64 low = 0;
    u64 high = 0;
    bool IsZero() const {
        return low == 0 && high == 0;
    }
    auto operator<=>(const ProcedureSessionId&) const = default;
};
static_assert(sizeof(ProcedureSessionId) == 0x10);

namespace detail {
struct ProcedureState;
}

class IOAuthProcedureForUserRegistration final
    : public ServiceFramework<IOAuthProcedureForUserRegistration> {
public:
    IOAuthProcedureForUserRegistration(Core::System& system_,
                                       std::shared_ptr<OpenPak::AsyncWorker> worker_);
    ~IOAuthProcedureForUserRegistration() override;

    /// acc:su 212/214: carry on with a suspended procedure.
    Result Resume(const ProcedureSessionId& id);

private:
    void PrepareAsync(HLERequestContext& ctx);
    void GetRequest(HLERequestContext& ctx);
    void ApplyResponse(HLERequestContext& ctx);
    void ApplyResponseAsync(HLERequestContext& ctx);
    void ApplyResponseForUserCreationAsync(HLERequestContext& ctx);
    void Suspend(HLERequestContext& ctx);
    void GetAccountId(HLERequestContext& ctx);
    void GetLinkedNintendoAccountId(HLERequestContext& ctx);
    void GetNickname(HLERequestContext& ctx);
    void GetProfileImage(HLERequestContext& ctx);
    void GetProfileLargeImage(HLERequestContext& ctx);
    void RegisterUserAsync(HLERequestContext& ctx);
    void GetUid(HLERequestContext& ctx);
    void IsProfileAvailable(HLERequestContext& ctx);
    void RegisterUserWithProfileAsync(HLERequestContext& ctx);

    void Apply(HLERequestContext& ctx, bool profile_required);
    void Register(HLERequestContext& ctx, std::string nickname, std::vector<u8> image);

    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<detail::ProcedureState> state;
    ProfileManager& profile_manager;
    Common::UUID registered_uid{};
};

class IOAuthProcedureForNintendoAccountLinkage final
    : public ServiceFramework<IOAuthProcedureForNintendoAccountLinkage> {
public:
    IOAuthProcedureForNintendoAccountLinkage(Core::System& system_,
                                             std::shared_ptr<OpenPak::AsyncWorker> worker_,
                                             Common::UUID user_id_);
    ~IOAuthProcedureForNintendoAccountLinkage() override;

    /// IAdministrator 252: carry on with a suspended procedure.
    Result Resume(const ProcedureSessionId& id);

private:
    void PrepareAsync(HLERequestContext& ctx);
    void GetRequest(HLERequestContext& ctx);
    void ApplyResponse(HLERequestContext& ctx);
    void ApplyResponseAsync(HLERequestContext& ctx);
    void Suspend(HLERequestContext& ctx);
    void GetRequestWithTheme(HLERequestContext& ctx);
    void IsNetworkServiceAccountReplaced(HLERequestContext& ctx);

    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<detail::ProcedureState> state;
    Common::UUID user_id;
};

} // namespace Service::Account
