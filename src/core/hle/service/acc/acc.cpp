// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <mutex>
#include <map>
#include <array>
#include <cstring>
#include <random>

#include "common/common_types.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/redact.h"
#include "common/settings.h"
#include <ranges>
#include "common/stb.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/constants.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/acc/acc.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/acc/async_context.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/nintendo_account_oauth.h"
#include "core/hle/service/acc/oauth_procedure.h"
#include "core/hle/service/acc/openpak_account.h"
#include "core/hle/service/acc/openpak_async.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/server_manager.h"
#include "core/loader/loader.h"
#include "core/openpak/account.h"
#include "core/openpak/async_worker.h"
#include "core/openpak/platform.h"
#include "core/openpak/session.h"

namespace Service::Account {

// Thumbnails are hard coded to be at least this size
constexpr std::size_t THUMBNAIL_SIZE = 0x24000;

static std::filesystem::path GetImagePath(const Common::UUID& uuid) {
    return OpenPakAccount::ProfileImagePath(uuid);
}

static void JPGToMemory(void* context, void* data, int len) {
    std::vector<u8>* jpg_image = static_cast<std::vector<u8>*>(context);
    unsigned char* jpg = static_cast<unsigned char*>(data);
    jpg_image->insert(jpg_image->end(), jpg, jpg + len);
}

static void SanitizeJPEGImageSize(std::vector<u8>& image) {
    constexpr std::size_t max_jpeg_image_size = 0x20000;
    constexpr int profile_dimensions = 256;
    int original_width = 0, original_height = 0, color_channels = 0;

    const auto plain_image =
        stbi_load_from_memory(image.data(), static_cast<int>(image.size()), &original_width,
                              &original_height, &color_channels, STBI_rgb);
    if (plain_image == nullptr) {
        // [Harbor] Not a picture stb can read (a file cut short, say): handed over as it is
        // rather than resized from nothing, which crashed the host.
        LOG_WARNING(Service_ACC, "The user provided image could not be decoded; passed as is");
        image.resize((std::min)(image.size(), max_jpeg_image_size));
        return;
    }

    // Resize image to match 256*256
    if (original_width != profile_dimensions || original_height != profile_dimensions) {
        // Use vector instead of array to avoid overflowing the stack
        std::vector<u8> out_image(profile_dimensions * profile_dimensions * STBI_rgb);
        stbir_resize_uint8_srgb(plain_image, original_width, original_height, 0, out_image.data(),
                                profile_dimensions, profile_dimensions, 0, STBI_rgb, 0,
                                STBIR_FILTER_BOX);
        image.clear();
        if (!stbi_write_jpg_to_func(JPGToMemory, &image, profile_dimensions, profile_dimensions,
                                    STBI_rgb, out_image.data(), 0)) {
            LOG_ERROR(Service_ACC, "Failed to resize the user provided image.");
        }
    }
    stbi_image_free(plain_image);

    image.resize((std::min)(image.size(), max_jpeg_image_size));
}

// [Harbor] The BAAS managers answer from the OpenPak session for the user they were made for.
// IAdministrator 250 and the Nintendo Account data of 120/136 follow one rule: the active user,
// linked. Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

static void PushResult(HLERequestContext& ctx, Result result) {
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(result);
}

static bool IsActiveLinked(const Common::UUID& user) {
    return OpenPakAccount::IsActiveLinked(user);
}

/// The BAAS user id OpenPak issued the user, or the offline placeholder stock Eden answers.
static u64 NetworkServiceAccountIdOf(const Common::UUID& user) {
    const u64 id = OpenPakAccount::Enabled()
                       ? OpenPak::Session::NetworkServiceAccountId(OpenPakAccount::UidString(user))
                       : 0;
    return id != 0 ? id : user.Hash();
}

/// A user OpenPak signed in whose device account is not linked to an account: on a console that
/// user has no network service account a title can use (2124-0200), so a title is never handed
/// its id_token. The token carries no openpak claim, and every title server refuses it with the
/// title's own error instead of the firmware's link prompt. False while nobody has signed the
/// user in yet (that is EnsureIdTokenCacheAsync's to find out) and when OpenPak is off.
static bool SignedInUnlinked(const Common::UUID& user) {
    if (!OpenPakAccount::Enabled()) {
        return false;
    }
    const std::string uid = OpenPakAccount::UidString(user);
    OpenPak::WaitForFirmwareUserLink(uid);
    return OpenPak::Session::NetworkServiceAccountId(uid) != 0 && !OpenPak::Session::Linked(uid);
}

/// aauth refused the running title for this user (an old version: 0111): the account module's
/// 2124 result, which the title hands to the error applet (2124-3001, "A software update is
/// required"). Only a title's own id-token requests answer it: offline play, launch and the
/// system's sign-ins never depend on the title's version.
static std::optional<Result> AauthRefused(const Common::UUID& user) {
    if (!OpenPakAccount::Enabled()) {
        return std::nullopt;
    }
    const std::string code = OpenPak::Session::AauthRefusal(OpenPakAccount::UidString(user));
    const auto description = OpenPak::Session::AauthRefusalDescription(code);
    if (!description) {
        return std::nullopt;
    }
    LOG_INFO(Service_ACC, "acc: aauth refused the title (code {}): 2124-{:04}", code, *description);
    return Result{ErrorModule::Account, *description};
}

/// LoadIdTokenCache (3 and 4): the user's OpenPak id_token, size checked against the buffer.
static void LoadIdTokenCacheFor(HLERequestContext& ctx, const Common::UUID& user) {
    std::string token;
    if (OpenPakAccount::Enabled()) {
        token = OpenPak::Session::IdToken(OpenPakAccount::UidString(user));
    }

    if (token.empty()) {
        // Offline: what stock Eden has always answered.
        std::vector<u8> zeros(0x100);
        if (ctx.CanWriteBuffer()) {
            ctx.WriteBuffer(zeros.data(), std::min(zeros.size(), ctx.GetWriteBufferSize()));
        }
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(zeros.size()));
        return;
    }

    // A real token is several times the size of the old stub's, so the buffer the guest offered
    // is checked before anything is written into it.
    if (!ctx.CanWriteBuffer() || token.size() > ctx.GetWriteBufferSize()) {
        LOG_WARNING(Service_ACC, "acc: id_token is {} bytes, guest buffer is {}", token.size(),
                    ctx.CanWriteBuffer() ? ctx.GetWriteBufferSize() : 0);
        PushResult(ctx, ResultInvalidIdTokenCacheBufferSize);
        return;
    }

    ctx.WriteBuffer(token.data(), token.size());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(token.size()));
}

/// The Nintendo Account behind the user, under IAdministrator 250's rule. Otherwise the module's
/// answer for a user with no network service account (2124-0200); firmware has no "registered,
/// not linked" state, so an unlinked device account maps there too.
static Result NintendoAccountOf(const Common::UUID& user,
                                std::optional<OpenPakAccount::NintendoAccountInfo>& out) {
    out.reset();
    if (!IsActiveLinked(user)) {
        return ResultNetworkServiceAccountUnavailable;
    }
    out = OpenPakAccount::NintendoAccount(OpenPakAccount::UidString(user));
    if (!out) {
        return ResultNetworkServiceAccountUnavailable;
    }
    // A login reply with no links.nintendoAccount.id: firmware's answer for a linked
    // registration holding id 0.
    return out->id != 0 ? ResultSuccess : ResultNintendoAccountIdUnavailable;
}

using NintendoAccountOAuth::NasUserBaseSize;

// nn::account::baas::IManagerForSystemService (acc:su 102), and IAdministrator (acc:su 250),
// which is the same manager plus the registration and Nintendo Account linkage commands.
class IManagerForSystemService final : public ServiceFramework<IManagerForSystemService> {
public:
    explicit IManagerForSystemService(Core::System& system_, Common::UUID uuid,
                                      std::shared_ptr<OpenPak::AsyncWorker> worker_,
                                      bool administrator)
        : ServiceFramework{system_, administrator ? "IAdministrator" : "IManagerForSystemService"},
          account_id{uuid}, worker{std::move(worker_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IManagerForSystemService::CheckAvailability, "CheckAvailability"},
            {1, &IManagerForSystemService::GetAccountId, "GetAccountId"},
            {2, &IManagerForSystemService::EnsureIdTokenCacheAsync, "EnsureIdTokenCacheAsync"},
            {3, &IManagerForSystemService::LoadIdTokenCache, "LoadIdTokenCacheDeprecated"},
            {4, &IManagerForSystemService::LoadIdTokenCache, "LoadIdTokenCache"}, // 19.0.0+
            {100, &IManagerForSystemService::SetSystemProgramIdentification, "SetSystemProgramIdentification"},
            {101, nullptr, "RefreshNotificationTokenAsync"}, // 7.0.0+
            {110, nullptr, "GetServiceEntryRequirementCache"}, // 4.0.0+
            {111, &IManagerForSystemService::InvalidateServiceEntryRequirementCache, "InvalidateServiceEntryRequirementCache"}, // 4.0.0+
            {112, nullptr, "InvalidateTokenCache"}, // 4.0.0 - 6.2.0
            {113, &IManagerForSystemService::GetServiceEntryRequirementCacheForOnlinePlay, "GetServiceEntryRequirementCacheForOnlinePlay"}, // 6.1.0+
            {120, &IManagerForSystemService::GetNintendoAccountId, "GetNintendoAccountId"},
            {121, nullptr, "CalculateNintendoAccountAuthenticationFingerprint"}, // 9.0.0+
            {131, &IManagerForSystemService::RefreshUserResourceAsync, "RefreshNintendoAccountUserResourceCacheAsync"},
            {132, &IManagerForSystemService::RefreshUserResourceIfElapsedAsync, "RefreshNintendoAccountUserResourceCacheAsyncIfSecondsElapsed"},
            {133, nullptr, "GetNintendoAccountVerificationUrlCache"}, // 9.0.0+
            {134, nullptr, "RefreshNintendoAccountVerificationUrlCache"}, // 9.0.0+
            {135, nullptr, "RefreshNintendoAccountVerificationUrlCacheAsyncIfSecondsElapsed"}, // 9.0.0+
            // No input data (qlaunch 0x0881b3b0, systemWeb 0x08567650). 130 is a different
            // contract on 22.5.0 and stays absent.
            {136, &IManagerForSystemService::GetNintendoAccountUserResourceCache, "GetNintendoAccountUserResourceCache"}, // 19.0.0+
            {140, nullptr, "GetNetworkServiceLicenseCache"}, // 5.0.0+
            {141, &IManagerForSystemService::RefreshNetworkServiceLicenseCacheAsync, "RefreshNetworkServiceLicenseCacheAsync"}, // 5.0.0+
            {142, &IManagerForSystemService::RefreshNetworkServiceLicenseCacheAsyncIfSecondsElapsed, "RefreshNetworkServiceLicenseCacheAsyncIfSecondsElapsed"}, // 5.0.0+
            {143, &IManagerForSystemService::GetNetworkServiceLicenseCacheEx, "GetNetworkServiceLicenseCacheEx"}, // 15.0.0+
            {150, nullptr, "CreateAuthorizationRequest"},
            {160, nullptr, "RequiresUpdateNetworkServiceAccountIdTokenCache"},
            {161, nullptr, "RequireReauthenticationOfNetworkServiceAccount"},
            {170, nullptr, "CreateDeviceHistoryRequest"}, // 17.0.0+
            {180, nullptr, "GetRequestForNintendoAccountReauthentication"} // 18.0.0+
        };
        static const FunctionInfo administrator_functions[] = {
            {200, &IManagerForSystemService::IsRegistered, "IsRegistered"},
            {201, &IManagerForSystemService::RegisterAsync, "RegisterAsync"},
            {202, nullptr, "UnregisterAsync"},
            {203, nullptr, "DeleteRegistrationInfoLocally"},
            {220, nullptr, "SynchronizeProfileAsync"},
            {221, nullptr, "UploadProfileAsync"},
            {222, nullptr, "SynchronizaProfileAsyncIfSecondsElapsed"},
            {250, &IManagerForSystemService::IsLinkedWithNintendoAccount, "IsLinkedWithNintendoAccount"},
            {251, &IManagerForSystemService::CreateProcedureToLinkWithNintendoAccount, "CreateProcedureToLinkWithNintendoAccount"},
            {252, &IManagerForSystemService::ResumeProcedureToLinkWithNintendoAccount, "ResumeProcedureToLinkWithNintendoAccount"},
            {255, nullptr, "CreateProcedureToUpdateLinkageStateOfNintendoAccount"},
            {256, nullptr, "ResumeProcedureToUpdateLinkageStateOfNintendoAccount"},
            {260, nullptr, "CreateProcedureToLinkNnidWithNintendoAccount"}, // 3.0.0+
            {261, nullptr, "ResumeProcedureToLinkNnidWithNintendoAccount"}, // 3.0.0+
            {280, nullptr, "ProxyProcedureToAcquireApplicationAuthorizationForNintendoAccount"},
            {290, &IManagerForSystemService::GetRequestForNintendoAccountUserResourceView, "GetRequestForNintendoAccountUserResourceView"}, // 8.0.0+
            {300, nullptr, "TryRecoverNintendoAccountUserStateAsync"}, // 6.0.0+
            {400, &IManagerForSystemService::IsServiceEntryRequirementCacheRefreshRequiredForOnlinePlay, "IsServiceEntryRequirementCacheRefreshRequiredForOnlinePlay"}, // 6.1.0+
            {401, &IManagerForSystemService::RefreshServiceEntryRequirementCacheForOnlinePlayAsync, "RefreshServiceEntryRequirementCacheForOnlinePlayAsync"}, // 6.1.0+
            {900, nullptr, "GetAuthenticationInfoForWin"}, // 9.0.0+
            {901, nullptr, "ImportAsyncForWin"}, // 9.0.0+
            {997, nullptr, "DebugUnlinkNintendoAccountAsync"},
            {998, nullptr, "DebugSetAvailabilityErrorDetail"},
        };
        // clang-format on

        RegisterHandlers(functions);
        if (administrator) {
            RegisterHandlers(administrator_functions);
        }
    }

private:
    // The module reads su/baas/<uid>.dat (23.0.1 0x085BB4F0, research/acc-fw23 notes §1): no
    // registration at BAAS is 2124-0200, which is what a user made a moment ago has, and
    // PlayerSelect's "Sign In and Link" only links a user that answers 0200. OpenPak's heartbeat
    // signs every active user in with a device account of its own, which the firmware never made:
    // once signed in this process, a user counts as registered when it is linked; before that, when
    // it has a device account in the secret store. Offline: as stock Eden.
    // [OpenPak] 100: the caller names the system program the manager works for (olsc at start).
    // Nothing here depends on it.
    void SetSystemProgramIdentification(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CheckAvailability(HLERequestContext& ctx) {
        const bool registered = IsRegisteredAtBaas();
        LOG_DEBUG(Service_ACC, "called, registered={}", registered);
        PushResult(ctx, registered ? ResultSuccess : ResultNetworkServiceAccountUnavailable);
    }

    // 200: (no input) -> bool (method info in 0x10, out 0x11).
    void IsRegistered(HLERequestContext& ctx) {
        const bool registered = IsRegisteredAtBaas();
        LOG_DEBUG(Service_ACC, "called, registered={}", registered);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(registered);
    }

    // 201: (no input) -> IAsyncContext. The user's device account is made and signed in at
    // OpenPak (Session::Ensure), which is BAAS registration for Harbor.
    void RegisterAsync(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(
            ctx, system, worker,
            [uid = OpenPakAccount::UidString(account_id)](std::stop_token, std::vector<u8>&) {
                if (!OpenPakAccount::Enabled()) {
                    return ResultInternetRequestDenied;
                }
                const bool signed_in = OpenPakAccount::EnsureAndAdopt(uid) &&
                                       OpenPak::Session::NetworkServiceAccountId(uid) != 0;
                LOG_INFO(Service_ACC, "acc: BAAS registration -> {}",
                         signed_in ? "registered" : "failed");
                return signed_in ? ResultSuccess : ResultInternetRequestDenied;
            });
    }

    // 111, 113, 400 and 401: the online-play entitlement check PlayerSelect runs before a game
    // hosts or joins online (401, 113, then 111). OpenPak entitles every linked account, so the
    // cache never needs a refresh, a refresh completes at once, the cache holds no requirement
    // and there is nothing to invalidate.
    void InvalidateServiceEntryRequirementCache(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "unknown contract: acc IAdministrator 111, nothing to invalidate");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetServiceEntryRequirementCacheForOnlinePlay(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "unknown contract: acc IAdministrator 113, answering 0");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
    }

    void IsServiceEntryRequirementCacheRefreshRequiredForOnlinePlay(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "unknown contract: acc IAdministrator 400, answering false");
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(false);
    }

    void RefreshServiceEntryRequirementCacheForOnlinePlayAsync(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC,
                    "unknown contract: acc IAdministrator 401, an async context already done");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(ctx, system, worker,
                                                 OpenPakAsyncContext::Done(ResultSuccess));
    }

    bool IsRegisteredAtBaas() const {
        if (!OpenPakAccount::Enabled()) {
            return true;
        }
        const std::string uid = OpenPakAccount::UidString(account_id);
        // PlayerSelect's "Sign In and Link" asks this right after creating the user, while the
        // setup's account is still being linked to it (OnFirmwareUserRegistered).
        OpenPak::WaitForFirmwareUserLink(uid);
        // A user nobody has signed in this session (not the active one): a device account says
        // nothing about a link, and answering "registered" for it made User Settings show an
        // unlinked user as linked ("Nintendo Account ()"). The sign-in takes ~1.5 s.
        if (OpenPakAccount::SignedInWithin(uid, std::chrono::seconds{5})) {
            return OpenPak::Session::Linked(uid);
        }
        // Offline: what the console last knew of it here.
        return OpenPak::Session::HasDeviceAccount(uid);
    }

    void GetAccountId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(NetworkServiceAccountIdOf(account_id));
    }

    void EnsureIdTokenCacheAsync(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        // The call the guest makes before reading the cache, which makes it the one place a slow
        // network round trip belongs. LoadIdTokenCache is synchronous.
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(
            ctx, system, worker,
            [uid = OpenPakAccount::UidString(account_id)](std::stop_token, std::vector<u8>&) {
                if (OpenPakAccount::Enabled()) {
                    OpenPakAccount::EnsureAndAdopt(uid);
                }
                return ResultSuccess;
            });
    }

    void LoadIdTokenCache(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        LoadIdTokenCacheFor(ctx, account_id);
    }

    void GetNintendoAccountId(HLERequestContext& ctx) {
        std::optional<OpenPakAccount::NintendoAccountInfo> account;
        const Result result = NintendoAccountOf(account_id, account);
        LOG_DEBUG(Service_ACC, "called, linked={}", result.IsSuccess());
        if (result.IsError()) {
            PushResult(ctx, result);
            return;
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(account->id);
    }

    // 136: the Nintendo Account id plus the module's users/me cache as a NasUserBase in the 0x1A
    // buffer. The type-6 buffer is the module's own work memory (it refuses one under 0x800);
    // neither qlaunch nor systemWeb reads it back. Filled as the module's reader would fill it
    // from a users/me holding what the OpenPak login reply carries.
    void RefreshUserResourceAsync(HLERequestContext& ctx) {
        PushUserResourceRefresh(ctx, 0);
    }

    void RefreshUserResourceIfElapsedAsync(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        PushUserResourceRefresh(ctx, rp.Pop<u32>());
    }

    void PushUserResourceRefresh(HLERequestContext& ctx, u32 minimum_seconds) {
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(
            ctx, system, worker,
            [uid = OpenPakAccount::UidString(account_id), minimum_seconds](
                std::stop_token stop, std::vector<u8>&) {
                return OpenPakAccount::RefreshUserResource(uid, minimum_seconds, stop);
            });
    }

    void GetNintendoAccountUserResourceCache(HLERequestContext& ctx) {
        const auto& b = ctx.BufferDescriptorB();
        const auto& c = ctx.BufferDescriptorC();
        if (b.empty() || b[0].Size() < 0x800 || c.empty() || c[0].Size() < NasUserBaseSize) {
            LOG_WARNING(Service_ACC,
                        "acc: GetNintendoAccountUserResourceCache buffers not as expected (map {}, "
                        "pointer {})",
                        b.size(), c.size());
            PushResult(ctx, ResultInvalidBufferSize);
            return;
        }

        std::optional<OpenPakAccount::NintendoAccountInfo> account;
        if (const Result result = NintendoAccountOf(account_id, account); result.IsError()) {
            LOG_DEBUG(Service_ACC, "called, not linked");
            PushResult(ctx, result);
            return;
        }

        // systemWeb splits the birthday at fixed offsets and asserts on a shorter one
        // (0x08d2f21c); without one the honest state is the module's "no users/me cache".
        if (!NintendoAccountOAuth::IsUsableBirthday(account->birthday)) {
            LOG_WARNING(Service_ACC,
                        "acc: the login reply has no usable birthday; Nintendo Account cache "
                        "reported missing");
            PushResult(ctx, ResultNintendoAccountUserResourceCacheNotFound);
            return;
        }

        const auto user =
            NintendoAccountOAuth::NasUserBase(account->nickname, account->birthday, account->country,
                                              account->names);
        ctx.WriteBufferC(user.data(), user.size(), 0);

        // The account's data stays out of the log: it ends up in crash reports.
        LOG_DEBUG(Service_ACC, "called, linked account data written");
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(account->id);
    }

    // 141: () -> IAsyncContext; 142 is the same with a u32 seconds threshold in. 23.0.1
    // 0x0859F2B0 checks first, synchronously, that the user can use the network service
    // (0x085F9EE0: a BAAS registration, then a linked Nintendo Account, else 2124-0220), and only
    // then starts the refresh. PlayerSelect's license check (Save Data Cloud for an unlinked user)
    // reads 0200/0220 as "link first". OpenPak has no paid tier: the refresh of the cache 143
    // reads (Subscribed, no expiry) completes at once for a linked user.
    void RefreshNetworkServiceLicenseCacheAsync(HLERequestContext& ctx) {
        PushLicenseRefresh(ctx);
    }

    void RefreshNetworkServiceLicenseCacheAsyncIfSecondsElapsed(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 seconds = rp.Pop<u32>();
        LOG_WARNING(Service_ACC,
                    "unknown contract: acc IAdministrator 142 ({} s), answered as 141", seconds);
        PushLicenseRefresh(ctx);
    }

    /// The module's network-service precondition (23.0.1 0x085F9EE0): a BAAS registration
    /// (2124-0200), then a linked Nintendo Account (2124-0220). Offline: available, as stock Eden.
    Result NetworkServiceAvailability() const {
        if (!OpenPakAccount::Enabled()) {
            return ResultSuccess;
        }
        // IsActiveLinked only knows the active profile; Session::Linked covers another user
        // signed in this session (IsRegisteredAtBaas waited for that sign-in).
        const bool registered = IsRegisteredAtBaas();
        const bool linked = IsActiveLinked(account_id) ||
                            OpenPak::Session::Linked(OpenPakAccount::UidString(account_id));
        return !registered ? ResultNetworkServiceAccountUnavailable
               : !linked   ? ResultNintendoAccountIdUnavailable
                           : ResultSuccess;
    }

    void PushLicenseRefresh(HLERequestContext& ctx) {
        const Result available = NetworkServiceAvailability();
        LOG_DEBUG(Service_ACC, "called, result={:#x}", available.raw);
        if (available.IsError()) {
            PushResult(ctx, available);
            return;
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(ctx, system, worker,
                                                 OpenPakAsyncContext::Done(ResultSuccess));
    }

    // 143: (u32, u32, u64). 23.0.1 0x0859F6B0 (research/g3pctl-re/notes.md 2d): the same
    // preconditions as 141 and 290 (2124-0200 unregistered, 2124-0220 not linked), then the two
    // u32 are the "default" (Nintendo Switch Online) and "ex" (Expansion Pack) memberships' active
    // flags from the account's membership cache, and the u64 is always 0. qlaunch 22.5.0
    // (+0x3173c0) asserts each u32 is below 2. OpenPak has no paid tier: a linked user is a
    // member, with no Expansion Pack.
    void GetNetworkServiceLicenseCacheEx(HLERequestContext& ctx) {
        const Result available = NetworkServiceAvailability();
        LOG_DEBUG(Service_ACC, "called, result={:#x}", available.raw);
        if (available.IsError()) {
            PushResult(ctx, available);
            return;
        }
        IPC::ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.Push<u32>(1);
        rb.Push<u32>(0);
        rb.Push<u64>(0);
    }

    void IsLinkedWithNintendoAccount(HLERequestContext& ctx) {
        ApplyPendingAdoptions();
        const bool linked = IsActiveLinked(account_id);

        // Link state comes from the server at each boot: the active user's first sign-in of this
        // process is started here when nothing has asked for one yet.
        if (!linked && OpenPakAccount::Enabled() &&
            OpenPakAccount::UidString(account_id) == OpenPak::Platform::ProfileId()) {
            OpenPakAccount::EnsureAndAdoptInBackground(OpenPakAccount::UidString(account_id));
        }

        LOG_DEBUG(Service_ACC, "called, linked={}", linked);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(linked);
    }

    /// HOME's user settings link an existing user (0x085a4d50): a user already linked is
    /// refused, under the rule 250 answers by. The module also wants the user registered at BAAS;
    /// a device account is made for it during the response when there is none.
    void CreateProcedureToLinkWithNintendoAccount(HLERequestContext& ctx) {
        Linkage(ctx, std::nullopt);
    }

    void ResumeProcedureToLinkWithNintendoAccount(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto id = rp.PopRaw<ProcedureSessionId>();
        if (id.IsZero()) {
            PushResult(ctx, ResultSfInvalidArgument);
            return;
        }
        Linkage(ctx, id);
    }

    void Linkage(HLERequestContext& ctx, std::optional<ProcedureSessionId> resume) {
        if (IsActiveLinked(account_id)) {
            PushResult(ctx, ResultNintendoAccountAlreadyLinked);
            return;
        }
        auto procedure =
            std::make_shared<IOAuthProcedureForNintendoAccountLinkage>(system, worker, account_id);
        if (resume) {
            if (const Result result = procedure->Resume(*resume); result.IsError()) {
                PushResult(ctx, result);
                return;
            }
        }
        LOG_INFO(Service_ACC, "acc: Nintendo Account linkage procedure {}",
                 resume ? "resumed" : "created");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IOAuthProcedureForNintendoAccountLinkage>(ctx, std::move(procedure));
    }

    // 290: the page qlaunch's Settings -> Users -> Nintendo Account opens in the Login applet.
    // 23.0.1 (scratch/harbor-eden/research/acc-fw23/notes.md, impl 0x085A6C00): no raw data, one
    // 0x1A buffer of 0x1000; the BAAS registration and the Nintendo Account id are checked
    // (2124-0200, 2124-0220), then "https://accounts.nintendo.com/app/profile?id_token_hint=" and
    // the cached NA id token are written, NUL terminated (2124-0031 when they do not fit). No
    // network call, no object. The hint here is the user's BAAS id_token, which OpenPak's pages
    // accept for a linked user; the module's own NA id token cache is not kept.
    void GetRequestForNintendoAccountUserResourceView(HLERequestContext& ctx) {
        const auto& c = ctx.BufferDescriptorC();
        if (c.empty() || c[0].Size() == 0) {
            PushResult(ctx, ResultInvalidBufferSize);
            return;
        }

        std::string token;
        if (const Result result = OpenPakAccount::LinkedIdToken(account_id, token);
            result.IsError()) {
            LOG_DEBUG(Service_ACC, "called, not linked");
            PushResult(ctx, result);
            return;
        }

        const std::size_t capacity =
            std::min<std::size_t>(c[0].Size(), NintendoAccountOAuth::UserResourceViewUrlSize);
        const auto url = NintendoAccountOAuth::UserResourceViewUrl(token, capacity);
        if (!url) {
            LOG_WARNING(Service_ACC, "acc: the profile page URL does not fit the {}-byte buffer",
                        capacity);
            PushResult(ctx, ResultInvalidBufferSize);
            return;
        }
        std::vector<u8> out(capacity);
        std::memcpy(out.data(), url->data(), url->size());
        ctx.WriteBufferC(out.data(), out.size(), 0);

        LOG_INFO(Service_ACC, "acc: Nintendo Account profile page requested");
        PushResult(ctx, ResultSuccess);
    }

    void ApplyPendingAdoptions() {
        OpenPakAccount::ApplyPendingAdoptions(system.GetProfileManager());
    }

    Common::UUID account_id;
    std::shared_ptr<OpenPak::AsyncWorker> worker;
};

// 3.0.0+
class IFloatingRegistrationRequest final : public ServiceFramework<IFloatingRegistrationRequest> {
public:
    explicit IFloatingRegistrationRequest(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IFloatingRegistrationRequest"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSessionId"},
            {12, nullptr, "GetAccountId"},
            {13, nullptr, "GetLinkedNintendoAccountId"},
            {14, nullptr, "GetNickname"},
            {15, nullptr, "GetProfileImage"},
            {16, nullptr, "GetProfileLargeImage"}, // 18.0.0+
            {21, nullptr, "LoadIdTokenCache"},
            {100, nullptr, "RegisterUser"}, // [1.0.0-3.0.2] RegisterAsync
            {101, nullptr, "RegisterUserWithUid"}, // [1.0.0-3.0.2] RegisterWithUidAsync
            {102, nullptr, "RegisterNetworkServiceAccountAsync"}, // 4.0.0+
            {103, nullptr, "RegisterNetworkServiceAccountWithUidAsync"}, // 4.0.0+
            {110, nullptr, "SetSystemProgramIdentification"},
            {111, nullptr, "EnsureIdTokenCacheAsync"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IOAuthProcedure final : public ServiceFramework<IOAuthProcedure> {
public:
    explicit IOAuthProcedure(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IOAuthProcedure"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "PrepareAsync"},
            {1, nullptr, "GetRequest"},
            {2, nullptr, "ApplyResponse"},
            {3, nullptr, "ApplyResponseAsync"},
            {10, nullptr, "Suspend"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

// 3.0.0+
class IOAuthProcedureForExternalNsa final : public ServiceFramework<IOAuthProcedureForExternalNsa> {
public:
    explicit IOAuthProcedureForExternalNsa(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IOAuthProcedureForExternalNsa"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "PrepareAsync"},
            {1, nullptr, "GetRequest"},
            {2, nullptr, "ApplyResponse"},
            {3, nullptr, "ApplyResponseAsync"},
            {10, nullptr, "Suspend"},
            {100, nullptr, "GetAccountId"},
            {101, nullptr, "GetLinkedNintendoAccountId"},
            {102, nullptr, "GetNickname"},
            {103, nullptr, "GetProfileImage"},
            {104, nullptr, "GetProfileLargeImage"} // 18.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class INotifier final : public ServiceFramework<INotifier> {
public:
    explicit INotifier(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "INotifier"}, service_context{system_, "INotifier"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INotifier::GetSystemEvent, "GetSystemEvent"},
        };
        // clang-format on

        RegisterHandlers(functions);
        event = service_context.CreateEvent("INotifier:Event");
    }

    ~INotifier() override {
        service_context.CloseEvent(event);
    }

private:
    // [OpenPak] Never signalled: the firmware's own friends program waits on these (user
    // registration, state change, profile update) and Harbor's account changes do not raise them.
    void GetSystemEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
};

class IProfileCommon : public ServiceFramework<IProfileCommon> {
public:
    explicit IProfileCommon(Core::System& system_, const char* name, bool editor_commands,
                            Common::UUID user_id_, ProfileManager& profile_manager_)
        : ServiceFramework{system_, name}, profile_manager{profile_manager_}, user_id{user_id_} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IProfileCommon::Get, "Get"},
            {1, &IProfileCommon::GetBase, "GetBase"},
            {10, &IProfileCommon::GetImageSize, "GetImageSize"},
            {11, &IProfileCommon::LoadImage, "LoadImage"},
            {20, &IProfileCommon::Unknown20, "Unknown20"},
            {21, &IProfileCommon::Unknown21, "Unknown21"},
            {30, &IProfileCommon::GetImageId, "GetImageId"}
        };
        // clang-format on
        RegisterHandlers(functions);

        if (editor_commands) {
            // clang-format off
            static const FunctionInfo editor_functions[] = {
                {100, &IProfileCommon::Store, "Store"},
                {101, &IProfileCommon::StoreWithImage, "StoreWithImage"},
                {110, &IProfileCommon::Unknown110, "Unknown110"}
            };
            // clang-format on

            RegisterHandlers(editor_functions);
        }
    }

protected:
    void Unknown20(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        // TODO (jarrodnorwell)
        // inbytes: 0x0, outbytes: 0x4

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Unknown21(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        // TODO (jarrodnorwell)
        // buffers: [0x6], inbytes: 0x0, outbytes: 0x4

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // qlaunch (RdtAccountManager::update_) reloads the HOME icon only when these 16 bytes change.
    void GetImageId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        OpenPakAccount::ApplyPendingAdoptions(profile_manager);

        std::array<u8, 0x10> id{};
        const Common::FS::IOFile image(GetImagePath(user_id), Common::FS::FileAccessMode::Read,
                                       Common::FS::FileType::BinaryFile);
        if (image.IsOpen()) {
            std::vector<u8> buffer(image.GetSize());
            if (image.Read(buffer) == buffer.size()) {
                id = ProfileManager::ImageId(buffer);
            }
        } else {
            id = ProfileManager::ImageId(Core::Constants::ACCOUNT_BACKUP_JPEG);
        }

        IPC::ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.PushRaw(id);
    }

    void Unknown110(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "(STUBBED) called.");

        // TODO (jarrodnorwell)
        // buffer_entry_sizes: [0x80, 0x0], buffers: [0x19, 0x5], inbytes: 0x38, outbytes: 0x0

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Get(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        OpenPakAccount::ApplyPendingAdoptions(profile_manager);
        ProfileBase profile_base{};
        UserData data{};
        if (profile_manager.GetProfileBaseAndData(user_id, profile_base, data)) {
            ctx.WriteBuffer(data);
            IPC::ResponseBuilder rb{ctx, 16};
            rb.Push(ResultSuccess);
            rb.PushRaw(profile_base);
        } else {
            LOG_ERROR(Service_ACC, "Failed to get profile base and data for a user");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown); // TODO(ogniK): Get actual error code
        }
    }

    void GetBase(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        OpenPakAccount::ApplyPendingAdoptions(profile_manager);
        ProfileBase profile_base{};
        if (profile_manager.GetProfileBase(user_id, profile_base)) {
            IPC::ResponseBuilder rb{ctx, 16};
            rb.Push(ResultSuccess);
            rb.PushRaw(profile_base);
        } else {
            LOG_ERROR(Service_ACC, "Failed to get profile base for a user");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown); // TODO(ogniK): Get actual error code
        }
    }

    void LoadImage(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        OpenPakAccount::ApplyPendingAdoptions(profile_manager);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);

        const Common::FS::IOFile image(GetImagePath(user_id), Common::FS::FileAccessMode::Read,
                                       Common::FS::FileType::BinaryFile);
        if (!image.IsOpen()) {
            LOG_WARNING(Service_ACC,
                        "Failed to load user provided image! Falling back to built-in backup...");
            ctx.WriteBuffer(Core::Constants::ACCOUNT_BACKUP_JPEG);
            rb.Push(static_cast<u32>(Core::Constants::ACCOUNT_BACKUP_JPEG.size()));
            return;
        }

        std::vector<u8> buffer(image.GetSize());

        if (image.Read(buffer) != buffer.size()) {
            LOG_ERROR(Service_ACC, "Failed to read all the bytes in the user provided image.");
        }

        SanitizeJPEGImageSize(buffer);

        ctx.WriteBuffer(buffer);
        rb.Push(static_cast<u32>(buffer.size()));
    }

    void GetImageSize(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        OpenPakAccount::ApplyPendingAdoptions(profile_manager);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);

        const Common::FS::IOFile image(GetImagePath(user_id), Common::FS::FileAccessMode::Read,
                                       Common::FS::FileType::BinaryFile);

        if (!image.IsOpen()) {
            LOG_WARNING(Service_ACC,
                        "Failed to load user provided image! Falling back to built-in backup...");
            rb.Push(static_cast<u32>(Core::Constants::ACCOUNT_BACKUP_JPEG.size()));
            return;
        }

        std::vector<u8> buffer(image.GetSize());

        if (image.Read(buffer) != buffer.size()) {
            LOG_ERROR(Service_ACC, "Failed to read all the bytes in the user provided image.");
        }

        SanitizeJPEGImageSize(buffer);
        rb.Push(static_cast<u32>(buffer.size()));
    }

    void LoadIdTokenCache(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "(STUBBED) called");

        std::vector<u8> token_data(0x100);
        std::fill(token_data.begin(), token_data.end(), u8(0));

        (void)ctx.WriteBuffer(token_data);

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(static_cast<u32>(token_data.size()));
    }

    void GetNintendoAccountUserResourceCacheForApplication(HLERequestContext& ctx) {
        LOG_WARNING(Service_ACC, "(STUBBED) called");

        std::vector<u8> nas_user_base_for_application(0x68);
        (void)ctx.WriteBuffer(nas_user_base_for_application);

        if (ctx.CanWriteBuffer(1)) {
            std::vector<u8> unknown_out_buffer(ctx.GetWriteBufferSize(1));
            (void)ctx.WriteBuffer(unknown_out_buffer, 1);
        }

        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw<u64>(profile_manager.GetLastOpenedUser().Hash());
    }

    void Store(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto base = rp.PopRaw<ProfileBase>();

        const auto user_data = ctx.ReadBuffer();

        LOG_DEBUG(Service_ACC, "called");

        if (user_data.size() < sizeof(UserData)) {
            LOG_ERROR(Service_ACC, "UserData buffer too small!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultInvalidArrayLength);
            return;
        }

        UserData data;
        std::memcpy(&data, user_data.data(), sizeof(UserData));

        const ProfileUsername before = CurrentUsername();
        if (!profile_manager.SetProfileBaseAndData(user_id, base, data)) {
            LOG_ERROR(Service_ACC, "Failed to update user data and base!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultAccountUpdateFailed);
            return;
        }

        SendNicknameIfChanged(before, base);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void StoreWithImage(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto base = rp.PopRaw<ProfileBase>();

        const auto image_data = ctx.ReadBufferA(0);
        const auto user_data = ctx.ReadBufferX(0);

        LOG_INFO(Service_ACC, "called, image_size={}", image_data.size());

        if (user_data.size() < sizeof(UserData)) {
            LOG_ERROR(Service_ACC, "UserData buffer too small!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultInvalidArrayLength);
            return;
        }

        UserData data;
        std::memcpy(&data, user_data.data(), sizeof(UserData));

        // A new console has no su/avators yet: the setup's first user is stored with its image
        // before profiles.dat is ever written.
        const ProfileUsername before = CurrentUsername();
        const auto image_path = GetImagePath(user_id);
        if (!Common::FS::CreateParentDirs(image_path)) {
            LOG_ERROR(Service_ACC, "Could not create the profile image directory");
        }
        Common::FS::IOFile image(image_path, Common::FS::FileAccessMode::Write,
                                 Common::FS::FileType::BinaryFile);

        if (!image.IsOpen() || !image.SetSize(image_data.size()) ||
            image.Write(image_data) != image_data.size() ||
            !profile_manager.SetProfileBaseAndData(user_id, base, data)) {
            LOG_ERROR(Service_ACC, "Failed to update profile data, base, and image!");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(Account::ResultAccountUpdateFailed);
            return;
        }

        // A real console's account module sends the picture and the nickname on; here nothing
        // else does.
        SendNicknameIfChanged(before, base);
        if (OpenPakAccount::Enabled()) {
            OpenPakAccount::UploadProfileImageInBackground(
                OpenPakAccount::UidString(user_id),
                std::vector<u8>(image_data.begin(), image_data.end()));
        }

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    /// The nickname the profile holds now; empty when it has no profile yet (the setup's user).
    ProfileUsername CurrentUsername() const {
        ProfileBase base{};
        UserData data{};
        return profile_manager.GetProfileBaseAndData(user_id, base, data) ? base.username
                                                                           : ProfileUsername{};
    }

    void SendNicknameIfChanged(const ProfileUsername& before, const ProfileBase& after) const {
        if (OpenPakAccount::Enabled() && before != after.username) {
            OpenPakAccount::UpdateNicknameInBackground(OpenPakAccount::UidString(user_id),
                                                       after.username);
        }
    }

    ProfileManager& profile_manager;
    Common::UUID user_id{}; ///< The user id this profile refers to.
};

class IProfile final : public IProfileCommon {
public:
    explicit IProfile(Core::System& system_, Common::UUID user_id_,
                      ProfileManager& profile_manager_)
        : IProfileCommon{system_, "IProfile", false, user_id_, profile_manager_} {}
};

class IProfileEditor final : public IProfileCommon {
public:
    explicit IProfileEditor(Core::System& system_, Common::UUID user_id_,
                            ProfileManager& profile_manager_)
        : IProfileCommon{system_, "IProfileEditor", true, user_id_, profile_manager_} {}
};

class ISessionObject final : public ServiceFramework<ISessionObject> {
public:
    explicit ISessionObject(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "ISessionObject"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {999, nullptr, "Dummy"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class IGuestLoginRequest final : public ServiceFramework<IGuestLoginRequest> {
public:
    explicit IGuestLoginRequest(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IGuestLoginRequest"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetSessionId"},
            {11, nullptr, "Unknown"}, // 1.0.0 - 2.3.0 (the name is blank on Switchbrew)
            {12, nullptr, "GetAccountId"},
            {13, nullptr, "GetLinkedNintendoAccountId"},
            {14, nullptr, "GetNickname"},
            {15, nullptr, "GetProfileImage"},
            {16, nullptr, "GetProfileLargeImage"}, // 18.0.0+
            {21, nullptr, "LoadIdTokenCache"}, // 3.0.0+
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

// [Harbor] nn::account::nas::IAuthorizationRequest: the title-driven half of the console
// authorization flow. A title that talks to a third-party network (Battle.net for Diablo II:
// Resurrected, EA's Nucleus for Plants vs. Zombies: Battle for Neighborville) asks the account
// system to authorize it against the signed-in user, then collects the proof, an authorization
// code and the id_token, to hand to that network.
//
// The console does this with the silent authorize (prompt=none, response_type "code id_token")
// against accounts.nintendo.com, which nx-baas answers with a redirect whose fragment carries
// state, code and id_token. The request keeps the title's parameters (scope, state, nonce),
// runs that authorize in InvokeWithoutInteractionAsync, and hands the results out: GetState the
// title's own state back, GetAuthorizationCode the code (a 0x200 buffer), GetIdToken the NA
// id_token (0x600). When the authorize cannot be done, both proofs fall back to the user's
// OpenPak id_token, the credential OpenPak's check_token resolves.
class IAuthorizationRequest final : public ServiceFramework<IAuthorizationRequest> {
public:
    /// What the request holds; the async work fills it in while the guest polls.
    struct Held {
        std::mutex mutex;
        std::string scope;
        std::string state;
        std::string nonce;
        std::string code;
        std::string id_token;
    };

    explicit IAuthorizationRequest(Core::System& system_, Common::UUID user_,
                                   std::shared_ptr<OpenPak::AsyncWorker> worker_,
                                   std::vector<u8> parameters)
        : ServiceFramework{system_, "IAuthorizationRequest"}, user{user_},
          worker{std::move(worker_)}, held{std::make_shared<Held>()},
          session_id{std::random_device{}() | (u64{std::random_device{}()} << 32)} {
        // NintendoAccountAuthorizationRequestParameters: scope[0x100] at +0, state[0x80] at
        // +0x100, nonce[0x80] after it; each a NUL-terminated string.
        const auto field = [&parameters](std::size_t offset, std::size_t size) {
            std::string out;
            for (std::size_t i = offset; i < offset + size && i < parameters.size(); ++i) {
                if (parameters[i] == 0) {
                    break;
                }
                out.push_back(static_cast<char>(parameters[i]));
            }
            return out;
        };
        held->scope = field(0, 0x100);
        held->state = field(0x100, 0x80);
        held->nonce = field(0x180, 0x80);
        LOG_INFO(Service_ACC, "acc: authorization request (scope '{}', state {} bytes, nonce {} bytes)",
                 held->scope, held->state.size(), held->nonce.size());
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IAuthorizationRequest::GetSessionId, "GetSessionId"},
            {10, &IAuthorizationRequest::InvokeWithoutInteractionAsync, "InvokeWithoutInteractionAsync"},
            {19, &IAuthorizationRequest::IsAuthorized, "IsAuthorized"},
            {20, &IAuthorizationRequest::GetAuthorizationCode, "GetAuthorizationCode"},
            {21, &IAuthorizationRequest::GetIdToken, "GetIdToken"},
            {22, &IAuthorizationRequest::GetState, "GetState"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    static std::string UrlEncode(std::string_view in) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        for (const unsigned char c : in) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            }
        }
        return out;
    }

    static std::string UrlDecode(std::string_view in) {
        std::string out;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
                out.push_back(static_cast<char>(std::stoi(std::string{in.substr(i + 1, 2)}, nullptr, 16)));
                i += 2;
            } else if (in[i] == '+') {
                out.push_back(' ');
            } else {
                out.push_back(in[i]);
            }
        }
        return out;
    }

    /// The silent authorize, as the account module sends it. True when the answer carried a code
    /// and an id_token and echoed the title's state.
    static bool Authorize(const std::string& uid, u64 program_id, Held& held) {
        if (!OpenPak::Session::Enabled() || program_id == 0) {
            return false;
        }
        OpenPak::Session::Ensure(uid);
        const std::string bearer = OpenPak::Session::IdToken(uid);
        if (bearer.empty()) {
            return false;
        }
        const std::string client = fmt::format("{:016x}", program_id);
        std::string scope, state, nonce;
        {
            std::scoped_lock lock{held.mutex};
            scope = held.scope;
            state = held.state;
            nonce = held.nonce;
        }
        std::string body = "prompt=none&response_type=" + UrlEncode("code id_token") +
                           "&client_id=" + client +
                           "&redirect_uri=" + UrlEncode("npf" + client + "://auth") +
                           "&scope=" + UrlEncode(scope) + "&state=" + UrlEncode(state) +
                           "&nonce=" + UrlEncode(nonce);
        const auto reply = OpenPak::Session::Send(
            "POST", "accounts.nintendo.com", "/connect/1.0.0/authorize",
            {{"Authorization", "Bearer " + bearer}}, "application/x-www-form-urlencoded", body);
        const auto hash = reply.location.find('#');
        if (reply.status / 100 != 3 || hash == std::string::npos) {
            LOG_WARNING(Service_ACC, "acc: silent authorize answered {}", reply.status);
            return false;
        }
        std::string code, id_token, echoed, error;
        std::string_view fragment{reply.location};
        fragment.remove_prefix(hash + 1);
        while (!fragment.empty()) {
            const auto amp = fragment.find('&');
            const std::string_view pair = fragment.substr(0, amp);
            const auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                const std::string_view key = pair.substr(0, eq);
                const std::string value = UrlDecode(pair.substr(eq + 1));
                if (key == "code") {
                    code = value;
                } else if (key == "id_token") {
                    id_token = value;
                } else if (key == "state") {
                    echoed = value;
                } else if (key == "error") {
                    error = value;
                }
            }
            fragment = amp == std::string_view::npos ? std::string_view{} : fragment.substr(amp + 1);
        }
        if (!error.empty() || code.empty() || id_token.empty()) {
            LOG_WARNING(Service_ACC, "acc: silent authorize refused ({})",
                        error.empty() ? "no code" : error);
            return false;
        }
        std::scoped_lock lock{held.mutex};
        held.code = std::move(code);
        held.id_token = std::move(id_token);
        if (!echoed.empty()) {
            held.state = std::move(echoed);
        }
        return true;
    }

    void GetSessionId(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(session_id);
    }

    void InvokeWithoutInteractionAsync(HLERequestContext& ctx) {
        // The title polls the async shape; the work is the silent authorize for the user.
        const u64 program_id = system.GetApplicationProcessProgramID();
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(
            ctx, system, worker,
            [uid = OpenPakAccount::UidString(user), program_id,
             held = held](std::stop_token, std::vector<u8>&) {
                if (OpenPakAccount::Enabled()) {
                    OpenPak::Session::Ensure(uid);
                    if (Authorize(uid, program_id, *held)) {
                        LOG_INFO(Service_ACC, "acc: authorization request done (code and id_token)");
                    }
                }
                return ResultSuccess;
            });
    }

    void IsAuthorized(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u8>(1);
    }

    /// Writes `value` to the guest's buffer when it has one; the size is always answered, so a
    /// title that only wants the size asks with no buffer.
    void AnswerString(HLERequestContext& ctx, const std::string& value) {
        if (ctx.CanWriteBuffer() && ctx.GetWriteBufferSize() > 0 && !value.empty()) {
            if (value.size() > ctx.GetWriteBufferSize()) {
                LOG_WARNING(Service_ACC, "acc: answer is {} bytes, guest buffer is {}",
                            value.size(), ctx.GetWriteBufferSize());
                PushResult(ctx, ResultInvalidBufferSize);
                return;
            }
            ctx.WriteBuffer(value.data(), value.size());
        }
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>(value.size()));
    }

    std::string Fallback() const {
        return OpenPakAccount::Enabled()
                   ? OpenPak::Session::IdToken(OpenPakAccount::UidString(user))
                   : std::string{};
    }

    void GetAuthorizationCode(HLERequestContext& ctx) {
        std::string code;
        {
            std::scoped_lock lock{held->mutex};
            code = held->code;
        }
        AnswerString(ctx, code.empty() ? Fallback() : code);
    }

    void GetIdToken(HLERequestContext& ctx) {
        std::string token;
        {
            std::scoped_lock lock{held->mutex};
            token = held->id_token;
        }
        AnswerString(ctx, token.empty() ? Fallback() : token);
    }

    void GetState(HLERequestContext& ctx) {
        std::string state;
        {
            std::scoped_lock lock{held->mutex};
            state = held->state;
        }
        AnswerString(ctx, state);
    }

    Common::UUID user;
    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<Held> held;
    u64 session_id;
};

class IManagerForApplication final : public ServiceFramework<IManagerForApplication> {
public:
    explicit IManagerForApplication(Core::System& system_, Common::UUID user_id_,
                                    std::shared_ptr<OpenPak::AsyncWorker> worker_)
        : ServiceFramework{system_, "IManagerForApplication"}, user_id{user_id_},
          worker{std::move(worker_)} {
        // 23.0.1 acc opens the user in the caller's context when it creates this object
        // (0x085AFFF0 -> 0x085B9B40) and closes it when the object goes (0x085B07B0 ->
        // 0x085B9E90); nn::account::OpenUser and OpenPreselectedUser hold it while the user is
        // open.
        system.GetProfileManager().OpenUserForApplication(user_id);
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IManagerForApplication::CheckAvailability, "CheckAvailability"},
            {1, &IManagerForApplication::GetAccountId, "GetAccountId"},
            {2, &IManagerForApplication::EnsureIdTokenCacheAsync, "EnsureIdTokenCacheAsync"},
            // Titles built against older SDKs (Crash Team Racing Nitro-Fueled) fetch their
            // id_token through 3: the same token as 4, or Demonware gets an empty login token.
            {3, &IManagerForApplication::LoadIdTokenCache, "LoadIdTokenCacheDeprecated"},
            {4, &IManagerForApplication::LoadIdTokenCache, "LoadIdTokenCache"},
            {130, &IManagerForApplication::GetNintendoAccountUserResourceCacheForApplication, "GetNintendoAccountUserResourceCacheForApplication"},
            {136, &IManagerForApplication::GetNintendoAccountUserResourceCacheForApplication, "GetNintendoAccountUserResourceCache"}, // 19.0.0+
            {150, &IManagerForApplication::CreateAuthorizationRequest, "CreateAuthorizationRequest"},
            {160, &IManagerForApplication::StoreOpenContext, "StoreOpenContext"},
            {170, &IManagerForApplication::LoadNetworkServiceLicenseKindAsync, "LoadNetworkServiceLicenseKindAsync"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void CheckAvailability(HLERequestContext& ctx) {
        const bool unlinked = SignedInUnlinked(user_id);
        LOG_DEBUG(Service_ACC, "called, unlinked={}", unlinked);
        PushResult(ctx, unlinked ? ResultNetworkServiceAccountUnavailable : ResultSuccess);
    }

    void GetAccountId(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push(NetworkServiceAccountIdOf(user_id));
    }

    void EnsureIdTokenCacheAsync(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(ctx, system, worker, EnsureWork());
    }

    void LoadIdTokenCache(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        if (SignedInUnlinked(user_id)) {
            LOG_INFO(Service_ACC, "acc: the user is not linked; no id_token for the title");
            PushResult(ctx, ResultNetworkServiceAccountUnavailable);
            return;
        }
        if (const auto refused = AauthRefused(user_id)) {
            PushResult(ctx, *refused);
            return;
        }
        LoadIdTokenCacheFor(ctx, user_id);
    }

    // 130/136: (u64 Nintendo Account id, NasUserBaseForApplication 0x68, work buffer >= 0x800).
    // The account module (23.0.1, 0x1fa5c0) reads the users/me cache into the work buffer
    // (NasUserBase at its head) and copies birthday, gender, language, country, region and
    // timezone out of it; MK8D shows the country as the player's flag.
    void GetNintendoAccountUserResourceCacheForApplication(HLERequestContext& ctx) {
        // The 0x68 out is an auto-select (0x1A) buffer: MK8D sends it as a pointer (C) buffer,
        // which leaves the work buffer as the only mapped (B) one. Sent mapped, it comes first.
        const bool pointer_out =
            !ctx.BufferDescriptorC().empty() && ctx.BufferDescriptorC()[0].Size() != 0;
        const std::size_t work_index = pointer_out ? 0 : 1;
        const std::size_t work_size = ctx.BufferDescriptorB().size() > work_index
                                          ? ctx.BufferDescriptorB()[work_index].Size()
                                          : 0;
        // The module's answer for a work buffer under 0x800.
        constexpr Result ResultWorkBufferTooSmall{ErrorModule::Account, 31};
        if (work_size < 0x800) {
            PushResult(ctx, ResultWorkBufferTooSmall);
            return;
        }
        std::optional<OpenPakAccount::NintendoAccountInfo> account;
        if (const Result result = NintendoAccountOf(user_id, account); result.IsError()) {
            LOG_DEBUG(Service_ACC, "called, not linked");
            PushResult(ctx, result);
            return;
        }

        const auto for_application = NintendoAccountOAuth::NasUserBaseForApplication(
            account->birthday, account->country, account->names);
        if (pointer_out) {
            ctx.WriteBufferC(for_application.data(), for_application.size());
        } else {
            ctx.WriteBufferB(for_application.data(), for_application.size());
        }
        std::vector<u8> work(work_size);
        const auto base = NintendoAccountOAuth::NasUserBase(account->nickname, account->birthday,
                                                            account->country, account->names);
        std::memcpy(work.data(), base.data(), base.size());
        ctx.WriteBufferB(work.data(), work.size(), work_index);

        // The account's data stays out of the log.
        LOG_DEBUG(Service_ACC, "called, linked account data written ({} out, work {:#x})",
                  pointer_out ? "pointer" : "mapped", work_size);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.PushRaw<u64>(account->id);
    }

    void CreateAuthorizationRequest(HLERequestContext& ctx) {
        // The request input carries the title's authorization parameters (scope, state, nonce).
        LOG_DEBUG(Service_ACC, "called");
        std::vector<u8> parameters;
        if (ctx.CanReadBuffer()) {
            parameters = ctx.ReadBufferCopy();
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IAuthorizationRequest>(ctx, system, user_id, worker,
                                                   std::move(parameters));
    }

    void StoreOpenContext(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        system.GetProfileManager().StoreOpenedUsers();
        PushResult(ctx, ResultSuccess);
    }

    void LoadNetworkServiceLicenseKindAsync(HLERequestContext& ctx) {
        LOG_DEBUG(Service_ACC, "called");
        // An extension of the data the id token cache holds: Subscribed once it is there.
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(ctx, system, worker, EnsureWork(), true);
    }

    OpenPak::AsyncWorker::Work EnsureWork() const {
        return [user = user_id](std::stop_token, std::vector<u8>&) {
            if (!OpenPakAccount::Enabled()) {
                return ResultSuccess;
            }
            OpenPakAccount::EnsureAndAdopt(OpenPakAccount::UidString(user));
            if (const auto refused = AauthRefused(user)) {
                return *refused;
            }
            // Signed in but not linked: the console's answer for a user without a network service
            // account, which sends the title to the firmware's link prompt.
            return SignedInUnlinked(user) ? ResultNetworkServiceAccountUnavailable : ResultSuccess;
        };
    }

    Common::UUID user_id; ///< The user the title asked about.
    std::shared_ptr<OpenPak::AsyncWorker> worker;

public:
    ~IManagerForApplication() override {
        system.GetProfileManager().CloseUserForApplication(user_id);
    }
};

class DAUTH_O final : public ServiceFramework<DAUTH_O> {
public:
    explicit DAUTH_O(Core::System& system_, Common::UUID) : ServiceFramework{system_, "dauth:o"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "EnsureAuthenticationTokenCacheAsync"},
            {1, nullptr, "LoadAuthenticationTokenCache"},
            {2, nullptr, "InvalidateAuthenticationTokenCache"},
            {3, nullptr, "IsDeviceAuthenticationTokenCacheAvailable"},
            {10, nullptr, "EnsureEdgeTokenCacheAsync"},
            {11, nullptr, "LoadEdgeTokenCache"},
            {12, nullptr, "InvalidateEdgeTokenCache"},
            {13, nullptr, "IsEdgeTokenCacheAvailable"},
            {20, nullptr, "EnsureApplicationAuthenticationCacheAsync"},
            {21, nullptr, "LoadApplicationAuthenticationTokenCache"},
            {22, nullptr, "LoadApplicationNetworkServiceClientConfigCache"},
            {23, nullptr, "IsApplicationAuthenticationCacheAvailable"},
            {24, nullptr, "InvalidateApplicationAuthenticationCache"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

// 6.0.0+
class IAsyncResult final : public ServiceFramework<IAsyncResult> {
public:
    explicit IAsyncResult(Core::System& system_, Common::UUID)
        : ServiceFramework{system_, "IAsyncResult"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetResult"},
            {1, nullptr, "Cancel"},
            {2, nullptr, "IsAvailable"},
            {3, nullptr, "GetSystemEvent"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void Module::Interface::GetUserCount(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(static_cast<u32>(profile_manager->GetUserCount()));
}

void Module::Interface::GetUserExistence(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(profile_manager->UserExists(user_id));
}

void Module::Interface::ListAllUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    ctx.WriteBuffer(profile_manager->GetAllUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ListOpenUsers(HLERequestContext& ctx) {
    const auto open_users = profile_manager->GetOpenUsers();
    std::string positions;
    for (const auto& uuid : open_users) {
        if (const auto index =
                uuid.IsValid() ? profile_manager->GetUserIndex(uuid) : std::nullopt) {
            positions += fmt::format(" #{}", *index);
        }
    }
    LOG_DEBUG(Service_ACC, "called, open users:{}", positions.empty() ? " none" : positions);
    ctx.WriteBuffer(open_users);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::GetLastOpenedUser(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw<Common::UUID>(profile_manager->GetLastOpenedUser());
}

void Module::Interface::GetProfile(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called");
    OpenPakAccount::ApplyPendingAdoptions(*profile_manager);

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IProfile>(ctx, system, user_id, *profile_manager);
}

void Module::Interface::IsUserRegistrationRequestPermitted(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(profile_manager->CanSystemRegisterUser());
}

void Module::Interface::InitializeApplicationInfo(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(InitializeApplicationInfoBase());
}

void Module::Interface::InitializeApplicationInfoRestricted(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(Partial implementation) called");

    // TODO(ogniK): We require checking if the user actually owns the title and what not. As of
    // currently, we assume the user owns the title. InitializeApplicationInfoBase SHOULD be called
    // first then we do extra checks if the game is a digital copy.

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(InitializeApplicationInfoBase());
}

Result Module::Interface::InitializeApplicationInfoBase() {
    if (application_info) {
        LOG_ERROR(Service_ACC, "Application already initialized");
        return Account::ResultApplicationInfoAlreadyInitialized;
    }

    // TODO(ogniK): This should be changed to reflect the target process for when we have multiple
    // processes emulated. As we don't actually have pid support we should assume we're just using
    // our own process
    Glue::ApplicationLaunchProperty launch_property{};
    const auto result = system.GetARPManager().GetLaunchProperty(
        &launch_property, system.GetApplicationProcessProgramID());

    if (result != ResultSuccess) {
        LOG_ERROR(Service_ACC, "Failed to get launch property");
        return Account::ResultInvalidApplication;
    }

    switch (launch_property.base_game_storage_id) {
    case FileSys::StorageId::GameCard:
        application_info.application_type = ApplicationType::GameCard;
        break;
    case FileSys::StorageId::Host:
    case FileSys::StorageId::NandSystem:
    case FileSys::StorageId::NandUser:
    case FileSys::StorageId::SdCard:
    case FileSys::StorageId::None: // Yuzu specific, differs from hardware
        application_info.application_type = ApplicationType::Digital;
        break;
    default:
        LOG_ERROR(Service_ACC, "Invalid game storage ID! storage_id={}",
                  launch_property.base_game_storage_id);
        return Account::ResultInvalidApplication;
    }

    LOG_WARNING(Service_ACC, "ApplicationInfo init required");
    // TODO(ogniK): Actual initialization here

    return ResultSuccess;
}

void Module::Interface::GetBaasAccountManagerForApplication(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    auto user_id = rp.PopRaw<Common::UUID>();
    if (user_id.IsInvalid()) {
        user_id = profile_manager->GetLastOpenedUser();
    }
    LOG_DEBUG(Service_ACC, "called");

    // The title's sign-in is bound to it, as a console's is.
    if (OpenPakAccount::Enabled()) {
        OpenPakAccount::BindRunningApplication(system);
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IManagerForApplication>(ctx, system, user_id, module->worker);
}

void Module::Interface::IsUserAccountSwitchLocked(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    FileSys::NACP nacp;
    const auto res = system.GetAppLoader().ReadControlData(nacp);

    bool is_locked = false;

    if (res != Loader::ResultStatus::Success) {
        const FileSys::PatchManager pm{system.GetApplicationProcessProgramID(),
                                       system.GetFileSystemController(),
                                       system.GetContentProvider()};
        const auto nacp_unique = pm.GetControlMetadata().first;

        if (nacp_unique != nullptr) {
            is_locked = nacp_unique->GetUserAccountSwitchLock();
        } else {
            LOG_ERROR(Service_ACC, "nacp_unique is null!");
        }
    } else {
        is_locked = nacp.GetUserAccountSwitchLock();
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(is_locked);
}

void Module::Interface::InitializeApplicationInfoV2(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::BeginUserRegistration(HLERequestContext& ctx) {
    // A nameless user: the firmware's user creation names it through IProfileEditor.
    const auto user_id = Common::UUID::MakeRandom();
    const auto result = profile_manager->BeginUserRegistration(user_id);

    LOG_INFO(Service_ACC, "acc: BeginUserRegistration -> new user, {} user(s)",
             profile_manager->GetUserCount());

    if (result.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
        return;
    }

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(user_id);
}

void Module::Interface::CompleteUserRegistration(HLERequestContext& ctx) {
    // Also acc:su 206 CompleteUserRegistrationForcibly, the first-time setup's variant.
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    IPC::ResponseBuilder rb{ctx, 2};
    if (!profile_manager->CompleteUserRegistration(user_id)) {
        LOG_WARNING(Service_ACC, "acc: CompleteUserRegistration of a user that does not exist");
        constexpr Result ResultUserNotFound{ErrorModule::Account, 100};
        rb.Push(ResultUserNotFound);
        return;
    }

    LOG_INFO(Service_ACC, "acc: CompleteUserRegistration, {} user(s), saved",
             profile_manager->GetUserCount());
    // The wizard's OpenPak sign-in links the first firmware user to that account (it returns at
    // once and does nothing unless OpenPak is on and signed in).
    const std::string uid = user_id.RawString();
    OpenPak::OnFirmwareUserRegistered(uid, [uid](const std::string& id_token) {
        OpenPakAccount::KeepNamesOfLink(uid, id_token);
    });
    rb.Push(ResultSuccess);
}

void Module::Interface::DeleteUser(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();
    LOG_INFO(Service_ACC, "called");
    if (!profile_manager->RemoveUser(user_id)) {
        LOG_ERROR(Service_ACC, "Failed to delete a user");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(1U);
        return;
    }
    // Its device account and push registration go with it: no sign-in outlives the user.
    OpenPak::Session::ForgetProfile(OpenPakAccount::UidString(user_id));

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::SetUserPosition(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    u64 position = rp.Pop<u64>();
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_ACC, "called, position={}", position);

    profile_manager->SetUserPosition(position, user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::GetProfileEditor(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    Common::UUID user_id = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_ACC, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IProfileEditor>(ctx, system, user_id, *profile_manager);
}

void Module::Interface::GetBaasAccountAdministrator(HLERequestContext &ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_ACC, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IManagerForSystemService>(ctx, system, uuid, module->worker, true);
}

void Module::Interface::ListQualifiedUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");

    // All users should be qualified. We don't actually have parental control or anything to do with
    // nintendo online currently. We're just going to assume the user running the game has access to
    // the game regardless of parental control settings.
    ctx.WriteBuffer(profile_manager->GetAllUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ListOpenContextStoredUsers(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");

    ctx.WriteBuffer(profile_manager->GetStoredOpenedUsers());
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::StoreSaveDataThumbnailApplication(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_WARNING(Service_ACC, "(STUBBED) called");

    // TODO(ogniK): Check if application ID is zero on acc initialize. As we don't have a reliable
    // way of confirming things like the TID, we're going to assume a non zero value for the time
    // being.
    constexpr u64 tid{1};
    StoreSaveDataThumbnail(ctx, uuid, tid);
}

void Module::Interface::AaEnsureCacheAsync(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uid = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<OpenPakAsyncContext>(
        ctx, system, module->worker,
        [uid_string = OpenPakAccount::UidString(uid)](std::stop_token, std::vector<u8>&) {
            if (OpenPakAccount::Enabled()) {
                OpenPakAccount::EnsureAndAdopt(uid_string);
            }
            return ResultSuccess;
        });
}

void Module::Interface::AaLoadCache(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uid = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called");
    // acc:aa's cache is the BAAS access token (the Bearer), not the id_token a title gets.
    const std::string token =
        OpenPakAccount::Enabled() ? OpenPak::Session::AccessToken(OpenPakAccount::UidString(uid))
                                  : std::string{};
    if (token.empty() || !ctx.CanWriteBuffer() || token.size() > ctx.GetWriteBufferSize()) {
        LoadIdTokenCacheFor(ctx, uid);
        return;
    }
    ctx.WriteBuffer(token.data(), token.size());
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(token.size()));
}

void Module::Interface::AaGetDeviceAccountId(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uid = rp.PopRaw<Common::UUID>();
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(NetworkServiceAccountIdOf(uid));
}

void Module::Interface::GetNotifier(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INotifier>(ctx, system, Common::UUID{});
}

void Module::Interface::GetBaasAccountManagerForSystemService(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();

    LOG_DEBUG(Service_ACC, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IManagerForSystemService>(ctx, system, uuid, module->worker, false);
}

void Module::Interface::StoreSaveDataThumbnailSystem(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    const auto tid = rp.Pop<u64_le>();

    LOG_WARNING(Service_ACC, "(STUBBED) called, tid={:016X}", tid);
    StoreSaveDataThumbnail(ctx, uuid, tid);
}

void Module::Interface::GetPinCodeLength(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0);
}

void Module::Interface::StoreSaveDataThumbnail(HLERequestContext& ctx, const Common::UUID& uuid,
                                               const u64 tid) {
    IPC::ResponseBuilder rb{ctx, 2};

    if (tid == 0) {
        LOG_ERROR(Service_ACC, "TitleID is not valid!");
        rb.Push(Account::ResultInvalidApplication);
        return;
    }

    if (uuid.IsInvalid()) {
        LOG_ERROR(Service_ACC, "User ID is not valid!");
        rb.Push(Account::ResultInvalidUserId);
        return;
    }
    const auto thumbnail_size = ctx.GetReadBufferSize();
    if (thumbnail_size != THUMBNAIL_SIZE) {
        LOG_ERROR(Service_ACC, "Buffer size is empty! size={:X} expecting {:X}", thumbnail_size,
                  THUMBNAIL_SIZE);
        rb.Push(Account::ResultInvalidArrayLength);
        return;
    }

    // TODO(ogniK): Construct save data thumbnail
    rb.Push(ResultSuccess);
}

void Module::Interface::TrySelectUserWithoutInteractionDeprecated(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    // A u8 is passed into this function which we can safely ignore. It's to determine if we have
    // access to use the network or not by the looks of it
    IPC::ResponseBuilder rb{ctx, 6};
    if (profile_manager->GetUserCount() != 1) {
        rb.Push(ResultSuccess);
        rb.PushRaw(Common::InvalidUUID);
        return;
    }

    const auto user_list = profile_manager->GetAllUsers();
    if (std::ranges::all_of(user_list, [](const auto& user) { return user.IsInvalid(); })) {
        rb.Push(ResultUnknown); // TODO(ogniK): Find the correct error code
        rb.PushRaw(Common::InvalidUUID);
        return;
    }

    // Select the first user we have
    rb.Push(ResultSuccess);
    rb.PushRaw(profile_manager->GetUser(0)->uuid);
}

void Module::Interface::TrySelectUserWithoutInteraction(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    // A u8 is passed into this function which we can safely ignore. It's to determine if we have
    // access to use the network or not by the looks of it
    IPC::ResponseBuilder rb{ctx, 6};
    if (profile_manager->GetUserCount() != 1) {
        rb.Push(ResultSuccess);
        rb.PushRaw(Common::InvalidUUID);
        return;
    }

    const auto user_list = profile_manager->GetAllUsers();
    if (std::ranges::all_of(user_list, [](const auto& user) { return user.IsInvalid(); })) {
        rb.Push(ResultUnknown); // TODO(ogniK): Find the correct error code
        rb.PushRaw(Common::InvalidUUID);
        return;
    }

    // Select the first user we have
    rb.Push(ResultSuccess);
    rb.PushRaw(profile_manager->GetUser(0)->uuid);
}

/// acc:su 211/213 and 212/214 (22.5.0 serves 213/214 with the very code of 211/212). The
/// module needs 0x22000 bytes of the caller's transfer memory as the procedure's work area
/// (0x085a15c0); the procedure here lives in host memory, so the size is only checked.
void Module::Interface::CreateProcedureToRegisterUserWithNintendoAccount(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto size = rp.Pop<u32>();
    if (size < 0x22000) {
        PushResult(ctx, ResultInvalidBufferSize);
        return;
    }
    LOG_INFO(Service_ACC, "acc: user registration procedure created");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(ctx, system, module->worker);
}

void Module::Interface::ResumeProcedureToRegisterUserWithNintendoAccount(HLERequestContext& ctx) {
    // (Uuid, u32 size, handle<copy>): the session id comes first.
    IPC::RequestParser rp{ctx};
    const auto id = rp.PopRaw<ProcedureSessionId>();
    const auto size = rp.Pop<u32>();
    if (id.IsZero()) {
        PushResult(ctx, ResultSfInvalidArgument);
        return;
    }
    if (size < 0x22000) {
        PushResult(ctx, ResultInvalidBufferSize);
        return;
    }
    auto procedure = std::make_shared<IOAuthProcedureForUserRegistration>(system, module->worker);
    if (const Result result = procedure->Resume(id); result.IsError()) {
        PushResult(ctx, result);
        return;
    }
    LOG_INFO(Service_ACC, "acc: user registration procedure resumed");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IOAuthProcedureForUserRegistration>(ctx, std::move(procedure));
}

/// acc:su 230. 23.0.1 (research/acc-fw23 notes §3, 0x0859B8E0): the module ensures a dauth device
/// token for the BAAS client 8f849b5d34778d8e and nothing more (no BAAS login, no Nintendo Account
/// call), so no device account is made here.
void Module::Interface::AuthenticateServiceAsync(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<OpenPakAsyncContext>(
        ctx, system, module->worker, [](std::stop_token, std::vector<u8>&) {
            if (!OpenPakAccount::Enabled()) {
                return ResultSuccess;
            }
            if (OpenPak::Session::DauthToken("8f849b5d34778d8e", {}).empty()) {
                // The module maps dauth's failure to its own 2124-45xx results; which one is not
                // traced.
                LOG_UNKNOWN_CONTRACT(Service_ACC,
                                     "acc:su 230 dauth failure answered as InternetRequestDenied");
                return ResultInternetRequestDenied;
            }
            return ResultSuccess;
        });
}

void Module::Interface::GetProfileDigest(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    if (ctx.CanWriteBuffer()) {
        const std::array<u8, 0x20> digest{};
        ctx.WriteBuffer(digest);
    }
    PushResult(ctx, ResultSuccess);
}

void Module::Interface::DebugActivateOpenContextRetention(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISessionObject>(ctx, system, Common::UUID{});
}

/// acc:u0 102 AuthenticateApplicationAsync and 103 CheckNetworkServiceAvailabilityAsync: the open
/// user's OpenPak sign-in, which is what makes the network service available.
void Module::Interface::AuthenticateApplicationAsync(HLERequestContext& ctx) {
    LOG_DEBUG(Service_ACC, "called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<OpenPakAsyncContext>(
        ctx, system, module->worker, [](std::stop_token, std::vector<u8>&) {
            if (OpenPakAccount::Enabled()) {
                OpenPak::Session::Ensure(OpenPak::Platform::ProfileId());
            }
            return ResultSuccess;
        });
}

void Module::Interface::ClearSaveDataThumbnail(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    PushResult(ctx, ResultSuccess);
}

// 112: (Uid, ApplicationId, out buffer) -> u32 size. 23.0.1 (account 0x19ac70) checks the id,
// the user and the buffer, reads the save's 0x40-byte thumbnail header through fs, answers 395
// when it is not the one 110 writes, needs 0x24000 bytes of buffer (31) and reads that much.
// Harbor's 110 keeps no thumbnail, so every header reads as not written.
void Module::Interface::LoadSaveDataThumbnail(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto uuid = rp.PopRaw<Common::UUID>();
    const auto application_id = rp.Pop<u64>();
    Result result = Account::ResultSaveDataThumbnailNotFound;
    if (application_id == 0) {
        result = Account::ResultInvalidApplication;
    } else if (uuid.IsInvalid()) {
        result = Account::ResultInvalidUserId;
    } else if (ctx.GetWriteBufferSize() == 0) {
        result = Account::ResultNullptr;
    }
    LOG_DEBUG(Service_ACC, "called, application_id={:016X}, result={:#x}", application_id,
              result.raw);
    PushResult(ctx, result);
}

void Module::Interface::CreateGuestLoginRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IGuestLoginRequest>(ctx, system, Common::UUID{});
}

void Module::Interface::LoadOpenContext(HLERequestContext& ctx) {
    LOG_WARNING(Service_ACC, "(STUBBED) called");
    PushResult(ctx, ResultSuccess);
}

Module::Module(Core::System& system)
    : worker{std::make_shared<OpenPak::AsyncWorker>(system, "acc:OpenPak")} {}

Module::~Module() = default;

Module::Interface::Interface(std::shared_ptr<Module> module_,
                             std::shared_ptr<ProfileManager> profile_manager_,
                             Core::System& system_, const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)},
      profile_manager{std::move(profile_manager_)} {}

Module::Interface::~Interface() = default;

class ACC_AA final : public Module::Interface {
public:
    explicit ACC_AA(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:aa") {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ACC_AA::AaEnsureCacheAsync, "EnsureCacheAsync"},
            {1, &ACC_AA::AaLoadCache, "LoadCache"},
            {2, &ACC_AA::AaGetDeviceAccountId, "GetDeviceAccountId"},
            {50, nullptr, "RegisterNotificationTokenAsync"},   // 1.0.0 - 6.2.0
            {51, nullptr, "UnregisterNotificationTokenAsync"}, // 1.0.0 - 6.2.0
        };
        // clang-format on
        RegisterHandlers(functions);
    }
    ~ACC_AA() override = default;
};

class ACC_SU final : public Module::Interface {
public:
    explicit ACC_SU(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:su") {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ACC_SU::GetUserCount, "GetUserCount"},
            {1, &ACC_SU::GetUserExistence, "GetUserExistence"},
            {2, &ACC_SU::ListAllUsers, "ListAllUsers"},
            {3, &ACC_SU::ListOpenUsers, "ListOpenUsers"},
            {4, &ACC_SU::GetLastOpenedUser, "GetLastOpenedUser"},
            {5, &ACC_SU::GetProfile, "GetProfile"},
            {6, nullptr, "GetProfileDigest"},
            {50, &ACC_SU::IsUserRegistrationRequestPermitted, "IsUserRegistrationRequestPermitted"},
            {51, &ACC_SU::TrySelectUserWithoutInteractionDeprecated, "TrySelectUserWithoutInteractionDeprecated"},
            {52, &ACC_SU::TrySelectUserWithoutInteraction, "TrySelectUserWithoutInteraction"}, // 19.0.0+
            {60, &ACC_SU::ListOpenContextStoredUsers, "ListOpenContextStoredUsers"},
            {99, nullptr, "DebugActivateOpenContextRetention"},
            {100, nullptr, "GetUserRegistrationNotifier"},
            {101, nullptr, "GetUserStateChangeNotifier"},
            {102, &ACC_SU::GetBaasAccountManagerForSystemService, "GetBaasAccountManagerForSystemService"},
            {103, nullptr, "GetBaasUserAvailabilityChangeNotifier"},
            {104, nullptr, "GetProfileUpdateNotifier"},
            {105, nullptr, "CheckNetworkServiceAvailabilityAsync"},
            {106, nullptr, "GetProfileSyncNotifier"},
            {110, &ACC_SU::StoreSaveDataThumbnailSystem, "StoreSaveDataThumbnail"},
            {111, nullptr, "ClearSaveDataThumbnail"},
            {112, &ACC_SU::LoadSaveDataThumbnail, "LoadSaveDataThumbnail"},
            {113, nullptr, "GetSaveDataThumbnailExistence"},
            {120, nullptr, "ListOpenUsersInApplication"},
            {130, nullptr, "ActivateOpenContextRetention"},
            {140, &ACC_SU::ListQualifiedUsers, "ListQualifiedUsers"},
            {150, nullptr, "AuthenticateApplicationAsync"},
            {151, nullptr, "EnsureSignedDeviceIdentifierCacheForNintendoAccountAsync"},
            {152, nullptr, "LoadSignedDeviceIdentifierCacheForNintendoAccount"},
            {190, nullptr, "GetUserLastOpenedApplication"},
            {191, nullptr, "ActivateOpenContextHolder"},
            {200, &ACC_SU::BeginUserRegistration, "BeginUserRegistration"},
            {201, &ACC_SU::CompleteUserRegistration, "CompleteUserRegistration"},
            {202, nullptr, "CancelUserRegistration"},
            {203, &ACC_SU::DeleteUser, "DeleteUser"},
            {204, &ACC_SU::SetUserPosition, "SetUserPosition"},
            {205, &ACC_SU::GetProfileEditor, "GetProfileEditor"},
            {206, &ACC_SU::CompleteUserRegistration, "CompleteUserRegistrationForcibly"},
            {210, nullptr, "CreateFloatingRegistrationRequest"},
            {211, &ACC_SU::CreateProcedureToRegisterUserWithNintendoAccount, "CreateProcedureToRegisterUserWithNintendoAccount"}, // 8.0.0+
            {212, &ACC_SU::ResumeProcedureToRegisterUserWithNintendoAccount, "ResumeProcedureToRegisterUserWithNintendoAccount"}, // 8.0.0+
            {213, &ACC_SU::CreateProcedureToRegisterUserWithNintendoAccount, "CreateProcedureToCreateUserWithNintendoAccount"}, // 17.0.0+
            {214, &ACC_SU::ResumeProcedureToRegisterUserWithNintendoAccount, "ResumeProcedureToCreateUserWithNintendoAccount"}, // 17.0.0+
            {215, nullptr, "ResumeProcedureToCreateUserWithNintendoAccountAfterApplyResponse"}, // 17.0.0+
            {230, &ACC_SU::AuthenticateServiceAsync, "AuthenticateServiceAsync"},
            {250, &ACC_SU::GetBaasAccountAdministrator, "GetBaasAccountAdministrator"},
            {290, nullptr, "ProxyProcedureForGuestLoginWithNintendoAccount"},
            {291, nullptr, "ProxyProcedureForFloatingRegistrationWithNintendoAccount"},
            {299, nullptr, "SuspendBackgroundDaemon"},
            {400, nullptr, "SetPinCode"}, // 18.0.0+
            {401, &ACC_SU::GetPinCodeLength, "GetPinCodeLength"}, // 18.0.0+
            {402, nullptr, "GetPinCode"}, // 18.0.0+
            {403, nullptr, "GetPinCodeParity"},
            {404, nullptr, "VerifyPinCode"},
            {405, nullptr, "IsPinCodeVerificationForbidden"},
            {410, nullptr, "GetPinCodeErrorCount"}, // 18.0.0+
            {411, nullptr, "ResetPinCodeErrorCount"}, // 18.0.0+
            {412, nullptr, "IncrementPinCodeErrorCount"}, // 18.0.0+
            {900, nullptr, "SetUserUnqualifiedForDebug"},
            {901, nullptr, "UnsetUserUnqualifiedForDebug"},
            {902, nullptr, "ListUsersUnqualifiedForDebug"},
            {910, nullptr, "RefreshFirmwareSettingsForDebug"},
            {997, nullptr, "DebugInvalidateTokenCacheForUser"},
            {998, nullptr, "DebugSetUserStateClose"},
            {999, nullptr, "DebugSetUserStateOpen"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
    ~ACC_SU() override = default;
};

class ACC_U0 final : public Module::Interface {
public:
    ACC_U0(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:u0") {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ACC_U0::GetUserCount, "GetUserCount"},
            {1, &ACC_U0::GetUserExistence, "GetUserExistence"},
            {2, &ACC_U0::ListAllUsers, "ListAllUsers"},
            {3, &ACC_U0::ListOpenUsers, "ListOpenUsers"},
            {4, &ACC_U0::GetLastOpenedUser, "GetLastOpenedUser"},
            {5, &ACC_U0::GetProfile, "GetProfile"},
            {6, &ACC_U0::GetProfileDigest, "GetProfileDigest"}, // 3.0.0+
            {50, &ACC_U0::IsUserRegistrationRequestPermitted, "IsUserRegistrationRequestPermitted"},
            {51, &ACC_U0::TrySelectUserWithoutInteractionDeprecated, "TrySelectUserWithoutInteractionDeprecated"},
            {52, &ACC_U0::TrySelectUserWithoutInteraction, "TrySelectUserWithoutInteraction"},
            {60, &ACC_U0::ListOpenContextStoredUsers, "ListOpenContextStoredUsers"}, // 5.0.0 - 5.1.0
            {99, &ACC_U0::DebugActivateOpenContextRetention, "DebugActivateOpenContextRetention"}, // 6.0.0+
            {100, &ACC_U0::InitializeApplicationInfo, "InitializeApplicationInfo"},
            {101, &ACC_U0::GetBaasAccountManagerForApplication, "GetBaasAccountManagerForApplication"},
            {102, &ACC_U0::AuthenticateApplicationAsync, "AuthenticateApplicationAsync"},
            {103, &ACC_U0::AuthenticateApplicationAsync, "CheckNetworkServiceAvailabilityAsync"}, // 4.0.0+
            {110, &ACC_U0::StoreSaveDataThumbnailApplication, "StoreSaveDataThumbnail"},
            {111, &ACC_U0::ClearSaveDataThumbnail, "ClearSaveDataThumbnail"},
            {120, &ACC_U0::CreateGuestLoginRequest, "CreateGuestLoginRequest"},
            {130, &ACC_U0::LoadOpenContext, "LoadOpenContext"}, // 5.0.0+
            {131, &ACC_U0::ListOpenContextStoredUsers, "ListOpenContextStoredUsers"}, // 6.0.0+
            {140, &ACC_U0::InitializeApplicationInfoRestricted, "InitializeApplicationInfoRestricted"}, // 6.0.0+
            {141, &ACC_U0::ListQualifiedUsers, "ListQualifiedUsers"}, // 6.0.0+
            {150, &ACC_U0::IsUserAccountSwitchLocked, "IsUserAccountSwitchLocked"}, // 6.0.0+
            {160, &ACC_U0::InitializeApplicationInfoV2, "InitializeApplicationInfoV2"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
    ~ACC_U0() override = default;
};

class ACC_U1 final : public Module::Interface {
public:
    ACC_U1(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:u1") {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ACC_U1::GetUserCount, "GetUserCount"},
            {1, &ACC_U1::GetUserExistence, "GetUserExistence"},
            {2, &ACC_U1::ListAllUsers, "ListAllUsers"},
            {3, &ACC_U1::ListOpenUsers, "ListOpenUsers"},
            {4, &ACC_U1::GetLastOpenedUser, "GetLastOpenedUser"},
            {5, &ACC_U1::GetProfile, "GetProfile"},
            {6, nullptr, "GetProfileDigest"},
            {50, &ACC_U1::IsUserRegistrationRequestPermitted, "IsUserRegistrationRequestPermitted"},
            {51, &ACC_U1::TrySelectUserWithoutInteractionDeprecated, "TrySelectUserWithoutInteractionDeprecated"},
            {52, &ACC_U1::TrySelectUserWithoutInteraction, "TrySelectUserWithoutInteraction"},
            {60, &ACC_U1::ListOpenContextStoredUsers, "ListOpenContextStoredUsers"},
            {99, nullptr, "DebugActivateOpenContextRetention"},
            {100, nullptr, "GetUserRegistrationNotifier"},
            {101, nullptr, "GetUserStateChangeNotifier"},
            {102, &ACC_U1::GetBaasAccountManagerForSystemService, "GetBaasAccountManagerForSystemService"},
            {103, nullptr, "GetBaasUserAvailabilityChangeNotifier"},
            {104, nullptr, "GetProfileUpdateNotifier"},
            {105, nullptr, "CheckNetworkServiceAvailabilityAsync"},
            {106, nullptr, "GetProfileSyncNotifier"},
            {110, &ACC_U1::StoreSaveDataThumbnailApplication, "StoreSaveDataThumbnail"},
            {111, nullptr, "ClearSaveDataThumbnail"},
            {112, &ACC_U1::LoadSaveDataThumbnail, "LoadSaveDataThumbnail"},
            {113, nullptr, "GetSaveDataThumbnailExistence"},
            {120, nullptr, "ListOpenUsersInApplication"},
            {130, nullptr, "ActivateOpenContextRetention"},
            {140, &ACC_U1::ListQualifiedUsers, "ListQualifiedUsers"},
            {150, nullptr, "AuthenticateApplicationAsync"},
            {151, nullptr, "EnsureSignedDeviceIdentifierCacheForNintendoAccountAsync"},
            {152, nullptr, "LoadSignedDeviceIdentifierCacheForNintendoAccount"},
            {190, nullptr, "GetUserLastOpenedApplication"},
            {191, nullptr, "ActivateOpenContextHolder"},
            {401, &ACC_U1::GetPinCodeLength, "GetPinCodeLength"}, // 18.0.0+
            {402, nullptr, "GetPinCode"}, // 18.0.0+
            {997, nullptr, "DebugInvalidateTokenCacheForUser"},
            {998, nullptr, "DebugSetUserStateClose"},
            {999, nullptr, "DebugSetUserStateOpen"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
    ~ACC_U1() override = default;
};

// [OpenPak] dauth:0, the device authentication tokens the firmware's own programs (olsc, nim,
// npns) fetch for their clients. The token comes from the console server through the session's
// dauth client; one cache entry per client id, loaded by the Ensure call's async context.
class DAUTH_0 final : public ServiceFramework<DAUTH_0> {
public:
    explicit DAUTH_0(Core::System& system_)
        : ServiceFramework{system_, "dauth:0"},
          worker{std::make_shared<OpenPak::AsyncWorker>(system_, "dauth:OpenPak")} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &DAUTH_0::EnsureAuthenticationTokenCacheAsync, "EnsureAuthenticationTokenCacheAsync"},
            {1, &DAUTH_0::LoadAuthenticationTokenCache, "LoadAuthenticationTokenCache"},
            {2, &DAUTH_0::InvalidateAuthenticationTokenCache, "InvalidateAuthenticationTokenCache"},
            {3, &DAUTH_0::IsDeviceAuthenticationTokenCacheAvailable, "IsDeviceAuthenticationTokenCacheAvailable"},
            {10, &DAUTH_0::EnsureEdgeTokenCacheAsync, "EnsureEdgeTokenCacheAsync"},
            {11, &DAUTH_0::LoadEdgeTokenCache, "LoadEdgeTokenCache"},
            {12, &DAUTH_0::InvalidateEdgeTokenCache, "InvalidateEdgeTokenCache"},
            {13, &DAUTH_0::IsEdgeTokenCacheAvailable, "IsEdgeTokenCacheAvailable"},
            {20, nullptr, "EnsureApplicationAuthenticationCacheAsync"},
            {21, nullptr, "LoadApplicationAuthenticationTokenCache"},
            {22, nullptr, "LoadApplicationNetworkServiceClientConfigCache"},
            {23, nullptr, "IsApplicationAuthenticationCacheAvailable"},
            {24, nullptr, "InvalidateApplicationAuthenticationCache"},
            {30, nullptr, "EnsureGameCardAuthenticationCacheAsync"},
            {31, nullptr, "LoadGameCardAuthenticationTokenCache"},
            {32, nullptr, "IsGameCardAuthenticationCacheAvailable"},
            {33, nullptr, "InvalidateGameCardAuthenticationCache"},
            {1000, nullptr, "GetInactiveElicenseUsedEvent"},
            {9000, nullptr, "ImportVirtualClientCertificate"},
            {9010, nullptr, "DeleteVirtualClientCertificate"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    // Ensure carries (pid placeholder, client id, flag); Load, Invalidate and IsAvailable carry
    // (pid placeholder, a zero u64, client id). Read from the bcat program's client stubs.
    // Ensure carries (pid placeholder, client id, force flag). Load, Invalidate and IsAvailable
    // carry no client id, only an offset and the buffer size: they mean the token the calling
    // program ensured last, so the cache is keyed by process.
    static u64 PopClientId(HLERequestContext& ctx, bool ensure) {
        if (!ensure) {
            const auto* process = ctx.GetThread().GetOwnerProcess();
            return process != nullptr ? process->GetProcessId() : 0;
        }
        IPC::RequestParser rp{ctx};
        rp.Pop<u64>();
        return rp.Pop<u64>();
    }

    static u64 Caller(HLERequestContext& ctx) {
        const auto* process = ctx.GetThread().GetOwnerProcess();
        return process != nullptr ? process->GetProcessId() : 0;
    }

    // The token caches and their mutex. The Ensure work runs on a plain thread that can outlive
    // this service object (session closed, emulation stopped) while the network call is stuck in
    // a timeout, so the work holds the state by shared_ptr, never through `this`.
    struct State {
        std::mutex mutex;
        std::map<u64, std::string> device_tokens;
        std::map<u64, std::string> edge_tokens;
    };

    void Ensure(HLERequestContext& ctx, std::map<u64, std::string> State::*cache_member,
                std::string vendor) {
        const u64 client_id = PopClientId(ctx, true);
        const u64 caller = Caller(ctx);
        LOG_INFO(Service_ACC, "dauth: token for client {:016x}{}{} requested", client_id,
                 vendor.empty() ? "" : " vendor ", vendor);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<OpenPakAsyncContext>(
            ctx, system, worker,
            [st = state, cache_member, client_id, caller,
             vendor](std::stop_token, std::vector<u8>&) {
                const auto token =
                    OpenPak::Session::DauthToken(fmt::format("{:016x}", client_id), vendor);
                LOG_INFO(Service_ACC, "dauth: token for client {:016x}{}{} -> {}", client_id,
                         vendor.empty() ? "" : " vendor ", vendor, token.empty() ? "none" : "ok");
                std::scoped_lock lk{st->mutex};
                auto& cache = (*st).*cache_member;
                if (token.empty()) {
                    cache.erase(caller);
                    return ResultInternetRequestDenied;
                }
                cache[caller] = token;
                return ResultSuccess;
            },
            false, true);
    }

    void Load(HLERequestContext& ctx, std::map<u64, std::string> State::*cache_member) {
        const u64 client_id = PopClientId(ctx, false);
        std::scoped_lock lk{state->mutex};
        auto& cache = (*state).*cache_member;
        const auto it = cache.find(client_id);
        if (it == cache.end()) {
            LOG_WARNING(Service_ACC, "dauth: no cached token for process {}", client_id);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultInternetRequestDenied);
            return;
        }
        std::vector<u8> out(it->second.begin(), it->second.end());
        out.push_back(0);
        ctx.WriteBuffer(out);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Invalidate(HLERequestContext& ctx, std::map<u64, std::string> State::*cache_member) {
        const u64 client_id = PopClientId(ctx, false);
        std::scoped_lock lk{state->mutex};
        auto& cache = (*state).*cache_member;
        cache.erase(client_id);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsAvailable(HLERequestContext& ctx, std::map<u64, std::string> State::*cache_member) {
        const u64 client_id = PopClientId(ctx, false);
        std::scoped_lock lk{state->mutex};
        auto& cache = (*state).*cache_member;
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<bool>(cache.contains(client_id));
    }

    void EnsureAuthenticationTokenCacheAsync(HLERequestContext& ctx) { Ensure(ctx, &State::device_tokens, ""); }
    void LoadAuthenticationTokenCache(HLERequestContext& ctx) { Load(ctx, &State::device_tokens); }
    void InvalidateAuthenticationTokenCache(HLERequestContext& ctx) { Invalidate(ctx, &State::device_tokens); }
    void IsDeviceAuthenticationTokenCacheAvailable(HLERequestContext& ctx) { IsAvailable(ctx, &State::device_tokens); }

    // No vendor is passed: the dauth program knows it per client, and every edge client here is
    // the CDN's ("akamai").
    void EnsureEdgeTokenCacheAsync(HLERequestContext& ctx) {
        Ensure(ctx, &State::edge_tokens, "akamai");
    }
    void LoadEdgeTokenCache(HLERequestContext& ctx) { Load(ctx, &State::edge_tokens); }
    void InvalidateEdgeTokenCache(HLERequestContext& ctx) { Invalidate(ctx, &State::edge_tokens); }
    void IsEdgeTokenCacheAvailable(HLERequestContext& ctx) { IsAvailable(ctx, &State::edge_tokens); }

    std::shared_ptr<OpenPak::AsyncWorker> worker;
    std::shared_ptr<State> state = std::make_shared<State>();
};

class ACC_E final : public Module::Interface {
public:
    explicit ACC_E(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:e") {
        // clang-format off
        static const FunctionInfo functions[] = {
            { 0, nullptr, "GetUserCount"},
            { 1, nullptr, "GetUserExistence"},
            { 2, nullptr, "ListAllUsers"},
            { 3, nullptr, "ListOpenUsers"},
            { 4, nullptr, "GetLastOpenedUser"},
            { 5, nullptr, "GetProfile"},
            { 6, nullptr, "GetProfileDigest"},
            { 50, nullptr, "IsUserRegistrationRequestPermitted"},
            { 51, nullptr, "TrySelectUserWithoutInteractionDeprecated"},
            { 52, nullptr, "TrySelectUserWithoutInteraction"},
            { 99, nullptr, "DebugActivateOpenContextRetention"},
            { 100, nullptr, "GetUserRegistrationNotifier"},
            { 101, nullptr, "GetUserStateChangeNotifier"},
            { 102, nullptr, "GetBaasAccountManagerForSystemService"},
            { 103, nullptr, "GetBaasUserAvailabilityChangeNotifier"},
            { 104, nullptr, "GetProfileUpdateNotifier"},
            { 105, nullptr, "CheckNetworkServiceAvailabilityAsync"},
            { 106, nullptr, "GetProfileSyncNotifier"},
            { 110, nullptr, "StoreSaveDataThumbnail"},
            { 111, nullptr, "ClearSaveDataThumbnail"},
            { 112, &ACC_E::LoadSaveDataThumbnail, "LoadSaveDataThumbnail"},
            { 113, nullptr, "GetSaveDataThumbnailExistence"},
            { 120, nullptr, "ListOpenUsersInApplication"},
            { 130, nullptr, "ActivateOpenContextRetention"},
            { 140, nullptr, "ListQualifiedUsers"},
            { 151, nullptr, "EnsureSignedDeviceIdentifierCacheForNintendoAccountAsync"},
            { 152, nullptr, "LoadSignedDeviceIdentifierCacheForNintendoAccount"},
            { 170, nullptr, "GetNasOp2MembershipStateChangeNotifier"},
            { 191, nullptr, "UpdateNotificationReceiverInfo"},
            { 200, nullptr, "BeginUserRegistration"},
            { 201, nullptr, "CompleteUserRegistration"},
            { 202, nullptr, "CancelUserRegistration"},
            { 203, nullptr, "DeleteUser"},
            { 204, nullptr, "SetUserPosition"},
            { 205, nullptr, "GetProfileEditor"},
            { 206, nullptr, "CompleteUserRegistrationForcibly"},
            { 210, nullptr, "CreateFloatingRegistrationRequest"},
            { 211, nullptr, "CreateProcedureToRegisterUserWithNintendoAccount"},
            { 212, nullptr, "ResumeProcedureToRegisterUserWithNintendoAccount"},
            { 213, nullptr, "CreateProcedureToCreateUserWithNintendoAccount"},
            { 214, nullptr, "ResumeProcedureToCreateUserWithNintendoAccount"},
            { 215, nullptr, "ResumeProcedureToCreateUserWithNintendoAccountAfterApplyResponse"},
            { 230, nullptr, "AuthenticateServiceAsync"},
            { 250, nullptr, "GetBaasAccountAdministrator"},
            { 251, nullptr, "SynchronizeNetworkServiceAccountsSnapshotAsync"},
            { 290, nullptr, "ProxyProcedureForGuestLoginWithNintendoAccount"},
            { 291, nullptr, "ProxyProcedureForFloatingRegistrationWithNintendoAccount"},
            { 292, nullptr, "ProxyProcedureForDeviceMigrationAuthenticatingOperatingUser"},
            { 293, nullptr, "ProxyProcedureForDeviceMigrationDownload"},
            { 299, nullptr, "SuspendBackgroundDaemon"},
            { 350, nullptr, "CreateDeviceMigrationUserExportRequest"},
            { 351, nullptr, "UploadNasCredential"},
            { 352, nullptr, "CreateDeviceMigrationUserImportRequest"},
            { 353, nullptr, "DeleteUserMigrationSaveData"},
            { 400, nullptr, "SetPinCode"},
            { 401, nullptr, "GetPinCodeLength"},
            { 402, nullptr, "GetPinCode"},
            { 403, nullptr, "GetPinCodeParity"},
            { 404, nullptr, "VerifyPinCode"},
            { 405, nullptr, "IsPinCodeVerificationForbidden"},
            { 410, nullptr, "GetPinCodeErrorCount"},
            { 411, nullptr, "ResetPinCodeErrorCount"},
            { 412, nullptr, "IncrementPinCodeErrorCount"},
            { 413, nullptr, "SetPinCodeErrorCount"},
            { 420, nullptr, "SetStartPenaltyTime"},
            { 421, nullptr, "GetStartPenaltyTime"},
            { 900, nullptr, "SetUserUnqualifiedForDebug"},
            { 901, nullptr, "UnsetUserUnqualifiedForDebug"},
            { 902, nullptr, "ListUsersUnqualifiedForDebug"},
            { 910, nullptr, "RefreshFirmwareSettingsForDebug"},
            { 997, nullptr, "DebugInvalidateTokenCacheForUser"},
            { 998, nullptr, "DebugSetUserStateClose"},
            { 999, nullptr, "DebugSetUserStateOpen"},
            { 1000, nullptr, "CreateIAccountEntityServiceForApplication"},
            { 1100, nullptr, "CreateIUserStateManager"},
            { 10050, nullptr, "IsUserRegistrationRequestPermittedForAccountPolicy"},
            { 10105, nullptr, "CheckNetworkServiceAvailabilityAsyncForAccountPolicy"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }
};

class ACC_E_U1 final : public Module::Interface {
public:
    explicit ACC_E_U1(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:e:u1") {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ACC_E_U1::GetUserCount, "GetUserCount"},
            {1, &ACC_E_U1::GetUserExistence, "GetUserExistence"},
            {2, &ACC_E_U1::ListAllUsers, "ListAllUsers"},
            {3, &ACC_E_U1::ListOpenUsers, "ListOpenUsers"},
            {4, &ACC_E_U1::GetLastOpenedUser, "GetLastOpenedUser"},
            {5, &ACC_E_U1::GetProfile, "GetProfile"},
            {6, nullptr, "GetProfileDigest"},
            {50, &ACC_E_U1::IsUserRegistrationRequestPermitted, "IsUserRegistrationRequestPermitted"},
            {51, &ACC_E_U1::TrySelectUserWithoutInteractionDeprecated, "TrySelectUserWithoutInteractionDeprecated"},
            {99, nullptr, "DebugActivateOpenContextRetention"},
            {100, &ACC_E_U1::GetNotifier, "GetUserRegistrationNotifier"},
            {101, &ACC_E_U1::GetNotifier, "GetUserStateChangeNotifier"},
            {102, &ACC_E_U1::GetBaasAccountManagerForSystemService, "GetBaasAccountManagerForSystemService"},
            {103, &ACC_E_U1::GetNotifier, "GetBaasUserAvailabilityChangeNotifier"},
            {104, &ACC_E_U1::GetNotifier, "GetProfileUpdateNotifier"},
            {105, nullptr, "CheckNetworkServiceAvailabilityAsync"},
            {106, &ACC_E_U1::GetNotifier, "GetProfileSyncNotifier"},
            {110, &ACC_E_U1::StoreSaveDataThumbnailApplication, "StoreSaveDataThumbnail"},
            {111, nullptr, "ClearSaveDataThumbnail"},
            {112, &ACC_E_U1::LoadSaveDataThumbnail, "LoadSaveDataThumbnail"},
            {113, nullptr, "GetSaveDataThumbnailExistence"},
            {120, nullptr, "ListOpenUsersInApplication"},
            {130, nullptr, "ActivateOpenContextRetention"},
            {140, &ACC_E_U1::ListQualifiedUsers, "ListQualifiedUsers"},
            {151, nullptr, "EnsureSignedDeviceIdentifierCacheForNintendoAccountAsync"},
            {152, nullptr, "LoadSignedDeviceIdentifierCacheForNintendoAccount"},
            {170, &ACC_E_U1::GetNotifier, "GetNasOp2MembershipStateChangeNotifier"},
            {191, nullptr, "UpdateNotificationReceiverInfo"},
            {997, nullptr, "DebugInvalidateTokenCacheForUser"},
            {998, nullptr, "DebugSetUserStateClose"},
            {999, nullptr, "DebugSetUserStateOpen"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }
};

// [OpenPak] acc:e:u2 answers like acc:e:u1 (the firmware bcat program opens it).
class ACC_E_U2 final : public Module::Interface {
public:
    explicit ACC_E_U2(std::shared_ptr<Module> module_, std::shared_ptr<ProfileManager> profile_manager_, Core::System& system_)
        : Interface(std::move(module_), std::move(profile_manager_), system_, "acc:e:u2") {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &ACC_E_U2::GetUserCount, "GetUserCount"},
            {1, &ACC_E_U2::GetUserExistence, "GetUserExistence"},
            {2, &ACC_E_U2::ListAllUsers, "ListAllUsers"},
            {3, &ACC_E_U2::ListOpenUsers, "ListOpenUsers"},
            {4, &ACC_E_U2::GetLastOpenedUser, "GetLastOpenedUser"},
            {5, &ACC_E_U2::GetProfile, "GetProfile"},
            {6, nullptr, "GetProfileDigest"},
            {50, &ACC_E_U2::IsUserRegistrationRequestPermitted, "IsUserRegistrationRequestPermitted"},
            {51, &ACC_E_U2::TrySelectUserWithoutInteractionDeprecated, "TrySelectUserWithoutInteractionDeprecated"},
            {99, nullptr, "DebugActivateOpenContextRetention"},
            {100, &ACC_E_U2::GetNotifier, "GetUserRegistrationNotifier"},
            {101, &ACC_E_U2::GetNotifier, "GetUserStateChangeNotifier"},
            {102, &ACC_E_U2::GetBaasAccountManagerForSystemService, "GetBaasAccountManagerForSystemService"},
            {103, &ACC_E_U2::GetNotifier, "GetBaasUserAvailabilityChangeNotifier"},
            {104, &ACC_E_U2::GetNotifier, "GetProfileUpdateNotifier"},
            {105, nullptr, "CheckNetworkServiceAvailabilityAsync"},
            {106, &ACC_E_U2::GetNotifier, "GetProfileSyncNotifier"},
            {110, &ACC_E_U2::StoreSaveDataThumbnailApplication, "StoreSaveDataThumbnail"},
            {111, nullptr, "ClearSaveDataThumbnail"},
            {112, &ACC_E_U2::LoadSaveDataThumbnail, "LoadSaveDataThumbnail"},
            {113, nullptr, "GetSaveDataThumbnailExistence"},
            {120, nullptr, "ListOpenUsersInApplication"},
            {130, nullptr, "ActivateOpenContextRetention"},
            {140, &ACC_E_U2::ListQualifiedUsers, "ListQualifiedUsers"},
            {151, nullptr, "EnsureSignedDeviceIdentifierCacheForNintendoAccountAsync"},
            {152, nullptr, "LoadSignedDeviceIdentifierCacheForNintendoAccount"},
            {170, &ACC_E_U2::GetNotifier, "GetNasOp2MembershipStateChangeNotifier"},
            {191, nullptr, "UpdateNotificationReceiverInfo"},
            {997, nullptr, "DebugInvalidateTokenCacheForUser"},
            {998, nullptr, "DebugSetUserStateClose"},
            {999, nullptr, "DebugSetUserStateOpen"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }
};

void LoopProcess(Core::System& system) {
    auto module = std::make_shared<Module>(system);
    // The system's ProfileManager, shared with the frontend (not owned here).
    auto profile_manager =
        std::shared_ptr<ProfileManager>(std::shared_ptr<void>{}, &system.GetProfileManager());
    auto server_manager = std::make_unique<ServerManager>(system);

    // The OpenPak session speaks for the user acc has open.
    OpenPakAccount::InstallProfileSource(system.GetProfileManager());

    server_manager->RegisterNamedService("acc:aa", std::make_shared<ACC_AA>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:su", std::make_shared<ACC_SU>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:u0", std::make_shared<ACC_U0>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:u1", std::make_shared<ACC_U1>(module, profile_manager, system));

    server_manager->RegisterNamedService("acc:e", std::make_shared<ACC_E>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:e:u1", std::make_shared<ACC_E_U1>(module, profile_manager, system));
    server_manager->RegisterNamedService("acc:e:u2", std::make_shared<ACC_E_U2>(module, profile_manager, system));

    server_manager->RegisterNamedService("dauth:0", std::make_shared<DAUTH_0>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Account
