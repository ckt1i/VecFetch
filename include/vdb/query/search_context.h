#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/query/result_collector.h"
#include "vdb/query/span_planner.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace query {

inline constexpr size_t kSafeInOptionalLatencyBucketCount = 12;

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
    // Benchmark-only upper bound: admit only GT top-k SafeIn candidates.
    Oracle = 3,
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
    uint32_t flush_index = 0;
    uint32_t cluster_id = std::numeric_limits<uint32_t>::max();
    uint64_t combined_offset = 0;
    uint64_t physical_offset = 0;
    uint32_t read_length = 0;
    uint8_t request_type = 0;  // 0=VEC_ONLY, 1=VEC_ALL vector component.
    bool selected_topk = false;
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
    // Behavior-preserving P0 diagnostics for full probe-loop and I/O phase
    // attribution. Disabled by default because steady-clock sampling adds
    // measurable overhead in the query hot path.
    bool enable_pipeline_io_detailed_timing = false;

    // When enabled, Dynamic SafeOut maintains a kth upper-bound frontier for
    // estimate-driven SafeOut pruning.
    bool enable_dynamic_safeout = true;

    // Runtime-calibrated, precision-conditioned verification envelope.
    // Stage 1 uses the existing epsilon overrides; Stage 2 is independent
    // because active bit width is query-configurable.
    bool rabitq_pcve_enabled = false;
    float rabitq_pcve_stage2_epsilon_lower = 0.0f;
    float rabitq_pcve_stage2_epsilon_upper = 0.0f;

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
    // In decoupled payload-prefix mode, reserve this many SQEs at the front of
    // each submit batch so optional payload work can overlap later probing.
    uint32_t safein_prefetch_emit_reserve = 0;
    // Static, query-level SafeIn scheduler. The legacy max-bytes limit counts
    // submitted bytes; query_extra_bytes counts bytes beyond raw-vector
    // verification and is therefore comparable across record layouts.
    SafeInPrefetchOrder safein_prefetch_order = SafeInPrefetchOrder::Arrival;
    // Diagnostic control: execute oracle labeling but suppress optional
    // payload reads so oracle-selection overhead can be subtracted.
    bool oracle_prefetch_label_only = false;
    uint32_t safein_prefetch_rank_batch_size = 32;
    // Non-zero enables a rolling cross-batch SafeIn admission window. Plans
    // are ranked together once the window fills; the tail flushes at drain.
    uint32_t safein_prefetch_global_window = 0;
    uint64_t safein_query_extra_bytes = 0;
    uint32_t safein_max_full_payload_bytes = 0;
    // For payloads stored outside the vector-bearing record, issue a bounded
    // payload-prefix read alongside SafeIn vector verification. The prefix is
    // cached and the final top-k path reads only the remaining suffix.
    bool enable_safein_cold_payload_prefetch = false;
    uint32_t safein_cold_payload_prefix_bytes = 0;

    // P0 optional-I/O path. SafeIn payload reads use a dedicated reader and
    // are submitted only while mandatory vector I/O is below the configured
    // low watermark. Disabled by default to preserve the legacy scheduler.
    bool enable_safein_optional_io_isolation = false;
    uint32_t safein_optional_io_queue_depth = 8;
    uint32_t safein_optional_io_max_inflight = 4;
    uint32_t safein_optional_io_mandatory_low_watermark = 8;
    uint32_t safein_optional_io_min_remaining_probes = 4;
    bool enable_safein_optional_io_detailed_timing = false;
    // Record query-relative optional queue/submit/probe/drain timing without
    // adding diagnostic polls or changing scheduling.
    bool enable_safein_optional_io_timeline = false;
    // Keep the optional ring's task work deferred to explicit poll calls.
    // This is independently configurable from mandatory vector readers.
    bool safein_optional_io_defer_taskrun = true;
    // Force optional payload SQEs through io-wq. Mandatory vector SQEs never
    // inherit this setting.
    bool safein_optional_io_force_async = false;
    // Poll the optional ring during probe only when pending work is blocked by
    // max in-flight capacity. Also skips zero-work tail polls.
    bool enable_safein_optional_io_refill_only_polling = false;

    // Allocate the final payload-sized buffer at prefix submission time so a
    // surviving candidate can append its suffix without a second allocation
    // or prefix copy.
    bool enable_safein_reusable_payload_buffer = false;
    // Avoid retaining an unexpectedly large final-sized buffer for a
    // speculative prefix. Zero disables the cap.
    uint32_t safein_reusable_payload_buffer_max_bytes = 4u * 1024u * 1024u;

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
    // Four-way span-planner contract. The rational fields make admission
    // deterministic across platforms and avoid floating-point boundary drift.
    SpanPlannerMode vec_span_planner_mode = SpanPlannerMode::GV;
    uint32_t vec_span_alpha_num = 3;
    uint32_t vec_span_alpha_den = 2;
    uint32_t vec_span_safein_rho_num = 1;
    uint32_t vec_span_safein_rho_den = 1;
    // Legacy compatibility only; rational alpha is authoritative.
    float vec_span_max_byte_amplification = 1.10f;
    // Retain coalesced span buffers until final materialization and expose
    // fully covered inline payloads as zero-copy query-local views.
    bool enable_vec_span_payload_reuse = false;
    // Copy covered inline payloads into one compact buffer per span so the
    // larger vector-read buffer can return to the pool immediately.
    bool compact_vec_span_payload_reuse = false;
    // Extend at most this many coalesced reads from a final SafeIn vector
    // through the end of its hot record without issuing another SQE.
    uint32_t vec_span_safein_tail_count = 0;
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
    // Oracle admission is separate from false-stat instrumentation so GT
    // lookups are limited to SafeIn candidates rather than every scanned row.
    const std::vector<std::vector<uint32_t>>* oracle_cluster_members = nullptr;
    const std::unordered_set<uint32_t>* oracle_true_topk_rows = nullptr;
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
    uint32_t vec_span_payload_views = 0;
    uint64_t vec_span_payload_view_bytes = 0;
    uint64_t vec_span_payload_retained_bytes = 0;
    uint32_t vec_span_payload_reuse_hits = 0;
    uint64_t vec_span_payload_reuse_bytes = 0;
    uint32_t vec_span_payload_requests_avoided = 0;
    uint32_t vec_span_safein_tails_extended = 0;
    uint64_t vec_span_safein_tail_extra_bytes = 0;
    uint64_t vec_span_planner_calls = 0;
    uint64_t vec_span_planner_runs = 0;
    uint64_t vec_span_planner_candidates = 0;
    uint64_t vec_span_planner_groups = 0;
    uint64_t vec_span_planner_max_run = 0;
    uint64_t vec_span_planned_physical_bytes = 0;
    uint64_t vec_span_planned_vector_bytes = 0;
    uint64_t vec_span_planner_credit_bytes = 0;
    uint64_t vec_span_planner_admission_checks = 0;
    uint64_t vec_span_planner_fenwick_queries = 0;
    uint64_t vec_span_planner_fenwick_updates = 0;
    uint64_t vec_span_planner_workspace_growths = 0;
    uint64_t vec_span_planner_plan_hash = 0;
    uint64_t vec_span_planner_fallbacks = 0;
    double vec_span_planner_ms = 0.0;
    double vec_span_sort_ms = 0.0;
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
    uint32_t safein_cold_prefetch_requests = 0;
    uint32_t safein_cold_prefetch_full_payloads = 0;
    uint64_t safein_cold_prefetch_bytes = 0;
    uint64_t safein_cold_prefetch_used_bytes = 0;
    uint64_t safein_cold_prefetch_wasted_bytes = 0;
    uint32_t safein_optional_io_queued = 0;
    uint32_t safein_optional_io_submitted = 0;
    uint32_t safein_optional_io_completed = 0;
    uint32_t safein_optional_io_dropped_late = 0;
    uint64_t safein_optional_io_submitted_bytes = 0;
    uint64_t safein_optional_io_completed_bytes = 0;
    uint32_t safein_optional_io_submit_calls = 0;
    uint32_t safein_optional_io_nonblocking_poll_calls = 0;
    uint32_t safein_optional_io_empty_polls = 0;
    uint32_t safein_optional_io_poll_completions = 0;
    uint32_t safein_optional_io_get_events_calls = 0;
    uint32_t safein_optional_io_completed_before_probe_end = 0;
    uint64_t safein_optional_io_completed_bytes_before_probe_end = 0;
    uint32_t safein_optional_io_completed_in_final_drain = 0;
    uint64_t safein_optional_io_completed_bytes_in_final_drain = 0;
    uint32_t safein_optional_io_completed_in_optional_drain = 0;
    uint64_t safein_optional_io_completed_bytes_in_optional_drain = 0;
    uint32_t safein_optional_io_timeline_queries = 0;
    uint32_t safein_optional_io_timeline_submit_queries = 0;
    uint32_t safein_optional_io_timeline_completion_queries = 0;
    uint32_t safein_optional_io_first_submit_probes_remaining = 0;
    uint32_t safein_optional_io_first_submit_clusters_processed = 0;
    uint32_t safein_optional_io_inflight_at_probe_end = 0;
    uint32_t safein_optional_io_prepped_at_probe_end = 0;
    uint32_t safein_optional_io_pending_at_probe_end = 0;
    uint32_t safein_optional_io_inflight_at_final_drain_start = 0;
    uint32_t safein_optional_io_inflight_at_optional_drain_start = 0;
    uint32_t safein_optional_io_blocked_mandatory_calls = 0;
    uint32_t safein_optional_io_blocked_max_inflight_calls = 0;
    uint32_t safein_optional_io_blocked_short_window_calls = 0;
    double safein_optional_io_first_queue_offset_us = 0.0;
    double safein_optional_io_first_submit_offset_us = 0.0;
    double safein_optional_io_last_submit_offset_us = 0.0;
    double safein_optional_io_queue_to_submit_us = 0.0;
    double safein_optional_io_probe_end_offset_us = 0.0;
    double safein_optional_io_first_submit_to_probe_end_us = 0.0;
    double safein_optional_io_first_completion_offset_us = 0.0;
    std::array<uint32_t, kSafeInOptionalLatencyBucketCount>
        safein_optional_io_submit_to_cqe_histogram{};
    uint32_t safein_payload_buffer_reuses = 0;
    uint64_t safein_payload_prefix_copy_bytes_avoided = 0;
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
    uint32_t mandatory_io_prepared = 0;
    uint64_t mandatory_io_prepared_bytes = 0;
    uint32_t mandatory_io_explicit_submitted = 0;
    uint32_t mandatory_io_submit_calls = 0;
    uint32_t mandatory_io_nonblocking_poll_calls = 0;
    uint32_t mandatory_io_wait_poll_calls = 0;
    uint32_t mandatory_io_empty_polls = 0;
    uint32_t mandatory_io_poll_completions = 0;
    uint32_t mandatory_io_get_events_calls = 0;
    uint32_t mandatory_io_completed = 0;
    uint64_t mandatory_io_completed_bytes = 0;
    uint32_t mandatory_io_completed_before_probe_end = 0;
    uint64_t mandatory_io_completed_bytes_before_probe_end = 0;
    uint32_t mandatory_io_completed_in_final_drain = 0;
    uint64_t mandatory_io_completed_bytes_in_final_drain = 0;
    uint32_t mandatory_io_inflight_at_probe_end = 0;
    uint32_t mandatory_io_prepped_at_probe_end = 0;
    uint32_t mandatory_io_pending_at_probe_end = 0;
    uint32_t final_payload_io_prepared = 0;
    uint64_t final_payload_io_prepared_bytes = 0;
    uint32_t final_payload_io_explicit_submitted = 0;
    uint32_t final_payload_io_submit_calls = 0;
    uint32_t final_payload_io_wait_poll_calls = 0;
    uint32_t final_payload_io_nonblocking_poll_calls = 0;
    uint32_t final_payload_io_completions = 0;
    double probe_classify_ms = 0;
    double probe_submit_ms = 0;
    double probe_submit_prepare_vec_only_ms = 0;
    double probe_submit_prepare_all_ms = 0;
    double probe_submit_emit_ms = 0;
    double probe_submit_vec_only_emit_ms = 0;
    double probe_submit_pending_slot_alloc_ms = 0;
    double probe_submit_prep_read_ms = 0;
    double probe_loop_wall_ms = 0;
    double probe_cluster_wall_ms = 0;
    double probe_loop_scheduler_ms = 0;
    double mandatory_io_first_submit_offset_us = 0;
    double mandatory_io_last_submit_offset_us = 0;
    double mandatory_io_first_completion_offset_us = 0;
    double mandatory_io_probe_end_offset_us = 0;
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
    double safein_optional_io_prepare_ms = 0;
    double safein_optional_io_submit_ms = 0;
    double safein_optional_io_completion_cpu_ms = 0;
    double safein_optional_io_wait_ms = 0;
    double safein_optional_io_nonblocking_poll_ms = 0;
    double safein_optional_io_get_events_ms = 0;
    double safein_optional_io_final_drain_poll_ms = 0;
    double safein_optional_io_drain_ms = 0;
    double mandatory_io_submit_ms = 0;
    double mandatory_io_nonblocking_poll_ms = 0;
    double mandatory_io_get_events_ms = 0;
    double mandatory_io_wait_ms = 0;
    double mandatory_io_completion_cpu_ms = 0;
    double mandatory_io_final_drain_wait_ms = 0;
    double mandatory_io_final_drain_poll_ms = 0;
    double mandatory_io_final_drain_completion_cpu_ms = 0;
    double final_payload_resolve_ms = 0;
    double final_payload_plan_ms = 0;
    double final_payload_sync_prefix_ms = 0;
    double final_payload_submit_ms = 0;
    double final_payload_wait_ms = 0;
    double final_payload_nonblocking_poll_ms = 0;
    double final_payload_completion_cpu_ms = 0;
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
