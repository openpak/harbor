// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/acc/errors.h"
#include "core/hle/service/acc/nintendo_account_oauth.h"
#include "core/hle/service/acc/openpak_account.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/openpak/account.h"
#include "core/openpak/platform.h"
#include "core/openpak/secret_store.h"
#include "core/openpak/session.h"

namespace Service::Account::OpenPakAccount {

namespace {

namespace oauth = NintendoAccountOAuth;

// The module downloads a thumbnail of at most this many bytes (0x085f06a0); IProfile's loader
// caps an image at the same size.
constexpr std::size_t MaxImageSize = 0x20000;
constexpr int ImageDimensions = 256;

struct Pending {
    std::string nickname;
    std::vector<u8> image;
};

struct AdoptionState {
    std::mutex mutex;
    std::map<std::string, Pending> pending;
    std::set<std::string> adopted;
    std::set<std::string> in_flight;
    std::condition_variable finished; ///< A background sign-in left in_flight.
};

AdoptionState& Adoptions() {
    static AdoptionState state;
    return state;
}

void AppendJpeg(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<u8>*>(context);
    const auto* bytes = static_cast<const u8*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// "https://host/path?query" split for Session::Send. Only https: the session pins every request
/// to the OpenPak CA.
bool SplitUrl(std::string_view url, std::string& host, std::string& path) {
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme) {
        return false;
    }
    const std::string_view rest = url.substr(scheme.size());
    const std::size_t slash = rest.find_first_of("/?");
    host = std::string{rest.substr(0, slash)};
    path = slash == std::string_view::npos ? "/" : std::string{rest.substr(slash)};
    if (!path.empty() && path[0] == '?') {
        path.insert(path.begin(), '/');
    }
    return !host.empty() && host.find_first_of("@:") == std::string::npos;
}

std::string UsernameText(const ProfileUsername& username) {
    const auto end = std::find(username.begin(), username.end(), u8{0});
    return std::string(username.begin(), end);
}

} // namespace

/// At most 32 bytes, never cutting a UTF-8 sequence in half.
ProfileUsername ToUsername(std::string_view nickname) {
    std::size_t size = std::min(nickname.size(), profile_username_size);
    while (size > 0 && size < nickname.size() &&
           (static_cast<u8>(nickname[size]) & 0xC0) == 0x80) {
        --size;
    }
    ProfileUsername out{};
    std::memcpy(out.data(), nickname.data(), size);
    return out;
}

std::string UidString(const Common::UUID& uid) {
    return uid.RawString();
}

bool Enabled() {
    return OpenPak::Session::Enabled();
}

void InstallProfileSource(ProfileManager& profile_manager) {
    // The ProfileManager is the System's and lives as long as the process.
    OpenPak::Platform::SetProfileSource([&profile_manager] {
        const Common::UUID open = profile_manager.GetLastOpenedUser();
        return open.IsInvalid() ? std::string{} : UidString(open);
    });
}

void BindRunningApplication(Core::System& system) {
    // A console's BAAS login carries an application_auth_token naming the running game, and a
    // title's own online stack checks that binding before it uses the id_token.
    const u64 program_id = system.GetApplicationProcessProgramID();
    if (program_id == 0) {
        return;
    }
    std::string version;
    const auto [nacp, icon] = FileSys::PatchManager{program_id, system.GetFileSystemController(),
                                                    system.GetContentProvider()}
                                  .GetControlMetadata();
    if (nacp) {
        version = nacp->GetVersionString();
    }
    OpenPak::Session::SetApplication(fmt::format("{:016x}", program_id), std::move(version));
}

std::filesystem::path ProfileImagePath(const Common::UUID& uid) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
           fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uid.FormattedString());
}

namespace {

// users/me's names for a user, as the console keeps users/me in its NasUserBase cache: in the
// secret store (they are account data), with a copy in memory for 136's IPC thread.
std::string NamesKey(const std::string& uid) {
    return fmt::format("nas-user-{}", uid);
}

std::mutex& NamesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, oauth::NasUserNames>& NamesCache() {
    static std::map<std::string, oauth::NasUserNames> cache;
    return cache;
}

void KeepNames(const std::string& uid, const oauth::User& me) {
    oauth::NasUserNames names{me.screen_name, me.email, me.login_id, me.gender,
                              me.language,    me.region, me.timezone};
    const nlohmann::json doc{{"screenName", names.screen_name}, {"email", names.email},
                             {"loginId", names.login_id},       {"gender", names.gender},
                             {"language", names.language},      {"region", names.region},
                             {"timezone", names.timezone}};
    if (!OpenPak::StoreSecret(NamesKey(uid), doc.dump())) {
        LOG_WARNING(Service_ACC, "acc: the Nintendo Account names were not kept");
    }
    std::scoped_lock lock{NamesMutex()};
    NamesCache()[uid] = std::move(names);
}

oauth::NasUserNames LoadNames(const std::string& uid) {
    {
        std::scoped_lock lock{NamesMutex()};
        if (const auto it = NamesCache().find(uid); it != NamesCache().end()) {
            return it->second;
        }
    }
    oauth::NasUserNames names;
    if (const auto kept = OpenPak::LoadSecret(NamesKey(uid)); kept && !kept->empty()) {
        const auto doc = nlohmann::json::parse(*kept, nullptr, false);
        const auto text = [&doc](const char* key) {
            return doc.is_object() && doc.contains(key) && doc.at(key).is_string()
                       ? doc.at(key).get<std::string>()
                       : std::string{};
        };
        names = {text("screenName"), text("email"),  text("loginId"), text("gender"),
                 text("language"),   text("region"), text("timezone")};
    }
    std::scoped_lock lock{NamesMutex()};
    NamesCache()[uid] = names;
    return names;
}

} // namespace

void KeepNamesOfLink(const std::string& uid, const std::string& id_token) {
    const auto reply = OpenPak::Session::Send(
        "GET", std::string{oauth::ApiAccountsHost}, std::string{oauth::UsersMePath},
        {{"Authorization", "Bearer " + id_token}}, {}, {});
    const auto me = reply.status == 200 ? oauth::ParseUsersMe(reply.body) : std::nullopt;
    if (!me) {
        LOG_WARNING(Service_ACC, "acc: users/me after the setup's link failed (status {})",
                    reply.status);
        return;
    }
    KeepNames(uid, *me);
}

Result ApplyCode(const std::stop_token& stop, const std::string& code,
                 const std::string& verifier, const std::string& uid,
                 const std::vector<std::string>& others, NintendoAccountLink& out) {
    out = {};
    if (!OpenPak::Session::Enabled()) {
        LOG_WARNING(Service_ACC, "acc: Nintendo Account sign-in with no OpenPak server configured");
        return ResultInternetRequestDenied;
    }

    // The module's token exchange carries a dauth device token of its own client (0x085f81f0).
    const std::string device_token =
        OpenPak::Session::DauthToken(std::string{oauth::DeviceTokenClientId}, {});
    if (device_token.empty() || stop.stop_requested()) {
        LOG_WARNING(Service_ACC, "acc: no dauth device token for the Nintendo Account exchange");
        return ResultInternetRequestDenied;
    }

    const auto exchanged = OpenPak::Session::Send(
        "POST", std::string{oauth::AccountsHost}, std::string{oauth::TokenPath}, {},
        "application/x-www-form-urlencoded", oauth::TokenRequestBody(code, verifier, device_token));
    const auto tokens = exchanged.status == 200 ? oauth::ParseTokens(exchanged.body) : std::nullopt;
    if (!tokens || stop.stop_requested()) {
        // The status only: these answers carry tokens and account data.
        LOG_WARNING(Service_ACC, "acc: the Nintendo Account code exchange failed (status {})",
                    exchanged.status);
        return ResultInternetRequestDenied;
    }

    const auto me_reply = OpenPak::Session::Send(
        "GET", std::string{oauth::ApiAccountsHost}, std::string{oauth::UsersMePath},
        {{"Authorization", "Bearer " + tokens->access_token}}, {}, {});
    const auto me = me_reply.status == 200 ? oauth::ParseUsersMe(me_reply.body) : std::nullopt;
    if (!me || stop.stop_requested()) {
        LOG_WARNING(Service_ACC, "acc: users/me failed (status {})", me_reply.status);
        return ResultInternetRequestDenied;
    }

    // The module refuses an account another user on this console already holds (0x085c5a40).
    // Known here for the users signed in during this process.
    for (const std::string& other : others) {
        if (other != uid && OpenPak::Session::Linked(other) &&
            OpenPak::Session::NintendoAccountId(other) == me->id) {
            LOG_WARNING(Service_ACC, "acc: that Nintendo Account belongs to another user here");
            return ResultNintendoAccountAlreadyRegistered;
        }
    }

    const u64 before = OpenPak::Session::NetworkServiceAccountId(uid);
    if (const std::string error = OpenPak::Session::Link(uid, tokens->id_token); !error.empty()) {
        // Core's reason is fixed text, never a server answer.
        LOG_WARNING(Service_ACC, "acc: BAAS federation failed: {}", error);
        return ResultNetworkServiceAccountFederationFailed;
    }

    KeepNames(uid, *me);
    out.nintendo_account_id = me->id;
    out.network_service_account_id = OpenPak::Session::NetworkServiceAccountId(uid);
    out.nickname = OpenPak::Session::Nickname(uid);
    if (out.nickname.empty()) {
        out.nickname = me->nickname;
    }
    out.replaced = before != 0 && before != out.network_service_account_id;

    std::string picture = OpenPak::Session::ThumbnailUrl(uid);
    if (picture.empty()) {
        picture = me->icon_uri;
    }
    if (!picture.empty() && !stop.stop_requested()) {
        out.image = FetchProfileImage(picture);
    }

    LOG_INFO(Service_ACC, "acc: Nintendo Account applied (profile {}, picture {})",
             out.nickname.empty() ? "missing" : "present", out.image.empty() ? "none" : "fetched");
    return ResultSuccess;
}

bool IsActiveLinked(const Common::UUID& uid) {
    if (uid.IsInvalid() || !OpenPak::Session::Enabled()) {
        return false;
    }
    const std::string text = UidString(uid);
    // A user the setup's account is being linked to right now answers as it will in a moment.
    OpenPak::WaitForFirmwareUserLink(text);
    return text == OpenPak::Platform::ProfileId() && OpenPak::Session::Linked(text);
}

Result LinkedIdToken(const Common::UUID& uid, std::string& token) {
    token.clear();
    if (!IsActiveLinked(uid)) {
        return ResultNetworkServiceAccountUnavailable;
    }
    const std::string text = UidString(uid);
    if (OpenPak::Session::NintendoAccountId(text) == 0) {
        return ResultNintendoAccountIdUnavailable;
    }
    token = OpenPak::Session::IdToken(text);
    return token.empty() ? ResultNetworkServiceAccountUnavailable : ResultSuccess;
}

std::optional<NintendoAccountInfo> NintendoAccount(const std::string& uid) {
    if (uid.empty() || !OpenPak::Session::Linked(uid)) {
        return std::nullopt;
    }
    return NintendoAccountInfo{
        .id = OpenPak::Session::NintendoAccountId(uid),
        .nickname = OpenPak::Session::Nickname(uid),
        .birthday = OpenPak::Session::Birthday(uid),
        .country = OpenPak::Session::Country(uid),
        .names = LoadNames(uid),
    };
}

std::vector<u8> FetchProfileImage(const std::string& url) {
    std::string host;
    std::string path;
    if (!SplitUrl(url, host, path)) {
        LOG_WARNING(Service_ACC, "acc: the account picture is not an https URL");
        return {};
    }

    const auto reply = OpenPak::Session::Send("GET", host, path, {}, {}, {});
    if (reply.status != 200 || reply.body.empty()) {
        LOG_WARNING(Service_ACC, "acc: the account picture could not be fetched (status {})",
                    reply.status);
        return {};
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    u8* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(reply.body.data()),
                                       static_cast<int>(reply.body.size()), &width, &height,
                                       &channels, STBI_rgb);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        LOG_WARNING(Service_ACC, "acc: the account picture is not a JPEG this build can read");
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return {};
    }

    // Always re-encoded: stb writes baseline JPEG, and qlaunch draws a progressive one as a white
    // circle.
    std::vector<u8> square(ImageDimensions * ImageDimensions * STBI_rgb);
    stbir_resize_uint8_srgb(pixels, width, height, 0, square.data(), ImageDimensions,
                            ImageDimensions, 0, STBI_rgb, STBIR_ALPHA_CHANNEL_NONE, 0);
    stbi_image_free(pixels);

    std::vector<u8> jpeg;
    if (!stbi_write_jpg_to_func(AppendJpeg, &jpeg, ImageDimensions, ImageDimensions, STBI_rgb,
                                square.data(), 90) ||
        jpeg.empty() || jpeg.size() > MaxImageSize) {
        LOG_WARNING(Service_ACC, "acc: the account picture could not be re-encoded");
        return {};
    }
    return jpeg;
}

bool EnsureAndAdopt(const std::string& uid) {
    if (uid.empty() || !OpenPak::Session::Ensure(uid)) {
        return false;
    }
    if (!OpenPak::Session::Linked(uid) || Adopted(uid)) {
        return true;
    }
    std::vector<u8> image;
    if (const std::string picture = OpenPak::Session::ThumbnailUrl(uid); !picture.empty()) {
        image = FetchProfileImage(picture);
    }
    QueueAdoption(Common::UUID{uid}, OpenPak::Session::Nickname(uid), std::move(image));
    return true;
}

void EnsureAndAdoptInBackground(const std::string& uid) {
    if (uid.empty()) {
        return;
    }
    {
        auto& state = Adoptions();
        std::lock_guard lock{state.mutex};
        if (!state.in_flight.insert(uid).second) {
            return;
        }
    }
    // Detached on purpose: it may sit in a network timeout, and nothing waits for it. It holds no
    // kernel object; the profile change it finds is applied on an IPC thread.
    std::thread([uid] {
        EnsureAndAdopt(uid);
        auto& state = Adoptions();
        std::lock_guard lock{state.mutex};
        state.in_flight.erase(uid);
        state.finished.notify_all();
    }).detach();
}

bool SignedInWithin(const std::string& uid, std::chrono::milliseconds max) {
    if (uid.empty() || !OpenPak::Session::Enabled()) {
        return false;
    }
    if (OpenPak::Session::NetworkServiceAccountId(uid) != 0) {
        return true;
    }
    EnsureAndAdoptInBackground(uid);
    auto& state = Adoptions();
    std::unique_lock lock{state.mutex};
    state.finished.wait_for(lock, max, [&] { return !state.in_flight.contains(uid); });
    lock.unlock();
    return OpenPak::Session::NetworkServiceAccountId(uid) != 0;
}

void QueueAdoption(const Common::UUID& uid, std::string nickname, std::vector<u8> image) {
    if (uid.IsInvalid() || (nickname.empty() && image.empty())) {
        return;
    }
    auto& state = Adoptions();
    std::lock_guard lock{state.mutex};
    state.pending[UidString(uid)] = Pending{std::move(nickname), std::move(image)};
}

void ApplyPendingAdoptions(ProfileManager& profile_manager) {
    auto& state = Adoptions();
    std::map<std::string, Pending> pending;
    {
        std::lock_guard lock{state.mutex};
        if (state.pending.empty()) {
            return;
        }
        pending.swap(state.pending);
    }

    for (auto& [uid_text, change] : pending) {
        const Common::UUID uid{uid_text};
        ProfileBase base{};
        UserData data{};
        if (!profile_manager.GetProfileBaseAndData(uid, base, data)) {
            // Not registered (yet): a registration makes the user from the link itself.
            continue;
        }

        bool changed = false;
        if (!change.nickname.empty() && UsernameText(base.username) != change.nickname) {
            base.username = ToUsername(change.nickname);
            changed = profile_manager.SetProfileBaseAndData(uid, base, data);
        }

        if (!change.image.empty()) {
            const auto path = ProfileImagePath(uid);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            Common::FS::IOFile file(path, Common::FS::FileAccessMode::Write,
                                    Common::FS::FileType::BinaryFile);
            changed = (file.IsOpen() && file.Write(change.image) == change.image.size()) || changed;
        }

        {
            std::lock_guard lock{state.mutex};
            state.adopted.insert(uid_text);
        }
        LOG_INFO(Service_ACC, "acc: a linked user's profile follows its account ({})",
                 changed ? "updated" : "unchanged");
    }
}

bool Adopted(const std::string& uid) {
    auto& state = Adoptions();
    std::lock_guard lock{state.mutex};
    return state.adopted.contains(uid);
}

} // namespace Service::Account::OpenPakAccount
