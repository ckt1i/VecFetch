/// bench_conann_recall.cpp — ConANN recall benchmark with exact distances.
///
/// Measures IVF partitioning quality + d_k early stop effectiveness
/// using exact L2 distances (no RaBitQ approximation).
///
/// Usage:
///   bench_conann_recall [--dataset path] [--nlist 16] [--nprobe 8]
///                       [--topk 10] [--p-for-dk 99] [--queries 0]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "superkmeans/superkmeans.h"

#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/io/npy_reader.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
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

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// K-Means (SuperKMeans)
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
// Recall computation
// ============================================================================

static double ComputeRecallAtK(const std::vector<uint32_t>& predicted,
                                const std::vector<uint32_t>& gt,
                                uint32_t K) {
    uint32_t pk = std::min(K, static_cast<uint32_t>(predicted.size()));
    uint32_t gk = std::min(K, static_cast<uint32_t>(gt.size()));
    std::unordered_set<uint32_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return gk > 0 ? static_cast<double>(hits) / gk : 0.0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetArg(argc, argv, "--dataset",
                                  "/home/zcq/VDB/data/coco_1k");
    uint32_t nlist = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 16));
    uint32_t nprobe = static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 8));
    uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    uint32_t p_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-dk", 99));
    int q_limit = GetIntArg(argc, argv, "--queries", 0);
    uint32_t max_iter = static_cast<uint32_t>(GetIntArg(argc, argv, "--max-iter", 20));
    uint32_t seed = static_cast<uint32_t>(GetIntArg(argc, argv, "--seed", 42));

    Log("=== ConANN Recall Benchmark (Exact Distances) ===\n");
    Log("Dataset: %s\n", data_dir.c_str());
    Log("nlist=%u, nprobe=%u, topk=%u, p_for_dk=%u\n\n", nlist, nprobe, top_k, p_for_dk);

    // ================================================================
    // Phase 1: Load data
    // ================================================================
    Log("[Phase 1] Loading data...\n");

    auto img_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_or.ok()) {
        std::fprintf(stderr, "Failed: %s\n", img_or.status().ToString().c_str());
        return 1;
    }
    auto& img = img_or.value();
    uint32_t N = img.rows;
    Dim dim = static_cast<Dim>(img.cols);

    auto qry_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed: %s\n", qry_or.status().ToString().c_str());
        return 1;
    }
    auto& qry = qry_or.value();
    uint32_t Q_total = qry.rows;
    uint32_t Q = (q_limit > 0 && static_cast<uint32_t>(q_limit) < Q_total)
                     ? static_cast<uint32_t>(q_limit) : Q_total;

    Log("  N=%u, Q=%u/%u, dim=%u\n\n", N, Q, Q_total, dim);

    // ================================================================
    // Phase 2: Brute-force ground truth
    // ================================================================
    Log("[Phase 2] Computing brute-force ground truth (top-%u)...\n", top_k);

    auto t_bf = std::chrono::steady_clock::now();

    // gt_topk[qi] = indices of top-K nearest database vectors
    std::vector<std::vector<uint32_t>> gt_topk(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry.data.data() + static_cast<size_t>(qi) * dim;

        std::vector<std::pair<float, uint32_t>> dists(N);
        for (uint32_t j = 0; j < N; ++j) {
            dists[j] = {simd::L2Sqr(qvec, img.data.data() + static_cast<size_t>(j) * dim, dim), j};
        }

        std::partial_sort(dists.begin(), dists.begin() + top_k, dists.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        gt_topk[qi].resize(top_k);
        for (uint32_t k = 0; k < top_k; ++k) {
            gt_topk[qi][k] = dists[k].second;
        }
    }

    double bf_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_bf).count();
    Log("  Brute-force time: %.1f ms\n\n", bf_ms);

    // ================================================================
    // Phase 3: K-Means clustering
    // ================================================================
    Log("[Phase 3] K-Means (K=%u, iter=%u)...\n", nlist, max_iter);

    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    RunSuperKMeans(img.data.data(), N, dim, nlist, centroids, assignments);

    // Build cluster member lists
    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    Log("  Cluster sizes: ");
    for (uint32_t k = 0; k < nlist; ++k) {
        Log("%zu ", cluster_members[k].size());
    }
    Log("\n\n");

    // ================================================================
    // Phase 4: Calibrate d_k (query→database)
    // ================================================================
    Log("[Phase 4] Calibrating d_k (P%u, query→database)...\n", p_for_dk);

    float dk_percentile = static_cast<float>(p_for_dk) / 100.0f;
    uint32_t dk_samples = std::min(100u, Q);
    float d_k = ConANN::CalibrateDistanceThreshold(
        qry.data.data(), Q,
        img.data.data(), N,
        dim, dk_samples, top_k, dk_percentile, seed);

    Log("  d_k = %.4f\n\n", d_k);

    // ================================================================
    // Phase 5: Per-query search with exact distances and fixed nprobe
    // ================================================================
    Log("[Phase 5] Searching (nprobe=%u, fixed probe)...\n", nprobe);

    struct QueryResult {
        uint32_t clusters_probed;
        std::vector<uint32_t> result_ids;  // top-K indices
    };

    std::vector<QueryResult> results(Q);

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry.data.data() + static_cast<size_t>(qi) * dim;

        // Find nearest nprobe centroids
        std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
        for (uint32_t k = 0; k < nlist; ++k) {
            centroid_dists[k] = {
                simd::L2Sqr(qvec, centroids.data() + static_cast<size_t>(k) * dim, dim), k};
        }
        std::partial_sort(centroid_dists.begin(),
                          centroid_dists.begin() + nprobe,
                          centroid_dists.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });

        // Max-heap for top-K (distance, index)
        std::vector<std::pair<float, uint32_t>> topk_heap;

        auto push_candidate = [&](float dist, uint32_t idx) {
            if (topk_heap.size() < top_k) {
                topk_heap.push_back({dist, idx});
                std::push_heap(topk_heap.begin(), topk_heap.end());
            } else if (dist < topk_heap.front().first) {
                std::pop_heap(topk_heap.begin(), topk_heap.end());
                topk_heap.back() = {dist, idx};
                std::push_heap(topk_heap.begin(), topk_heap.end());
            }
        };

        uint32_t probed = 0;

        for (uint32_t p = 0; p < nprobe; ++p) {
            uint32_t cluster_id = centroid_dists[p].second;

            // Compute exact L2² to all vectors in this cluster
            for (uint32_t vec_idx : cluster_members[cluster_id]) {
                float dist = simd::L2Sqr(
                    qvec,
                    img.data.data() + static_cast<size_t>(vec_idx) * dim,
                    dim);
                push_candidate(dist, vec_idx);
            }

            probed++;
        }

        // Extract sorted results
        std::sort_heap(topk_heap.begin(), topk_heap.end());
        results[qi].clusters_probed = probed;
        results[qi].result_ids.reserve(topk_heap.size());
        for (auto& [dist, idx] : topk_heap) {
            results[qi].result_ids.push_back(idx);
        }
    }

    Log("  Search complete.\n\n");

    // ================================================================
    // Phase 6: Evaluate
    // ================================================================
    Log("[Phase 6] Evaluating...\n\n");

    double sum_r1 = 0, sum_r5 = 0, sum_r10 = 0;
    double sum_probes = 0;

    for (uint32_t qi = 0; qi < Q; ++qi) {
        sum_r1  += ComputeRecallAtK(results[qi].result_ids, gt_topk[qi], 1);
        sum_r5  += ComputeRecallAtK(results[qi].result_ids, gt_topk[qi], 5);
        sum_r10 += ComputeRecallAtK(results[qi].result_ids, gt_topk[qi], top_k);
        sum_probes += results[qi].clusters_probed;
    }

    Log("╔══════════════════════════════════════════════════╗\n");
    Log("║              ConANN Recall Results               ║\n");
    Log("╠══════════════════════════════════════════════════╣\n");
    Log("║  nlist = %-5u    nprobe = %-5u   topk = %-5u  ║\n", nlist, nprobe, top_k);
    Log("║  d_k = %-10.4f  (P%u)                         ║\n", d_k, p_for_dk);
    Log("╠══════════════════════════════════════════════════╣\n");
    Log("║  recall@1  = %.4f                              ║\n", sum_r1 / Q);
    Log("║  recall@5  = %.4f                              ║\n", sum_r5 / Q);
    Log("║  recall@10 = %.4f                              ║\n", sum_r10 / Q);
    Log("╠══════════════════════════════════════════════════╣\n");
    Log("║  avg probes     = %.2f / %u                    ║\n", sum_probes / Q, nprobe);
    Log("╚══════════════════════════════════════════════════╝\n");

    // Per-query sample (every 20th)
    Log("\n--- Per-Query Sample (every 20th) ---\n");
    Log("%-6s  %-8s  %-8s  %-8s  %-7s\n",
        "Query", "R@1", "R@5", "R@10", "Probes");
    Log("------  --------  --------  --------  -------\n");

    uint32_t stride = std::max(Q / 50, 1u);
    for (uint32_t qi = 0; qi < Q; qi += stride) {
        double r1  = ComputeRecallAtK(results[qi].result_ids, gt_topk[qi], 1);
        double r5  = ComputeRecallAtK(results[qi].result_ids, gt_topk[qi], 5);
        double r10 = ComputeRecallAtK(results[qi].result_ids, gt_topk[qi], top_k);
        Log("%-6u  %-8.4f  %-8.4f  %-8.4f  %-7u\n",
            qi, r1, r5, r10,
            results[qi].clusters_probed);
    }

    return 0;
}
