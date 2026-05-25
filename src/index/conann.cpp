#include "vdb/index/conann.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "vdb/simd/distance_l2.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace vdb {
namespace index {

// ============================================================================
// Construction
// ============================================================================

ConANN::ConANN(float epsilon, float d_k)
    : epsilon_(epsilon),
      d_k_(d_k),
      safein_d_k_(d_k),
      has_safein_d_k_(false),
      tau_in_(safein_d_k_ - 2.0f * epsilon),
      tau_out_(d_k + 2.0f * epsilon) {}

ConANN::ConANN(float epsilon, float legacy_d_k, float safein_d_k,
               bool has_safein_d_k)
    : epsilon_(epsilon),
      d_k_(legacy_d_k),
      safein_d_k_(has_safein_d_k ? safein_d_k : legacy_d_k),
      has_safein_d_k_(has_safein_d_k),
      tau_in_(safein_d_k_ - 2.0f * epsilon),
      tau_out_(legacy_d_k + 2.0f * epsilon) {}

ConANN ConANN::FromConfig(const RaBitQConfig& cfg, Dim dim, float d_k) {
    // epsilon = c_factor * 2^(-bits/2) / sqrt(dim)
    float eps = cfg.c_factor
              * std::pow(2.0f, -static_cast<float>(cfg.bits) / 2.0f)
              / std::sqrt(static_cast<float>(dim));
    return ConANN(eps, d_k);
}

// ============================================================================
// Classification
// ============================================================================

ResultClass ConANN::Classify(float approx_dist) const {
    if (approx_dist < tau_in_) {
        return ResultClass::SafeIn;
    }
    if (approx_dist > tau_out_) {
        return ResultClass::SafeOut;
    }
    return ResultClass::Uncertain;
}

ResultClass ConANN::Classify(float approx_dist, float margin) const {
    if (approx_dist > d_k_ + 2 * margin) {
        return ResultClass::SafeOut;
    }
    if (approx_dist < safein_d_k_ - 2 * margin) {
        return ResultClass::SafeIn;
    }
    return ResultClass::Uncertain;
}

// ============================================================================
// Calibration
// ============================================================================

float ConANN::CalibrateDistanceThreshold(
    const float* vectors, uint32_t N, Dim dim,
    uint32_t num_samples,
    uint32_t top_k,
    float percentile,
    uint64_t seed) {
    // Delegate to query/database separated overload.
    // top_k+1 compensates for self-distance: when query IS in the database,
    // dists includes a 0 entry (self-hit), so we need one extra to skip it.
    return CalibrateDistanceThreshold(
        vectors, N, vectors, N, dim,
        num_samples, top_k + 1, percentile, seed);
}

float ConANN::CalibrateDistanceThreshold(
    const float* queries, uint32_t Q,
    const float* database, uint32_t N,
    Dim dim,
    uint32_t num_samples,
    uint32_t top_k,
    float percentile,
    uint64_t seed) {

    if (Q == 0 || N == 0 || num_samples == 0) {
        return 0.0f;
    }

    if (top_k > N) top_k = N;
    if (top_k == 0) top_k = 1;
    if (num_samples > Q) num_samples = Q;

    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);

    // Sample query indices
    std::vector<uint32_t> indices(Q);
    for (uint32_t i = 0; i < Q; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), rng);
    indices.resize(num_samples);

    std::vector<float> sample_dk(num_samples);

    // Per-thread dists buffer to avoid false sharing and malloc in the hot loop
#ifdef _OPENMP
    const int nt = omp_get_max_threads();
#else
    const int nt = 1;
#endif
    std::vector<std::vector<float>> tl_dists(nt, std::vector<float>(N));

#pragma omp parallel for schedule(dynamic, 4)
    for (int s = 0; s < static_cast<int>(num_samples); ++s) {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        auto& dists = tl_dists[tid];
        const float* query = queries + static_cast<size_t>(indices[s]) * dim;

        for (uint32_t i = 0; i < N; ++i) {
            dists[i] = simd::L2Sqr(query,
                                    database + static_cast<size_t>(i) * dim,
                                    dim);
        }

        // top_k-1 because 0-indexed and no self-distance to skip
        uint32_t nth = std::min(top_k - 1, N - 1);
        std::nth_element(dists.begin(), dists.begin() + nth, dists.end());
        sample_dk[s] = dists[nth];
    }

    // Percentile: take the k-th smallest, where k = floor((1 - percentile) * N).
    // For P99 with 100 samples: k = floor(0.01 * 100) = 1 → 2nd smallest value.
    // This gives a tight (low) threshold suitable for early-stop decisions.
    std::sort(sample_dk.begin(), sample_dk.end());
    uint32_t k = static_cast<uint32_t>(
        std::floor((1.0f - percentile) * static_cast<float>(num_samples)));
    k = std::min(k, num_samples - 1);
    return sample_dk[k];
}

}  // namespace index
}  // namespace vdb
