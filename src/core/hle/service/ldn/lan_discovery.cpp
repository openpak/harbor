// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "core/harbor_calibration/calibration.h"
#include "core/hle/service/ldn/lan_discovery.h"
#include "core/hle/service/ldn/local_wireless_transport.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"

namespace Service::LDN {

LanStation::LanStation(s8 node_id_, LANDiscovery* discovery_)
    : node_info(nullptr), status(NodeStatus::Disconnected), node_id(node_id_),
      discovery(discovery_) {}

LanStation::~LanStation() = default;

NodeStatus LanStation::GetStatus() const {
    return status;
}

void LanStation::OnClose() {
    LOG_INFO(Service_LDN, "OnClose {}", node_id);
    Reset();
    discovery->UpdateNodes();
}

void LanStation::Reset() {
    status = NodeStatus::Disconnected;
};

void LanStation::OverrideInfo() {
    bool connected = GetStatus() == NodeStatus::Connected;
    node_info->node_id = node_id;
    node_info->is_connected = connected ? 1 : 0;
}

LANDiscovery::LANDiscovery()
    : stations({{{1, this}, {2, this}, {3, this}, {4, this}, {5, this}, {6, this}, {7, this}}}){}

LANDiscovery::~LANDiscovery() {
    if (inited) {
        Result rc = Finalize();
        LOG_INFO(Service_LDN, "Finalize: {}", rc.raw);
    }
}

void LANDiscovery::InitNetworkInfo() {
    network_info.common.bssid = GetFakeMac();
    network_info.common.channel = WifiChannel::Wifi24_6;
    network_info.common.link_level = LinkLevel::Good;
    network_info.common.network_type = PackedNetworkType::Ldn;
    network_info.common.ssid = fake_ssid;

    auto& nodes = network_info.ldn.nodes;
    for (std::size_t i = 0; i < NodeCountMax; i++) {
        nodes[i].node_id = static_cast<s8>(i);
        nodes[i].is_connected = 0;
    }
}

void LANDiscovery::InitNodeStateChange() {
    for (auto& node_update : node_changes) {
        node_update.state_change = NodeStateChange::None;
    }
    for (auto& node_state : node_last_states) {
        node_state = 0;
    }
}

State LANDiscovery::GetState() const {
    std::scoped_lock lock{packet_mutex};
    return state;
}

void LANDiscovery::SetState(State new_state) {
    state = new_state;
}

Result LANDiscovery::GetNetworkInfo(NetworkInfo& out_network) const {
    std::scoped_lock lock{packet_mutex};
    if (state == State::AccessPointCreated || state == State::StationConnected) {
        std::memcpy(&out_network, &network_info, sizeof(network_info));
        return ResultSuccess;
    }

    return ResultBadState;
}

Result LANDiscovery::GetNetworkInfo(NetworkInfo& out_network,
                                    std::span<NodeLatestUpdate> out_updates) {
    std::scoped_lock lock{packet_mutex};
    if (out_updates.size() > NodeCountMax) {
        return ResultInvalidBufferCount;
    }

    if (state == State::AccessPointCreated || state == State::StationConnected) {
        std::memcpy(&out_network, &network_info, sizeof(network_info));
        for (std::size_t i = 0; i < out_updates.size(); i++) {
            out_updates[i].state_change = node_changes[i].state_change;
            node_changes[i].state_change = NodeStateChange::None;
        }
        return ResultSuccess;
    }

    return ResultBadState;
}

DisconnectReason LANDiscovery::GetDisconnectReason() const {
    std::scoped_lock lock{packet_mutex};
    return disconnect_reason;
}

Result LANDiscovery::Scan(std::span<NetworkInfo> out_networks, s16& out_count,
                          const ScanFilter& filter) {
    out_count = 0;
    {
        std::scoped_lock lock{packet_mutex};
        if (!inited) {
            return ResultBadState;
        }
        scan_results.clear();

        SendBroadcast(Network::LDNPacketType::Scan);
    }

    LOG_INFO(Service_LDN, "Waiting for scan replies");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::scoped_lock lock{packet_mutex};
    for (const auto& [key, info] : scan_results) {
        if (out_count >= static_cast<s16>(out_networks.size())) {
            break;
        }

        if (IsFlagSet(filter.flag, ScanFilterFlag::LocalCommunicationId)) {
            if (filter.network_id.intent_id.local_communication_id !=
                info.network_id.intent_id.local_communication_id) {
                continue;
            }
        }
        if (IsFlagSet(filter.flag, ScanFilterFlag::SessionId)) {
            if (filter.network_id.session_id != info.network_id.session_id) {
                continue;
            }
        }
        if (IsFlagSet(filter.flag, ScanFilterFlag::NetworkType)) {
            if (filter.network_type != static_cast<NetworkType>(info.common.network_type)) {
                continue;
            }
        }
        if (IsFlagSet(filter.flag, ScanFilterFlag::Ssid)) {
            if (filter.ssid != info.common.ssid) {
                continue;
            }
        }
        if (IsFlagSet(filter.flag, ScanFilterFlag::SceneId)) {
            if (filter.network_id.intent_id.scene_id != info.network_id.intent_id.scene_id) {
                continue;
            }
        }

        out_networks[out_count++] = info;
    }

    return ResultSuccess;
}

Result LANDiscovery::SetAdvertiseData(std::span<const u8> data) {
    std::scoped_lock lock{packet_mutex};
    const std::size_t size = data.size();
    if (size > AdvertiseDataSizeMax) {
        return ResultAdvertiseDataTooLarge;
    }

    std::memcpy(network_info.ldn.advertise_data.data(), data.data(), size);
    network_info.ldn.advertise_data_size = static_cast<u16>(size);

    UpdateNodes();

    return ResultSuccess;
}

Result LANDiscovery::OpenAccessPoint() {
    std::scoped_lock lock{packet_mutex};
    disconnect_reason = DisconnectReason::None;
    if (state == State::None) {
        return ResultBadState;
    }

    ResetStations();
    SetState(State::AccessPointOpened);

    return ResultSuccess;
}

Result LANDiscovery::CloseAccessPoint() {
    std::scoped_lock lock{packet_mutex};
    if (state == State::None) {
        return ResultBadState;
    }

    if (state == State::AccessPointCreated) {
        DestroyNetworkLocked();
    }

    ResetStations();
    SetState(State::Initialized);

    return ResultSuccess;
}

Result LANDiscovery::OpenStation() {
    std::scoped_lock lock{packet_mutex};
    disconnect_reason = DisconnectReason::None;
    if (state == State::None) {
        return ResultBadState;
    }

    ResetStations();
    SetState(State::StationOpened);

    return ResultSuccess;
}

Result LANDiscovery::CloseStation() {
    std::scoped_lock lock{packet_mutex};
    if (state == State::None) {
        return ResultBadState;
    }

    if (state == State::StationConnected || connection_pending) {
        DisconnectLocked();
    }

    ResetStations();
    SetState(State::Initialized);

    return ResultSuccess;
}

Result LANDiscovery::CreateNetwork(const SecurityConfig& security_config,
                                   const UserConfig& user_config,
                                   const NetworkConfig& network_config) {
    std::scoped_lock lock{packet_mutex};

    if (state != State::AccessPointOpened) {
        return ResultBadState;
    }

    if (network_config.node_count_max == 0 || network_config.node_count_max > NodeCountMax) {
        return ResultInvalidNodeCount;
    }
    InitNetworkInfo();
    network_info.ldn.node_count_max = network_config.node_count_max;
    network_info.ldn.security_mode = security_config.security_mode;

    if (network_config.channel == WifiChannel::Default) {
        network_info.common.channel = WifiChannel::Wifi24_6;
    } else {
        network_info.common.channel = network_config.channel;
    }

    std::independent_bits_engine<std::mt19937, 64, u64> bits_engine;
    network_info.network_id.session_id.high = bits_engine();
    network_info.network_id.session_id.low = bits_engine();
    network_info.network_id.intent_id = network_config.intent_id;

    NodeInfo& node0 = network_info.ldn.nodes[0];
    const Result rc2 = GetNodeInfo(node0, user_config, network_config.local_communication_version);
    if (rc2.IsError()) {
        return ResultAccessPointConnectionFailed;
    }

    SetState(State::AccessPointCreated);

    if (local_transport) {
        local_transport->SetHosting(true);
    }
    InitNodeStateChange();
    node0.is_connected = 1;
    UpdateNodes();

    return rc2;
}

Result LANDiscovery::DestroyNetwork() {
    std::scoped_lock lock{packet_mutex};
    return DestroyNetworkLocked();
}

Result LANDiscovery::DestroyNetworkLocked() {
    for (auto local_ip : connected_clients) {
        SendPacket(Network::LDNPacketType::DestroyNetwork, local_ip);
    }

    if (local_transport) {
        local_transport->SetHosting(false);
    }
    ResetStations();

    SetState(State::AccessPointOpened);
    lan_event();

    return ResultSuccess;
}

Result LANDiscovery::Connect(const NetworkInfo& candidate, const UserConfig& user_config,
                             u16 local_communication_version) {
    std::unique_lock lock{packet_mutex};
    if (state != State::StationOpened || connection_pending) {
        return ResultBadState;
    }
    if (candidate.ldn.node_count == 0 || candidate.ldn.node_count_max > NodeCountMax ||
        candidate.ldn.node_count >= candidate.ldn.node_count_max) {
        return ResultInvalidNodeCount;
    }
    node_info = {};
    if (GetNodeInfo(node_info, user_config, local_communication_version).IsError()) {
        return ResultConnectionFailed;
    }
    network_info = candidate;
    auto node_host = candidate.ldn.nodes[0].ipv4_address;
    std::reverse(node_host.begin(), node_host.end());
    host_ip = node_host;
    InitNodeStateChange();
    connection_pending = true;
    SendPacket(Network::LDNPacketType::Connect, node_info, *host_ip);

    // Receiving SyncNetwork requires packet_mutex too. Waiting while holding it used to
    // report success before the receiver could process even one reply.
    connection_changed.wait_for(lock, std::chrono::seconds(5), [&] { return !connection_pending; });
    if (state == State::StationConnected) {
        return ResultSuccess;
    }
    connection_pending = false;
    if (host_ip) {
        SendPacket(Network::LDNPacketType::Disconnect, node_info, *host_ip);
        host_ip.reset();
    }
    return ResultConnectionFailed;
}

Result LANDiscovery::Disconnect() {
    std::scoped_lock lock{packet_mutex};
    return DisconnectLocked();
}

Result LANDiscovery::DisconnectLocked() {
    if (host_ip) {
        SendPacket(Network::LDNPacketType::Disconnect, node_info, *host_ip);
    }

    host_ip.reset();
    connection_pending = false;
    connection_changed.notify_all();
    disconnect_reason = DisconnectReason::DisconnectedByUser;
    SetState(State::StationOpened);
    lan_event();

    return ResultSuccess;
}

Result LANDiscovery::Initialize(LanEventFunc lan_event_, bool listening) {
    std::scoped_lock lock{packet_mutex};
    if (inited) {
        return ResultSuccess;
    }

    for (auto& station : stations) {
        station.discovery = this;
        station.node_info = &network_info.ldn.nodes[station.node_id];
        station.Reset();
    }

    connected_clients.clear();
    host_ip.reset();
    connection_pending = false;
    network_info = {};
    node_info = {};
    lan_event = lan_event_;
    use_room = false;
    if (auto room = Network::GetRoomMember().lock()) {
        use_room = room->IsConnected();
    }
    if (!use_room && listening) {
        const auto interface = Network::GetSelectedNetworkInterface();
        if (!interface) {
            return ResultNoIpAddress;
        }
        auto transport = std::make_unique<LocalWirelessTransport>();
        std::string error;
        if (!transport->Start(
                Network::TranslateIPv4(interface->ip_address),
                Network::TranslateIPv4(interface->subnet_mask),
                [this](LocalWirelessPacket packet) {
                    ReceivePacket({static_cast<Network::LDNPacketType>(packet.type),
                                   packet.peer,
                                   {},
                                   false,
                                   std::move(packet.data)},
                                  false);
                },
                error)) {
            LOG_ERROR(Service_LDN, "Local network transport could not start: {}", error);
            return ResultConnectionFailed;
        }
        local_transport = std::move(transport);
    }
    LOG_INFO(Service_LDN, "Local wireless transport: {}",
             use_room ? "emulator room" : "home LAN (ldn_mitm)");

    SetState(State::Initialized);

    inited = true;
    return ResultSuccess;
}

Result LANDiscovery::Finalize() {
    std::unique_ptr<LocalWirelessTransport> transport;
    {
        std::scoped_lock lock{packet_mutex};
        if (inited) {
            if (state == State::AccessPointCreated) {
                DestroyNetworkLocked();
            }
            if (state == State::StationConnected || connection_pending) {
                DisconnectLocked();
            }
            ResetStations();
            inited = false;
        }
        SetState(State::None);
        connection_pending = false;
        connection_changed.notify_all();
        transport = std::move(local_transport);
    }
    // A callback may be waiting for packet_mutex. Join only after releasing it.
    if (transport) {
        transport->Stop();
    }
    return ResultSuccess;
}

void LANDiscovery::ResetStations() {
    for (auto& station : stations) {
        station.Reset();
    }
    connected_clients.clear();
}

void LANDiscovery::UpdateNodes() {
    u8 count = 0;
    for (auto& station : stations) {
        bool connected = station.GetStatus() == NodeStatus::Connected;
        if (connected) {
            count++;
        }
        station.OverrideInfo();
    }
    network_info.ldn.node_count = count + 1;

    for (auto local_ip : connected_clients) {
        SendPacket(Network::LDNPacketType::SyncNetwork, network_info, local_ip);
    }

    OnNetworkInfoChanged();
}

void LANDiscovery::OnSyncNetwork(const NetworkInfo& info) {
    network_info = info;
    if (state == State::StationOpened) {
        SetState(State::StationConnected);
    }
    connection_pending = false;
    connection_changed.notify_all();
    OnNetworkInfoChanged();
}

void LANDiscovery::OnDisconnectFromHost() {
    LOG_INFO(Service_LDN, "OnDisconnectFromHost state: {}", static_cast<int>(state));
    host_ip = std::nullopt;
    disconnect_reason = DisconnectReason::SignalLost;
    connection_pending = false;
    connection_changed.notify_all();
    if (state == State::StationConnected) {
        SetState(State::StationOpened);
        lan_event();
    }
}

void LANDiscovery::OnNetworkInfoChanged() {
    if (IsNodeStateChanged()) {
        lan_event();
    }
    return;
}

Ipv4Address LANDiscovery::GetLocalAddress() const {
    std::scoped_lock lock{packet_mutex};
    return GetLocalIp();
}

Network::IPv4Address LANDiscovery::GetLocalIp() const {
    if (use_room) {
        if (auto room = Network::GetRoomMember().lock(); room && room->IsConnected()) {
            return room->GetFakeIpAddress();
        }
    }
    return Network::GetHostIPv4Address().value_or(Network::IPv4Address{});
}

template <typename Data>
void LANDiscovery::SendPacket(Network::LDNPacketType type, const Data& data,
                              Ipv4Address remote_ip) {
    Network::LDNPacket packet;
    packet.type = type;

    packet.broadcast = false;
    packet.local_ip = GetLocalIp();
    packet.remote_ip = remote_ip;

    packet.data.resize(sizeof(data));
    std::memcpy(packet.data.data(), &data, sizeof(data));
    SendPacket(packet);
}

void LANDiscovery::SendPacket(Network::LDNPacketType type, Ipv4Address remote_ip) {
    Network::LDNPacket packet;
    packet.type = type;

    packet.broadcast = false;
    packet.local_ip = GetLocalIp();
    packet.remote_ip = remote_ip;

    SendPacket(packet);
}

template <typename Data>
void LANDiscovery::SendBroadcast(Network::LDNPacketType type, const Data& data) {
    Network::LDNPacket packet;
    packet.type = type;

    packet.broadcast = true;
    packet.local_ip = GetLocalIp();

    packet.data.resize(sizeof(data));
    std::memcpy(packet.data.data(), &data, sizeof(data));
    SendPacket(packet);
}

void LANDiscovery::SendBroadcast(Network::LDNPacketType type) {
    Network::LDNPacket packet;
    packet.type = type;

    packet.broadcast = true;
    packet.local_ip = GetLocalIp();

    SendPacket(packet);
}

void LANDiscovery::SendPacket(const Network::LDNPacket& packet) {
    if (local_transport) {
        local_transport->Send(
            {static_cast<LocalWirelessMessage>(packet.type), packet.remote_ip, packet.data});
        return;
    }
    if (auto room_member = Network::GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            room_member->SendLdnPacket(packet);
        }
    }
}

void LANDiscovery::ReceivePacket(const Network::LDNPacket& packet, bool from_room) {
    std::scoped_lock lock{packet_mutex};
    if (!inited || use_room != from_room) {
        return;
    }
    switch (packet.type) {
    case Network::LDNPacketType::Scan: {
        LOG_INFO(Frontend, "Scan packet received!");
        if (state == State::AccessPointCreated) {
            // Reply to the sender
            SendPacket(Network::LDNPacketType::ScanResp, network_info, packet.local_ip);
        }
        break;
    }
    case Network::LDNPacketType::ScanResp: {
        LOG_INFO(Frontend, "ScanResp packet received!");

        if (packet.data.size() != sizeof(NetworkInfo)) {
            break;
        }

        NetworkInfo info{};
        std::memcpy(&info, packet.data.data(), sizeof(NetworkInfo));
        auto advertised_host = info.ldn.nodes[0].ipv4_address;
        std::reverse(advertised_host.begin(), advertised_host.end());
        if (info.common.ssid.length > SsidLengthMax ||
            info.ldn.advertise_data_size > AdvertiseDataSizeMax || info.ldn.node_count == 0 ||
            info.ldn.node_count_max > NodeCountMax ||
            info.ldn.node_count > info.ldn.node_count_max ||
            (!use_room && advertised_host != packet.local_ip) || scan_results.size() >= 128) {
            break;
        }
        scan_results.insert_or_assign(info.common.bssid, info);

        break;
    }
    case Network::LDNPacketType::Connect: {
        LOG_INFO(Frontend, "Connect packet received!");

        if (packet.data.size() != sizeof(NodeInfo)) {
            break;
        }

        NodeInfo info{};
        std::memcpy(&info, packet.data.data(), sizeof(NodeInfo));

        if (state != State::AccessPointCreated ||
            network_info.ldn.station_accept_policy == AcceptPolicy::RejectAll ||
            network_info.ldn.node_count >= network_info.ldn.node_count_max ||
            std::find(connected_clients.begin(), connected_clients.end(), packet.local_ip) !=
                connected_clients.end() ||
            info.local_communication_version !=
                network_info.ldn.nodes[0].local_communication_version) {
            if (local_transport) {
                SendPacket(Network::LDNPacketType::Disconnect, packet.local_ip);
            }
            break;
        }
        connected_clients.push_back(packet.local_ip);

        for (LanStation& station : stations) {
            if (station.status != NodeStatus::Connected) {
                *station.node_info = info;
                station.status = NodeStatus::Connected;
                break;
            }
        }

        UpdateNodes();

        break;
    }
    case Network::LDNPacketType::Disconnect: {
        LOG_INFO(Frontend, "Disconnect packet received!");

        connected_clients.erase(
            std::remove(connected_clients.begin(), connected_clients.end(), packet.local_ip),
            connected_clients.end());

        if (packet.data.size() != sizeof(NodeInfo)) {
            break;
        }

        NodeInfo info{};
        std::memcpy(&info, packet.data.data(), sizeof(NodeInfo));

        for (LanStation& station : stations) {
            if (station.status == NodeStatus::Connected &&
                station.node_info->mac_address == info.mac_address) {
                station.OnClose();
                break;
            }
        }

        break;
    }
    case Network::LDNPacketType::DestroyNetwork: {
        if (!host_ip || packet.local_ip != *host_ip) {
            break;
        }
        ResetStations();
        OnDisconnectFromHost();
        break;
    }
    case Network::LDNPacketType::SyncNetwork: {
        if (host_ip && packet.local_ip == *host_ip &&
            (connection_pending || state == State::StationConnected)) {
            if (packet.data.size() != sizeof(NetworkInfo)) {
                break;
            }

            NetworkInfo info{};
            std::memcpy(&info, packet.data.data(), sizeof(NetworkInfo));

            // A connected station only follows the session it joined.
            if (info.common.ssid.length > SsidLengthMax ||
                info.ldn.advertise_data_size > AdvertiseDataSizeMax || info.ldn.node_count == 0 ||
                info.ldn.node_count_max > NodeCountMax ||
                info.ldn.node_count > info.ldn.node_count_max ||
                info.network_id.session_id != network_info.network_id.session_id) {
                break;
            }

            const auto own = std::find_if(
                info.ldn.nodes.begin(), info.ldn.nodes.end(), [this](const NodeInfo& node) {
                    return node.is_connected && node.mac_address == node_info.mac_address;
                });
            if (own == info.ldn.nodes.end()) {
                break;
            }
            LOG_INFO(Frontend, "SyncNetwork packet received!");
            OnSyncNetwork(info);
        } else {
            LOG_INFO(Frontend, "SyncNetwork packet received but in wrong State!");
        }

        break;
    }
    default: {
        LOG_INFO(Frontend, "ReceivePacket unhandled type {}", static_cast<int>(packet.type));
        break;
    }
    }
}

bool LANDiscovery::IsNodeStateChanged() {
    bool changed = false;
    const auto& nodes = network_info.ldn.nodes;
    for (int i = 0; i < NodeCountMax; i++) {
        if (nodes[i].is_connected != node_last_states[i]) {
            if (nodes[i].is_connected) {
                node_changes[i].state_change |= NodeStateChange::Connect;
            } else {
                node_changes[i].state_change |= NodeStateChange::Disconnect;
            }
            node_last_states[i] = nodes[i].is_connected;
            changed = true;
        }
    }
    return changed;
}

bool LANDiscovery::IsFlagSet(ScanFilterFlag flag, ScanFilterFlag search_flag) const {
    const auto flag_value = static_cast<u32>(flag);
    const auto search_flag_value = static_cast<u32>(search_flag);
    return (flag_value & search_flag_value) == search_flag_value;
}

int LANDiscovery::GetStationCount() const {
    return static_cast<int>(
        std::count_if(stations.begin(), stations.end(), [](const auto& station) {
            return station.GetStatus() != NodeStatus::Disconnected;
        }));
}

MacAddress LANDiscovery::GetFakeMac() const {
    // This NAND's Wi-Fi MAC (its calibration), unique per install; the address-derived one only
    // when there is no calibration.
    const auto wlan_mac = Harbor::Calibration::Get().WlanMacAddress();
    if (std::any_of(wlan_mac.begin(), wlan_mac.end(), [](u8 b) { return b != 0; })) {
        MacAddress own{};
        std::memcpy(own.raw.data(), wlan_mac.data(), own.raw.size());
        return own;
    }

    MacAddress mac{};
    mac.raw[0] = 0x02;
    mac.raw[1] = 0x00;

    const auto ip = GetLocalIp();
    memcpy(mac.raw.data() + 2, &ip, sizeof(ip));

    return mac;
}

Result LANDiscovery::GetNodeInfo(NodeInfo& node, const UserConfig& userConfig,
                                 u16 localCommunicationVersion) {
    const auto network_interface = Network::GetSelectedNetworkInterface();

    if (!network_interface) {
        LOG_ERROR(Service_LDN, "No network interface available");
        return ResultNoIpAddress;
    }

    node.mac_address = GetFakeMac();
    node.is_connected = 1;
    std::memcpy(node.user_name.data(), userConfig.user_name.data(), UserNameBytesMax + 1);
    node.local_communication_version = localCommunicationVersion;

    Ipv4Address current_address = GetLocalIp();
    std::reverse(std::begin(current_address), std::end(current_address)); // ntohl
    node.ipv4_address = current_address;

    return ResultSuccess;
}

} // namespace Service::LDN
