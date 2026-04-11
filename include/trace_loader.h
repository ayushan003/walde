#pragma once

// ─── Trace Loader ───────────────────────────────────────────
//
// Streaming trace file reader for real-world benchmarking.
//
// Design goals:
//   - Stream line-by-line (never loads entire file into memory)
//   - Supports two formats:
//       Format A (simple):  "GET user:123" or just "user:123"
//       Format B (CSV):     "timestamp,op,key" header + data rows
//   - Auto-detects format from first non-comment line
//   - Robust parsing with graceful skip of malformed lines
//   - Designed for traces with millions of operations
//
// This is a pure addition — it does NOT modify or replace the
// existing bench::load_trace() function in bench_workloads.h.

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace trace {

// ─── Parsed trace operation ─────────────────────────────────

enum class OpType : uint8_t {
    GET   = 0,
    PUT   = 1,
};

struct TraceOp {
    OpType      type = OpType::GET;
    std::string key;
};

// ─── Format auto-detection ──────────────────────────────────

enum class TraceFormat : uint8_t {
    UNKNOWN    = 0,
    SIMPLE     = 1,   // "GET key" or "PUT key" or just "key"
    CSV        = 2,   // "timestamp,op,key" (header + data)
};

// ─── Streaming trace reader ─────────────────────────────────
//
// Usage:
//   trace::TraceReader reader("path/to/trace.txt");
//   if (!reader.is_open()) { /* error */ }
//
//   // Streaming (low memory):
//   TraceOp op;
//   while (reader.next(op)) {
//       // use op.type and op.key
//   }
//
//   // Or batch load (convenience):
//   auto ops = trace::load_trace_file("path.txt", max_ops);

class TraceReader {
public:
    explicit TraceReader(const std::string& path);

    bool is_open() const { return stream_.is_open(); }

    // Read next operation. Returns false at EOF or on stream error.
    bool next(TraceOp& op);

    // Statistics
    uint64_t    lines_read()    const { return lines_read_; }
    uint64_t    lines_skipped() const { return lines_skipped_; }
    uint64_t    ops_parsed()    const { return ops_parsed_; }
    TraceFormat detected_format() const { return format_; }

private:
    bool detect_format(const std::string& line);
    bool parse_simple(const std::string& line, TraceOp& op);
    bool parse_csv(const std::string& line, TraceOp& op);

    static OpType parse_op_type(const std::string& token);
    static std::string trim(const std::string& s);

    std::ifstream stream_;
    TraceFormat   format_        = TraceFormat::UNKNOWN;
    bool          format_known_  = false;
    bool          csv_header_consumed_ = false;
    uint64_t      lines_read_    = 0;
    uint64_t      lines_skipped_ = 0;
    uint64_t      ops_parsed_    = 0;
};

// ─── Convenience batch loader ───────────────────────────────
//
// Loads up to max_ops operations from a trace file into a vector.
// If max_ops == 0, loads all operations.
// Returns empty vector on file open failure.

std::vector<TraceOp> load_trace_file(const std::string& path,
                                     size_t max_ops = 0);

// ─── Streaming callback interface ───────────────────────────
//
// Calls fn(op) for each parsed operation, up to max_ops.
// Returns the number of operations processed.

uint64_t stream_trace_file(const std::string& path,
                           std::function<void(const TraceOp&)> fn,
                           size_t max_ops = 0);

}  // namespace trace
