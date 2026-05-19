#pragma once

#include <cstdint>
#include <memory>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"
#include "vdb/index/conann.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace index {

// ============================================================================
// ClusterProber — cluster candidate classification (Stage 1 + Stage 2)
// ============================================================================

/// Classification of a non-SafeOut candidate passed to ProbeResultSink.
///
/// SafeOut lanes are consumed internally by ClusterProber (counted in
/// ProbeStats.s1_safeout / s2_safeout). Only SafeIn and Uncertain
/// candidates are forwarded to the sink.
enum class CandidateClass { SafeIn, Uncertain };

struct CandidateBatch {
    static constexpr uint32_t kMaxCandidates = 32;

    uint32_t count = 0;
    uint32_t global_idx[kMaxCandidates] = {};
    float est_dist[kMaxCandidates] = {};
    CandidateClass cls[kMaxCandidates] = {};
    AddressEntry decoded_addr[kMaxCandidates] = {};
};

/// Sink for non-SafeOut candidates produced by ClusterProber::Probe.
///
/// Implementors handle I/O submission (e.g. AsyncIOSink in OverlapScheduler).
/// Called once per compacted block-local batch in probe order.
class ProbeResultSink {
 public:
    virtual ~ProbeResultSink() = default;

    /// Called for a compacted batch of candidates that survived Stage 1 (and
    /// optionally Stage 2) classification.
    virtual void OnCandidates(const CandidateBatch& batch) = 0;
};

/// Per-cluster probe statistics.
struct ProbeStats {
    uint32_t s1_safein = 0;
    uint32_t s1_safeout = 0;
    uint32_t s1_uncertain = 0;
    uint32_t s2_safein = 0;
    uint32_t s2_safeout = 0;
    uint32_t s2_uncertain = 0;
    uint32_t num_stage1_blocks = 0;
    uint32_t num_stage2_candidates = 0;
    uint32_t num_stage2_block_lookups = 0;
    uint32_t num_stage2_block_reuses = 0;
    double stage1_ms = 0;
    double stage1_estimate_ms = 0;
    double stage1_mask_ms = 0;
    double stage1_iterate_ms = 0;
    double stage1_classify_ms = 0;
    double stage2_ms = 0;
    double stage2_collect_ms = 0;
    double stage2_kernel_ms = 0;
    double stage2_scatter_ms = 0;
    double stage2_kernel_sign_flip_ms = 0;
    double stage2_kernel_abs_fma_ms = 0;
    double stage2_kernel_tail_ms = 0;
    double stage2_kernel_reduce_ms = 0;
};

/// Cluster candidate classifier implementing the two-stage RaBitQ pipeline.
///
/// Encapsulates the FastScan Stage 1 / ExRaBitQ Stage 2 classification loop
/// from bench_vector_search (FastScan path, L883-1003).
///
/// Usage:
///   float margin_factor = 2.0f * pq.norm_qc * conann.epsilon();
///   float dynamic_d_k   = est_heap_full ? est_heap.front().first
///                                        : infinity;
///   prober.Probe(pc, pq, margin_factor, dynamic_d_k, sink, stats);
class ClusterProber {
 public:
    /// @param conann  ConANN classifier (caller owns, must outlive ClusterProber)
    /// @param dim     Vector dimensionality
    /// @param bits    RaBitQ quantization bits (1, 2, or 4)
    ClusterProber(const ConANN& conann, Dim dim, uint8_t bits);
    ~ClusterProber();

    VDB_DISALLOW_COPY_AND_MOVE(ClusterProber);

    /// Probe one parsed cluster and forward non-SafeOut candidates to sink.
    ///
    /// FastScan Stage 1: batch-32 SIMD SafeOut mask + per-lane SafeIn/Uncertain.
    /// Stage 2: ExRaBitQ re-classification for S1-Uncertain (when bits > 1).
    ///
    /// @param pc             Parsed cluster (Region 1 + Region 2)
    /// @param view           Lightweight prepared query view for this cluster
    /// @param margin_factor  2 * pq.norm_qc * conann.epsilon()
    /// @param dynamic_d_k    Current k-th estimate distance:
    ///                         (est_heap.size() >= top_k) ? est_heap.front()
    ///                                                     : +infinity
    /// @param sink           Receives non-SafeOut candidates
    /// @param stats          Accumulated classification statistics
    void Probe(const query::ParsedCluster& pc,
               const rabitq::PreparedClusterQueryView& view,
               float margin_factor,
               float dynamic_d_k,
               bool enable_address_decode_simd,
               bool enable_fine_grained_timing,
               bool enable_stage2_collect_block_first,
               bool enable_stage2_scatter_batch_classify,
               ProbeResultSink& sink,
               ProbeStats& stats) const;

 private:
    const ConANN& conann_;
    Dim dim_;
    uint8_t bits_;
    bool has_s2_;
    float margin_s2_divisor_;   // 2^(bits-1), precomputed; 1.0 when bits==1
    rabitq::RaBitQEstimator estimator_;
};

}  // namespace index
}  // namespace vdb
