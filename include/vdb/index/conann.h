#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace index {

// ============================================================================
// ConANN — Continuous Approximate Nearest Neighbor 3-class classifier
// ============================================================================

/// ConANN performs a three-way classification of candidate vectors based on
/// their approximate (RaBitQ) distance to the query vector.
///
/// The classification uses a dynamic margin derived from the inner-product
/// estimation error ε_ip and vector norms:
///
///   margin = 2 · r_max · r_q · ε_ip
///
///   - **SafeIn**:    approx_dist < d_k − margin
///   - **SafeOut**:   approx_dist > d_k + margin
///   - **Uncertain**: otherwise
///
/// where:
///   - d_k is a globally precomputed top-k reference distance (L2²).
///   - ε_ip is the global inner-product estimation error bound (stored here).
///   - r_max = max(‖o-c‖) within a cluster (stored in .clu lookup entry).
///   - r_q = ‖q-c‖ (query residual norm, computed at query time).
///
/// The margin is computed per (cluster, query) pair at query time via
/// Classify(approx_dist, margin). The legacy Classify(approx_dist) overload
/// uses the stored epsilon as a fixed margin for backward compatibility.
///
class ConANN {
 public:
    /// Construct a ConANN classifier with legacy exact d_k semantics.
    ///
    /// @param epsilon  ε_ip: inner-product estimation error bound (P95)
    /// @param d_k      Precomputed legacy top-k reference distance (L2²)
    ConANN(float epsilon, float d_k);

    /// Construct a ConANN classifier with separate legacy and SafeIn d_k.
    ///
    /// @param epsilon      ε_ip: inner-product estimation error bound (P95)
    /// @param legacy_d_k   Legacy exact-space top-k reference distance (L2²)
    /// @param safein_d_k   SafeIn threshold center in the active metric space
    /// @param has_safein_d_k  Whether safein_d_k was explicitly calibrated
    ConANN(float epsilon, float legacy_d_k, float safein_d_k,
           bool has_safein_d_k);

    /// Construct from RaBitQ config and a precomputed d_k.
    ///
    /// Computes epsilon from the config:
    ///   epsilon = cfg.c_factor * pow(2, -cfg.bits / 2.0f) / sqrt(dim)
    ///
    /// @param cfg  RaBitQ configuration (provides c_factor, bits)
    /// @param dim  Vector dimensionality
    /// @param d_k  Precomputed global top-k reference distance
    /// @return     ConANN instance
    static ConANN FromConfig(const RaBitQConfig& cfg, Dim dim, float d_k);

    /// Classify a candidate using the stored epsilon as a fixed margin.
    /// Legacy overload — prefer Classify(approx_dist, margin) for correct
    /// error bounds.
    ///
    /// @param approx_dist  Approximate squared L2 distance from RaBitQ
    /// @return             SafeIn / SafeOut / Uncertain
    ResultClass Classify(float approx_dist) const;

    /// Classify a candidate using a dynamic margin.
    ///
    /// The caller computes: margin = 2 · r_max · r_q · ε_ip
    ///
    /// @param approx_dist  Approximate squared L2 distance from RaBitQ
    /// @param margin       Dynamic distance error bound for this (cluster, query)
    /// @return             SafeIn / SafeOut / Uncertain
    ResultClass Classify(float approx_dist, float margin) const;

    /// Classify with dynamic SafeOut threshold.
    ///
    /// SafeOut uses dynamic_d_k (from RaBitQ estimate heap) instead of static d_k.
    /// SafeIn remains static and uses safein_d_k_ when available, otherwise
    /// falls back to the legacy d_k_ for backward compatibility.
    ///
    /// @param approx_dist  Approximate squared L2 distance from RaBitQ
    /// @param margin       Dynamic distance error bound for this (cluster, query)
    /// @param dynamic_d_k  Current k-th distance from RaBitQ estimate heap
    /// @return             SafeIn / SafeOut / Uncertain
    VDB_FORCE_INLINE ResultClass ClassifyAdaptive(float approx_dist, float margin,
                                                   float dynamic_d_k) const {
        if (approx_dist > dynamic_d_k + 2 * margin) return ResultClass::SafeOut;
        if (approx_dist < safein_d_k_ - 2 * margin)  return ResultClass::SafeIn;
        return ResultClass::Uncertain;
    }

    VDB_FORCE_INLINE ResultClass ClassifyAdaptive(float approx_dist,
                                                  float safein_margin,
                                                  float safeout_margin,
                                                  float dynamic_d_k) const {
        if (approx_dist > dynamic_d_k + 2 * safeout_margin) return ResultClass::SafeOut;
        if (approx_dist < safein_d_k_ - 2 * safein_margin) return ResultClass::SafeIn;
        return ResultClass::Uncertain;
    }

    /// Calibrate the global distance threshold d_k by sampling from the dataset.
    ///
    /// Algorithm:
    ///   1. Randomly sample `num_samples` vectors as pseudo-queries.
    ///   2. For each sample query, compute brute-force L2 distances to ALL
    ///      vectors in the dataset, find the top_k-th nearest distance.
    ///   3. Collect all sample top-k distances.
    ///   4. Sort and take the specified percentile as the global d_k.
    ///
    /// This d_k represents a "conservative" estimate of the top-k distance
    /// across the dataset, used as the center of the SafeIn/SafeOut interval.
    ///
    /// @param vectors     Row-major array of N vectors (N × dim floats)
    /// @param N           Number of vectors in the dataset
    /// @param dim         Vector dimensionality
    /// @param num_samples Number of pseudo-queries to sample (default 100)
    /// @param top_k       The k for top-k distance (default 10)
    /// @param percentile  Which percentile of sample d_k values to use (0–1,
    ///                    default 0.99). Higher = more conservative (fewer
    ///                    SafeOut, fewer missed true positives).
    /// @param seed        Random seed for reproducibility (0 = random)
    /// @return            Calibrated d_k value
    static float CalibrateDistanceThreshold(
        const float* vectors, uint32_t N, Dim dim,
        uint32_t num_samples = 100,
        uint32_t top_k = 10,
        float percentile = 0.99f,
        uint64_t seed = 42);

    /// Calibrate d_k with separate query and database arrays.
    ///
    /// Samples from `queries` array, searches against `database` array.
    /// No self-distance handling needed (queries are not in database).
    /// Use this for cross-modal retrieval (e.g., text→image).
    ///
    /// @param queries     Row-major array of Q query vectors
    /// @param Q           Number of query vectors
    /// @param database    Row-major array of N database vectors
    /// @param N           Number of database vectors
    /// @param dim         Vector dimensionality
    /// @param num_samples Number of queries to sample (clamped to Q)
    /// @param top_k       The k for top-k distance
    /// @param percentile  Which percentile of sample d_k values to use (0–1)
    /// @param seed        Random seed for reproducibility (0 = random)
    /// @return            Calibrated d_k value
    static float CalibrateDistanceThreshold(
        const float* queries, uint32_t Q,
        const float* database, uint32_t N,
        Dim dim,
        uint32_t num_samples = 100,
        uint32_t top_k = 10,
        float percentile = 0.99f,
        uint64_t seed = 42);

    /// Get the inner-product estimation error bound ε_ip.
    float epsilon() const { return epsilon_; }

    /// Get the precomputed global top-k reference distance.
    float d_k() const { return d_k_; }

    /// Get the legacy exact-space top-k reference distance.
    float legacy_d_k() const { return d_k_; }

    /// Get the SafeIn threshold center. Falls back to legacy d_k for old
    /// indexes that do not carry SafeIn-specific calibration.
    float safein_d_k() const { return safein_d_k_; }

    /// Whether the loaded index carries an explicit SafeIn d_k.
    bool has_safein_d_k() const { return has_safein_d_k_; }

    /// Get the SafeIn threshold: d_k − 2ε.
    float tau_in() const { return tau_in_; }

    /// Get the SafeOut threshold: d_k + 2ε.
    float tau_out() const { return tau_out_; }

 private:
    float epsilon_;    // ε_ip: inner-product estimation error bound
    float d_k_;        // Legacy precomputed exact-space top-k reference distance
    float safein_d_k_; // SafeIn threshold center, or legacy d_k_ as fallback
    bool has_safein_d_k_;
    float tau_in_;     // = safein_d_k − 2ε
    float tau_out_;    // = legacy d_k + 2ε
};

}  // namespace index
}  // namespace vdb
