/// bench_e2e.cpp — End-to-end benchmark on real COCO datasets.
///
/// Measures: recall@{1,5,10}, query latency (avg/p50/p95/p99),
///           build time, brute-force GT time, IO/CPU pipeline utilization.
///
/// Usage:
///   bench_e2e [--dataset /path/to/coco_1k]
///             [--crc 1] [--crc-alpha 0.1] [--crc-calib 0.5] [--crc-tune 0.1]
///
/// Output:
///   /home/zcq/VDB/test/{dataset}_{timestamp}/
///     index/         — built IVF index files
///     config.json    — parameter snapshot
///     results.json   — metrics + pipeline stats + per-query samples

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

#include "vdb/common/distance.h"
#include "vdb/common/types.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/jsonl_reader.h"
#include "vdb/io/npy_reader.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::query;

namespace fs = std::filesystem;

// ============================================================================
// Command-line helpers
// ============================================================================

static std::string GetStringArg(int argc, char* argv[], const char* name,
                                const std::string& default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return default_val;
}

static bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

static int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    }
    return default_val;
}

static float GetFloatArg(int argc, char* argv[], const char* name, float default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::strtof(argv[i + 1], nullptr);
    }
    return default_val;
}

static std::vector<uint64_t> ParseUint64List(const std::string& value) {
    std::vector<uint64_t> out;
    if (value.empty()) {
        return out;
    }
    std::istringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) {
            out.push_back(std::stoull(token));
        }
    }
    return out;
}

// ============================================================================
// Timestamp + dataset name
// ============================================================================

static std::string Timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", std::localtime(&t));
    return buf;
}

static std::string DatasetName(const std::string& path) {
    return fs::path(path).filename().string();
}

static std::string FormatEpsilonTag(float epsilon_percentile) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << epsilon_percentile;
    return oss.str();
}

static std::string ResolveBenchIndexDir(const std::string& dataset_name,
                                        const std::string& output_dir,
                                        int nlist,
                                        int bits,
                                        float epsilon_percentile,
                                        const std::string& assignment_mode_tag,
                                        float rair_lambda,
                                        const std::string& requested_index_dir) {
    if (!requested_index_dir.empty()) {
        return requested_index_dir;
    }
    if (dataset_name == "coco_100k" && nlist == 2048) {
        std::string dir = "/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits" +
               std::to_string(bits) + "_eps" +
               FormatEpsilonTag(epsilon_percentile);
        if (!assignment_mode_tag.empty() && assignment_mode_tag != "single") {
            dir += "_" + assignment_mode_tag;
            if (assignment_mode_tag == "redundant_top2_rair") {
                std::ostringstream oss;
                oss << "_lambda" << std::fixed << std::setprecision(2)
                    << rair_lambda;
                dir += oss.str();
            }
        }
        return dir;
    }
    return output_dir + "/index";
}

static std::string NormalizePath(const std::string& path) {
    std::error_code ec;
    fs::path normalized = fs::absolute(fs::path(path), ec);
    if (ec) {
        return path;
    }
    return normalized.lexically_normal().string();
}

static std::string CrcParamsSidecarPath(const std::string& index_dir) {
    return index_dir + "/crc_calibration_params.bin";
}

static std::string CrcRunCalibrationPath(const std::string& output_dir) {
    return output_dir + "/crc_calibration.json";
}

static double FileSizeBytesOrZero(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return 0.0;
    return static_cast<double>(size);
}

static bool EndsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string InferGtMode(const std::string& gt_file) {
    if (gt_file.empty()) return "";
    if (EndsWith(gt_file, ".ivecs")) return "ivecs";
    if (EndsWith(gt_file, ".npy")) return "npy";
    return "";
}

static StatusOr<std::vector<std::vector<int64_t>>> LoadExternalGroundTruth(
    const std::string& gt_file, uint32_t q_count, uint32_t gt_k) {
    std::vector<std::vector<int64_t>> gt_topk;
    const std::string mode = InferGtMode(gt_file);
    if (mode.empty()) {
        return Status::InvalidArgument(
            "Unsupported ground-truth file format: " + gt_file);
    }

    if (mode == "ivecs") {
        auto gt_or = io::LoadIvecs(gt_file);
        if (!gt_or.ok()) return gt_or.status();
        const auto& gt = gt_or.value();
        if (gt.rows < q_count) {
            return Status::InvalidArgument(
                "Ground-truth rows (" + std::to_string(gt.rows) +
                ") smaller than query count (" + std::to_string(q_count) + ")");
        }
        if (gt.cols < gt_k) {
            return Status::InvalidArgument(
                "Ground-truth cols (" + std::to_string(gt.cols) +
                ") smaller than required top-k (" + std::to_string(gt_k) + ")");
        }
        gt_topk.resize(q_count);
        for (uint32_t qi = 0; qi < q_count; ++qi) {
            gt_topk[qi].resize(gt_k);
            const int32_t* row = gt.data.data() + static_cast<size_t>(qi) * gt.cols;
            for (uint32_t k = 0; k < gt_k; ++k) {
                gt_topk[qi][k] = static_cast<int64_t>(row[k]);
            }
        }
        return gt_topk;
    }

    auto gt_or = io::LoadNpyInt64Matrix(gt_file);
    if (!gt_or.ok()) return gt_or.status();
    const auto& gt = gt_or.value();
    if (gt.rows < q_count) {
        return Status::InvalidArgument(
            "Ground-truth rows (" + std::to_string(gt.rows) +
            ") smaller than query count (" + std::to_string(q_count) + ")");
    }
    if (gt.cols < gt_k) {
        return Status::InvalidArgument(
            "Ground-truth cols (" + std::to_string(gt.cols) +
            ") smaller than required top-k (" + std::to_string(gt_k) + ")");
    }
    gt_topk.resize(q_count);
    for (uint32_t qi = 0; qi < q_count; ++qi) {
        gt_topk[qi].resize(gt_k);
        const int64_t* row = gt.data.data() + static_cast<size_t>(qi) * gt.cols;
        for (uint32_t k = 0; k < gt_k; ++k) {
            gt_topk[qi][k] = row[k];
        }
    }
    return gt_topk;
}

static bool ParseCrcSolverArg(const std::string& arg,
                              CrcCalibrator::Solver* solver) {
    if (solver == nullptr) return false;
    if (arg == "brent") {
        *solver = CrcCalibrator::Solver::Brent;
        return true;
    }
    if (arg == "discrete_threshold") {
        *solver = CrcCalibrator::Solver::DiscreteThreshold;
        return true;
    }
    return false;
}

static bool ParseCrcSplitModeArg(const std::string& arg, bool* use_stratified) {
    if (use_stratified == nullptr) return false;
    if (arg == "random") {
        *use_stratified = false;
        return true;
    }
    if (arg == "stratified") {
        *use_stratified = true;
        return true;
    }
    return false;
}

static const char* ExRaBitQStorageFormatName(uint32_t clu_file_version) {
    if (clu_file_version >= 11) return "compact_blocked";
    if (clu_file_version >= 10) return "packed_sign";
    return "legacy_byte_sign";
}

static Status ReadCalibrationResults(const std::string& path,
                                     CalibrationResults* results) {
    if (results == nullptr) {
        return Status::InvalidArgument("results must not be null");
    }

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Status::IOError("Failed to open CRC calibration sidecar: " +
                               path);
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!f.good()) {
        return Status::IOError("Failed to read CRC calibration sidecar header: " +
                               path);
    }
    if (magic != 0x43524350u) {
        return Status::InvalidArgument(
            "CRC calibration sidecar has invalid magic: " + path);
    }
    if (version != 1) {
        return Status::InvalidArgument(
            "CRC calibration sidecar has unsupported version: " + path);
    }

    f.read(reinterpret_cast<char*>(results), sizeof(*results));
    if (!f.good()) {
        return Status::IOError(
            "Failed to read CRC calibration sidecar payload: " + path);
    }
    return Status::OK();
}

// ============================================================================
// JSONL field parsing (no JSON library)
// ============================================================================

static int64_t ParseImageId(std::string_view line) {
    auto pos = line.find("\"image_id\":");
    if (pos == std::string_view::npos) return -1;
    pos += 11;  // skip "image_id":
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    int64_t val = 0;
    bool neg = false;
    if (pos < line.size() && line[pos] == '-') { neg = true; ++pos; }
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        val = val * 10 + (line[pos] - '0');
        ++pos;
    }
    return neg ? -val : val;
}

static std::string ParseCaption(std::string_view line) {
    auto pos = line.find("\"caption\":");
    if (pos == std::string_view::npos) return "";
    pos += 10;  // skip "caption":
    // find opening quote
    pos = line.find('"', pos);
    if (pos == std::string_view::npos) return "";
    ++pos;  // skip opening quote
    auto end = line.find('"', pos);
    if (end == std::string_view::npos) return "";
    return std::string(line.substr(pos, end - pos));
}

// ============================================================================
// JSON output helpers (hand-crafted)
// ============================================================================

static std::string JStr(const std::string& k, const std::string& v) {
    return "\"" + k + "\": \"" + v + "\"";
}

static std::string JNum(const std::string& k, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return "\"" + k + "\": " + buf;
}

static std::string JInt(const std::string& k, int64_t v) {
    return "\"" + k + "\": " + std::to_string(v);
}

static std::string JBool(const std::string& k, bool v) {
    return "\"" + k + "\": " + (v ? "true" : "false");
}

static std::string JArr64(const std::string& k, const std::vector<int64_t>& v) {
    std::string s = "\"" + k + "\": [";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ", ";
        s += std::to_string(v[i]);
    }
    s += "]";
    return s;
}

static std::string JArrF(const std::string& k, const std::vector<float>& v) {
    std::string s = "\"" + k + "\": [";
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ", ";
        std::snprintf(buf, sizeof(buf), "%.6f", v[i]);
        s += buf;
    }
    s += "]";
    return s;
}

// ============================================================================
// Percentile (on pre-sorted vector)
// ============================================================================

static double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ============================================================================
// Recall computation
// ============================================================================

static double ComputeRecallAtK(const std::vector<int64_t>& predicted,
                                const std::vector<int64_t>& gt,
                                uint32_t K) {
    uint32_t pk = std::min(K, static_cast<uint32_t>(predicted.size()));
    uint32_t gk = std::min(K, static_cast<uint32_t>(gt.size()));
    std::unordered_set<int64_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(gk);
}

// ============================================================================
// Log
// ============================================================================

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// Per-query result struct (used by both rounds)
// ============================================================================

struct QueryResult {
    int64_t query_id;
    std::vector<int64_t> predicted_ids;
    std::vector<float> predicted_dists;
    double query_time_ms;
    double io_wait_ms;
    double coarse_select_ms = 0;
    double coarse_score_ms = 0;
    double coarse_topn_ms = 0;
    double probe_time_ms;
    double probe_prepare_ms = 0;
    double probe_prepare_rotation_ms = 0;
    double probe_prepare_subtract_ms = 0;
    double probe_prepare_normalize_ms = 0;
    double probe_prepare_quantize_ms = 0;
    double probe_prepare_lut_build_ms = 0;
    double probe_prepare_quant_lut_ms = 0;
    double probe_stage1_ms = 0;
    double probe_stage1_estimate_ms = 0;
    double probe_stage1_mask_ms = 0;
    double probe_stage1_iterate_ms = 0;
    double probe_stage1_classify_only_ms = 0;
    double probe_stage2_ms = 0;
    double probe_stage2_collect_ms = 0;
    double probe_stage2_kernel_ms = 0;
    double probe_stage2_scatter_ms = 0;
    double probe_stage2_kernel_sign_flip_ms = 0;
    double probe_stage2_kernel_abs_fma_ms = 0;
    double probe_stage2_kernel_tail_ms = 0;
    double probe_stage2_kernel_reduce_ms = 0;
    double probe_classify_ms = 0;
    double probe_submit_ms = 0;
    double probe_submit_prepare_vec_only_ms = 0;
    double probe_submit_prepare_all_ms = 0;
    double probe_submit_emit_ms = 0;
    double rerank_cpu_ms = 0;
    double prefetch_submit_ms = 0;
    double prefetch_wait_ms = 0;
    double safein_payload_prefetch_ms = 0;
    double candidate_collect_ms = 0;
    double pool_vector_read_ms = 0;
    double rerank_compute_ms = 0;
    double remaining_payload_fetch_ms = 0;
    double uring_prep_ms = 0;
    double uring_submit_ms = 0;
    double parse_cluster_ms = 0;
    double fetch_missing_ms = 0;
    uint32_t submit_calls = 0;
    uint32_t submit_window_flushes = 0;
    uint32_t submit_window_tail_flushes = 0;
    uint32_t submit_stop_flushes = 0;
    double submit_window_requests = 0;
    double candidate_batches_per_cluster = 0;
    double crc_estimates_buffered_per_cluster = 0;
    double crc_estimates_merged_per_cluster = 0;
    double stage2_block_lookups = 0;
    double stage2_block_reuses = 0;
    double crc_decision_ms = 0;
    double crc_buffer_ms = 0;
    double crc_merge_ms = 0;
    double crc_online_ms = 0;
    uint32_t crc_would_stop = 0;
    bool early_stopped;
    uint32_t clusters_skipped;
    uint32_t probed_clusters = 0;
    uint32_t total_probed;
    uint32_t safe_in;
    uint32_t safe_out;
    uint32_t uncertain;
    uint32_t s2_safe_in;
    uint32_t s2_safe_out;
    uint32_t s2_uncertain;
    uint32_t duplicate_candidates = 0;
    uint32_t deduplicated_candidates = 0;
    uint32_t unique_fetch_candidates = 0;
    uint32_t num_candidates_buffered = 0;
    uint32_t num_candidates_reranked = 0;
    uint32_t num_safein_payload_prefetched = 0;
    uint32_t num_remaining_payload_fetches = 0;
    uint32_t false_safeout;       // GT IDs missing from predicted results
    uint32_t false_safein_upper;  // safe_in - hits_in_topk (upper bound)
};

// ============================================================================
// Aggregated round metrics
// ============================================================================

struct RoundMetrics {
    double recall_at[3] = {};          // recall@1, @5, @10
    bool recall_available = true;
    double avg_query_ms = 0;
    double p50 = 0, p95 = 0, p99 = 0;
    double avg_io_wait = 0;
    double avg_cpu = 0;
    double avg_coarse_select = 0;
    double avg_coarse_score = 0;
    double avg_coarse_topn = 0;
    double avg_probe = 0;
    double avg_probe_prepare = 0;
    double avg_probe_prepare_rotation = 0;
    double avg_probe_prepare_subtract = 0;
    double avg_probe_prepare_normalize = 0;
    double avg_probe_prepare_quantize = 0;
    double avg_probe_prepare_lut_build = 0;
    double avg_probe_prepare_quant_lut = 0;
    double avg_probe_stage1 = 0;
    double avg_probe_stage1_estimate = 0;
    double avg_probe_stage1_mask = 0;
    double avg_probe_stage1_iterate = 0;
    double avg_probe_stage1_classify_only = 0;
    double avg_probe_stage2 = 0;
    double avg_probe_stage2_collect = 0;
    double avg_probe_stage2_kernel = 0;
    double avg_probe_stage2_scatter = 0;
    double avg_probe_stage2_kernel_sign_flip = 0;
    double avg_probe_stage2_kernel_abs_fma = 0;
    double avg_probe_stage2_kernel_tail = 0;
    double avg_probe_stage2_kernel_reduce = 0;
    double avg_probe_classify = 0;
    double avg_probe_submit = 0;
    double avg_probe_submit_prepare_vec_only = 0;
    double avg_probe_submit_prepare_all = 0;
    double avg_probe_submit_emit = 0;
    double avg_rerank_cpu = 0;
    double avg_prefetch_submit = 0;
    double avg_prefetch_wait = 0;
    double avg_safein_payload_prefetch = 0;
    double avg_candidate_collect = 0;
    double avg_pool_vector_read = 0;
    double avg_rerank_compute = 0;
    double avg_remaining_payload_fetch = 0;
    double avg_uring_prep = 0;
    double avg_uring_submit = 0;
    double avg_parse_cluster = 0;
    double avg_fetch_missing = 0;
    double avg_submit_calls = 0;
    double avg_submit_window_flushes = 0;
    double avg_submit_window_tail_flushes = 0;
    double avg_submit_stop_flushes = 0;
    double avg_submit_window_requests = 0;
    double avg_probed_clusters = 0;
    double avg_probed = 0;
    double avg_safe_in = 0;
    double avg_safe_out = 0;
    double avg_uncertain = 0;
    double avg_s2_safe_in = 0;
    double avg_s2_safe_out = 0;
    double avg_s2_uncertain = 0;
    double avg_duplicate_candidates = 0;
    double avg_deduplicated_candidates = 0;
    double avg_unique_fetch_candidates = 0;
    double avg_candidate_batches_per_cluster = 0;
    double avg_crc_estimates_buffered_per_cluster = 0;
    double avg_crc_estimates_merged_per_cluster = 0;
    double avg_stage2_block_lookups = 0;
    double avg_stage2_block_reuses = 0;
    double avg_crc_decision_ms = 0;
    double avg_crc_buffer_ms = 0;
    double avg_crc_merge_ms = 0;
    double avg_crc_online_ms = 0;
    double avg_crc_would_stop = 0;
    double avg_candidates_buffered = 0;
    double avg_candidates_reranked = 0;
    double avg_safein_payload_prefetched = 0;
    double avg_remaining_payload_fetches = 0;
    double avg_false_safeout = 0;
    double avg_false_safein_upper = 0;
    uint64_t total_final_safein = 0;   // absolute count: S1 SafeIn + S2 SafeIn
    double early_pct = 0;
    double avg_skipped = 0;
    double overlap_ratio = 0;
    double preload_time_ms = 0;
    double preload_bytes = 0;
    double resident_cluster_mem_bytes = 0;
    double resident_parallel_view_build_ms = 0;
    double resident_parallel_view_bytes = 0;
    double index_total_bytes = 0;
    double index_cluster_clu_bytes = 0;
    double index_data_dat_bytes = 0;
    double index_rotation_bytes = 0;
    double index_rotated_centroids_bytes = 0;
    bool query_uses_resident_clusters = false;
};

// ============================================================================
// Run one query round and compute metrics
// ============================================================================

static std::pair<std::vector<QueryResult>, RoundMetrics> RunQueryRound(
    const char* label,
    IvfIndex& index, AsyncReader& cluster_reader, AsyncReader* data_reader,
    const SearchConfig& search_cfg,
    const float* queries, uint32_t Q, Dim dim,
    const std::vector<int64_t>& qry_ids_data,
    const std::vector<std::vector<int64_t>>& gt_topk,
    const std::vector<std::vector<float>>& gt_dists,
    bool recall_available) {
    (void)gt_dists;

    Log("\n[%s] Querying %u vectors...\n", label, Q);

    if (!search_cfg.use_resident_clusters &&
        search_cfg.clu_read_mode == CluReadMode::FullPreload &&
        !index.segment().resident_preload_enabled()) {
        auto preload_status = index.segment().PreloadAllClusters();
        if (!preload_status.ok()) {
            std::fprintf(stderr,
                         "Failed to preload .clu before benchmark round: %s\n",
                         preload_status.ToString().c_str());
            std::abort();
        }
    }

    std::unique_ptr<OverlapScheduler> scheduler;
    if (data_reader != nullptr) {
        scheduler = std::make_unique<OverlapScheduler>(
            index, cluster_reader, *data_reader, search_cfg);
    } else {
        scheduler = std::make_unique<OverlapScheduler>(
            index, cluster_reader, search_cfg);
    }

    std::vector<QueryResult> qresults(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = queries + static_cast<size_t>(qi) * dim;
        auto results = scheduler->Search(qvec);

        QueryResult& qr = qresults[qi];
        qr.query_id = qry_ids_data[qi];
        qr.query_time_ms = results.stats().total_time_ms;
        qr.io_wait_ms = results.stats().io_wait_time_ms;
        qr.coarse_select_ms = results.stats().coarse_select_ms;
        qr.coarse_score_ms = results.stats().coarse_score_ms;
        qr.coarse_topn_ms = results.stats().coarse_topn_ms;
        qr.probe_time_ms = results.stats().probe_time_ms;
        qr.probe_prepare_ms = results.stats().probe_prepare_ms;
        qr.probe_prepare_rotation_ms = results.stats().probe_prepare_rotation_ms;
        qr.probe_prepare_subtract_ms = results.stats().probe_prepare_subtract_ms;
        qr.probe_prepare_normalize_ms = results.stats().probe_prepare_normalize_ms;
        qr.probe_prepare_quantize_ms = results.stats().probe_prepare_quantize_ms;
        qr.probe_prepare_lut_build_ms = results.stats().probe_prepare_lut_build_ms;
        qr.probe_prepare_quant_lut_ms = results.stats().probe_prepare_quant_lut_ms;
        qr.probe_stage1_ms = results.stats().probe_stage1_ms;
        qr.probe_stage1_estimate_ms = results.stats().probe_stage1_estimate_ms;
        qr.probe_stage1_mask_ms = results.stats().probe_stage1_mask_ms;
        qr.probe_stage1_iterate_ms = results.stats().probe_stage1_iterate_ms;
        qr.probe_stage1_classify_only_ms = results.stats().probe_stage1_classify_only_ms;
        qr.probe_stage2_ms = results.stats().probe_stage2_ms;
        qr.probe_stage2_collect_ms = results.stats().probe_stage2_collect_ms;
        qr.probe_stage2_kernel_ms = results.stats().probe_stage2_kernel_ms;
        qr.probe_stage2_scatter_ms = results.stats().probe_stage2_scatter_ms;
        qr.probe_stage2_kernel_sign_flip_ms = results.stats().probe_stage2_kernel_sign_flip_ms;
        qr.probe_stage2_kernel_abs_fma_ms = results.stats().probe_stage2_kernel_abs_fma_ms;
        qr.probe_stage2_kernel_tail_ms = results.stats().probe_stage2_kernel_tail_ms;
        qr.probe_stage2_kernel_reduce_ms = results.stats().probe_stage2_kernel_reduce_ms;
        qr.probe_classify_ms = results.stats().probe_classify_ms;
        qr.probe_submit_ms = results.stats().probe_submit_ms;
        qr.probe_submit_prepare_vec_only_ms =
            results.stats().probe_submit_prepare_vec_only_ms;
        qr.probe_submit_prepare_all_ms =
            results.stats().probe_submit_prepare_all_ms;
        qr.probe_submit_emit_ms = results.stats().probe_submit_emit_ms;
        qr.rerank_cpu_ms = results.stats().rerank_cpu_ms;
        qr.prefetch_submit_ms = results.stats().prefetch_submit_ms;
        qr.prefetch_wait_ms = results.stats().prefetch_wait_ms;
        qr.safein_payload_prefetch_ms = results.stats().safein_payload_prefetch_ms;
        qr.candidate_collect_ms = results.stats().candidate_collect_ms;
        qr.pool_vector_read_ms = results.stats().pool_vector_read_ms;
        qr.rerank_compute_ms = results.stats().rerank_compute_ms;
        qr.remaining_payload_fetch_ms = results.stats().remaining_payload_fetch_ms;
        qr.uring_prep_ms = results.stats().uring_prep_ms;
        qr.uring_submit_ms = results.stats().uring_submit_ms;
        qr.parse_cluster_ms = results.stats().parse_cluster_ms;
        qr.fetch_missing_ms = results.stats().fetch_missing_ms;
        qr.submit_calls = results.stats().total_submit_calls;
        qr.submit_window_flushes = results.stats().total_submit_window_flushes;
        qr.submit_window_tail_flushes =
            results.stats().total_submit_window_tail_flushes;
        qr.submit_stop_flushes = results.stats().total_submit_stop_flushes;
        qr.submit_window_requests =
            static_cast<double>(results.stats().total_submit_window_requests);
        qr.crc_decision_ms = results.stats().crc_decision_ms;
        qr.crc_buffer_ms = results.stats().crc_buffer_ms;
        qr.crc_merge_ms = results.stats().crc_merge_ms;
        qr.crc_online_ms = qr.crc_decision_ms + qr.crc_buffer_ms + qr.crc_merge_ms;
        qr.crc_would_stop = results.stats().total_crc_would_stop;
        qr.stage2_block_lookups = static_cast<double>(results.stats().total_stage2_block_lookups);
        qr.stage2_block_reuses = static_cast<double>(results.stats().total_stage2_block_reuses);
        const uint32_t probed_clusters_u32 =
            (results.stats().crc_clusters_probed > 0)
                ? results.stats().crc_clusters_probed
                : (search_cfg.nprobe - results.stats().clusters_skipped);
        qr.probed_clusters = probed_clusters_u32;
        const double probed_clusters = static_cast<double>(probed_clusters_u32);
        if (probed_clusters > 0.0) {
            qr.candidate_batches_per_cluster =
                static_cast<double>(results.stats().total_candidate_batches) /
                probed_clusters;
            qr.crc_estimates_buffered_per_cluster =
                static_cast<double>(results.stats().total_crc_estimates_buffered) /
                probed_clusters;
            qr.crc_estimates_merged_per_cluster =
                static_cast<double>(results.stats().total_crc_estimates_merged) /
                probed_clusters;
        }
        qr.early_stopped = results.stats().early_stopped;
        qr.clusters_skipped = results.stats().clusters_skipped;
        qr.total_probed = results.stats().total_probed;
        qr.safe_in = results.stats().total_safe_in;
        qr.safe_out = results.stats().total_safe_out;
        qr.uncertain = results.stats().total_uncertain;
        qr.s2_safe_in = results.stats().s2_safe_in;
        qr.s2_safe_out = results.stats().s2_safe_out;
        qr.s2_uncertain = results.stats().s2_uncertain;
        qr.duplicate_candidates = results.stats().duplicate_candidates;
        qr.deduplicated_candidates = results.stats().deduplicated_candidates;
        qr.unique_fetch_candidates = results.stats().unique_fetch_candidates;
        qr.num_candidates_buffered = results.stats().buffered_candidates;
        qr.num_candidates_reranked = results.stats().reranked_candidates;
        qr.num_safein_payload_prefetched = results.stats().total_safein_payload_prefetched;
        qr.num_remaining_payload_fetches = results.stats().total_payload_fetched;

        for (uint32_t j = 0; j < results.size(); ++j) {
            qr.predicted_dists.push_back(results[j].distance);
            if (!results[j].payload.empty() &&
                results[j].payload[0].dtype == DType::INT64) {
                qr.predicted_ids.push_back(results[j].payload[0].fixed.i64);
            }
        }

        // False SafeOut: GT IDs not found in predicted results
        if (recall_available && qi < gt_topk.size()) {
            uint32_t gt_k = std::min(search_cfg.top_k,
                static_cast<uint32_t>(gt_topk[qi].size()));
            std::unordered_set<int64_t> pred_set(
                qr.predicted_ids.begin(), qr.predicted_ids.end());
            uint32_t missing = 0;
            uint32_t hits = 0;
            for (uint32_t g = 0; g < gt_k; ++g) {
                if (pred_set.count(gt_topk[qi][g]) == 0) missing++;
                else hits++;
            }
            qr.false_safeout = missing;
            qr.false_safein_upper = (qr.safe_in > hits)
                ? (qr.safe_in - hits) : 0;
        }

        if ((qi + 1) % 100 == 0 || qi + 1 == Q) {
            Log("  %s progress: %u/%u\n", label, qi + 1, Q);
        }
    }
    Log("  %s: all %u queries complete.\n", label, Q);

    // Compute metrics
    RoundMetrics m;
    m.recall_available = recall_available;
    uint32_t recall_K[3] = {1, 5, 10};
    double recall_sum[3] = {0, 0, 0};
    if (recall_available) {
        for (uint32_t qi = 0; qi < Q; ++qi) {
            for (int r = 0; r < 3; ++r) {
                recall_sum[r] += ComputeRecallAtK(qresults[qi].predicted_ids,
                                                  gt_topk[qi], recall_K[r]);
            }
        }
        for (int r = 0; r < 3; ++r) m.recall_at[r] = recall_sum[r] / Q;
    }

    std::vector<double> query_times(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) query_times[qi] = qresults[qi].query_time_ms;
    std::sort(query_times.begin(), query_times.end());
    m.avg_query_ms = std::accumulate(query_times.begin(), query_times.end(), 0.0) / Q;
    m.p50 = Percentile(query_times, 0.50);
    m.p95 = Percentile(query_times, 0.95);
    m.p99 = Percentile(query_times, 0.99);

    double sum_probed_clusters = 0, sum_probed = 0, sum_si = 0, sum_so = 0, sum_unc = 0;
    double sum_s2_si = 0, sum_s2_so = 0, sum_s2_unc = 0;
    double sum_dup = 0, sum_dedup = 0, sum_unique_fetch = 0;
    double sum_false_so = 0, sum_false_si = 0;
    double sum_io_wait = 0, sum_total = 0;
    double sum_coarse_select = 0, sum_coarse_score = 0, sum_coarse_topn = 0;
    double sum_probe = 0, sum_probe_prepare = 0;
    double sum_probe_prepare_rotation = 0;
    double sum_probe_prepare_subtract = 0;
    double sum_probe_prepare_normalize = 0;
    double sum_probe_prepare_quantize = 0;
    double sum_probe_prepare_lut_build = 0;
    double sum_probe_prepare_quant_lut = 0;
    double sum_probe_stage1 = 0, sum_probe_stage1_estimate = 0;
    double sum_probe_stage1_mask = 0, sum_probe_stage1_iterate = 0;
    double sum_probe_stage1_classify_only = 0, sum_probe_stage2 = 0;
    double sum_probe_stage2_collect = 0;
    double sum_probe_stage2_kernel = 0;
    double sum_probe_stage2_scatter = 0;
    double sum_probe_stage2_kernel_sign_flip = 0;
    double sum_probe_stage2_kernel_abs_fma = 0;
    double sum_probe_stage2_kernel_tail = 0;
    double sum_probe_stage2_kernel_reduce = 0;
    double sum_probe_classify = 0, sum_probe_submit = 0;
    double sum_probe_submit_prepare_vec_only = 0;
    double sum_probe_submit_prepare_all = 0;
    double sum_probe_submit_emit = 0;
    double sum_rerank_cpu = 0;
    double sum_prefetch_submit = 0, sum_prefetch_wait = 0;
    double sum_safein_payload_prefetch = 0, sum_candidate_collect = 0;
    double sum_pool_vector_read = 0, sum_rerank_compute = 0;
    double sum_remaining_payload_fetch = 0;
    double sum_uring_prep = 0, sum_uring_submit = 0;
    double sum_parse_cluster = 0, sum_fetch_missing = 0;
    double sum_submit_calls = 0;
    double sum_submit_window_flushes = 0;
    double sum_submit_window_tail_flushes = 0;
    double sum_submit_stop_flushes = 0;
    double sum_submit_window_requests = 0;
    double sum_candidate_batches_per_cluster = 0;
    double sum_crc_estimates_buffered_per_cluster = 0;
    double sum_crc_estimates_merged_per_cluster = 0;
    double sum_stage2_block_lookups = 0;
    double sum_stage2_block_reuses = 0;
    double sum_crc_decision = 0;
    double sum_crc_buffer = 0;
    double sum_crc_merge = 0;
    double sum_crc_online = 0;
    double sum_crc_would_stop = 0;
    double sum_candidates_buffered = 0, sum_candidates_reranked = 0;
    double sum_safein_payload_prefetched = 0, sum_remaining_payload_fetches = 0;
    uint32_t early_count = 0;
    double sum_skipped = 0;
    for (uint32_t qi = 0; qi < Q; ++qi) {
        sum_probed_clusters += qresults[qi].probed_clusters;
        sum_probed += qresults[qi].total_probed;
        sum_si += qresults[qi].safe_in;
        sum_so += qresults[qi].safe_out;
        sum_unc += qresults[qi].uncertain;
        sum_s2_si += qresults[qi].s2_safe_in;
        sum_s2_so += qresults[qi].s2_safe_out;
        sum_s2_unc += qresults[qi].s2_uncertain;
        sum_dup += qresults[qi].duplicate_candidates;
        sum_dedup += qresults[qi].deduplicated_candidates;
        sum_unique_fetch += qresults[qi].unique_fetch_candidates;
        sum_false_so += qresults[qi].false_safeout;
        sum_false_si += qresults[qi].false_safein_upper;
        sum_io_wait += qresults[qi].io_wait_ms;
        sum_coarse_select += qresults[qi].coarse_select_ms;
        sum_coarse_score += qresults[qi].coarse_score_ms;
        sum_coarse_topn += qresults[qi].coarse_topn_ms;
        sum_probe += qresults[qi].probe_time_ms;
        sum_probe_prepare += qresults[qi].probe_prepare_ms;
        sum_probe_prepare_rotation += qresults[qi].probe_prepare_rotation_ms;
        sum_probe_prepare_subtract += qresults[qi].probe_prepare_subtract_ms;
        sum_probe_prepare_normalize += qresults[qi].probe_prepare_normalize_ms;
        sum_probe_prepare_quantize += qresults[qi].probe_prepare_quantize_ms;
        sum_probe_prepare_lut_build += qresults[qi].probe_prepare_lut_build_ms;
        sum_probe_prepare_quant_lut += qresults[qi].probe_prepare_quant_lut_ms;
        sum_probe_stage1 += qresults[qi].probe_stage1_ms;
        sum_probe_stage1_estimate += qresults[qi].probe_stage1_estimate_ms;
        sum_probe_stage1_mask += qresults[qi].probe_stage1_mask_ms;
        sum_probe_stage1_iterate += qresults[qi].probe_stage1_iterate_ms;
        sum_probe_stage1_classify_only += qresults[qi].probe_stage1_classify_only_ms;
        sum_probe_stage2 += qresults[qi].probe_stage2_ms;
        sum_probe_stage2_collect += qresults[qi].probe_stage2_collect_ms;
        sum_probe_stage2_kernel += qresults[qi].probe_stage2_kernel_ms;
        sum_probe_stage2_scatter += qresults[qi].probe_stage2_scatter_ms;
        sum_probe_stage2_kernel_sign_flip += qresults[qi].probe_stage2_kernel_sign_flip_ms;
        sum_probe_stage2_kernel_abs_fma += qresults[qi].probe_stage2_kernel_abs_fma_ms;
        sum_probe_stage2_kernel_tail += qresults[qi].probe_stage2_kernel_tail_ms;
        sum_probe_stage2_kernel_reduce += qresults[qi].probe_stage2_kernel_reduce_ms;
        sum_probe_classify += qresults[qi].probe_classify_ms;
        sum_probe_submit += qresults[qi].probe_submit_ms;
        sum_probe_submit_prepare_vec_only +=
            qresults[qi].probe_submit_prepare_vec_only_ms;
        sum_probe_submit_prepare_all +=
            qresults[qi].probe_submit_prepare_all_ms;
        sum_probe_submit_emit += qresults[qi].probe_submit_emit_ms;
        sum_rerank_cpu += qresults[qi].rerank_cpu_ms;
        sum_prefetch_submit += qresults[qi].prefetch_submit_ms;
        sum_prefetch_wait += qresults[qi].prefetch_wait_ms;
        sum_safein_payload_prefetch += qresults[qi].safein_payload_prefetch_ms;
        sum_candidate_collect += qresults[qi].candidate_collect_ms;
        sum_pool_vector_read += qresults[qi].pool_vector_read_ms;
        sum_rerank_compute += qresults[qi].rerank_compute_ms;
        sum_remaining_payload_fetch += qresults[qi].remaining_payload_fetch_ms;
        sum_uring_prep += qresults[qi].uring_prep_ms;
        sum_uring_submit += qresults[qi].uring_submit_ms;
        sum_parse_cluster += qresults[qi].parse_cluster_ms;
        sum_fetch_missing += qresults[qi].fetch_missing_ms;
        sum_submit_calls += qresults[qi].submit_calls;
        sum_submit_window_flushes += qresults[qi].submit_window_flushes;
        sum_submit_window_tail_flushes +=
            qresults[qi].submit_window_tail_flushes;
        sum_submit_stop_flushes += qresults[qi].submit_stop_flushes;
        sum_submit_window_requests += qresults[qi].submit_window_requests;
        sum_candidate_batches_per_cluster += qresults[qi].candidate_batches_per_cluster;
        sum_crc_estimates_buffered_per_cluster +=
            qresults[qi].crc_estimates_buffered_per_cluster;
        sum_crc_estimates_merged_per_cluster +=
            qresults[qi].crc_estimates_merged_per_cluster;
        sum_stage2_block_lookups += qresults[qi].stage2_block_lookups;
        sum_stage2_block_reuses += qresults[qi].stage2_block_reuses;
        sum_crc_decision += qresults[qi].crc_decision_ms;
        sum_crc_buffer += qresults[qi].crc_buffer_ms;
        sum_crc_merge += qresults[qi].crc_merge_ms;
        sum_crc_online += qresults[qi].crc_online_ms;
        sum_crc_would_stop += qresults[qi].crc_would_stop;
        sum_candidates_buffered += qresults[qi].num_candidates_buffered;
        sum_candidates_reranked += qresults[qi].num_candidates_reranked;
        sum_safein_payload_prefetched += qresults[qi].num_safein_payload_prefetched;
        sum_remaining_payload_fetches += qresults[qi].num_remaining_payload_fetches;
        sum_total += qresults[qi].query_time_ms;
        if (qresults[qi].early_stopped) {
            early_count++;
            sum_skipped += qresults[qi].clusters_skipped;
        }
    }

    m.avg_io_wait = sum_io_wait / Q;
    m.avg_cpu = (sum_total - sum_io_wait) / Q;
    m.avg_coarse_select = sum_coarse_select / Q;
    m.avg_coarse_score = sum_coarse_score / Q;
    m.avg_coarse_topn = sum_coarse_topn / Q;
    m.avg_probe = sum_probe / Q;
    m.avg_probe_prepare = sum_probe_prepare / Q;
    m.avg_probe_prepare_rotation = sum_probe_prepare_rotation / Q;
    m.avg_probe_prepare_subtract = sum_probe_prepare_subtract / Q;
    m.avg_probe_prepare_normalize = sum_probe_prepare_normalize / Q;
    m.avg_probe_prepare_quantize = sum_probe_prepare_quantize / Q;
    m.avg_probe_prepare_lut_build = sum_probe_prepare_lut_build / Q;
    m.avg_probe_prepare_quant_lut = sum_probe_prepare_quant_lut / Q;
    m.avg_probe_stage1 = sum_probe_stage1 / Q;
    m.avg_probe_stage1_estimate = sum_probe_stage1_estimate / Q;
    m.avg_probe_stage1_mask = sum_probe_stage1_mask / Q;
    m.avg_probe_stage1_iterate = sum_probe_stage1_iterate / Q;
    m.avg_probe_stage1_classify_only = sum_probe_stage1_classify_only / Q;
    m.avg_probe_stage2 = sum_probe_stage2 / Q;
    m.avg_probe_stage2_collect = sum_probe_stage2_collect / Q;
    m.avg_probe_stage2_kernel = sum_probe_stage2_kernel / Q;
    m.avg_probe_stage2_scatter = sum_probe_stage2_scatter / Q;
    m.avg_probe_stage2_kernel_sign_flip = sum_probe_stage2_kernel_sign_flip / Q;
    m.avg_probe_stage2_kernel_abs_fma = sum_probe_stage2_kernel_abs_fma / Q;
    m.avg_probe_stage2_kernel_tail = sum_probe_stage2_kernel_tail / Q;
    m.avg_probe_stage2_kernel_reduce = sum_probe_stage2_kernel_reduce / Q;
    m.avg_probe_classify = sum_probe_classify / Q;
    m.avg_probe_submit = sum_probe_submit / Q;
    m.avg_probe_submit_prepare_vec_only = sum_probe_submit_prepare_vec_only / Q;
    m.avg_probe_submit_prepare_all = sum_probe_submit_prepare_all / Q;
    m.avg_probe_submit_emit = sum_probe_submit_emit / Q;
    m.avg_rerank_cpu = sum_rerank_cpu / Q;
    m.avg_prefetch_submit = sum_prefetch_submit / Q;
    m.avg_prefetch_wait = sum_prefetch_wait / Q;
    m.avg_safein_payload_prefetch = sum_safein_payload_prefetch / Q;
    m.avg_candidate_collect = sum_candidate_collect / Q;
    m.avg_pool_vector_read = sum_pool_vector_read / Q;
    m.avg_rerank_compute = sum_rerank_compute / Q;
    m.avg_remaining_payload_fetch = sum_remaining_payload_fetch / Q;
    m.avg_uring_prep = sum_uring_prep / Q;
    m.avg_uring_submit = sum_uring_submit / Q;
    m.avg_parse_cluster = sum_parse_cluster / Q;
    m.avg_fetch_missing = sum_fetch_missing / Q;
    m.avg_submit_calls = sum_submit_calls / Q;
    m.avg_submit_window_flushes = sum_submit_window_flushes / Q;
    m.avg_submit_window_tail_flushes = sum_submit_window_tail_flushes / Q;
    m.avg_submit_stop_flushes = sum_submit_stop_flushes / Q;
    m.avg_submit_window_requests = sum_submit_window_requests / Q;
    m.avg_candidate_batches_per_cluster = sum_candidate_batches_per_cluster / Q;
    m.avg_crc_estimates_buffered_per_cluster =
        sum_crc_estimates_buffered_per_cluster / Q;
    m.avg_crc_estimates_merged_per_cluster =
        sum_crc_estimates_merged_per_cluster / Q;
    m.avg_stage2_block_lookups = sum_stage2_block_lookups / Q;
    m.avg_stage2_block_reuses = sum_stage2_block_reuses / Q;
    m.avg_crc_decision_ms = sum_crc_decision / Q;
    m.avg_crc_buffer_ms = sum_crc_buffer / Q;
    m.avg_crc_merge_ms = sum_crc_merge / Q;
    m.avg_crc_online_ms = sum_crc_online / Q;
    m.avg_crc_would_stop = sum_crc_would_stop / Q;
    m.avg_probed_clusters = sum_probed_clusters / Q;
    m.avg_probed = sum_probed / Q;
    m.avg_safe_in = sum_si / Q;
    m.avg_safe_out = sum_so / Q;
    m.avg_uncertain = sum_unc / Q;
    m.avg_s2_safe_in = sum_s2_si / Q;
    m.avg_s2_safe_out = sum_s2_so / Q;
    m.avg_s2_uncertain = sum_s2_unc / Q;
    m.avg_duplicate_candidates = sum_dup / Q;
    m.avg_deduplicated_candidates = sum_dedup / Q;
    m.avg_unique_fetch_candidates = sum_unique_fetch / Q;
    m.avg_candidates_buffered = sum_candidates_buffered / Q;
    m.avg_candidates_reranked = sum_candidates_reranked / Q;
    m.avg_safein_payload_prefetched = sum_safein_payload_prefetched / Q;
    m.avg_remaining_payload_fetches = sum_remaining_payload_fetches / Q;
    m.avg_false_safeout = sum_false_so / Q;
    m.avg_false_safein_upper = sum_false_si / Q;
    m.total_final_safein = static_cast<uint64_t>(sum_si + sum_s2_si);
    m.early_pct = static_cast<double>(early_count) / Q;
    m.avg_skipped = early_count > 0 ? sum_skipped / early_count : 0;
    m.overlap_ratio = (sum_total > 0) ? 1.0 - sum_io_wait / sum_total : 0.0;
    m.preload_time_ms = index.segment().resident_preload_time_ms();
    m.preload_bytes = static_cast<double>(index.segment().resident_preload_bytes());
    m.resident_cluster_mem_bytes =
        static_cast<double>(index.segment().resident_cluster_mem_bytes());
    m.resident_parallel_view_build_ms =
        index.segment().resident_parallel_view_build_ms();
    m.resident_parallel_view_bytes =
        static_cast<double>(index.segment().resident_parallel_view_bytes());
    m.index_cluster_clu_bytes = FileSizeBytesOrZero(index.dir() + "/cluster.clu");
    m.index_data_dat_bytes = FileSizeBytesOrZero(index.dir() + "/data.dat");
    m.index_rotation_bytes = FileSizeBytesOrZero(index.dir() + "/rotation.bin");
    m.index_rotated_centroids_bytes =
        FileSizeBytesOrZero(index.dir() + "/rotated_centroids.bin");
    m.index_total_bytes = m.index_cluster_clu_bytes + m.index_data_dat_bytes +
        m.index_rotation_bytes + m.index_rotated_centroids_bytes +
        FileSizeBytesOrZero(index.dir() + "/centroids.bin") +
        FileSizeBytesOrZero(index.dir() + "/segment.meta") +
        FileSizeBytesOrZero(index.dir() + "/build_metadata.json");
    m.query_uses_resident_clusters = search_cfg.use_resident_clusters;

    if (m.recall_available) {
        Log("  %s: recall@1=%.4f @5=%.4f @10=%.4f\n", label,
            m.recall_at[0], m.recall_at[1], m.recall_at[2]);
    } else {
        Log("  %s: recall skipped (query-only mode)\n", label);
    }
    Log("  %s: avg=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n", label,
        m.avg_query_ms, m.p50, m.p95, m.p99);
    Log("  %s: coarse_select=%.3f ms  score=%.3f ms  topn=%.3f ms\n",
        label, m.avg_coarse_select, m.avg_coarse_score, m.avg_coarse_topn);
    Log("  %s: io_wait=%.3f ms  cpu=%.3f ms  probe=%.3f ms  rerank_cpu=%.3f ms\n", label,
        m.avg_io_wait, m.avg_cpu, m.avg_probe, m.avg_rerank_cpu);
    Log("  %s: probe_prepare=%.3f ms  stage1=%.3f ms  stage2=%.3f ms  classify=%.3f ms  submit=%.3f ms\n",
        label, m.avg_probe_prepare, m.avg_probe_stage1, m.avg_probe_stage2,
        m.avg_probe_classify, m.avg_probe_submit);
    Log("  %s: submit_prepare_vec_only=%.3f ms  submit_prepare_all=%.3f ms  submit_emit=%.3f ms\n",
        label, m.avg_probe_submit_prepare_vec_only,
        m.avg_probe_submit_prepare_all, m.avg_probe_submit_emit);
    Log("  %s: stage1_estimate=%.3f ms  stage1_mask=%.3f ms  stage1_iterate=%.3f ms  stage1_classify=%.3f ms\n",
        label, m.avg_probe_stage1_estimate, m.avg_probe_stage1_mask,
        m.avg_probe_stage1_iterate, m.avg_probe_stage1_classify_only);
    Log("  %s: prefetch_submit=%.3f ms  prefetch_wait=%.3f ms  safein_payload_prefetch=%.3f ms\n",
        label, m.avg_prefetch_submit, m.avg_prefetch_wait,
        m.avg_safein_payload_prefetch);
    Log("  %s: candidate_collect=%.3f ms  pool_vector_read=%.3f ms  rerank_compute=%.3f ms  remaining_payload_fetch=%.3f ms\n",
        label, m.avg_candidate_collect, m.avg_pool_vector_read,
        m.avg_rerank_compute, m.avg_remaining_payload_fetch);
    Log("  %s: uring_prep=%.3f ms  uring_submit=%.3f ms  parse_cluster=%.3f ms  fetch_missing=%.3f ms\n",
        label, m.avg_uring_prep, m.avg_uring_submit, m.avg_parse_cluster, m.avg_fetch_missing);
    Log("  %s: submit_calls=%.1f\n", label, m.avg_submit_calls);
    Log("  %s: submit_window_flushes=%.1f  submit_window_tail_flushes=%.1f  submit_stop_flushes=%.1f  submit_window_requests=%.1f\n",
        label, m.avg_submit_window_flushes,
        m.avg_submit_window_tail_flushes,
        m.avg_submit_stop_flushes,
        m.avg_submit_window_requests);
    Log("  %s: avg_probed_clusters=%.1f\n", label, m.avg_probed_clusters);
    Log("  %s: candidate_batches_per_cluster=%.2f  crc_buffered_per_cluster=%.2f  crc_merged_per_cluster=%.2f\n",
        label, m.avg_candidate_batches_per_cluster,
        m.avg_crc_estimates_buffered_per_cluster,
        m.avg_crc_estimates_merged_per_cluster);
    Log("  %s: stage2_block_lookups=%.1f  stage2_block_reuses=%.1f\n",
        label, m.avg_stage2_block_lookups, m.avg_stage2_block_reuses);
    Log("  %s: crc_decision=%.6f ms  crc_buffer=%.6f ms  crc_merge=%.6f ms  crc_online=%.6f ms  crc_would_stop=%.1f\n",
        label, m.avg_crc_decision_ms, m.avg_crc_buffer_ms,
        m.avg_crc_merge_ms, m.avg_crc_online_ms, m.avg_crc_would_stop);
    Log("  %s: safe_in=%.1f  safe_out=%.1f  uncertain=%.1f\n", label,
        m.avg_safe_in, m.avg_safe_out, m.avg_uncertain);
    Log("  %s: s2_safe_in=%.1f  s2_safe_out=%.1f  s2_uncertain=%.1f\n", label,
        m.avg_s2_safe_in, m.avg_s2_safe_out, m.avg_s2_uncertain);
    Log("  %s: duplicate_candidates=%.1f  deduplicated=%.1f  unique_fetch=%.1f\n",
        label, m.avg_duplicate_candidates, m.avg_deduplicated_candidates,
        m.avg_unique_fetch_candidates);
    Log("  %s: buffered=%.1f  reranked=%.1f  safein_payload_prefetched=%.1f  remaining_payload_fetches=%.1f\n",
        label, m.avg_candidates_buffered, m.avg_candidates_reranked,
        m.avg_safein_payload_prefetched, m.avg_remaining_payload_fetches);
    Log("  %s: false_safeout=%.2f  false_safein_upper=%.1f  total_safein=%lu\n",
        label, m.avg_false_safeout, m.avg_false_safein_upper,
        static_cast<unsigned long>(m.total_final_safein));
    Log("  %s: early_stop=%.1f%%  avg_skipped=%.1f  overlap=%.4f\n", label,
        m.early_pct * 100, m.avg_skipped, m.overlap_ratio);

    return {std::move(qresults), m};
}

static const char* AssignmentModeName(index::AssignmentMode mode) {
    switch (mode) {
        case index::AssignmentMode::Single:
            return "single";
        case index::AssignmentMode::RedundantTop2Naive:
            return "redundant_top2_naive";
        case index::AssignmentMode::RedundantTop2Rair:
            return "redundant_top2_rair";
    }
    return "unknown";
}

static const char* ClusteringSourceName(index::ClusteringSource source) {
    switch (source) {
        case index::ClusteringSource::Auto:
            return "auto";
        case index::ClusteringSource::Precomputed:
            return "precomputed";
    }
    return "unknown";
}

static const char* CoarseBuilderName(index::CoarseBuilder builder) {
    switch (builder) {
        case index::CoarseBuilder::SuperKMeans:
            return "superkmeans";
        case index::CoarseBuilder::HierarchicalSuperKMeans:
            return "hierarchical_superkmeans";
        case index::CoarseBuilder::FaissKMeans:
            return "faiss_kmeans";
        case index::CoarseBuilder::Auto:
        default:
            return "auto";
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetStringArg(argc, argv, "--dataset",
                                         "/home/zcq/VDB/data/coco_1k");
    std::string output_base = GetStringArg(argc, argv, "--output",
                                            "/home/zcq/VDB/test/");
    int arg_nlist      = GetIntArg(argc, argv, "--nlist", 32);
    int arg_nprobe     = GetIntArg(argc, argv, "--nprobe", 32);
    int arg_topk       = GetIntArg(argc, argv, "--topk", 10);
    int arg_early_stop = GetIntArg(argc, argv, "--early-stop", 1);
    int arg_bits       = GetIntArg(argc, argv, "--bits", 1);
    int arg_block_size = GetIntArg(argc, argv, "--block-size", 64);
    float arg_c_factor = GetFloatArg(argc, argv, "--c-factor", 5.75f);
    int arg_max_iter   = GetIntArg(argc, argv, "--max-iter", 20);
    int arg_seed       = GetIntArg(argc, argv, "--seed", 42);
    int arg_page_size  = GetIntArg(argc, argv, "--page-size", 4096);
    int arg_p_for_dk   = GetIntArg(argc, argv, "--p-for-dk", 99);
    int arg_assignment_factor = GetIntArg(argc, argv, "--assignment-factor", 1);
    std::string arg_assignment_mode =
        GetStringArg(argc, argv, "--assignment-mode", "");
    std::string arg_coarse_builder =
        GetStringArg(argc, argv, "--coarse-builder", "auto");
    bool arg_build_only = HasFlag(argc, argv, "--build-only");
    float arg_rair_lambda = GetFloatArg(argc, argv, "--rair-lambda", 0.75f);
    int arg_rair_strict_second_choice =
        GetIntArg(argc, argv, "--rair-strict-second-choice", 0);
    int arg_epsilon_samples = GetIntArg(argc, argv, "--epsilon-samples", 100);
    float arg_epsilon_percentile = GetFloatArg(argc, argv, "--epsilon-percentile", 0.99f);
    int arg_io_queue_depth = GetIntArg(argc, argv, "--io-queue-depth", 64);
    int arg_cluster_submit_reserve =
        GetIntArg(argc, argv, "--cluster-submit-reserve", 8);
    std::string arg_submission_mode =
        GetStringArg(argc, argv, "--submission-mode", "shared");
    std::string arg_clu_read_mode =
        GetStringArg(argc, argv, "--clu-read-mode", "window");
    int arg_use_resident_clusters =
        GetIntArg(argc, argv, "--use-resident-clusters", 0);
    int arg_query_only = GetIntArg(argc, argv, "--query-only", 0);
    int arg_skip_gt = GetIntArg(argc, argv, "--skip-gt", 0);
    std::string arg_gt_file = GetStringArg(argc, argv, "--gt-file", "");
    int arg_fine_grained_timing =
        GetIntArg(argc, argv, "--fine-grained-timing", 0);
    int arg_submit_online = GetIntArg(argc, argv, "--submit-online", 0);
    float arg_submit_ema_alpha =
        GetFloatArg(argc, argv, "--submit-ema-alpha", 0.25f);
    int arg_address_decode_simd =
        GetIntArg(argc, argv, "--address-decode-simd", 1);
    int arg_rerank_batched_distance_simd =
        GetIntArg(argc, argv, "--rerank-batched-distance-simd", 1);
    int arg_coarse_select_simd =
        GetIntArg(argc, argv, "--coarse-select-simd", 1);
    int arg_coarse_select_phase2 =
        GetIntArg(argc, argv, "--coarse-select-phase2", 0);
    int arg_stage2_block_first =
        GetIntArg(argc, argv, "--stage2-block-first", 1);
    int arg_stage2_batch_classify =
        GetIntArg(argc, argv, "--stage2-batch-classify", 1);
    int arg_pad_to_pow2 =
        GetIntArg(argc, argv, "--pad-to-pow2", 0);
    int arg_blocked_hadamard_permuted =
        GetIntArg(argc, argv, "--blocked-hadamard-permuted", 1);

    bool arg_cold = HasFlag(argc, argv, "--cold");
    bool arg_direct_io = HasFlag(argc, argv, "--direct-io");
    bool arg_iopoll = HasFlag(argc, argv, "--iopoll");
    bool arg_sqpoll = HasFlag(argc, argv, "--sqpoll");
    const bool query_only_mode = (arg_query_only != 0);
    const bool skip_gt = query_only_mode || (arg_skip_gt != 0);
    const bool external_gt_requested = !arg_gt_file.empty();

    if (arg_submission_mode != "shared" &&
        arg_submission_mode != "isolated") {
        std::fprintf(stderr,
                     "Error: unsupported --submission-mode=%s (expected 'shared' or 'isolated')\n",
                     arg_submission_mode.c_str());
        return 1;
    }
    if (arg_clu_read_mode != "window" &&
        arg_clu_read_mode != "full_preload") {
        std::fprintf(stderr,
                     "Invalid --clu-read-mode: %s (expected window or full_preload)\n",
                     arg_clu_read_mode.c_str());
        return 1;
    }
    if (arg_use_resident_clusters != 0 &&
        arg_use_resident_clusters != 1) {
        std::fprintf(stderr,
                     "Invalid --use-resident-clusters: %d (expected 0 or 1)\n",
                     arg_use_resident_clusters);
        return 1;
    }
    if (arg_assignment_factor != 1 && arg_assignment_factor != 2) {
        std::fprintf(stderr,
                     "Invalid --assignment-factor: %d (expected 1 or 2)\n",
                     arg_assignment_factor);
        return 1;
    }
    if (!arg_assignment_mode.empty() &&
        arg_assignment_mode != "single" &&
        arg_assignment_mode != "redundant_top2_naive" &&
        arg_assignment_mode != "redundant_top2_rair") {
        std::fprintf(stderr,
                     "Invalid --assignment-mode: %s (expected single, redundant_top2_naive, or redundant_top2_rair)\n",
                     arg_assignment_mode.c_str());
        return 1;
    }
    if (arg_coarse_builder != "auto" &&
        arg_coarse_builder != "superkmeans" &&
        arg_coarse_builder != "hierarchical_superkmeans" &&
        arg_coarse_builder != "faiss_kmeans") {
        std::fprintf(stderr,
                     "Invalid --coarse-builder: %s (expected auto, superkmeans, hierarchical_superkmeans, or faiss_kmeans)\n",
                     arg_coarse_builder.c_str());
        return 1;
    }
    if (arg_rair_strict_second_choice != 0 &&
        arg_rair_strict_second_choice != 1) {
        std::fprintf(stderr,
                     "Invalid --rair-strict-second-choice: %d (expected 0 or 1)\n",
                     arg_rair_strict_second_choice);
        return 1;
    }
    if (external_gt_requested && skip_gt) {
        Log("  External GT provided but recall is disabled (--query-only/--skip-gt); GT will be loaded only if needed.\n");
    }

    // nprobe sweep: --nprobe-sweep 50,100,150,200 (mutually exclusive with --nprobe)
    std::string arg_nprobe_sweep_str = GetStringArg(argc, argv, "--nprobe-sweep", "");
    std::vector<int> nprobe_sweep_list;
    if (!arg_nprobe_sweep_str.empty()) {
        // Check mutual exclusion with explicit --nprobe
        for (int i = 1; i < argc - 1; ++i) {
            if (std::strcmp(argv[i], "--nprobe") == 0) {
                std::fprintf(stderr, "Error: --nprobe and --nprobe-sweep are mutually exclusive\n");
                return 1;
            }
        }
        // Parse comma-separated list
        std::istringstream ss(arg_nprobe_sweep_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                nprobe_sweep_list.push_back(std::stoi(token));
            }
        }
        if (nprobe_sweep_list.empty()) {
            std::fprintf(stderr, "Error: --nprobe-sweep requires at least one value\n");
            return 1;
        }
    }

    // Precomputed clustering (skip KMeans if both provided)
    std::string arg_centroids = GetStringArg(argc, argv, "--centroids", "");
    std::string arg_assignments = GetStringArg(argc, argv, "--assignments", "");
    std::string arg_save_centroids =
        GetStringArg(argc, argv, "--save-centroids", "");
    std::string arg_save_assignments =
        GetStringArg(argc, argv, "--save-assignments", "");
    std::string arg_save_secondary_assignments =
        GetStringArg(argc, argv, "--save-secondary-assignments", "");

    // Pre-built index directory (skip Phase C build if specified)
    std::string arg_index_dir = GetStringArg(argc, argv, "--index-dir", "");

    int arg_crc_enable = GetIntArg(argc, argv, "--crc", 1);
    if (arg_crc_enable != 0 && arg_crc_enable != 1) {
        std::fprintf(stderr,
                     "Invalid --crc: %d (expected 0 or 1)\n",
                     arg_crc_enable);
        return 1;
    }
    int arg_crc_load_sidecar = GetIntArg(argc, argv, "--crc-load-sidecar", 0);
    if (arg_crc_load_sidecar != 0 && arg_crc_load_sidecar != 1) {
        std::fprintf(stderr,
                     "Invalid --crc-load-sidecar: %d (expected 0 or 1)\n",
                     arg_crc_load_sidecar);
        return 1;
    }
    int arg_crc_no_break = GetIntArg(argc, argv, "--crc-no-break", 0);
    if (arg_crc_no_break != 0 && arg_crc_no_break != 1) {
        std::fprintf(stderr,
                     "Invalid --crc-no-break: %d (expected 0 or 1)\n",
                     arg_crc_no_break);
        return 1;
    }

    // CRC parameters
    float arg_crc_alpha = GetFloatArg(argc, argv, "--crc-alpha", 0.1f);
    float arg_crc_calib = GetFloatArg(argc, argv, "--crc-calib", 0.7f);
    float arg_crc_tune  = GetFloatArg(argc, argv, "--crc-tune", 0.2f);
    std::string arg_crc_solver_str =
        GetStringArg(argc, argv, "--crc-solver", "brent");
    CrcCalibrator::Solver arg_crc_solver = CrcCalibrator::Solver::Brent;
    if (!ParseCrcSolverArg(arg_crc_solver_str, &arg_crc_solver)) {
        std::fprintf(stderr,
                     "Invalid --crc-solver: %s (expected brent or discrete_threshold)\n",
                     arg_crc_solver_str.c_str());
        return 1;
    }
    bool arg_crc_use_stratified_split = false;
    if (!ParseCrcSplitModeArg(
            GetStringArg(argc, argv, "--crc-split-mode", "random"),
            &arg_crc_use_stratified_split)) {
        std::fprintf(stderr,
                     "Invalid --crc-split-mode: %s (expected random or stratified)\n",
                     GetStringArg(argc, argv, "--crc-split-mode", "random").c_str());
        return 1;
    }
    uint32_t arg_crc_split_buckets = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--crc-split-buckets", 4));
    if (arg_crc_split_buckets < 2) {
        arg_crc_split_buckets = 2;
    }
    auto arg_crc_robust_seeds =
        ParseUint64List(GetStringArg(argc, argv, "--crc-robust-seeds", ""));

    float arg_override_eps_ip = GetFloatArg(argc, argv, "--override-eps-ip", -1.0f);
    float arg_override_d_k = GetFloatArg(argc, argv, "--override-d-k", -1.0f);
    int arg_crc_samples = GetIntArg(argc, argv, "--crc-samples", 1000);

    std::string ds_name = DatasetName(data_dir);
    std::string ts = Timestamp();
    std::string resolved_assignment_mode = arg_assignment_mode;
    if (resolved_assignment_mode.empty()) {
        resolved_assignment_mode =
            (arg_assignment_factor == 2) ? "redundant_top2_naive" : "single";
    }

    Log("=== VDB E2E Benchmark ===\n");
    Log("Dataset: %s\n", data_dir.c_str());
    Log("CRC mode: %s\n", arg_crc_enable ? "ON" : "OFF");
    if (arg_crc_enable) {
        Log("CRC alpha: %.2f\n", arg_crc_alpha);
        Log("CRC solver: %s\n", CrcCalibrator::SolverName(arg_crc_solver));
        Log("CRC samples: %d\n", arg_crc_samples);
        Log("CRC split mode: %s\n",
            arg_crc_use_stratified_split ? "stratified" : "random");
        if (arg_crc_use_stratified_split) {
            Log("CRC split buckets: %u\n", arg_crc_split_buckets);
        }
        if (!arg_crc_robust_seeds.empty()) {
            std::ostringstream ss;
            for (size_t i = 0; i < arg_crc_robust_seeds.size(); ++i) {
                if (i) ss << ",";
                ss << arg_crc_robust_seeds[i];
            }
            Log("CRC robust seeds: %s\n", ss.str().c_str());
        }
        Log("CRC no-break: %s\n", arg_crc_no_break ? "ON" : "OFF");
    }
    // ================================================================
    // Phase A: Load Data
    // ================================================================
    Log("\n[Phase A] Loading data...\n");

    auto img_emb_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load image_embeddings: %s\n",
                     img_emb_or.status().ToString().c_str());
        return 1;
    }
    auto& img_emb = img_emb_or.value();

    auto img_ids_or = io::LoadNpyInt64(data_dir + "/image_ids.npy");
    if (!img_ids_or.ok()) {
        std::fprintf(stderr, "Failed to load image_ids: %s\n",
                     img_ids_or.status().ToString().c_str());
        return 1;
    }
    auto& img_ids = img_ids_or.value();

    auto qry_emb_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load query_embeddings: %s\n",
                     qry_emb_or.status().ToString().c_str());
        return 1;
    }
    auto& qry_emb = qry_emb_or.value();

    auto qry_ids_or = io::LoadNpyInt64(data_dir + "/query_ids.npy");
    if (!qry_ids_or.ok()) {
        std::fprintf(stderr, "Failed to load query_ids: %s\n",
                     qry_ids_or.status().ToString().c_str());
        return 1;
    }
    auto& qry_ids = qry_ids_or.value();

    uint32_t N = img_emb.rows;
    uint32_t Q_total = qry_emb.rows;
    Dim dim = static_cast<Dim>(img_emb.cols);

    int q_limit = GetIntArg(argc, argv, "--queries", 0);
    uint32_t Q = (q_limit > 0 && static_cast<uint32_t>(q_limit) < Q_total)
                     ? static_cast<uint32_t>(q_limit) : Q_total;

    Log("  Images: %u, Queries: %u/%u, Dim: %u\n", N, Q, Q_total, dim);

    // Load metadata
    std::unordered_map<int64_t, std::string> id_to_caption;
    auto s = io::ReadJsonlLines(data_dir + "/metadata.jsonl",
        [&](uint32_t, std::string_view line) {
            int64_t id = ParseImageId(line);
            if (id >= 0) {
                id_to_caption[id] = ParseCaption(line);
            }
        });
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to load metadata: %s\n", s.ToString().c_str());
        return 1;
    }
    Log("  Metadata entries: %zu\n", id_to_caption.size());

    // ================================================================
    // Phase B: Brute-Force Ground Truth
    // ================================================================
    const uint32_t GT_K = static_cast<uint32_t>(arg_topk);
    std::vector<std::vector<int64_t>> gt_topk(Q);
    std::vector<std::vector<float>> gt_dists(Q);
    std::string gt_mode = skip_gt ? "skipped" : "computed";
    std::string gt_source = "";
    double brute_force_time_ms = 0.0;
    if (!skip_gt) {
        if (external_gt_requested) {
            Log("\n[Phase B] Loading external ground truth from %s (top-%u)...\n",
                arg_gt_file.c_str(), GT_K);
            auto gt_or = LoadExternalGroundTruth(arg_gt_file, Q, GT_K);
            if (!gt_or.ok()) {
                std::fprintf(stderr, "Failed to load external ground truth: %s\n",
                             gt_or.status().ToString().c_str());
                return 1;
            }
            gt_topk = std::move(gt_or.value());
            gt_mode = "external";
            gt_source = arg_gt_file;
        } else {
        Log("\n[Phase B] Computing brute-force ground truth (top-%u)...\n", GT_K);
        auto t_bf_start = std::chrono::steady_clock::now();
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* qvec = qry_emb.data.data() + static_cast<size_t>(qi) * dim;

            std::vector<std::pair<float, int64_t>> dists(N);
            for (uint32_t j = 0; j < N; ++j) {
                const float* ivec = img_emb.data.data() + static_cast<size_t>(j) * dim;
                dists[j] = {L2Sqr(qvec, ivec, dim), img_ids.data[j]};
            }

            std::partial_sort(dists.begin(), dists.begin() + GT_K, dists.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            gt_topk[qi].resize(GT_K);
            gt_dists[qi].resize(GT_K);
            for (uint32_t k = 0; k < GT_K; ++k) {
                gt_topk[qi][k] = dists[k].second;
                gt_dists[qi][k] = dists[k].first;
            }

            if ((qi + 1) % 100 == 0 || qi + 1 == Q) {
                double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_bf_start).count();
                Log("  GT progress: %u/%u (%.0f ms)\n", qi + 1, Q, elapsed);
            }
        }
        brute_force_time_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_bf_start).count();
        Log("  Brute-force time: %.1f ms\n", brute_force_time_ms);
        gt_mode = "computed";
        gt_source = "brute_force";
        }
    } else {
        Log("\n[Phase B] Skipped brute-force ground truth (%s)\n",
            query_only_mode ? "--query-only enabled" : "--skip-gt enabled");
        if (external_gt_requested) {
            gt_mode = "external_unused";
            gt_source = arg_gt_file;
        }
    }

    // ================================================================
    // Phase C: Build Index (skipped when --index-dir is provided)
    // ================================================================
    std::string output_dir = output_base + "/" + ds_name + "_" + ts;
    std::string index_dir = ResolveBenchIndexDir(
        ds_name, output_dir, arg_nlist, arg_bits,
        arg_epsilon_percentile, resolved_assignment_mode,
        arg_rair_lambda, arg_index_dir);
    const std::string index_source =
        arg_index_dir.empty() ? "rebuilt" : "reused";
    const std::string resolved_index_dir = NormalizePath(index_dir);
    double training_time_ms = 0.0;

    if (arg_index_dir.empty()) {
        fs::create_directories(index_dir);
        Log("\n[Phase C] Building index -> %s\n", index_dir.c_str());

        IvfBuilderConfig cfg;
        cfg.nlist = static_cast<uint32_t>(arg_nlist);
        if (arg_coarse_builder == "superkmeans") {
            cfg.coarse_builder = index::CoarseBuilder::SuperKMeans;
        } else if (arg_coarse_builder == "hierarchical_superkmeans") {
            cfg.coarse_builder = index::CoarseBuilder::HierarchicalSuperKMeans;
        } else if (arg_coarse_builder == "faiss_kmeans") {
            cfg.coarse_builder = index::CoarseBuilder::FaissKMeans;
        } else {
            cfg.coarse_builder = index::CoarseBuilder::Auto;
        }
        cfg.max_iterations = static_cast<uint32_t>(arg_max_iter);
        cfg.seed = static_cast<uint64_t>(arg_seed);
        cfg.metric = "cosine";
        cfg.faiss_train_size = 100000;
        cfg.faiss_niter = static_cast<uint32_t>(arg_max_iter == 20 ? 0 : arg_max_iter);
        cfg.faiss_nredo = 1;
        cfg.rabitq = {static_cast<uint8_t>(arg_bits),
                      static_cast<uint32_t>(arg_block_size), arg_c_factor};
        cfg.nprobe = static_cast<uint32_t>(std::max(1, arg_nprobe));
        cfg.calibration_samples = std::min(static_cast<uint32_t>(arg_crc_samples), N);
        cfg.epsilon_samples = static_cast<uint32_t>(arg_epsilon_samples);
        cfg.epsilon_percentile = arg_epsilon_percentile;
        cfg.assignment_factor = static_cast<uint32_t>(arg_assignment_factor);
        if (arg_assignment_mode == "redundant_top2_naive") {
            cfg.assignment_mode = index::AssignmentMode::RedundantTop2Naive;
            cfg.assignment_factor = 2;
        } else if (arg_assignment_mode == "redundant_top2_rair") {
            cfg.assignment_mode = index::AssignmentMode::RedundantTop2Rair;
            cfg.assignment_factor = 2;
        } else {
            cfg.assignment_mode = index::AssignmentMode::Single;
        }
        cfg.rair_lambda = arg_rair_lambda;
        cfg.rair_strict_second_choice = (arg_rair_strict_second_choice != 0);
        cfg.calibration_topk = GT_K;
        cfg.calibration_percentile = static_cast<float>(arg_p_for_dk) / 100.0f;
        cfg.page_size = static_cast<uint32_t>(arg_page_size);
        cfg.calibration_queries = qry_emb.data.data();
        cfg.num_calibration_queries = Q;
        cfg.centroids_path = arg_centroids;
        cfg.assignments_path = arg_assignments;
        cfg.save_centroids_path = arg_save_centroids;
        cfg.save_assignments_path = arg_save_assignments;
        cfg.save_secondary_assignments_path = arg_save_secondary_assignments;
        cfg.crc_top_k = GT_K;  // always precompute CRC scores at build time
        cfg.pad_non_power_of_two_to_pow2 = (arg_pad_to_pow2 != 0);
        cfg.use_blocked_hadamard_permuted =
            (arg_blocked_hadamard_permuted != 0);
        cfg.payload_schemas = {
            {0, "id",      DType::INT64,  false},
            {1, "caption", DType::STRING, false},
        };

        PayloadFn payload_fn = [&](uint32_t idx) -> std::vector<Datum> {
            int64_t id = img_ids.data[idx];
            auto it = id_to_caption.find(id);
            std::string cap = (it != id_to_caption.end()) ? it->second : "";
            return {Datum::Int64(id), Datum::String(std::move(cap))};
        };

        IvfBuilder builder(cfg);
        builder.SetProgressCallback([](uint32_t cluster, uint32_t total) {
            if ((cluster + 1) % 8 == 0 || cluster + 1 == total) {
                Log("  Build progress: cluster %u/%u\n", cluster + 1, total);
            }
        });

        auto t_build_start = std::chrono::steady_clock::now();
        Log("  Starting Build...\n");
        s = builder.Build(img_emb.data.data(), N, dim, index_dir, payload_fn);
        training_time_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_build_start).count();

        if (!s.ok()) {
            std::fprintf(stderr, "Build failed: %s\n", s.ToString().c_str());
            return 1;
        }
        Log("  Build time: %.1f ms\n", training_time_ms);
    } else {
        Log("\n[Phase C] Skipped (using pre-built index: %s)\n", index_dir.c_str());
    }

    if (arg_build_only) {
        Log("\n[Phase C.5/D] Skipped (--build-only enabled)\n");
        Log("  Index source: %s  resolved_index_dir=%s\n",
            index_source.c_str(), resolved_index_dir.c_str());
        return 0;
    }

    // ================================================================
    // Phase C.5: Open Index + CRC Calibration (from disk)
    // ================================================================
    IvfIndex index;
    s = index.Open(index_dir, arg_direct_io);
    if (!s.ok()) {
        std::fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
        return 1;
    }

    Log("  Index params: eps_ip=%.6f  d_k=%.6f\n",
        index.conann().epsilon(), index.conann().d_k());
    Log("  Index dims: logical=%u  effective=%u  padding=%s  rotation=%s\n",
        index.logical_dim(), index.effective_dim(),
        index.padding_mode().c_str(), index.rotation_mode().c_str());
    Log("  Index source: %s  resolved_index_dir=%s\n",
        index_source.c_str(), resolved_index_dir.c_str());
    if (arg_override_eps_ip >= 0.0f || arg_override_d_k >= 0.0f) {
        const float eps = (arg_override_eps_ip >= 0.0f)
            ? arg_override_eps_ip
            : index.conann().epsilon();
        const float dk = (arg_override_d_k >= 0.0f)
            ? arg_override_d_k
            : index.conann().d_k();
        index.OverrideConANN(eps, dk);
        Log("  Override params applied: eps_ip=%.6f  d_k=%.6f\n",
            index.conann().epsilon(), index.conann().d_k());
    }

    CalibrationResults calib_results;
    std::vector<std::tuple<uint64_t, CalibrationResults, CrcCalibrator::EvalResults>> crc_trials;
    std::vector<uint64_t> crc_seed_plan;
    uint64_t crc_selected_seed = static_cast<uint64_t>(arg_seed);
    double crc_actual_fnr_mean = 0.0;
    double crc_actual_fnr_std = 0.0;
    double crc_avg_probed_mean = 0.0;
    double crc_avg_probed_std = 0.0;
    double crc_recall10_mean = 0.0;
    double crc_recall10_std = 0.0;
    bool crc_loaded = false;
    bool crc_sidecar_loaded = false;
    CrcCalibrator::EvalResults crc_eval_results;
    bool crc_eval_available = false;
    std::string crc_params_source = "disabled";
    if ((arg_use_resident_clusters != 0) ||
        arg_clu_read_mode == "full_preload") {
        if (!index.segment().resident_preload_enabled()) {
            Log("\n[Phase C.5] Prewarming resident/full-preload clusters...\n");
            auto preload_status = index.segment().PreloadAllClusters();
            if (!preload_status.ok()) {
                std::fprintf(stderr,
                             "Failed to preload resident clusters before CRC preparation: %s\n",
                             preload_status.ToString().c_str());
                return 1;
            }
            Log("  Prewarm done: preload_time=%.3f ms  preload_bytes=%llu\n",
                index.segment().resident_preload_time_ms(),
                static_cast<unsigned long long>(
                    index.segment().resident_preload_bytes()));
        }
    }
    if (arg_crc_enable) {
        Log("\n[Phase C.5] Preparing CRC parameters...\n");
        auto t_crc_start = std::chrono::steady_clock::now();
        if (arg_crc_load_sidecar != 0) {
            const std::string params_path = CrcParamsSidecarPath(index_dir);
            s = ReadCalibrationResults(params_path, &calib_results);
            if (!s.ok()) {
                std::fprintf(stderr,
                             "CRC legacy sidecar requested but failed to load %s: %s\n",
                             params_path.c_str(), s.ToString().c_str());
                return 1;
            }
            crc_loaded = true;
            crc_sidecar_loaded = true;
            crc_params_source = "sidecar_legacy";
            double crc_time_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_crc_start).count();
            Log("  CRC parameters loaded from legacy sidecar (%.1f ms, sidecar=%s)\n",
                crc_time_ms, params_path.c_str());
            Log("  d_min=%.6f  d_max=%.6f  lamhat=%.6f\n",
                calib_results.d_min, calib_results.d_max, calib_results.lamhat);
            Log("  kreg=%u  reg_lambda=%.6f\n",
                calib_results.kreg, calib_results.reg_lambda);
        } else {
            std::string scores_path = index_dir + "/crc_scores.bin";
            std::vector<QueryScores> crc_scores;
            uint32_t scores_nlist = 0, scores_top_k = 0;
            s = CrcCalibrator::ReadScores(scores_path, crc_scores,
                                          scores_nlist, scores_top_k);
            if (!s.ok()) {
                std::fprintf(stderr,
                             "CRC enabled but failed to load precomputed scores from %s: %s\n",
                             scores_path.c_str(), s.ToString().c_str());
                return 1;
            }

            Log("  Loaded %zu queries from %s (nlist=%u, top_k=%u)\n",
                crc_scores.size(), scores_path.c_str(), scores_nlist, scores_top_k);

            crc_seed_plan = arg_crc_robust_seeds.empty()
                ? std::vector<uint64_t>{static_cast<uint64_t>(arg_seed)}
                : arg_crc_robust_seeds;

            double sum_fnr = 0.0;
            double sum_probed = 0.0;
            double sum_recall10 = 0.0;

            for (uint64_t seed : crc_seed_plan) {
                CrcCalibrator::Config crc_cfg;
                crc_cfg.alpha = arg_crc_alpha;
                crc_cfg.top_k = scores_top_k;
                crc_cfg.calib_ratio = arg_crc_calib;
                crc_cfg.tune_ratio = arg_crc_tune;
                crc_cfg.seed = seed;
                crc_cfg.solver = arg_crc_solver;
                crc_cfg.use_stratified_split = arg_crc_use_stratified_split;
                crc_cfg.split_buckets = arg_crc_split_buckets;

                Log("  CRC calibrating with seed=%llu...\n",
                    static_cast<unsigned long long>(seed));
                auto t_seed_start = std::chrono::steady_clock::now();
                auto [cal, eval] = CrcCalibrator::Calibrate(
                    crc_cfg, crc_scores, scores_nlist);
                double seed_time_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_seed_start).count();
                crc_trials.emplace_back(seed, cal, eval);
                crc_loaded = true;
                crc_eval_available = crc_eval_available || eval.test_size > 0;

                sum_fnr += eval.actual_fnr;
                sum_probed += eval.avg_probed;
                sum_recall10 += eval.recall_at_10;

                Log("  CRC seed=%llu result (%.1f ms): lamhat=%.6f  FNR=%.4f  avg_probed=%.1f  recall@10=%.4f  test=%u\n",
                    static_cast<unsigned long long>(seed),
                    seed_time_ms, cal.lamhat, eval.actual_fnr,
                    eval.avg_probed, eval.recall_at_10, eval.test_size);
            }

            crc_actual_fnr_mean = sum_fnr / crc_seed_plan.size();
            crc_avg_probed_mean = sum_probed / crc_seed_plan.size();
            crc_recall10_mean = sum_recall10 / crc_seed_plan.size();

            if (crc_trials.size() == 1) {
                crc_eval_results = std::get<2>(crc_trials[0]);
                calib_results = std::get<1>(crc_trials[0]);
                crc_selected_seed = std::get<0>(crc_trials[0]);
                crc_params_source = "runtime_calibrated";
            } else {
                // pick by best recall@10 first, then minimum avg_probed.
                size_t best_idx = 0;
                for (size_t i = 1; i < crc_trials.size(); ++i) {
                    const auto& cur_eval = std::get<2>(crc_trials[i]);
                    const auto& best_eval = std::get<2>(crc_trials[best_idx]);
                    if (cur_eval.recall_at_10 > best_eval.recall_at_10 + 1e-9f ||
                        (std::fabs(cur_eval.recall_at_10 - best_eval.recall_at_10) <= 1e-9f &&
                         cur_eval.avg_probed < best_eval.avg_probed)) {
                        best_idx = i;
                    }
                }
                crc_eval_results = std::get<2>(crc_trials[best_idx]);
                calib_results = std::get<1>(crc_trials[best_idx]);
                crc_selected_seed = std::get<0>(crc_trials[best_idx]);
                crc_params_source = "runtime_calibrated_robust";
                uint64_t best_seed = std::get<0>(crc_trials[best_idx]);
                Log("  CRC robust selection picks seed=%llu as final params\n",
                    static_cast<unsigned long long>(best_seed));
            }

            if (!crc_seed_plan.empty()) {
                double ss_fnr = 0.0;
                double ss_probed = 0.0;
                double ss_recall10 = 0.0;
                for (const auto& rec : crc_trials) {
                    const auto& e = std::get<2>(rec);
                    ss_fnr += (e.actual_fnr - crc_actual_fnr_mean) *
                              (e.actual_fnr - crc_actual_fnr_mean);
                    ss_probed += (e.avg_probed - crc_avg_probed_mean) *
                                 (e.avg_probed - crc_avg_probed_mean);
                    ss_recall10 +=
                        (e.recall_at_10 - crc_recall10_mean) *
                        (e.recall_at_10 - crc_recall10_mean);
                }
                double inv = 1.0 / static_cast<double>(crc_trials.size());
                crc_actual_fnr_std = std::sqrt(ss_fnr * inv);
                crc_avg_probed_std = std::sqrt(ss_probed * inv);
                crc_recall10_std = std::sqrt(ss_recall10 * inv);
            }

            double crc_time_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_crc_start).count();
            Log("  CRC parameters runtime-calibrated (%.1f ms, source=%s)\n",
                crc_time_ms, crc_params_source.c_str());
            Log("  Robust aggregation: seed_count=%zu, actual_fnr_mean=%.4f±%.4f, avg_probed_mean=%.1f±%.1f, recall@10_mean=%.4f±%.4f\n",
                crc_seed_plan.size(), crc_actual_fnr_mean, crc_actual_fnr_std,
                crc_avg_probed_mean, crc_avg_probed_std,
                crc_recall10_mean, crc_recall10_std);
            Log("  d_min=%.6f  d_max=%.6f  lamhat=%.6f\n",
                calib_results.d_min, calib_results.d_max, calib_results.lamhat);
            Log("  kreg=%u  reg_lambda=%.6f\n",
                calib_results.kreg, calib_results.reg_lambda);
            Log("  eval: FNR=%.4f  avg_probed=%.1f  test_size=%u\n",
                crc_eval_results.actual_fnr, crc_eval_results.avg_probed, crc_eval_results.test_size);
            Log("  eval: recall@1=%.4f  @5=%.4f  @10=%.4f\n",
                crc_eval_results.recall_at_1, crc_eval_results.recall_at_5, crc_eval_results.recall_at_10);
            Log("  solver: %s  candidates=%u  objective_evals=%u  profile_build=%.3f ms  solver_ms=%.3f ms\n",
                CrcCalibrator::SolverName(crc_eval_results.solver_used),
                crc_eval_results.candidate_count, crc_eval_results.objective_evals,
                crc_eval_results.profile_build_ms, crc_eval_results.solver_ms);
        }
    } else {
        Log("\n[Phase C.5] Skipped CRC parameter preparation (--crc 0)\n");
    }

    // ================================================================
    // Phase D: Query
    // ================================================================
    IoUringReader cluster_reader;
    auto init_status = cluster_reader.Init(
        static_cast<uint32_t>(arg_io_queue_depth), 4096, arg_iopoll, arg_sqpoll);
    if (!init_status.ok()) {
        std::fprintf(stderr, "IoUring init failed: %s\n", init_status.ToString().c_str());
        return 1;
    }

    std::unique_ptr<IoUringReader> data_reader;
    if (arg_submission_mode == "isolated") {
        data_reader = std::make_unique<IoUringReader>();
        auto data_status = data_reader->Init(
            static_cast<uint32_t>(arg_io_queue_depth), 4096, arg_iopoll, arg_sqpoll);
        if (!data_status.ok()) {
            std::fprintf(stderr, "Data IoUring init failed: %s\n",
                         data_status.ToString().c_str());
            return 1;
        }
    }

    // Register file descriptors for IOSQE_FIXED_FILE optimization
    if (arg_submission_mode == "isolated") {
        int clu_fd = index.segment().clu_fd();
        auto reg_status = cluster_reader.RegisterFiles(&clu_fd, 1);
        if (!reg_status.ok()) {
            Log("  Warning: RegisterFiles(cluster) failed: %s (continuing without)\n",
                reg_status.ToString().c_str());
        }
        int dat_fd = index.segment().data_reader().fd();
        auto data_reg_status = data_reader->RegisterFiles(&dat_fd, 1);
        if (!data_reg_status.ok()) {
            Log("  Warning: RegisterFiles(data) failed: %s (continuing without)\n",
                data_reg_status.ToString().c_str());
        }
    } else {
        int fds[2] = {index.segment().clu_fd(),
                      index.segment().data_reader().fd()};
        auto reg_status = cluster_reader.RegisterFiles(fds, 2);
        if (!reg_status.ok()) {
            Log("  Warning: RegisterFiles failed: %s (continuing without)\n",
                reg_status.ToString().c_str());
        }
    }

    // Prepare query IDs as a flat vector for RunQueryRound
    std::vector<int64_t> qry_ids_vec(qry_ids.data.begin(),
                                      qry_ids.data.begin() + Q);

    // Base search config (shared across single run and sweep)
    SearchConfig search_cfg;
    search_cfg.top_k = GT_K;
    search_cfg.nprobe = static_cast<uint32_t>(arg_nprobe);
    search_cfg.early_stop = (arg_early_stop != 0);
    search_cfg.prefetch_depth = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--prefetch-depth", 16));
    search_cfg.io_queue_depth = static_cast<uint32_t>(arg_io_queue_depth);
    search_cfg.cluster_submit_reserve = static_cast<uint32_t>(
        std::max(arg_cluster_submit_reserve, 1));
    search_cfg.use_sqpoll = arg_sqpoll;
    search_cfg.submission_mode =
        (arg_submission_mode == "isolated")
            ? SubmissionMode::Isolated
            : SubmissionMode::Shared;
    search_cfg.clu_read_mode =
        (arg_clu_read_mode == "full_preload")
            ? CluReadMode::FullPreload
            : CluReadMode::Window;
    search_cfg.use_resident_clusters = (arg_use_resident_clusters != 0);
    search_cfg.enable_fine_grained_timing = (arg_fine_grained_timing != 0);
    search_cfg.crc_params = crc_loaded ? &calib_results : nullptr;
    search_cfg.crc_no_break = (arg_crc_no_break != 0);
    search_cfg.initial_prefetch = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--initial-prefetch", 16));
    search_cfg.refill_threshold = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--refill-threshold", 4));
    search_cfg.refill_count = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--refill-count", 8));
    search_cfg.submit_batch_size = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--submit-batch", 32));
    search_cfg.enable_online_submit_tuning = (arg_submit_online != 0);
    search_cfg.submit_ema_alpha = arg_submit_ema_alpha;
    search_cfg.enable_address_decode_simd = (arg_address_decode_simd != 0);
    search_cfg.enable_rerank_batched_distance_simd =
        (arg_rerank_batched_distance_simd != 0);
    search_cfg.enable_coarse_select_simd = (arg_coarse_select_simd != 0);
    search_cfg.enable_coarse_select_phase2 = (arg_coarse_select_phase2 != 0);
    search_cfg.enable_stage2_collect_block_first = (arg_stage2_block_first != 0);
    search_cfg.enable_stage2_scatter_batch_classify = (arg_stage2_batch_classify != 0);

    if (search_cfg.use_resident_clusters) {
        if (search_cfg.clu_read_mode != CluReadMode::FullPreload) {
            std::fprintf(stderr,
                         "Resident cluster mode requires --clu-read-mode full_preload\n");
            return 1;
        }
        if (!index.segment().resident_preload_enabled()) {
            auto preload_status = index.segment().PreloadAllClusters();
            if (!preload_status.ok()) {
                std::fprintf(stderr,
                             "Failed to preload resident clusters before benchmark: %s\n",
                             preload_status.ToString().c_str());
                return 1;
            }
        }
    }

    // ================================================================
    // nprobe sweep mode (tasks 3.2–3.4)
    // ================================================================
    if (!nprobe_sweep_list.empty()) {
        Log("\n[Sweep] nprobe sweep: %s\n", arg_nprobe_sweep_str.c_str());

        // Prepare CSV
        fs::create_directories(output_dir);
        std::string sweep_csv_path = output_dir + "/nprobe_sweep.csv";
        bool write_header = !fs::exists(sweep_csv_path);
        std::ofstream sweep_csv(sweep_csv_path, std::ios::app);
        if (write_header) {
            sweep_csv << "nprobe,recall@1,recall@10,avg_ms,p50_ms,p99_ms,"
                         "avg_probe_ms,avg_io_wait_ms,avg_uring_submit_ms,"
                         "avg_submit_calls,avg_safe_out_rate,io_queue_depth,"
                         "sqpoll_enabled,cluster_submit_reserve,submission_mode,"
                         "clu_read_mode,assignment_mode,assignment_factor,clustering_source,"
                         "rair_lambda,rair_strict_second_choice,"
                         "avg_duplicate_candidates,avg_deduplicated_candidates,"
                         "avg_unique_fetch_candidates,index_source,resolved_index_dir,"
                         "loaded_eps_ip,loaded_d_k,preload_time_ms,preload_bytes,"
                         "resident_cluster_mem_bytes,query_uses_resident_clusters,"
                         "logical_dim,effective_dim,padding_mode,rotation_mode,"
                         "avg_prepare_rotation_ms,avg_prepare_quant_lut_ms,index_total_bytes\n";
        }

        // Warmup query count for sweep (100 or all if Q < 100)
        uint32_t sw_warmup = std::min(100u, Q);
        uint32_t sw_measure = Q;

        for (int np : nprobe_sweep_list) {
            Log("\n[Sweep] nprobe=%d ...\n", np);
            search_cfg.nprobe = static_cast<uint32_t>(np);

            // Warmup round (discard results)
            {
                auto [wq, wm] = RunQueryRound(
                    "SWEEP-WARMUP", index, cluster_reader, data_reader.get(), search_cfg,
                    qry_emb.data.data(), sw_warmup, dim, qry_ids_vec, gt_topk, gt_dists,
                    !skip_gt);
                (void)wq; (void)wm;
            }

            // Measurement round
            auto [sq, sm] = RunQueryRound(
                "SWEEP-MEASURE", index, cluster_reader, data_reader.get(), search_cfg,
                qry_emb.data.data(), sw_measure, dim, qry_ids_vec, gt_topk, gt_dists,
                !skip_gt);

            double safe_out_rate = (sm.avg_probed > 0)
                ? sm.avg_safe_out / sm.avg_probed * 100.0 : 0.0;

            Log("[sweep] nprobe=%d  recall@10=%.4f  avg=%.3fms  probe=%.3fms  safe_out_rate=%.1f%%\n",
                np, sm.recall_at[2], sm.avg_query_ms, sm.avg_probe, safe_out_rate);

            sweep_csv << np << "," << sm.recall_at[0] << "," << sm.recall_at[2] << ","
                      << sm.avg_query_ms << "," << sm.p50 << "," << sm.p99 << ","
                      << sm.avg_probe << "," << sm.avg_io_wait << ","
                      << sm.avg_uring_submit << "," << sm.avg_submit_calls << ","
                      << safe_out_rate << "," << search_cfg.io_queue_depth << ","
                      << (cluster_reader.sqpoll_enabled() ? 1 : 0) << ","
                      << search_cfg.cluster_submit_reserve << ","
                      << arg_submission_mode << ","
                      << arg_clu_read_mode << ","
                      << AssignmentModeName(index.assignment_mode()) << ","
                      << index.assignment_factor() << ","
                      << ClusteringSourceName(index.clustering_source()) << ","
                      << index.rair_lambda() << ","
                      << (index.rair_strict_second_choice() ? 1 : 0) << ","
                      << sm.avg_duplicate_candidates << ","
                      << sm.avg_deduplicated_candidates << ","
                      << sm.avg_unique_fetch_candidates << ","
                      << index_source << ","
                      << "\"" << resolved_index_dir << "\"" << ","
                      << index.conann().epsilon() << ","
                      << index.conann().d_k() << ","
                      << sm.preload_time_ms << ","
                      << sm.preload_bytes << ","
                      << sm.resident_cluster_mem_bytes << ","
                      << (sm.query_uses_resident_clusters ? 1 : 0) << ","
                      << index.logical_dim() << ","
                      << index.effective_dim() << ","
                      << "\"" << index.padding_mode() << "\"" << ","
                      << "\"" << index.rotation_mode() << "\"" << ","
                      << sm.avg_probe_prepare_rotation << ","
                      << sm.avg_probe_prepare_quant_lut << ","
                      << sm.index_total_bytes << "\n";
            sweep_csv.flush();
        }

        Log("\n[Sweep] Results written to %s\n", sweep_csv_path.c_str());
        return 0;
    }

    // Single round: CRC early stop + dynamic SafeOut
    auto [qresults, metrics] = RunQueryRound(
        arg_cold ? "HOT" : "CRC", index, cluster_reader, data_reader.get(), search_cfg,
        qry_emb.data.data(), Q, dim, qry_ids_vec, gt_topk, gt_dists, !skip_gt);

    // ================================================================
    // Phase D2: Cold-read round (optional)
    // ================================================================
    RoundMetrics cold_metrics{};
    if (arg_cold) {
        int clu_fd = index.segment().clu_fd();
        int dat_fd = index.segment().data_reader().fd();

        off_t clu_size = lseek(clu_fd, 0, SEEK_END);
        off_t dat_size = lseek(dat_fd, 0, SEEK_END);

        Log("\n[Cold] Evicting page cache via posix_fadvise(FADV_DONTNEED)...\n");
        Log("  .clu fd=%d size=%ld bytes\n", clu_fd, static_cast<long>(clu_size));
        Log("  .dat fd=%d size=%ld bytes\n", dat_fd, static_cast<long>(dat_size));

        int r1 = posix_fadvise(clu_fd, 0, clu_size, POSIX_FADV_DONTNEED);
        int r2 = posix_fadvise(dat_fd, 0, dat_size, POSIX_FADV_DONTNEED);
        if (r1 != 0 || r2 != 0) {
            Log("  WARNING: posix_fadvise returned non-zero (clu=%d, dat=%d)\n",
                r1, r2);
        } else {
            Log("  Page cache eviction done.\n");
        }

        auto [cold_qresults, cm] = RunQueryRound(
            "COLD", index, cluster_reader, data_reader.get(), search_cfg,
            qry_emb.data.data(), Q, dim, qry_ids_vec, gt_topk, gt_dists, !skip_gt);
        cold_metrics = cm;

        Log("\n╔══════════════════════════════════════════════════════════╗\n");
        Log("║              HOT vs COLD Read Comparison                ║\n");
        Log("╠══════════════════════════════════════════════════════════╣\n");
        Log("║  Metric              │     HOT     │     COLD    │ Δ   ║\n");
        Log("╟──────────────────────┼─────────────┼─────────────┼─────╢\n");
        Log("║  avg_query (ms)      │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_query_ms, cold_metrics.avg_query_ms,
            (cold_metrics.avg_query_ms / metrics.avg_query_ms - 1.0) * 100);
        Log("║  avg_io_wait (ms)    │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_io_wait, cold_metrics.avg_io_wait,
            cold_metrics.avg_io_wait > 0.001
                ? (cold_metrics.avg_io_wait / std::max(metrics.avg_io_wait, 0.001) - 1.0) * 100
                : 0.0);
        Log("║  avg_probe (ms)      │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_probe, cold_metrics.avg_probe,
            (cold_metrics.avg_probe / std::max(metrics.avg_probe, 0.001) - 1.0) * 100);
        Log("║  avg_rerank_cpu (ms) │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_rerank_cpu, cold_metrics.avg_rerank_cpu,
            (cold_metrics.avg_rerank_cpu / std::max(metrics.avg_rerank_cpu, 0.001) - 1.0) * 100);
        Log("║  p99 (ms)            │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.p99, cold_metrics.p99,
            (cold_metrics.p99 / std::max(metrics.p99, 0.001) - 1.0) * 100);
        Log("║  overlap_ratio       │ %11.4f │ %11.4f │     ║\n",
            metrics.overlap_ratio, cold_metrics.overlap_ratio);
        Log("║  recall@10           │ %11.4f │ %11.4f │     ║\n",
            metrics.recall_at[2], cold_metrics.recall_at[2]);
        Log("╚══════════════════════════════════════════════════════════╝\n");
    }

    // ================================================================
    // Phase E: Summary Output
    // ================================================================
    Log("\n=== Summary ===\n");
    if (metrics.recall_available) {
        Log("  recall@1=%.4f  recall@5=%.4f  recall@10=%.4f\n",
            metrics.recall_at[0], metrics.recall_at[1], metrics.recall_at[2]);
    } else {
        Log("  recall skipped (query-only mode)\n");
    }
    Log("  avg_query=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n",
        metrics.avg_query_ms, metrics.p50, metrics.p95, metrics.p99);
    Log("  build_time=%.1f ms  brute_force=%.1f ms\n",
        training_time_ms, brute_force_time_ms);
    Log("  benchmark_mode=%s  gt_mode=%s\n",
        query_only_mode ? "query_only" : "full_e2e",
        gt_mode.c_str());
    Log("  index_source=%s  resolved_index_dir=%s\n",
        index_source.c_str(), resolved_index_dir.c_str());
    Log("  exrabitq_storage_version=%u  exrabitq_storage_format=%s\n",
        index.segment().cluster_reader().file_version(),
        ExRaBitQStorageFormatName(index.segment().cluster_reader().file_version()));
    Log("  loaded_eps_ip=%.6f  loaded_d_k=%.6f\n",
        index.conann().epsilon(), index.conann().d_k());
    Log("  assignment_mode=%s  assignment_factor=%u  clustering_source=%s\n",
        AssignmentModeName(index.assignment_mode()),
        index.assignment_factor(),
        ClusteringSourceName(index.clustering_source()));
    Log("  rair_lambda=%.3f  rair_strict_second_choice=%d\n",
        index.rair_lambda(), index.rair_strict_second_choice() ? 1 : 0);
    Log("  io_wait=%.3f ms  cpu=%.3f ms  coarse_select=%.3f ms  score=%.3f ms  topn=%.3f ms  probe=%.3f ms\n",
        metrics.avg_io_wait, metrics.avg_cpu, metrics.avg_coarse_select,
        metrics.avg_coarse_score, metrics.avg_coarse_topn, metrics.avg_probe);
    Log("  avg_probed_clusters=%.1f\n", metrics.avg_probed_clusters);
    Log("  fine_grained_timing=%d\n", search_cfg.enable_fine_grained_timing ? 1 : 0);
    if (!search_cfg.enable_fine_grained_timing) {
        Log("  timing_mode=low_overhead_coarse_split (stage1/stage2 are workload-weighted classify splits)\n");
    }
    Log("  probe_prepare=%.3f ms  probe_stage1=%.3f ms  probe_stage2=%.3f ms  probe_classify=%.3f ms  probe_submit=%.3f ms\n",
        metrics.avg_probe_prepare, metrics.avg_probe_stage1, metrics.avg_probe_stage2,
        metrics.avg_probe_classify, metrics.avg_probe_submit);
    Log("  prepare_subtract=%.3f ms  prepare_normalize=%.3f ms  prepare_quantize=%.3f ms  prepare_lut_build=%.3f ms\n",
        metrics.avg_probe_prepare_subtract, metrics.avg_probe_prepare_normalize,
        metrics.avg_probe_prepare_quantize, metrics.avg_probe_prepare_lut_build);
    Log("  stage1_estimate=%.3f ms  stage1_mask=%.3f ms  stage1_iterate=%.3f ms  stage1_classify=%.3f ms\n",
        metrics.avg_probe_stage1_estimate, metrics.avg_probe_stage1_mask,
        metrics.avg_probe_stage1_iterate, metrics.avg_probe_stage1_classify_only);
    Log("  stage2_collect=%.3f ms  stage2_kernel=%.3f ms  stage2_scatter=%.3f ms\n",
        metrics.avg_probe_stage2_collect, metrics.avg_probe_stage2_kernel,
        metrics.avg_probe_stage2_scatter);
    Log("  stage2_kernel_sign_flip=%.3f ms  stage2_kernel_abs_fma=%.3f ms  stage2_kernel_tail=%.3f ms  stage2_kernel_reduce=%.3f ms\n",
        metrics.avg_probe_stage2_kernel_sign_flip,
        metrics.avg_probe_stage2_kernel_abs_fma,
        metrics.avg_probe_stage2_kernel_tail,
        metrics.avg_probe_stage2_kernel_reduce);
    Log("  clu_mode=%s  resident=%d  preload_time=%.3f ms  preload_bytes=%.0f  resident_mem=%.0f  parallel_view_build=%.3f ms  parallel_view_bytes=%.0f\n",
        arg_clu_read_mode.c_str(),
        metrics.query_uses_resident_clusters ? 1 : 0,
        metrics.preload_time_ms,
        metrics.preload_bytes,
        metrics.resident_cluster_mem_bytes,
        metrics.resident_parallel_view_build_ms,
        metrics.resident_parallel_view_bytes);
    Log("  safe_in=%.1f  safe_out=%.1f  uncertain=%.1f\n",
        metrics.avg_safe_in, metrics.avg_safe_out, metrics.avg_uncertain);
    Log("  s2_safe_in=%.1f  s2_safe_out=%.1f  s2_uncertain=%.1f\n",
        metrics.avg_s2_safe_in, metrics.avg_s2_safe_out, metrics.avg_s2_uncertain);
    Log("  duplicate_candidates=%.1f  deduplicated=%.1f  unique_fetch=%.1f\n",
        metrics.avg_duplicate_candidates,
        metrics.avg_deduplicated_candidates,
        metrics.avg_unique_fetch_candidates);
    Log("  false_safeout=%.2f  false_safein_upper=%.1f  total_safein=%lu\n",
        metrics.avg_false_safeout, metrics.avg_false_safein_upper,
        static_cast<unsigned long>(metrics.total_final_safein));
    Log("  early_stop=%.1f%%  avg_skipped=%.1f  overlap=%.4f\n",
        metrics.early_pct * 100, metrics.avg_skipped, metrics.overlap_ratio);

    // ================================================================
    // Phase F: Output JSON
    // ================================================================
    fs::create_directories(output_dir);  // ensure output_dir exists (needed when --index-dir is used)
    Log("\n[Phase F] Writing results to %s\n", output_dir.c_str());

    // config.json
    {
        std::ofstream f(output_dir + "/config.json");
        f << "{\n";
        f << "  " << JStr("dataset", ds_name) << ",\n";
        f << "  " << JStr("dataset_path", data_dir) << ",\n";
        f << "  " << JStr("timestamp", ts) << ",\n";
        f << "  " << JStr("benchmark_mode", query_only_mode ? "query_only" : "full_e2e") << ",\n";
        f << "  " << JStr("gt_mode", gt_mode) << ",\n";
        f << "  " << JStr("gt_source", gt_source) << ",\n";
        f << "  " << JStr("gt_file", arg_gt_file) << ",\n";
        f << "  " << JStr("index_source", index_source) << ",\n";
        f << "  " << JStr("resolved_index_dir", resolved_index_dir) << ",\n";
        f << "  " << JInt("exrabitq_storage_version",
                          index.segment().cluster_reader().file_version()) << ",\n";
        f << "  " << JStr("exrabitq_storage_format",
                          ExRaBitQStorageFormatName(
                              index.segment().cluster_reader().file_version())) << ",\n";
        f << "  " << JInt("num_images", N) << ",\n";
        f << "  " << JInt("num_queries", Q) << ",\n";
        f << "  " << JInt("dimension", dim) << ",\n";
        f << "  " << JInt("logical_dimension", index.logical_dim()) << ",\n";
        f << "  " << JInt("effective_dimension", index.effective_dim()) << ",\n";
        f << "  " << JStr("padding_mode", index.padding_mode()) << ",\n";
        f << "  " << JStr("rotation_mode", index.rotation_mode()) << ",\n";
        f << "  \"build_config\": {\n";
        f << "    " << JInt("nlist", arg_nlist) << ",\n";
        f << "    " << JInt("max_iterations", arg_max_iter) << ",\n";
        f << "    " << JInt("seed", arg_seed) << ",\n";
        f << "    " << JInt("rabitq_bits", arg_bits) << ",\n";
        f << "    " << JInt("rabitq_block_size", arg_block_size) << ",\n";
        f << "    " << JNum("rabitq_c_factor", arg_c_factor) << ",\n";
        f << "    " << JInt("page_size", arg_page_size) << ",\n";
        f << "    " << JInt("epsilon_samples", arg_epsilon_samples) << ",\n";
        f << "    " << JNum("epsilon_percentile", arg_epsilon_percentile) << ",\n";
        f << "    " << JBool("pad_to_pow2", arg_pad_to_pow2 != 0) << ",\n";
        f << "    " << JBool("blocked_hadamard_permuted", arg_blocked_hadamard_permuted != 0) << ",\n";
        f << "    " << JStr("assignment_mode", resolved_assignment_mode) << ",\n";
        f << "    " << JStr("coarse_builder", arg_coarse_builder) << ",\n";
        f << "    " << JStr("requested_metric", index.requested_metric()) << ",\n";
        f << "    " << JStr("effective_metric", index.effective_metric()) << ",\n";
        f << "    " << JInt("faiss_train_size", 100000) << ",\n";
        f << "    " << JInt("faiss_niter", arg_max_iter == 20 ? 10 : arg_max_iter) << ",\n";
        f << "    " << JInt("faiss_nredo", 1) << ",\n";
        f << "    " << JStr("faiss_backend", "cpu") << ",\n";
        f << "    " << JInt("assignment_factor", arg_assignment_factor) << ",\n";
        f << "    " << JNum("rair_lambda", arg_rair_lambda) << ",\n";
        f << "    " << JBool("rair_strict_second_choice", arg_rair_strict_second_choice != 0) << ",\n";
        f << "    " << JStr("requested_index_dir", arg_index_dir) << ",\n";
        f << "    " << JStr("resolved_index_dir", resolved_index_dir) << "\n";
        f << "  },\n";
        f << "  \"search_config\": {\n";
        f << "    " << JInt("top_k", search_cfg.top_k) << ",\n";
        f << "    " << JInt("nprobe", search_cfg.nprobe) << ",\n";
        f << "    " << JBool("early_stop", search_cfg.early_stop) << ",\n";
        f << "    " << JInt("prefetch_depth", search_cfg.prefetch_depth) << ",\n";
        f << "    " << JInt("io_queue_depth", search_cfg.io_queue_depth) << ",\n";
        f << "    " << JInt("cluster_submit_reserve", search_cfg.cluster_submit_reserve) << ",\n";
        f << "    " << JBool("iopoll_requested", arg_iopoll) << ",\n";
        f << "    " << JBool("sqpoll_requested", arg_sqpoll) << ",\n";
        f << "    " << JBool("sqpoll_effective", cluster_reader.sqpoll_enabled()) << ",\n";
        f << "    " << JStr("submission_mode", arg_submission_mode) << ",\n";
        f << "    " << JStr("clu_read_mode", arg_clu_read_mode) << ",\n";
        f << "    " << JBool("use_resident_clusters", search_cfg.use_resident_clusters) << ",\n";
        f << "    " << JBool("enable_fine_grained_timing", search_cfg.enable_fine_grained_timing) << ",\n";
        f << "    " << JBool("enable_address_decode_simd", search_cfg.enable_address_decode_simd) << ",\n";
        f << "    " << JBool("enable_rerank_batched_distance_simd", search_cfg.enable_rerank_batched_distance_simd) << ",\n";
        f << "    " << JBool("enable_coarse_select_simd", search_cfg.enable_coarse_select_simd) << ",\n";
        f << "    " << JBool("enable_coarse_select_phase2", search_cfg.enable_coarse_select_phase2) << ",\n";
        f << "    " << JBool("enable_online_submit_tuning", search_cfg.enable_online_submit_tuning) << ",\n";
        f << "    " << JNum("submit_ema_alpha", search_cfg.submit_ema_alpha) << ",\n";
        f << "    " << JStr("timing_mode",
                             search_cfg.enable_fine_grained_timing
                                 ? "fine_grained_diagnostic"
                                 : "low_overhead_coarse_split") << "\n";
        f << "  },\n";
        f << "  \"crc_config\": {\n";
        f << "    " << JBool("enabled", arg_crc_enable != 0) << ",\n";
        f << "    " << JBool("load_sidecar_legacy", arg_crc_load_sidecar != 0) << ",\n";
        f << "    " << JBool("no_break", arg_crc_no_break != 0) << ",\n";
        f << "    " << JStr("solver", CrcCalibrator::SolverName(arg_crc_solver)) << ",\n";
        f << "    " << JStr("split_mode", arg_crc_use_stratified_split ? "stratified" : "random") << ",\n";
        f << "    " << JNum("split_buckets", arg_crc_split_buckets) << ",\n";
        f << "    " << JNum("alpha", arg_crc_alpha) << ",\n";
        f << "    " << JNum("calib_ratio", arg_crc_calib) << ",\n";
        f << "    " << JNum("tune_ratio", arg_crc_tune) << ",\n";
        f << "    " << JNum("seed_count", crc_seed_plan.size()) << ",\n";
        f << "    \"seed_list\": [";
        for (size_t i = 0; i < crc_seed_plan.size(); ++i) {
            if (i > 0) f << ",";
            f << crc_seed_plan[i];
        }
        f << "],\n";
        f << "    " << JInt("seed", arg_seed) << "\n";
        f << "  },\n";
        f << "  \"runtime_index\": {\n";
        f << "    " << JNum("loaded_eps_ip", index.conann().epsilon()) << ",\n";
        f << "    " << JNum("loaded_d_k", index.conann().d_k()) << ",\n";
        f << "    " << JStr("assignment_mode", AssignmentModeName(index.assignment_mode())) << ",\n";
        f << "    " << JStr("coarse_builder", CoarseBuilderName(index.coarse_builder())) << ",\n";
        f << "    " << JStr("requested_metric", index.requested_metric()) << ",\n";
        f << "    " << JStr("effective_metric", index.effective_metric()) << ",\n";
        f << "    " << JStr("faiss_backend", "cpu") << ",\n";
        f << "    " << JInt("assignment_factor", index.assignment_factor()) << ",\n";
        f << "    " << JNum("rair_lambda", index.rair_lambda()) << ",\n";
        f << "    " << JBool("rair_strict_second_choice", index.rair_strict_second_choice()) << ",\n";
        f << "    " << JStr("clustering_source", ClusteringSourceName(index.clustering_source())) << "\n";
        f << "  }\n";
        f << "}\n";
    }

    // results.json
    {
        std::ofstream f(output_dir + "/results.json");
        f << "{\n";

        // metrics
        f << "  \"metrics\": {\n";
        f << "    " << JNum("training_time_ms", training_time_ms) << ",\n";
        f << "    " << JNum("brute_force_time_ms", brute_force_time_ms) << ",\n";
        f << "    " << JStr("index_source", index_source) << ",\n";
        f << "    " << JStr("resolved_index_dir", resolved_index_dir) << ",\n";
        f << "    " << JInt("exrabitq_storage_version",
                            index.segment().cluster_reader().file_version()) << ",\n";
        f << "    " << JStr("exrabitq_storage_format",
                            ExRaBitQStorageFormatName(
                                index.segment().cluster_reader().file_version())) << ",\n";
        f << "    " << JNum("loaded_eps_ip", index.conann().epsilon()) << ",\n";
        f << "    " << JNum("loaded_d_k", index.conann().d_k()) << ",\n";
        f << "    " << JBool("recall_available", metrics.recall_available) << ",\n";
        f << "    " << JInt("logical_dimension", index.logical_dim()) << ",\n";
        f << "    " << JInt("effective_dimension", index.effective_dim()) << ",\n";
        f << "    " << JStr("padding_mode", index.padding_mode()) << ",\n";
        f << "    " << JStr("rotation_mode", index.rotation_mode()) << ",\n";
        f << "    " << JStr("gt_mode", gt_mode) << ",\n";
        f << "    " << JStr("gt_source", gt_source) << ",\n";
        f << "    " << JStr("gt_file", arg_gt_file) << ",\n";
        f << "    " << JNum("recall_at_1", metrics.recall_at[0]) << ",\n";
        f << "    " << JNum("recall_at_5", metrics.recall_at[1]) << ",\n";
        f << "    " << JNum("recall_at_10", metrics.recall_at[2]) << ",\n";
        f << "    " << JNum("avg_query_time_ms", metrics.avg_query_ms) << ",\n";
        f << "    " << JNum("p50_query_time_ms", metrics.p50) << ",\n";
        f << "    " << JNum("p95_query_time_ms", metrics.p95) << ",\n";
        f << "    " << JNum("p99_query_time_ms", metrics.p99) << ",\n";
        f << "    " << JNum("preload_time_ms", metrics.preload_time_ms) << ",\n";
        f << "    " << JNum("preload_bytes", metrics.preload_bytes) << ",\n";
        f << "    " << JNum("resident_cluster_mem_bytes", metrics.resident_cluster_mem_bytes) << ",\n";
        f << "    " << JNum("resident_parallel_view_build_ms", metrics.resident_parallel_view_build_ms) << ",\n";
        f << "    " << JNum("resident_parallel_view_bytes", metrics.resident_parallel_view_bytes) << ",\n";
        f << "    " << JNum("index_total_bytes", metrics.index_total_bytes) << ",\n";
        f << "    " << JNum("index_cluster_clu_bytes", metrics.index_cluster_clu_bytes) << ",\n";
        f << "    " << JNum("index_data_dat_bytes", metrics.index_data_dat_bytes) << ",\n";
        f << "    " << JNum("index_rotation_bytes", metrics.index_rotation_bytes) << ",\n";
        f << "    " << JNum("index_rotated_centroids_bytes", metrics.index_rotated_centroids_bytes) << ",\n";
        f << "    " << JBool("query_uses_resident_clusters", metrics.query_uses_resident_clusters) << ",\n";
        f << "    " << JInt("num_queries", Q) << "\n";
        f << "  },\n";

        // pipeline_stats
        f << "  \"pipeline_stats\": {\n";
        f << "    " << JNum("avg_total_probed", metrics.avg_probed) << ",\n";
        f << "    " << JNum("avg_safe_in", metrics.avg_safe_in) << ",\n";
        f << "    " << JNum("avg_safe_out", metrics.avg_safe_out) << ",\n";
        f << "    " << JNum("avg_uncertain", metrics.avg_uncertain) << ",\n";
        f << "    " << JNum("avg_s2_safe_in", metrics.avg_s2_safe_in) << ",\n";
        f << "    " << JNum("avg_s2_safe_out", metrics.avg_s2_safe_out) << ",\n";
        f << "    " << JNum("avg_s2_uncertain", metrics.avg_s2_uncertain) << ",\n";
        f << "    " << JNum("avg_duplicate_candidates", metrics.avg_duplicate_candidates) << ",\n";
        f << "    " << JNum("avg_deduplicated_candidates", metrics.avg_deduplicated_candidates) << ",\n";
        f << "    " << JNum("avg_unique_fetch_candidates", metrics.avg_unique_fetch_candidates) << ",\n";
        f << "    " << JNum("avg_false_safeout", metrics.avg_false_safeout) << ",\n";
        f << "    " << JNum("avg_false_safein_upper", metrics.avg_false_safein_upper) << ",\n";
        f << "    " << JInt("total_final_safein", static_cast<int64_t>(metrics.total_final_safein)) << ",\n";
        f << "    " << JNum("avg_io_wait_ms", metrics.avg_io_wait) << ",\n";
        f << "    " << JNum("avg_cpu_time_ms", metrics.avg_cpu) << ",\n";
        f << "    " << JNum("avg_coarse_select_ms", metrics.avg_coarse_select) << ",\n";
        f << "    " << JNum("avg_coarse_score_ms", metrics.avg_coarse_score) << ",\n";
        f << "    " << JNum("avg_coarse_topn_ms", metrics.avg_coarse_topn) << ",\n";
        f << "    " << JNum("avg_probe_time_ms", metrics.avg_probe) << ",\n";
        f << "    " << JNum("avg_probe_prepare_ms", metrics.avg_probe_prepare) << ",\n";
        f << "    " << JNum("avg_probe_prepare_rotation_ms", metrics.avg_probe_prepare_rotation) << ",\n";
        f << "    " << JNum("avg_probe_prepare_subtract_ms", metrics.avg_probe_prepare_subtract) << ",\n";
        f << "    " << JNum("avg_probe_prepare_normalize_ms", metrics.avg_probe_prepare_normalize) << ",\n";
        f << "    " << JNum("avg_probe_prepare_quantize_ms", metrics.avg_probe_prepare_quantize) << ",\n";
        f << "    " << JNum("avg_probe_prepare_lut_build_ms", metrics.avg_probe_prepare_lut_build) << ",\n";
        f << "    " << JNum("avg_probe_prepare_quant_lut_ms", metrics.avg_probe_prepare_quant_lut) << ",\n";
        f << "    " << JNum("avg_probe_stage1_ms", metrics.avg_probe_stage1) << ",\n";
        f << "    " << JNum("avg_probe_stage1_estimate_ms", metrics.avg_probe_stage1_estimate) << ",\n";
        f << "    " << JNum("avg_probe_stage1_mask_ms", metrics.avg_probe_stage1_mask) << ",\n";
        f << "    " << JNum("avg_probe_stage1_iterate_ms", metrics.avg_probe_stage1_iterate) << ",\n";
        f << "    " << JNum("avg_probe_stage1_classify_only_ms", metrics.avg_probe_stage1_classify_only) << ",\n";
        f << "    " << JNum("avg_probe_stage2_ms", metrics.avg_probe_stage2) << ",\n";
        f << "    " << JNum("avg_probe_stage2_collect_ms", metrics.avg_probe_stage2_collect) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_ms", metrics.avg_probe_stage2_kernel) << ",\n";
        f << "    " << JNum("avg_probe_stage2_scatter_ms", metrics.avg_probe_stage2_scatter) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_sign_flip_ms", metrics.avg_probe_stage2_kernel_sign_flip) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_abs_fma_ms", metrics.avg_probe_stage2_kernel_abs_fma) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_tail_ms", metrics.avg_probe_stage2_kernel_tail) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_reduce_ms", metrics.avg_probe_stage2_kernel_reduce) << ",\n";
        f << "    " << JNum("avg_probe_classify_ms", metrics.avg_probe_classify) << ",\n";
        f << "    " << JNum("avg_probe_submit_ms", metrics.avg_probe_submit) << ",\n";
        f << "    " << JNum("avg_probe_submit_prepare_vec_only_ms", metrics.avg_probe_submit_prepare_vec_only) << ",\n";
        f << "    " << JNum("avg_probe_submit_prepare_all_ms", metrics.avg_probe_submit_prepare_all) << ",\n";
        f << "    " << JNum("avg_probe_submit_emit_ms", metrics.avg_probe_submit_emit) << ",\n";
        f << "    " << JNum("avg_rerank_cpu_ms", metrics.avg_rerank_cpu) << ",\n";
        f << "    " << JNum("avg_prefetch_submit_ms", metrics.avg_prefetch_submit) << ",\n";
        f << "    " << JNum("avg_prefetch_wait_ms", metrics.avg_prefetch_wait) << ",\n";
        f << "    " << JNum("avg_safein_payload_prefetch_ms", metrics.avg_safein_payload_prefetch) << ",\n";
        f << "    " << JNum("avg_candidate_collect_ms", metrics.avg_candidate_collect) << ",\n";
        f << "    " << JNum("avg_pool_vector_read_ms", metrics.avg_pool_vector_read) << ",\n";
        f << "    " << JNum("avg_rerank_compute_ms", metrics.avg_rerank_compute) << ",\n";
        f << "    " << JNum("avg_remaining_payload_fetch_ms", metrics.avg_remaining_payload_fetch) << ",\n";
        f << "    " << JNum("avg_uring_prep_ms", metrics.avg_uring_prep) << ",\n";
        f << "    " << JNum("avg_uring_submit_ms", metrics.avg_uring_submit) << ",\n";
        f << "    " << JNum("avg_parse_cluster_ms", metrics.avg_parse_cluster) << ",\n";
        f << "    " << JNum("avg_fetch_missing_ms", metrics.avg_fetch_missing) << ",\n";
        f << "    " << JNum("avg_submit_calls", metrics.avg_submit_calls) << ",\n";
        f << "    " << JNum("avg_submit_window_flushes", metrics.avg_submit_window_flushes) << ",\n";
        f << "    " << JNum("avg_submit_window_tail_flushes", metrics.avg_submit_window_tail_flushes) << ",\n";
        f << "    " << JNum("avg_submit_stop_flushes", metrics.avg_submit_stop_flushes) << ",\n";
        f << "    " << JNum("avg_submit_window_requests", metrics.avg_submit_window_requests) << ",\n";
        f << "    " << JNum("avg_probed_clusters", metrics.avg_probed_clusters) << ",\n";
        f << "    " << JNum("avg_candidate_batches_per_cluster", metrics.avg_candidate_batches_per_cluster) << ",\n";
        f << "    " << JNum("avg_crc_estimates_buffered_per_cluster", metrics.avg_crc_estimates_buffered_per_cluster) << ",\n";
        f << "    " << JNum("avg_crc_estimates_merged_per_cluster", metrics.avg_crc_estimates_merged_per_cluster) << ",\n";
        f << "    " << JNum("avg_stage2_block_lookups", metrics.avg_stage2_block_lookups) << ",\n";
        f << "    " << JNum("avg_stage2_block_reuses", metrics.avg_stage2_block_reuses) << ",\n";
        f << "    " << JNum("avg_crc_decision_ms", metrics.avg_crc_decision_ms) << ",\n";
        f << "    " << JNum("avg_crc_buffer_ms", metrics.avg_crc_buffer_ms) << ",\n";
        f << "    " << JNum("avg_crc_merge_ms", metrics.avg_crc_merge_ms) << ",\n";
        f << "    " << JNum("avg_crc_online_ms", metrics.avg_crc_online_ms) << ",\n";
        f << "    " << JNum("avg_crc_would_stop", metrics.avg_crc_would_stop) << ",\n";
        f << "    " << JNum("avg_candidates_buffered", metrics.avg_candidates_buffered) << ",\n";
        f << "    " << JNum("avg_candidates_reranked", metrics.avg_candidates_reranked) << ",\n";
        f << "    " << JNum("avg_safein_payload_prefetched", metrics.avg_safein_payload_prefetched) << ",\n";
        f << "    " << JNum("avg_remaining_payload_fetches", metrics.avg_remaining_payload_fetches) << ",\n";
        f << "    " << JNum("early_stopped_pct", metrics.early_pct) << ",\n";
        f << "    " << JNum("avg_clusters_skipped", metrics.avg_skipped) << ",\n";
        f << "    " << JNum("overlap_ratio", metrics.overlap_ratio) << "\n";
        f << "  },\n";

        // per_query_sample
        uint32_t stride = std::max(Q / 50, 1u);
        f << "  \"per_query_sample\": [\n";
        bool first = true;
        for (uint32_t qi = 0; qi < Q; qi += stride) {
            if (!first) f << ",\n";
            first = false;

            const auto& qr = qresults[qi];
            double r10 = metrics.recall_available
                ? ComputeRecallAtK(qr.predicted_ids, gt_topk[qi], 10)
                : 0.0;
            bool hit1 = metrics.recall_available &&
                        !qr.predicted_ids.empty() && !gt_topk[qi].empty() &&
                        qr.predicted_ids[0] == gt_topk[qi][0];
            double cpu_ms = qr.query_time_ms - qr.io_wait_ms;

            f << "    {\n";
            f << "      " << JInt("query_id", qr.query_id) << ",\n";
            f << "      " << JArr64("gt_top10_ids", metrics.recall_available ? gt_topk[qi] : std::vector<int64_t>{}) << ",\n";
            f << "      " << JArr64("predicted_top10_ids", qr.predicted_ids) << ",\n";
            f << "      " << JArrF("predicted_top10_distances", qr.predicted_dists) << ",\n";
            f << "      " << JBool("hit_at_1", hit1) << ",\n";
            f << "      " << JNum("recall_at_10", r10) << ",\n";
            f << "      " << JNum("query_time_ms", qr.query_time_ms) << ",\n";
            f << "      " << JNum("io_wait_ms", qr.io_wait_ms) << ",\n";
            f << "      " << JNum("cpu_time_ms", cpu_ms) << ",\n";
            f << "      " << JNum("coarse_select_ms", qr.coarse_select_ms) << ",\n";
            f << "      " << JNum("coarse_score_ms", qr.coarse_score_ms) << ",\n";
            f << "      " << JNum("coarse_topn_ms", qr.coarse_topn_ms) << ",\n";
            f << "      " << JNum("probe_ms", qr.probe_time_ms) << ",\n";
            f << "      " << JNum("probe_prepare_ms", qr.probe_prepare_ms) << ",\n";
            f << "      " << JNum("probe_prepare_rotation_ms", qr.probe_prepare_rotation_ms) << ",\n";
            f << "      " << JNum("probe_prepare_subtract_ms", qr.probe_prepare_subtract_ms) << ",\n";
            f << "      " << JNum("probe_prepare_normalize_ms", qr.probe_prepare_normalize_ms) << ",\n";
            f << "      " << JNum("probe_prepare_quantize_ms", qr.probe_prepare_quantize_ms) << ",\n";
            f << "      " << JNum("probe_prepare_lut_build_ms", qr.probe_prepare_lut_build_ms) << ",\n";
            f << "      " << JNum("probe_prepare_quant_lut_ms", qr.probe_prepare_quant_lut_ms) << ",\n";
            f << "      " << JNum("probe_stage1_ms", qr.probe_stage1_ms) << ",\n";
            f << "      " << JNum("probe_stage1_estimate_ms", qr.probe_stage1_estimate_ms) << ",\n";
            f << "      " << JNum("probe_stage1_mask_ms", qr.probe_stage1_mask_ms) << ",\n";
            f << "      " << JNum("probe_stage1_iterate_ms", qr.probe_stage1_iterate_ms) << ",\n";
            f << "      " << JNum("probe_stage1_classify_only_ms", qr.probe_stage1_classify_only_ms) << ",\n";
            f << "      " << JNum("probe_stage2_ms", qr.probe_stage2_ms) << ",\n";
            f << "      " << JNum("probe_stage2_collect_ms", qr.probe_stage2_collect_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_ms", qr.probe_stage2_kernel_ms) << ",\n";
            f << "      " << JNum("probe_stage2_scatter_ms", qr.probe_stage2_scatter_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_sign_flip_ms", qr.probe_stage2_kernel_sign_flip_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_abs_fma_ms", qr.probe_stage2_kernel_abs_fma_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_tail_ms", qr.probe_stage2_kernel_tail_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_reduce_ms", qr.probe_stage2_kernel_reduce_ms) << ",\n";
            f << "      " << JNum("probe_classify_ms", qr.probe_classify_ms) << ",\n";
            f << "      " << JNum("probe_submit_ms", qr.probe_submit_ms) << ",\n";
            f << "      " << JNum("probe_submit_prepare_vec_only_ms", qr.probe_submit_prepare_vec_only_ms) << ",\n";
            f << "      " << JNum("probe_submit_prepare_all_ms", qr.probe_submit_prepare_all_ms) << ",\n";
            f << "      " << JNum("probe_submit_emit_ms", qr.probe_submit_emit_ms) << ",\n";
            f << "      " << JNum("prefetch_submit_ms", qr.prefetch_submit_ms) << ",\n";
            f << "      " << JNum("prefetch_wait_ms", qr.prefetch_wait_ms) << ",\n";
            f << "      " << JNum("safein_payload_prefetch_ms", qr.safein_payload_prefetch_ms) << ",\n";
            f << "      " << JNum("candidate_collect_ms", qr.candidate_collect_ms) << ",\n";
            f << "      " << JNum("pool_vector_read_ms", qr.pool_vector_read_ms) << ",\n";
            f << "      " << JNum("rerank_compute_ms", qr.rerank_compute_ms) << ",\n";
            f << "      " << JNum("remaining_payload_fetch_ms", qr.remaining_payload_fetch_ms) << ",\n";
            f << "      " << JInt("num_candidates_buffered", qr.num_candidates_buffered) << ",\n";
            f << "      " << JInt("num_candidates_reranked", qr.num_candidates_reranked) << ",\n";
            f << "      " << JInt("num_safein_payload_prefetched", qr.num_safein_payload_prefetched) << ",\n";
            f << "      " << JInt("num_remaining_payload_fetches", qr.num_remaining_payload_fetches) << ",\n";
            f << "      " << JInt("submit_calls", qr.submit_calls) << ",\n";
            f << "      " << JInt("submit_window_flushes", qr.submit_window_flushes) << ",\n";
            f << "      " << JInt("submit_window_tail_flushes", qr.submit_window_tail_flushes) << ",\n";
            f << "      " << JInt("submit_stop_flushes", qr.submit_stop_flushes) << ",\n";
            f << "      " << JNum("submit_window_requests", qr.submit_window_requests) << ",\n";
            f << "      " << JNum("crc_decision_ms", qr.crc_decision_ms) << ",\n";
            f << "      " << JNum("crc_buffer_ms", qr.crc_buffer_ms) << ",\n";
            f << "      " << JNum("crc_merge_ms", qr.crc_merge_ms) << ",\n";
            f << "      " << JNum("crc_online_ms", qr.crc_online_ms) << ",\n";
            f << "      " << JInt("crc_would_stop", qr.crc_would_stop) << ",\n";
            f << "      " << JNum("stage2_block_lookups", qr.stage2_block_lookups) << ",\n";
            f << "      " << JNum("stage2_block_reuses", qr.stage2_block_reuses) << ",\n";
            f << "      " << JInt("probed_clusters", qr.probed_clusters) << ",\n";
            f << "      " << JBool("early_stopped", qr.early_stopped) << ",\n";
            f << "      " << JInt("clusters_skipped", qr.clusters_skipped) << "\n";
            f << "    }";
        }
        f << "\n  ],\n";

        // CRC calibration
        f << "  \"crc_calibration\": {\n";
        f << "    " << JBool("loaded", crc_loaded) << ",\n";
        f << "    " << JBool("loaded_from_sidecar", crc_sidecar_loaded) << ",\n";
        f << "    " << JStr("params_source", crc_params_source) << ",\n";
        f << "    " << JStr("split_mode", arg_crc_use_stratified_split ? "stratified" : "random") << ",\n";
        f << "    " << JNum("seed_count", crc_trials.empty() ? 0 : crc_trials.size()) << ",\n";
        f << "    " << JNum("crc_seed_mean_actual_fnr", crc_actual_fnr_mean) << ",\n";
        f << "    " << JNum("crc_seed_std_actual_fnr", crc_actual_fnr_std) << ",\n";
        f << "    " << JNum("crc_seed_mean_avg_probed", crc_avg_probed_mean) << ",\n";
        f << "    " << JNum("crc_seed_std_avg_probed", crc_avg_probed_std) << ",\n";
        f << "    " << JNum("crc_seed_mean_recall_at_10", crc_recall10_mean) << ",\n";
        f << "    " << JNum("crc_seed_std_recall_at_10", crc_recall10_std) << ",\n";
        f << "    " << JStr("solver", CrcCalibrator::SolverName(crc_eval_results.solver_used)) << ",\n";
        f << "    " << JNum("alpha", arg_crc_alpha) << ",\n";
        f << "    " << JNum("calib_ratio", arg_crc_calib) << ",\n";
        f << "    " << JNum("tune_ratio", arg_crc_tune) << ",\n";
        f << "    " << JNum("selected_seed", crc_selected_seed) << ",\n";
        f << "    " << JNum("d_min", calib_results.d_min) << ",\n";
        f << "    " << JNum("d_max", calib_results.d_max) << ",\n";
        f << "    " << JNum("lamhat", calib_results.lamhat) << ",\n";
        f << "    " << JInt("kreg", calib_results.kreg) << ",\n";
        f << "    " << JNum("reg_lambda", calib_results.reg_lambda) << ",\n";
        f << "    " << JBool("eval_available", crc_eval_available) << ",\n";
        f << "    " << JNum("eval_actual_fnr", crc_eval_results.actual_fnr) << ",\n";
        f << "    " << JNum("eval_avg_probed", crc_eval_results.avg_probed) << ",\n";
        f << "    " << JNum("eval_recall_at_1", crc_eval_results.recall_at_1) << ",\n";
        f << "    " << JNum("eval_recall_at_5", crc_eval_results.recall_at_5) << ",\n";
        f << "    " << JNum("eval_recall_at_10", crc_eval_results.recall_at_10) << ",\n";
        f << "    " << JInt("eval_test_size", crc_eval_results.test_size) << ",\n";
        f << "    " << JInt("candidate_count", crc_eval_results.candidate_count) << ",\n";
        f << "    " << JInt("objective_evals", crc_eval_results.objective_evals) << ",\n";
        f << "    " << JNum("profile_build_ms", crc_eval_results.profile_build_ms) << ",\n";
        f << "    " << JNum("solver_ms", crc_eval_results.solver_ms) << "\n";
        f << "  }\n";

        f << "}\n";
    }

    if (arg_crc_enable) {
        std::ofstream f(CrcRunCalibrationPath(output_dir));
        f << "{\n";
        f << "  " << JStr("params_source", crc_params_source) << ",\n";
        f << "  " << JBool("loaded_from_sidecar", crc_sidecar_loaded) << ",\n";
        f << "  " << JStr("solver", CrcCalibrator::SolverName(crc_eval_results.solver_used)) << ",\n";
        f << "  " << JStr("split_mode", arg_crc_use_stratified_split ? "stratified" : "random") << ",\n";
        f << "  " << JNum("seed_count", crc_trials.empty() ? 0 : crc_trials.size()) << ",\n";
        f << "  \"seed_list\": [";
        for (size_t i = 0; i < crc_seed_plan.size(); ++i) {
            if (i) f << ",";
            f << crc_seed_plan[i];
        }
        f << "],\n";
        f << "  " << JNum("selected_seed", crc_selected_seed) << ",\n";
        f << "  " << JNum("seed_agg_actual_fnr_mean", crc_actual_fnr_mean) << ",\n";
        f << "  " << JNum("seed_agg_actual_fnr_std", crc_actual_fnr_std) << ",\n";
        f << "  " << JNum("seed_agg_avg_probed_mean", crc_avg_probed_mean) << ",\n";
        f << "  " << JNum("seed_agg_avg_probed_std", crc_avg_probed_std) << ",\n";
        f << "  " << JNum("seed_agg_recall10_mean", crc_recall10_mean) << ",\n";
        f << "  " << JNum("seed_agg_recall10_std", crc_recall10_std) << ",\n";
        f << "  " << JNum("alpha", arg_crc_alpha) << ",\n";
        f << "  " << JNum("calib_ratio", arg_crc_calib) << ",\n";
        f << "  " << JNum("tune_ratio", arg_crc_tune) << ",\n";
        f << "  " << JNum("seed", crc_selected_seed) << ",\n";
        f << "  " << JNum("d_min", calib_results.d_min) << ",\n";
        f << "  " << JNum("d_max", calib_results.d_max) << ",\n";
        f << "  " << JNum("lamhat", calib_results.lamhat) << ",\n";
        f << "  " << JInt("kreg", calib_results.kreg) << ",\n";
        f << "  " << JNum("reg_lambda", calib_results.reg_lambda) << ",\n";
        f << "  \"eval\": {\n";
        f << "    " << JBool("available", crc_eval_available) << ",\n";
        f << "    " << JNum("actual_fnr", crc_eval_results.actual_fnr) << ",\n";
        f << "    " << JNum("avg_probed", crc_eval_results.avg_probed) << ",\n";
        f << "    " << JNum("recall_at_1", crc_eval_results.recall_at_1) << ",\n";
        f << "    " << JNum("recall_at_5", crc_eval_results.recall_at_5) << ",\n";
        f << "    " << JNum("recall_at_10", crc_eval_results.recall_at_10) << ",\n";
        f << "    " << JInt("test_size", crc_eval_results.test_size) << ",\n";
        f << "    " << JInt("candidate_count", crc_eval_results.candidate_count) << ",\n";
        f << "    " << JInt("objective_evals", crc_eval_results.objective_evals) << ",\n";
        f << "    " << JNum("profile_build_ms", crc_eval_results.profile_build_ms) << ",\n";
        f << "    " << JNum("solver_ms", crc_eval_results.solver_ms) << "\n";
        f << "  }\n";
        f << "}\n";
    }

    Log("  Output: %s\n", output_dir.c_str());

    return 0;
}
