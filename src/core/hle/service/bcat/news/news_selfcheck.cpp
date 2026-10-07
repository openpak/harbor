// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// A standalone self-check for the news query grammar, the record layout and the BCAT delivery
// grouping. Eden's test target is off in the Harbor build, so this is built by hand and prints
// "news selfcheck: N checks, 0 failures" when it passes:
//
//   g++ -std=c++20 -I src
//       src/core/hle/service/bcat/news/news_selfcheck.cpp
//       src/core/hle/service/bcat/news/news_record.cpp
//       src/core/hle/service/bcat/news/news_query.cpp
//       src/core/hle/service/bcat/news/msgpack.cpp
//       src/core/hle/service/bcat/backend/openpak_bcat.cpp -o <out> && <out>
//
// With a directory argument it also writes two sample articles and the index the news fill reads
// (<dir>/openpak/v1/local/...), for a desktop run with the stub directory <user dir>/openpak/stub (or HARBOR_OPENPAK_STUB_DIR) pointing at <dir>.

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/hle/service/bcat/backend/openpak_bcat.h"
#include "core/hle/service/bcat/news/msgpack.h"
#include "core/hle/service/bcat/news/news_query.h"
#include "core/hle/service/bcat/news/news_record.h"

using namespace Service::News;
using namespace Service::BCAT;

namespace {

int checks = 0;
int failures = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

NewsRow Row(const char* id, const char* topic, s64 published, s64 read, s64 newly, s64 priority = 0) {
    NewsRow row;
    row.news_id = id;
    row.topic_id = topic;
    row.application_ids = "/0100000000010000/";
    row.Int(IntColumn::PublishedAt) = published;
    row.Int(IntColumn::Read) = read;
    row.Int(IntColumn::Newly) = newly;
    row.Int(IntColumn::Priority) = priority;
    return row;
}

std::vector<std::string> Ids(const std::vector<NewsRow>& rows) {
    std::vector<std::string> ids;
    for (const auto& row : rows) {
        ids.push_back(row.news_id);
    }
    return ids;
}

bool Throws(const char* where) {
    try {
        ParseWhere(where);
    } catch (const QueryError&) {
        return true;
    }
    return false;
}

std::vector<u8> Article(u64 id, const char* topic, const char* title, const char* body, u64 published) {
    MsgPack::Writer w;
    w.WriteFixMap(9);
    w.WriteKey("version");
    w.WriteFixMap(2);
    w.WriteKey("format");
    w.WriteUInt(1);
    w.WriteKey("semantics");
    w.WriteUInt(1);
    w.WriteKey("news_id");
    w.WriteUInt(id);
    w.WriteKey("published_at");
    w.WriteUInt(published);
    w.WriteKey("pickup_limit");
    w.WriteUInt(published + 86400 * 30);
    w.WriteKey("priority");
    w.WriteUInt(5);
    w.WriteKey("deletion_priority");
    w.WriteUInt(100);
    w.WriteKey("topic_id");
    w.WriteString(topic);
    w.WriteKey("subject");
    w.WriteFixMap(2);
    w.WriteKey("caption");
    w.WriteUInt(1);
    w.WriteKey("text");
    w.WriteString(title);
    w.WriteKey("body");
    w.WriteFixMap(1);
    w.WriteKey("text");
    w.WriteString(body);
    return w.Take();
}

void WriteFile(const std::filesystem::path& path, const std::vector<u8>& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                static_cast<std::streamsize>(data.size()));
}

} // namespace

int main(int argc, char** argv) {
    // The record layout: sizes and the offsets qlaunch reads.
    Check(sizeof(NewsRecord) == 0x80, "NewsRecord is 0x80 bytes");
    Check(offsetof(NewsRecord, decoration_type) == 0x60, "decoration_type at 0x60");
    Check(offsetof(NewsRecord, read) == 0x64, "read at 0x64");
    Check(offsetof(NewsRecord, newly) == 0x68, "newly at 0x68");
    Check(offsetof(NewsRecord, topic_id) == 0x30, "topic_id at 0x30");
    Check(sizeof(NewsRecordV1) == 0x48, "NewsRecordV1 is 0x48 bytes");

    NewsRow sample = Row("LS00000000000000000001", "nx_notice", 100, 1, 0);
    sample.Int(IntColumn::DecorationType) = 3;
    sample.Int(IntColumn::ReceivedAt) = 1234;
    sample.Int(IntColumn::Displayed) = 7;
    NewsRecord wire;
    sample.ToWire(wire);
    u8 raw[0x80];
    std::memcpy(raw, &wire, sizeof(raw));
    Check(std::memcmp(raw, "LS00000000000000000001", 22) == 0 && raw[22] == 0, "news_id text at 0");
    Check(std::memcmp(raw + 0x30, "nx_notice", 9) == 0, "topic text at 0x30");
    Check(raw[0x50] == 0xD2 && raw[0x51] == 0x04, "received_at at 0x50");
    Check(raw[0x60] == 3, "decoration_type value at 0x60");
    Check(raw[0x64] == 1, "read value at 0x64");
    Check(raw[0x68] == 0, "newly value at 0x68");
    Check(raw[0x6C] == 7, "displayed value at 0x6C");

    // Posting: ids, defaults, rejects.
    const auto article = Article(1909260910ULL, "nx_notice", "Hello", "World", 1700000000);
    const auto row = NewsRow::FromPayload(article, 42);
    Check(row.has_value(), "an article parses");
    if (row) {
        Check(row->news_id == "LS00000000001909260910", "local id is LS plus 20 digits");
        Check(row->user_id.empty(), "no user");
        Check(row->topic_id == "nx_notice", "topic");
        Check(row->Int(IntColumn::PublishedAt) == 1700000000, "published_at");
        Check(row->Int(IntColumn::DecorationType) == 1, "decoration_type defaults to 1");
        Check(row->Int(IntColumn::Newly) == 1 && row->Int(IntColumn::OptedIn) == 1, "newly, opted_in");
        Check(row->Int(IntColumn::ReceivedAt) == 42, "received_at");
        Check(row->FileName() == "D00000000000000000000_LS00000000001909260910.msgpack", "file name");
    }
    const std::vector<u8> junk{1, 2, 3};
    Check(!NewsRow::FromPayload(junk, 0).has_value(), "junk is not an article");
    MsgPack::Writer no_id;
    no_id.WriteFixMap(1);
    no_id.WriteKey("topic_id");
    no_id.WriteString("x");
    Check(!NewsRow::FromPayload(no_id.Take(), 0).has_value(), "no news_id is not an article");
    MsgPack::Writer text_id;
    text_id.WriteFixMap(1);
    text_id.WriteKey("news_id");
    text_id.WriteString("7");
    Check(!NewsRow::FromPayload(text_id.Take(), 0).has_value(), "a text news_id is not an article");

    // WHERE.
    const std::vector<NewsRow> rows{
        Row("A", "t1", 30, 0, 1, 1),
        Row("B", "t2", 10, 1, 0, 2),
        Row("C", "t1", 20, 0, 0, 3),
    };
    const auto where = [&](const char* text) {
        const auto match = ParseWhere(text);
        std::vector<NewsRow> out;
        for (const auto& item : rows) {
            if (match(item)) {
                out.push_back(item);
            }
        }
        return Ids(out);
    };
    Check(where("") == std::vector<std::string>{"A", "B", "C"}, "empty WHERE matches all");
    Check(where("topic_id='t1'") == std::vector<std::string>{"A", "C"}, "string equality");
    Check(where("read=0 AND newly=1") == std::vector<std::string>{"A"}, "AND");
    Check(where("read=1 OR newly=1") == std::vector<std::string>{"A", "B"}, "OR");
    Check(where("(read=0 AND newly=0) OR priority>=2") == std::vector<std::string>{"B", "C"}, "parentheses");
    Check(where("published_at<>20") == std::vector<std::string>{"A", "B"}, "<>");
    Check(where("priority*2>=4") == std::vector<std::string>{"B", "C"}, "multiplication");
    Check(where("N_SWITCH(news_id,'B',1,0)=1") == std::vector<std::string>{"B"}, "N_SWITCH picks by key");
    Check(where("N_IF(read=1,0,1)=1") == std::vector<std::string>{"A", "C"}, "N_IF");
    Check(where("N_PMATCH(application_ids,'/0100000000010000/')") == std::vector<std::string>{"A", "B", "C"},
          "N_PMATCH substring");
    Check(where("N_PMATCH(application_ids,'/0100000000099999/')").empty(), "N_PMATCH miss");
    Check(where("topic_id='it''s'").empty(), "a doubled quote is a quote");
    Check(Throws("bogus_column=1"), "an unknown column is rejected");
    Check(Throws("read=CAST(1 AS TEXT)"), "an unknown function is rejected");
    Check(Throws("read=1 AND"), "a dangling AND is rejected");
    Check(Throws("topic_id='open"), "an unterminated string is rejected");

    // ORDER BY.
    const auto order = [&](const char* text) { return Ids(OrderRows(rows, text)); };
    Check(order("") == std::vector<std::string>{"A", "B", "C"}, "empty ORDER BY keeps the order");
    Check(order("published_at") == std::vector<std::string>{"B", "C", "A"}, "ASC by default");
    Check(order("published_at DESC") == std::vector<std::string>{"A", "C", "B"}, "DESC");
    Check(order("topic_id, published_at DESC") == std::vector<std::string>{"A", "C", "B"}, "two keys");
    Check(order("N_IF(read=1,0,1) DESC, priority DESC") == std::vector<std::string>{"C", "A", "B"},
          "an expression key");
    Check(order("read ASC, newly DESC") == std::vector<std::string>{"A", "C", "B"}, "ASC then DESC");

    // BCAT manifest helpers.
    std::vector<OpenPak::Bcat::File> files;
    for (int i = 0; i < 120; ++i) {
        files.push_back({"d", "f" + std::to_string(i), {}});
    }
    files.push_back({"D", "F0", {9}});
    const auto groups = GroupBcatFiles(files);
    Check(groups.size() == 1 && groups[0].files.size() == MaxBcatEntries, "duplicates and overflow are dropped");

    if (argc > 1) {
        const std::filesystem::path root = std::filesystem::path(argv[1]) / "openpak" / "v1" / "local";
        const auto first = Article(1000000001ULL, "harbor_sample", "Harbor sample one",
                                   "The first sample article, served from a file.", 1790000000);
        const auto second = Article(1000000002ULL, "harbor_sample", "Harbor sample two",
                                     "The second sample article, served from a file.", 1790086400);
        WriteFile(root / "harbor_sample" / "news" / "1000000001", first);
        WriteFile(root / "harbor_sample" / "news" / "1000000002", second);
        const std::string index = "harbor_sample 1000000001 " + std::to_string(first.size()) +
                                  "\nharbor_sample 1000000002 " + std::to_string(second.size()) + "\n";
        WriteFile(root / "index", std::vector<u8>(index.begin(), index.end()));
    }

    std::printf("news selfcheck: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
