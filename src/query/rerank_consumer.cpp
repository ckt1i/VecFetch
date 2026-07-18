#include "vdb/query/rerank_consumer.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <utility>

#include "vdb/common/distance.h"
#include "vdb/simd/rerank_distance.h"
#include "vdb/storage/hot_record.h"

namespace vdb {
namespace query {

RerankConsumer::RerankConsumer(
    SearchContext& ctx, Dim raw_dim,
    const std::unordered_map<uint64_t, uint64_t>* candidate_order)
    : ctx_(ctx),
      dim_(raw_dim),
      vec_bytes_(raw_dim * sizeof(float)),
      aligned_vec_bytes_((vec_bytes_ + 4095u) & ~4095u),
      candidate_order_(candidate_order) {
    buffered_candidates_.reserve(1024);
    GrowVectorChunk(aligned_vec_bytes_ * 1024u);
}

uint64_t RerankConsumer::CandidateOrder(AddressEntry addr) const {
    if (candidate_order_ == nullptr) return addr.offset;
    const auto found = candidate_order_->find(addr.offset);
    return found == candidate_order_->end() ? addr.offset : found->second;
}

RerankConsumer::~RerankConsumer() {
    ClearPayloadCache();
}

bool RerankConsumer::GrowVectorChunk(uint32_t min_capacity) {
    const uint32_t chunk_capacity = std::max(aligned_vec_bytes_ * 1024u, min_capacity);
    const bool detailed_timing = ctx_.config().enable_hotpath_detailed_timing;
    const auto alloc_start = detailed_timing ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
    uint8_t* raw = static_cast<uint8_t*>(
        std::aligned_alloc(4096, chunk_capacity));
    if (detailed_timing) {
        ctx_.stats().rerank_vec_alloc_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - alloc_start).count();
    }
    if (raw == nullptr) {
        return false;
    }
    vector_chunks_.push_back(
        VectorChunk{AlignedBufPtr(raw), chunk_capacity, 0});
    return true;
}

const float* RerankConsumer::AllocateVectorCopy(const uint8_t* src) {
    if (vector_chunks_.empty() ||
        vector_chunks_.back().used + aligned_vec_bytes_ > vector_chunks_.back().capacity) {
        if (!GrowVectorChunk(aligned_vec_bytes_)) {
            std::fprintf(stderr, "FATAL: aligned_alloc failed for rerank vector slab (%u bytes)\n",
                         aligned_vec_bytes_);
            std::abort();
        }
    }
    VectorChunk& chunk = vector_chunks_.back();
    uint8_t* dst = chunk.storage.get() + chunk.used;
    const bool detailed_timing = ctx_.config().enable_hotpath_detailed_timing;
    const auto copy_start = detailed_timing ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    std::memcpy(dst, src, vec_bytes_);
    if (detailed_timing) {
        ctx_.stats().rerank_vec_copy_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - copy_start).count();
    }
    chunk.used += aligned_vec_bytes_;
    return reinterpret_cast<const float*>(dst);
}

void RerankConsumer::ResetVectorChunks() {
    for (auto& chunk : vector_chunks_) {
        chunk.used = 0;
    }
}

void RerankConsumer::ConsumeVec(uint8_t* buf, AddressEntry addr) {
    buffered_candidates_.push_back(
        BufferedCandidate{addr, AllocateVectorCopy(buf), CandidateOrder(addr)});
    ctx_.stats().buffered_candidates++;
}

void RerankConsumer::ConsumeOwnedVec(AlignedBufPtr buf, AddressEntry addr) {
    const float* vec = reinterpret_cast<const float*>(buf.get());
    owned_vector_buffers_.push_back(std::move(buf));
    buffered_candidates_.push_back(
        BufferedCandidate{addr, vec, CandidateOrder(addr)});
    ctx_.stats().buffered_candidates++;
}

void RerankConsumer::ConsumeAll(uint8_t* buf, AddressEntry addr,
                                uint32_t record_bytes_read) {
    buffered_candidates_.push_back(
        BufferedCandidate{addr, AllocateVectorCopy(buf), CandidateOrder(addr)});
    ctx_.stats().buffered_candidates++;

    if (ctx_.config().inline_hot_record_store.enabled) {
        const uint32_t desc_bytes =
            ctx_.config().inline_hot_record_store.descriptor_bytes;
        if (desc_bytes != sizeof(storage::HotPayloadDescriptor) ||
            record_bytes_read < vec_bytes_ + desc_bytes) {
            ctx_.stats().inline_descriptor_errors++;
            return;
        }
        const auto desc = storage::DecodeHotPayloadDescriptor(buf + vec_bytes_);
        Status s = storage::ValidateHotPayloadDescriptor(desc);
        if (!s.ok()) {
            ctx_.stats().inline_descriptor_errors++;
            return;
        }
        if (desc.payload_storage_type ==
            static_cast<uint8_t>(storage::HotPayloadStorageType::kColdPointer)) {
            ctx_.stats().inline_cold_payload_deferred++;
            return;
        }
        if (desc.inline_bytes == 0) {
            return;
        }
        const uint32_t payload_offset = vec_bytes_ + desc_bytes;
        if (record_bytes_read < payload_offset + desc.inline_bytes) {
            ctx_.stats().inline_descriptor_errors++;
            return;
        }
        const uint32_t alloc_len = (desc.inline_bytes + 4095u) & ~4095u;
        uint8_t* pbuf =
            static_cast<uint8_t*>(std::aligned_alloc(4096, alloc_len));
        if (pbuf == nullptr) {
            std::fprintf(stderr,
                         "FATAL: aligned_alloc failed for inline payload (%u bytes)\n",
                         alloc_len);
            std::abort();
        }
        std::memcpy(pbuf, buf + payload_offset, desc.inline_bytes);
        StorePayload(addr.offset, pbuf, desc.inline_bytes,
                     desc.inline_bytes, nullptr);
        ctx_.stats().total_safein_payload_prefetched++;
        return;
    }

    const uint32_t covered = std::min(record_bytes_read, addr.size);
    if (covered > vec_bytes_) {
        const uint32_t payload_prefix_len = covered - vec_bytes_;
        const uint32_t alloc_len = (payload_prefix_len + 4095u) & ~4095u;
        uint8_t* pbuf =
            static_cast<uint8_t*>(std::aligned_alloc(4096, alloc_len));
        if (pbuf == nullptr) {
            std::fprintf(stderr,
                         "FATAL: aligned_alloc failed for payload prefix (%u bytes)\n",
                         alloc_len);
            std::abort();
        }
        std::memcpy(pbuf, buf + vec_bytes_, payload_prefix_len);
        StorePayload(addr.offset, pbuf, payload_prefix_len,
                     payload_prefix_len, nullptr);
        ctx_.stats().total_safein_payload_prefetched++;
    }
}

void RerankConsumer::ConsumePayloadPrefix(uint8_t* buf, AddressEntry addr,
                                          uint32_t prefix_len,
                                          BufferPool* pool_owner,
                                          uint32_t payload_capacity) {
    StorePayload(addr.offset, buf, prefix_len,
                 std::max(prefix_len, payload_capacity), pool_owner);
    ctx_.stats().total_safein_payload_prefetched++;
}

void RerankConsumer::ConsumePayload(uint8_t* buf, AddressEntry addr,
                                    uint32_t payload_len,
                                    BufferPool* pool_owner) {
    StorePayload(addr.offset, buf, payload_len, payload_len, pool_owner);
    ctx_.stats().total_payload_prefetched++;
}

bool RerankConsumer::CachePayloadView(
    uint64_t offset, const uint8_t* payload, uint32_t payload_len) {
    if (payload == nullptr || payload_len == 0 ||
        payload_cache_.count(offset) > 0) {
        return false;
    }
    PayloadCacheEntry entry;
    entry.storage = const_cast<uint8_t*>(payload);
    entry.bytes = payload_len;
    entry.capacity = payload_len;
    entry.span_view = true;
    payload_cache_.emplace(offset, std::move(entry));
    return true;
}

bool RerankConsumer::HasPayload(uint64_t offset) const {
    return payload_cache_.count(offset) > 0;
}

uint32_t RerankConsumer::CachedPayloadBytes(uint64_t offset) const {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return 0;
    return it->second.bytes;
}

const uint8_t* RerankConsumer::CachedPayloadData(uint64_t offset) const {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return nullptr;
    return it->second.storage;
}

uint8_t* RerankConsumer::MutableCachedPayloadData(uint64_t offset) {
    auto it = payload_cache_.find(offset);
    return it == payload_cache_.end() ? nullptr : it->second.storage;
}

uint32_t RerankConsumer::CachedPayloadCapacity(uint64_t offset) const {
    auto it = payload_cache_.find(offset);
    return it == payload_cache_.end() ? 0 : it->second.capacity;
}

bool RerankConsumer::CachedPayloadIsSpanView(uint64_t offset) const {
    auto it = payload_cache_.find(offset);
    return it != payload_cache_.end() && it->second.span_view;
}

bool RerankConsumer::ExtendCachedPayload(uint64_t offset,
                                         uint32_t ready_bytes) {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end() || ready_bytes > it->second.capacity) {
        return false;
    }
    it->second.bytes = std::max(it->second.bytes, ready_bytes);
    return true;
}

AlignedBufPtr RerankConsumer::TakePayload(uint64_t offset) {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return nullptr;
    PayloadCacheEntry entry = std::move(it->second);
    payload_cache_.erase(it);
    if (!entry.span_view && entry.pool_owner == nullptr) {
        return AlignedBufPtr(entry.storage);
    }

    const uint32_t alloc_len =
        (std::max(entry.capacity, 1u) + 4095u) & ~4095u;
    uint8_t* copy = static_cast<uint8_t*>(std::aligned_alloc(4096, alloc_len));
    if (copy == nullptr) {
        std::fprintf(stderr,
                     "FATAL: aligned_alloc failed while detaching pooled payload "
                     "(%u bytes)\n",
                     alloc_len);
        std::abort();
    }
    if (entry.bytes > 0) {
        std::memcpy(copy, entry.storage, entry.bytes);
    }
    if (!entry.span_view) {
        entry.pool_owner->Release(entry.storage);
    }
    return AlignedBufPtr(copy);
}

void RerankConsumer::ReleasePayload(uint64_t offset) {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return;
    ReleasePayloadEntry(&it->second);
    payload_cache_.erase(it);
}

void RerankConsumer::CachePayload(uint64_t offset, AlignedBufPtr buf) {
    StorePayload(offset, buf.release(), 0, 0, nullptr);
}

void RerankConsumer::StorePayload(uint64_t offset, uint8_t* storage,
                                  uint32_t bytes, uint32_t capacity,
                                  BufferPool* pool_owner) {
    auto found = payload_cache_.find(offset);
    if (found != payload_cache_.end()) {
        ReleasePayloadEntry(&found->second);
        found->second = PayloadCacheEntry{
            storage, bytes, std::max(bytes, capacity), pool_owner, false};
        return;
    }
    payload_cache_.emplace(
        offset, PayloadCacheEntry{
            storage, bytes, std::max(bytes, capacity), pool_owner, false});
}

void RerankConsumer::ReleasePayloadEntry(PayloadCacheEntry* entry) {
    if (entry == nullptr || entry->storage == nullptr) return;
    if (!entry->span_view && entry->pool_owner != nullptr) {
        entry->pool_owner->Release(entry->storage);
    } else if (!entry->span_view) {
        std::free(entry->storage);
    }
    entry->storage = nullptr;
    entry->bytes = 0;
    entry->capacity = 0;
    entry->pool_owner = nullptr;
    entry->span_view = false;
}

void RerankConsumer::ClearPayloadCache() {
    for (auto& item : payload_cache_) {
        ReleasePayloadEntry(&item.second);
    }
    payload_cache_.clear();
}

void RerankConsumer::CleanupUnusedCache(
    const std::vector<CollectorEntry>& final_results) {
    std::unordered_set<uint64_t> needed;
    for (const auto& entry : final_results) {
        needed.insert(entry.addr.offset);
    }
    for (auto it = payload_cache_.begin(); it != payload_cache_.end();) {
        if (needed.count(it->first) == 0) {
            ReleasePayloadEntry(&it->second);
            it = payload_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void RerankConsumer::ExecuteBuffered() {
    auto collect_start = std::chrono::steady_clock::now();
    std::sort(buffered_candidates_.begin(), buffered_candidates_.end(),
              [](const BufferedCandidate& a, const BufferedCandidate& b) {
                  if (a.candidate_order != b.candidate_order) {
                      return a.candidate_order < b.candidate_order;
                  }
                  return a.addr.offset < b.addr.offset;
              });
    ctx_.stats().candidate_collect_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - collect_start).count();

    auto read_start = std::chrono::steady_clock::now();
    volatile uint64_t touch_bytes = 0;
    for (const auto& candidate : buffered_candidates_) {
        touch_bytes += candidate.addr.size;
        (void)touch_bytes;
    }
    ctx_.stats().pool_vector_read_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - read_start).count();

    auto compute_start = std::chrono::steady_clock::now();
    if (ctx_.config().enable_rerank_batched_distance_simd) {
        const uint32_t count = static_cast<uint32_t>(buffered_candidates_.size());
        constexpr uint32_t kBatchSize = 32;
        for (uint32_t base = 0; base < count; base += kBatchSize) {
            const uint32_t batch_count = std::min(kBatchSize, count - base);
            const float* vec_ptrs[kBatchSize] = {};
            float dists[kBatchSize] = {};
            for (uint32_t i = 0; i < batch_count; ++i) {
                vec_ptrs[i] = buffered_candidates_[base + i].vec;
            }
            simd::L2SqrBatch1xN(ctx_.query_vec(), vec_ptrs, batch_count, dim_, dists);
            for (uint32_t i = 0; i < batch_count; ++i) {
                ctx_.collector().TryInsert(dists[i], buffered_candidates_[base + i].addr);
                ctx_.stats().total_reranked++;
                ctx_.stats().reranked_candidates++;
            }
        }
    } else {
        for (const auto& candidate : buffered_candidates_) {
            float dist = L2Sqr(ctx_.query_vec(), candidate.vec, dim_);
            ctx_.collector().TryInsert(dist, candidate.addr);
            ctx_.stats().total_reranked++;
            ctx_.stats().reranked_candidates++;
        }
    }
    ctx_.stats().rerank_compute_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - compute_start).count();
    buffered_candidates_.clear();
    ResetVectorChunks();
}

uint32_t RerankConsumer::BufferedCount() const {
    return static_cast<uint32_t>(buffered_candidates_.size());
}

}  // namespace query
}  // namespace vdb
