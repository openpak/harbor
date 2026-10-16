// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include <array>
#include <optional>
#include <vector>
#include <chrono>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>
#include "common/common_funcs.h"
#include "common/lz4_compression.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/nso.h"
#include "core/harbor_calibration/calibration.h"
#include "core/hle/kernel/k_page_table_base.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/set/system_settings_server.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include <algorithm>
#include <initializer_list>
#include <utility>
#include <filesystem>
#include <fstream>
#include <functional>
#include "core/hle/kernel/k_handle_table.h"
#include "core/openpak/platform.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/sm/sm.h"
#include "core/memory.h"
#include "core/openpak/guest_client.h"
#include "core/openpak/real_firmware.h"

namespace OpenPak::GuestClient {

namespace {

// One request on a session, from the dummy thread of the serving process. Returns the server's
// result; a move handle in the reply (an object) is handed back through out_handle.
Result Send(Core::System& system, Kernel::KProcess* process, Kernel::KClientSession* session,
            u32 command, std::span<const u64> args, const Buffers& buffers, u32* out_handle) {
    auto& kernel = system.Kernel();
    Kernel::KProcessAddress message{};
    if (process->CreateThreadLocalRegion(kernel, &message).IsError()) {
        LOG_WARNING(Service, "guest client: no message buffer");
        return ResultUnknown;
    }
    auto& memory = process->GetMemory();
    auto& page_table = process->GetPageTable();
    Kernel::KProcessAddress map_page{};
    const bool has_map = !buffers.map_in.empty();
    const std::size_t map_pages = (buffers.map_in.size() + Kernel::PageSize - 1) / Kernel::PageSize;
    if (has_map) {
        if (page_table.MapPages(&map_page, map_pages, Kernel::KMemoryState::Normal,
                                Kernel::KMemoryPermission::UserReadWrite).IsError()) {
            LOG_WARNING(Service, "guest client: no mapped buffer");
            process->DeleteThreadLocalRegion(kernel, message);
            return ResultUnknown;
        }
        memory.WriteBlock(map_page, buffers.map_in.data(), buffers.map_in.size());
    }
    const bool has_pointer = !buffers.pointer_in.empty();
    const u64 pointer_addr = GetInteger(message) + 0x100;
    const u32 pointer_size = static_cast<u32>(std::min<std::size_t>(buffers.pointer_in.size(), 0xF0));
    if (has_pointer) {
        memory.WriteBlock(pointer_addr, buffers.pointer_in.data(), pointer_size);
    }

    // HIPC: header, X descriptor, A descriptor, then the raw data (SFCI at the first 16-byte
    // boundary) with 16 bytes of alignment reserve counted in its size.
    std::array<u32, 0x40> words{};
    std::size_t w = 2;
    if (has_pointer) {
        const u64 a = pointer_addr;
        words[w++] = static_cast<u32>(((a >> 36) & 0x7) << 6) |
                     static_cast<u32>(((a >> 32) & 0xF) << 12) | (pointer_size << 16);
        words[w++] = static_cast<u32>(a & 0xFFFFFFFF);
    }
    if (has_map) {
        const u64 a = GetInteger(map_page);
        const u64 size = buffers.map_in.size();
        words[w++] = static_cast<u32>(size & 0xFFFFFFFF);
        words[w++] = static_cast<u32>(a & 0xFFFFFFFF);
        words[w++] = static_cast<u32>(((a >> 36) & 0x7) << 2) |
                     static_cast<u32>(((size >> 32) & 0xF) << 24) |
                     static_cast<u32>(((a >> 32) & 0xF) << 28);
    }
    const std::size_t raw_start = w;
    const std::size_t sfci = (w + 3) & ~std::size_t{3};
    words[0] = 4 | (has_pointer ? 1u << 16 : 0) | (has_map ? 1u << 20 : 0);
    words[sfci] = 0x49434653; // SFCI
    words[sfci + 1] = 1;
    words[sfci + 2] = command;
    words[sfci + 3] = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::memcpy(&words[sfci + 4 + 2 * i], &args[i], 8);
    }
    const std::size_t raw_end = sfci + 4 + 2 * args.size();
    words[1] = static_cast<u32>(raw_end - raw_start + 4);
    memory.WriteBlock(message, words.data(), 0x100);

    // The kernel's TLS path: an explicit address is treated as a locked user buffer.
    Kernel::GetCurrentThread(kernel).SetTlsAddressForHostClient(message);
    Result result = session->SendSyncRequest(kernel, 0, 0);
    Kernel::GetCurrentThread(kernel).SetTlsAddressForHostClient({});
    if (result.IsSuccess()) {
        memory.ReadBlock(message, words.data(), 0x100);
        result = ResultUnknown;
        bool found = false;
        for (std::size_t i = 0; i + 2 < words.size(); ++i) {
            if (words[i] == 0x4F434653) { // SFCO
                result = Result{words[i + 2]};
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_WARNING(Service, "guest client: reply to {} has no SFCO: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                        command, words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
        }
        // A handle descriptor: send-pid flag, copy count, move count, then the handles.
        if (out_handle != nullptr && (words[1] & 0x80000000u) != 0) {
            const u32 desc = words[2];
            std::size_t h = 3 + ((desc & 1) ? 2 : 0) + ((desc >> 1) & 0xF);
            *out_handle = ((desc >> 5) & 0xF) != 0 ? words[h] : 0;
        }
    }
    if (has_map) {
        page_table.UnmapPages(map_page, map_pages, Kernel::KMemoryState::Normal);
    }
    process->DeleteThreadLocalRegion(kernel, message);
    return result;
}

// Runs `work` on a dummy thread of the program hosting `service`, with a fresh session on it.
Result Transact(Core::System& system, const std::string& service,
                std::function<Result(Kernel::KProcess*, Kernel::KClientSession*)> work) {
    // Nothing to talk to once the emulation is shutting down (the kernel is going away).
    if (!system.IsPoweredOn()) {
        return ResultUnknown;
    }
    auto& kernel = system.Kernel();
    const auto program_id = RealFirmware::ProgramHosting(system, service);
    if (!program_id) {
        LOG_WARNING(Service, "guest client: no firmware program hosts {}", service);
        return ResultUnknown;
    }
    Kernel::KProcess* process = nullptr;
    for (auto& candidate : kernel.GetProcessList()) {
        if (candidate->GetProgramId() == *program_id) {
            process = candidate.GetPointerUnsafe();
            break;
        }
    }
    if (process == nullptr) {
        LOG_WARNING(Service, "guest client: program {:016x} ({}) is not running", *program_id, service);
        return ResultUnknown;
    }
    Result result = ResultUnknown;
    auto thread = kernel.RunOnHostThreadOfProcess(process, "OpenPak:GuestClient", [&] {
        Kernel::KClientPort* port{};
        if (system.ServiceManager().GetServicePort(&port, service).IsError()) {
            LOG_WARNING(Service, "guest client: {} is not registered", service);
            return;
        }
        Kernel::KClientSession* session{};
        if (port->CreateSession(kernel, &session).IsError()) {
            LOG_WARNING(Service, "guest client: no session to {}", service);
            return;
        }
        result = work(process, session);
        session->Close(kernel);
    });
    thread.join();
    return result;
}

} // namespace

Result Call(Core::System& system, const std::string& service, u32 command,
            std::span<const u64> args, const Buffers& buffers) {
    const Result result = Transact(system, service, [&](Kernel::KProcess* process, Kernel::KClientSession* session) {
        return Send(system, process, session, command, args, buffers, nullptr);
    });
    LOG_INFO(Service, "guest client: {} {} -> {:#x}", service, command, result.raw);
    return result;
}

namespace {

// The string the starter applet (0100000000001012) hands to news SetPassphrase for qlaunch at
// initial setup: News derives its content key from it. The starter's code builds the title id
// (movz #0x1000 / movk #0x100 lsl 48) next to an adrp/add x1 of the string in its read-only data;
// this finds that spot in the installed starter's main NSO.
// One firmware code pattern; a build that lays it out differently logs and sets nothing.
std::optional<std::string> StarterNewsPassphrase(Core::System& system) {
    const auto nca = system.GetContentProvider().GetEntry(0x0100000000001012ULL,
                                                          FileSys::ContentRecordType::Program);
    if (!nca) {
        return std::nullopt;
    }
    const auto exefs = nca->GetExeFS();
    const auto file = exefs ? exefs->GetFile("main") : nullptr;
    if (!file) {
        return std::nullopt;
    }
    const auto raw = file->ReadAllBytes();
    Loader::NSOHeader header{};
    if (raw.size() < sizeof(header)) {
        return std::nullopt;
    }
    std::memcpy(&header, raw.data(), sizeof(header));
    if (header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
        return std::nullopt;
    }
    const auto segment = [&](std::size_t i) -> std::vector<u8> {
        const auto& seg = header.segments[i];
        const std::size_t stored = header.IsSegmentCompressed(i) ? header.segments_compressed_size[i] : seg.size;
        if (seg.offset > raw.size() || stored > raw.size() - seg.offset) {
            return {};
        }
        const std::span<const u8> bytes{raw.data() + seg.offset, stored};
        if (header.IsSegmentCompressed(i)) {
            return Common::Compression::DecompressDataLZ4(bytes, seg.size);
        }
        return std::vector<u8>(bytes.begin(), bytes.end());
    };
    const auto text = segment(0);
    const auto rodata = segment(1);
    const u64 text_base = header.segments[0].location;
    const u64 ro_base = header.segments[1].location;
    const auto word = [&](std::size_t at) {
        u32 v{};
        std::memcpy(&v, text.data() + at, sizeof(v));
        return v;
    };
    for (std::size_t pc = 32; pc + 4 <= text.size(); pc += 4) {
        const u32 movk = word(pc);
        if ((movk & 0xFFFFFFE0) != 0xF2E02000) { // movk xN, #0x100, lsl #48
            continue;
        }
        const u32 reg = movk & 0x1F;
        bool movz = false;
        std::optional<u64> page;
        std::optional<u32> add;
        for (std::size_t k = 1; k <= 8; ++k) {
            const std::size_t at = pc - 4 * k;
            const u32 v = word(at);
            if (v == (0xD2820000 | reg)) { // movz xN, #0x1000
                movz = true;
            } else if ((v & 0x9F00001F) == 0x90000001) { // adrp x1, page
                s64 imm = static_cast<s64>((((v >> 5) & 0x7FFFF) << 2) | ((v >> 29) & 3));
                if (imm & (1 << 20)) {
                    imm -= (1 << 21);
                }
                page = ((text_base + at) & ~u64{0xFFF}) + (imm << 12);
            } else if ((v & 0xFFC003FF) == 0x91000021) { // add x1, x1, #imm
                add = (v >> 10) & 0xFFF;
            }
        }
        if (!movz || !page || !add) {
            continue;
        }
        const u64 addr = *page + *add;
        if (addr < ro_base || addr >= ro_base + rodata.size()) {
            continue;
        }
        const std::size_t start = addr - ro_base;
        std::size_t len = 0;
        while (start + len < rodata.size() && len < 0x7F && rodata[start + len] != 0) {
            ++len;
        }
        const std::string_view found{reinterpret_cast<const char*>(rodata.data() + start), len};
        if (len >= 16 && std::all_of(found.begin(), found.end(), [](char c) { return c > 0x20 && c < 0x7F; })) {
            return std::string{found};
        }
    }
    return std::nullopt;
}

} // namespace

void SetNewsPassphrase(Core::System& system) {
    using namespace std::chrono_literals;
    if (!RealFirmware::ProgramHosting(system, "news:a")) {
        return;
    }
    const auto passphrase = StarterNewsPassphrase(system);
    if (!passphrase) {
        LOG_WARNING(Service, "news: the installed starter holds no recognisable News passphrase; "
                             "the firmware's news module cannot decode OpenPak channels");
        return;
    }
    for (int i = 0; i < 300 && system.IsPoweredOn() && !system.ServiceManager().HasService("news:a"); ++i) {
        std::this_thread::sleep_for(200ms);
    }
    if (!system.IsPoweredOn()) {
        return;
    }
    const Result result = Transact(system, "news:a", [&](Kernel::KProcess* process, Kernel::KClientSession* creator) {
        u32 handle = 0;
        R_TRY(Send(system, process, creator, 0, {}, {}, &handle)); // CreateNewsService
        auto& kernel = system.Kernel();
        Kernel::KScopedAutoObject news =
            process->GetHandleTable().GetObject<Kernel::KClientSession>(kernel, handle);
        R_UNLESS(news.IsNotNull(), ResultUnknown);
        // As the starter: qlaunch's id and the string, terminator included.
        const std::string with_nul = *passphrase + '\0';
        const Result r = Send(system, process, news.GetPointerUnsafe(), 20100,
                              std::array<u64, 1>{0x0100000000001000ULL}, Buffers{with_nul, {}}, nullptr); // SetPassphrase
        process->GetHandleTable().Remove(kernel, handle);
        R_RETURN(r);
    });
    LOG_INFO(Service, "news: passphrase for qlaunch set from the starter ({} chars) -> {:#x}",
             passphrase->size(), result.raw);
}

void SubscribeDefaultNews(Core::System& system) {
    using namespace std::chrono_literals;
    if (!RealFirmware::ProgramHosting(system, "news:m")) {
        LOG_INFO(Service, "news: the firmware news program is not on the list; no default follows");
        return;
    }
    // Once per install, as the OpenPak NRO did on the console: afterwards the follows are the
    // person's own choice and the module keeps them.
    const auto marker = Platform::DataDir() / "news-default-subscribed";
    if (std::filesystem::exists(marker)) {
        LOG_INFO(Service, "news: default follows were done before ({})", marker.string());
        return;
    }
    LOG_INFO(Service, "news: following the default channels once (marker {})", marker.string());
    for (int i = 0; i < 300 && system.IsPoweredOn() && !system.ServiceManager().HasService("news:m"); ++i) {
        std::this_thread::sleep_for(200ms);
    }
    if (!system.IsPoweredOn()) {
        return;
    }
    const Result result = Transact(system, "news:m", [&](Kernel::KProcess* process, Kernel::KClientSession* creator) {
        u32 handle = 0;
        const Result created = Send(system, process, creator, 0, {}, {}, &handle); // CreateNewsService
        LOG_INFO(Service, "news: CreateNewsService -> {:#x}, object handle {:#x}", created.raw, handle);
        R_TRY(created);
        auto& kernel = system.Kernel();
        Kernel::KScopedAutoObject news =
            process->GetHandleTable().GetObject<Kernel::KClientSession>(kernel, handle);
        R_UNLESS(news.IsNotNull(), ResultUnknown);
        for (const char* topic : {"openpak_news", "openpak_games"}) {
            // The module wants the terminator inside the buffer (its last byte must be NUL).
            const std::string_view with_nul{topic, std::strlen(topic) + 1};
            const Result r = Send(system, process, news.GetPointerUnsafe(), 40100,
                                  std::array<u64, 1>{2}, Buffers{with_nul, {}}, nullptr); // SetSubscriptionStatus
            LOG_INFO(Service, "news: follow {} -> {:#x}", topic, r.raw);
            R_TRY(r);
            // The module's download task already ran this boot: fetch the new follow now.
            const Result fetch = Send(system, process, news.GetPointerUnsafe(), 30300, {},
                                      Buffers{with_nul, {}}, nullptr); // RequestImmediateReception
            LOG_INFO(Service, "news: fetch {} -> {:#x}", topic, fetch.raw);
        }
        process->GetHandleTable().Remove(kernel, handle);
        R_SUCCEED();
    });
    if (result.IsSuccess()) {
        std::ofstream{marker} << "1\n";
    }
}

namespace {

std::string Rgb(u32 value) {
    // The first three bytes of the settings word, as six lowercase hex digits.
    return fmt::format("{:02x}{:02x}{:02x}", value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
}

std::string Rgb(const Core::HID::NpadColor& color) {
    return fmt::format("{:02x}{:02x}{:02x}", color.r, color.g, color.b);
}

std::string CString(const auto& chars) {
    return std::string{chars.data(), strnlen(chars.data(), chars.size())};
}

} // namespace

void PublishDigitalTwin(Core::System& system) {
    using namespace std::chrono_literals;
    if (!RealFirmware::ProgramHosting(system, "npns:s")) {
        return;
    }
    for (int i = 0; i < 300 && system.IsPoweredOn() && !system.ServiceManager().HasService("npns:s"); ++i) {
        std::this_thread::sleep_for(200ms);
    }
    if (!system.IsPoweredOn()) {
        return;
    }

    // c-appearance.main: the device as ns describes it.
    Service::Set::FirmwareVersionFormat firmware{};
    Service::Set::GetFirmwareVersionImpl(firmware, system, Service::Set::GetFirmwareVersionType::Version2);
    const u32 comparable = (u32{firmware.major} << 16) | (u32{firmware.minor} << 8) | firmware.micro;
    const Service::Set::HomeMenuScheme scheme{
        .main = 0xFF323232, .back = 0xFF323232, .sub = 0xFFFFFFFF, .bezel = 0xFFFFFFFF, .extra = 0xFF000000};
    nlohmann::json main{
        {"DeviceNickName", Settings::values.device_name.GetValue()},
        {"FirmwareVersion", CString(firmware.display_version)},
        {"FirmwareComparableVersion", comparable},
        {"ProductModel", "Nx"},
        {"SerialNumber", Harbor::Calibration::Get().SerialNumber()},
        {"HomeMenuScheme", {{"Main", Rgb(scheme.main)}, {"Back", Rgb(scheme.back)},
                            {"Sub", Rgb(scheme.sub)}, {"Bezel", Rgb(scheme.bezel)}}},
    };

    // c-appearance.controller: the two Joy-Con halves as Harbor emulates them.
    const auto* controller = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    const auto colors = controller != nullptr ? controller->GetColors() : Core::HID::ControllerColors{};
    const auto half = [](const char* position, const Core::HID::NpadControllerColor& color) {
        return nlohmann::json{{"Position", position}, {"DeviceType", 1},
                              {"ControllerColor", {{"Main", Rgb(color.body)}, {"Sub", Rgb(color.button)}}}};
    };
    nlohmann::json controllers{{"Controllers", {half("Left", colors.left), half("Right", colors.right)}}};

    // c-storage.summary: the NAND user partition and the SD card.
    auto& fsc = system.GetFileSystemController();
    nlohmann::json storages{{"Storages", nlohmann::json::array()}};
    storages["Storages"].push_back({{"StorageType", "Internal"},
                                    {"Total", fsc.GetTotalSpaceSize(FileSys::StorageId::NandUser)},
                                    {"Free", fsc.GetFreeSpaceSize(FileSys::StorageId::NandUser)}});
    storages["Storages"].push_back({{"StorageType", "SD"},
                                    {"Total", fsc.GetTotalSpaceSize(FileSys::StorageId::SdCard)},
                                    {"Free", fsc.GetFreeSpaceSize(FileSys::StorageId::SdCard)}});

    for (const auto& [name, document] : std::initializer_list<std::pair<const char*, const nlohmann::json*>>{
             {"c-appearance.main", &main}, {"c-appearance.controller", &controllers},
             {"c-storage.summary", &storages}}) {
        const std::string body = document->dump();
        const Result result = Call(system, "npns:s", 50, {}, Buffers{name, std::span<const u8>{reinterpret_cast<const u8*>(body.data()), body.size()}});
        LOG_INFO(Service, "digital twin: {} ({} bytes) -> {:#x}", name, body.size(), result.raw);
    }
}

void NotifyOperationMode(Core::System& system) {
    using namespace std::chrono_literals;
    if (!RealFirmware::ProgramHosting(system, "prepo:m")) {
        return;
    }
    for (int i = 0; i < 300 && system.IsPoweredOn() && !system.ServiceManager().HasService("prepo:m"); ++i) {
        std::this_thread::sleep_for(200ms);
    }
    if (!system.IsPoweredOn()) {
        return;
    }
    const u64 mode = Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked ? 1 : 0;
    Call(system, "prepo:m", 20200, std::array<u64, 1>{mode});
}

} // namespace OpenPak::GuestClient
