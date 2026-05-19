/// bench_vector_search_inline.cpp — FROZEN BASELINE (vector-search-lib-unification Phase 0.5)
///
/// This file is a byte-for-byte copy of bench_vector_search.cpp at the state
/// after the third Phase B optimization round (encoder-mbit-fast-quantize +
/// eps-calibration-simd + calibration-omp-parallel). It is preserved as an
/// unchanged control group for the library-path refactor.
///
/// DO NOT add new features here. If a library API change breaks compilation,
/// apply only the minimal fix required to restore the build, or mark this
/// target as deprecated. The "new" bench_vector_search.cpp (Phase 4) will be
/// the thin disk-path bench that calls IvfBuilder + OverlapScheduler.
///
/// -----------------------------------------------------------------------
/// bench_vector_search.cpp — Pure vector search benchmark with M-bit RaBitQ.
///
/// Supports standard ANN-Benchmark datasets (.fvecs/.npy) without payload.
/// Pipeline: Load → KMeans → RaBitQ Encode → Calibrate → Probe+Rerank → Recall.
/// Supports two-stage ConANN classification (Stage 1: popcount, Stage 2: M-bit LUT).
/// Optionally integrates CRC early-stop in the cluster probing loop.
///
/// Usage:
///   bench_vector_search --base /path/to/base.fvecs --query /path/to/query.fvecs
///       [--gt /path/to/gt.ivecs] [--nlist 256] [--nprobe 32] [--topk 10]
///       [--bits 1] [--crc 1] [--crc-alpha 0.1] [--early-stop 1] [--outdir ./results]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "superkmeans/superkmeans.h"

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/fastscan.h"
#include <immintrin.h>
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/simd/popcount.h"
#include "vdb/simd/signed_dot.h"
#include "vdb/storage/pack_codes.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef VDB_USE_MKL
#include <mkl.h>
#endif

using namespace vdb;
using namespace vdb::rabitq;
using namespace vdb::index;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Power-of-2 padding helpers
// ---------------------------------------------------------------------------
static inline bool IsPowerOf2(uint32_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

static inline uint32_t NextPowerOf2(uint32_t n) {
    if (n == 0) return 1;
    if (IsPowerOf2(n)) return n;
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/// Pad each row of `data` from `old_dim` to `new_dim` with trailing zeros.
/// L2 distance, dot product, and norms are preserved (zeros contribute 0).
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
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
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

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// KMeans (SuperKMeans)
// ============================================================================

static void RunSuperKMeans(const float* data, uint32_t N, Dim dim, uint32_t K,
                           std::vector<float>& centroids,
                           std::vector<uint32_t>& assignments) {
    skmeans::SuperKMeansConfig cfg;
    cfg.iters = 10;
    cfg.seed = 42;
    cfg.verbose = false;
    auto skm = skmeans::SuperKMeans(K, dim, cfg);
    auto c = skm.Train(data, N);
    auto a = skm.Assign(data, c.data(), N, K);
    centroids.assign(c.begin(), c.end());
    assignments.assign(a.begin(), a.end());
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
    uint32_t p_for_dk  = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-dk", 99));
    uint32_t p_for_eps = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-eps", 99));
    uint8_t bits = static_cast<uint8_t>(GetIntArg(argc, argv, "--bits", 1));
    std::string centroids_path = GetArg(argc, argv, "--centroids", "");
    std::string assignments_path = GetArg(argc, argv, "--assignments", "");
    bool use_fastscan_classify = GetIntArg(argc, argv, "--use-fastscan", 1) != 0;
    bool pad_to_pow2 = GetIntArg(argc, argv, "--pad-to-pow2", 0) != 0;

    if (base_path.empty() || query_path.empty()) {
        std::fprintf(stderr,
            "Usage: bench_vector_search --base <path> --query <path> "
            "[--gt <path>] [--nlist N] [--nprobe N] [--topk K] "
            "[--crc 0|1] [--early-stop 0|1] [--outdir dir]\n");
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

    // Pad to next power of 2 if requested and dim is not already 2^k.
    // L2/IP/norms are preserved by zero padding, so recall should be unchanged.
    // This unlocks Hadamard rotation (O(L log L)) for non-power-of-2 dims.
    uint32_t orig_dim = dim;
    if (pad_to_pow2 && !IsPowerOf2(dim)) {
        uint32_t new_dim = NextPowerOf2(dim);
        Log("  [Padding] dim %u -> %u (next power of 2)\n", dim, new_dim);
        PadVectors(base.data, N, dim, new_dim);
        PadVectors(qry.data, qry.rows, dim, new_dim);
        dim = static_cast<Dim>(new_dim);
        base.cols = new_dim;
        qry.cols = new_dim;
    }

    // GT loading
    std::vector<std::vector<uint32_t>> gt_ids;  // [Q][gt_k]
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
            for (uint32_t i = 0; i < std::min(gt_k, N); ++i) {
                gt_ids[qi][i] = dists[i].second;
            }
        }
        Log("  Brute-force GT computed (k=%u).\n", gt_k);
    }

    // ================================================================
    // Phase B: Build — KMeans + RaBitQ Encode + calibrate ε_ip/d_k
    // ================================================================
    auto t_build = std::chrono::steady_clock::now();

    std::vector<float> centroids;
    std::vector<uint32_t> assignments;

    if (!centroids_path.empty() && !assignments_path.empty()) {
        Log("\n[Phase B] Loading precomputed centroids + assignments...\n");
        auto c_or = io::LoadVectors(centroids_path);
        if (!c_or.ok()) {
            std::fprintf(stderr, "Failed to load centroids: %s\n",
                         c_or.status().ToString().c_str());
            return 1;
        }
        auto& c = c_or.value();
        if (c.cols != dim) {
            // If padding is enabled and centroids match the original dim,
            // pad them to match.
            if (pad_to_pow2 && c.cols == orig_dim) {
                Log("  [Padding] centroids %u -> %u\n", c.cols, dim);
                PadVectors(c.data, c.rows, c.cols, dim);
                c.cols = dim;
            } else {
                std::fprintf(stderr, "Centroid dim mismatch: %u vs %u\n", c.cols, dim);
                return 1;
            }
        }
        nlist = c.rows;
        centroids.assign(c.data.begin(), c.data.end());

        auto a_or = io::LoadIvecs(assignments_path);
        if (!a_or.ok()) {
            std::fprintf(stderr, "Failed to load assignments: %s\n",
                         a_or.status().ToString().c_str());
            return 1;
        }
        auto& a = a_or.value();
        if (a.rows != N) {
            std::fprintf(stderr, "Assignment count mismatch: %u vs %u\n", a.rows, N);
            return 1;
        }
        assignments.resize(N);
        for (uint32_t i = 0; i < N; ++i) {
            assignments[i] = static_cast<uint32_t>(a.data[i]);
        }
        Log("  Loaded K=%u centroids, %u assignments.\n", nlist, N);
    } else {
        Log("\n[Phase B] KMeans (K=%u) + RaBitQ encoding...\n", nlist);
        RunSuperKMeans(base.data.data(), N, dim, nlist, centroids, assignments);

        // Export centroids and assignments if --outdir is set
        if (!outdir.empty()) {
            fs::create_directories(outdir);
            {
                std::string p = outdir + "/centroids.fvecs";
                std::ofstream f(p, std::ios::binary);
                for (uint32_t k = 0; k < nlist; ++k) {
                    f.write(reinterpret_cast<const char*>(&dim), sizeof(int32_t));
                    f.write(reinterpret_cast<const char*>(
                        centroids.data() + static_cast<size_t>(k) * dim),
                        dim * sizeof(float));
                }
                Log("  Exported centroids -> %s\n", p.c_str());
            }
            {
                std::string p = outdir + "/assignments.ivecs";
                std::ofstream f(p, std::ios::binary);
                int32_t one = 1;
                for (uint32_t i = 0; i < N; ++i) {
                    f.write(reinterpret_cast<const char*>(&one), sizeof(int32_t));
                    int32_t a = static_cast<int32_t>(assignments[i]);
                    f.write(reinterpret_cast<const char*>(&a), sizeof(int32_t));
                }
                Log("  Exported assignments -> %s\n", p.c_str());
            }
        }
    }

#ifdef VDB_USE_MKL
    // Precompute ||c||² for MKL-accelerated centroid distance
    std::vector<float> centroid_norms(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        const float* cv = centroids.data() + static_cast<size_t>(c) * dim;
        centroid_norms[c] = cblas_sdot(dim, cv, 1, cv, 1);
    }
    Log("  Precomputed centroid norms (MKL).\n");
#endif

    // Diagnostic: check centroid quality
    {
        float c0_norm = 0;
        for (uint32_t d = 0; d < dim; ++d) c0_norm += centroids[d] * centroids[d];
        c0_norm = std::sqrt(c0_norm);
        float v0_norm = 0;
        for (uint32_t d = 0; d < dim; ++d) v0_norm += base.data[d] * base.data[d];
        v0_norm = std::sqrt(v0_norm);
        float dist_v0_c0 = 0;
        uint32_t a0 = assignments[0];
        for (uint32_t d = 0; d < dim; ++d) {
            float diff = base.data[d] - centroids[static_cast<size_t>(a0)*dim + d];
            dist_v0_c0 += diff * diff;
        }
        Log("  [DIAG] centroid[0] norm=%.4f, vec[0] norm=%.4f, ||vec[0]-centroid[a[0]]||=%.4f\n",
            c0_norm, v0_norm, std::sqrt(dist_v0_c0));
    }

    RotationMatrix rotation(dim);
    // Prefer Hadamard rotation when dim is a power of 2 (O(L log L) Apply
    // via FWHT instead of O(L²) matrix multiply). Falls back to random
    // Gaussian QR for non-power-of-2 dims.
    bool used_hadamard = (dim > 0 && (dim & (dim - 1)) == 0)
                       && rotation.GenerateHadamard(seed, /*use_fast_transform=*/true);
    if (!used_hadamard) {
        rotation.GenerateRandom(seed);
    }
    Log("  Rotation type: %s\n", used_hadamard ? "Hadamard (O(L log L))"
                                              : "Random Gaussian (O(L²))");
    RaBitQEncoder encoder(dim, rotation, bits);
    RaBitQEstimator estimator(dim, bits);

    std::vector<RaBitQCode> codes(N);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t k = assignments[i];
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        codes[i] = encoder.Encode(
            base.data.data() + static_cast<size_t>(i) * dim, centroid);
    }

    // Cluster members
    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    // Per-cluster r_max
    std::vector<float> per_cluster_r_max(nlist, 0.0f);
    for (uint32_t k = 0; k < nlist; ++k) {
        for (uint32_t idx : cluster_members[k]) {
            per_cluster_r_max[k] = std::max(per_cluster_r_max[k], codes[idx].norm);
        }
    }

    // ε_ip calibration (from intra-cluster inner-product error)
    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    const uint32_t num_words = (dim + 63) / 64;
    uint32_t samples_for_eps = 100;

    // Popcount IP-error calibration (OMP-parallelized, thread-local results)
#ifdef _OPENMP
    const int nt_ip = omp_get_max_threads();
#else
    const int nt_ip = 1;
#endif
    std::vector<std::vector<float>> tl_ip_errors(nt_ip);

#pragma omp parallel
    {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        auto& local = tl_ip_errors[tid];
        local.reserve(samples_for_eps * 300);

#pragma omp for schedule(dynamic, 16)
        for (int k = 0; k < static_cast<int>(nlist); ++k) {
            const auto& members = cluster_members[k];
            if (members.size() < 2) continue;
            uint32_t n_queries = std::min(samples_for_eps,
                                           static_cast<uint32_t>(members.size()));
            const float* centroid = centroids.data() +
                static_cast<size_t>(k) * dim;
            std::vector<uint32_t> indices(members.size());
            std::iota(indices.begin(), indices.end(), 0u);
            std::mt19937 rng(seed + static_cast<uint32_t>(k));
            std::shuffle(indices.begin(), indices.end(), rng);

            for (uint32_t q = 0; q < n_queries; ++q) {
                uint32_t q_global = members[indices[q]];
                const float* q_vec = base.data.data() +
                    static_cast<size_t>(q_global) * dim;
                auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);
                for (uint32_t t = 0; t < static_cast<uint32_t>(members.size());
                     ++t) {
                    if (t == indices[q]) continue;
                    uint32_t t_global = members[t];
                    const auto& code = codes[t_global];
                    uint32_t hamming = simd::PopcountXor(
                        pq.sign_code.data(), code.code.data(), num_words);
                    float ip_hat = 1.0f - 2.0f * static_cast<float>(hamming) /
                                                  static_cast<float>(dim);
                    float dot = simd::SignedDotFromBits(
                        pq.rotated.data(), code.code.data(), dim);
                    float ip_accurate = dot * inv_sqrt_dim;
                    local.push_back(std::abs(ip_hat - ip_accurate));
                }
            }
        }
    }

    // Merge thread-local ip_errors
    std::vector<float> ip_errors;
    {
        size_t total_ip = 0;
        for (const auto& v : tl_ip_errors) total_ip += v.size();
        ip_errors.reserve(total_ip);
        for (auto& v : tl_ip_errors)
            ip_errors.insert(ip_errors.end(), v.begin(), v.end());
    }

    float eps_ip = 0.0f;
    if (!ip_errors.empty()) {
        std::sort(ip_errors.begin(), ip_errors.end());
        float p = static_cast<float>(p_for_eps) / 100.0f;
        auto idx = static_cast<size_t>(
            p * static_cast<float>(ip_errors.size() - 1));
        eps_ip = ip_errors[idx];
    }
    Log("  eps_ip = %.6f (P%u)\n", eps_ip, p_for_eps);

    // ---- FastScan block packing (needed for --use-fastscan) ----
    // Pack each cluster's sign bits into FastScan batch-32 blocks.
    const uint32_t fastscan_packed_sz = storage::FastScanPackedSize(dim);
    const uint32_t fastscan_block_bytes = fastscan_packed_sz + 32 * sizeof(float);  // codes + norms

    // Per-cluster: vector of packed blocks (each block = 32 vectors)
    struct FsBlock {
        std::vector<uint8_t> packed;  // fastscan_packed_sz bytes
        std::vector<float> norms;     // 32 floats
        uint32_t count;               // actual vectors in this block (≤32)
    };
    std::vector<std::vector<FsBlock>> cluster_fs_blocks(nlist);

    {
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            const auto& members = cluster_members[cid];
            uint32_t n_blocks = (static_cast<uint32_t>(members.size()) + 31) / 32;
            cluster_fs_blocks[cid].resize(n_blocks);

            for (uint32_t b = 0; b < n_blocks; ++b) {
                uint32_t base_idx = b * 32;
                uint32_t cnt = std::min(32u, static_cast<uint32_t>(members.size()) - base_idx);

                auto& fb = cluster_fs_blocks[cid][b];
                fb.packed.resize(fastscan_packed_sz, 0);
                fb.norms.resize(32, 0.0f);
                fb.count = cnt;

                // Collect codes for this block
                std::vector<RaBitQCode> block_codes(cnt);
                for (uint32_t j = 0; j < cnt; ++j) {
                    block_codes[j] = codes[members[base_idx + j]];
                    fb.norms[j] = codes[members[base_idx + j]].norm;
                }
                storage::PackSignBitsForFastScan(
                    block_codes.data(), cnt, dim, fb.packed.data());
            }
        }
        Log("  FastScan blocks packed (%u clusters).\n", nlist);
    }

    // ---- Precompute rotated centroids (Hadamard path only) ----
    // rotated_centroids[k] = P^T × centroids[k], computed once at build time.
    // Phase D uses these to skip per-cluster FWHT: diff = rotated_q - rotated_c[k].
    std::vector<float> rotated_centroids;
    if (used_hadamard) {
        auto t_rc = std::chrono::steady_clock::now();
        rotated_centroids.resize(static_cast<size_t>(nlist) * dim, 0.0f);
        for (uint32_t k = 0; k < nlist; ++k) {
            rotation.Apply(
                centroids.data() + static_cast<size_t>(k) * dim,
                rotated_centroids.data() + static_cast<size_t>(k) * dim);
        }
        double rc_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_rc).count();
        Log("  Precomputed rotated centroids (%u × %u floats, %.1f ms).\n",
            nlist, dim, rc_ms);
        // Numerical check: ||rotated_c[0]|| ≈ ||c[0]|| (orthogonal transform preserves norm)
        float norm_rc0 = 0.0f, norm_c0 = 0.0f;
        for (uint32_t i = 0; i < dim; ++i) {
            norm_rc0 += rotated_centroids[i] * rotated_centroids[i];
            norm_c0  += centroids[i] * centroids[i];
        }
        Log("  [CHECK] ||rotated_c[0]||=%.6f, ||c[0]||=%.6f (diff=%.2e)\n",
            std::sqrt(norm_rc0), std::sqrt(norm_c0),
            std::abs(std::sqrt(norm_rc0) - std::sqrt(norm_c0)));
    }

    // d_k calibration (must be before eps_ip_fs for truncation)
    float p_dk_f = static_cast<float>(p_for_dk) / 100.0f;
    float d_k = ConANN::CalibrateDistanceThreshold(
        qry.data.data(), Q, base.data.data(), N,
        dim, 100, top_k, p_dk_f, seed);
    Log("  d_k = %.4f (P%u)\n", d_k, p_for_dk);

    // ---- FastScan distance-error calibration (normalized, d_k truncated) ----
    float eps_ip_fs = 0.0f;
    {
        const float trunc_lo = 0.1f * d_k;
        const float trunc_hi = 10.0f * d_k;
        // OMP-parallelized: thread-local error vectors, merged after
#ifdef _OPENMP
        const int nt_fs = omp_get_max_threads();
#else
        const int nt_fs = 1;
#endif
        std::vector<std::vector<float>> tl_fs_errors(nt_fs);

#pragma omp parallel
        {
#ifdef _OPENMP
            int tid_fs = omp_get_thread_num();
#else
            int tid_fs = 0;
#endif
            auto& local_fs = tl_fs_errors[tid_fs];
            local_fs.reserve(samples_for_eps * 300);

#pragma omp for schedule(dynamic, 16)
            for (int k = 0; k < static_cast<int>(nlist); ++k) {
                const auto& members = cluster_members[k];
                if (members.size() < 2) continue;

                uint32_t n_queries_fs = std::min(
                    samples_for_eps, static_cast<uint32_t>(members.size()));
                const float* centroid = centroids.data() +
                    static_cast<size_t>(k) * dim;
                std::vector<uint32_t> indices(members.size());
                std::iota(indices.begin(), indices.end(), 0u);
                std::mt19937 rng(seed + static_cast<uint32_t>(k) + nlist);
                std::shuffle(indices.begin(), indices.end(), rng);

                for (uint32_t q = 0; q < n_queries_fs; ++q) {
                    uint32_t q_global = members[indices[q]];
                    const float* q_vec = base.data.data() +
                        static_cast<size_t>(q_global) * dim;
                    auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);

                    for (uint32_t b = 0;
                         b < static_cast<uint32_t>(cluster_fs_blocks[k].size());
                         ++b) {
                        const auto& fb = cluster_fs_blocks[k][b];
                        alignas(64) float fs_dists[32];
                        estimator.EstimateDistanceFastScan(
                            pq, fb.packed.data(), fb.norms.data(),
                            fb.count, fs_dists);

                        for (uint32_t j = 0; j < fb.count; ++j) {
                            uint32_t local_idx = b * 32 + j;
                            if (local_idx == indices[q]) continue;

                            uint32_t t_global = members[local_idx];
                            float norm_oc = fb.norms[j];

                            float true_dist = simd::L2Sqr(q_vec,
                                base.data.data() +
                                    static_cast<size_t>(t_global) * dim, dim);

                            // Truncate: only sample near decision boundary
                            if (true_dist < trunc_lo ||
                                true_dist > trunc_hi) continue;

                            float dist_err = std::abs(fs_dists[j] - true_dist);
                            float denom = 2.0f * norm_oc * pq.norm_qc;
                            if (denom > 1e-10f) {
                                local_fs.push_back(dist_err / denom);
                            }
                        }
                    }
                }
            }
        }

        // Merge thread-local fs errors
        std::vector<float> fs_normalized_errors;
        {
            size_t total_fs = 0;
            for (const auto& v : tl_fs_errors) total_fs += v.size();
            fs_normalized_errors.reserve(total_fs);
            for (auto& v : tl_fs_errors)
                fs_normalized_errors.insert(
                    fs_normalized_errors.end(), v.begin(), v.end());
        }

        if (!fs_normalized_errors.empty()) {
            std::sort(fs_normalized_errors.begin(), fs_normalized_errors.end());
            float p = static_cast<float>(p_for_eps) / 100.0f;
            auto idx = static_cast<size_t>(
                p * static_cast<float>(fs_normalized_errors.size() - 1));
            eps_ip_fs = fs_normalized_errors[idx];
        }
        Log("  eps_ip_fs = %.6f (P%u, %zu samples, trunc=[%.4f,%.4f])  [vs eps_ip_pop = %.6f]\n",
            eps_ip_fs, p_for_eps, fs_normalized_errors.size(), trunc_lo, trunc_hi, eps_ip);
    }

    double build_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_build).count();
    Log("  Build complete (%.1f ms).\n", build_ms);

    // ================================================================
    // Phase C: CRC Calibration (optional)
    // ================================================================
    CrcStopper crc_stopper;
    CalibrationResults calib_results;
    bool crc_active = false;

    if (use_crc) {
        Log("\n[Phase C] CRC calibration (inline RaBitQ)...\n");
        auto t_crc = std::chrono::steady_clock::now();

        // Build ClusterData for CalibrateWithRaBitQ
        // Pack codes into contiguous blocks per cluster
        uint32_t code_words = (dim + 63) / 64;
        uint32_t code_entry_size = code_words * sizeof(uint64_t) + sizeof(float) + sizeof(uint32_t);

        std::vector<std::vector<uint8_t>> cluster_code_blocks(nlist);
        std::vector<std::vector<uint32_t>> cluster_ids(nlist);

        for (uint32_t cid = 0; cid < nlist; ++cid) {
            const auto& members = cluster_members[cid];
            cluster_ids[cid] = members;
            cluster_code_blocks[cid].resize(members.size() * code_entry_size);

            for (size_t j = 0; j < members.size(); ++j) {
                uint8_t* dst = cluster_code_blocks[cid].data() + j * code_entry_size;
                const auto& code = codes[members[j]];
                // Pack: code words || norm || vector_id
                std::memcpy(dst, code.code.data(), code_words * sizeof(uint64_t));
                dst += code_words * sizeof(uint64_t);
                std::memcpy(dst, &code.norm, sizeof(float));
                dst += sizeof(float);
                uint32_t vid = members[j];
                std::memcpy(dst, &vid, sizeof(uint32_t));
            }
        }

        std::vector<ClusterData> clusters(nlist);
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            clusters[cid].vectors = nullptr;
            clusters[cid].ids = cluster_ids[cid].data();
            clusters[cid].count = static_cast<uint32_t>(cluster_members[cid].size());
            clusters[cid].codes_block = cluster_code_blocks[cid].data();
            clusters[cid].code_entry_size = code_entry_size;
        }

        // Fill pre-packed FastScan block metadata from Phase B cluster_fs_blocks.
        // This avoids redundant PackSignBitsForFastScan calls in ComputeScoresForQueryRaBitQ.
        std::vector<std::vector<ClusterData::FsBlock>> cluster_fs_meta(nlist);
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            const auto& fbs = cluster_fs_blocks[cid];
            cluster_fs_meta[cid].resize(fbs.size());
            for (size_t b = 0; b < fbs.size(); ++b) {
                cluster_fs_meta[cid][b] = ClusterData::FsBlock{
                    fbs[b].packed.data(),
                    fbs[b].norms.data(),
                    fbs[b].count
                };
            }
            clusters[cid].fs_blocks    = cluster_fs_meta[cid].data();
            clusters[cid].num_fs_blocks = static_cast<uint32_t>(cluster_fs_meta[cid].size());
        }

        CrcCalibrator::Config crc_cfg;
        crc_cfg.alpha = crc_alpha;
        crc_cfg.top_k = top_k;
        crc_cfg.calib_ratio = crc_calib;
        crc_cfg.tune_ratio = crc_tune;
        crc_cfg.seed = seed;

        auto [cal, eval] = CrcCalibrator::CalibrateWithRaBitQ(
            crc_cfg, qry.data.data(), Q, dim,
            centroids.data(), nlist, clusters, rotation);
        calib_results = cal;
        crc_stopper = CrcStopper(calib_results, nlist);
        crc_active = (cal.lamhat < 1.0f);

        double crc_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_crc).count();

        Log("  CRC done (%.1f ms)\n", crc_ms);
        Log("  lamhat=%.6f  kreg=%u  reg_lambda=%.6f\n",
            cal.lamhat, cal.kreg, cal.reg_lambda);
        Log("  d_min=%.6f  d_max=%.6f\n", cal.d_min, cal.d_max);
        Log("  eval: FNR=%.4f  avg_probed=%.1f  test_size=%u\n",
            eval.actual_fnr, eval.avg_probed, eval.test_size);
        Log("  CRC active: %s\n", crc_active ? "yes" : "no (lamhat >= 1.0)");
    }

    // ================================================================
    // Build SOA layout for Stage 2 code data (contiguous, cache-friendly)
    // ================================================================
    // Interleave norm + xipnorm into a single 8-byte struct so a single
    // cache-line load gets both values needed by Stage 2.
    struct alignas(8) NormPair { float norm; float xipnorm; };
    std::vector<uint8_t>   soa_ex_code(static_cast<size_t>(N) * dim);
    const uint32_t ex_sign_bytes = (dim + 7) / 8;
    std::vector<uint8_t>   soa_ex_sign(static_cast<size_t>(N) * ex_sign_bytes);
    std::vector<NormPair>  soa_norm_pairs(N);
    for (uint32_t i = 0; i < N; ++i) {
        const auto& c = codes[i];
        soa_norm_pairs[i] = NormPair{c.norm, c.xipnorm};
        if (!c.ex_code.empty()) {
            std::memcpy(soa_ex_code.data() + static_cast<size_t>(i) * dim,
                        c.ex_code.data(), dim);
            std::memcpy(soa_ex_sign.data() + static_cast<size_t>(i) * ex_sign_bytes,
                        c.ex_sign_packed.data(), ex_sign_bytes);
        }
    }
    Log("  SOA code layout built (%u vectors, %.1f MB).\n",
        N, static_cast<double>(soa_ex_code.size() + soa_ex_sign.size()
                              + soa_norm_pairs.size() * sizeof(NormPair)) / 1e6);

    // ================================================================
    // Phase D: Vector Search
    // ================================================================
    Log("\n[Phase D] Searching (%u queries, nprobe=%u, fastscan_classify=%d)...\n",
        Q, nprobe, use_fastscan_classify ? 1 : 0);
    auto t_search = std::chrono::steady_clock::now();

    float active_eps_ip = use_fastscan_classify ? eps_ip_fs : eps_ip;
    ConANN conann(active_eps_ip, d_k);
    Log("  Using eps_ip = %.6f (%s)\n", active_eps_ip,
        use_fastscan_classify ? "FastScan" : "Popcount");

    // Results: per-query top-K IDs
    std::vector<std::vector<uint32_t>> results(Q);
    std::vector<double> latencies(Q);

    // Stats — Stage 1
    uint64_t s1_safein = 0, s1_safeout = 0, s1_uncertain = 0;
    // Stats — Stage 2 (applied to S1 Uncertain when bits > 1)
    uint64_t s2_safein = 0, s2_safeout = 0, s2_uncertain = 0;
    // Final (after both stages)
    uint64_t final_safein = 0, final_safeout = 0, final_uncertain = 0;
    // False SafeIn/Out (against GT)
    uint64_t final_false_safein = 0, final_false_safeout = 0;
    uint64_t total_probed_clusters = 0;
    uint32_t early_stop_count = 0;

    const float margin_s2_divisor = static_cast<float>(1u << (bits - 1));

    // Pre-allocate per-query temporaries (reused across queries)
    std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
#ifdef VDB_USE_MKL
    std::vector<float> qc(nlist);
#endif
    std::vector<std::pair<float, uint32_t>> est_heap;
    std::vector<std::pair<float, uint32_t>> exact_heap;
    est_heap.reserve(top_k * 2);
    exact_heap.reserve(top_k * 2);
    PreparedQuery pq;  // Reused across clusters via PrepareQueryInto
    // Per-query rotated query buffer (only used in Hadamard fast path).
    std::vector<float> rotated_q(used_hadamard ? dim : 0);

    for (uint32_t qi = 0; qi < Q; ++qi) {
        auto t_q = std::chrono::steady_clock::now();
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;

        // Sort clusters by centroid distance
#ifdef VDB_USE_MKL
        // MKL: ||q-c||² = ||q||² + ||c||² - 2·(q·c)
        cblas_sgemv(CblasRowMajor, CblasNoTrans,
                    nlist, dim,
                    1.0f, centroids.data(), dim,
                    q_vec, 1,
                    0.0f, qc.data(), 1);
        const float q_norm = cblas_sdot(dim, q_vec, 1, q_vec, 1);
        for (uint32_t c = 0; c < nlist; ++c) {
            centroid_dists[c] = {q_norm + centroid_norms[c] - 2.0f * qc[c], c};
        }
#else
        for (uint32_t c = 0; c < nlist; ++c) {
            centroid_dists[c] = {
                simd::L2Sqr(q_vec,
                    centroids.data() + static_cast<size_t>(c) * dim, dim),
                c};
        }
#endif
        // Only need top-nprobe; use partial_sort to avoid O(N log N) full sort
        const uint32_t actual_nprobe = std::min(nprobe, nlist);
        std::partial_sort(centroid_dists.begin(),
                          centroid_dists.begin() + actual_nprobe,
                          centroid_dists.end());

        // Dual heaps: max-heap (largest at front) — reuse allocated capacity
        est_heap.clear();
        exact_heap.clear();

        // One-time query rotation (Hadamard path): rotated_q = P^T × q.
        // Per-cluster FWHT is then replaced by O(dim) vector subtraction.
        if (used_hadamard) {
            rotation.Apply(q_vec, rotated_q.data());
        }

        uint32_t probed = 0;
        bool stopped_early = false;

        for (uint32_t p = 0; p < nprobe && p < nlist; ++p) {
            uint32_t cid = centroid_dists[p].second;
            if (used_hadamard) {
                const float* rotated_c =
                    rotated_centroids.data() + static_cast<size_t>(cid) * dim;
                estimator.PrepareQueryRotatedInto(
                    rotated_q.data(), rotated_c, &pq, nullptr, nullptr);
            } else {
                const float* centroid = centroids.data() +
                    static_cast<size_t>(cid) * dim;
                estimator.PrepareQueryInto(
                    q_vec, centroid, rotation, &pq, nullptr, nullptr);
            }
            float margin_factor = 2.0f * pq.norm_qc * active_eps_ip;

            // --- FastScan classify path: batch-32 blocks ---
            if (use_fastscan_classify) {
                const auto& blocks = cluster_fs_blocks[cid];
                const auto& members = cluster_members[cid];

                // Cache est_kth across vectors; only refresh when est_heap mutates.
                float est_kth = (est_heap.size() >= top_k)
                    ? est_heap.front().first
                    : std::numeric_limits<float>::infinity();

                for (uint32_t b = 0; b < blocks.size(); ++b) {
                    const auto& fb = blocks[b];
                    alignas(64) float fs_dists[32];
                    estimator.EstimateDistanceFastScan(
                        pq, fb.packed.data(), fb.norms.data(), fb.count, fs_dists);

                    // Phase 1: batch SafeOut classification via SIMD bitmask.
                    // Bit v in so_mask is 1 iff lane v is SafeOut.
                    uint32_t so_mask = simd::FastScanSafeOutMask(
                        fs_dists, fb.norms.data(), fb.count, est_kth, margin_factor);
                    uint32_t so_count = static_cast<uint32_t>(__builtin_popcount(so_mask));
                    s1_safeout += so_count;
                    final_safeout += so_count;

                    // Phase 2: process only non-SafeOut lanes via ctz iteration.
                    uint32_t lane_valid = (fb.count >= 32)
                        ? 0xFFFFFFFFu
                        : ((1u << fb.count) - 1u);
                    uint32_t maybe_in = (~so_mask) & lane_valid;

                    while (maybe_in) {
                        uint32_t j = static_cast<uint32_t>(__builtin_ctz(maybe_in));
                        maybe_in &= maybe_in - 1u;

                        uint32_t local_idx = b * 32 + j;
                        uint32_t vid = members[local_idx];

                        // Prefetch next candidate's base vector while processing
                        // current one. Stage 2 IPExRaBitQ provides ~200+ cycles
                        // of compute for the prefetch to complete in background.
                        if (maybe_in) {
                            uint32_t j_next = static_cast<uint32_t>(__builtin_ctz(maybe_in));
                            uint32_t vid_next = members[(b * 32) + j_next];
                            const char* base_next = reinterpret_cast<const char*>(
                                base.data.data() + static_cast<size_t>(vid_next) * dim);
                            _mm_prefetch(base_next,       _MM_HINT_T0);
                            _mm_prefetch(base_next + 64,  _MM_HINT_T0);
                            //_mm_prefetch(base_next + 128, _MM_HINT_T1);
                        }

                        float est_dist_s1 = fs_dists[j];
                        float margin_s1 = margin_factor * fb.norms[j];

                        // Reclassify the non-SafeOut lane as SafeIn or Uncertain.
                        // (SafeOut already removed via mask.)
                        ResultClass rc_s1;
                        if (est_dist_s1 < d_k - 2.0f * margin_s1) {
                            rc_s1 = ResultClass::SafeIn;
                            s1_safein++;
                        } else {
                            rc_s1 = ResultClass::Uncertain;
                            s1_uncertain++;
                        }

                        ResultClass rc_final = rc_s1;

                        // Stage 2: for S1-Uncertain when bits > 1
                        if (rc_s1 == ResultClass::Uncertain && bits > 1) {
                            // SOA path: contiguous memory, no pointer indirection
                            const uint8_t* ec = soa_ex_code.data() + static_cast<size_t>(vid) * dim;
                            const uint8_t* es = soa_ex_sign.data() + static_cast<size_t>(vid) * ex_sign_bytes;
                            _mm_prefetch(reinterpret_cast<const char*>(&soa_norm_pairs[vid]), _MM_HINT_T0);
                            float ip_raw = simd::IPExRaBitQ(pq.rotated.data(), ec, es, true, dim);
                            NormPair np = soa_norm_pairs[vid];  // single 8-byte load
                            float ip_est = ip_raw * np.xipnorm;
                            float norm_oc = np.norm;
                            float est_dist_s2 = norm_oc * norm_oc + pq.norm_qc_sq
                                              - 2.0f * norm_oc * pq.norm_qc * ip_est;
                            est_dist_s2 = std::max(est_dist_s2, 0.0f);
                            float margin_s2 = margin_s1 / margin_s2_divisor;
                            ResultClass rc_s2 = conann.ClassifyAdaptive(est_dist_s2, margin_s2, est_kth);

                            if (rc_s2 == ResultClass::SafeIn) s2_safein++;
                            else if (rc_s2 == ResultClass::SafeOut) s2_safeout++;
                            else s2_uncertain++;

                            rc_final = rc_s2;
                        }

                        if (rc_final == ResultClass::SafeIn) final_safein++;
                        else if (rc_final == ResultClass::SafeOut) { final_safeout++; continue; }
                        else final_uncertain++;

                        // Only non-SafeOut reach here (SafeIn + Uncertain)

                        bool est_heap_modified = false;
                        if (est_heap.size() < top_k) {
                            est_heap.push_back({est_dist_s1, vid});
                            std::push_heap(est_heap.begin(), est_heap.end());
                            est_heap_modified = true;
                        } else if (est_dist_s1 < est_heap.front().first) {
                            std::pop_heap(est_heap.begin(), est_heap.end());
                            est_heap.back() = {est_dist_s1, vid};
                            std::push_heap(est_heap.begin(), est_heap.end());
                            est_heap_modified = true;
                        }
                        if (est_heap_modified && est_heap.size() >= top_k) {
                            est_kth = est_heap.front().first;
                        }

                        float exact_dist = simd::L2Sqr(q_vec,
                            base.data.data() + static_cast<size_t>(vid) * dim, dim);
                        if (exact_heap.size() < top_k) {
                            exact_heap.push_back({exact_dist, vid});
                            std::push_heap(exact_heap.begin(), exact_heap.end());
                        } else if (exact_dist < exact_heap.front().first) {
                            std::pop_heap(exact_heap.begin(), exact_heap.end());
                            exact_heap.back() = {exact_dist, vid};
                            std::push_heap(exact_heap.begin(), exact_heap.end());
                        }
                    }
                }
            } else {
            // --- Original popcount classify path ---
            // Cache est_kth across vectors; only refresh on heap mutation.
            float est_kth = (est_heap.size() >= top_k)
                ? est_heap.front().first
                : std::numeric_limits<float>::infinity();
            for (uint32_t vid : cluster_members[cid]) {
                float est_dist_s1 = estimator.EstimateDistance(pq, codes[vid]);
                float margin_s1 = margin_factor * codes[vid].norm;  // per-vector

                ResultClass rc_s1 = conann.ClassifyAdaptive(est_dist_s1, margin_s1, est_kth);

                if (rc_s1 == ResultClass::SafeIn) s1_safein++;
                else if (rc_s1 == ResultClass::SafeOut) { s1_safeout++; final_safeout++; continue; }
                else s1_uncertain++;

                ResultClass rc_final = rc_s1;

                // Stage 2: only for S1-Uncertain when bits > 1
                if (rc_s1 == ResultClass::Uncertain && bits > 1) {
                    const uint8_t* ec = soa_ex_code.data() + static_cast<size_t>(vid) * dim;
                    const uint8_t* es = soa_ex_sign.data() + static_cast<size_t>(vid) * ex_sign_bytes;
                    _mm_prefetch(reinterpret_cast<const char*>(&soa_norm_pairs[vid]), _MM_HINT_T0);
                    float ip_raw = simd::IPExRaBitQ(pq.rotated.data(), ec, es, true, dim);
                    NormPair np = soa_norm_pairs[vid];  // single 8-byte load
                    float ip_est = ip_raw * np.xipnorm;
                    float norm_oc_s2 = np.norm;
                    float est_dist_s2 = norm_oc_s2 * norm_oc_s2 + pq.norm_qc_sq
                                      - 2.0f * norm_oc_s2 * pq.norm_qc * ip_est;
                    est_dist_s2 = std::max(est_dist_s2, 0.0f);
                    float margin_s2 = margin_s1 / margin_s2_divisor;
                    ResultClass rc_s2 = conann.ClassifyAdaptive(est_dist_s2, margin_s2, est_kth);

                    if (rc_s2 == ResultClass::SafeIn) s2_safein++;
                    else if (rc_s2 == ResultClass::SafeOut) s2_safeout++;
                    else s2_uncertain++;

                    rc_final = rc_s2;
                }

                if (rc_final == ResultClass::SafeIn) final_safein++;
                else if (rc_final == ResultClass::SafeOut) { final_safeout++; continue; }
                else final_uncertain++;

                // SafeIn and Uncertain: update est_heap
                bool est_heap_modified = false;
                if (est_heap.size() < top_k) {
                    est_heap.push_back({est_dist_s1, vid});
                    std::push_heap(est_heap.begin(), est_heap.end());
                    est_heap_modified = true;
                } else if (est_dist_s1 < est_heap.front().first) {
                    std::pop_heap(est_heap.begin(), est_heap.end());
                    est_heap.back() = {est_dist_s1, vid};
                    std::push_heap(est_heap.begin(), est_heap.end());
                    est_heap_modified = true;
                }
                if (est_heap_modified && est_heap.size() >= top_k) {
                    est_kth = est_heap.front().first;
                }

                // Rerank with exact L2 (both SafeIn and Uncertain read vectors)
                float exact_dist = simd::L2Sqr(q_vec,
                    base.data.data() + static_cast<size_t>(vid) * dim, dim);
                if (exact_heap.size() < top_k) {
                    exact_heap.push_back({exact_dist, vid});
                    std::push_heap(exact_heap.begin(), exact_heap.end());
                } else if (exact_dist < exact_heap.front().first) {
                    std::pop_heap(exact_heap.begin(), exact_heap.end());
                    exact_heap.back() = {exact_dist, vid};
                    std::push_heap(exact_heap.begin(), exact_heap.end());
                }
            }
            } // end popcount path

            probed++;

            // Early stop: CRC or legacy d_k
            if (use_crc && crc_active) {
                float est_kth = (est_heap.size() >= top_k)
                    ? est_heap.front().first
                    : std::numeric_limits<float>::infinity();
                if (crc_stopper.ShouldStop(probed, est_kth)) {
                    stopped_early = true;
                    break;
                }
            } else if (early_stop) {
                if (exact_heap.size() >= top_k && exact_heap.front().first < d_k) {
                    stopped_early = true;
                    break;
                }
            }
        }

        total_probed_clusters += probed;
        if (stopped_early) early_stop_count++;

        // Extract top-K from exact_heap, sorted by distance (ascending)
        std::sort_heap(exact_heap.begin(), exact_heap.end());
        results[qi].resize(exact_heap.size());
        for (size_t i = 0; i < exact_heap.size(); ++i) {
            results[qi][i] = exact_heap[i].second;
        }

        latencies[qi] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_q).count();
    }

    double search_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_search).count();
    Log("  Search complete (%.1f ms).\n", search_ms);

    // ================================================================
    // Phase E: Recall & Statistics
    // ================================================================
    Log("\n[Phase E] Computing recall...\n");

    auto ComputeRecall = [&](uint32_t at_k) -> double {
        if (at_k == 0) return 0.0;
        uint32_t valid = 0;
        double sum = 0.0;
        for (uint32_t qi = 0; qi < Q && qi < gt_ids.size(); ++qi) {
            std::unordered_set<uint32_t> gt_set;
            uint32_t gt_limit = std::min(at_k, static_cast<uint32_t>(gt_ids[qi].size()));
            for (uint32_t i = 0; i < gt_limit; ++i) {
                gt_set.insert(static_cast<uint32_t>(gt_ids[qi][i]));
            }
            if (gt_set.empty()) continue;

            uint32_t hits = 0;
            uint32_t check_limit = std::min(at_k, static_cast<uint32_t>(results[qi].size()));
            for (uint32_t i = 0; i < check_limit; ++i) {
                if (gt_set.count(results[qi][i])) hits++;
            }
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
    uint64_t final_total = final_safein + final_safeout + final_uncertain;
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
    Log("  avg_probed      = %.2f / %u clusters\n", avg_probed, nprobe);
    Log("  early_stop_rate = %.4f\n", early_stop_rate);

    // Stage 1 classification
    Log("\n  --- Stage 1 (popcount) ---\n");
    Log("    SafeIn    = %lu (%.2f%%)\n", s1_safein, pct(s1_safein, s1_total));
    Log("    SafeOut   = %lu (%.2f%%)\n", s1_safeout, pct(s1_safeout, s1_total));
    Log("    Uncertain = %lu (%.2f%%)\n", s1_uncertain, pct(s1_uncertain, s1_total));

    // Stage 2 classification (only when bits > 1)
    if (bits > 1) {
        uint64_t s2_total = s2_safein + s2_safeout + s2_uncertain;
        Log("\n  --- Stage 2 (%u-bit LUT, margin / %g) ---\n",
            bits, static_cast<double>(margin_s2_divisor));
        Log("    Input (S1 Uncertain) = %lu\n", s1_uncertain);
        Log("    SafeIn    = %lu (%.2f%%)\n", s2_safein, pct(s2_safein, s2_total));
        Log("    SafeOut   = %lu (%.2f%%)\n", s2_safeout, pct(s2_safeout, s2_total));
        Log("    Uncertain = %lu (%.2f%%)\n", s2_uncertain, pct(s2_uncertain, s2_total));
    }

    // Final classification + False SafeIn/Out
    Log("\n  --- Final (S1%s) ---\n", bits > 1 ? "+S2" : "");
    Log("    SafeIn    = %lu (%.2f%%)\n", final_safein, pct(final_safein, final_total));
    Log("    SafeOut   = %lu (%.2f%%)\n", final_safeout, pct(final_safeout, final_total));
    Log("    Uncertain = %lu (%.2f%%)\n", final_uncertain, pct(final_uncertain, final_total));
    Log("\n");
    Log("    False SafeIn  = %lu / %lu (%.4f%%)  [not GT top-K but SafeIn]\n",
        final_false_safein, final_safein, pct(final_false_safein, final_safein));
    Log("    False SafeOut = %lu / %lu (%.4f%%)  [GT top-K but SafeOut !!!]\n",
        final_false_safeout, final_safeout, pct(final_false_safeout, final_safeout));

    if (use_crc) {
        Log("\n  CRC stats:\n");
        Log("    lamhat     = %.6f\n", calib_results.lamhat);
        Log("    crc_active = %s\n", crc_active ? "yes" : "no");
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
        jf << "  \"dim\": " << dim << ",\n";
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
        jf << "  \"avg_probed\": " << avg_probed << ",\n";
        jf << "  \"early_stop_rate\": " << early_stop_rate << ",\n";
        jf << "  \"s1_safein_pct\": " << pct(s1_safein, s1_total) << ",\n";
        jf << "  \"s1_safeout_pct\": " << pct(s1_safeout, s1_total) << ",\n";
        jf << "  \"s1_uncertain_pct\": " << pct(s1_uncertain, s1_total) << ",\n";
        jf << "  \"final_safein_pct\": " << pct(final_safein, final_total) << ",\n";
        jf << "  \"final_safeout_pct\": " << pct(final_safeout, final_total) << ",\n";
        jf << "  \"final_uncertain_pct\": " << pct(final_uncertain, final_total) << ",\n";
        jf << "  \"false_safein_rate\": " << pct(final_false_safein, final_safein) << ",\n";
        jf << "  \"false_safeout_rate\": " << pct(final_false_safeout, final_safeout) << "\n";
        jf << "}\n";
        jf.close();
        Log("\nJSON results written to: %s\n", json_path.c_str());
    }

    Log("\nDone.\n");
    return 0;
}
