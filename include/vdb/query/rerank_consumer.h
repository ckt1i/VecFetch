#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/query/buffer_pool.h"
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
        const float* vec = nullptr;
        uint64_t candidate_order = 0;
    };

    /// @param ctx   SearchContext (provides query_vec and collector)
    /// @param raw_dim   Raw-vector dimensionality used by exact rerank
    RerankConsumer(
        SearchContext& ctx, Dim raw_dim,
        const std::unordered_map<uint64_t, uint64_t>* candidate_order = nullptr);
    ~RerankConsumer();

    /// Consume a VEC_ONLY buffer: copy the vector for later batch rerank.
    /// The caller keeps ownership of buf.
    void ConsumeVec(uint8_t* buf, AddressEntry addr);

    /// Consume an already owned raw-vector buffer without copying it again.
    void ConsumeOwnedVec(AlignedBufPtr buf, AddressEntry addr);

    /// Consume an ALL buffer: buffer vector for later batch rerank and cache
    /// the payload bytes covered by the read.
    /// Ownership of buf remains with caller; payload bytes may be copied into cache.
    void ConsumeAll(uint8_t* buf, AddressEntry addr, uint32_t record_bytes_read);

    /// Consume a SafeIn payload-prefix buffer from a separated payload store.
    /// When pool_owner is non-null, the buffer is returned to that pool when
    /// the cache entry is released; otherwise it is freed normally.
    void ConsumePayloadPrefix(uint8_t* buf, AddressEntry addr,
                              uint32_t prefix_len,
                              BufferPool* pool_owner = nullptr,
                              uint32_t payload_capacity = 0);

    /// Consume a PAYLOAD buffer: transfers ownership to the payload cache.
    /// When pool_owner is non-null, the buffer is returned to that pool when
    /// the cache entry is released; otherwise it is freed normally.
    void ConsumePayload(uint8_t* buf, AddressEntry addr,
                        uint32_t payload_len = 0,
                        BufferPool* pool_owner = nullptr);

    /// Cache a non-owning payload view backed by scheduler-owned query-local
    /// storage. Returns false when a payload entry already exists.
    bool CachePayloadView(uint64_t offset, const uint8_t* payload,
                          uint32_t payload_len);

    /// Check if any payload bytes are cached for this address offset.
    bool HasPayload(uint64_t offset) const;

    /// Number of contiguous payload bytes cached from payload offset 0.
    uint32_t CachedPayloadBytes(uint64_t offset) const;

    /// Pointer to cached payload prefix/full bytes, or nullptr if absent.
    const uint8_t* CachedPayloadData(uint64_t offset) const;

    /// Mutable access used to append a missing suffix directly into a
    /// prefetched final-sized buffer.
    uint8_t* MutableCachedPayloadData(uint64_t offset);

    /// Allocated payload capacity for an existing cache entry.
    uint32_t CachedPayloadCapacity(uint64_t offset) const;

    /// Whether the cached payload is a zero-copy view into a vector span.
    bool CachedPayloadIsSpanView(uint64_t offset) const;

    /// Mark more contiguous payload bytes as ready after an in-place suffix
    /// read. Returns false if the cache entry is missing or too small.
    bool ExtendCachedPayload(uint64_t offset, uint32_t ready_bytes);

    /// Take ownership of a cached payload buffer (removes from cache).
    /// Returns nullptr if not found.  Buffer was allocated via aligned_alloc.
    AlignedBufPtr TakePayload(uint64_t offset);

    /// Release a cached payload after it has been parsed. Pool-backed buffers
    /// are returned to their pool instead of being passed to free().
    void ReleasePayload(uint64_t offset);

    /// Cache a payload buffer (takes ownership).
    void CachePayload(uint64_t offset, AlignedBufPtr buf);

    /// Remove cached payloads not in the final TopK.
    /// Call after Finalize to free memory for entries that didn't make it.
    void CleanupUnusedCache(const std::vector<CollectorEntry>& final_results);

    /// Execute a single batch rerank over all buffered candidates.
    void ExecuteBuffered();

    uint32_t BufferedCount() const;

 private:
    struct VectorChunk {
        AlignedBufPtr storage;
        uint32_t capacity = 0;
        uint32_t used = 0;
    };

    const float* AllocateVectorCopy(const uint8_t* src);
    uint64_t CandidateOrder(AddressEntry addr) const;
    bool GrowVectorChunk(uint32_t min_capacity);
    void ResetVectorChunks();

    SearchContext& ctx_;
    Dim dim_;
    uint32_t vec_bytes_;  // raw_dim * sizeof(float)
    uint32_t aligned_vec_bytes_;
    const std::unordered_map<uint64_t, uint64_t>* candidate_order_ = nullptr;
    std::vector<BufferedCandidate> buffered_candidates_;
    std::vector<VectorChunk> vector_chunks_;
    std::vector<AlignedBufPtr> owned_vector_buffers_;

    struct PayloadCacheEntry {
        uint8_t* storage = nullptr;
        uint32_t bytes = 0;
        uint32_t capacity = 0;
        BufferPool* pool_owner = nullptr;
        bool span_view = false;
    };

    void StorePayload(uint64_t offset, uint8_t* storage, uint32_t bytes,
                      uint32_t capacity, BufferPool* pool_owner);
    static void ReleasePayloadEntry(PayloadCacheEntry* entry);
    void ClearPayloadCache();

    // Payload cache: addr.offset -> contiguous payload prefix/full buffer.
    std::unordered_map<uint64_t, PayloadCacheEntry> payload_cache_;
};

}  // namespace query
}  // namespace vdb
