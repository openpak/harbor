// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Self-check for erpt's MessagePack writer, context parsing, report body and report store. Eden's unit tests are off in the
// Harbor build, so this is a standalone program:
//
//   g++ -std=c++23 -I <src> erpt_store_selfcheck.cpp -o erpt_selfcheck && ./erpt_selfcheck <dir>
//
// <dir> is an empty scratch directory it may write to. It prints "erpt store self-check: OK" and
// exits 0, or names the failed check and exits 1.

#include <cstdio>
#include <cstdlib>

#include "core/hle/service/erpt/msgpack_writer.h"
#include "core/hle/service/erpt/report_builder.h"
#include "core/hle/service/erpt/report_store.h"

using namespace Service::ERPT;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond);                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

namespace {

std::vector<std::uint8_t> Bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int v : values) {
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::filesystem::path dir{argv[1]};

    // MessagePack: the smallest encodings (spec examples).
    {
        MsgPackWriter w;
        w.MapHeader(2);
        w.String("ErrorCode");
        w.String("2162-0002");
        w.String("OccurrenceTimestamp");
        w.UInt(0x1'0000'0000ULL);
        const auto& d = w.Data();
        CHECK(d[0] == 0x82);
        CHECK(d[1] == (0xa0 | 9));
        CHECK(d[11] == (0xa0 | 9));
        CHECK(d[21] == (0xa0 | 19));
        CHECK(d[41] == 0xcf && d[42] == 0 && d[45] == 1 && d.size() == 50);
    }
    {
        MsgPackWriter w;
        w.UInt(127);
        w.UInt(128);
        w.UInt(0x10000);
        w.Int(-1);
        w.Int(-33);
        w.Bool(true);
        w.Binary(Bytes({1, 2}));
        CHECK(w.Data() == Bytes({0x7f, 0xcc, 0x80, 0xce, 0, 1, 0, 0, 0xff, 0xd0, 0xdf, 0xc3, 0xc4,
                                 2, 1, 2}));
    }
    {
        MsgPackWriter w;
        w.String(std::string(40, 'x'));
        CHECK(w.Data()[0] == 0xd9 && w.Data()[1] == 40 && w.Data().size() == 42);
    }

    // Context parsing and the report body.
    {
        std::vector<std::uint8_t> entry(ContextEntrySize);
        std::vector<std::uint8_t> array;
        const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(&entry[at], &v, 4); };
        const std::string code = "2162-0002";
        array.insert(array.end(), code.begin(), code.end());
        put32(4, 2);                    // field count
        put32(8, Category_ErrorInfo);   // category
        put32(0x10, 10);                // ErrorCode
        entry[0x14] = FieldType_String;
        put32(0x18, 0);
        put32(0x1c, static_cast<std::uint32_t>(code.size()));
        put32(0x20, 116);               // AbortFlag
        entry[0x24] = FieldType_Bool;
        entry[0x28] = 1;
        Context ctx;
        CHECK(ParseContext(entry, array, ctx) == ErptError::None);
        CHECK(CheckReportContext(ctx) == ErptError::None);
        CHECK(std::get<std::string>(ctx.Get(10)->value) == code);
        CHECK(std::get<bool>(ctx.Get(116)->value));

        entry[0x24] = FieldType_U8; // wrong type for AbortFlag
        CHECK(ParseContext(entry, array, ctx) == ErptError::FieldTypeMismatch);
        entry[0x24] = FieldType_Bool;
        put32(8, Category_ApplicationInfo); // ErrorCode is not ApplicationInfo
        CHECK(ParseContext(entry, array, ctx) == ErptError::FieldCategoryMismatch);
        put32(8, Category_ErrorInfo);
        put32(0x1c, 40); // past the array buffer
        CHECK(ParseContext(entry, array, ctx) == ErptError::InvalidArgument);
        put32(0x1c, static_cast<std::uint32_t>(code.size()));
        CHECK(ParseContext(entry, array, ctx) == ErptError::None);

        Context settings;
        settings.category = Category_SystemSettingInfo;
        settings.Set(65, std::string{"XAW10000000000"}); // SerialNumber: private
        settings.Set(66, std::string{"23.0.1"});         // OsVersion
        const Context* contexts[] = {&ctx, &settings};
        const auto body = BuildReportBody(contexts);
        CHECK(body[0] == 0x83); // ErrorCode, AbortFlag, OsVersion
        const std::string text(body.begin(), body.end());
        CHECK(text.find("ErrorCode") != std::string::npos);
        CHECK(text.find("OsVersion") != std::string::npos);
        CHECK(text.find("SerialNumber") == std::string::npos);
        CHECK(text.find("XAW1") == std::string::npos);

        std::array<std::uint8_t, 16> uuid{};
        uuid[0] = 0xab;
        uuid[15] = 0x01;
        CHECK(ReportIdentifierString(uuid) == "ab000000-0000-0000-0000-000000000001");
    }

    // Store: persisted, newest first, at most 50, console eviction, filtered by type.
    std::filesystem::remove_all(dir);
    const auto info = [](std::uint8_t n, std::uint32_t type) {
        ReportInfo r{};
        r.id[0] = n;
        r.type = type;
        r.timestamp_user = 1000 + n;
        return r;
    };
    {
        ReportStore store{dir};
        store.Load();
        CHECK(store.List(ReportType_Any).empty());
        CHECK(store.Add(info(1, ReportType_Visible), Bytes({0x80})));
        CHECK(store.Add(info(2, ReportType_Invisible), Bytes({0x81, 0xa1, 'a', 1})));
        CHECK(store.Add(info(3, ReportType_Visible), Bytes({0x80})));
        CHECK(!store.Add(info(3, ReportType_Visible), Bytes({0x80}))); // the same id again
        const auto all = store.List(ReportType_Any);
        CHECK(all.size() == 3 && all[0].id[0] == 3 && all[2].id[0] == 1);
        CHECK(store.List(ReportType_Visible).size() == 2);
        CHECK(store.Find(info(2, 0).id)->size == 4);
        CHECK(std::filesystem::exists(dir / ReportStore::FileName(info(2, 0).id)));
    }
    {
        ReportStore store{dir};
        store.Load();
        CHECK(store.List(ReportType_Any).size() == 3);
        CHECK(*store.ReadBody(info(2, 0).id) == Bytes({0x81, 0xa1, 'a', 1}));
        CHECK(store.SetFlags(info(3, 0).id, 1));
        CHECK(store.Find(info(3, 0).id)->flags == 1);
        CHECK(store.Delete(info(2, 0).id));
        // Fill to 50 with Visible reports (2 already stored), then one more: the oldest Visible
        // one goes, as Visible has more stored.
        for (std::uint8_t n = 10; n < 58; ++n) {
            CHECK(store.Add(info(n, ReportType_Visible), Bytes({0x80})));
        }
        CHECK(store.List(ReportType_Any).size() == 50);
        CHECK(store.Add(info(60, ReportType_Invisible), Bytes({0x80})));
        CHECK(store.List(ReportType_Any).size() == 50);
        CHECK(!store.Find(info(1, 0).id));
        CHECK(!store.ReadBody(info(1, 0).id));
    }
    {
        ReportStore store{dir};
        store.Load();
        const auto all = store.List(ReportType_Any);
        CHECK(all.size() == 50 && all[0].id[0] == 60 && all[0].type == ReportType_Invisible);
        store.Clear();
        CHECK(store.List(ReportType_Any).empty());
    }
    std::filesystem::remove_all(dir);

    std::puts("erpt store self-check: OK");
    return 0;
}
