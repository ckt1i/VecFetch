#pragma once

#include <cstdint>

#include "vdb/common/types.h"
#include "vdb/index/crc_stopper.h"
#include "vdb/query/result_collector.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace query {

enum class SubmissionMode : uint8_t {
    Shared = 0,
    Isolated = 1,
};

enum class CluReadMode : uint8_t {
    Window = 0,
    FullPreload = 1,
};

struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 8;
    uint32_t probe_batch_size = 128;
    uint32_t cluster_atomic_threshold = 1024;
    uint32_t io_queue_depth = 64;
    uint32_t cq_entries = 4096;
    uint32_t safein_all_threshold = 256 * 1024;  // 256KB
    uint32_t cluster_submit_reserve = 8;
    bool use_sqpoll = false;
    SubmissionMode submission_mode = SubmissionMode::Shared;

    // Early stop: skip remaining clusters when TopK quality exceeds d_k
    bool early_stop = true;

    // Phase 8: Async cluster prefetch
    uint32_t prefetch_depth = 16;      // Initial cluster block prefetch count
    uint32_t initial_prefetch = 4;     // CRC mode: initial cluster prefetch count
    uint32_t refill_threshold = 2;     // Refill when inflight_clusters drops below
    uint32_t refill_count = 2;         // Number of clusters to refill per check
    CluReadMode clu_read_mode = CluReadMode::Window;
    bool use_resident_clusters = false;
    bool enable_fine_grained_timing = true;

    // CRC early stop parameters (nullptr = use legacy d_k early stop)
    const index::CalibrationResults* crc_params = nullptr;
    bool crc_no_break = false;  // experiment mode: evaluate CRC but never break

    // Submit batching: submit when pending vec requests reach N.
    // `0` preserves the legacy "submit on pressure/final drain" behavior.
    uint32_t submit_batch_size = 32;
    bool enable_online_submit_tuning = false;
    float submit_ema_alpha = 0.25f;
    uint32_t submit_batch_min = 16;
    uint32_t submit_batch_max = 48;

    bool enable_address_decode_simd = true;
    bool enable_rerank_batched_distance_simd = true;
    bool enable_coarse_select_simd = true;
    bool enable_coarse_select_phase2 = false;
    bool enable_stage2_collect_block_first = true;
    bool enable_stage2_scatter_batch_classify = true;
};

struct SearchStats {
    uint32_t total_probed = 0;
    uint32_t total_safe_in = 0;
    uint32_t total_safe_out = 0;
    uint32_t total_uncertain = 0;
    uint32_t total_io_submitted = 0;
    uint32_t total_reranked = 0;
    uint32_t total_payload_prefetched = 0;
    uint32_t total_payload_fetched = 0;
    uint32_t total_safein_payload_prefetched = 0;
    uint32_t total_submit_calls = 0;
    uint32_t total_submit_window_flushes = 0;
    uint32_t total_submit_window_tail_flushes = 0;
    uint32_t total_submit_stop_flushes = 0;
    uint32_t total_submit_window_requests = 0;
    uint32_t total_candidate_batches = 0;
    uint32_t total_crc_estimates_buffered = 0;
    uint32_t total_crc_estimates_merged = 0;
    uint32_t total_crc_would_stop = 0;
    uint32_t total_stage2_block_lookups = 0;
    uint32_t total_stage2_block_reuses = 0;
    uint32_t duplicate_candidates = 0;
    uint32_t deduplicated_candidates = 0;
    uint32_t unique_fetch_candidates = 0;
    uint32_t buffered_candidates = 0;
    uint32_t reranked_candidates = 0;
    bool early_stopped = false;
    uint32_t clusters_skipped = 0;
    uint32_t crc_clusters_probed = 0;
    double coarse_select_ms = 0;
    double coarse_score_ms = 0;
    double coarse_topn_ms = 0;
    double probe_time_ms = 0;
    double probe_prepare_ms = 0;
    double probe_prepare_rotation_ms = 0;
    double probe_prepare_subtract_ms = 0;
    double probe_prepare_normalize_ms = 0;
    double probe_prepare_quantize_ms = 0;
    double probe_prepare_lut_build_ms = 0;
    double probe_prepare_quant_lut_ms = 0;
    double probe_stage1_ms = 0;
    double probe_stage1_estimate_ms = 0;
    double probe_stage1_mask_ms = 0;
    double probe_stage1_iterate_ms = 0;
    double probe_stage1_classify_only_ms = 0;
    double probe_stage2_ms = 0;
    double probe_stage2_collect_ms = 0;
    double probe_stage2_kernel_ms = 0;
    double probe_stage2_scatter_ms = 0;
    double probe_stage2_kernel_sign_flip_ms = 0;
    double probe_stage2_kernel_abs_fma_ms = 0;
    double probe_stage2_kernel_tail_ms = 0;
    double probe_stage2_kernel_reduce_ms = 0;
    double probe_classify_ms = 0;
    double probe_submit_ms = 0;
    double probe_submit_prepare_vec_only_ms = 0;
    double probe_submit_prepare_all_ms = 0;
    double probe_submit_emit_ms = 0;
    double rerank_time_ms = 0;
    double rerank_cpu_ms = 0;
    double total_time_ms = 0;
    double io_wait_time_ms = 0;
    // Stage 2 (ExRaBitQ re-classification, only when bits > 1)
    uint32_t s2_safe_in = 0;
    uint32_t s2_safe_out = 0;
    uint32_t s2_uncertain = 0;
    // Fine-grained timing breakdown (ms)
    double uring_prep_ms = 0;    // io_uring PrepRead() calls in AsyncIOSink batch submit path
    double uring_submit_ms = 0;  // reader_.Submit() calls in pipeline
    double parse_cluster_ms = 0; // ParseClusterBlock() in DispatchCompletion()
    double fetch_missing_ms = 0; // FetchMissingPayloads() wall time
    double prefetch_submit_ms = 0;          // Reserved field: disabled in low-overhead benchmark path
    double prefetch_wait_ms = 0;            // Reserved field: disabled in low-overhead benchmark path
    double safein_payload_prefetch_ms = 0;  // Reserved field: disabled in low-overhead benchmark path
    double candidate_collect_ms = 0;        // Organize buffered candidates before batch rerank
    double pool_vector_read_ms = 0;         // Batch read/visit of prefetched vectors from memory pool
    double rerank_compute_ms = 0;           // Batch L2/top-k compute
    double remaining_payload_fetch_ms = 0;  // Final missing payload fetch
    double crc_decision_ms = 0;             // Time spent inside CrcStopper::ShouldStop
    double crc_buffer_ms = 0;               // Time spent buffering CRC estimates
    double crc_merge_ms = 0;                // Time spent merging CRC estimates into est_heap
};

class SearchContext {
 public:
    SearchContext(const float* query_vec, const SearchConfig& config)
        : query_vec_(query_vec), config_(config),
          collector_(config.top_k) {}

    const float* query_vec() const { return query_vec_; }
    const SearchConfig& config() const { return config_; }
    SearchStats& stats() { return stats_; }
    const SearchStats& stats() const { return stats_; }
    ResultCollector& collector() { return collector_; }

 private:
    const float* query_vec_;
    SearchConfig config_;
    SearchStats stats_;
    ResultCollector collector_;
};

}  // namespace query
}  // namespace vdb
