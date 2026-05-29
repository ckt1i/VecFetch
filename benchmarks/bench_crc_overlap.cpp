/// bench_crc_overlap.cpp — CRC OverlapScheduler integration benchmark.
///
/// Compares static vs dynamic SafeOut, exact vs RaBitQ CRC calibration,
/// and measures SafeOut false pruning rates.
///
/// Usage:
///   bench_crc_overlap [--dataset path] [--nlist 16] [--topk 10]
///                     [--alpha 0.1] [--queries 0] [--sweep-prefetch]
///                     [--p-for-dk 99] [--p-for-eps-ip 95]
///                     [--samples-for-dk 100] [--samples-for-eps-ip 100]

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
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/io/npy_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/popcount.h"

using namespace vdb;
using namespace vdb::index;

// ============================================================================
// Helpers
// ============================================================================

struct EstimateHeapEntry {
    float distance = 0.0f;
    float error_bound = 0.0f;
    uint32_t id = 0;

    bool operator<(const EstimateHeapEntry& other) const {
        return distance < other.distance;
    }
};

static float RecomputeMaxError(const std::vector<EstimateHeapEntry>& heap) {
    float max_error = 0.0f;
    for (const EstimateHeapEntry& entry : heap) {
        max_error = std::max(max_error, entry.error_bound);
    }
    return max_error;
}

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
// RaBitQ encoding for clusters
// ============================================================================

struct EncodedCluster {
    std::vector<uint8_t> codes_block;  // packed [code_words | norm_oc | sum_x] per entry
    uint32_t code_entry_size;
    uint32_t count;
};

static std::vector<EncodedCluster> EncodeAllClusters(
    const float* data, Dim dim,
    const std::vector<float>& centroids, uint32_t nlist,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const rabitq::RotationMatrix& rotation) {

    rabitq::RaBitQEncoder encoder(dim, rotation);
    uint32_t num_words = (dim + 63) / 64;
    // entry layout: [num_words * 8 bytes code] [4 bytes norm] [4 bytes sum_x]
    uint32_t entry_size = num_words * sizeof(uint64_t) + sizeof(float) + sizeof(uint32_t);

    std::vector<EncodedCluster> encoded(nlist);

    for (uint32_t c = 0; c < nlist; ++c) {
        const auto& members = cluster_members[c];
        encoded[c].count = static_cast<uint32_t>(members.size());
        encoded[c].code_entry_size = entry_size;
        encoded[c].codes_block.resize(static_cast<size_t>(members.size()) * entry_size);

        const float* centroid = centroids.data() + static_cast<size_t>(c) * dim;

        for (size_t vi = 0; vi < members.size(); ++vi) {
            uint32_t gid = members[vi];
            const float* vec = data + static_cast<size_t>(gid) * dim;

            rabitq::RaBitQCode code = encoder.Encode(vec, centroid);

            uint8_t* entry = encoded[c].codes_block.data() + vi * entry_size;
            // Write code words
            std::memcpy(entry, code.code.data(),
                        num_words * sizeof(uint64_t));
            // Write norm (‖o-c‖₂)
            std::memcpy(entry + num_words * sizeof(uint64_t),
                        &code.norm, sizeof(float));
            // Write sum_x
            std::memcpy(entry + num_words * sizeof(uint64_t) + sizeof(float),
                        &code.sum_x, sizeof(uint32_t));
        }
    }
    return encoded;
}

// ============================================================================
// SafeOut comparison: static vs dynamic
// ============================================================================

struct SafeOutStats {
    uint32_t safe_out = 0;
    uint32_t safe_in = 0;
    uint32_t uncertain = 0;
    uint32_t false_prunes = 0;  // SafeOut vectors actually in GT top-k
};

static SafeOutStats ClassifyAllVectors(
    const float* query, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const float* data,
    const std::vector<EncodedCluster>& encoded,
    const rabitq::RotationMatrix& rotation,
    const ConANN& conann,
    uint32_t top_k,
    const std::unordered_set<uint32_t>& gt_set,
    bool use_dynamic) {

    rabitq::RaBitQEstimator estimator(dim);
    uint32_t num_words = (dim + 63) / 64;
    uint32_t norm_byte_offset = num_words * sizeof(uint64_t);

    SafeOutStats stats;

    // Sort clusters by centroid distance
    std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        centroid_dists[c] = {
            simd::L2Sqr(query, centroids + static_cast<size_t>(c) * dim, dim), c};
    }
    std::sort(centroid_dists.begin(), centroid_dists.end());

    // Estimate heap for the relaxed SafeOut frontier.
    std::vector<EstimateHeapEntry> est_heap;
    float max_error_in_est_heap = 0.0f;

    for (uint32_t p = 0; p < nlist; ++p) {
        uint32_t cid = centroid_dists[p].second;
        const auto& enc = encoded[cid];

        auto pq = estimator.PrepareQuery(
            query, centroids + static_cast<size_t>(cid) * dim, rotation);

        // Compute r_max for this cluster
        float r_max = 0;
        for (uint32_t gid : cluster_members[cid]) {
            const float* vec = data + static_cast<size_t>(gid) * dim;
            float d = std::sqrt(simd::L2Sqr(vec, centroids + static_cast<size_t>(cid) * dim, dim));
            r_max = std::max(r_max, d);
        }
        float margin = 2.0f * r_max * pq.norm_qc * conann.epsilon();

        float safeout_frontier_upper = (use_dynamic && est_heap.size() >= top_k)
            ? est_heap.front().distance + max_error_in_est_heap
            : conann.d_k();

        for (uint32_t vi = 0; vi < enc.count; ++vi) {
            const auto* entry = enc.codes_block.data() +
                                static_cast<size_t>(vi) * enc.code_entry_size;
            const auto* words = reinterpret_cast<const uint64_t*>(entry);
            float norm_oc;
            std::memcpy(&norm_oc, entry + norm_byte_offset, sizeof(float));

            float dist = estimator.EstimateDistanceRaw(pq, words, num_words, norm_oc);

            // Classify
            ResultClass rc = use_dynamic
                ? conann.ClassifyAdaptive(dist, margin, safeout_frontier_upper)
                : conann.Classify(dist, margin);

            uint32_t gid = cluster_members[cid][vi];
            if (rc == ResultClass::SafeOut) {
                stats.safe_out++;
                if (gt_set.count(gid)) stats.false_prunes++;
            } else if (rc == ResultClass::SafeIn) {
                stats.safe_in++;
            } else {
                stats.uncertain++;
            }

            if (use_dynamic && rc != ResultClass::SafeOut) {
                const EstimateHeapEntry estimate{dist, margin, gid};
                if (est_heap.size() < top_k) {
                    est_heap.push_back(estimate);
                    std::push_heap(est_heap.begin(), est_heap.end());
                    max_error_in_est_heap =
                        std::max(max_error_in_est_heap, estimate.error_bound);
                } else if (estimate.distance < est_heap.front().distance) {
                    std::pop_heap(est_heap.begin(), est_heap.end());
                    est_heap.back() = estimate;
                    std::push_heap(est_heap.begin(), est_heap.end());
                    max_error_in_est_heap = RecomputeMaxError(est_heap);
                }
            }
        }
    }
    return stats;
}

// ============================================================================
// CRC search with RaBitQ estimates
// ============================================================================

struct CrcSearchResult {
    double avg_probed = 0;
    double recall_at_1 = 0;
    double recall_at_5 = 0;
    double recall_at_10 = 0;
    double fnr = 0;
    uint32_t early_stopped = 0;
    double avg_search_time_us = 0;
};

static CrcSearchResult RunCrcRaBitQ(
    float alpha, uint32_t top_k, uint32_t nlist, Dim dim, uint64_t seed,
    const float* img_data, const float* qry_data, uint32_t Q,
    const std::vector<float>& centroids,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const std::vector<std::vector<uint32_t>>& gt_topk,
    const std::vector<EncodedCluster>& encoded,
    const rabitq::RotationMatrix& rotation) {

    // Build ClusterData with codes.
    std::vector<std::vector<float>> cluster_vecs(nlist);
    std::vector<ClusterData> clusters(nlist);
    for (uint32_t c = 0; c < nlist; ++c) {
        cluster_vecs[c].resize(static_cast<size_t>(cluster_members[c].size()) * dim);
        for (size_t vi = 0; vi < cluster_members[c].size(); ++vi) {
            uint32_t gid = cluster_members[c][vi];
            std::memcpy(cluster_vecs[c].data() + vi * dim,
                        img_data + static_cast<size_t>(gid) * dim,
                        dim * sizeof(float));
        }
        clusters[c].vectors = cluster_vecs[c].data();
        clusters[c].ids = cluster_members[c].data();
        clusters[c].count = static_cast<uint32_t>(cluster_members[c].size());
        clusters[c].codes_block = encoded[c].codes_block.data();
        clusters[c].code_entry_size = encoded[c].code_entry_size;
    }

    // Calibrate with RaBitQ distances.
    CrcCalibrator::Config config;
    config.alpha = alpha;
    config.top_k = top_k;
    config.seed = seed;

    auto [cal, eval] = CrcCalibrator::CalibrateWithRaBitQ(
        config, qry_data, Q, dim,
        centroids.data(), nlist, clusters, rotation);

    Log("  Calibration: lamhat=%.4f  d_min=%.4f  d_max=%.4f\n",
        cal.lamhat, cal.d_min, cal.d_max);
    Log("  Test FNR=%.4f  avg_probed=%.2f  recall@10=%.4f\n",
        eval.actual_fnr, eval.avg_probed, eval.recall_at_10);

    // Full search with CrcStopper using RaBitQ estimates.
    CrcStopper stopper(cal, nlist);
    rabitq::RaBitQEstimator estimator(dim);
    uint32_t num_words = (dim + 63) / 64;
    uint32_t norm_byte_offset = num_words * sizeof(uint64_t);

    CrcSearchResult result;
    double sum_r1 = 0, sum_r5 = 0, sum_r10 = 0, sum_probed = 0;
    uint32_t fn_count = 0, early_count = 0;
    double total_time_us = 0;

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry_data + static_cast<size_t>(qi) * dim;
        auto t0 = std::chrono::steady_clock::now();

        // Sort clusters by centroid distance.
        std::vector<std::pair<float, uint32_t>> centroid_dists(nlist);
        for (uint32_t c = 0; c < nlist; ++c) {
            centroid_dists[c] = {
                simd::L2Sqr(qvec, centroids.data() + static_cast<size_t>(c) * dim, dim), c};
        }
        std::sort(centroid_dists.begin(), centroid_dists.end());

        // RaBitQ estimate heap for CRC.
        std::vector<std::pair<float, uint32_t>> est_heap;
        uint32_t probed = 0;

        for (uint32_t p = 0; p < nlist; ++p) {
            uint32_t cid = centroid_dists[p].second;
            const auto& enc = encoded[cid];

            auto pq = estimator.PrepareQuery(
                qvec, centroids.data() + static_cast<size_t>(cid) * dim, rotation);

            for (uint32_t vi = 0; vi < enc.count; ++vi) {
                const auto* entry = enc.codes_block.data() +
                                    static_cast<size_t>(vi) * enc.code_entry_size;
                const auto* words = reinterpret_cast<const uint64_t*>(entry);
                float norm_oc;
                std::memcpy(&norm_oc, entry + norm_byte_offset, sizeof(float));

                float dist = estimator.EstimateDistanceRaw(
                    pq, words, num_words, norm_oc);

                uint32_t gid = cluster_members[cid][vi];
                if (est_heap.size() < top_k) {
                    est_heap.push_back({dist, gid});
                    std::push_heap(est_heap.begin(), est_heap.end());
                } else if (dist < est_heap.front().first) {
                    std::pop_heap(est_heap.begin(), est_heap.end());
                    est_heap.back() = {dist, gid};
                    std::push_heap(est_heap.begin(), est_heap.end());
                }
            }
            probed++;

            float kth_dist = est_heap.size() >= top_k
                ? est_heap.front().first
                : std::numeric_limits<float>::infinity();
            if (stopper.ShouldStop(probed, kth_dist)) {
                early_count++;
                break;
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        total_time_us += std::chrono::duration<double, std::micro>(t1 - t0).count();

        // Extract result IDs.
        std::sort_heap(est_heap.begin(), est_heap.end());
        std::vector<uint32_t> result_ids;
        result_ids.reserve(est_heap.size());
        for (auto& [d, id] : est_heap) result_ids.push_back(id);

        // Recall against exact GT.
        auto recall_at = [&](uint32_t K) -> double {
            uint32_t pk = std::min(K, static_cast<uint32_t>(result_ids.size()));
            uint32_t gk = std::min(K, static_cast<uint32_t>(gt_topk[qi].size()));
            std::unordered_set<uint32_t> gt_set(gt_topk[qi].begin(),
                                                 gt_topk[qi].begin() + gk);
            uint32_t hits = 0;
            for (uint32_t i = 0; i < pk; ++i) {
                if (gt_set.count(result_ids[i])) ++hits;
            }
            return gk > 0 ? static_cast<double>(hits) / gk : 0.0;
        };

        sum_r1 += recall_at(1);
        sum_r5 += recall_at(5);
        sum_r10 += recall_at(std::min(10u, top_k));
        sum_probed += probed;

        std::unordered_set<uint32_t> pred_set(result_ids.begin(), result_ids.end());
        for (uint32_t gt_id : gt_topk[qi]) {
            if (!pred_set.count(gt_id)) { fn_count++; break; }
        }
    }

    result.recall_at_1 = sum_r1 / Q;
    result.recall_at_5 = sum_r5 / Q;
    result.recall_at_10 = sum_r10 / Q;
    result.avg_probed = sum_probed / Q;
    result.fnr = static_cast<double>(fn_count) / Q;
    result.early_stopped = early_count;
    result.avg_search_time_us = total_time_us / Q;

    return result;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetArg(argc, argv, "--dataset",
                                  "/home/zcq/VDB/data/coco_1k");
    uint32_t nlist = static_cast<uint32_t>(GetIntArg(argc, argv, "--nlist", 16));
    uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    float alpha = GetFloatArg(argc, argv, "--alpha", 0.1f);
    int q_limit = GetIntArg(argc, argv, "--queries", 0);
    uint32_t max_iter = static_cast<uint32_t>(GetIntArg(argc, argv, "--max-iter", 20));
    uint64_t seed = static_cast<uint64_t>(GetIntArg(argc, argv, "--seed", 42));
    uint32_t p_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-dk", 99));
    uint32_t p_for_eps_ip = static_cast<uint32_t>(GetIntArg(argc, argv, "--p-for-eps-ip", 95));
    uint32_t samples_for_dk = static_cast<uint32_t>(GetIntArg(argc, argv, "--samples-for-dk", 100));
    uint32_t samples_for_eps_ip = static_cast<uint32_t>(GetIntArg(argc, argv, "--samples-for-eps-ip", 100));

    Log("=== CRC OverlapScheduler Integration Benchmark ===\n");
    Log("Dataset: %s\n", data_dir.c_str());
    Log("nlist=%u, topk=%u, alpha=%.2f, P_dk=%u, P_eps=%u\n\n",
        nlist, top_k, alpha, p_for_dk, p_for_eps_ip);

    // ================================================================
    // Phase 1: Load data
    // ================================================================
    Log("[Phase 1] Loading data...\n");

    auto img_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_or.ok()) {
        std::fprintf(stderr, "Failed to load images: %s\n",
                     img_or.status().ToString().c_str());
        return 1;
    }
    auto& img = img_or.value();
    uint32_t N = img.rows;
    Dim dim = static_cast<Dim>(img.cols);

    auto qry_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load queries: %s\n",
                     qry_or.status().ToString().c_str());
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

    std::vector<std::vector<uint32_t>> gt_topk(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry.data.data() + static_cast<size_t>(qi) * dim;
        std::vector<std::pair<float, uint32_t>> dists(N);
        for (uint32_t j = 0; j < N; ++j) {
            dists[j] = {simd::L2Sqr(qvec,
                img.data.data() + static_cast<size_t>(j) * dim, dim), j};
        }
        std::partial_sort(dists.begin(), dists.begin() + top_k, dists.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        gt_topk[qi].resize(top_k);
        for (uint32_t k = 0; k < top_k; ++k) {
            gt_topk[qi][k] = dists[k].second;
        }
    }
    Log("  Done.\n\n");

    // ================================================================
    // Phase 3: K-Means + RaBitQ encoding
    // ================================================================
    Log("[Phase 3] K-Means (K=%u) + RaBitQ encoding...\n", nlist);

    std::vector<float> centroids;
    std::vector<uint32_t> assignments;
    RunSuperKMeans(img.data.data(), N, dim, nlist, centroids, assignments);

    std::vector<std::vector<uint32_t>> cluster_members(nlist);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments[i]].push_back(i);
    }

    // Generate rotation matrix.
    rabitq::RotationMatrix rotation(dim);
    rotation.GenerateRandom(seed);

    // Encode all clusters.
    auto encoded = EncodeAllClusters(img.data.data(), dim, centroids, nlist,
                                     cluster_members, rotation);

    Log("  Cluster sizes:");
    for (uint32_t k = 0; k < nlist; ++k) {
        Log(" %zu", cluster_members[k].size());
    }
    Log("\n\n");

    // ================================================================
    // Phase 4a: Calibrate d_k
    // ================================================================
    Log("[Phase 4a] Calibrating d_k (P%u, %u samples)...\n",
        p_for_dk, samples_for_dk);

    float p_dk = static_cast<float>(p_for_dk) / 100.0f;
    float d_k = ConANN::CalibrateDistanceThreshold(
        qry.data.data(), Q, img.data.data(), N, dim,
        std::min(samples_for_dk, Q), top_k, p_dk, seed);

    Log("  d_k=%.4f (L2², from qry→img)\n\n", d_k);

    // ================================================================
    // Phase 4b: Calibrate ε_ip from actual RaBitQ inner-product errors
    // ================================================================
    Log("[Phase 4b] Calibrating ε_ip (P%u, %u samples/cluster)...\n",
        p_for_eps_ip, samples_for_eps_ip);

    // Encode individual vectors to get RaBitQCode objects (need sign_code access)
    rabitq::RaBitQEncoder encoder(dim, rotation);
    rabitq::RaBitQEstimator estimator_cal(dim);
    std::vector<rabitq::RaBitQCode> codes(N);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t k = assignments[i];
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;
        codes[i] = encoder.Encode(
            img.data.data() + static_cast<size_t>(i) * dim, centroid);
    }

    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
    const uint32_t num_words_cal = (dim + 63) / 64;

    // Per-cluster r_max (from encoded norms)
    std::vector<float> per_cluster_r_max(nlist, 0.0f);
    for (uint32_t k = 0; k < nlist; ++k) {
        for (uint32_t idx : cluster_members[k]) {
            per_cluster_r_max[k] = std::max(per_cluster_r_max[k], codes[idx].norm);
        }
    }

    // Sample intra-cluster query-vector pairs, compute |ŝ - s|
    std::vector<float> ip_errors;
    for (uint32_t k = 0; k < nlist; ++k) {
        const auto& members = cluster_members[k];
        if (members.size() < 2) continue;

        uint32_t n_queries = std::min(samples_for_eps_ip,
                                       static_cast<uint32_t>(members.size()));
        const float* centroid = centroids.data() + static_cast<size_t>(k) * dim;

        std::vector<uint32_t> indices(members.size());
        std::iota(indices.begin(), indices.end(), 0u);
        std::mt19937 rng(seed + k);
        std::shuffle(indices.begin(), indices.end(), rng);

        for (uint32_t q = 0; q < n_queries; ++q) {
            uint32_t q_global = members[indices[q]];
            const float* q_vec = img.data.data() +
                static_cast<size_t>(q_global) * dim;
            auto pq = estimator_cal.PrepareQuery(q_vec, centroid, rotation);

            for (uint32_t t = 0; t < static_cast<uint32_t>(members.size()); ++t) {
                if (t == indices[q]) continue;
                uint32_t t_global = members[t];
                const auto& code = codes[t_global];

                // Popcount path: ŝ = 1 - 2·hamming/dim
                uint32_t hamming = simd::PopcountXor(
                    pq.sign_code.data(), code.code.data(), num_words_cal);
                float ip_hat = 1.0f - 2.0f * static_cast<float>(hamming) /
                                              static_cast<float>(dim);

                // Accurate path: exact inner product with quantized directions
                float dot = 0.0f;
                for (uint32_t d = 0; d < dim; ++d) {
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

    ConANN conann(eps_ip, d_k);

    Log("  ε_ip = %.6f (P%u of %zu samples)\n", eps_ip, p_for_eps_ip, ip_errors.size());
    Log("  d_k=%.4f  epsilon=%.6f\n", d_k, conann.epsilon());

    // Print per-cluster r_max
    Log("\n  Per-Cluster r_max:\n");
    Log("  %-10s %-14s %-12s\n", "Cluster", "Num Vectors", "r_max");
    Log("  %-10s %-14s %-12s\n", "-------", "-----------", "-----");
    for (uint32_t k = 0; k < nlist; ++k) {
        Log("  %-10u %-14zu %.6f\n", k, cluster_members[k].size(),
            per_cluster_r_max[k]);
    }
    Log("\n");

    // ================================================================
    // Phase 5: SafeOut comparison (static vs dynamic)
    // ================================================================
    Log("[Phase 5] SafeOut comparison (static vs dynamic d_k)...\n\n");

    SafeOutStats total_static{}, total_dynamic{};

    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = qry.data.data() + static_cast<size_t>(qi) * dim;
        std::unordered_set<uint32_t> gt_set(gt_topk[qi].begin(), gt_topk[qi].end());

        auto s_static = ClassifyAllVectors(
            qvec, dim, centroids.data(), nlist, cluster_members,
            img.data.data(), encoded, rotation, conann, top_k, gt_set, false);
        auto s_dynamic = ClassifyAllVectors(
            qvec, dim, centroids.data(), nlist, cluster_members,
            img.data.data(), encoded, rotation, conann, top_k, gt_set, true);

        total_static.safe_out += s_static.safe_out;
        total_static.safe_in += s_static.safe_in;
        total_static.uncertain += s_static.uncertain;
        total_static.false_prunes += s_static.false_prunes;

        total_dynamic.safe_out += s_dynamic.safe_out;
        total_dynamic.safe_in += s_dynamic.safe_in;
        total_dynamic.uncertain += s_dynamic.uncertain;
        total_dynamic.false_prunes += s_dynamic.false_prunes;
    }

    uint32_t total_vecs = total_static.safe_out + total_static.safe_in + total_static.uncertain;
    Log("  %-12s | %-10s | %-10s | %-10s | %-12s | %-10s\n",
        "Method", "SafeOut", "SafeIn", "Uncertain", "FalsePrunes", "FP_Rate");
    Log("  -------------|------------|------------|------------|--------------|----------\n");
    Log("  %-12s | %-10u | %-10u | %-10u | %-12u | %.6f\n",
        "Static d_k",
        total_static.safe_out, total_static.safe_in, total_static.uncertain,
        total_static.false_prunes,
        total_static.safe_out > 0
            ? static_cast<double>(total_static.false_prunes) / total_static.safe_out : 0.0);
    Log("  %-12s | %-10u | %-10u | %-10u | %-12u | %.6f\n",
        "Dynamic d_k",
        total_dynamic.safe_out, total_dynamic.safe_in, total_dynamic.uncertain,
        total_dynamic.false_prunes,
        total_dynamic.safe_out > 0
            ? static_cast<double>(total_dynamic.false_prunes) / total_dynamic.safe_out : 0.0);
    Log("\n  Total vectors classified per query: %u\n", total_vecs / Q);
    Log("  SafeOut increase: %.1f%%\n\n",
        total_static.safe_out > 0
            ? 100.0 * (static_cast<double>(total_dynamic.safe_out) / total_static.safe_out - 1.0)
            : 0.0);

    // ================================================================
    // Phase 6: CRC search with RaBitQ estimates
    // ================================================================
    Log("[Phase 6] CRC search with RaBitQ estimates (alpha=%.2f)...\n", alpha);

    auto crc_result = RunCrcRaBitQ(
        alpha, top_k, nlist, dim, seed,
        img.data.data(), qry.data.data(), Q,
        centroids, cluster_members, gt_topk, encoded, rotation);

    Log("\n  CRC+RaBitQ search results:\n");
    Log("    FNR:          %.4f (target <= %.2f)\n", crc_result.fnr, alpha);
    Log("    avg_probed:   %.2f / %u\n", crc_result.avg_probed, nlist);
    Log("    early_stop:   %u / %u (%.1f%%)\n",
        crc_result.early_stopped, Q,
        100.0 * crc_result.early_stopped / Q);
    Log("    recall@1:     %.4f\n", crc_result.recall_at_1);
    Log("    recall@5:     %.4f\n", crc_result.recall_at_5);
    Log("    recall@10:    %.4f\n", crc_result.recall_at_10);
    Log("    avg_time:     %.1f us/query\n", crc_result.avg_search_time_us);

    Log("\nDone.\n");
    return 0;
}
