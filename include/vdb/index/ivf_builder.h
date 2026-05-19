#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_metadata.h"

namespace vdb {
namespace index {

// ============================================================================
// IvfBuilderConfig — configuration for building an IVF index
// ============================================================================

/// Configuration for IvfBuilder.  All parameters for IVF clustering,
/// RaBitQ encoding, ConANN calibration, and on-disk layout.
struct IvfBuilderConfig {
    /// Coarse builder selection. Auto keeps the current repository behavior:
    /// HierarchicalSuperKMeans for K >= 128 and SuperKMeans otherwise.
    CoarseBuilder coarse_builder = CoarseBuilder::Auto;

    /// Explicit assignment mode. Legacy behavior is preserved when this is
    /// left as Single and assignment_factor is set to 2.
    AssignmentMode assignment_mode = AssignmentMode::Single;

    /// Number of IVF clusters (nlist).
    uint32_t nlist = 16;

    /// K-means maximum iterations.
    uint32_t max_iterations = 20;

    /// K-means convergence tolerance (relative centroid movement).
    float tolerance = 1e-4f;

    /// Random seed for K-means initialization and ConANN calibration.
    uint64_t seed = 42;

    /// Optional precomputed centroids file (.fvecs).
    /// When set (along with assignments_path), skip clustering entirely.
    std::string centroids_path;

    /// Optional precomputed assignments file (.ivecs).
    /// When set (along with centroids_path), skip clustering entirely.
    std::string assignments_path;

    /// RaBitQ configuration for encoding.
    RaBitQConfig rabitq;

    /// Payload column schemas (empty = no payload).
    std::vector<ColumnSchema> payload_schemas;

    // ----- ConANN calibration -----
    /// Number of pseudo-query samples for d_k calibration.
    uint32_t calibration_samples = 100;

    /// Top-k used in d_k calibration.
    uint32_t calibration_topk = 10;

    /// Percentile for d_k calibration (0–1, e.g. 0.99).
    float calibration_percentile = 0.99f;

    /// Page size for DataFile alignment (bytes). 1 = no padding.
    uint32_t page_size = 1;

    /// Segment ID to embed in segment.meta.
    uint64_t segment_id = 0;

    /// Default nprobe stored in segment.meta.
    uint32_t nprobe = 1;

    /// Number of pseudo-query samples per cluster for ε_ip calibration.
    /// If cluster_size < 2 * epsilon_samples, uses max(cluster_size / 2, 1).
    uint32_t epsilon_samples = 20;

    /// Percentile for ε_ip calibration (0–1, e.g. 0.95).
    float epsilon_percentile = 0.95f;

    /// Optional query vectors for d_k calibration (cross-modal support).
    /// If provided, d_k is calibrated from query→database distances.
    /// If nullptr, fallback to database self-sampling (existing behavior).
    /// Pointer must remain valid for the duration of Build().
    const float* calibration_queries = nullptr;
    uint32_t num_calibration_queries = 0;

    /// Top-k for CRC score precomputation. 0 = skip CRC scores.
    /// When > 0 and calibration_queries is set, IvfBuilder will compute
    /// QueryScores for each calibration query using RaBitQ full-probe search
    /// and serialize them to crc_scores.bin in the output directory.
    uint32_t crc_top_k = 0;

    /// Optional paths to save clustering results after automatic K-means.
    /// If non-empty, centroids/assignments are written as .fvecs/.ivecs
    /// for reuse in future builds. Ignored when loading precomputed files.
    std::string save_centroids_path;
    std::string save_assignments_path;
    std::string save_secondary_assignments_path;

    /// Assignment factor for IVF membership. `1` keeps the current
    /// single-assignment behavior; `2` enables top-2 redundant assignment.
    uint32_t assignment_factor = 1;

    /// RAIR lambda for AIR-style secondary assignment.
    float rair_lambda = 0.75f;

    /// When true, force a distinct secondary cluster even if the primary
    /// cluster minimizes the AIR loss.
    bool rair_strict_second_choice = false;

    /// Metric label for build metadata sidecar. The current segment metadata
    /// remains L2-oriented; this string is written to build_metadata.json for
    /// offline diagnostics and experiment bookkeeping.
    std::string metric = "l2";

    /// Faiss training rows used by coarse_builder=faiss_kmeans.
    /// Matches the existing Python exporter default semantics.
    uint32_t faiss_train_size = 100000;

    /// Optional Faiss-specific iteration count. Zero falls back to
    /// max_iterations so the existing build knob remains the primary control.
    uint32_t faiss_niter = 0;

    /// Number of Faiss k-means restarts.
    uint32_t faiss_nredo = 1;

    /// Experimental mode: for non-power-of-two dimensions, zero-pad vectors
    /// to the next power of two so the build path can use Hadamard rotation.
    bool pad_non_power_of_two_to_pow2 = false;

    /// When true, non-power-of-two dimensions can use blocked Hadamard with a
    /// deterministic permutation instead of falling back to a dense random matrix.
    bool use_blocked_hadamard_permuted = true;
};

// ============================================================================
// PayloadFn — callback to supply per-vector payload during Build
// ============================================================================

/// Callback to supply payload columns for a given vector index.
/// Returns a vector of Datum values matching config_.payload_schemas.
/// Return empty vector if no payload for this vector.
using PayloadFn = std::function<std::vector<Datum>(uint32_t vec_index)>;

// ============================================================================
// IvfBuilder — builds an IVF+RaBitQ index to disk
// ============================================================================

/// Builds a complete IVF index from raw float vectors.
///
/// Build phases:
///   1. **K-means clustering** (optionally capacity-constrained via
///      balance_factor).
///   2. **ConANN calibration** — sample-based d_k estimation.
///   3. **Per-cluster writing** — for each cluster:
///      a. RaBitQ encode all assigned vectors.
///      b. Write a .dat DataFile (raw vectors, optional payload).
///      c. Write a .clu ClusterStore (centroid, codes, norms, address column).
///   4. **Global metadata** — write centroids.bin, rotation.bin, segment.meta
///      (FlatBuffers).
///
/// Usage:
///   IvfBuilderConfig cfg;
///   cfg.nlist = 16;
///   IvfBuilder builder(cfg);
///   // vectors: row-major N × dim float array
///   Status s = builder.Build(vectors, N, dim, output_dir);
///
class IvfBuilder {
 public:
    explicit IvfBuilder(const IvfBuilderConfig& config);
    ~IvfBuilder();

    VDB_DISALLOW_COPY_AND_MOVE(IvfBuilder);

    /// Build the full IVF index from raw vectors and write to `output_dir`.
    ///
    /// @param vectors    Row-major array of N × dim floats
    /// @param N          Number of vectors
    /// @param dim        Vector dimensionality
    /// @param output_dir Output directory (will be created if missing)
    /// @param payload_fn Optional callback to supply per-vector payload.
    ///                   If nullptr, records are written with empty payload.
    /// @return Status
    Status Build(const float* vectors, uint32_t N, Dim dim,
                 const std::string& output_dir,
                 PayloadFn payload_fn = nullptr);

    /// Optional progress callback: called once per cluster written.
    /// Arguments: (cluster_index, total_clusters)
    using ProgressCallback = std::function<void(uint32_t, uint32_t)>;
    void SetProgressCallback(ProgressCallback cb) { progress_cb_ = std::move(cb); }

    /// Get the assignments from the last Build() call  (length N).
    /// assignments[i] = primary cluster index [0, nlist) for vector i.
    const std::vector<uint32_t>& assignments() const { return assignments_; }

    /// Get the secondary assignments from the last Build() call.
    /// Values are UINT32_MAX when redundant assignment is disabled or when a
    /// secondary cluster is not present.
    const std::vector<uint32_t>& secondary_assignments() const {
        return secondary_assignments_;
    }

    AssignmentMode assignment_mode() const { return assignment_mode_; }
    float rair_lambda() const { return rair_lambda_; }
    bool rair_strict_second_choice() const { return rair_strict_second_choice_; }
    ClusteringSource clustering_source() const { return clustering_source_; }
    CoarseBuilder coarse_builder() const { return coarse_builder_used_; }
    Dim logical_dim() const { return logical_dim_; }
    Dim effective_dim() const { return effective_dim_; }
    const std::string& padding_mode() const { return padding_mode_; }
    const std::string& rotation_mode() const { return rotation_mode_; }

    /// Get the centroids from the last Build() call (nlist × dim row-major).
    const std::vector<float>& centroids() const { return centroids_; }

    /// Get the calibrated d_k from the last Build() call.
    float calibrated_dk() const { return calibrated_dk_; }

 private:
    // ----- Internal helpers -----

    /// Phase A: capacity-constrained K-means clustering.
    Status RunKMeans(const float* vectors, uint32_t N, Dim dim);

    /// Phase B: calibrate ConANN d_k by sampling.
    void CalibrateDk(const float* vectors, uint32_t N, Dim dim);

    /// Derive secondary assignments from final centroids for top-2
    /// redundant-assignment mode.
    Status DeriveSecondaryAssignments(const float* vectors, uint32_t N, Dim dim);

    /// Phase C+D: write per-cluster files + global metadata.
    Status WriteIndex(const float* raw_vectors,
                      const float* encoded_vectors,
                      uint32_t N,
                      Dim dim,
                      const std::string& output_dir,
                      PayloadFn payload_fn);

    IvfBuilderConfig config_;
    std::vector<uint32_t> assignments_;  // vector → cluster index
    std::vector<uint32_t> secondary_assignments_;
    std::vector<float> centroids_;       // nlist × dim row-major
    float calibrated_dk_ = 0.0f;
    float calibrated_eps_ip_ = 0.0f;
    AssignmentMode assignment_mode_ = AssignmentMode::Single;
    float rair_lambda_ = 0.75f;
    bool rair_strict_second_choice_ = false;
    ClusteringSource clustering_source_ = ClusteringSource::Auto;
    CoarseBuilder coarse_builder_used_ = CoarseBuilder::Auto;
    std::string effective_metric_ = "l2";
    Dim logical_dim_ = 0;
    Dim effective_dim_ = 0;
    std::string padding_mode_ = "none";
    std::string rotation_mode_ = "random_matrix";
    ProgressCallback progress_cb_;
};

}  // namespace index
}  // namespace vdb
