// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "core/harbor_calibration/enrolment_wire.h"

namespace Harbor::Enrolment {

namespace {

constexpr char ProofLabel[] = "OpenPak Harbor device enrolment v1";

std::string Base64(std::span<const u8> data) {
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data.data(),
                                     static_cast<int>(data.size()));
    out.resize(size < 0 ? 0 : static_cast<std::size_t>(size));
    return out;
}

std::vector<u8> Unbase64(const std::string& text) {
    if (text.empty() || text.size() % 4 != 0) {
        return {};
    }
    std::vector<u8> out(text.size() / 4 * 3);
    const int size = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<int>(text.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    for (auto it = text.rbegin(); it != text.rend() && *it == '=' && padding < 2; ++it) {
        ++padding;
    }
    out.resize(static_cast<std::size_t>(size) - padding);
    return out;
}

std::string Field(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

nlohmann::json Parse(std::string_view body) {
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    return parsed.is_object() ? parsed : nlohmann::json::object();
}

std::string Hex16(u64 value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

} // namespace

std::optional<Challenge> ParseChallenge(std::string_view body) {
    Challenge out;
    out.text = Field(Parse(body), "nonce");
    out.raw = Unbase64(out.text);
    if (out.raw.empty()) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> ProofMessage(std::span<const u8> nonce, u64 device_id) {
    std::vector<u8> message(ProofLabel, ProofLabel + sizeof(ProofLabel)); // with its NUL
    message.insert(message.end(), nonce.begin(), nonce.end());
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<u8>(device_id >> shift));
    }
    return message;
}

std::string RequestBody(std::span<const u8> cal0, const Cal0::Proofs& proofs,
                        const Challenge& challenge) {
    const nlohmann::json request{
        {"nonce", challenge.text},
        {"device_id", Hex16(Cal0::DeviceId(cal0))},
        {"serial", Cal0::Serial(cal0)},
        {"ecc_certificate", Base64(Cal0::Certificate(cal0, Cal0::EccB233DeviceCertificate))},
        {"eticket_certificate", Base64(Cal0::Certificate(cal0, Cal0::Rsa2048ETicketCertificate))},
        {"device_certificate", Base64(Cal0::Certificate(cal0, Cal0::Rsa2048DeviceCertificate))},
        {"tls_certificate", Base64(Cal0::SslCertificateDer(cal0))},
        {"proofs",
         {{"ecc", Base64(proofs.ecc)},
          {"eticket", Base64(proofs.eticket)},
          {"device", Base64(proofs.device)},
          {"tls", Base64(proofs.tls)}}},
    };
    return request.dump();
}

std::optional<Cal0::SignedCertificates> ParseSigned(std::string_view body) {
    const nlohmann::json answer = Parse(body);
    Cal0::SignedCertificates out{
        .ecc = Unbase64(Field(answer, "ecc_certificate")),
        .eticket = Unbase64(Field(answer, "eticket_certificate")),
        .device = Unbase64(Field(answer, "device_certificate")),
        .tls = Unbase64(Field(answer, "tls_certificate")),
    };
    if (out.ecc.empty() || out.eticket.empty() || out.device.empty() || out.tls.empty()) {
        return std::nullopt;
    }
    return out;
}

std::string ErrorCode(std::string_view body) {
    return Field(Parse(body), "error");
}

} // namespace Harbor::Enrolment
