#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/query/result_collector.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace query {

enum class SubmissionMode : uint8_t {
    Shared = 0,
    Isolated = 1,
};

enum class DynamicSafeInMode : uint8_t {
    Static = 0,
    Frontier = 1,
};

enum class QueryExecutionMode : uint8_t {
    Overlap = 0,
    SerialNoOverlap = 1,
};

enum class MaterializationMode : uint8_t {
    EagerSafeIn = 0,
    Late = 1,
};

enum class SafeInPrefetchOrder : uint8_t {
    Arrival = 0,
    Confidence = 1,
    ConfidencePerByte = 2,
};

struct SeparateRecordLocation {
    uint64_t row_id = 0;
    uint64_t payload_offset = 0;
    uint32_t payload_bytes = 0;
};

using SeparateRecordMap = std::unordered_map<uint64_t, SeparateRecordLocation>;

struct SeparateRecordStoreConfig {
    bool enabled = false;
    // Shadow-vector experiments keep payload reads on the combined data.dat
    // while redirecting only raw-vector verification to vector_fd.
    bool redirect_payload_reads = true;
    int vector_fd = -1;
    int payload_fd = -1;
    const SeparateRecordMap* address_map = nullptr;
};

struct VectorReadTraceEntry {
    uint32_t query_index = 0;
    uint32_t request_index = 0;
    uint64_t combined_offset = 0;
    uint32_t read_length = 0;
    uint8_t request_type = 0;  // 0=VEC_ONLY, 1=VEC_ALL vector component.
};

struct SafeInConfidenceTraceEntry {
    uint32_t query_index = 0;
    uint32_t probe_index = 0;
    uint32_t cluster_id = 0;
    uint32_t cluster_local_index = 0;
    uint64_t candidate_offset = 0;
    uint32_t record_bytes = 0;
    uint8_t classification_stage = 0;
    bool frontier_ready = false;
    bool has_gt_label = false;
    bool gt_topk = false;
    bool selected_topk = false;
    float est_dist = 0.0f;
    float est_error = 0.0f;
    float safein_margin = 0.0f;
    float safein_upper_bound = 0.0f;
    float safein_threshold = 0.0f;
    float safein_frontier = 0.0f;
    float raw_slack = 0.0f;
    float normalized_slack_error = 0.0f;
    float normalized_slack_safein_margin = 0.0f;
};

struct BextraWindowTraceEntry {
    uint32_t query_index = 0;
    uint32_t window_index = 0;
    uint32_t clusters_processed = 0;
    uint32_t clusters_remaining = 0;
    uint32_t eligible_candidates = 0;
    uint32_t scheduled_candidates = 0;
    double rho = 0.0;
    double probe_cluster_ema_ms = 0.0;
    double hide_time_ms = 0.0;
    uint64_t predicted_service_bytes = 0;
    uint64_t inflight_bytes = 0;
    uint64_t mandatory_pending_bytes = 0;
    uint64_t mandatory_future_bytes = 0;
    uint64_t eligible_extra_bytes = 0;
    uint64_t predicted_extra_bytes = 0;
    uint64_t scheduled_extra_bytes = 0;
    uint64_t completed_before_final_drain_bytes = 0;
    uint64_t spilled_to_final_drain_bytes = 0;
};

struct InlineHotRecordStoreConfig {
    bool enabled = false;
    int cold_payload_fd = -1;
    // Buffered descriptor/prefix reads remain valid when the main data reader
    // opens data.dat with O_DIRECT.
    int buffered_hot_record_fd = -1;
    uint32_t descriptor_bytes = 0;
    uint32_t inline_payload_threshold = 0;
    struct PayloadMetadata {
        uint64_t payload_offset = 0;
        uint64_t payload_bytes = 0;
        uint32_t inline_bytes = 0;
        uint8_t payload_storage_type = 0;
    };
    using PayloadMetadataMap =
        std::unordered_map<uint64_t, PayloadMetadata>;
    // Optional resident metadata plane keyed by combined record offset. This
    // is the inline-layout counterpart of NoCombine's resident address map.
    const PayloadMetadataMap* payload_metadata = nullptr;
};

struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 8;
    uint32_t probe_batch_size = 128;
    uint32_t cluster_atomic_threshold = 1024;
    uint32_t io_queue_depth = 64;
    uint32_t cq_entries = 4096;
    // SafeIn eager materialization reads at most this many bytes from the
    // beginning of each combined record. The scheduler always reads enough
    // bytes to cover the raw vector; large records fetch the remaining payload
    // suffix only if they survive into the final top-k.
    uint32_t safein_threshold_bytes = 256 * 1024;  // 256KB
    uint32_t cluster_submit_reserve = 8;
    bool use_sqpoll = false;
    SubmissionMode submission_mode = SubmissionMode::Shared;

    bool enable_fine_grained_timing = true;
    bool enable_hotpath_detailed_timing = false;

    // When enabled, Dynamic SafeOut maintains a kth upper-bound frontier for
    // estimate-driven SafeOut pruning.
    bool enable_dynamic_safeout = true;

    // Query-adaptive SafeIn threshold for payload prefetch. Static preserves
    // the legacy global safein_d_k threshold. Frontier uses the online kth
    // lower-bound frontier maintained during the query.
    DynamicSafeInMode dynamic_safein_mode = DynamicSafeInMode::Static;
    uint32_t dynamic_safein_min_probes = 0;
    uint32_t dynamic_safein_stable_probes = 2;
    float dynamic_safein_rel_tol = 0.005f;
    float dynamic_safein_abs_tol = 0.0f;
    uint32_t dynamic_safein_defer_initial_clusters = 0;
    bool dynamic_safein_defer_until_ready = false;
    uint32_t dynamic_safein_defer_max_candidates = 0;
    // EagerSafeIn allows high-confidence SafeIn candidates to issue early
    // full-record VEC_ALL reads. Late keeps SafeIn classification/frontier
    // statistics but verifies raw vectors first and materializes payloads only
    // after final top-k ranking.
    MaterializationMode materialization_mode = MaterializationMode::EagerSafeIn;
    // Backward-compatible ablation alias. Prefer materialization_mode=Late for
    // new code and experiments.
    bool safein_as_vec_only = false;
    bool late_materialization_enabled() const {
        return materialization_mode == MaterializationMode::Late ||
               safein_as_vec_only;
    }

    // Optional query-level guardrails for SafeIn full-record prefetch.
    // 0 keeps the legacy unlimited behavior. When either limit is exceeded,
    // the candidate is downgraded to VEC_ONLY verification; final top-k
    // payload materialization remains unchanged.
    uint32_t safein_prefetch_max_count = 0;
    uint64_t safein_prefetch_max_bytes = 0;
    // When non-zero, each data-submit flush emits at most this many SafeIn
    // full-record reads before giving VEC_ONLY reads a chance. Remaining
    // full-record reads are still emitted in the same flush if capacity remains.
    uint32_t safein_prefetch_emit_quantum = 0;
    // Static, query-level SafeIn scheduler. The legacy max-bytes limit counts
    // submitted bytes; query_extra_bytes counts bytes beyond raw-vector
    // verification and is therefore comparable across record layouts.
    SafeInPrefetchOrder safein_prefetch_order = SafeInPrefetchOrder::Arrival;
    uint32_t safein_prefetch_rank_batch_size = 32;
    // Non-zero enables a rolling cross-batch SafeIn admission window. Plans
    // are ranked together once the window fills; the tail flushes at drain.
    uint32_t safein_prefetch_global_window = 0;
    uint64_t safein_query_extra_bytes = 0;
    uint32_t safein_max_full_payload_bytes = 0;

    bool enable_safein_bextra_probe_budget = false;
    double safein_bextra_rho = 0.0;
    double safein_bextra_bytes_per_ms = 0.0;
    float safein_bextra_ema_alpha = 0.25f;

    // Submit batching: submit when pending vec requests reach N.
    // `0` preserves the legacy "submit on pressure/final drain" behavior.
    uint32_t submit_batch_size = 32;
    // Merge nearby VEC_ONLY candidates within a logical tile into one minimal
    // contiguous span. The span is admitted only when its bytes stay within
    // max_byte_amplification of issuing the vectors separately.
    bool enable_vec_span_coalescing = false;
    uint32_t vec_span_tile_bytes = 4096;
    float vec_span_max_byte_amplification = 1.10f;
    bool enable_online_submit_tuning = false;
    float submit_ema_alpha = 0.25f;
    uint32_t submit_batch_min = 16;
    uint32_t submit_batch_max = 48;
    uint32_t fixed_vec_buffer_count = 0;
    bool serial_data_drains = false;  // Benchmark-only: disable probe/I/O overlap.
    QueryExecutionMode execution_mode = QueryExecutionMode::Overlap;

    bool enable_address_decode_simd = true;
    bool enable_rerank_batched_distance_simd = true;
    bool enable_coarse_select_simd = true;
    bool enable_coarse_select_phase2 = false;
    bool enable_two_level_coarse_routing = false;
    uint32_t two_level_coarse_threshold = 4096;
    uint32_t two_level_coarse_super_count = 0;
    uint32_t two_level_coarse_super_factor = 0;
    uint32_t two_level_coarse_budget_factor = 8;
    uint32_t two_level_coarse_budget_cap = 0;
    bool enable_two_level_coarse_exact_overlap = false;
    bool enable_stage1_safein = true;
    bool enable_stage2_collect_block_first = true;
    bool enable_stage2_scatter_batch_classify = true;
    bool rabitq_active_ex_bits_set = false;
    uint8_t rabitq_active_ex_bits = 0;
    float safein_epsilon_override = -1.0f;
    float safeout_epsilon_override = -1.0f;

    // Optional cap for non-SafeOut candidates. When non-zero, the scheduler
    // keeps only the best estimated candidates for vector verification.
    uint32_t non_safeout_candidate_budget = 0;

    // Optional speculative raw-vector prefetch for budgeted queries. The final
    // rerank budget remains non_safeout_candidate_budget; prefetched vectors
    // are only used if their candidate is selected into the final budget.
    uint32_t budgeted_prefetch_limit = 0;

    // Benchmark-only No Combine mode. Candidate generation and cluster-resident
    // codes still use the source index, but raw-vector reads and payload reads
    // are redirected through a separated row-id address map.
    SeparateRecordStoreConfig separate_record_store;
    InlineHotRecordStoreConfig inline_hot_record_store;

    // Optional benchmark-only trace sink. Disabled in timed runs unless the
    // caller explicitly supplies a vector.
    std::vector<VectorReadTraceEntry>* vector_read_trace = nullptr;
    // Optional benchmark-only first-decision SafeIn trace. Each query/candidate
    // is recorded once at the first point where a speculative read could be
    // issued; selected_topk is filled after exact reranking finalizes.
    std::vector<SafeInConfidenceTraceEntry>* safein_confidence_trace = nullptr;
    std::vector<BextraWindowTraceEntry>* bextra_window_trace = nullptr;

    // Optional benchmark-only truth metadata for per-stage false classification
    // counters. cluster_members maps cluster-local vector offsets to original
    // database row ids; true_topk_rows is the current query's GT top-k set.
    const std::vector<std::vector<uint32_t>>* false_stats_cluster_members = nullptr;
    const std::unordered_set<uint32_t>* false_stats_true_topk_rows = nullptr;
};

struct SearchStats {
    uint32_t total_probed = 0;
    uint32_t total_safe_in = 0;
    uint32_t total_safe_out = 0;
    uint32_t s1_uncertain_raw = 0;
    uint32_t total_uncertain = 0;
    uint32_t total_io_submitted = 0;
    uint32_t total_reranked = 0;
    uint32_t total_payload_prefetched = 0;
    uint32_t total_payload_fetched = 0;
    uint32_t total_safein_payload_prefetched = 0;
    uint32_t total_submit_calls = 0;
    uint32_t total_submit_window_flushes = 0;
    uint32_t total_submit_window_tail_flushes = 0;
    uint32_t total_submit_window_requests = 0;
    uint32_t vec_only_read_requests = 0;
    uint32_t all_read_requests = 0;
    uint32_t payload_read_requests = 0;
    uint32_t separate_store_lookup_misses = 0;
    uint32_t inline_descriptor_read_requests = 0;
    uint32_t inline_descriptor_errors = 0;
    uint32_t inline_cold_payload_deferred = 0;
    uint32_t inline_payload_cache_hits = 0;
    uint64_t vec_only_read_bytes = 0;
    uint32_t vec_span_read_requests = 0;
    uint32_t vec_span_candidates = 0;
    uint64_t vec_span_read_bytes = 0;
    uint64_t all_read_bytes = 0;
    uint64_t payload_read_bytes = 0;
    uint32_t safein_prefix_read_requests = 0;
    uint32_t safein_full_read_requests = 0;
    uint32_t safein_suffix_read_requests = 0;
    uint64_t safein_prefix_read_bytes = 0;
    uint64_t safein_full_read_bytes = 0;
    uint64_t safein_suffix_read_bytes = 0;
    uint32_t serial_vector_read_requests = 0;
    uint32_t serial_full_record_read_requests = 0;
    uint32_t serial_payload_read_requests = 0;
    uint64_t serial_vector_read_bytes = 0;
    uint64_t serial_full_record_read_bytes = 0;
    uint64_t serial_payload_read_bytes = 0;
    uint32_t fixed_vec_buffer_hits = 0;
    uint32_t fixed_vec_buffer_misses = 0;
    uint32_t total_candidate_batches = 0;
    uint32_t total_safeout_frontier_estimates_buffered = 0;
    uint32_t total_safeout_frontier_estimates_merged = 0;
    uint32_t total_safeout_frontier_updates = 0;
    uint32_t dynamic_safein_clusters = 0;
    uint32_t dynamic_safein_active_clusters = 0;
    uint32_t dynamic_safein_disabled_clusters = 0;
    uint32_t dynamic_safein_threshold_changed_clusters = 0;
    uint32_t dynamic_safein_ready_transitions = 0;
    uint32_t dynamic_safein_frontier_samples = 0;
    uint32_t dynamic_safein_threshold_samples = 0;
    double dynamic_safein_frontier_sum = 0.0;
    double dynamic_safein_threshold_sum = 0.0;
    float dynamic_safein_final_frontier = 0.0f;
    float dynamic_safein_final_threshold = 0.0f;
    uint32_t dynamic_safein_deferred_candidates = 0;
    uint32_t dynamic_safein_deferred_flushes = 0;
    uint32_t dynamic_safein_deferred_safein = 0;
    uint32_t safein_prefetch_candidates = 0;
    uint32_t safein_prefetch_true_topk = 0;
    uint32_t safein_prefetch_false = 0;
    uint32_t safein_prefetch_unknown = 0;
    uint32_t safein_prefetch_considered = 0;
    uint32_t safein_prefetch_skipped_count_limit = 0;
    uint32_t safein_prefetch_skipped_byte_limit = 0;
    uint64_t safein_prefetch_scheduled_bytes = 0;
    uint32_t bextra_windows = 0;
    uint32_t bextra_eligible_candidates = 0;
    uint32_t bextra_scheduled_candidates = 0;
    uint64_t bextra_predicted_service_bytes = 0;
    uint64_t bextra_predicted_extra_bytes = 0;
    uint64_t bextra_eligible_extra_bytes = 0;
    uint64_t bextra_scheduled_extra_bytes = 0;
    uint64_t bextra_completed_before_final_drain_bytes = 0;
    uint64_t bextra_spilled_to_final_drain_bytes = 0;
    uint32_t total_stage2_block_lookups = 0;
    uint32_t total_stage2_block_reuses = 0;
    uint32_t duplicate_candidates = 0;
    uint32_t deduplicated_candidates = 0;
    uint32_t unique_fetch_candidates = 0;
    uint32_t buffered_candidates = 0;
    uint32_t reranked_candidates = 0;
    uint32_t candidate_budget_seen = 0;
    uint32_t candidate_budget_selected = 0;
    uint32_t candidate_budget_dropped = 0;
    uint32_t budgeted_prefetch_considered = 0;
    uint32_t budgeted_prefetch_scheduled = 0;
    uint32_t budgeted_prefetch_duplicates = 0;
    uint32_t budgeted_prefetch_skipped_limit = 0;
    uint32_t budgeted_prefetch_completed = 0;
    uint32_t budgeted_prefetch_cache_hits = 0;
    uint32_t budgeted_prefetch_inflight_uses = 0;
    uint32_t budgeted_prefetch_used = 0;
    uint32_t budgeted_prefetch_wasted = 0;
    uint64_t budgeted_prefetch_read_bytes = 0;
    double coarse_select_ms = 0;
    double coarse_score_ms = 0;
    double coarse_topn_ms = 0;
    uint32_t coarse_routing_mode = 0;  // 0=exact, 1=two_level
    uint32_t coarse_super_count = 0;
    uint32_t coarse_super_probes = 0;
    uint32_t coarse_child_candidates_scored = 0;
    uint32_t coarse_candidate_budget = 0;
    uint32_t coarse_exact_fallback = 0;
    uint32_t coarse_exact_overlap = 0;
    double coarse_hierarchy_build_ms = 0;
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
    double probe_stage2_decode_ms = 0;
    uint32_t stage1_fused_blocks = 0;
    uint32_t stage1_fused_safeout_lanes = 0;
    uint32_t stage1_fused_safein_lanes = 0;
    uint64_t stage2_masked_kernel_calls = 0;
    uint64_t stage2_lanes_requested = 0;
    uint64_t stage2_lanes_skipped = 0;
    uint64_t stage2_lanes_total_valid = 0;
    uint64_t stage2_decode_blocks = 0;
    uint64_t stage2_decode_input_bytes = 0;
    uint64_t stage2_decode_output_bytes = 0;
    uint64_t stage2_active_ex_bits_sum = 0;
    uint64_t stage2_stored_ex_bits_sum = 0;
    double probe_classify_ms = 0;
    double probe_submit_ms = 0;
    double probe_submit_prepare_vec_only_ms = 0;
    double probe_submit_prepare_all_ms = 0;
    double probe_submit_emit_ms = 0;
    double probe_submit_vec_only_emit_ms = 0;
    double probe_submit_pending_slot_alloc_ms = 0;
    double probe_submit_prep_read_ms = 0;
    double rerank_time_ms = 0;
    double rerank_cpu_ms = 0;
    double total_time_ms = 0;
    double io_wait_time_ms = 0;
    // Stage 2 (ExRaBitQ re-classification, only when bits > 1)
    uint32_t s2_safe_in = 0;
    uint32_t s2_safe_out = 0;
    uint32_t s2_uncertain = 0;
    uint32_t s1_false_safe_in = 0;
    uint32_t s1_false_safe_out = 0;
    uint32_t s2_false_safe_in = 0;
    uint32_t s2_false_safe_out = 0;
    // Fine-grained timing breakdown (ms)
    double uring_prep_ms = 0;    // io_uring PrepRead() calls in AsyncIOSink batch submit path
    double uring_submit_ms = 0;  // reader_.Submit() calls in pipeline
    double fetch_missing_ms = 0; // FetchMissingPayloads() wall time
    double safein_payload_prefetch_ms = 0;  // Reserved field: disabled in low-overhead benchmark path
    double candidate_collect_ms = 0;        // Organize buffered candidates before batch rerank
    double pool_vector_read_ms = 0;         // Batch read/visit of prefetched vectors from memory pool
	    double rerank_compute_ms = 0;           // Batch L2/top-k compute
	    double rerank_vec_alloc_ms = 0;         // Vector slab growth / allocation
	    double rerank_vec_copy_ms = 0;          // Copy vector bytes into rerank storage
	    double final_drain_ms = 0;              // Final submit/drain after probing clusters
	    double execute_buffered_ms = 0;         // Buffered rerank execution wall time
	    double collector_finalize_ms = 0;       // Top-k collector final sort/extract
	    double assemble_results_ms = 0;         // Materialize SearchResults rows
	    double search_unaccounted_ms = 0;       // Search total minus coarse/probe/tail fields
	    double remaining_payload_fetch_ms = 0;  // Final missing payload fetch
	    double serial_vector_read_ms = 0;       // Serial VEC_ONLY raw-vector reads
	    double serial_full_record_read_ms = 0;  // Serial VEC_ALL full-record reads
	    double serial_payload_read_ms = 0;      // Serial final or separate payload reads
    double safeout_frontier_buffer_ms = 0;  // Time spent buffering dynamic SafeOut estimates
    double safeout_frontier_merge_ms = 0;   // Time spent updating kth upper-bound frontier
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
