#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vdb/common/aligned_alloc.h"
#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/index/ivf_metadata.h"
#include "vdb/simd/coarse_select.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/storage/segment.h"

namespace faiss {
struct IndexHNSWFlat;
}

namespace vdb {
namespace index {

// ============================================================================
// IvfIndex — IVF index for query-time cluster selection
// ============================================================================

/// IVF index that loads centroids and segment metadata from disk.
///
/// Provides:
///   1. Find the `nprobe` nearest clusters for a given query.
///   2. Access to the underlying Segment (ClusterStoreReader / DataFileReader).
///   3. Access to the ConANN classifier.
///   4. Access to the shared RotationMatrix (loaded from rotation.bin).
///
/// On-disk layout (loaded by Open):
///   <dir>/
///   ├── segment.meta         # SegmentMeta FlatBuffers
///   ├── centroids.bin        # nlist × dim × float32 (raw binary)
///   ├── rotation.bin         # dim × dim × float32 (raw binary)
///   ├── cluster.clu          # unified ClusterStore file
///   └── data.dat             # unified DataFile
///
class IvfIndex {
 public:
    struct CoarsePackedLayout {
        CacheAlignedVector<float> data;
        uint32_t centroid_block = 0;
        uint32_t vec_width = 0;
        uint32_t num_centroid_blocks = 0;
        uint32_t num_dim_blocks = 0;
        size_t packed_dim = 0;

        bool empty() const { return data.empty(); }
    };

    struct CoarseTopKEntry {
        float score = 0.0f;
        uint32_t child = 0;
    };

    IvfIndex();
    ~IvfIndex();

    VDB_DISALLOW_COPY_AND_MOVE(IvfIndex);

    /// Open an IVF index from a directory.
    ///
    /// Reads segment.meta, centroids.bin, rotation.bin, and registers all
    /// clusters into the internal Segment.
    ///
    /// @param dir  Path to the directory containing the index files
    /// @return     Status
    Status Open(const std::string& dir, bool use_direct_io = false);

    /// Find the nprobe nearest clusters for a query vector.
    ///
    /// Computes L2 squared distance from the query to all centroids,
    /// then returns the nprobe clusters with the smallest distances.
    ///
    /// @param query   Raw query vector (length = dim)
    /// @param nprobe  Number of clusters to probe
    /// @return        ClusterIDs of the nprobe nearest clusters,
    ///                ordered by distance (nearest first)
    std::vector<ClusterID> FindNearestClusters(const float* query,
                                                uint32_t nprobe) const;
    double last_coarse_score_ms() const { return last_coarse_score_ms_; }
    double last_coarse_topn_ms() const { return last_coarse_topn_ms_; }
    bool use_coarse_select_simd() const { return use_coarse_select_simd_; }
    void SetUseCoarseSelectSimd(bool enabled) { use_coarse_select_simd_ = enabled; }
    bool use_coarse_select_phase2() const { return use_coarse_select_phase2_; }
    void SetUseCoarseSelectPhase2(bool enabled) { use_coarse_select_phase2_ = enabled; }
    void SetTwoLevelCoarseRouting(bool enabled,
                                  uint32_t threshold,
                                  uint32_t super_count,
                                  uint32_t super_factor,
                                  uint32_t budget_factor,
                                  bool exact_overlap = false);
    bool PrepareTwoLevelCoarseRouting(uint32_t nprobe) const;
    void SetHnswCoarseRouting(bool enabled,
                              uint32_t m,
                              uint32_t ef_construction,
                              uint32_t ef_search);
    bool PrepareHnswCoarseRouting() const;
    uint32_t last_coarse_routing_mode() const { return last_coarse_routing_mode_; }
    uint32_t last_coarse_super_count() const { return last_coarse_super_count_; }
    uint32_t last_coarse_super_probes() const { return last_coarse_super_probes_; }
    uint32_t last_coarse_child_candidates_scored() const {
        return last_coarse_child_candidates_scored_;
    }
    uint32_t last_coarse_candidate_budget() const { return last_coarse_candidate_budget_; }
    uint32_t last_coarse_exact_fallback() const { return last_coarse_exact_fallback_; }
    uint32_t last_coarse_exact_overlap() const { return last_coarse_exact_overlap_; }
    double last_coarse_hierarchy_build_ms() const { return last_coarse_hierarchy_build_ms_; }
    double last_coarse_hnsw_graph_build_ms() const { return last_coarse_hnsw_graph_build_ms_; }
    uint32_t last_coarse_hnsw_m() const { return last_coarse_hnsw_m_; }
    uint32_t last_coarse_hnsw_ef_search() const { return last_coarse_hnsw_ef_search_; }
    uint32_t last_coarse_hnsw_returned_clusters() const {
        return last_coarse_hnsw_returned_clusters_;
    }
    uint32_t last_coarse_hnsw_visited_nodes() const {
        return last_coarse_hnsw_visited_nodes_;
    }
    uint32_t last_coarse_hnsw_distance_computations() const {
        return last_coarse_hnsw_distance_computations_;
    }
    uint32_t last_coarse_hnsw_hops() const { return last_coarse_hnsw_hops_; }
    static uint32_t DefaultTwoLevelSuperCount(uint32_t nlist,
                                              uint32_t override_count = 0,
                                              uint32_t nprobe = 0,
                                              uint32_t factor = 0);
    static uint32_t DefaultTwoLevelCandidateBudget(uint32_t nlist,
                                                   uint32_t nprobe,
                                                   uint32_t budget_factor = 8);

    /// Get the ConANN classifier.
    const ConANN& conann() const { return conann_; }
    void OverrideConANN(float epsilon, float d_k) { conann_ = ConANN(epsilon, d_k); }
    void OverrideConANN(float epsilon, float legacy_d_k, float safein_d_k,
                        bool has_safein_d_k) {
        conann_ = ConANN(epsilon, legacy_d_k, safein_d_k, has_safein_d_k);
    }

    /// Get the underlying Segment (mutable for lazy-opening readers).
    storage::Segment& segment() { return segment_; }

    /// Get the shared rotation matrix.
    const rabitq::RotationMatrix& rotation() const { return *rotation_; }

    /// Number of clusters (nlist).
    uint32_t nlist() const { return nlist_; }

    /// Vector dimensionality.
    Dim dim() const { return dim_; }
    Dim logical_dim() const { return logical_dim_; }
    Dim effective_dim() const { return dim_; }
    const std::string& padding_mode() const { return padding_mode_; }
    const std::string& rotation_mode() const { return rotation_mode_; }
    bool uses_padded_hadamard() const {
        return logical_dim_ != dim_ && rotation_mode_ == "hadamard_padded";
    }

    /// Get centroid for a specific cluster (row-major offset into centroids_).
    const std::vector<float>& centroids() const { return centroids_; }

    const float* centroid(uint32_t cluster_idx) const {
        return centroids_.data() + static_cast<size_t>(cluster_idx) * dim_;
    }

    /// Whether Hadamard rotation was detected at Open() time (dim is power-of-2).
    /// When true, rotated_centroid() is valid and PrepareQueryRotatedInto can be used.
    bool used_hadamard() const { return used_hadamard_; }

    /// Get pre-rotated centroid P^T × c_k for the given cluster.
    /// Only valid when used_hadamard() == true.
    const float* rotated_centroid(uint32_t cluster_idx) const {
        return rotated_centroids_.data() + static_cast<size_t>(cluster_idx) * dim_;
    }

    /// Get all cluster IDs.
    const std::vector<ClusterID>& cluster_ids() const { return cluster_ids_; }

    /// The directory this index was loaded from.
    const std::string& dir() const { return dir_; }

    /// Payload column schemas (loaded from segment.meta).
    const std::vector<ColumnSchema>& payload_schemas() const { return payload_schemas_; }

    AssignmentMode assignment_mode() const { return assignment_mode_; }
    uint32_t assignment_factor() const { return assignment_factor_; }
    float rair_lambda() const { return rair_lambda_; }
    bool rair_strict_second_choice() const { return rair_strict_second_choice_; }
    ClusteringSource clustering_source() const { return clustering_source_; }
    CoarseBuilder coarse_builder() const { return coarse_builder_; }
    const std::string& requested_metric() const { return requested_metric_; }
    const std::string& effective_metric() const { return effective_metric_; }
    SafeInDkSpace safein_dk_space() const { return safein_dk_space_; }
    SafeInDkSearchScope safein_dk_search_scope() const { return safein_dk_search_scope_; }
    float safein_dk_percentile() const { return safein_dk_percentile_; }
    uint32_t safein_dk_calibration_samples() const { return safein_dk_calibration_samples_; }
    uint32_t safein_dk_nprobe() const { return safein_dk_nprobe_; }
    uint8_t safein_dk_bits() const { return safein_dk_bits_; }

 private:
    std::string dir_;
    Dim dim_ = 0;
    uint32_t nlist_ = 0;

    std::vector<float> centroids_;              // row-major, nlist × dim
    std::vector<float> rotated_centroids_;      // P^T × c_k, nlist × dim (Hadamard only)
    bool used_hadamard_ = false;                // true when dim is power-of-2
    std::vector<ClusterID> cluster_ids_;        // ordered cluster IDs
    ConANN conann_{0.0f, 0.0f};                // default, overwritten by Open
    storage::Segment segment_;
    std::unique_ptr<rabitq::RotationMatrix> rotation_;

    // Payload schemas (loaded from segment meta, for DataFileReader)
    std::vector<ColumnSchema> payload_schemas_;
    AssignmentMode assignment_mode_ = AssignmentMode::Single;
    uint32_t assignment_factor_ = 1;
    float rair_lambda_ = 0.75f;
    bool rair_strict_second_choice_ = false;
    ClusteringSource clustering_source_ = ClusteringSource::Auto;
    CoarseBuilder coarse_builder_ = CoarseBuilder::Auto;
    std::string requested_metric_ = "l2";
    std::string effective_metric_ = "l2";
    SafeInDkSpace safein_dk_space_ = SafeInDkSpace::ExactL2;
    SafeInDkSearchScope safein_dk_search_scope_ = SafeInDkSearchScope::FullDatabase;
    float safein_dk_percentile_ = 0.0f;
    uint32_t safein_dk_calibration_samples_ = 0;
    uint32_t safein_dk_nprobe_ = 0;
    uint8_t safein_dk_bits_ = 0;
    Dim logical_dim_ = 0;
    std::string padding_mode_ = "none";
    std::string rotation_mode_ = "random_matrix";
    std::vector<float> normalized_centroids_;
    CoarsePackedLayout packed_centroids_;
    CoarsePackedLayout packed_normalized_centroids_;

    struct HierarchicalCoarseIndex {
        std::vector<float> super_centroids;
        std::vector<float> normalized_super_centroids;
        std::vector<uint32_t> child_offsets;
        std::vector<uint32_t> child_centroid_ids;
        CoarsePackedLayout packed_super_centroids;
        CoarsePackedLayout packed_normalized_super_centroids;
        std::vector<CoarsePackedLayout> packed_child_centroids_by_super;
        std::vector<CoarsePackedLayout> packed_normalized_child_centroids_by_super;
        uint32_t n_super = 0;
        uint32_t nlist = 0;
        Dim dim = 0;
        bool ready = false;
    };

    struct HnswCoarseGraph {
        std::unique_ptr<::faiss::IndexHNSWFlat> index;
        uint32_t m = 0;
        uint32_t ef_construction = 0;
        uint32_t ef_search = 0;
        uint32_t nlist = 0;
        Dim dim = 0;
        bool cosine_path = false;
        bool supported_metric = false;
        bool ready = false;
    };

    struct CoarseScratch {
        std::vector<float> scores;
        std::vector<uint32_t> order;
        std::vector<float> query_buffer;
        std::vector<float> super_scores;
        std::vector<uint32_t> super_order;
        std::vector<uint32_t> child_candidates;
        std::vector<float> child_scores;
        std::vector<uint32_t> child_order;
        std::vector<uint8_t> child_seen;
        std::vector<uint32_t> exact_order_for_overlap;
        std::vector<CoarseTopKEntry> child_topk;
    };
    mutable std::unique_ptr<CoarseScratch> coarse_scratch_;
    mutable double last_coarse_score_ms_ = 0;
    mutable double last_coarse_topn_ms_ = 0;
    mutable bool use_coarse_select_simd_ = true;
    mutable bool use_coarse_select_phase2_ = false;
    mutable bool use_two_level_coarse_routing_ = false;
    mutable bool use_hnsw_coarse_routing_ = false;
    mutable uint32_t hnsw_coarse_m_ = 32;
    mutable uint32_t hnsw_coarse_ef_construction_ = 128;
    mutable uint32_t hnsw_coarse_ef_search_ = 512;
    mutable uint32_t two_level_coarse_threshold_ = 4096;
    mutable uint32_t two_level_coarse_super_count_ = 0;
    mutable uint32_t two_level_coarse_super_factor_ = 0;
    mutable uint32_t two_level_coarse_budget_factor_ = 8;
    mutable bool two_level_coarse_exact_overlap_ = false;
    mutable HierarchicalCoarseIndex coarse_hierarchy_;
    mutable HnswCoarseGraph coarse_hnsw_;
    mutable double last_coarse_hierarchy_build_ms_ = 0;
    mutable double last_coarse_hnsw_graph_build_ms_ = 0;
    mutable uint32_t last_coarse_routing_mode_ = 0;
    mutable uint32_t last_coarse_super_count_ = 0;
    mutable uint32_t last_coarse_super_probes_ = 0;
    mutable uint32_t last_coarse_child_candidates_scored_ = 0;
    mutable uint32_t last_coarse_candidate_budget_ = 0;
    mutable uint32_t last_coarse_exact_fallback_ = 0;
    mutable uint32_t last_coarse_exact_overlap_ = 0;
    mutable uint32_t last_coarse_hnsw_m_ = 0;
    mutable uint32_t last_coarse_hnsw_ef_search_ = 0;
    mutable uint32_t last_coarse_hnsw_returned_clusters_ = 0;
    mutable uint32_t last_coarse_hnsw_visited_nodes_ = 0;
    mutable uint32_t last_coarse_hnsw_distance_computations_ = 0;
    mutable uint32_t last_coarse_hnsw_hops_ = 0;

    uint32_t ResolveTwoLevelSuperCount(uint32_t nprobe) const;
    uint32_t ResolveTwoLevelCandidateBudget(uint32_t nprobe) const;
    bool EnsureCoarseHierarchy(uint32_t nprobe) const;
    bool EnsureHnswCoarseGraph() const;
    std::vector<ClusterID> FindNearestClustersExact(const float* query,
                                                    uint32_t nprobe) const;
    std::vector<ClusterID> FindNearestClustersTwoLevel(const float* query,
                                                       uint32_t nprobe) const;
    std::vector<ClusterID> FindNearestClustersHnsw(const float* query,
                                                   uint32_t nprobe) const;

#ifdef VDB_USE_MKL
    // Precomputed ||c||² for each centroid (MKL-accelerated distance)
    std::vector<float> centroid_norms_;
#endif
};

}  // namespace index
}  // namespace vdb
