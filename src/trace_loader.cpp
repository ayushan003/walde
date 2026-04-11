// ─── trace_loader.cpp ───────────────────────────────────────
//
// Implementation of the streaming trace file reader.
// See trace_loader.h for full documentation.

#include "trace_loader.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace trace {

// ─── TraceReader ────────────────────────────────────────────

TraceReader::TraceReader(const std::string& path)
    : stream_(path)
{}

bool TraceReader::next(TraceOp& op) {
    std::string line;
    while (std::getline(stream_, line)) {
        ++lines_read_;

        // Skip empty lines and comments
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            ++lines_skipped_;
            continue;
        }

        // Auto-detect format from first data line
        if (!format_known_) {
            if (!detect_format(trimmed)) {
                ++lines_skipped_;
                continue;
            }
        }

        // Skip CSV header if we just detected CSV format
        if (format_ == TraceFormat::CSV && !csv_header_consumed_) {
            csv_header_consumed_ = true;
            // The header line was the detection line; skip it
            ++lines_skipped_;
            continue;
        }

        bool ok = false;
        switch (format_) {
            case TraceFormat::SIMPLE:
                ok = parse_simple(trimmed, op);
                break;
            case TraceFormat::CSV:
                ok = parse_csv(trimmed, op);
                break;
            default:
                break;
        }

        if (ok) {
            ++ops_parsed_;
            return true;
        }
        ++lines_skipped_;
    }
    return false;
}

// ─── Format detection ───────────────────────────────────────
//
// Heuristic: if the first data line contains commas and the
// tokens look like "timestamp,op,key" (3 comma-separated fields
// where the second is GET/PUT/READ/WRITE), it's CSV.
// Otherwise it's SIMPLE format.

bool TraceReader::detect_format(const std::string& line) {
    // Check for CSV: contains commas and has 3+ fields
    if (line.find(',') != std::string::npos) {
        // Try to parse as CSV header: "timestamp,op,key" or similar
        std::istringstream ss(line);
        std::string f1, f2, f3;
        if (std::getline(ss, f1, ',') &&
            std::getline(ss, f2, ',') &&
            std::getline(ss, f3, ',')) {
            // Normalize for comparison
            auto f2t = trim(f2);
            std::transform(f2t.begin(), f2t.end(), f2t.begin(), ::tolower);
            if (f2t == "op" || f2t == "operation" || f2t == "type" ||
                f2t == "get" || f2t == "put" || f2t == "read" || f2t == "write") {
                format_ = TraceFormat::CSV;
                format_known_ = true;
                // If the header contains "op"/"operation"/"type" literally,
                // this line is a header and will be skipped.
                // If it contains an actual op like "GET", treat this line
                // as data (no header).
                if (f2t == "op" || f2t == "operation" || f2t == "type") {
                    csv_header_consumed_ = false;  // will skip on next call
                } else {
                    csv_header_consumed_ = true;   // no header to skip
                }
                return true;
            }
        }
    }

    // Default: SIMPLE format (op key, or just key)
    format_ = TraceFormat::SIMPLE;
    format_known_ = true;
    return true;
}

// ─── Simple format parser ───────────────────────────────────
//
// Accepts:
//   "GET user:123"
//   "PUT user:456"
//   "READ user:789"    (mapped to GET)
//   "WRITE user:012"   (mapped to PUT)
//   "user:123"         (bare key, defaults to GET)
//   "12345 67890"      (multi-column numeric, first field = key)

bool TraceReader::parse_simple(const std::string& line, TraceOp& op) {
    // Split on whitespace
    auto space_pos = line.find_first_of(" \t");

    if (space_pos == std::string::npos) {
        // Single token — bare key, default to GET
        if (line.empty()) return false;
        op.type = OpType::GET;
        op.key  = line;
        return true;
    }

    std::string first  = line.substr(0, space_pos);
    std::string rest   = trim(line.substr(space_pos + 1));

    // Check if first token is an operation keyword
    auto op_upper = first;
    std::transform(op_upper.begin(), op_upper.end(), op_upper.begin(), ::toupper);

    if (op_upper == "GET" || op_upper == "READ") {
        if (rest.empty()) return false;
        op.type = OpType::GET;
        // Key is the next token (ignore anything after)
        auto sp = rest.find_first_of(" \t");
        op.key = (sp != std::string::npos) ? rest.substr(0, sp) : rest;
        return true;
    }

    if (op_upper == "PUT" || op_upper == "WRITE" ||
        op_upper == "SET" || op_upper == "UPDATE" || op_upper == "INSERT") {
        if (rest.empty()) return false;
        op.type = OpType::PUT;
        auto sp = rest.find_first_of(" \t");
        op.key = (sp != std::string::npos) ? rest.substr(0, sp) : rest;
        return true;
    }

    // Not a keyword — first token is the key (ARC/numeric format)
    op.type = OpType::GET;
    op.key  = first;
    return true;
}

// ─── CSV format parser ──────────────────────────────────────
//
// Expects: timestamp,op,key
// The timestamp field is ignored (used only for ordering).

bool TraceReader::parse_csv(const std::string& line, TraceOp& op) {
    std::istringstream ss(line);
    std::string f1, f2, f3;

    if (!std::getline(ss, f1, ',')) return false;
    if (!std::getline(ss, f2, ',')) return false;
    if (!std::getline(ss, f3, ',')) {
        // Try rest of line (no trailing comma)
        f3 = "";
        // Re-parse: maybe only 3 fields total with last not comma-terminated
        std::istringstream ss2(line);
        std::getline(ss2, f1, ',');
        std::getline(ss2, f2, ',');
        std::getline(ss2, f3);
    }

    auto key = trim(f3);
    if (key.empty()) return false;

    op.type = parse_op_type(trim(f2));
    op.key  = key;
    return true;
}

// ─── Helpers ────────────────────────────────────────────────

OpType TraceReader::parse_op_type(const std::string& token) {
    std::string upper = token;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    if (upper == "PUT" || upper == "WRITE" || upper == "SET" ||
        upper == "UPDATE" || upper == "INSERT") {
        return OpType::PUT;
    }
    // Default: GET (covers GET, READ, and unknown types)
    return OpType::GET;
}

std::string TraceReader::trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ─── Convenience functions ──────────────────────────────────

std::vector<TraceOp> load_trace_file(const std::string& path,
                                     size_t max_ops) {
    std::vector<TraceOp> ops;
    TraceReader reader(path);
    if (!reader.is_open()) return ops;

    TraceOp op;
    while (reader.next(op)) {
        ops.push_back(std::move(op));
        if (max_ops > 0 && ops.size() >= max_ops) break;
    }
    return ops;
}

uint64_t stream_trace_file(const std::string& path,
                           std::function<void(const TraceOp&)> fn,
                           size_t max_ops) {
    TraceReader reader(path);
    if (!reader.is_open()) return 0;

    uint64_t count = 0;
    TraceOp op;
    while (reader.next(op)) {
        fn(op);
        ++count;
        if (max_ops > 0 && count >= max_ops) break;
    }
    return count;
}

}  // namespace trace
