/// bench_vector_search.cpp — Vector search benchmark using VDB library path.
///
/// Supports standard ANN-Benchmark datasets (.fvecs/.ivecs) without payload.
/// Pipeline: Load → IvfBuilder::Build → IvfIndex::Open → OverlapScheduler::Search → Recall.
/// Uses the production library path (IvfBuilder, IvfIndex, OverlapScheduler) for
/// bit-exact alignment with bench_e2e. The frozen inline implementation is in
/// bench_vector_search_inline.cpp.
///
/// Usage:
///   bench_vector_search --base /path/to/base.fvecs --query /path/to/query.fvecs
///       [--gt /path/to/gt.ivecs] [--nlist 256] [--nprobe 32] [--topk 10]
///       [--bits 4] [--crc 1] [--early-stop 1] [--outdir ./results]
///       [--centroids /path/to/centroids.fvecs] [--assignments /path/to/assign.ivecs]
///       [--pad-to-pow2 1] [--index-dir /path/to/existing/index]
///       [--tmp-dir /tmp/bench_vs] [--keep-index]

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/query/search_context.h"
#include "vdb/query/search_results.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::query;

namespace fs = std::filesystem;

// ============================================================================
// Power-of-2 padding helpers (kept for --pad-to-pow2 / Deep1M dim=96→128)
// ============================================================================
static inline bool IsPowerOf2(uint32_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}
static inline uint32_t NextPowerOf2(uint32_t n) {
    if (n == 0) return 1;
    if (IsPowerOf2(n)) return n;
    uint32_t p = 1; while (p < n) p <<= 1;
    return p;
}
static void PadVectors(std::vector<float>& data, uint32_t N,
                       uint32_t old_dim, uint32_t new_dim) {
    if (new_dim == old_dim) return;
    std::vector<float> padded(static_cast<size_t>(N) * new_dim, 0.0f);
    for (uint32_t i = 0; i < N; ++i) {
        std::memcpy(padded.data() + static_cast<size_t>(i) * new_dim,
                    data.data() + static_cast<size_t>(i) * old_dim,
                    old_dim * sizeof(float));
    }
    data = std::move(padded);
}

// ============================================================================
// CLI helpers
// ============================================================================
static std::string GetArg(int argc, char* argv[], const char* name,
                          const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}
static int GetIntArg(int argc, char* argv[], const char* name, int def) {
    auto s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::atoi(s.c_str());
}
static float GetFloatArg(int argc, char* argv[], const char* name, float def) {
    auto s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::strtof(s.c_str(), nullptr);
}
static bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}
static void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vprintf(fmt, ap); va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // --- CLI ---
    std::string base_path  = GetArg(argc, argv, "--base", "");
    std::string query_path = GetArg(argc, argv, "--query", "");
    std::string gt_path    = GetArg(argc, argv, "--gt", "");
    uint32_t nlist   = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 256));
    uint32_t nprobe  = static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 32));
    uint32_t top_k   = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    bool use_crc     = GetIntArg(argc, argv, "--crc", 1) != 0;
    float crc_alpha  = GetFloatArg(argc, argv, "--crc-alpha", 0.1f);
    float crc_calib  = GetFloatArg(argc, argv, "--crc-calib", 0.5f);
    float crc_tune   = GetFloatArg(argc, argv, "--crc-tune", 0.1f);
    bool early_stop  = GetIntArg(argc, argv, "--early-stop", 1) != 0;
    uint64_t seed    = static_cast<uint64_t>(GetIntArg(argc, argv, "--seed", 42));
    std::string outdir = GetArg(argc, argv, "--outdir", "");
    int q_limit      = GetIntArg(argc, argv, "--queries", 0);
    uint8_t bits     = static_cast<uint8_t>(GetIntArg(argc, argv, "--bits", 1));
    uint32_t epsilon_samples = static_cast<uint32_t>(GetIntArg(argc, argv, "--epsilon-samples", 100));
    float epsilon_percentile = GetFloatArg(argc, argv, "--epsilon-percentile", 0.99f);
    std::string centroids_path   = GetArg(argc, argv, "--centroids", "");
    std::string assignments_path = GetArg(argc, argv, "--assignments", "");
    bool pad_to_pow2 = GetIntArg(argc, argv, "--pad-to-pow2", 0) != 0;
    std::string arg_index_dir = GetArg(argc, argv, "--index-dir", "");
    std::string arg_tmp_dir   = GetArg(argc, argv, "--tmp-dir", "");
    bool keep_index           = HasFlag(argc, argv, "--keep-index");

    if (base_path.empty() || query_path.empty()) {
        std::fprintf(stderr,
            "Usage: bench_vector_search --base <path> --query <path> "
            "[--gt <path>] [--nlist N] [--nprobe N] [--topk K] "
            "[--crc 0|1] [--early-stop 0|1] [--bits N] [--outdir dir] "
            "[--centroids <path>] [--assignments <path>] [--pad-to-pow2 1] "
            "[--index-dir <path>] [--tmp-dir <path>] [--keep-index]\n");
        return 1;
    }

    Log("=== Vector Search Benchmark (bits=%u) ===\n", bits);
    Log("Base:   %s\n", base_path.c_str());
    Log("Query:  %s\n", query_path.c_str());
    Log("GT:     %s\n", gt_path.empty() ? "(brute-force)" : gt_path.c_str());
    Log("nlist=%u  nprobe=%u  top_k=%u  bits=%u  crc=%d  early_stop=%d\n",
        nlist, nprobe, top_k, bits, use_crc, early_stop);

    // ================================================================
    // Phase A: Load data
    // ================================================================
    Log("\n[Phase A] Loading data...\n");

    auto base_or = io::LoadVectors(base_path);
    if (!base_or.ok()) {
        std::fprintf(stderr, "Failed to load base: %s\n",
                     base_or.status().ToString().c_str());
        return 1;
    }
    auto& base = base_or.value();
    uint32_t N = base.rows;
    Dim dim = static_cast<Dim>(base.cols);

    auto qry_or = io::LoadVectors(query_path);
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load query: %s\n",
                     qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q = (q_limit > 0) ? std::min(static_cast<uint32_t>(q_limit), qry.rows)
                                : qry.rows;
    if (qry.cols != base.cols) {
        std::fprintf(stderr, "Dimension mismatch: base=%u, query=%u\n",
                     base.cols, qry.cols);
        return 1;
    }
    Log("  N=%u, Q=%u, dim=%u\n", N, Q, dim);

    // Pad to next power of 2 if requested (e.g. Deep1M dim=96→128)
    uint32_t orig_dim = dim;
    std::string padded_centroids_path;  // temp file if centroids need padding
    if (pad_to_pow2 && !IsPowerOf2(dim)) {
        uint32_t new_dim = NextPowerOf2(dim);
        Log("  [Padding] dim %u -> %u (next power of 2)\n", dim, new_dim);
        PadVectors(base.data, N, dim, new_dim);
        PadVectors(qry.data, qry.rows, dim, new_dim);

        // Also pad pre-computed centroids if provided
        if (!centroids_path.empty()) {
            auto cents_or = io::LoadVectors(centroids_path);
            if (cents_or.ok()) {
                auto& cents = cents_or.value();
                if (cents.cols == dim) {  // original dim before update
                    PadVectors(cents.data, cents.rows, dim, new_dim);
                    // Write padded centroids to a temp file
                    padded_centroids_path = "/tmp/padded_centroids_bench.fvecs";
                    std::ofstream pf(padded_centroids_path, std::ios::binary);
                    for (uint32_t i = 0; i < cents.rows; ++i) {
                        uint32_t d32 = new_dim;
                        pf.write(reinterpret_cast<const char*>(&d32), 4);
                        pf.write(reinterpret_cast<const char*>(
                                     cents.data.data() + static_cast<size_t>(i) * new_dim),
                                 new_dim * sizeof(float));
                    }
                    centroids_path = padded_centroids_path;
                    Log("  [Padding] Centroids padded: %u×%u → %u×%u\n",
                        cents.rows, dim, cents.rows, new_dim);
                }
            }
        }

        dim = static_cast<Dim>(new_dim);
        base.cols = new_dim;
        qry.cols = new_dim;
    }

    // GT loading
    std::vector<std::vector<uint32_t>> gt_ids;
    uint32_t gt_k = 0;

    if (!gt_path.empty() && fs::exists(gt_path)) {
        Log("  Loading GT from %s...\n", gt_path.c_str());
        auto gt_or = io::LoadIvecs(gt_path);
        if (!gt_or.ok()) {
            std::fprintf(stderr, "Failed to load GT: %s\n",
                         gt_or.status().ToString().c_str());
            return 1;
        }
        auto& gt = gt_or.value();
        gt_k = gt.cols;
        uint32_t gt_rows = std::min(gt.rows, Q);
        gt_ids.resize(gt_rows);
        for (uint32_t qi = 0; qi < gt_rows; ++qi) {
            gt_ids[qi].assign(
                gt.data.data() + static_cast<size_t>(qi) * gt.cols,
                gt.data.data() + static_cast<size_t>(qi) * gt.cols + gt.cols);
        }
        Log("  GT: %u queries, k=%u\n", gt_rows, gt_k);
    } else {
        Log("  No GT file — computing brute-force L2...\n");
        gt_k = std::max(top_k, 100u);
        gt_ids.resize(Q);
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
            std::vector<std::pair<float, uint32_t>> dists(N);
            for (uint32_t i = 0; i < N; ++i) {
                dists[i] = {simd::L2Sqr(q_vec,
                    base.data.data() + static_cast<size_t>(i) * dim, dim), i};
            }
            std::partial_sort(dists.begin(),
                              dists.begin() + std::min(gt_k, N), dists.end());
            gt_ids[qi].resize(std::min(gt_k, N));
            for (uint32_t i = 0; i < std::min(gt_k, N); ++i)
                gt_ids[qi][i] = dists[i].second;
        }
        Log("  Brute-force GT computed (k=%u).\n", gt_k);
    }

    // ================================================================
    // Phase B: Build Index (via IvfBuilder) — or use --index-dir
    // ================================================================
    std::string index_dir;
    bool tmp_index = false;  // whether we created a temp index to clean up
    double build_time_ms = 0.0;

    if (!arg_index_dir.empty()) {
        index_dir = arg_index_dir;
        Log("\n[Phase B] Skipped (using pre-built index: %s)\n", index_dir.c_str());
    } else {
        // Determine temp directory
        if (arg_tmp_dir.empty()) {
            // Generate timestamped temp dir
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", std::localtime(&t));
            arg_tmp_dir = std::string("/tmp/bench_vs_") + ts;
        }
        index_dir = arg_tmp_dir;
        tmp_index = !keep_index;

        Log("\n[Phase B] Building index -> %s\n", index_dir.c_str());
        fs::create_directories(index_dir);

        IvfBuilderConfig cfg;
        cfg.nlist = nlist;
        cfg.seed = seed;
        cfg.rabitq = {bits, 64, 5.75f};
        cfg.calibration_samples = std::min(100u, N);
        cfg.epsilon_samples = epsilon_samples;
        cfg.epsilon_percentile = epsilon_percentile;
        cfg.calibration_topk = top_k;
        cfg.calibration_percentile = 0.99f;
        cfg.centroids_path = centroids_path;
        cfg.assignments_path = assignments_path;
        cfg.crc_top_k = top_k;
        cfg.calibration_queries = qry.data.data();
        cfg.num_calibration_queries = Q;
        // Store vector index as INT64 payload for recall computation
        cfg.payload_schemas = {{0, "idx", DType::INT64, false}};

        PayloadFn payload_fn = [](uint32_t idx) -> std::vector<Datum> {
            return {Datum::Int64(static_cast<int64_t>(idx))};
        };

        IvfBuilder builder(cfg);
        builder.SetProgressCallback([&](uint32_t cluster, uint32_t total) {
            if ((cluster + 1) % (total / std::max(total / 8u, 1u)) == 0
                || cluster + 1 == total) {
                Log("  Build progress: cluster %u/%u\n", cluster + 1, total);
            }
        });

        auto t0 = std::chrono::steady_clock::now();
        Log("  Starting Build...\n");
        auto s = builder.Build(base.data.data(), N, dim, index_dir, payload_fn);
        build_time_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        if (!s.ok()) {
            std::fprintf(stderr, "Build failed: %s\n", s.ToString().c_str());
            return 1;
        }
        Log("  Build time: %.1f ms\n", build_time_ms);
    }

    // ================================================================
    // Phase C: Open Index + CRC Calibration
    // ================================================================
    Log("\n[Phase C] Opening index...\n");
    IvfIndex index;
    {
        auto s = index.Open(index_dir);
        if (!s.ok()) {
            std::fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
            return 1;
        }
    }
    Log("  eps_ip=%.6f  d_k=%.6f  nlist=%u  used_hadamard=%d\n",
        index.conann().epsilon(), index.conann().d_k(),
        index.nlist(), index.used_hadamard());

    CalibrationResults calib_results;
    if (use_crc) {
        Log("\n[Phase C.5] CRC calibration...\n");
        std::string scores_path = index_dir + "/crc_scores.bin";
        std::vector<QueryScores> crc_scores;
        uint32_t scores_nlist, scores_top_k;
        auto s = CrcCalibrator::ReadScores(scores_path, crc_scores,
                                           scores_nlist, scores_top_k);
        if (s.ok()) {
            Log("  Loaded %zu query scores (nlist=%u, top_k=%u)\n",
                crc_scores.size(), scores_nlist, scores_top_k);
            CrcCalibrator::Config crc_cfg;
            crc_cfg.alpha = crc_alpha;
            crc_cfg.top_k = scores_top_k;
            crc_cfg.calib_ratio = crc_calib;
            crc_cfg.tune_ratio = crc_tune;
            crc_cfg.seed = seed;
            auto [cr, er] = CrcCalibrator::Calibrate(crc_cfg, crc_scores, index.nlist());
            calib_results = cr;
            Log("  lamhat=%.6f  kreg=%u  reg_lambda=%.6f\n",
                calib_results.lamhat, calib_results.kreg,
                calib_results.reg_lambda);
            Log("  fnr=%.4f  avg_probed=%.1f  test_size=%u\n",
                er.actual_fnr, er.avg_probed, er.test_size);
        } else {
            Log("  No crc_scores.bin (disabling CRC)\n");
            use_crc = false;
        }
    }

    // ================================================================
    // Phase D: Query
    // ================================================================
    Log("\n[Phase D] Querying %u vectors...\n", Q);

    SearchConfig search_cfg;
    search_cfg.top_k = top_k;
    search_cfg.nprobe = nprobe;
    search_cfg.early_stop = early_stop;
    search_cfg.prefetch_depth = 16;
    search_cfg.io_queue_depth = 64;
    search_cfg.crc_params = use_crc ? &calib_results : nullptr;
    search_cfg.initial_prefetch = 16;
    search_cfg.refill_threshold = 4;
    search_cfg.refill_count = 8;

    IoUringReader reader;
    {
        auto s = reader.Init(64, 4096, /*iopoll=*/false);
        if (!s.ok()) {
            std::fprintf(stderr, "IoUringReader::Init failed: %s\n", s.ToString().c_str());
            return 1;
        }
    }
    {
        int fds[2] = {index.segment().clu_fd(),
                      index.segment().data_reader().fd()};
        auto s = reader.RegisterFiles(fds, 2);
        if (!s.ok()) {
            Log("  Warning: RegisterFiles failed: %s (continuing)\n",
                s.ToString().c_str());
        }
    }
    OverlapScheduler scheduler(index, reader, search_cfg);

    std::vector<std::vector<uint32_t>> results(Q);
    std::vector<double> latencies(Q);

    // Aggregate stats
    uint64_t s1_safein = 0, s1_safeout = 0, s1_uncertain = 0;
    uint64_t s2_safein = 0, s2_safeout = 0, s2_uncertain = 0;
    uint64_t final_uncertain = 0;
    uint64_t total_probed_clusters = 0;
    uint32_t early_stop_count = 0;

    for (uint32_t qi = 0; qi < Q; ++qi) {
        auto t0 = std::chrono::steady_clock::now();
        auto sr = scheduler.Search(qry.data.data() + static_cast<size_t>(qi) * dim);
        latencies[qi] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

        // Extract result vector indices from payload
        results[qi].reserve(sr.results().size());
        for (const auto& res : sr.results()) {
            if (!res.payload.empty()) {
                results[qi].push_back(
                    static_cast<uint32_t>(res.payload[0].fixed.i64));
            }
        }

        // Aggregate stats
        const auto& st = sr.stats();
        s1_safein       += st.total_safe_in;
        s1_safeout      += st.total_safe_out;
        s1_uncertain    += st.s1_uncertain_raw;
        s2_safein       += st.s2_safe_in;
        s2_safeout      += st.s2_safe_out;
        s2_uncertain    += st.s2_uncertain;
        final_uncertain += st.total_uncertain;
        total_probed_clusters += st.crc_clusters_probed > 0
            ? st.crc_clusters_probed
            : (static_cast<uint32_t>(nprobe) - st.clusters_skipped);
        if (st.early_stopped) early_stop_count++;

        if ((qi + 1) % (Q / std::max(Q / 4u, 1u)) == 0 || qi + 1 == Q) {
            Log("  Progress: %u/%u\n", qi + 1, Q);
        }
    }

    // Cleanup temp index if not keeping
    if (tmp_index && fs::exists(index_dir)) {
        fs::remove_all(index_dir);
        Log("\n[Cleanup] Removed temp index: %s\n", index_dir.c_str());
    }

    // ================================================================
    // Phase E: Recall computation
    // ================================================================
    auto ComputeRecall = [&](uint32_t at_k) -> double {
        if (at_k == 0 || gt_ids.empty()) return 0.0;
        uint32_t valid = 0;
        double sum = 0.0;
        for (uint32_t qi = 0; qi < Q && qi < gt_ids.size(); ++qi) {
            std::unordered_set<uint32_t> gt_set;
            uint32_t gt_limit = std::min(at_k, static_cast<uint32_t>(gt_ids[qi].size()));
            for (uint32_t i = 0; i < gt_limit; ++i)
                gt_set.insert(static_cast<uint32_t>(gt_ids[qi][i]));
            if (gt_set.empty()) continue;
            uint32_t hits = 0;
            uint32_t check_limit = std::min(at_k, static_cast<uint32_t>(results[qi].size()));
            for (uint32_t i = 0; i < check_limit; ++i)
                if (gt_set.count(results[qi][i])) hits++;
            sum += static_cast<double>(hits) / static_cast<double>(gt_set.size());
            valid++;
        }
        return valid > 0 ? sum / valid : 0.0;
    };

    double recall_1   = ComputeRecall(1);
    double recall_5   = ComputeRecall(std::min(5u, top_k));
    double recall_10  = ComputeRecall(std::min(10u, top_k));
    double recall_100 = ComputeRecall(std::min(100u, top_k));

    // Latency stats
    std::vector<double> sorted_lat = latencies;
    std::sort(sorted_lat.begin(), sorted_lat.end());
    double avg_lat = std::accumulate(sorted_lat.begin(), sorted_lat.end(), 0.0) / Q;
    double p50 = sorted_lat[Q / 2];
    double p95 = sorted_lat[static_cast<size_t>(Q * 0.95)];
    double p99 = sorted_lat[static_cast<size_t>(Q * 0.99)];

    double avg_probed = static_cast<double>(total_probed_clusters) / Q;
    double early_stop_rate = static_cast<double>(early_stop_count) / Q;

    uint64_t s1_total = s1_safein + s1_safeout + s1_uncertain;
    auto pct = [](uint64_t num, uint64_t den) -> double {
        return den > 0 ? 100.0 * static_cast<double>(num) / static_cast<double>(den) : 0.0;
    };

    // ================================================================
    // Phase F: Output
    // ================================================================
    Log("\n========================================\n");
    Log("RESULTS (bits=%u)\n", bits);
    Log("========================================\n");
    Log("  recall@1  = %.4f\n", recall_1);
    Log("  recall@5  = %.4f\n", recall_5);
    Log("  recall@10 = %.4f\n", recall_10);
    if (top_k >= 100)
        Log("  recall@100= %.4f\n", recall_100);
    Log("\n");
    Log("  latency avg = %.3f ms\n", avg_lat);
    Log("  latency p50 = %.3f ms\n", p50);
    Log("  latency p95 = %.3f ms\n", p95);
    Log("  latency p99 = %.3f ms\n", p99);
    Log("\n");
    Log("  build_time      = %.1f ms\n", build_time_ms);
    Log("  avg_probed      = %.2f / %u clusters\n", avg_probed, nprobe);
    Log("  early_stop_rate = %.4f\n", early_stop_rate);

    // Stage 1 Uncertain is the raw FastScan population that flows into Stage 2.
    // Stage 2 Uncertain is the unresolved remainder after ExRaBitQ re-classification.
    Log("\n  --- Stage 1 (FastScan) ---\n");
    Log("    SafeIn    = %lu (%.2f%%)\n", s1_safein, pct(s1_safein, s1_total));
    Log("    SafeOut   = %lu (%.2f%%)\n", s1_safeout, pct(s1_safeout, s1_total));
    Log("    Uncertain = %lu (%.2f%%)\n", s1_uncertain, pct(s1_uncertain, s1_total));

    if (bits > 1) {
        uint64_t s2_total = s2_safein + s2_safeout + s2_uncertain;
        Log("\n  --- Stage 2 (%u-bit ExRaBitQ) ---\n", bits);
        Log("    SafeIn    = %lu (%.2f%%)\n", s2_safein, pct(s2_safein, s2_total));
        Log("    SafeOut   = %lu (%.2f%%)\n", s2_safeout, pct(s2_safeout, s2_total));
        Log("    Uncertain = %lu (%.2f%%)\n", s2_uncertain, pct(s2_uncertain, s2_total));
        Log("    Final Uncertain = %lu\n", final_uncertain);
    }

    if (use_crc) {
        Log("\n  CRC stats:\n");
        Log("    lamhat     = %.6f\n", calib_results.lamhat);
    }
    Log("========================================\n");

    // JSON output
    if (!outdir.empty()) {
        fs::create_directories(outdir);
        std::string json_path = outdir + "/results.json";
        std::ofstream jf(json_path);
        jf << "{\n";
        jf << "  \"base\": \"" << base_path << "\",\n";
        jf << "  \"query\": \"" << query_path << "\",\n";
        jf << "  \"N\": " << N << ",\n";
        jf << "  \"Q\": " << Q << ",\n";
        jf << "  \"dim\": " << orig_dim << ",\n";
        jf << "  \"nlist\": " << nlist << ",\n";
        jf << "  \"nprobe\": " << nprobe << ",\n";
        jf << "  \"top_k\": " << top_k << ",\n";
        jf << "  \"bits\": " << static_cast<int>(bits) << ",\n";
        jf << "  \"crc\": " << (use_crc ? "true" : "false") << ",\n";
        jf << "  \"recall_at_1\": " << recall_1 << ",\n";
        jf << "  \"recall_at_5\": " << recall_5 << ",\n";
        jf << "  \"recall_at_10\": " << recall_10 << ",\n";
        jf << "  \"recall_at_100\": " << recall_100 << ",\n";
        jf << "  \"latency_avg_ms\": " << avg_lat << ",\n";
        jf << "  \"latency_p50_ms\": " << p50 << ",\n";
        jf << "  \"latency_p95_ms\": " << p95 << ",\n";
        jf << "  \"latency_p99_ms\": " << p99 << ",\n";
        jf << "  \"build_time_ms\": " << build_time_ms << ",\n";
        jf << "  \"avg_probed\": " << avg_probed << ",\n";
        jf << "  \"early_stop_rate\": " << early_stop_rate << "\n";
        jf << "}\n";
        jf.close();
        Log("\nJSON results written to: %s\n", json_path.c_str());
    }

    Log("\nDone.\n");
    return 0;
}
