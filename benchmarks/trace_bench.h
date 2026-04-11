#pragma once

// ─── Trace Benchmark Bridge ────────────────────────────────
//
// Converts trace::TraceOp (from trace_loader) into bench::BenchOp
// so that trace-driven workloads can be run through the existing
// benchmark pipeline (run_lru, run_wtinylfu, run_walde).
//
// This is a pure additive adapter — it does NOT modify
// bench_workloads.h or any existing benchmark logic.

#include "trace_loader.h"
#include "bench_workloads.h"

#include <string>
#include <vector>
#include <cstdio>

namespace trace {

// ─── Convert trace ops to bench ops ─────────────────────────
//
// Maps trace::OpType → bench::BenchOp::Type and generates
// dummy values for PUT operations.

inline std::vector<bench::BenchOp> to_bench_ops(
        const std::vector<trace::TraceOp>& trace_ops) {
    std::vector<bench::BenchOp> result;
    result.reserve(trace_ops.size());

    uint64_t seq = 0;
    for (const auto& top : trace_ops) {
        bench::BenchOp bop;
        bop.key = top.key;

        switch (top.type) {
            case trace::OpType::PUT:
                bop.type  = bench::BenchOp::PUT;
                bop.value = "trace_val_" + std::to_string(seq);
                break;
            case trace::OpType::GET:
            default:
                bop.type = bench::BenchOp::GET;
                break;
        }
        result.push_back(std::move(bop));
        ++seq;
    }
    return result;
}

// ─── Load and convert in one step ───────────────────────────

inline std::vector<bench::BenchOp> load_trace_as_bench_ops(
        const std::string& path, size_t max_ops = 0) {
    auto trace_ops = trace::load_trace_file(path, max_ops);
    return to_bench_ops(trace_ops);
}

// ─── Print trace load summary ───────────────────────────────

inline void print_trace_summary(const std::string& path,
                                const TraceReader& reader,
                                size_t ops_loaded,
                                size_t ops_capped = 0) {
    const char* fmt_name = "unknown";
    switch (reader.detected_format()) {
        case TraceFormat::SIMPLE: fmt_name = "simple (key-per-line)"; break;
        case TraceFormat::CSV:    fmt_name = "CSV"; break;
        default: break;
    }

    std::printf("  Trace file    : %s\n", path.c_str());
    std::printf("  Format        : %s\n", fmt_name);
    std::printf("  Lines read    : %lu\n",
                static_cast<unsigned long>(reader.lines_read()));
    std::printf("  Lines skipped : %lu\n",
                static_cast<unsigned long>(reader.lines_skipped()));
    std::printf("  Ops loaded    : %zu", ops_loaded);
    if (ops_capped > 0 && ops_capped != ops_loaded)
        std::printf(" (capped from %zu)", ops_capped);
    std::printf("\n");
}

}  // namespace trace
