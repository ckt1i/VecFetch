#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/cluster_prober.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/buffer_pool.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/query/search_context.h"
#include "vdb/query/search_results.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/storage/hot_record.h"

class OverlapSchedulerTest;

namespace vdb {
namespace query {

/// Single-threaded resident-cluster query scheduler.
///
/// Pipeline:
///   ProbeResidentClusters (resident cluster probe + raw-data submit) →
///   FinalDrain → FetchMissingPayloads → AssembleResults.
class OverlapScheduler {
 public:
    OverlapScheduler(index::IvfIndex& index, AsyncReader& reader,
                     const SearchConfig& config);
    OverlapScheduler(index::IvfIndex& index, AsyncReader& cluster_reader,
                     AsyncReader& data_reader, const SearchConfig& config);
    OverlapScheduler(index::IvfIndex& index, AsyncReader& cluster_reader,
                     AsyncReader& data_reader,
                     AsyncReader& optional_payload_reader,
                     const SearchConfig& config);
    ~OverlapScheduler();

    /// Execute a single query and return results.
    SearchResults Search(const float* query_vec);

    bool fixed_vec_buffers_enabled() const {
        return fixed_vec_buffers_enabled_;
    }

    /// Update query-local truth rows used only by SafeIn/SafeOut false-stat
    /// accounting. This does not change candidate generation or ranking.
    void SetFalseStatsTrueTopKRows(const std::unordered_set<uint32_t>* rows);
    void SetOracleTrueTopKRows(const std::unordered_set<uint32_t>* rows);

    /// Label vector-read trace records emitted by the next Search call.
    void SetVectorReadTraceQueryIndex(uint32_t query_index);

 private:
    friend class ::OverlapSchedulerTest;

    struct ReadPlanEntry;

    enum class PendingBufferCleanup : uint8_t {
        None,
        Free,
        Pool,
        VecPool,
        FixedVec,
    };

    struct VecSpanMember {
        AddressEntry addr;
        uint32_t buffer_offset = 0;
        uint32_t safein_credit_bytes = 0;
    };

    struct PendingIO {
        enum class Type : uint8_t {
            VEC_ONLY, VEC_ALL, PAYLOAD, PAYLOAD_PREFIX,
            PAYLOAD_DESCRIPTOR, VEC_SPAN
        };
        Type type;
        AddressEntry addr;          // VEC_ONLY / VEC_ALL / PAYLOAD
        uint64_t read_offset = 0;
        uint32_t read_length = 0;
        uint32_t payload_total_length = 0;
        uint32_t payload_prefix_length = 0;
        uint32_t payload_buffer_capacity = 0;
        bool payload_reuses_cache = false;
        bool optional_payload_io = false;
        uint64_t optional_submit_timestamp_ns = 0;
        uint64_t bextra_extra_bytes = 0;
        size_t bextra_trace_index = std::numeric_limits<size_t>::max();
        std::vector<VecSpanMember> span_members;
    };

    struct PendingSlot {
        bool in_use = false;
        uint8_t* buffer = nullptr;
        uint16_t fixed_buffer_index = 0;
        PendingBufferCleanup cleanup = PendingBufferCleanup::None;
        PendingIO io;
    };

    void ProbeResidentClusters(SearchContext& ctx,
                               class RerankConsumer& reranker,
                               const std::vector<ClusterID>& sorted_clusters);
    void FinalDrain(SearchContext& ctx, class RerankConsumer& reranker);
    void DrainOptionalPayloads(SearchContext& ctx,
                               class RerankConsumer& reranker);
    void DispatchCompletion(uint64_t slot_token, int32_t result,
                            SearchContext& ctx,
                            class RerankConsumer& reranker);
    const ParsedCluster* GetResidentParsedCluster(uint32_t cluster_id) const;
    void ProbeCluster(const ParsedCluster& pc, uint32_t cluster_id,
                      SearchContext& ctx, class RerankConsumer& reranker);
    void ExecuteSerialDataReads(SearchContext& ctx,
                                class RerankConsumer& reranker);
    void FetchMissingPayloads(SearchContext& ctx,
                              class RerankConsumer& reranker,
                              const std::vector<CollectorEntry>& results);
    void FetchMissingPayloadsSerial(SearchContext& ctx,
                                    class RerankConsumer& reranker,
                                    const std::vector<CollectorEntry>& results);
    void ResolvePayloadLocations(
        SearchContext& ctx, class RerankConsumer& reranker,
        const std::vector<CollectorEntry>& results);
    SearchResults AssembleResults(SearchContext& ctx,
                                  class RerankConsumer& reranker,
                                  const std::vector<CollectorEntry>& results);
    uint32_t AllocatePendingSlot(PendingIO io, uint8_t* buffer,
                                 PendingBufferCleanup cleanup,
                                 uint16_t fixed_buffer_index = 0);
    uint32_t AllocateVectorOnlyPendingSlot(
        AddressEntry addr, uint8_t* buffer, PendingBufferCleanup cleanup,
        uint16_t fixed_buffer_index = 0,
        PendingIO::Type type = PendingIO::Type::VEC_ONLY);
    PendingSlot* GetPendingSlot(uint64_t slot_token);
    void ReleasePendingSlot(uint32_t slot_id);
    void CleanupPendingSlot(PendingSlot& slot);
    void CleanupPendingSlots();
    void ReleaseRetainedVecSpans();
    void InstallFinalSpanPayloadViews(
        SearchContext& ctx, class RerankConsumer& reranker,
        const std::vector<CollectorEntry>& results);
    uint8_t* AcquireVecOnlyBuffer();
    void ReleaseVecOnlyBuffer(uint8_t* buf);
    void ReleaseVectorOnlyPendingSlot(uint32_t slot_id);
    void InitializeDataBufferSlab();
    bool TryAcquireFixedVecBuffer(uint8_t** buffer, uint16_t* buffer_index);
    void ReleaseFixedVecBuffer(uint16_t buffer_index);
    void EmitPendingDataRequests(SearchContext& ctx, uint32_t max_count);
    void EmitPendingOptionalPayloadRequests(SearchContext& ctx,
                                            uint32_t max_count);
    void MaybeSubmitOptionalPayloadRequests(
        SearchContext& ctx, class RerankConsumer& reranker,
        uint32_t probes_remaining);
    void PollOptionalPayloadCompletions(SearchContext& ctx,
                                        class RerankConsumer& reranker,
                                        bool wait_for_one);
    uint32_t PrepareSafeInPayloadRead(SearchContext& ctx,
                                      const ReadPlanEntry& plan,
                                      AsyncReader& reader,
                                      bool optional_payload_io);
    void SnapshotOptionalPayloadProbeEnd(SearchContext& ctx);
    uint32_t PendingDataRequestCount() const;
    uint32_t PendingMandatoryDataRequestCount() const;
    uint32_t PendingOptionalPayloadRequestCount() const;
    void ApplyBextraWindowBudget(SearchContext& ctx,
                                 uint32_t clusters_processed,
                                 uint32_t clusters_remaining);
    void RecordBextraCompletion(const PendingIO& io, SearchContext& ctx);

    // AsyncIOSink: ProbeResultSink implementation that submits io_uring reads
    // and maintains query-level estimate frontiers. Defined in
    // overlap_scheduler.cpp; declared here so ProbeCluster can instantiate it
    // without exposing internals.
    class AsyncIOSink;

    index::IvfIndex& index_;
    AsyncReader& cluster_reader_;
    AsyncReader& data_reader_;
    AsyncReader& optional_payload_reader_;
    bool isolated_submission_mode_ = false;
    bool isolated_optional_payload_io_ = false;
    SearchConfig config_;
    BufferPool buffer_pool_;
    std::vector<PendingSlot> pending_slots_;
    std::vector<uint32_t> free_pending_slots_;
    std::vector<uint8_t*> fixed_vec_buffers_;
    std::vector<uint32_t> fixed_vec_buffer_capacities_;
    std::vector<uint16_t> free_fixed_vec_buffers_;
    std::vector<uint8_t*> vec_only_owned_buffers_;
    std::vector<uint8_t*> free_vec_only_buffers_;
    IoUringReader* fixed_buffer_reader_ = nullptr;
    bool fixed_vec_buffers_enabled_ = false;

    struct ResidentScratch {
        std::vector<IoCompletion> completions;
    };
    ResidentScratch resident_scratch_;

    struct QueryWrapper {
        rabitq::PreparedQuery prepared;
        rabitq::ClusterPreparedScratch scratch;
        std::vector<float> rotated_q;
        std::vector<float> padded_q;
    };

    struct QueryDedupSet {
        static constexpr uint64_t kEmpty = std::numeric_limits<uint64_t>::max();

        void Reserve(size_t expected);
        void Clear();
        bool Insert(uint64_t key);

        std::vector<uint64_t> slots;
        size_t mask = 0;
        size_t size = 0;
    };

    struct SubmitScratch {
        static constexpr uint32_t kMax = index::CandidateBatch::kMaxCandidates;

        uint32_t unique_count = 0;
        uint32_t safein_all_count = 0;
        uint32_t vec_only_count = 0;

        uint32_t unique_indices[kMax] = {};
        uint32_t safein_all_indices[kMax] = {};
        uint32_t vec_only_indices[kMax] = {};
        uint32_t slot_ids[kMax] = {};
        uint16_t fixed_buffer_indices[kMax] = {};
        uint8_t* buffers[kMax] = {};
        bool uses_fixed_buffer[kMax] = {};
    };

    struct ReadPlanEntry {
        PendingIO::Type type = PendingIO::Type::VEC_ONLY;
        AddressEntry addr;
        uint32_t read_length = 0;
        int payload_prefix_fd = -1;
        uint64_t cold_payload_offset = 0;
        uint32_t cold_payload_total_length = 0;
        uint32_t cold_payload_prefix_length = 0;
        bool has_truth = false;
        bool is_true_topk = false;
        uint64_t bextra_extra_bytes = 0;
        size_t bextra_trace_index = std::numeric_limits<size_t>::max();
    };

    struct VecOnlyReadPlan {
        AddressEntry addr;
        bool safein = false;
        uint32_t safein_credit_bytes = 0;
    };

    struct VecSpanExecutionGroup {
        size_t begin = 0;
        size_t end = 0;
        uint64_t read_offset = 0;
        uint32_t read_length = 0;
        uint64_t safein_credit_bytes = 0;
    };

    uint32_t ResolveVecSpanSafeInCreditBytes(AddressEntry addr,
                                             bool safein) const;
    VecOnlyReadPlan MakeVecOnlyReadPlan(AddressEntry addr,
                                        bool safein) const;
    void PlanVecOnlySpanGroups(SearchContext& ctx,
                               std::vector<VecOnlyReadPlan>& plans,
                               size_t begin, size_t end,
                               std::vector<VecSpanExecutionGroup>* groups);

    struct PayloadLocation {
        uint32_t length = 0;
        uint64_t offset = 0;
        int fd = -1;
        bool from_inline_hot_record = false;
        bool from_cold_payload = false;
        uint32_t inline_prefix_length = 0;
        uint64_t inline_prefix_offset = 0;
        int inline_prefix_fd = -1;
    };

    struct DeferredSafeInPlan {
        AddressEntry addr;
        float rank_key = std::numeric_limits<float>::infinity();
        float safein_upper_bound = std::numeric_limits<float>::infinity();
        float confidence = -std::numeric_limits<float>::infinity();
        uint32_t read_length = 0;
        uint32_t extra_bytes = 0;
        bool has_truth = false;
        bool is_true_topk = false;
    };

    using PreparedClusterQueryView = rabitq::PreparedClusterQueryView;
    PreparedClusterQueryView PrepareClusterQueryView(const SearchContext& ctx,
                                                     uint32_t cluster_id,
                                                     rabitq::PrepareTimingBreakdown* timing = nullptr);
    QueryWrapper query_wrapper_;

    // Query-local state (reset per Search() call)
    QueryDedupSet submitted_candidate_offsets_;
    SubmitScratch submit_scratch_;
    std::deque<ReadPlanEntry> pending_all_plans_;
    std::deque<ReadPlanEntry> pending_optional_payload_plans_;
    std::deque<uint32_t> optional_payload_prepped_slots_;
    std::vector<VecOnlyReadPlan> pending_vec_only_plans_;
    std::vector<DeferredSafeInPlan> deferred_safein_plans_;
    size_t pending_vec_only_head_ = 0;
    std::unordered_map<uint64_t, uint64_t> candidate_order_by_offset_;
    std::unordered_map<uint64_t, PayloadLocation> payload_location_cache_;
    std::unordered_map<uint64_t, uint32_t> cold_prefetched_bytes_by_offset_;
    struct RetainedVecSpan {
        uint8_t* buffer = nullptr;
        uint32_t bytes = 0;
        uint32_t used = 0;
        bool compact = false;
        bool keep_for_final_payload = false;
    };
    struct SpanPayloadRef {
        AddressEntry addr;
        const uint8_t* payload = nullptr;
        uint32_t bytes = 0;
        uint32_t span_index = 0;
    };
    std::vector<RetainedVecSpan> retained_vec_spans_;
    std::vector<SpanPayloadRef> span_payload_refs_;
    std::vector<uint64_t> final_payload_offset_table_scratch_;
    std::vector<std::vector<VecSpanMember>> free_vec_span_member_vectors_;
    SpanPlannerScratch vec_span_planner_scratch_;
    std::vector<SpanPlannerItem> vec_span_planner_items_;
    std::vector<SpanPlannerGroup> vec_span_planner_groups_;
    std::vector<VecSpanExecutionGroup> vec_span_execution_groups_;
    uint32_t vec_span_safein_tails_extended_ = 0;
    uint64_t next_candidate_order_ = 0;
    uint64_t optional_probe_start_ns_ = 0;
    uint32_t optional_probe_total_clusters_ = 0;
    uint64_t pipeline_probe_start_ns_ = 0;

    uint32_t vec_bytes_;
    uint32_t aligned_vec_bytes_ = 0;
    int data_fd_registered_index_ = -1;

    struct EstimateHeapEntry {
        float distance = 0.0f;
        float error_bound = 0.0f;
        float lower_bound = 0.0f;

        bool operator<(const EstimateHeapEntry& other) const {
            return distance < other.distance;
        }
    };

    // Dynamic SafeOut state (reset per Search() call). This heap is ordered by
    // candidate upper bound U = d_hat + e, so its top is kth_smallest(U).
    struct SafeOutFrontierEntry {
        float distance = 0.0f;
        float error_bound = 0.0f;
        float upper_bound = 0.0f;

        bool operator<(const SafeOutFrontierEntry& other) const {
            return upper_bound < other.upper_bound;
        }
    };

    struct SafeInFrontierEntry {
        float lower_bound = 0.0f;

        bool operator<(const SafeInFrontierEntry& other) const {
            return lower_bound < other.lower_bound;
        }
    };

    std::vector<SafeOutFrontierEntry> safeout_frontier_heap_;
    std::vector<SafeInFrontierEntry> safein_frontier_heap_;
    uint32_t est_top_k_ = 0;
    bool use_dynamic_safeout_ = true;
    bool use_dynamic_safein_ = false;
    bool use_estimate_frontier_ = true;

    uint32_t dynamic_safein_probes_seen_ = 0;
    uint32_t dynamic_safein_stable_count_ = 0;
    bool dynamic_safein_ready_ = false;
    bool dynamic_safein_small_pool_extra_defer_ = false;
    float dynamic_safein_last_frontier_ = std::numeric_limits<float>::infinity();
    float dynamic_safein_current_frontier_ = std::numeric_limits<float>::infinity();
    float dynamic_safein_current_threshold_ = std::numeric_limits<float>::infinity();
    uint32_t safein_prefetch_scheduled_count_ = 0;
    uint64_t safein_prefetch_scheduled_bytes_ = 0;
    uint64_t safein_prefetch_scheduled_extra_bytes_ = 0;

    bool AddSafeOutFrontierEstimate(const EstimateHeapEntry& estimate);
    bool AddSafeInFrontierEstimate(float lower_bound);
    float SafeOutFrontierUpper() const;
    float SafeInFrontierLower() const;
    float DynamicSafeInReferenceFrontier() const;
    void UpdateDynamicSafeInState(SearchContext& ctx, bool advance_probe);
    float SafeInThresholdForProbe() const;
    void RecordDynamicSafeInStats(SearchContext& ctx, float threshold,
                                  float frontier);
    bool ShouldDeferSafeInPlans() const;
    bool ShouldHoldDeferredSafeInPlans();
    void RecordSafeInPrefetchDecision(SearchContext& ctx,
                                      bool has_truth,
                                      bool is_true_topk) const;
    uint32_t SafeInReadLength(AddressEntry addr) const;
    void ConfigureSafeInReadPlan(SearchContext& ctx, AddressEntry addr,
                                 ReadPlanEntry* plan) const;
    uint64_t SafeInPlanTotalReadBytes(const ReadPlanEntry& plan) const;
    uint64_t SafeInPlanExtraBytes(const ReadPlanEntry& plan) const;
    bool UseDecoupledSafeInPrefetch() const;
    bool UseIsolatedOptionalPayloadIO() const;
    void EnqueueSafeInReadPlan(SearchContext& ctx,
                               const ReadPlanEntry& plan);
    bool HasSafeInPrefetchCountCapacity() const;
    bool ShouldScheduleSafeInPrefetch(SearchContext& ctx,
                                      uint64_t read_bytes,
                                      uint64_t extra_bytes = 0);
    void FinalizeColdPrefetchStats(
        SearchContext& ctx, const std::vector<CollectorEntry>& results) const;
    void FlushDeferredSafeInPlans(SearchContext& ctx, float threshold,
                                  bool force);
    bool UseSeparateRecordStore() const;
    bool UseVectorSidecarStore() const;
    bool UseInlineHotRecordStore() const;
    const SeparateRecordLocation& LookupSeparateRecordOrAbort(
        SearchContext& ctx, AddressEntry addr) const;
    uint64_t SeparateVectorOffset(const SeparateRecordLocation& loc) const;
    void RecordVectorReadTrace(AddressEntry addr, bool from_vec_all);
    void MarkVectorReadTraceSelectedTopK(
        const std::vector<CollectorEntry>& results);
    void MarkSafeInConfidenceTraceSelectedTopK(
        const std::vector<CollectorEntry>& results);
    storage::HotPayloadDescriptor ReadInlinePayloadDescriptorOrAbort(
        SearchContext& ctx, AddressEntry addr) const;
    PayloadLocation PayloadLocationFromDescriptorOrAbort(
        SearchContext& ctx, AddressEntry addr,
        const storage::HotPayloadDescriptor& desc) const;
    void CacheInlinePayloadLocationFromRecord(
        SearchContext& ctx, AddressEntry addr, const uint8_t* record,
        uint32_t record_bytes);
    PayloadLocation LocatePayloadOrAbort(SearchContext& ctx,
                                         AddressEntry addr);

    // Stage 2 ExRaBitQ re-classification
    float margin_s2_divisor_ = 1.0f;  // 2^(total_bits-1), precomputed
    bool has_s2_ = false;             // true when bits > 1

    uint32_t vector_read_trace_query_index_ = 0;
    uint32_t vector_read_trace_request_index_ = 0;
    uint32_t vector_read_trace_flush_index_ = 0;
    uint32_t vector_read_trace_current_flush_index_ = 0;
    std::unordered_map<uint64_t, uint32_t>
        vector_read_trace_cluster_by_offset_;
    size_t safein_confidence_trace_query_start_ = 0;
    double bextra_probe_cluster_ema_ms_ = 0.0;
    uint64_t bextra_inflight_bytes_ = 0;
    uint32_t bextra_window_index_ = 0;
    bool bextra_before_final_drain_ = true;

    // Phase 3: per-query estimator for PrepareQueryInto, ClusterProber for
    // FastScan classification, and reusable PreparedQuery buffer.
    rabitq::RaBitQEstimator estimator_;  // for PrepareQueryInto in ProbeCluster
    index::ClusterProber prober_;
};

}  // namespace query
}  // namespace vdb
