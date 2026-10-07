// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <filesystem>
#include <fstream>
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nifm/nifm.h"
#include "core/hle/service/server_manager.h"
#include "core/internal_network/emu_net_state.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "network/network.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include "common/container/unordered_map.h"
#include <common/settings.h>

#ifdef _WIN32
#include <windows.h>
#undef CreateEvent
#pragma push_macro("interface")
#undef interface
#include <wlanapi.h>
#pragma pop_macro("interface")
#ifdef _MSC_VER
#pragma comment(lib, "wlanapi.lib")
#endif
#endif

namespace {

// Avoids name conflict with Windows' CreateEvent macro.
[[nodiscard]] Kernel::KEvent* CreateKEvent(Service::KernelHelpers::ServiceContext& service_context,
                                           std::string&& name) {
    return service_context.CreateEvent(std::move(name));
}

} // Anonymous namespace

namespace Service::NIFM {

// The console has one network and it is wired: whatever carries the host's internet (Wi-Fi,
// mobile data, a LAN) is presented to the guest as an Ethernet link with one stored wired
// profile. Its uuid ("OPENPAK\0network\x01") is the same in EnumerateNetworkProfiles, the current
// profile and GetNetworkProfile, so a system applet that lists then fetches gets the same record
// back. No host network name ever reaches the guest or the log. Behaviour follows the Ryujinx
// Harbor (MIT-licensed Ryujinx base).
static u128 StoredProfileUuid() {
    static constexpr std::array<u8, 16> bytes{0x4f, 0x50, 0x45, 0x4e, 0x50, 0x41, 0x4b, 0x00,
                                              0x6e, 0x65, 0x74, 0x77, 0x6f, 0x72, 0x6b, 0x01};
    u128 uuid{};
    std::memcpy(uuid.data(), bytes.data(), sizeof(uuid));
    return uuid;
}

constexpr std::string_view StoredProfileName = "HarborNetwork";

// This is nn::nifm::RequestState
enum class RequestState : u32 {
    NotSubmitted = 1,
    Invalid = 1, ///< The duplicate 1 is intentional; it means both not submitted and error on HW.
    OnHold = 2,
    Accepted = 3,
    Blocking = 4,
};

// This is nn::nifm::NetworkInterfaceType
enum class NetworkInterfaceType : u32 {
    Invalid = 0,
    WiFi_Ieee80211 = 1,
    Ethernet = 2,
};

enum class InternetConnectionStatus : u8 {
    ConnectingUnknown1,
    ConnectingUnknown2,
    ConnectingUnknown3,
    ConnectingUnknown4,
    Connected,
};

// This is nn::nifm::NetworkProfileType
enum class NetworkProfileType : u32 {
    User,
    SsidList,
    Temporary,
};

// This is nn::nifm::IpAddressSetting
struct IpAddressSetting {
    bool is_automatic{};
    Network::IPv4Address ip_address{};
    Network::IPv4Address subnet_mask{};
    Network::IPv4Address default_gateway{};
};
static_assert(sizeof(IpAddressSetting) == 0xD, "IpAddressSetting has incorrect size.");

// This is nn::nifm::DnsSetting
struct DnsSetting {
    bool is_automatic{};
    Network::IPv4Address primary_dns{};
    Network::IPv4Address secondary_dns{};
};
static_assert(sizeof(DnsSetting) == 0x9, "DnsSetting has incorrect size.");

// This is nn::nifm::detail::sf::AccessPointData (for >= 19.0.0)
struct AccessPointDataV3 {
    u8 ssid_len;     // Length of SSID
    char ssid[0x20]; // SSID Name (not null-terminated)
    u8 unk_pad[11];  // Unk
    u8 strength;     // 0-3 = Signal Strengh Bar
    u8 pad2[3];      // Unk 3 bytes
    u8 visible;      // 0 = Hidden, 1 = Shows up in UI
    u8 has_password; // 0 = Open, 1 = Password Protected
    u8 uk1;          // Unk
    u8 is_available; // 0 = Gray, 1 = Connectable
};
static_assert(sizeof(AccessPointDataV3) == 52, "AccessPointData18 must be 64 bytes");

// This is nn::nifm::AuthenticationSetting
struct AuthenticationSetting {
    bool is_enabled{};
    std::array<char, 0x20> user{};
    std::array<char, 0x20> password{};
};
static_assert(sizeof(AuthenticationSetting) == 0x41, "AuthenticationSetting has incorrect size.");

// This is nn::nifm::ProxySetting
struct ProxySetting {
    bool is_enabled{};
    INSERT_PADDING_BYTES(1);
    u16 port{};
    std::array<char, 0x64> proxy_server{};
    AuthenticationSetting authentication{};
    INSERT_PADDING_BYTES(1);
};
static_assert(sizeof(ProxySetting) == 0xAA, "ProxySetting has incorrect size.");

// This is nn::nifm::IpSettingData
struct IpSettingData {
    IpAddressSetting ip_address_setting{};
    DnsSetting dns_setting{};
    ProxySetting proxy_setting{};
    u16 mtu{};
};
static_assert(sizeof(IpSettingData) == 0xC2, "IpSettingData has incorrect size.");

struct SfWirelessSettingData {
    u8 ssid_length{};
    std::array<char, 0x20> ssid{};
    u8 unknown_1{};
    u8 unknown_2{};
    u8 is_secured{};
    std::array<char, 0x41> passphrase{};
};
static_assert(sizeof(SfWirelessSettingData) == 0x65, "SfWirelessSettingData has incorrect size.");

struct NifmWirelessSettingData {
    u8 ssid_length{};
    std::array<char, 0x21> ssid{};
    u8 unknown_1{};
    INSERT_PADDING_BYTES(1);
    u32 unknown_2{};
    u32 unknown_3{};
    std::array<char, 0x41> passphrase{};
    INSERT_PADDING_BYTES(3);
};
static_assert(sizeof(NifmWirelessSettingData) == 0x70,
              "NifmWirelessSettingData has incorrect size.");

// This is nn::nifm::detail::sf::NetworkProfileData
#pragma pack(push, 1)
struct SfNetworkProfileData {
    IpSettingData ip_setting_data{};
    u128 uuid{};
    std::array<char, 0x40> network_name{};
    u8 profile_type;
    u8 interface_type;
    u8 is_auto_connect;
    u8 is_large_capacity;
    SfWirelessSettingData wireless_setting_data{};
    INSERT_PADDING_BYTES(1);
};
#pragma pack(pop)
static_assert(sizeof(SfNetworkProfileData) == 0x17C, "SfNetworkProfileData has incorrect size.");

// This is nn::nifm::NetworkProfileData
#pragma pack(push, 1)
struct NifmNetworkProfileData {
    u128 uuid{};
    std::array<char, 0x40> network_name{};
    NetworkProfileType network_profile_type{};
    NetworkInterfaceType network_interface_type{};
    bool is_auto_connect{};
    bool is_large_capacity{};
    INSERT_PADDING_BYTES(2);
    NifmWirelessSettingData wireless_setting_data{};
    IpSettingData ip_setting_data{};
};
#pragma pack(pop)
static_assert(sizeof(NifmNetworkProfileData) == 0x18E, "NifmNetworkProfileData has incorrect size.");

constexpr Result ResultPendingConnection{ErrorModule::NIFM, 111};
constexpr Result ResultInvalidInput{ErrorModule::NIFM, 112};
constexpr Result ResultNetworkCommunicationDisabled{ErrorModule::NIFM, 1111};


// A Wi-Fi scan completes at once with no access points: the guest's network is the host's,
// there is nothing to pick, and the host's own networks are nobody's business (location data).
class IScanRequest final : public ServiceFramework<IScanRequest> {
public:
    explicit IScanRequest(Core::System& system_)
        : ServiceFramework{system_, "IScanRequest"}, svc_ctx{system_, "IScanRequest"} {

        static const FunctionInfo functions[] = {
            {0, &IScanRequest::Submit, "Submit"},
            {1, &IScanRequest::IsProcessing, "IsProcessing"},
            {2, &IScanRequest::GetResult, "GetResult"},
            {3, &IScanRequest::GetSystemEventReadableHandle, "GetSystemEventReadableHandle"},
            {4, &IScanRequest::SetChannels, "SetChannels"},
        };
        RegisterHandlers(functions);

        evt_scan_complete = CreateKEvent(svc_ctx, "IScanRequest:Complete");
        evt_processing = CreateKEvent(svc_ctx, "IScanRequest:Processing");
    }

    ~IScanRequest() override {
        svc_ctx.CloseEvent(evt_scan_complete);
        svc_ctx.CloseEvent(evt_processing);
    }

private:
    void Submit(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called, completes at once with no access points");
        evt_scan_complete->Signal(system.Kernel());
        IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
    }

    void IsProcessing(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u8>(false);
    }

    void GetResult(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetSystemEventReadableHandle(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 2};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, evt_scan_complete->GetReadableEvent(),
                           evt_processing->GetReadableEvent());
    }

    void SetChannels(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called");
        IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext svc_ctx;

    Kernel::KEvent* evt_scan_complete{};
    Kernel::KEvent* evt_processing{};
};

class IRequest final : public ServiceFramework<IRequest> {
public:
    explicit IRequest(Core::System& system_)
        : ServiceFramework{system_, "IRequest"}, service_context{system_, "IRequest"} {
        static const FunctionInfo functions[] = {
            {0, &IRequest::GetRequestState, "GetRequestState"},
            {1, &IRequest::GetResult, "GetResult"},
            {2, &IRequest::GetSystemEventReadableHandles, "GetSystemEventReadableHandles"},
            {3, &IRequest::Cancel, "Cancel"},
            {4, &IRequest::Submit, "Submit"},
            {5, &IRequest::StubSuccess, "SetRequirement"},
            {6, &IRequest::SetRequirementPreset, "SetRequirementPreset"},
            // switchbrew has no 7; the Ryujinx Harbor's 7/8/9 are these three, one id low.
            {8, &IRequest::SetPriority, "SetPriority"},
            {9, &IRequest::SetNetworkProfileId, "SetNetworkProfileId"},
            {10, &IRequest::SetRejectable, "SetRejectable"},
            {11, &IRequest::SetConnectionConfirmationOption, "SetConnectionConfirmationOption"},
            {12, &IRequest::StubSuccess, "SetPersistent"},
            {13, &IRequest::StubSuccess, "SetInstant"},
            {14, &IRequest::StubSuccess, "SetSustainable"},
            {15, &IRequest::StubSuccess, "SetRawPriority"},
            {16, &IRequest::StubSuccess, "SetGreedy"},
            {17, &IRequest::StubSuccess, "SetSharable"},
            {18, &IRequest::StubSuccess, "SetRequirementByRevision"},
            {19, &IRequest::GetRequirement, "GetRequirement"},
            {20, &IRequest::GetRevision, "GetRevision"},
            {21, &IRequest::GetAppletInfo, "GetAppletInfo"},
            {22, &IRequest::GetAdditionalInfo, "GetAdditionalInfo"},
            {23, &IRequest::StubSuccess, "SetKeptInSleep"},
            {24, &IRequest::StubSuccess, "RegisterSocketDescriptor"},
            {25, &IRequest::StubSuccess, "UnregisterSocketDescriptor"},
            {26, nullptr, "GetNetworkAccessStatus"}, //21.0.0+
        };
        RegisterHandlers(functions);

        event1 = CreateKEvent(service_context, "IRequest:Event1");
        event2 = CreateKEvent(service_context, "IRequest:Event2");
        state = RequestState::NotSubmitted;
    }

    ~IRequest() override {
        service_context.CloseEvent(event1);
        service_context.CloseEvent(event2);
    }

private:
    void Submit(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "(STUBBED) called");

        // [OpenPak] Decide at once, from any state, and signal both events every time: a title
        // that waits on them or polls the state would otherwise hang in OnHold or never wake.
        const auto has_connection = Network::GetHostIPv4Address().has_value() &&
                                    !Settings::values.airplane_mode.GetValue();
        awaiting_connection = !has_connection;
        UpdateState(has_connection ? RequestState::Accepted : RequestState::OnHold);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetRequestState(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "(STUBBED) called");

        AcceptIfConnected();

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(state);
    }

    void SetRequirementPreset(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto param_1 = rp.Pop<u32>();

        LOG_WARNING(Service_NIFM, "(STUBBED) called, param_1={}", param_1);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Request options are recorded as set: the emulated network has one profile and no
    // priority or rejection logic.
    void SetPriority(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        priority = rp.Pop<u8>();
        LOG_DEBUG(Service_NIFM, "called, priority={}", priority);
        IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
    }

    void SetNetworkProfileId(HLERequestContext& ctx) {
        // The argument is an nn::util::Uuid (16 bytes), not an SSID length.
        IPC::RequestParser rp{ctx};
        network_profile_id = rp.PopRaw<u128>();
        LOG_DEBUG(Service_NIFM, "called, stored profile={}",
                  network_profile_id == StoredProfileUuid());
        IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
    }

    void SetRejectable(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        rejectable = rp.Pop<bool>();
        LOG_DEBUG(Service_NIFM, "called, rejectable={}", rejectable);
        IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
    }

    void GetResult(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "(STUBBED) called");

        AcceptIfConnected();

        const auto result = [this] {
            const auto has_connection = Network::GetHostIPv4Address().has_value() &&
                                        !Settings::values.airplane_mode.GetValue();
            switch (state) {
            case RequestState::NotSubmitted:
                return has_connection ? ResultSuccess : ResultNetworkCommunicationDisabled;
            case RequestState::OnHold:
                if (has_connection) {
                    UpdateState(RequestState::Accepted);
                } else {
                    UpdateState(RequestState::Invalid);
                }
                return ResultPendingConnection;
            case RequestState::Accepted:
            default:
                return ResultSuccess;
            }
        }();

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result);
    }

    void GetRequirement(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        // nn::nifm::Requirement is 0x24 bytes on 3.0.0+; no requirement is tracked.
        constexpr std::array<u8, 0x24> requirement{};
        IPC::ResponseBuilder rb{ctx, 2 + sizeof(requirement) / sizeof(u32)};
        rb.Push(ResultSuccess);
        rb.PushRaw(requirement);
    }

    void GetSystemEventReadableHandles(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2, 2};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event1->GetReadableEvent(), event2->GetReadableEvent());
    }

    void Cancel(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        // The request goes back to Free/Error and says so on the first event.
        awaiting_connection = false;
        state = RequestState::Invalid;
        event1->Signal(system.Kernel());

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetConnectionConfirmationOption(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetAppletInfo(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        std::vector<u8> out_buffer(ctx.GetWriteBufferSize());

        ctx.WriteBuffer(out_buffer);

        IPC::ResponseBuilder rb{ctx, 5};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0);
        rb.Push<u32>(0);
        rb.Push<u32>(0);
    }

    // [OpenPak] A request submitted while the host had no address is accepted once the host has
    // one, without the title submitting again: a title that waits on the events or polls the
    // state has no reason to.
    void AcceptIfConnected() {
        if (!awaiting_connection || !Network::GetHostIPv4Address().has_value() ||
            Settings::values.airplane_mode.GetValue()) {
            return;
        }
        awaiting_connection = false;
        UpdateState(RequestState::Accepted);
    }

    void UpdateState(RequestState new_state) {
        LOG_DEBUG(Service_NIFM, "(STUBBED) called");
        state = new_state;
        event1->Signal(system.Kernel());
        // [OpenPak] Splatoon 3's NPLN readiness thread waits on the second event.
        event2->Signal(system.Kernel());
    }

    void StubSuccess(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetRevision(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(1); // Arbitrary revision number
    }

    void GetAdditionalInfo(HLERequestContext& ctx) {
        LOG_WARNING(Service_NIFM, "(STUBBED) called");

        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(0); // No additional info
    }

    KernelHelpers::ServiceContext service_context;

    RequestState state;
    // [OpenPak] Submitted with no host address, and not accepted since.
    bool awaiting_connection = false;
    u8 priority = 0;
    u128 network_profile_id{};
    bool rejectable = false;

    Kernel::KEvent* event1;
    Kernel::KEvent* event2;

    std::thread connect_worker;
    std::atomic<bool> connect_done{false};
    std::atomic<Result> connect_result{ResultPendingConnection};
};

class INetworkProfile final : public ServiceFramework<INetworkProfile> {
public:
    explicit INetworkProfile(Core::System& system_) : ServiceFramework{system_, "INetworkProfile"} {
        static const FunctionInfo functions[] = {
            {0, nullptr, "Update"},
            {1, nullptr, "PersistOld"},
            {2, nullptr, "Persist"},
        };
        RegisterHandlers(functions);
    }
};

// The one stored profile: wired, automatic addressing, the host's address when it has one.
static SfNetworkProfileData MakeStoredProfile() {
    Network::RefreshFromHost();
    auto& st = Network::EmuNetState::Get();

    SfNetworkProfileData profile{};
    profile.uuid = StoredProfileUuid();
    std::memcpy(profile.network_name.data(), StoredProfileName.data(), StoredProfileName.size());
    profile.profile_type = static_cast<u8>(NetworkProfileType::User);
    profile.interface_type = static_cast<u8>(NetworkInterfaceType::Ethernet);
    profile.is_auto_connect = 1;

    profile.ip_setting_data.ip_address_setting.is_automatic = true;
    profile.ip_setting_data.dns_setting.is_automatic = true;
    profile.ip_setting_data.mtu = 1500;
    {
        std::scoped_lock lock{st.mtx};
        if (st.connected) {
            profile.ip_setting_data.ip_address_setting.ip_address = st.ip;
            profile.ip_setting_data.ip_address_setting.subnet_mask = st.mask;
            profile.ip_setting_data.ip_address_setting.default_gateway = st.gw;
            profile.ip_setting_data.dns_setting.primary_dns = {1, 1, 1, 1};
            profile.ip_setting_data.dns_setting.secondary_dns = {1, 0, 0, 1};
        }
    }

    if (auto room_member = Network::GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            profile.ip_setting_data.ip_address_setting.ip_address = room_member->GetFakeIpAddress();
        }
    }
    return profile;
}

void IGeneralService::GetClientId(HLERequestContext& ctx) {
    static constexpr u32 client_id = 1;
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u64>(client_id); // Client ID needs to be non zero otherwise it's considered invalid
}

void IGeneralService::CreateScanRequest(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};

    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IScanRequest>(ctx, system);
}

void IGeneralService::CreateRequest(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};

    rb.Push(ResultSuccess);
    rb.PushIpcInterface<IRequest>(ctx, system);
}

void IGeneralService::GetCurrentNetworkProfile(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    ctx.WriteBuffer(MakeStoredProfile());

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// One interface, "eth0": Ethernet whatever the host link is, connected when the host has one.
void IGeneralService::EnumerateNetworkInterfaces(HLERequestContext& ctx) {
    constexpr size_t kEntry = 0x3F0;
    std::vector<u8> entry(kEntry, 0);

    const u32 type = static_cast<u32>(NetworkInterfaceType::Ethernet);
    const u32 enabled = 1;
    std::memcpy(entry.data() + 0x0, &type, sizeof(type));
    std::memcpy(entry.data() + 0x4, &enabled, sizeof(enabled));

    Network::RefreshFromHost();
    auto& st = Network::EmuNetState::Get();
    {
        std::scoped_lock lock{st.mtx};
        if (st.connected) {
            std::memcpy(entry.data() + 0x18, st.ip.data(), st.ip.size());
            std::memcpy(entry.data() + 0x1C, st.mask.data(), st.mask.size());
            std::memcpy(entry.data() + 0x20, st.gw.data(), st.gw.size());
        }
    }
    constexpr std::string_view name = "eth0";
    std::memcpy(entry.data() + 0x2E0, name.data(), name.size());

    const size_t guest_bytes = ctx.GetWriteBufferSize();
    if (guest_bytes != 0) {
        ctx.WriteBuffer(entry.data(), (std::min)(guest_bytes, entry.size()));
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u32>(1);
}

void IGeneralService::EnumerateNetworkProfiles(HLERequestContext& ctx) {
    const u128 uuid = StoredProfileUuid();
    if (ctx.GetWriteBufferSize() >= sizeof(uuid)) {
        ctx.WriteBuffer(&uuid, sizeof(uuid));
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(1);
}

// NetConnect asks for the profile EnumerateNetworkProfiles listed; any uuid gets the one profile.
void IGeneralService::GetNetworkProfile(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    ctx.WriteBuffer(MakeStoredProfile());

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// The edited profile is accepted and dropped (the network comes from the host); the stored
// profile keeps its uuid. Nothing it carries, a Wi-Fi passphrase included, is kept.
void IGeneralService::SetNetworkProfile(HLERequestContext& ctx) {
    LOG_INFO(Service_NIFM, "SetNetworkProfile accepted, not stored ({} bytes)",
             ctx.GetReadBufferSize());

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(StoredProfileUuid());
}

void IGeneralService::RemoveNetworkProfile(HLERequestContext& ctx) {
    LOG_INFO(Service_NIFM, "RemoveNetworkProfile accepted; the stored profile stays");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// No access point is ever listed (see IScanRequest); the output array is zeroed.
static void NoScanData(HLERequestContext& ctx) {
    const size_t guest_bytes = ctx.GetWriteBufferSize();
    if (guest_bytes != 0) {
        const std::vector<u8> zeros(guest_bytes, 0);
        ctx.WriteBuffer(zeros);
    }
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(0);
}

void IGeneralService::GetScanData(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called, 0 access points");
    NoScanData(ctx);
}

void IGeneralService::GetScanDataV2(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called, 0 access points");
    NoScanData(ctx);
}

void IGeneralService::GetScanDataV3(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called, 0 access points");
    NoScanData(ctx);
}

void IGeneralService::GetCurrentIpAddress(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    auto ipv4 = Network::GetHostIPv4Address();
    if (!ipv4) {
        LOG_ERROR(Service_NIFM, "Couldn't get host IPv4 address, defaulting to 0.0.0.0");
        ipv4.emplace(Network::IPv4Address{0, 0, 0, 0});
    }

    if (auto room_member = Network::GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            ipv4 = room_member->GetFakeIpAddress();
        }
    }

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw(*ipv4);
}

void IGeneralService::CreateTemporaryNetworkProfile(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    ASSERT_MSG(ctx.GetReadBufferSize() == 0x17c, "SfNetworkProfileData is not the correct size");
    u128 uuid{};
    auto buffer = ctx.ReadBuffer();
    std::memcpy(&uuid, buffer.data() + 8, sizeof(u128));

    IPC::ResponseBuilder rb{ctx, 6, 0, 1};

    rb.Push(ResultSuccess);
    rb.PushIpcInterface<INetworkProfile>(ctx, system);
    rb.PushRaw<u128>(uuid);
}

void IGeneralService::GetCurrentIpConfigInfo(HLERequestContext& ctx) {
    Network::RefreshFromHost();
    const auto& st = Network::EmuNetState::Get();

    struct IpConfigInfo {
        IpAddressSetting ip{};
        DnsSetting dns{};
    };
    static_assert(sizeof(IpConfigInfo) == sizeof(IpAddressSetting) + sizeof(DnsSetting));

    IpConfigInfo info{};

    if (st.connected) {
        info.ip.is_automatic = true;
        info.ip.ip_address = st.ip;
        info.ip.subnet_mask = st.mask;
        info.ip.default_gateway = st.gw;

        info.dns.is_automatic = true;
        info.dns.primary_dns = {1, 1, 1, 1};
        info.dns.secondary_dns = {8, 8, 8, 8};
    }

    if (auto room_member = Network::GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            info.ip.ip_address = room_member->GetFakeIpAddress();
        }
        info.dns.is_automatic = true;
        info.dns.primary_dns = {1, 1, 1, 1};
        info.dns.secondary_dns = {8, 8, 8, 8};
    }

    IPC::ResponseBuilder rb{ctx, 2 + (sizeof(IpConfigInfo) + 3) / sizeof(u32)};
    rb.Push(ResultSuccess);
    rb.PushRaw(info);
}

void IGeneralService::SetWirelessCommunicationEnabled(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u8 enable = rp.Pop<u8>();

    Settings::values.airplane_mode.SetValue(enable == 0);

    IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
}

void IGeneralService::IsWirelessCommunicationEnabled(HLERequestContext& ctx) {
    const bool en = !Settings::values.airplane_mode.GetValue();
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(en);
}

void IGeneralService::GetInternetConnectionStatus(HLERequestContext& ctx) {

    Network::RefreshFromHost();

    const auto& st = Network::EmuNetState::Get();
    struct Output {
        u8 type; // 1 Wi-Fi, 2 Ethernet
        u8 bars; // 0-3
        InternetConnectionStatus state;
    };
    Output out{};

    // Wired, so no signal bars.
    out.type = static_cast<u8>(NetworkInterfaceType::Ethernet);
    out.bars = 0;
    out.state = st.connected ? InternetConnectionStatus::Connected
                             : InternetConnectionStatus::ConnectingUnknown1;

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.PushRaw(out);
}

void IGeneralService::SetEthernetCommunicationEnabled(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u8 enable = rp.Pop<u8>();

    Network::EmuNetState::Get().ethernet_enabled.store(enable != 0);

    IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
}

// The wired link is always enabled; airplane mode only switches wireless off.
void IGeneralService::IsEthernetCommunicationEnabled(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(1);
}

void IGeneralService::IsAnyInternetRequestAccepted(HLERequestContext& ctx) {
    LOG_ERROR(Service_NIFM, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    if (Network::GetHostIPv4Address().has_value() && !Settings::values.airplane_mode.GetValue()) {
        rb.Push<u8>(1);
    } else {
        rb.Push<u8>(0);
    }
}

void IGeneralService::IsAnyForegroundRequestAccepted(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(true);
}

// One client owning the network is a console power-management idea; emulation has nothing to
// arbitrate, so the claim is accepted and forgotten.
void IGeneralService::SetExclusiveClient(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called");
    IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
}

void IGeneralService::SetDefaultIpSetting(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "called, accepted and not stored");
    IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
}

// Test-only switches titles poke before they trust the network: the answers they gate stay yes.
void IGeneralService::SetWirelessCommunicationEnabledForTest(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    LOG_DEBUG(Service_NIFM, "called, enabled={}", rp.Pop<bool>());
    IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
}

void IGeneralService::SetEthernetCommunicationEnabledForTest(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    LOG_DEBUG(Service_NIFM, "called, enabled={}", rp.Pop<bool>());
    IPC::ResponseBuilder{ctx, 2}.Push(ResultSuccess);
}

void IGeneralService::GetSsidListVersion(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called");

    constexpr u32 ssid_list_version = 0;

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(ssid_list_version);
}

void IGeneralService::ConfirmSystemAvailability(HLERequestContext& ctx) {
    LOG_DEBUG(Service_NIFM, "(STUBBED) called.");

    // TODO (jarrodnorwell)

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void IGeneralService::SetBackgroundRequestEnabled(HLERequestContext& ctx) {
    LOG_WARNING(Service_NIFM, "(STUBBED) called.");

    // TODO (jarrodnorwell)

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

// The link is wired, so there is no access point: the record stays zeroed.
void IGeneralService::GetCurrentAccessPoint(HLERequestContext& ctx) {
    ctx.WriteBuffer(AccessPointDataV3{});

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

IGeneralService::IGeneralService(Core::System& system_)
    : ServiceFramework{system_, "IGeneralService"} {
    // clang-format off

    static const FunctionInfo functions[] = {
        {1, &IGeneralService::GetClientId, "GetClientId"},
        {2, &IGeneralService::CreateScanRequest, "CreateScanRequest"},
        {4, &IGeneralService::CreateRequest, "CreateRequest"},
        {5, &IGeneralService::GetCurrentNetworkProfile, "GetCurrentNetworkProfile"},
        {6, &IGeneralService::EnumerateNetworkInterfaces, "EnumerateNetworkInterfaces"},
        {7, &IGeneralService::EnumerateNetworkProfiles, "EnumerateNetworkProfiles"},
        {8, &IGeneralService::GetNetworkProfile, "GetNetworkProfile"},
        {9, &IGeneralService::SetNetworkProfile, "SetNetworkProfile"},
        {10, &IGeneralService::RemoveNetworkProfile, "RemoveNetworkProfile"},
        {11, &IGeneralService::GetScanData, "GetScanDataOld"},
        {12, &IGeneralService::GetCurrentIpAddress, "GetCurrentIpAddress"},
        {13, nullptr, "GetCurrentAccessPointOld"},
        {14, &IGeneralService::CreateTemporaryNetworkProfile, "CreateTemporaryNetworkProfile"},
        {15, &IGeneralService::GetCurrentIpConfigInfo, "GetCurrentIpConfigInfo"},
        {16, &IGeneralService::SetWirelessCommunicationEnabled, "SetWirelessCommunicationEnabled"},
        {17, &IGeneralService::IsWirelessCommunicationEnabled, "IsWirelessCommunicationEnabled"},
        {18, &IGeneralService::GetInternetConnectionStatus, "GetInternetConnectionStatus"},
        {19, &IGeneralService::SetEthernetCommunicationEnabled, "SetEthernetCommunicationEnabled"},
        {20, &IGeneralService::IsEthernetCommunicationEnabled, "IsEthernetCommunicationEnabled"},
        {21, &IGeneralService::IsAnyInternetRequestAccepted, "IsAnyInternetRequestAccepted"},
        {22, &IGeneralService::IsAnyForegroundRequestAccepted, "IsAnyForegroundRequestAccepted"},
        {23, nullptr, "PutToSleep"},
        {24, nullptr, "WakeUp"},
        {25, &IGeneralService::GetSsidListVersion, "GetSsidListVersion"},
        {26, &IGeneralService::SetExclusiveClient, "SetExclusiveClient"},
        {27, nullptr, "GetDefaultIpSetting"},
        {28, &IGeneralService::SetDefaultIpSetting, "SetDefaultIpSetting"},
        {29, &IGeneralService::SetWirelessCommunicationEnabledForTest, "SetWirelessCommunicationEnabledForTest"},
        {30, &IGeneralService::SetEthernetCommunicationEnabledForTest, "SetEthernetCommunicationEnabledForTest"},
        {31, nullptr, "GetTelemetorySystemEventReadableHandle"},
        {32, nullptr, "GetTelemetryInfo"},
        {33, &IGeneralService::ConfirmSystemAvailability, "ConfirmSystemAvailability"}, // 2.0.0+
        {34, &IGeneralService::SetBackgroundRequestEnabled, "SetBackgroundRequestEnabled"}, // 4.0.0+
        {35, &IGeneralService::GetScanDataV2, "GetScanData"},
        {36, &IGeneralService::GetCurrentAccessPoint, "GetCurrentAccessPoint"},
        {37, nullptr, "Shutdown"},
        {38, nullptr, "GetAllowedChannels"},
        {39, nullptr, "NotifyApplicationSuspended"},
        {40, nullptr, "SetAcceptableNetworkTypeFlag"},
        {41, nullptr, "GetAcceptableNetworkTypeFlag"},
        {42, nullptr, "NotifyConnectionStateChanged"},
        {43, nullptr, "SetWowlDelayedWakeTime"},
        {44, nullptr, "IsWiredConnectionAvailable"}, // 18.0.0+
        {45, nullptr, "IsNetworkEmulationFeatureEnabled"}, // 18.0.0+
        {46, nullptr, "SelectActiveNetworkEmulationProfileIdForDebug"}, // 18.0.0+
        {47, &IGeneralService::GetScanDataV3, "GetScanData"}, // 19.0.0+
        {50, nullptr, "IsRewriteFeatureEnabled"}, // 18.0.0+
        {51, nullptr, "CreateRewriteRule"}, // 18.0.0+
        {52, nullptr, "DestroyRewriteRule"} // 18.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);
}

IGeneralService::~IGeneralService() = default;

class NetworkInterface final : public ServiceFramework<NetworkInterface> {
public:
    explicit NetworkInterface(const char* name, Core::System& system_)
        : ServiceFramework{system_, name} {
        static const FunctionInfo functions[] = {
            {4, &NetworkInterface::CreateGeneralServiceOld, "CreateGeneralServiceOld"},
            {5, &NetworkInterface::CreateGeneralService, "CreateGeneralService"},
        };
        RegisterHandlers(functions);
    }

private:
    void CreateGeneralServiceOld(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IGeneralService>(ctx, system);
    }

    void CreateGeneralService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NIFM, "called");

        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IGeneralService>(ctx, system);
    }
};

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);

    server_manager->RegisterNamedService("nifm:a", std::make_shared<NetworkInterface>("nifm:a", system), 2);
    server_manager->RegisterNamedService("nifm:s", std::make_shared<NetworkInterface>("nifm:s", system), 16);
    server_manager->RegisterNamedService("nifm:u", std::make_shared<NetworkInterface>("nifm:u", system), 5);
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::NIFM
