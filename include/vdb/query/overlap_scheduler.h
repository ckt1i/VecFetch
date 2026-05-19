#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
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

namespace vdb {
namespace query {

/// Single-threaded query scheduler with async cluster prefetch.
///
/// Pipeline:
///   PrefetchClusters (sliding window io_uring batch) →
///   ProbeAndDrainInterleaved (unified event loop: probe CPU + rerank CQE) →
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
    enum class PendingBufferCleanup : uint8_t {
        None,
        Free,
        Pool,
        FixedVec,
    };

    struct PendingIO {
        enum class Type : uint8_t {
            CLUSTER_BLOCK, VEC_ONLY, VEC_ALL, PAYLOAD
        };
        Type type;
        uint32_t cluster_id = 0;   // CLUSTER_BLOCK
        uint32_t block_size = 0;   // CLUSTER_BLOCK
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

    void PrefetchClusters(SearchContext& ctx,
                          const std::vector<ClusterID>& sorted_clusters);
    void ProbeAndDrainInterleaved(SearchContext& ctx,
                                   class RerankConsumer& reranker,
                                   const std::vector<ClusterID>& sorted_clusters);
    void ProbeResidentThinPath(SearchContext& ctx,
                               class RerankConsumer& reranker,
                               const std::vector<ClusterID>& sorted_clusters);
    void FinalDrain(SearchContext& ctx, class RerankConsumer& reranker);
    void DispatchCompletion(uint64_t slot_token, SearchContext& ctx,
                            class RerankConsumer& reranker);
    void SubmitClusterRead(uint32_t cluster_id);
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
    PendingSlot* GetPendingSlot(uint64_t slot_token);
    void ReleasePendingSlot(uint32_t slot_id);
    void CleanupPendingSlot(PendingSlot& slot);
    void CleanupPendingSlots();
    void InitializeDataBufferSlab();
    bool TryAcquireFixedVecBuffer(uint8_t** buffer, uint16_t* buffer_index);
    void ReleaseFixedVecBuffer(uint16_t buffer_index);
    void EmitPendingDataRequests(SearchContext& ctx, uint32_t max_count);
    uint32_t PendingDataRequestCount() const;

    // AsyncIOSink: ProbeResultSink implementation that submits io_uring reads
    // and maintains est_heap_. Defined in overlap_scheduler.cpp; declared here
    // so ProbeCluster can instantiate it without exposing internals.
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
    };

    using PreparedClusterQueryView = rabitq::PreparedClusterQueryView;
    PreparedClusterQueryView PrepareClusterQueryView(const SearchContext& ctx,
                                                     uint32_t cluster_id,
                                                     rabitq::PrepareTimingBreakdown* timing = nullptr);
    QueryWrapper query_wrapper_;

    // Sliding window state (reset per Search() call)
    std::unordered_map<uint32_t, ParsedCluster> ready_clusters_;
    QueryDedupSet submitted_candidate_offsets_;
    SubmitScratch submit_scratch_;
    std::deque<ReadPlanEntry> pending_all_plans_;
    std::deque<ReadPlanEntry> pending_vec_only_plans_;
    uint32_t next_to_submit_ = 0;
    uint32_t inflight_clusters_ = 0;

    uint32_t vec_bytes_;

    // CRC early stop state (reset per Search() call)
    index::CrcStopper crc_stopper_;
    std::vector<std::pair<float, uint32_t>> est_heap_;
    uint32_t est_top_k_ = 0;
    bool use_crc_ = false;

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
