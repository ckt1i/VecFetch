#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
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
    ~OverlapScheduler();

    /// Execute a single query and return results.
    SearchResults Search(const float* query_vec);

 private:
    friend class ::OverlapSchedulerTest;

    enum class PendingBufferCleanup : uint8_t {
        None,
        Free,
        Pool,
        VecPool,
        FixedVec,
    };

    struct PendingIO {
        enum class Type : uint8_t {
            VEC_ONLY, VEC_ALL, PAYLOAD
        };
        Type type;
        AddressEntry addr;          // VEC_ONLY / VEC_ALL / PAYLOAD
        uint64_t read_offset = 0;
        uint32_t read_length = 0;
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
    void DispatchCompletion(uint64_t slot_token, SearchContext& ctx,
                            class RerankConsumer& reranker);
    const ParsedCluster* GetResidentParsedCluster(uint32_t cluster_id) const;
    void ProbeCluster(const ParsedCluster& pc, uint32_t cluster_id,
                      SearchContext& ctx, class RerankConsumer& reranker);
    void FetchMissingPayloads(SearchContext& ctx,
                              class RerankConsumer& reranker,
                              const std::vector<CollectorEntry>& results);
    SearchResults AssembleResults(class RerankConsumer& reranker,
                                  const std::vector<CollectorEntry>& results);
    uint32_t AllocatePendingSlot(PendingIO io, uint8_t* buffer,
                                 PendingBufferCleanup cleanup,
                                 uint16_t fixed_buffer_index = 0);
    uint32_t AllocateVectorOnlyPendingSlot(AddressEntry addr, uint8_t* buffer,
                                           PendingBufferCleanup cleanup,
                                           uint16_t fixed_buffer_index = 0);
    PendingSlot* GetPendingSlot(uint64_t slot_token);
    void ReleasePendingSlot(uint32_t slot_id);
    void CleanupPendingSlot(PendingSlot& slot);
    void CleanupPendingSlots();
    uint8_t* AcquireVecOnlyBuffer();
    void ReleaseVecOnlyBuffer(uint8_t* buf);
    void ReleaseVectorOnlyPendingSlot(uint32_t slot_id);
    void InitializeDataBufferSlab();
    bool TryAcquireFixedVecBuffer(uint8_t** buffer, uint16_t* buffer_index);
    void ReleaseFixedVecBuffer(uint16_t buffer_index);
    void EmitPendingDataRequests(SearchContext& ctx, uint32_t max_count);
    uint32_t PendingDataRequestCount() const;

    // AsyncIOSink: ProbeResultSink implementation that submits io_uring reads
    // and maintains query-level estimate frontiers. Defined in
    // overlap_scheduler.cpp; declared here so ProbeCluster can instantiate it
    // without exposing internals.
    class AsyncIOSink;

    index::IvfIndex& index_;
    AsyncReader& cluster_reader_;
    AsyncReader& data_reader_;
    bool isolated_submission_mode_ = false;
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
        bool has_truth = false;
        bool is_true_topk = false;
    };

    struct VecOnlyReadPlan {
        AddressEntry addr;
    };

    struct DeferredSafeInPlan {
        AddressEntry addr;
        float rank_key = std::numeric_limits<float>::infinity();
        float safein_upper_bound = std::numeric_limits<float>::infinity();
        bool has_truth = false;
        bool is_true_topk = false;
    };

    struct BudgetedReadPlan {
        float rank_key = std::numeric_limits<float>::infinity();
        ReadPlanEntry plan;

        bool operator<(const BudgetedReadPlan& other) const {
            return rank_key < other.rank_key;
        }
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
    std::vector<VecOnlyReadPlan> pending_vec_only_plans_;
    std::vector<DeferredSafeInPlan> deferred_safein_plans_;
    std::vector<BudgetedReadPlan> budgeted_read_plan_heap_;
    size_t pending_vec_only_head_ = 0;

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
    void FlushDeferredSafeInPlans(SearchContext& ctx, float threshold,
                                  bool force);
    bool UseNonSafeOutCandidateBudget() const;
    void AddBudgetedReadPlan(SearchContext& ctx, const ReadPlanEntry& plan,
                             float rank_key);
    void MaterializeBudgetedReadPlans(SearchContext& ctx);

    // Stage 2 ExRaBitQ re-classification
    float margin_s2_divisor_ = 1.0f;  // 2^(bits-1), precomputed
    bool has_s2_ = false;             // true when bits > 1

    // Phase 3: per-query estimator for PrepareQueryInto, ClusterProber for
    // FastScan classification, and reusable PreparedQuery buffer.
    rabitq::RaBitQEstimator estimator_;  // for PrepareQueryInto in ProbeCluster
    index::ClusterProber prober_;
};

}  // namespace query
}  // namespace vdb
