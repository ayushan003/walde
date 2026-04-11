// ─── test_trace_loader.cpp ──────────────────────────────────
//
// Unit tests for the trace_loader module.
// Tests format detection, parsing, streaming, and edge cases.

#include "trace_loader.h"
#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace {

// Helper: write a temporary trace file
std::string write_temp_file(const std::string& name,
                            const std::string& content) {
    std::string path = "/tmp/test_trace_" + name + ".txt";
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

// ─── Simple format tests ────────────────────────────────────

TEST(TraceLoader, SimpleFormatBareKeys) {
    auto path = write_temp_file("bare", "user:123\nuser:456\nuser:789\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0].key, "user:123");
    EXPECT_EQ(ops[0].type, trace::OpType::GET);
    EXPECT_EQ(ops[1].key, "user:456");
    EXPECT_EQ(ops[2].key, "user:789");
}

TEST(TraceLoader, SimpleFormatWithOps) {
    auto path = write_temp_file("ops",
        "GET user:123\n"
        "PUT user:456\n"
        "READ user:789\n"
        "WRITE user:012\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0].type, trace::OpType::GET);
    EXPECT_EQ(ops[0].key, "user:123");
    EXPECT_EQ(ops[1].type, trace::OpType::PUT);
    EXPECT_EQ(ops[1].key, "user:456");
    EXPECT_EQ(ops[2].type, trace::OpType::GET);   // READ → GET
    EXPECT_EQ(ops[2].key, "user:789");
    EXPECT_EQ(ops[3].type, trace::OpType::PUT);    // WRITE → PUT
    EXPECT_EQ(ops[3].key, "user:012");
}

TEST(TraceLoader, SimpleFormatCaseInsensitive) {
    auto path = write_temp_file("case",
        "get key1\n"
        "Get key2\n"
        "GET key3\n"
        "put key4\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0].type, trace::OpType::GET);
    EXPECT_EQ(ops[1].type, trace::OpType::GET);
    EXPECT_EQ(ops[2].type, trace::OpType::GET);
    EXPECT_EQ(ops[3].type, trace::OpType::PUT);
}

TEST(TraceLoader, SimpleFormatMultiColumn) {
    // ARC-style: "address size ..."
    auto path = write_temp_file("arc", "12345 678\n99999 100\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].key, "12345");
    EXPECT_EQ(ops[0].type, trace::OpType::GET);
    EXPECT_EQ(ops[1].key, "99999");
}

// ─── CSV format tests ───────────────────────────────────────

TEST(TraceLoader, CSVFormatWithHeader) {
    auto path = write_temp_file("csv",
        "timestamp,op,key\n"
        "1000,GET,user:123\n"
        "1001,PUT,user:456\n"
        "1002,GET,user:789\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0].type, trace::OpType::GET);
    EXPECT_EQ(ops[0].key, "user:123");
    EXPECT_EQ(ops[1].type, trace::OpType::PUT);
    EXPECT_EQ(ops[1].key, "user:456");
    EXPECT_EQ(ops[2].type, trace::OpType::GET);
    EXPECT_EQ(ops[2].key, "user:789");
}

TEST(TraceLoader, CSVFormatDetectsFormat) {
    auto path = write_temp_file("csv_detect",
        "timestamp,op,key\n"
        "1,GET,a\n");
    trace::TraceReader reader(path);
    ASSERT_TRUE(reader.is_open());
    trace::TraceOp op;
    ASSERT_TRUE(reader.next(op));
    EXPECT_EQ(reader.detected_format(), trace::TraceFormat::CSV);
}

// ─── Edge cases ─────────────────────────────────────────────

TEST(TraceLoader, SkipsCommentsAndBlanks) {
    auto path = write_temp_file("comments",
        "# This is a comment\n"
        "\n"
        "   \n"
        "key1\n"
        "# Another comment\n"
        "key2\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].key, "key1");
    EXPECT_EQ(ops[1].key, "key2");
}

TEST(TraceLoader, HandlesWhitespace) {
    auto path = write_temp_file("whitespace",
        "  GET   user:123  \n"
        "\t  key_with_tabs  \t\n");
    auto ops = trace::load_trace_file(path);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].key, "user:123");
    EXPECT_EQ(ops[1].key, "key_with_tabs");
}

TEST(TraceLoader, EmptyFile) {
    auto path = write_temp_file("empty", "");
    auto ops = trace::load_trace_file(path);
    EXPECT_TRUE(ops.empty());
}

TEST(TraceLoader, NonexistentFile) {
    auto ops = trace::load_trace_file("/tmp/nonexistent_trace_file_xyz.txt");
    EXPECT_TRUE(ops.empty());
}

TEST(TraceLoader, MaxOpsLimit) {
    auto path = write_temp_file("limit",
        "k1\nk2\nk3\nk4\nk5\nk6\nk7\nk8\nk9\nk10\n");
    auto ops = trace::load_trace_file(path, 3);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0].key, "k1");
    EXPECT_EQ(ops[2].key, "k3");
}

// ─── Streaming interface ────────────────────────────────────

TEST(TraceLoader, StreamingReader) {
    auto path = write_temp_file("stream",
        "GET a\nPUT b\nGET c\n");
    trace::TraceReader reader(path);
    ASSERT_TRUE(reader.is_open());

    trace::TraceOp op;
    ASSERT_TRUE(reader.next(op));
    EXPECT_EQ(op.key, "a");
    EXPECT_EQ(op.type, trace::OpType::GET);

    ASSERT_TRUE(reader.next(op));
    EXPECT_EQ(op.key, "b");
    EXPECT_EQ(op.type, trace::OpType::PUT);

    ASSERT_TRUE(reader.next(op));
    EXPECT_EQ(op.key, "c");

    ASSERT_FALSE(reader.next(op));  // EOF

    EXPECT_EQ(reader.ops_parsed(), 3u);
    EXPECT_EQ(reader.lines_skipped(), 0u);
}

TEST(TraceLoader, StreamCallback) {
    auto path = write_temp_file("callback",
        "k1\nk2\nk3\nk4\nk5\n");
    std::vector<std::string> keys;
    auto count = trace::stream_trace_file(path,
        [&](const trace::TraceOp& op) { keys.push_back(op.key); }, 3);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], "k1");
    EXPECT_EQ(keys[2], "k3");
}

// ─── Statistics tracking ────────────────────────────────────

TEST(TraceLoader, ReaderStats) {
    auto path = write_temp_file("stats",
        "# comment\n"
        "\n"
        "key1\n"
        "key2\n"
        "# another comment\n"
        "key3\n");
    trace::TraceReader reader(path);
    trace::TraceOp op;
    while (reader.next(op)) {}

    EXPECT_EQ(reader.lines_read(), 6u);
    EXPECT_EQ(reader.ops_parsed(), 3u);
    EXPECT_EQ(reader.lines_skipped(), 3u);  // 1 comment + 1 blank + 1 comment
}

}  // namespace
