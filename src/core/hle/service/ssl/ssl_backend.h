// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "core/hle/result.h"

namespace Network {
class SocketBase;
}

namespace Service::SSL {

constexpr Result ResultNoSocket{ErrorModule::SSLSrv, 103};
constexpr Result ResultInvalidSocket{ErrorModule::SSLSrv, 106};
constexpr Result ResultTimeout{ErrorModule::SSLSrv, 205};
// A handshake the host refused, a certificate that failed verification included: the guest sees a
// reset connection and shows its own retry, as the Ryujinx Harbor answers.
constexpr Result ResultConnectionReset{ErrorModule::SSLSrv, 209};
constexpr Result ResultInternalError{ErrorModule::SSLSrv, 999}; // made up

// ResultWouldBlock is returned from Read and Write, and oddly, DoHandshake,
// with no way in the latter case to distinguish whether the client should poll
// for read or write.  The one official client I've seen handles this by always
// polling for read (with a timeout).
constexpr Result ResultWouldBlock{ErrorModule::SSLSrv, 204};

class SSLConnectionBackend {
public:
    virtual ~SSLConnectionBackend() {}
    virtual void SetSocket(std::shared_ptr<Network::SocketBase> socket) = 0;
    // Recorded only: the host verifies every connection whatever the guest asks (ssl_trust.h).
    virtual void SetVerifyOption(u32 option) = 0;
    virtual Result DoHandshake() = 0;
    virtual Result Read(size_t* out_size, std::span<u8> data) = 0;
    virtual Result Peek(size_t* out_size, std::span<u8> data) = 0;
    virtual Result Write(size_t* out_size, std::span<const u8> data) = 0;
    virtual Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) = 0;
    virtual Result SetHostName(const char* hostname) = 0;
    virtual Result GetHostName(std::span<u8> hostname, u32* out_size) = 0;
    virtual int Pending() = 0;
    virtual Result SetRenegotiationMode(u32 mode) = 0;
    virtual Result GetRenegotiationMode(u32* mode) = 0;
    // The length-prefixed protocol list the guest offers, applied before the handshake.
    virtual void SetAlpnProtos(std::span<const u8> protos) {}
    // The protocol the server selected, empty when none.
    virtual std::vector<u8> GetNegotiatedAlpnProto() {
        return {};
    }
    virtual Result GetCipherInfo(std::string* cipher_name, std::string* protocol_version) {
        return ResultInternalError;
    }
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend);

// The pinned OpenPak CA (openpak/ca.pem) as DER, when the file holds a CA certificate; empty
// otherwise, and always on a build without a TLS backend.
std::vector<u8> LoadPinnedCaDer();

} // namespace Service::SSL
