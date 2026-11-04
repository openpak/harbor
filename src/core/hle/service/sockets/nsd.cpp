// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include <fmt/format.h>

#include "core/harbor_calibration/calibration.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/nsd.h"
#include "core/hle/service/sockets/sfdnsres_redirect.h"

#include "common/string_util.h"

namespace Service::Sockets {

constexpr Result ResultOverflow{ErrorModule::NSD, 6};

// This is nn::oe::ServerEnvironmentType
enum class ServerEnvironmentType : u8 {
    Dd,
    Lp,
    Sd,
    Sp,
    Dp,
};

// This is nn::nsd::EnvironmentIdentifier
struct EnvironmentIdentifier {
    std::array<u8, 8> identifier;
};
static_assert(sizeof(EnvironmentIdentifier) == 0x8);

NSD::NSD(Core::System& system_, const char* name) : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {5, nullptr, "GetSettingUrl"},
        {10, nullptr, "GetSettingName"},
        {11, &NSD::GetEnvironmentIdentifier, "GetEnvironmentIdentifier"},
        {12, &NSD::GetDeviceId, "GetDeviceId"},
        {13, nullptr, "DeleteSettings"},
        {14, nullptr, "ImportSettings"},
        {15, &NSD::SetChangeEnvironmentIdentifierDisabled, "SetChangeEnvironmentIdentifierDisabled"},
        {20, &NSD::Resolve, "Resolve"},
        {21, &NSD::ResolveEx, "ResolveEx"},
        {30, &NSD::GetNasServiceSetting, "GetNasServiceSetting"},
        {31, &NSD::GetNasServiceSettingEx, "GetNasServiceSettingEx"},
        {40, nullptr, "GetNasRequestFqdn"},
        {41, nullptr, "GetNasRequestFqdnEx"},
        {42, nullptr, "GetNasApiFqdn"},
        {43, nullptr, "GetNasApiFqdnEx"},
        {50, nullptr, "GetCurrentSetting"},
        {51, nullptr, "WriteTestParameter"},
        {52, nullptr, "ReadTestParameter"},
        {60, nullptr, "ReadSaveDataFromFsForTest"},
        {61, nullptr, "WriteSaveDataToFsForTest"},
        {62, nullptr, "DeleteSaveDataOfFsForTest"},
        {63, nullptr, "IsChangeEnvironmentIdentifierDisabled"},
        {64, nullptr, "SetWithoutDomainExchangeFqdns"},
        {100, &NSD::GetApplicationServerEnvironmentType, "GetApplicationServerEnvironmentType"},
        {101, nullptr, "SetApplicationServerEnvironmentType"},
        {102, nullptr, "DeleteApplicationServerEnvironmentType"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

// The substitutions nsd makes: '%' becomes the environment and the account names map to their
// BaaS hosts, except accounts.nintendo.com (sfdnsres_redirect.h). Behaviour follows the Ryujinx
// Harbor (MIT-licensed Ryujinx base).
static std::string ResolveImpl(const std::string& fqdn_in) {
    std::string resolved = Dns::NsdResolve(fqdn_in);
    LOG_DEBUG(Service, "called, {} -> {}", fqdn_in, resolved);
    return resolved;
}

static Result ResolveCommon(const std::string& fqdn_in, std::array<char, 0x100>& fqdn_out) {
    const auto res = ResolveImpl(fqdn_in);
    if (res.size() >= fqdn_out.size()) {
        return ResultOverflow;
    }
    std::memcpy(fqdn_out.data(), res.c_str(), res.size() + 1);
    return ResultSuccess;
}

void NSD::SetChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const bool disabled = rp.Pop<bool>();

    LOG_WARNING(Service, "(STUBBED) called, disabled={}", disabled);

    IPC::ResponseBuilder rb{ctx, 1};
    rb.Push(ResultSuccess);
}

void NSD::Resolve(HLERequestContext& ctx) {
    const std::string fqdn_in = Common::StringFromBuffer(ctx.ReadBuffer(0));

    std::array<char, 0x100> fqdn_out{};
    const Result res = ResolveCommon(fqdn_in, fqdn_out);

    ctx.WriteBuffer(fqdn_out);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(res);
}

void NSD::ResolveEx(HLERequestContext& ctx) {
    const std::string fqdn_in = Common::StringFromBuffer(ctx.ReadBuffer(0));

    std::array<char, 0x100> fqdn_out;
    const Result res = ResolveCommon(fqdn_in, fqdn_out);

    if (res.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
        return;
    }

    ctx.WriteBuffer(fqdn_out);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(ResultSuccess);
}

// 30 and 31: the Nintendo Account service a title names (an input string) gives a setting of
// 0x108 bytes. A console that has no entry for the name answers with an error, and Stumble Guys
// turned that into 2511-8191 on its first button press. Harbor has no per-title list, so the
// setting is all zero and the name is logged, which is what the title needs to go on and what the
// next step to learn is.
static void WriteNasServiceSetting(HLERequestContext& ctx) {
    const std::string name = Common::StringFromBuffer(ctx.ReadBuffer(0));
    LOG_WARNING(Service,
                "unknown contract: nsd GetNasServiceSetting name='{}' (buffer {} bytes); "
                "answered with an empty setting",
                name, ctx.GetWriteBufferSize());
    std::array<u8, 0x108> setting{};
    ctx.WriteBuffer(setting.data(), std::min(setting.size(), ctx.GetWriteBufferSize()));
}

void NSD::GetNasServiceSetting(HLERequestContext& ctx) {
    WriteNasServiceSetting(ctx);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetNasServiceSettingEx(HLERequestContext& ctx) {
    WriteNasServiceSetting(ctx);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(ResultSuccess);
}

void NSD::GetEnvironmentIdentifier(HLERequestContext& ctx) {
    constexpr EnvironmentIdentifier lp1 = {
        .identifier = {'l', 'p', '1', '\0', '\0', '\0', '\0', '\0'}};
    ctx.WriteBuffer(lp1);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetDeviceId(HLERequestContext& ctx) {
    // The device id as 16 hex digits, the form the device certificate names it in.
    LOG_WARNING(Service, "unknown contract: nsd GetDeviceId (buffer {} bytes); 16 hex digits",
                ctx.GetWriteBufferSize());
    const std::string id = fmt::format("{:016X}", Harbor::Calibration::Get().DeviceId());
    ctx.WriteBuffer(id.data(), std::min(id.size(), ctx.GetWriteBufferSize()));

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetApplicationServerEnvironmentType(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(ServerEnvironmentType::Lp));
}

NSD::~NSD() = default;

} // namespace Service::Sockets
