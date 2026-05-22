#include "vdb/query/rerank_consumer.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "vdb/common/distance.h"
#include "vdb/simd/rerank_distance.h"

namespace vdb {
namespace query {

RerankConsumer::RerankConsumer(SearchContext& ctx, Dim raw_dim)
    : ctx_(ctx),
      dim_(raw_dim),
      vec_bytes_(raw_dim * sizeof(float)),
      aligned_vec_bytes_((vec_bytes_ + 4095u) & ~4095u) {
    buffered_candidates_.reserve(1024);
    GrowVectorChunk(aligned_vec_bytes_ * 1024u);
}

RerankConsumer::~RerankConsumer() = default;

bool RerankConsumer::GrowVectorChunk(uint32_t min_capacity) {
    const uint32_t chunk_capacity = std::max(aligned_vec_bytes_ * 1024u, min_capacity);
    auto alloc_start = std::chrono::steady_clock::now();
    uint8_t* raw = static_cast<uint8_t*>(
        std::aligned_alloc(4096, chunk_capacity));
    ctx_.stats().rerank_vec_alloc_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - alloc_start).count();
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
    auto copy_start = std::chrono::steady_clock::now();
    std::memcpy(dst, src, vec_bytes_);
    ctx_.stats().rerank_vec_copy_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - copy_start).count();
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
        BufferedCandidate{addr, AllocateVectorCopy(buf)});
    ctx_.stats().buffered_candidates++;
}

void RerankConsumer::ConsumeAll(uint8_t* buf, AddressEntry addr) {
    buffered_candidates_.push_back(
        BufferedCandidate{addr, AllocateVectorCopy(buf)});
    ctx_.stats().buffered_candidates++;

    if (addr.size > vec_bytes_) {
        // Copy payload portion to cache (aligned for consistency)
        uint32_t payload_len = addr.size - vec_bytes_;
        uint32_t alloc_len = (payload_len + 4095u) & ~4095u;
        uint8_t* pbuf = static_cast<uint8_t*>(std::aligned_alloc(4096, alloc_len));
        std::memcpy(pbuf, buf + vec_bytes_, payload_len);
        payload_cache_[addr.offset] = AlignedBufPtr(pbuf);
        ctx_.stats().total_safein_payload_prefetched++;
    }
}

void RerankConsumer::ConsumePayload(uint8_t* buf, AddressEntry addr) {
    payload_cache_[addr.offset] = AlignedBufPtr(buf);
    ctx_.stats().total_payload_prefetched++;
}

bool RerankConsumer::HasPayload(uint64_t offset) const {
    return payload_cache_.count(offset) > 0;
}

AlignedBufPtr RerankConsumer::TakePayload(uint64_t offset) {
    auto it = payload_cache_.find(offset);
    if (it == payload_cache_.end()) return nullptr;
    auto buf = std::move(it->second);
    payload_cache_.erase(it);
    return buf;
}

void RerankConsumer::CachePayload(uint64_t offset, AlignedBufPtr buf) {
    payload_cache_[offset] = std::move(buf);
}

void RerankConsumer::CleanupUnusedCache(
    const std::vector<CollectorEntry>& final_results) {
    std::unordered_set<uint64_t> needed;
    for (const auto& entry : final_results) {
        needed.insert(entry.addr.offset);
    }
    for (auto it = payload_cache_.begin(); it != payload_cache_.end();) {
        if (needed.count(it->first) == 0) {
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
