// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <algorithm>
#include <deque>
#include <map>
#include <thread>
#include <utility>
#include <boost/asio.hpp>
#include "core/hle/service/ldn/local_wireless_codec.h"
#include "core/hle/service/ldn/local_wireless_transport.h"

namespace Service::LDN {
namespace asio = boost::asio;
using Error = boost::system::error_code;
using Tcp = asio::ip::tcp;
using Udp = asio::ip::udp;
using Address = LocalWirelessTransport::Address;
using namespace LocalWirelessCodec;

struct LocalWirelessTransport::Impl {
    struct Peer {
        explicit Peer(asio::io_context& io) : socket{io}, deadline{io} {}
        Tcp::socket socket;
        asio::steady_timer deadline;
        Address address{};
        bool outgoing{};
        bool joined{};
        std::array<std::uint8_t, HeaderSize> header{};
        std::vector<std::uint8_t> frame;
        std::deque<std::vector<std::uint8_t>> writes;
        std::vector<std::uint8_t> node;
    };

    asio::io_context io;
    Udp::socket udp{io};
    Tcp::acceptor listener{io};
    std::thread thread;
    Receiver receive;
    Address address{};
    Address mask{};
    Udp::endpoint sender;
    std::array<std::uint8_t, HeaderSize + MaxPayload + 1> datagram{};
    std::map<Address, std::shared_ptr<Peer>> peers;
    bool hosting{};

    bool OnSubnet(Address candidate) const {
        for (std::size_t i = 0; i < address.size(); ++i) {
            if ((candidate[i] & mask[i]) != (address[i] & mask[i])) {
                return false;
            }
        }
        return candidate != address;
    }

    void Deliver(LocalWirelessMessage type, Address from, std::vector<std::uint8_t> body = {}) {
        receive({type, from, std::move(body)});
    }

    void Drop(std::shared_ptr<Peer> peer, bool notify = true) {
        auto found = peers.find(peer->address);
        if (found == peers.end() || found->second != peer) {
            return;
        }
        peers.erase(found);
        peer->deadline.cancel();
        Error ignored;
        peer->socket.close(ignored);
        if (!notify) {
            return;
        }
        if (peer->outgoing) {
            Deliver(LocalWirelessMessage::Destroy, peer->address);
        } else if (peer->joined) {
            Deliver(LocalWirelessMessage::Disconnect, peer->address, peer->node);
        }
    }

    void Timeout(const std::shared_ptr<Peer>& peer) {
        peer->deadline.expires_after(std::chrono::seconds(5));
        peer->deadline.async_wait([this, peer](Error ec) {
            if (!ec) {
                Drop(peer);
            }
        });
    }

    bool Active(const std::shared_ptr<Peer>& peer) const {
        const auto found = peers.find(peer->address);
        return found != peers.end() && found->second == peer;
    }

    void Write(const std::shared_ptr<Peer>& peer) {
        asio::async_write(peer->socket, asio::buffer(peer->writes.front()),
                          [this, peer](Error ec, std::size_t) {
                              if (!Active(peer))
                                  return;
                              if (ec) {
                                  Drop(peer);
                                  return;
                              }
                              peer->writes.pop_front();
                              if (!peer->writes.empty()) {
                                  Write(peer);
                              }
                          });
    }

    void Queue(const std::shared_ptr<Peer>& peer, std::vector<std::uint8_t> frame) {
        if (peer->writes.size() >= 32) {
            Drop(peer);
            return;
        }
        peer->writes.push_back(std::move(frame));
        if (peer->writes.size() == 1) {
            Write(peer);
        }
    }

    void ReadHeader(const std::shared_ptr<Peer>& peer) {
        asio::async_read(
            peer->socket, asio::buffer(peer->header), [this, peer](Error ec, std::size_t) {
                if (!Active(peer))
                    return;
                if (ec || !ValidHeader(peer->header)) {
                    Drop(peer);
                    return;
                }
                const auto type = peer->header[4];
                if ((peer->outgoing && type != 3) ||
                    (!peer->outgoing && (type != 2 || peer->joined))) {
                    Drop(peer);
                    return;
                }
                peer->frame.assign(peer->header.begin(), peer->header.end());
                peer->frame.resize(HeaderSize + Read16(peer->header, 6));
                asio::async_read(
                    peer->socket,
                    asio::buffer(peer->frame.data() + HeaderSize, peer->frame.size() - HeaderSize),
                    [this, peer](Error body_ec, std::size_t) {
                        if (!Active(peer))
                            return;
                        auto body = Decode(peer->frame);
                        if (body_ec || !body || body->size() != (peer->outgoing ? 0x480 : 0x40)) {
                            Drop(peer);
                            return;
                        }
                        peer->joined = true;
                        peer->deadline.cancel();
                        if (!peer->outgoing) {
                            // NodeInfo stores IPv4 as a little-endian integer. The TCP peer is
                            // authoritative; don't let a client advertise another LAN machine.
                            std::reverse_copy(peer->address.begin(), peer->address.end(),
                                              body->begin());
                            peer->node = *body;
                        }
                        Deliver(peer->outgoing ? LocalWirelessMessage::Sync
                                               : LocalWirelessMessage::Connect,
                                peer->address, std::move(*body));
                        ReadHeader(peer);
                    });
            });
    }

    void Accept() {
        auto peer = std::make_shared<Peer>(io);
        listener.async_accept(peer->socket, [this, peer](Error ec) {
            if (ec) {
                return;
            }
            Error endpoint_ec;
            const auto endpoint = peer->socket.remote_endpoint(endpoint_ec);
            if (!endpoint_ec && endpoint.address().is_v4()) {
                peer->address = endpoint.address().to_v4().to_bytes();
                if (hosting && OnSubnet(peer->address) && peers.size() < 7 &&
                    !peers.contains(peer->address)) {
                    peers.emplace(peer->address, peer);
                    Timeout(peer);
                    ReadHeader(peer);
                }
            }
            Accept();
        });
    }

    void ReadDatagram() {
        udp.async_receive_from(asio::buffer(datagram), sender, [this](Error ec, std::size_t size) {
            if (!ec && sender.address().is_v4()) {
                const auto from = sender.address().to_v4().to_bytes();
                auto bytes = std::span{datagram}.first(size);
                auto body = Decode(bytes);
                if (OnSubnet(from) && body &&
                    ((bytes[4] == 0 && body->empty()) ||
                     (bytes[4] == 1 && body->size() == 0x480))) {
                    Deliver(static_cast<LocalWirelessMessage>(bytes[4]), from, std::move(*body));
                }
            }
            if (ec != asio::error::operation_aborted) {
                ReadDatagram();
            }
        });
    }

    void Send(LocalWirelessPacket packet) {
        if (packet.type == LocalWirelessMessage::Disconnect ||
            packet.type == LocalWirelessMessage::Destroy) {
            const auto found = peers.find(packet.peer);
            if (found != peers.end()) {
                Drop(found->second, false);
            }
            return;
        }
        auto frame = Encode(static_cast<std::uint8_t>(packet.type), packet.data);
        if (frame.empty()) {
            return;
        }
        if (packet.type == LocalWirelessMessage::Scan ||
            packet.type == LocalWirelessMessage::ScanResponse) {
            auto destination = packet.peer;
            if (packet.type == LocalWirelessMessage::Scan) {
                for (std::size_t i = 0; i < address.size(); ++i) {
                    destination[i] = static_cast<std::uint8_t>(address[i] | ~mask[i]);
                }
            }
            Error ec;
            udp.send_to(
                asio::buffer(frame),
                Udp::endpoint{asio::ip::address_v4{destination}, LocalWirelessTransport::Port}, 0,
                ec);
            return;
        }
        if (packet.type == LocalWirelessMessage::Connect) {
            // A station has one host. Close the previous session before changing hosts.
            while (!peers.empty()) {
                Drop(peers.begin()->second, false);
            }
            hosting = false;
            auto peer = std::make_shared<Peer>(io);
            peer->address = packet.peer;
            peer->outgoing = true;
            peers.emplace(peer->address, peer);
            Error ec;
            peer->socket.open(Tcp::v4(), ec);
            if (!ec) {
                peer->socket.bind(Tcp::endpoint{asio::ip::address_v4{address}, 0}, ec);
            }
            if (ec) {
                Drop(peer);
                return;
            }
            Timeout(peer);
            peer->socket.async_connect(
                Tcp::endpoint{asio::ip::address_v4{peer->address}, LocalWirelessTransport::Port},
                [this, peer, frame = std::move(frame)](Error connect_ec) mutable {
                    if (!Active(peer))
                        return;
                    if (connect_ec) {
                        Drop(peer);
                        return;
                    }
                    Queue(peer, std::move(frame));
                    ReadHeader(peer);
                });
            return;
        }
        const auto found = peers.find(packet.peer);
        if (found != peers.end()) {
            Queue(found->second, std::move(frame));
        }
    }
};

LocalWirelessTransport::LocalWirelessTransport() : impl{std::make_unique<Impl>()} {}
LocalWirelessTransport::~LocalWirelessTransport() {
    Stop();
}

bool LocalWirelessTransport::Start(Address address, Address mask, Receiver receiver,
                                   std::string& error) {
    auto& p = *impl;
    p.address = address;
    p.mask = mask;
    p.receive = std::move(receiver);
    Error ec;
    p.udp.open(Udp::v4(), ec);
    if (!ec)
        p.udp.set_option(asio::socket_base::broadcast{true}, ec);
    if (!ec)
        p.udp.bind(Udp::endpoint{Udp::v4(), Port}, ec);
    if (!ec)
        p.listener.open(Tcp::v4(), ec);
    if (!ec)
        p.listener.set_option(asio::socket_base::reuse_address{true}, ec);
    if (!ec)
        p.listener.bind(Tcp::endpoint{asio::ip::address_v4{address}, Port}, ec);
    if (!ec)
        p.listener.listen(7, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    p.ReadDatagram();
    p.Accept();
    p.thread = std::thread{[&p] { p.io.run(); }};
    return true;
}

void LocalWirelessTransport::Stop() {
    if (!impl)
        return;
    impl->io.stop();
    if (impl->thread.joinable())
        impl->thread.join();
    // Release ports and wake peers even if the owner retains this stopped transport.
    Error ignored;
    for (auto& [address, peer] : impl->peers) {
        peer->deadline.cancel();
        peer->socket.close(ignored);
    }
    impl->peers.clear();
    impl->listener.close(ignored);
    impl->udp.close(ignored);
}

void LocalWirelessTransport::SetHosting(bool hosting) {
    asio::post(impl->io, [p = impl.get(), hosting] {
        p->hosting = hosting;
        if (!hosting) {
            while (!p->peers.empty())
                p->Drop(p->peers.begin()->second, false);
        }
    });
}

void LocalWirelessTransport::Send(LocalWirelessPacket packet) {
    asio::post(impl->io, [p = impl.get(), packet = std::move(packet)]() mutable {
        p->Send(std::move(packet));
    });
}
} // namespace Service::LDN
