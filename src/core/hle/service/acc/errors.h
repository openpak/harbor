// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::Account {

constexpr Result ResultCancelledByUser{ErrorModule::Account, 1};
constexpr Result ResultNoNotifications{ErrorModule::Account, 15};
constexpr Result ResultInvalidUserId{ErrorModule::Account, 20};
constexpr Result ResultInvalidApplication{ErrorModule::Account, 22};
constexpr Result ResultNullptr{ErrorModule::Account, 30};
constexpr Result ResultInvalidArrayLength{ErrorModule::Account, 32};
constexpr Result ResultApplicationInfoAlreadyInitialized{ErrorModule::Account, 41};
constexpr Result ResultAccountUpdateFailed{ErrorModule::Account, 100};

// The Nintendo Account procedures and the BAAS managers, as the 22.5.0 account module answers
// (scratch/ryujinx-android/account-oauth/notes.md).
// ApplyResponse (sync), a linkage PrepareAsync and GetProfileLargeImage: async forms only.
constexpr Result ResultNotSupported{ErrorModule::Account, 11};
constexpr Result ResultInvalidArgument{ErrorModule::Account, 22};
constexpr Result ResultInvalidBufferSize{ErrorModule::Account, 31};
constexpr Result ResultInvalidBuffer{ErrorModule::Account, 32};
// GetRequestWithTheme with a theme above 3.
constexpr Result ResultInvalidTheme{ErrorModule::Account, 34};
// A procedure command at the wrong step (a request after a response, a result before it).
constexpr Result ResultInvalidProcedureState{ErrorModule::Account, 41};
constexpr Result ResultInternetRequestDenied{ErrorModule::Account, 59};
constexpr Result ResultUserNotFound{ErrorModule::Account, 100};
// CreateProcedureToLinkWithNintendoAccount for a user already linked.
constexpr Result ResultNintendoAccountAlreadyLinked{ErrorModule::Account, 140};
// No su/baas/<uid>.dat: the user has no network service account.
constexpr Result ResultNetworkServiceAccountUnavailable{ErrorModule::Account, 200};
// A registration that says linked but holds Nintendo Account id 0.
constexpr Result ResultNintendoAccountIdUnavailable{ErrorModule::Account, 220};
// ApplyResponse: BAAS refused the federation; the account belongs to another user here; the
// account has no network service account profile.
constexpr Result ResultNetworkServiceAccountFederationFailed{ErrorModule::Account, 290};
constexpr Result ResultNintendoAccountAlreadyRegistered{ErrorModule::Account, 291};
constexpr Result ResultNetworkServiceAccountProfileUnavailable{ErrorModule::Account, 292};
// The users/me cache reader: no cache.
constexpr Result ResultNintendoAccountUserResourceCacheNotFound{ErrorModule::Account, 400};
constexpr Result ResultInvalidIdTokenCacheBufferSize{ErrorModule::Account, 451};
// The callback URL is not the procedure's (other prefix, malformed query, other state, no code).
constexpr Result ResultOAuthResponseMismatch{ErrorModule::Account, 3122};
// sf InvalidArgument (0x60a): the command stubs refuse a zero Uuid or an empty buffer themselves.
constexpr Result ResultSfInvalidArgument{ErrorModule::CMIF, 3};

} // namespace Service::Account
