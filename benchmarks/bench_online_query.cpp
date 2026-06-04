#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/query/search_results.h"
#include "vdb/storage/cluster_store.h"

namespace fs = std::filesystem;

using vdb::ClusterID;
using vdb::DType;
using vdb::Dim;
using vdb::Status;
using vdb::StatusOr;
using vdb::index::IvfIndex;
using vdb::query::DynamicSafeInMode;
using vdb::query::IoUringReader;
using vdb::query::OverlapScheduler;
using vdb::query::SearchConfig;
using vdb::query::SearchResults;
using vdb::query::SubmissionMode;

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static bool HasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

static std::string GetStringArg(int argc, char** argv, const char* key,
                                const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return def;
}

static int GetIntArg(int argc, char** argv, const char* key, int def = 0) {
    std::string v = GetStringArg(argc, argv, key, "");
    if (v.empty()) return def;
    return std::atoi(v.c_str());
}

static double GetDoubleArg(int argc, char** argv, const char* key,
                           double def = 0.0) {
    std::string v = GetStringArg(argc, argv, key, "");
    if (v.empty()) return def;
    return std::atof(v.c_str());
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

static std::string JStr(const std::string& k, const std::string& v) {
    return "\"" + k + "\": \"" + JsonEscape(v) + "\"";
}

static std::string JNum(const std::string& k, double v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return "\"" + k + "\": " + buf;
}

static std::string JInt(const std::string& k, int64_t v) {
    return "\"" + k + "\": " + std::to_string(v);
}

static std::string JBool(const std::string& k, bool v) {
    return "\"" + k + "\": " + (v ? "true" : "false");
}

struct ProcRss {
    int64_t rss_kib = 0;
    int64_t hwm_kib = 0;
};

static ProcRss ReadProcRss() {
    std::ifstream f("/proc/self/status");
    ProcRss out;
    std::string key;
    while (f >> key) {
        if (key == "VmRSS:") {
            f >> out.rss_kib;
        } else if (key == "VmHWM:") {
            f >> out.hwm_kib;
        }
        std::string rest;
        std::getline(f, rest);
    }
    return out;
}

struct NpyHeader {
    std::string descr;
    bool fortran_order = false;
    std::vector<uint64_t> shape;
    uint64_t data_offset = 0;
};

static uint16_t ReadLe16(const char* p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint16_t>(static_cast<unsigned char>(p[1])) << 8);
}

static uint32_t ReadLe32(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

static std::string ExtractQuotedValue(const std::string& h,
                                      const std::string& key) {
    const size_t key_pos = h.find(key);
    if (key_pos == std::string::npos) return "";
    size_t colon = h.find(':', key_pos);
    if (colon == std::string::npos) return "";
    size_t quote = h.find_first_of("'\"", colon + 1);
    if (quote == std::string::npos) return "";
    const char q = h[quote];
    size_t end = h.find(q, quote + 1);
    if (end == std::string::npos) return "";
    return h.substr(quote + 1, end - quote - 1);
}

static StatusOr<NpyHeader> ParseNpyHeader(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Status::IOError("Failed to open npy file: " + path);
    }

    char magic[6];
    f.read(magic, sizeof(magic));
    if (!f.good() || std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        return Status::InvalidArgument("Invalid npy magic: " + path);
    }
    char version[2];
    f.read(version, sizeof(version));
    if (!f.good()) {
        return Status::IOError("Failed to read npy version: " + path);
    }

    uint32_t header_len = 0;
    if (version[0] == 1) {
        char len_buf[2];
        f.read(len_buf, sizeof(len_buf));
        if (!f.good()) {
            return Status::IOError("Failed to read npy v1 header length: " + path);
        }
        header_len = ReadLe16(len_buf);
    } else if (version[0] == 2 || version[0] == 3) {
        char len_buf[4];
        f.read(len_buf, sizeof(len_buf));
        if (!f.good()) {
            return Status::IOError("Failed to read npy v2/v3 header length: " + path);
        }
        header_len = ReadLe32(len_buf);
    } else {
        return Status::InvalidArgument("Unsupported npy version in: " + path);
    }

    std::string header(header_len, '\0');
    f.read(header.data(), static_cast<std::streamsize>(header_len));
    if (!f.good()) {
        return Status::IOError("Failed to read npy header: " + path);
    }

    NpyHeader out;
    out.descr = ExtractQuotedValue(header, "descr");
    out.fortran_order = (header.find("True") != std::string::npos &&
                         header.find("fortran_order") != std::string::npos);
    const size_t shape_key = header.find("shape");
    const size_t lparen = header.find('(', shape_key);
    const size_t rparen = header.find(')', lparen);
    if (shape_key == std::string::npos || lparen == std::string::npos ||
        rparen == std::string::npos || rparen <= lparen) {
        return Status::InvalidArgument("Failed to parse npy shape: " + path);
    }
    std::string shape = header.substr(lparen + 1, rparen - lparen - 1);
    std::stringstream ss(shape);
    while (ss.good()) {
        while (ss.peek() == ' ' || ss.peek() == ',') ss.get();
        if (!std::isdigit(ss.peek())) {
            ss.get();
            continue;
        }
        uint64_t dim = 0;
        ss >> dim;
        out.shape.push_back(dim);
        while (ss.peek() == ' ' || ss.peek() == ',') ss.get();
    }
    if (out.shape.empty()) {
        return Status::InvalidArgument("Empty npy shape: " + path);
    }
    out.data_offset = static_cast<uint64_t>(f.tellg());
    return out;
}

struct FloatRows {
    std::vector<float> data;
    uint32_t rows = 0;
    uint32_t cols = 0;
};

static StatusOr<FloatRows> LoadNpyFloat32Rows(const std::string& path,
                                              uint64_t row_offset,
                                              uint64_t row_count) {
    auto hdr_or = ParseNpyHeader(path);
    if (!hdr_or.ok()) return hdr_or.status();
    const auto& hdr = hdr_or.value();
    if (hdr.descr != "<f4" && hdr.descr != "=f4") {
        return Status::InvalidArgument("Expected float32 query npy: " + path);
    }
    if (hdr.fortran_order) {
        return Status::InvalidArgument("Fortran-order npy unsupported: " + path);
    }
    const uint64_t rows = hdr.shape[0];
    const uint64_t cols = hdr.shape.size() >= 2 ? hdr.shape[1] : 1;
    if (row_offset > rows) {
        return Status::InvalidArgument("query offset exceeds rows: " + path);
    }
    if (row_count == 0 || row_offset + row_count > rows) {
        row_count = rows - row_offset;
    }
    if (row_count > static_cast<uint64_t>(UINT32_MAX) ||
        cols > static_cast<uint64_t>(UINT32_MAX)) {
        return Status::InvalidArgument("query slice too large: " + path);
    }

    FloatRows out;
    out.rows = static_cast<uint32_t>(row_count);
    out.cols = static_cast<uint32_t>(cols);
    out.data.resize(static_cast<size_t>(row_count) * static_cast<size_t>(cols));

    std::ifstream f(path, std::ios::binary);
    const uint64_t byte_offset =
        hdr.data_offset + row_offset * cols * sizeof(float);
    f.seekg(static_cast<std::streamoff>(byte_offset));
    f.read(reinterpret_cast<char*>(out.data.data()),
           static_cast<std::streamsize>(out.data.size() * sizeof(float)));
    if (!f.good()) {
        return Status::IOError("Failed to read query rows from: " + path);
    }
    return out;
}

static StatusOr<std::vector<int64_t>> LoadNpyInt64VectorRows(
    const std::string& path, uint64_t row_offset, uint64_t row_count) {
    auto hdr_or = ParseNpyHeader(path);
    if (!hdr_or.ok()) return hdr_or.status();
    const auto& hdr = hdr_or.value();
    if (hdr.descr != "<i8" && hdr.descr != "=i8") {
        return Status::InvalidArgument("Expected int64 npy: " + path);
    }
    if (hdr.fortran_order || hdr.shape.size() != 1) {
        return Status::InvalidArgument("Expected 1-D C-order int64 npy: " + path);
    }
    const uint64_t rows = hdr.shape[0];
    if (row_offset > rows) {
        return Status::InvalidArgument("id offset exceeds rows: " + path);
    }
    if (row_count == 0 || row_offset + row_count > rows) {
        row_count = rows - row_offset;
    }
    std::vector<int64_t> out(row_count);
    std::ifstream f(path, std::ios::binary);
    const uint64_t byte_offset = hdr.data_offset + row_offset * sizeof(int64_t);
    f.seekg(static_cast<std::streamoff>(byte_offset));
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(out.size() * sizeof(int64_t)));
    if (!f.good()) {
        return Status::IOError("Failed to read int64 rows from: " + path);
    }
    return out;
}

static StatusOr<std::vector<std::vector<int64_t>>> LoadNpyGroundTruthRows(
    const std::string& path, uint64_t row_offset, uint64_t row_count,
    uint32_t topk) {
    auto hdr_or = ParseNpyHeader(path);
    if (!hdr_or.ok()) return hdr_or.status();
    const auto& hdr = hdr_or.value();
    const bool is_i64 = (hdr.descr == "<i8" || hdr.descr == "=i8");
    const bool is_i32 = (hdr.descr == "<i4" || hdr.descr == "=i4");
    if (!is_i64 && !is_i32) {
        return Status::InvalidArgument("Expected int32/int64 GT npy: " + path);
    }
    if (hdr.fortran_order || hdr.shape.size() < 2) {
        return Status::InvalidArgument("Expected 2-D C-order GT npy: " + path);
    }
    const uint64_t rows = hdr.shape[0];
    const uint64_t cols = hdr.shape[1];
    if (cols < topk) {
        return Status::InvalidArgument("GT top-k smaller than requested topk: " + path);
    }
    if (row_offset > rows) {
        return Status::InvalidArgument("GT offset exceeds rows: " + path);
    }
    if (row_count == 0 || row_offset + row_count > rows) {
        row_count = rows - row_offset;
    }

    std::vector<std::vector<int64_t>> out(row_count, std::vector<int64_t>(topk));
    std::ifstream f(path, std::ios::binary);
    const uint64_t elem_bytes = is_i64 ? sizeof(int64_t) : sizeof(int32_t);
    std::vector<char> row_buf(static_cast<size_t>(cols) * elem_bytes);
    for (uint64_t r = 0; r < row_count; ++r) {
        const uint64_t byte_offset =
            hdr.data_offset + (row_offset + r) * cols * elem_bytes;
        f.seekg(static_cast<std::streamoff>(byte_offset));
        f.read(row_buf.data(), static_cast<std::streamsize>(row_buf.size()));
        if (!f.good()) {
            return Status::IOError("Failed to read GT row from: " + path);
        }
        for (uint32_t k = 0; k < topk; ++k) {
            if (is_i64) {
                const auto* p = reinterpret_cast<const int64_t*>(row_buf.data());
                out[r][k] = p[k];
            } else {
                const auto* p = reinterpret_cast<const int32_t*>(row_buf.data());
                out[r][k] = static_cast<int64_t>(p[k]);
            }
        }
    }
    return out;
}

static double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double idx = p * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

static double ComputeRecallAtK(const std::vector<int64_t>& predicted,
                               const std::vector<int64_t>& gt,
                               uint32_t k) {
    if (gt.empty()) return 0.0;
    const uint32_t pk = std::min(k, static_cast<uint32_t>(predicted.size()));
    const uint32_t gk = std::min(k, static_cast<uint32_t>(gt.size()));
    std::unordered_set<int64_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(gk);
}

static const char* ExRaBitQStorageFormatName(uint32_t version) {
    if (version >= 12) return "compact_blocked_packed_magnitude";
    if (version >= 11) return "compact_blocked";
    if (version >= 10) return "packed_sign";
    return "legacy_byte_sign";
}

struct QueryMetrics {
    std::vector<double> latencies_ms;
    double recall1_sum = 0.0;
    double recall5_sum = 0.0;
    double recall10_sum = 0.0;
    double recallk_sum = 0.0;
    bool recall_available = false;
    uint64_t total_probed = 0;
    uint64_t total_safe_in = 0;
    uint64_t total_safe_out = 0;
    uint64_t total_uncertain = 0;
    uint64_t s2_safe_in = 0;
    uint64_t s2_safe_out = 0;
    uint64_t s2_uncertain = 0;
    uint64_t candidates_reranked = 0;
    uint64_t vec_only_reads = 0;
    uint64_t all_reads = 0;
    uint64_t payload_reads = 0;
    uint64_t stage2_decode_blocks = 0;
    uint64_t stage2_decode_input_bytes = 0;
    uint64_t stage2_decode_output_bytes = 0;
    double probe_ms = 0.0;
    double coarse_select_ms = 0.0;
    double stage1_ms = 0.0;
    double stage2_ms = 0.0;
    double stage2_decode_ms = 0.0;
    double submit_ms = 0.0;
    double rerank_compute_ms = 0.0;
    double fetch_missing_ms = 0.0;
};

static void Accumulate(QueryMetrics& m, const SearchResults& results,
                       const std::vector<int64_t>* gt, uint32_t topk) {
    const auto& st = results.stats();
    m.latencies_ms.push_back(st.total_time_ms);
    m.total_probed += st.total_probed;
    m.total_safe_in += st.total_safe_in;
    m.total_safe_out += st.total_safe_out;
    m.total_uncertain += st.total_uncertain;
    m.s2_safe_in += st.s2_safe_in;
    m.s2_safe_out += st.s2_safe_out;
    m.s2_uncertain += st.s2_uncertain;
    m.candidates_reranked += st.reranked_candidates;
    m.vec_only_reads += st.vec_only_read_requests;
    m.all_reads += st.all_read_requests;
    m.payload_reads += st.payload_read_requests;
    m.stage2_decode_blocks += st.stage2_decode_blocks;
    m.stage2_decode_input_bytes += st.stage2_decode_input_bytes;
    m.stage2_decode_output_bytes += st.stage2_decode_output_bytes;
    m.probe_ms += st.probe_time_ms;
    m.coarse_select_ms += st.coarse_select_ms;
    m.stage1_ms += st.probe_stage1_ms;
    m.stage2_ms += st.probe_stage2_ms;
    m.stage2_decode_ms += st.probe_stage2_decode_ms;
    m.submit_ms += st.probe_submit_ms;
    m.rerank_compute_ms += st.rerank_compute_ms;
    m.fetch_missing_ms += st.fetch_missing_ms;

    if (gt != nullptr) {
        std::vector<int64_t> predicted;
        predicted.reserve(results.size());
        for (uint32_t i = 0; i < results.size(); ++i) {
            const auto& payload = results[i].payload;
            if (!payload.empty() && payload[0].dtype == DType::INT64) {
                predicted.push_back(payload[0].fixed.i64);
            }
        }
        m.recall_available = true;
        m.recall1_sum += ComputeRecallAtK(predicted, *gt, 1);
        m.recall5_sum += ComputeRecallAtK(predicted, *gt, std::min<uint32_t>(5, topk));
        m.recall10_sum += ComputeRecallAtK(predicted, *gt, std::min<uint32_t>(10, topk));
        m.recallk_sum += ComputeRecallAtK(predicted, *gt, topk);
    }
}

static int Usage() {
    std::fprintf(stderr,
        "Usage: bench_e2e --index-dir DIR --query-file queries.npy [options]\n"
        "\n"
        "Online query benchmark. It does not load image_embeddings.npy or metadata.jsonl.\n"
        "\n"
        "Required:\n"
        "  --index-dir DIR             Existing index containing cluster.clu and data.dat\n"
        "  --query-file FILE.npy       Float32 query matrix\n"
        "\n"
        "Common options:\n"
        "  --output DIR                Output dir (default: ./bench_online_query_out)\n"
        "  --query-offset N            First query row (default: 0)\n"
        "  --query-count N             Number of query rows (default: all remaining)\n"
        "  --queries N                 Alias for --query-count\n"
        "  --query-ids FILE.npy        Optional int64 query ids slice\n"
        "  --gt-file FILE.npy          Optional int32/int64 GT matrix slice\n"
        "  --gt-offset N               First GT row (default: query-offset)\n"
        "  --topk N                    Top-k (default: 10)\n"
        "  --nprobe N                  Probed clusters (default: 64)\n"
        "  --dynamic-safein static|frontier (default: static)\n"
        "  --dynamic-safeout 0|1       Dynamic SafeOut (default: 1)\n"
        "  --non-safeout-candidate-budget N (default: 0)\n"
        "  --fixed-vec-buffer-count N  Fixed vector buffers (default: 0)\n"
        "  --fine-grained-timing 0|1   Enable detailed stage timing (default: 0)\n"
        "  --hotpath-detailed-timing 0|1 (default: 0)\n"
        "  --direct-io                 Open index files with direct I/O\n");
    return 2;
}

int main(int argc, char** argv) {
    if (HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        return Usage();
    }

    const auto t_process_start = std::chrono::steady_clock::now();
    const ProcRss rss_start = ReadProcRss();

    const std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    const std::string query_file = GetStringArg(argc, argv, "--query-file", "");
    if (index_dir.empty() || query_file.empty()) {
        return Usage();
    }

    const std::string output_dir =
        GetStringArg(argc, argv, "--output", "bench_online_query_out");
    const uint64_t query_offset =
        static_cast<uint64_t>(std::max(0, GetIntArg(argc, argv, "--query-offset", 0)));
    int query_count_arg = GetIntArg(argc, argv, "--query-count", -1);
    if (query_count_arg < 0) {
        query_count_arg = GetIntArg(argc, argv, "--queries", 0);
    }
    const uint64_t query_count =
        query_count_arg > 0 ? static_cast<uint64_t>(query_count_arg) : 0;
    const uint32_t topk = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--topk", 10)));
    const uint32_t nprobe = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--nprobe", 64)));
    const int io_queue_depth = std::max(1, GetIntArg(argc, argv, "--io-queue-depth", 64));
    const bool iopoll = GetIntArg(argc, argv, "--iopoll", 0) != 0;
    const bool sqpoll = GetIntArg(argc, argv, "--sqpoll", 0) != 0;
    const bool direct_io = HasFlag(argc, argv, "--direct-io");
    const std::string submission_mode =
        GetStringArg(argc, argv, "--submission-mode", "shared");
    const std::string dynamic_safein =
        GetStringArg(argc, argv, "--dynamic-safein", "static");

    Log("=== VDB Online Query Benchmark ===\n");
    Log("Index: %s\n", index_dir.c_str());
    Log("Query file: %s offset=%llu count=%llu\n", query_file.c_str(),
        static_cast<unsigned long long>(query_offset),
        static_cast<unsigned long long>(query_count));

    auto queries_or = LoadNpyFloat32Rows(query_file, query_offset, query_count);
    if (!queries_or.ok()) {
        std::fprintf(stderr, "Failed to load query slice: %s\n",
                     queries_or.status().ToString().c_str());
        return 1;
    }
    FloatRows queries = std::move(queries_or.value());
    const ProcRss rss_after_query_load = ReadProcRss();

    std::vector<int64_t> query_ids(queries.rows);
    std::iota(query_ids.begin(), query_ids.end(),
              static_cast<int64_t>(query_offset));
    const std::string query_ids_file = GetStringArg(argc, argv, "--query-ids", "");
    if (!query_ids_file.empty()) {
        auto ids_or = LoadNpyInt64VectorRows(query_ids_file, query_offset, queries.rows);
        if (!ids_or.ok()) {
            std::fprintf(stderr, "Failed to load query id slice: %s\n",
                         ids_or.status().ToString().c_str());
            return 1;
        }
        query_ids = std::move(ids_or.value());
    }

    std::vector<std::vector<int64_t>> gt;
    const std::string gt_file = GetStringArg(argc, argv, "--gt-file", "");
    const uint64_t gt_offset = static_cast<uint64_t>(
        std::max(0, GetIntArg(argc, argv, "--gt-offset",
                              static_cast<int>(query_offset))));
    if (!gt_file.empty()) {
        auto gt_or = LoadNpyGroundTruthRows(gt_file, gt_offset, queries.rows, topk);
        if (!gt_or.ok()) {
            std::fprintf(stderr, "Failed to load GT slice: %s\n",
                         gt_or.status().ToString().c_str());
            return 1;
        }
        gt = std::move(gt_or.value());
    }
    const ProcRss rss_after_gt_load = ReadProcRss();

    IvfIndex index;
    Status s = index.Open(index_dir, direct_io);
    if (!s.ok()) {
        std::fprintf(stderr, "Open index failed: %s\n", s.ToString().c_str());
        return 1;
    }
    if (queries.cols != index.logical_dim()) {
        std::fprintf(stderr,
                     "Query dim mismatch: query cols=%u index logical_dim=%u\n",
                     queries.cols, index.logical_dim());
        return 1;
    }
    const ProcRss rss_after_index_open = ReadProcRss();

    auto t_preload_start = std::chrono::steady_clock::now();
    s = index.segment().PreloadAllClusters();
    if (!s.ok()) {
        std::fprintf(stderr, "Preload failed: %s\n", s.ToString().c_str());
        return 1;
    }
    const double preload_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_preload_start).count();
    const ProcRss rss_after_preload = ReadProcRss();

    IoUringReader cluster_reader;
    auto init_status = cluster_reader.Init(
        static_cast<uint32_t>(io_queue_depth), 4096, iopoll, sqpoll);
    if (!init_status.ok()) {
        std::fprintf(stderr, "IoUring init failed: %s\n",
                     init_status.ToString().c_str());
        return 1;
    }

    std::unique_ptr<IoUringReader> data_reader;
    if (submission_mode == "isolated") {
        data_reader = std::make_unique<IoUringReader>();
        auto data_status = data_reader->Init(
            static_cast<uint32_t>(io_queue_depth), 4096, iopoll, sqpoll);
        if (!data_status.ok()) {
            std::fprintf(stderr, "Data IoUring init failed: %s\n",
                         data_status.ToString().c_str());
            return 1;
        }
        int clu_fd = index.segment().clu_fd();
        (void)cluster_reader.RegisterFiles(&clu_fd, 1);
        int dat_fd = index.segment().data_reader().fd();
        (void)data_reader->RegisterFiles(&dat_fd, 1);
    } else if (submission_mode == "shared") {
        int fds[2] = {index.segment().clu_fd(), index.segment().data_reader().fd()};
        (void)cluster_reader.RegisterFiles(fds, 2);
    } else {
        std::fprintf(stderr, "Invalid --submission-mode: %s\n",
                     submission_mode.c_str());
        return 1;
    }

    SearchConfig cfg;
    cfg.top_k = topk;
    cfg.nprobe = nprobe;
    cfg.io_queue_depth = static_cast<uint32_t>(io_queue_depth);
    cfg.use_sqpoll = sqpoll;
    cfg.submission_mode = submission_mode == "isolated"
        ? SubmissionMode::Isolated
        : SubmissionMode::Shared;
    cfg.enable_dynamic_safeout =
        GetIntArg(argc, argv, "--dynamic-safeout", 1) != 0;
    cfg.dynamic_safein_mode = dynamic_safein == "frontier"
        ? DynamicSafeInMode::Frontier
        : DynamicSafeInMode::Static;
    cfg.dynamic_safein_min_probes = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--dynamic-safein-min-probes", 0)));
    cfg.dynamic_safein_stable_probes = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--dynamic-safein-stable-probes", 2)));
    cfg.dynamic_safein_rel_tol =
        static_cast<float>(GetDoubleArg(argc, argv, "--dynamic-safein-rel-tol", 0.005));
    cfg.dynamic_safein_abs_tol =
        static_cast<float>(GetDoubleArg(argc, argv, "--dynamic-safein-abs-tol", 0.0));
    cfg.non_safeout_candidate_budget = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--non-safeout-candidate-budget", 0)));
    cfg.fixed_vec_buffer_count = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--fixed-vec-buffer-count", 0)));
    cfg.cluster_submit_reserve = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--cluster-submit-reserve", 8)));
    cfg.submit_batch_size = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--submit-batch", 32)));
    cfg.enable_fine_grained_timing =
        GetIntArg(argc, argv, "--fine-grained-timing", 0) != 0;
    cfg.enable_hotpath_detailed_timing =
        GetIntArg(argc, argv, "--hotpath-detailed-timing", 0) != 0;
    cfg.enable_address_decode_simd =
        GetIntArg(argc, argv, "--address-decode-simd", 1) != 0;
    cfg.enable_rerank_batched_distance_simd =
        GetIntArg(argc, argv, "--rerank-batched-distance-simd", 1) != 0;
    cfg.enable_coarse_select_simd =
        GetIntArg(argc, argv, "--coarse-select-simd", 1) != 0;
    cfg.enable_stage1_safein =
        GetIntArg(argc, argv, "--enable-stage1-safein", 1) != 0;
    cfg.enable_stage2_collect_block_first =
        GetIntArg(argc, argv, "--stage2-block-first", 1) != 0;
    cfg.enable_stage2_scatter_batch_classify =
        GetIntArg(argc, argv, "--stage2-batch-classify", 1) != 0;
    const double safein_eps_override =
        GetDoubleArg(argc, argv, "--safein-epsilon-override", -1.0);
    const double safeout_eps_override =
        GetDoubleArg(argc, argv, "--safeout-epsilon-override", -1.0);
    cfg.safein_epsilon_override = static_cast<float>(safein_eps_override);
    cfg.safeout_epsilon_override = static_cast<float>(safeout_eps_override);

    std::unique_ptr<OverlapScheduler> scheduler;
    if (data_reader) {
        scheduler = std::make_unique<OverlapScheduler>(
            index, cluster_reader, *data_reader, cfg);
    } else {
        scheduler = std::make_unique<OverlapScheduler>(index, cluster_reader, cfg);
    }

    QueryMetrics metrics;
    ProcRss peak_during_query = ReadProcRss();
    Log("\n[Query] Running %u queries...\n", queries.rows);
    for (uint32_t qi = 0; qi < queries.rows; ++qi) {
        const float* q = queries.data.data() + static_cast<size_t>(qi) * queries.cols;
        SearchResults results = scheduler->Search(q);
        const std::vector<int64_t>* gt_row =
            (!gt.empty() && qi < gt.size()) ? &gt[qi] : nullptr;
        Accumulate(metrics, results, gt_row, topk);
        const ProcRss sample = ReadProcRss();
        if (sample.rss_kib > peak_during_query.rss_kib) {
            peak_during_query.rss_kib = sample.rss_kib;
        }
        if (sample.hwm_kib > peak_during_query.hwm_kib) {
            peak_during_query.hwm_kib = sample.hwm_kib;
        }
        if ((qi + 1) % 100 == 0 || qi + 1 == queries.rows) {
            Log("  progress: %u/%u\n", qi + 1, queries.rows);
        }
    }
    const ProcRss rss_after_queries = ReadProcRss();
    const double process_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_process_start).count();

    const uint32_t Q = queries.rows;
    const double inv_q = Q > 0 ? 1.0 / static_cast<double>(Q) : 0.0;
    const double avg_latency =
        std::accumulate(metrics.latencies_ms.begin(), metrics.latencies_ms.end(), 0.0) *
        inv_q;
    const double p50 = Percentile(metrics.latencies_ms, 0.50);
    const double p95 = Percentile(metrics.latencies_ms, 0.95);
    const double p99 = Percentile(metrics.latencies_ms, 0.99);
    const double recall1 = metrics.recall_available ? metrics.recall1_sum * inv_q : 0.0;
    const double recall5 = metrics.recall_available ? metrics.recall5_sum * inv_q : 0.0;
    const double recall10 = metrics.recall_available ? metrics.recall10_sum * inv_q : 0.0;
    const double recallk = metrics.recall_available ? metrics.recallk_sum * inv_q : 0.0;

    Log("\n=== Summary ===\n");
    Log("  avg=%.4f ms p50=%.4f p95=%.4f p99=%.4f\n",
        avg_latency, p50, p95, p99);
    if (metrics.recall_available) {
        Log("  recall@1=%.4f recall@5=%.4f recall@10=%.4f recall@k=%.4f\n",
            recall1, recall5, recall10, recallk);
    }
    Log("  rss_after_query_load=%lld KiB rss_after_preload=%lld KiB peak_query=%lld KiB\n",
        static_cast<long long>(rss_after_query_load.rss_kib),
        static_cast<long long>(rss_after_preload.rss_kib),
        static_cast<long long>(peak_during_query.rss_kib));

    fs::create_directories(output_dir);
    const std::string json_path = output_dir + "/results.json";
    std::ofstream f(json_path);
    if (!f.is_open()) {
        std::fprintf(stderr, "Failed to open output: %s\n", json_path.c_str());
        return 1;
    }

    const uint32_t clu_version = index.segment().cluster_reader().file_version();
    const bool packed_stage2 =
        (clu_version >= 12 && index.segment().rabitq_config().bits > 1);

    f << "{\n";
    f << "  \"metrics\": {\n";
    f << "    " << JStr("benchmark_mode", "online_query") << ",\n";
    f << "    " << JStr("index_dir", index_dir) << ",\n";
    f << "    " << JStr("query_file", query_file) << ",\n";
    f << "    " << JInt("query_offset", static_cast<int64_t>(query_offset)) << ",\n";
    f << "    " << JInt("num_queries", Q) << ",\n";
    f << "    " << JInt("query_dim", queries.cols) << ",\n";
    f << "    " << JInt("topk", topk) << ",\n";
    f << "    " << JInt("nprobe", nprobe) << ",\n";
    f << "    " << JBool("recall_available", metrics.recall_available) << ",\n";
    f << "    " << JNum("recall_at_1", recall1) << ",\n";
    f << "    " << JNum("recall_at_5", recall5) << ",\n";
    f << "    " << JNum("recall_at_10", recall10) << ",\n";
    f << "    " << JNum("recall_at_k", recallk) << ",\n";
    f << "    " << JNum("avg_query_time_ms", avg_latency) << ",\n";
    f << "    " << JNum("p50_ms", p50) << ",\n";
    f << "    " << JNum("p95_ms", p95) << ",\n";
    f << "    " << JNum("p99_ms", p99) << ",\n";
    f << "    " << JNum("process_wall_ms", process_wall_ms) << ",\n";
    f << "    " << JNum("preload_wall_ms", preload_wall_ms) << ",\n";
    f << "    " << JStr("resident_preload_mode", index.segment().resident_preload_mode()) << ",\n";
    f << "    " << JInt("resident_preload_batch_size", static_cast<int64_t>(index.segment().resident_preload_batch_size())) << ",\n";
    f << "    " << JInt("preload_bytes", static_cast<int64_t>(index.segment().resident_preload_bytes())) << ",\n";
    f << "    " << JInt("resident_file_size_bytes", static_cast<int64_t>(index.segment().resident_file_size_bytes())) << ",\n";
    f << "    " << JInt("resident_file_buffer_bytes", static_cast<int64_t>(index.segment().resident_file_buffer_bytes())) << ",\n";
    f << "    " << JInt("resident_code_storage_bytes", static_cast<int64_t>(index.segment().resident_code_storage_bytes())) << ",\n";
    f << "    " << JInt("resident_decoded_address_bytes", static_cast<int64_t>(index.segment().resident_decoded_address_bytes())) << ",\n";
    f << "    " << JInt("resident_raw_address_bytes", static_cast<int64_t>(index.segment().resident_raw_address_bytes())) << ",\n";
    f << "    " << JInt("resident_parsed_address_duplicate_bytes", static_cast<int64_t>(index.segment().resident_parsed_address_duplicate_bytes())) << ",\n";
    f << "    " << JInt("resident_cluster_mem_bytes", static_cast<int64_t>(index.segment().resident_cluster_mem_bytes())) << ",\n";
    f << "    " << JInt("resident_parallel_view_bytes", static_cast<int64_t>(index.segment().resident_parallel_view_bytes())) << ",\n";
    f << "    " << JInt("exrabitq_storage_version", clu_version) << ",\n";
    f << "    " << JStr("exrabitq_storage_format", ExRaBitQStorageFormatName(clu_version)) << ",\n";
    f << "    " << JBool("exrabitq_stage2_magnitude_packed", packed_stage2) << "\n";
    f << "  },\n";
    f << "  \"pipeline_stats\": {\n";
    f << "    " << JNum("avg_total_probed", metrics.total_probed * inv_q) << ",\n";
    f << "    " << JNum("avg_safe_in", metrics.total_safe_in * inv_q) << ",\n";
    f << "    " << JNum("avg_safe_out", metrics.total_safe_out * inv_q) << ",\n";
    f << "    " << JNum("avg_uncertain", metrics.total_uncertain * inv_q) << ",\n";
    f << "    " << JNum("avg_s2_safe_in", metrics.s2_safe_in * inv_q) << ",\n";
    f << "    " << JNum("avg_s2_safe_out", metrics.s2_safe_out * inv_q) << ",\n";
    f << "    " << JNum("avg_s2_uncertain", metrics.s2_uncertain * inv_q) << ",\n";
    f << "    " << JNum("avg_candidates_reranked", metrics.candidates_reranked * inv_q) << ",\n";
    f << "    " << JNum("avg_vec_only_read_requests", metrics.vec_only_reads * inv_q) << ",\n";
    f << "    " << JNum("avg_all_read_requests", metrics.all_reads * inv_q) << ",\n";
    f << "    " << JNum("avg_payload_read_requests", metrics.payload_reads * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_ms", metrics.probe_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_select_ms", metrics.coarse_select_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_stage1_ms", metrics.stage1_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_stage2_ms", metrics.stage2_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_stage2_decode_ms", metrics.stage2_decode_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_submit_ms", metrics.submit_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_rerank_compute_ms", metrics.rerank_compute_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_fetch_missing_ms", metrics.fetch_missing_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_decode_blocks", metrics.stage2_decode_blocks * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_decode_input_bytes", metrics.stage2_decode_input_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_decode_output_bytes", metrics.stage2_decode_output_bytes * inv_q) << "\n";
    f << "  },\n";
    f << "  \"rss_profile\": {\n";
    f << "    " << JInt("process_start_rss_kib", rss_start.rss_kib) << ",\n";
    f << "    " << JInt("process_start_hwm_kib", rss_start.hwm_kib) << ",\n";
    f << "    " << JInt("after_query_load_rss_kib", rss_after_query_load.rss_kib) << ",\n";
    f << "    " << JInt("after_gt_load_rss_kib", rss_after_gt_load.rss_kib) << ",\n";
    f << "    " << JInt("after_index_open_rss_kib", rss_after_index_open.rss_kib) << ",\n";
    f << "    " << JInt("after_preload_rss_kib", rss_after_preload.rss_kib) << ",\n";
    f << "    " << JInt("peak_during_queries_rss_kib", peak_during_query.rss_kib) << ",\n";
    f << "    " << JInt("after_queries_rss_kib", rss_after_queries.rss_kib) << ",\n";
    f << "    " << JInt("after_queries_hwm_kib", rss_after_queries.hwm_kib) << ",\n";
    f << "    " << JInt("online_index_resident_delta_kib", rss_after_preload.rss_kib - rss_after_query_load.rss_kib) << ",\n";
    f << "    " << JInt("query_peak_delta_kib", peak_during_query.rss_kib - rss_after_preload.rss_kib) << "\n";
    f << "  }\n";
    f << "}\n";
    f.close();

    Log("  Output: %s\n", json_path.c_str());
    return 0;
}
