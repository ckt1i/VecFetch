/// bench_rabitq_accuracy.cpp — Measure RaBitQ M-bit distance estimation accuracy.
///
/// For each query × each database vector:
///   - Compute exact L2² distance
///   - Compute RaBitQ estimated distance (Stage 1: popcount, Stage 2: M-bit LUT)
///   - Compare and report error statistics
///
/// Supports two-stage SafeIn/SafeOut classification:
///   - Stage 1: MSB-plane popcount (same as 1-bit) with margin_s1
///   - Stage 2: M-bit LUT scan (only for Uncertain from S1) with margin_s2
///   - margin_s2 = margin_s1 / 2^(M-1)
///
/// Usage:
///   bench_rabitq_accuracy [--dataset /path/to/coco_1k] [--queries N] [--nlist K] [--bits M]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "superkmeans/superkmeans.h"

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/io/npy_reader.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/popcount.h"

using namespace vdb;
using namespace vdb::rabitq;
using namespace vdb::index;

// ============================================================================
// Helpers
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

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// SuperKMeans clustering helper
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
// Percentile helper
// ============================================================================

static float Percentile(std::vector<float>& v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    float idx = p * static_cast<float>(v.size() - 1);
    auto lo = static_cast<size_t>(std::floor(idx));
    auto hi = static_cast<size_t>(std::ceil(idx));
    lo = std::min(lo, v.size() - 1);
    hi = std::min(hi, v.size() - 1);
    if (lo == hi) return v[lo];
    float frac = idx - static_cast<float>(lo);
    return v[lo] * (1.0f - frac) + v[hi] * frac;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetArg(argc, argv, "--dataset",
                                  "/home/zcq/VDB/data/coco_100k");
    std::string base_path = GetArg(argc, argv, "--base", "");
    std::string query_path = GetArg(argc, argv, "--query", "");
    uint32_t nlist = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 32));
    int q_limit = GetIntArg(argc, argv, "--queries", 100);
    uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    uint32_t p_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-dk", 99));
    uint32_t p_for_eps_ip = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-eps-ip", 99));
    uint32_t samples_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--samples-for-dk", 100));
    uint32_t samples_for_eps_ip = static_cast<uint32_t>(GetIntArg(argc, argv, "--samples-for-eps-ip", 100));
    uint8_t bits = static_cast<uint8_t>(GetIntArg(argc, argv, "--bits", 1));
    std::string centroids_path = GetArg(argc, argv, "--centroids", "");
    std::string assignments_path = GetArg(argc, argv, "--assignments", "");

    // Fallback: --base/--query not specified → derive from --dataset
    if (base_path.empty())  base_path  = data_dir + "/image_embeddings.npy";
    if (query_path.empty()) query_path = data_dir + "/query_embeddings.npy";

    Log("=== RaBitQ Accuracy Benchmark (bits=%u) ===\n", bits);
    Log("Base:  %s\n", base_path.c_str());
    Log("Query: %s\n", query_path.c_str());

    // ================================================================
    // Phase 1: Load data
    // ================================================================
    Log("[Phase 1] Loading data...\n");

    auto img_or = io::LoadVectors(base_path);
    if (!img_or.ok()) {
        std::fprintf(stderr, "Failed to load base: %s\n", img_or.status().ToString().c_str());
        return 1;
    }
    auto& img = img_or.value();
    uint32_t N = img.rows;
    Dim dim = static_cast<Dim>(img.cols);

    auto qry_or = io::LoadVectors(query_path);
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load query: %s\n", qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q = std::min(static_cast<uint32_t>(q_limit), qry.rows);

    Log("  N=%u, Q=%u, dim=%u\n\n", N, Q, dim);

    // ================================================================ 
    // Phase 2: K-Means clustering + RaBitQ encoding
    // ================================================================
    std::vector<float> centroids;
    std::vector<uint32_t> assignments;

    if (!centroids_path.empty() && !assignments_path.empty()) {
        Log("[Phase 2] Loading precomputed centroids + assignments...\n");
        auto c_or = io::LoadVectors(centroids_path);
        if (!c_or.ok()) {
            std::fprintf(stderr, "Failed to load centroids: %s\n",
                         c_or.status().ToString().c_str());
            return 1;
        }
        auto& c = c_or.value();
        if (c.cols != dim) {
            std::fprintf(stderr, "Centroid dim mismatch: %u vs %u\n", c.cols, dim);
            return 1;
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
        Log("[Phase 2] SuperKMeans (K=%u) + RaBitQ encoding...\n", nlist);
        RunSuperKMeans(img.data.data(), N, dim, nlist, centroids, assignments);
    }

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation, bits);
    RaBitQEstimator estimator(dim, bits);

    // Encode each vector with its cluster centroid
    std::vector<RaBitQCode> codes(N);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t k = assignments[i];
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        codes[i] = encoder.Encode(
            img.data.data() + static_cast<size_t>(i) * dim, centroid);
    }
    Log("  Encoded %u vectors.\n\n", N);

    // ================================================================
    // Phase 2b: Compute per-cluster r_max and global ε_ip
    // ================================================================
    Log("[Phase 2b] Computing per-cluster r_max and global ε_ip...\n");

    // Count members per cluster
    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    const uint32_t num_words = (dim + 63) / 64;  // words per plane (MSB only)

    // Per-cluster r_max
    std::vector<float> per_cluster_r_max(nlist, 0.0f);
    for (uint32_t k = 0; k < nlist; ++k) {
        for (uint32_t idx : cluster_members[k]) {
            per_cluster_r_max[k] = std::max(per_cluster_r_max[k], codes[idx].norm);
        }
    }

    // Global ε_ip calibration: sample pseudo-queries, compute |ŝ - s|
    std::vector<float> ip_errors;
    for (uint32_t k = 0; k < nlist; ++k) {
        const auto& members = cluster_members[k];
        if (members.size() < 2) continue;

        uint32_t n_queries = std::min(samples_for_eps_ip,
                                       static_cast<uint32_t>(members.size()));
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;

        std::vector<uint32_t> indices(members.size());
        std::iota(indices.begin(), indices.end(), 0u);
        std::mt19937 rng(42 + k);
        std::shuffle(indices.begin(), indices.end(), rng);

        for (uint32_t q = 0; q < n_queries; ++q) {
            uint32_t q_global = members[indices[q]];
            const float* q_vec = img.data.data() +
                static_cast<size_t>(q_global) * dim;
            auto pq = estimator.PrepareQuery(q_vec, centroid, rotation);

            for (uint32_t t = 0; t < static_cast<uint32_t>(members.size()); ++t) {
                if (t == indices[q]) continue;
                uint32_t t_global = members[t];
                const auto& code = codes[t_global];

                // Popcount path: ŝ = 1 - 2·hamming/dim
                uint32_t hamming = simd::PopcountXor(
                    pq.sign_code.data(), code.code.data(), num_words);
                float ip_hat = 1.0f - 2.0f * static_cast<float>(hamming) /
                                              static_cast<float>(dim);

                // Accurate path
                float dot = 0.0f;
                for (size_t d = 0; d < dim; ++d) {
                    int bit = (code.code[d / 64] >> (d % 64)) & 1;
                    dot += pq.rotated[d] * (2.0f * bit - 1.0f);
                }
                float ip_accurate = dot * inv_sqrt_dim;

                ip_errors.push_back(std::abs(ip_hat - ip_accurate));
            }
        }
    }

    float eps_ip = 0.0f;
    if (!ip_errors.empty()) {
        std::sort(ip_errors.begin(), ip_errors.end());
        float p = static_cast<float>(p_for_eps_ip) / 100.0f;
        auto idx = static_cast<size_t>(
            p * static_cast<float>(ip_errors.size() - 1));
        eps_ip = ip_errors[idx];
    }

    Log("  ε_ip = %.6f (P%u of %zu samples)\n",
        eps_ip, p_for_eps_ip, ip_errors.size());
    /*
    // Print per-cluster statistics table
    Log("\n  Per-Cluster Statistics:\n");
    Log("  %-10s %-14s %-12s\n", "Cluster", "Num Vectors", "r_max");
    Log("  %-10s %-14s %-12s\n", "-------", "-----------", "-----");
    for (uint32_t k = 0; k < nlist; ++k) {
        Log("  %-10u %-14zu %.6f\n", k, cluster_members[k].size(),
            per_cluster_r_max[k]);
    }
    Log("\n");
    */
    // ================================================================
    // Phase 3: Calibrate d_k
    // ================================================================
    Log("[Phase 3] Calibrating d_k...\n");
    float p_dk = static_cast<float>(p_for_dk) / 100.0f;
    float d_k = ConANN::CalibrateDistanceThreshold(
        qry.data.data(), Q,         // query source
        img.data.data(), N,         // database source
        dim, samples_for_dk, top_k, p_dk, 42);
    Log("  d_k=%.4f (L2², from qry→img)\n\n", d_k);

    // ================================================================
    // Phase 4: Per-query distance comparison
    // ================================================================
    Log("[Phase 4] Computing exact vs RaBitQ distances (%u queries × %u vectors)...\n",
        Q, N);

    // Accumulators
    std::vector<float> all_abs_errors;     // Stage 1 errors
    std::vector<float> all_rel_errors;
    std::vector<float> all_abs_errors_s2;  // Stage 2 errors (bits > 1)
    std::vector<float> all_rel_errors_s2;
    float max_abs_error = 0.0f;

    // Recall accumulators — Stage 1 (popcount ranking)
    uint64_t recall_hit_1 = 0, recall_hit_5 = 0, recall_hit_10 = 0;
    // Recall accumulators — Stage 2 (multi-bit ranking, bits > 1)
    uint64_t s2_recall_hit_1 = 0, s2_recall_hit_5 = 0, s2_recall_hit_10 = 0;

    // Stage 1 classification
    uint64_t s1_safe_in = 0, s1_safe_out = 0, s1_uncertain = 0;
    uint64_t s1_false_safe_in = 0, s1_false_safe_out = 0;

    // Stage 2 classification (only for bits > 1, applied to S1 Uncertain)
    uint64_t s2_safe_in = 0, s2_safe_out = 0, s2_uncertain = 0;
    uint64_t s2_false_safe_in = 0, s2_false_safe_out = 0;

    // Final (after both stages) classification
    uint64_t final_safe_in = 0, final_safe_out = 0, final_uncertain = 0;
    uint64_t final_false_safe_in = 0, final_false_safe_out = 0;

    // Margin divisor for Stage 2: margin_s2 = margin_s1 / 2^(M-1)
    const float margin_s2_divisor = static_cast<float>(1u << (bits - 1));

    auto t0 = std::chrono::steady_clock::now();

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;

        // Exact brute-force distances
        std::vector<std::pair<float, uint32_t>> exact(N);
        for (uint32_t i = 0; i < N; ++i) {
            exact[i] = {simd::L2Sqr(q_vec, img.data.data() + static_cast<size_t>(i) * dim, dim), i};
        }

        // PreparedQuery per cluster (cached for this query)
        std::vector<PreparedQuery> pqs(nlist);
        for (uint32_t k = 0; k < nlist; ++k) {
            const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
            pqs[k] = estimator.PrepareQuery(q_vec, centroid, rotation);
        }

        // RaBitQ Stage 1 estimated distances
        std::vector<std::pair<float, uint32_t>> rabitq_dists(N);
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t k = assignments[i];
            rabitq_dists[i] = {estimator.EstimateDistance(pqs[k], codes[i]), i};
        }

        // Stage 2 estimated distances (only when bits > 1)
        std::vector<float> rabitq_dists_s2(N, 0.0f);
        if (bits > 1) {
            for (uint32_t i = 0; i < N; ++i) {
                uint32_t k = assignments[i];
                rabitq_dists_s2[i] = estimator.EstimateDistanceMultiBit(
                    pqs[k], codes[i]);
            }
        }

        // Distance error statistics — Stage 1
        for (uint32_t i = 0; i < N; ++i) {
            float abs_err = std::abs(rabitq_dists[i].first - exact[i].first);
            all_abs_errors.push_back(abs_err);
            max_abs_error = std::max(max_abs_error, abs_err);

            if (exact[i].first > 1e-6f) {
                all_rel_errors.push_back(abs_err / exact[i].first);
            }
        }

        // Distance error statistics — Stage 2 (bits > 1)
        if (bits > 1) {
            for (uint32_t i = 0; i < N; ++i) {
                float abs_err = std::abs(rabitq_dists_s2[i] - exact[i].first);
                all_abs_errors_s2.push_back(abs_err);
                if (exact[i].first > 1e-6f) {
                    all_rel_errors_s2.push_back(abs_err / exact[i].first);
                }
            }
        }

        // Exact top-K (for recall and classification eval)
        std::partial_sort(exact.begin(), exact.begin() + top_k, exact.end());
        std::unordered_set<uint32_t> true_topk_set;
        for (uint32_t i = 0; i < top_k; ++i) true_topk_set.insert(exact[i].second);

        // RaBitQ top-K (for recall)
        std::partial_sort(rabitq_dists.begin(),
                          rabitq_dists.begin() + top_k,
                          rabitq_dists.end());

        // Recall@{1,5,10}
        auto count_hits = [&](uint32_t k_val) -> uint32_t {
            uint32_t hits = 0;
            for (uint32_t i = 0; i < std::min(k_val, top_k); ++i) {
                if (true_topk_set.count(rabitq_dists[i].second)) hits++;
            }
            return hits;
        };
        recall_hit_1 += (count_hits(1) > 0) ? 1 : 0;
        recall_hit_5 += count_hits(5);
        recall_hit_10 += count_hits(10);

        // Stage 2 recall (ranking by multi-bit distance)
        if (bits > 1) {
            std::vector<std::pair<float, uint32_t>> s2_ranked(N);
            for (uint32_t i = 0; i < N; ++i) {
                s2_ranked[i] = {rabitq_dists_s2[i], i};
            }
            std::partial_sort(s2_ranked.begin(),
                              s2_ranked.begin() + top_k,
                              s2_ranked.end());
            auto count_hits_s2 = [&](uint32_t k_val) -> uint32_t {
                uint32_t hits = 0;
                for (uint32_t i = 0; i < std::min(k_val, top_k); ++i) {
                    if (true_topk_set.count(s2_ranked[i].second)) hits++;
                }
                return hits;
            };
            s2_recall_hit_1 += (count_hits_s2(1) > 0) ? 1 : 0;
            s2_recall_hit_5 += count_hits_s2(5);
            s2_recall_hit_10 += count_hits_s2(10);
        }

        // Two-stage SafeIn/SafeOut classification
        // Compute per-cluster margins (S1 and S2)
        std::vector<float> margin_s1_per_cluster(nlist);
        ConANN conann(eps_ip, d_k);
        for (uint32_t k = 0; k < nlist; ++k) {
            margin_s1_per_cluster[k] = 2.0f * per_cluster_r_max[k] *
                                        pqs[k].norm_qc * eps_ip;
        }

        for (uint32_t i = 0; i < N; ++i) {
            uint32_t vec_id = rabitq_dists[i].second;
            uint32_t k = assignments[vec_id];
            float margin_s1 = margin_s1_per_cluster[k];
            bool is_true_topk = true_topk_set.count(vec_id) > 0;

            // Stage 1 classification
            ResultClass rc_s1 = conann.Classify(rabitq_dists[i].first, margin_s1);
            switch (rc_s1) {
                case ResultClass::SafeIn:
                    s1_safe_in++;
                    if (!is_true_topk) s1_false_safe_in++;
                    break;
                case ResultClass::SafeOut:
                    s1_safe_out++;
                    if (is_true_topk) s1_false_safe_out++;
                    break;
                case ResultClass::Uncertain:
                    s1_uncertain++;
                    break;
            }

            // Final classification = S1 result by default
            ResultClass rc_final = rc_s1;

            // Stage 2: only for S1-Uncertain when bits > 1
            if (rc_s1 == ResultClass::Uncertain && bits > 1) {
                float margin_s2 = margin_s1 / margin_s2_divisor;
                ResultClass rc_s2 = conann.Classify(rabitq_dists_s2[vec_id], margin_s2);
                switch (rc_s2) {
                    case ResultClass::SafeIn:
                        s2_safe_in++;
                        if (!is_true_topk) s2_false_safe_in++;
                        break;
                    case ResultClass::SafeOut:
                        s2_safe_out++;
                        if (is_true_topk) s2_false_safe_out++;
                        break;
                    case ResultClass::Uncertain:
                        s2_uncertain++;
                        break;
                }
                rc_final = rc_s2;
            }

            // Final statistics
            switch (rc_final) {
                case ResultClass::SafeIn:
                    final_safe_in++;
                    if (!is_true_topk) final_false_safe_in++;
                    break;
                case ResultClass::SafeOut:
                    final_safe_out++;
                    if (is_true_topk) final_false_safe_out++;
                    break;
                case ResultClass::Uncertain:
                    final_uncertain++;
                    break;
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ================================================================
    // Phase 5: Report
    // ================================================================
    Log("\n[Phase 5] Results (%.1f ms)\n", elapsed_ms);

    float mean_abs = 0.0f;
    for (float e : all_abs_errors) mean_abs += e;
    mean_abs /= static_cast<float>(all_abs_errors.size());

    float mean_rel = 0.0f;
    for (float e : all_rel_errors) mean_rel += e;
    mean_rel /= static_cast<float>(all_rel_errors.size());

    Log("\n=== Stage 1 Distance Error (popcount) ===\n");
    Log("  Mean absolute error:     %.6f\n", mean_abs);
    Log("  Max absolute error:      %.6f\n", max_abs_error);
    Log("  Mean relative error:     %.6f\n", mean_rel);
    Log("  P95 relative error:      %.6f\n", Percentile(all_rel_errors, 0.95f));
    Log("  P99 relative error:      %.6f\n", Percentile(all_rel_errors, 0.99f));

    if (bits > 1) {
        float mean_abs_s2 = 0.0f;
        for (float e : all_abs_errors_s2) mean_abs_s2 += e;
        mean_abs_s2 /= static_cast<float>(all_abs_errors_s2.size());

        float mean_rel_s2 = 0.0f;
        for (float e : all_rel_errors_s2) mean_rel_s2 += e;
        mean_rel_s2 /= static_cast<float>(all_rel_errors_s2.size());

        Log("\n=== Stage 2 Distance Error (%u-bit LUT) ===\n", bits);
        Log("  Mean absolute error:     %.6f\n", mean_abs_s2);
        Log("  Mean relative error:     %.6f\n", mean_rel_s2);
        Log("  P95 relative error:      %.6f\n", Percentile(all_rel_errors_s2, 0.95f));
        Log("  P99 relative error:      %.6f\n", Percentile(all_rel_errors_s2, 0.99f));
    }

    float recall_1 = static_cast<float>(recall_hit_1) / static_cast<float>(Q);
    float recall_5 = static_cast<float>(recall_hit_5) /
                     static_cast<float>(Q * std::min(5u, top_k));
    float recall_10 = static_cast<float>(recall_hit_10) /
                      static_cast<float>(Q * std::min(10u, top_k));

    Log("\n=== Stage 1 Ranking (popcount brute-force vs exact) ===\n");
    Log("  recall@1:                %.4f\n", recall_1);
    Log("  recall@5:                %.4f\n", recall_5);
    Log("  recall@10:               %.4f\n", recall_10);

    if (bits > 1) {
        float s2_recall_1 = static_cast<float>(s2_recall_hit_1) / static_cast<float>(Q);
        float s2_recall_5 = static_cast<float>(s2_recall_hit_5) /
                            static_cast<float>(Q * std::min(5u, top_k));
        float s2_recall_10 = static_cast<float>(s2_recall_hit_10) /
                             static_cast<float>(Q * std::min(10u, top_k));

        Log("\n=== Stage 2 Ranking (%u-bit brute-force vs exact) ===\n", bits);
        Log("  recall@1:                %.4f\n", s2_recall_1);
        Log("  recall@5:                %.4f\n", s2_recall_5);
        Log("  recall@10:               %.4f\n", s2_recall_10);
    }

    // --- Stage 1 classification ---
    uint64_t s1_total = s1_safe_in + s1_safe_out + s1_uncertain;
    auto pct = [](uint64_t num, uint64_t den) -> double {
        return den > 0 ? 100.0 * static_cast<double>(num) / static_cast<double>(den) : 0.0;
    };

    Log("\n=== Stage 1 Classification (popcount, d_k=%.4f, ε_ip=%.6f) ===\n",
        d_k, eps_ip);
    Log("  Total:                   %lu\n", s1_total);
    Log("  SafeIn:                  %lu (%.2f%%)\n", s1_safe_in, pct(s1_safe_in, s1_total));
    Log("  SafeOut:                 %lu (%.2f%%)\n", s1_safe_out, pct(s1_safe_out, s1_total));
    Log("  Uncertain:               %lu (%.2f%%)\n", s1_uncertain, pct(s1_uncertain, s1_total));
    Log("  False SafeIn:            %lu / %lu (%.4f%%)\n",
        s1_false_safe_in, s1_safe_in, pct(s1_false_safe_in, s1_safe_in));
    Log("  False SafeOut:           %lu / %lu (%.4f%%)\n",
        s1_false_safe_out, s1_safe_out, pct(s1_false_safe_out, s1_safe_out));

    // --- Stage 2 classification (only when bits > 1) ---
    if (bits > 1) {
        uint64_t s2_total = s2_safe_in + s2_safe_out + s2_uncertain;
        Log("\n=== Stage 2 Classification (%u-bit LUT, margin_s2 = margin_s1 / %g) ===\n",
            bits, static_cast<double>(margin_s2_divisor));
        Log("  Input (S1 Uncertain):    %lu\n", s1_uncertain);
        Log("  SafeIn:                  %lu (%.2f%%)\n", s2_safe_in, pct(s2_safe_in, s2_total));
        Log("  SafeOut:                 %lu (%.2f%%)\n", s2_safe_out, pct(s2_safe_out, s2_total));
        Log("  Uncertain:               %lu (%.2f%%)\n", s2_uncertain, pct(s2_uncertain, s2_total));
        Log("  False SafeIn:            %lu / %lu (%.4f%%)\n",
            s2_false_safe_in, s2_safe_in, pct(s2_false_safe_in, s2_safe_in));
        Log("  False SafeOut:           %lu / %lu (%.4f%%)\n",
            s2_false_safe_out, s2_safe_out, pct(s2_false_safe_out, s2_safe_out));
    }

    // --- Final (after both stages) ---
    uint64_t final_total = final_safe_in + final_safe_out + final_uncertain;
    Log("\n=== Final Classification (after S1%s) ===\n",
        bits > 1 ? "+S2" : "");
    Log("  Total:                   %lu\n", final_total);
    Log("  SafeIn:                  %lu (%.2f%%)\n", final_safe_in, pct(final_safe_in, final_total));
    Log("  SafeOut:                 %lu (%.2f%%)\n", final_safe_out, pct(final_safe_out, final_total));
    Log("  Uncertain:               %lu (%.2f%%)\n", final_uncertain, pct(final_uncertain, final_total));
    Log("  False SafeIn:            %lu / %lu (%.4f%%)\n",
        final_false_safe_in, final_safe_in, pct(final_false_safe_in, final_safe_in));
    Log("  False SafeOut:           %lu / %lu (%.4f%%)  [!!!]\n",
        final_false_safe_out, final_safe_out, pct(final_false_safe_out, final_safe_out));

    Log("\nDone.\n");
    return 0;
}
