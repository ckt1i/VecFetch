#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <unordered_map>

#include "vdb/common/types.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/query/result_collector.h"
#include "vdb/query/search_context.h"

namespace vdb {
namespace query {

/// Consumes I/O completion buffers and buffers raw vectors for batch L2 reranking.
///
/// Three consumption modes based on ReadTaskType:
///   - ConsumeVec:     VEC_ONLY read → buffer vector for later batch rerank.
///   - ConsumeAll:     ALL read → buffer vector for later batch rerank and
///                     eagerly cache payload bytes.
///   - ConsumePayload: PAYLOAD read → transfer buf ownership to cache.
///
/// Payload cache is keyed by AddressEntry.offset (unique per record).
class RerankConsumer {
 public:
    struct BufferedCandidate {
        AddressEntry addr;
        AlignedBufPtr vec_buf;
    };

    /// @param ctx   SearchContext (provides query_vec and collector)
    /// @param raw_dim   Raw-vector dimensionality used by exact rerank
    RerankConsumer(SearchContext& ctx, Dim raw_dim);
    ~RerankConsumer();

    /// Consume a VEC_ONLY buffer: buffer vector for later batch rerank.
    /// Ownership of buf transfers to the consumer.
    void ConsumeVec(uint8_t* buf, AddressEntry addr);

    /// Consume an ALL buffer: buffer vector for later batch rerank and cache payload.
    /// Ownership of buf remains with caller; payload bytes may be copied into cache.
    void ConsumeAll(uint8_t* buf, AddressEntry addr);

    /// Consume a PAYLOAD buffer: transfers ownership to the payload cache.
    /// Caller must NOT free buf after this call.
    void ConsumePayload(uint8_t* buf, AddressEntry addr);

    /// Check if payload is cached for this address offset.
    bool HasPayload(uint64_t offset) const;

    /// Take ownership of a cached payload buffer (removes from cache).
    /// Returns nullptr if not found.  Buffer was allocated via aligned_alloc.
    AlignedBufPtr TakePayload(uint64_t offset);

    /// Cache a payload buffer (takes ownership).
    void CachePayload(uint64_t offset, AlignedBufPtr buf);

    /// Remove cached payloads not in the final TopK.
    /// Call after Finalize to free memory for entries that didn't make it.
    void CleanupUnusedCache(const std::vector<CollectorEntry>& final_results);

    /// Execute a single batch rerank over all buffered candidates.
    void ExecuteBuffered();

    uint32_t BufferedCount() const;

 private:
    SearchContext& ctx_;
    Dim dim_;
    uint32_t vec_bytes_;  // raw_dim * sizeof(float)
    std::vector<BufferedCandidate> buffered_candidates_;

    // Payload cache: addr.offset → owned payload buffer (freed via free())
    std::unordered_map<uint64_t, AlignedBufPtr> payload_cache_;
};

}  // namespace query
}  // namespace vdb
