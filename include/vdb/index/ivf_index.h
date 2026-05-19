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

    /// Get the ConANN classifier.
    const ConANN& conann() const { return conann_; }
    void OverrideConANN(float epsilon, float d_k) { conann_ = ConANN(epsilon, d_k); }

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
    Dim logical_dim_ = 0;
    std::string padding_mode_ = "none";
    std::string rotation_mode_ = "random_matrix";
    std::vector<float> normalized_centroids_;
    CoarsePackedLayout packed_centroids_;
    CoarsePackedLayout packed_normalized_centroids_;

    struct CoarseScratch {
        std::vector<float> scores;
        std::vector<uint32_t> order;
        std::vector<float> query_buffer;
    };
    mutable std::unique_ptr<CoarseScratch> coarse_scratch_;
    mutable double last_coarse_score_ms_ = 0;
    mutable double last_coarse_topn_ms_ = 0;
    mutable bool use_coarse_select_simd_ = true;
    mutable bool use_coarse_select_phase2_ = false;

#ifdef VDB_USE_MKL
    // Precomputed ||c||² for each centroid (MKL-accelerated distance)
    std::vector<float> centroid_norms_;
#endif
};

}  // namespace index
}  // namespace vdb
