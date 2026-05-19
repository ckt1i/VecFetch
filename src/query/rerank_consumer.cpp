#include "vdb/query/rerank_consumer.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_set>

#include "vdb/common/distance.h"
#include "vdb/simd/rerank_distance.h"

namespace vdb {
namespace query {

RerankConsumer::RerankConsumer(SearchContext& ctx, Dim raw_dim)
    : ctx_(ctx), dim_(raw_dim), vec_bytes_(raw_dim * sizeof(float)) {}

RerankConsumer::~RerankConsumer() = default;

void RerankConsumer::ConsumeVec(uint8_t* buf, AddressEntry addr) {
    uint32_t aligned_vec_bytes = (vec_bytes_ + 4095u) & ~4095u;
    uint8_t* vec_copy = static_cast<uint8_t*>(
        std::aligned_alloc(4096, aligned_vec_bytes));
    std::memcpy(vec_copy, buf, vec_bytes_);
    buffered_candidates_.push_back(
        BufferedCandidate{addr, AlignedBufPtr(vec_copy)});
    ctx_.stats().buffered_candidates++;
}

void RerankConsumer::ConsumeAll(uint8_t* buf, AddressEntry addr) {
    const float* vec = reinterpret_cast<const float*>(buf);
    uint32_t aligned_vec_bytes = (vec_bytes_ + 4095u) & ~4095u;
    uint8_t* vec_copy = static_cast<uint8_t*>(
        std::aligned_alloc(4096, aligned_vec_bytes));
    std::memcpy(vec_copy, vec, vec_bytes_);
    buffered_candidates_.push_back(
        BufferedCandidate{addr, AlignedBufPtr(vec_copy)});
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
                vec_ptrs[i] = reinterpret_cast<const float*>(
                    buffered_candidates_[base + i].vec_buf.get());
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
            const float* vec = reinterpret_cast<const float*>(candidate.vec_buf.get());
            float dist = L2Sqr(ctx_.query_vec(), vec, dim_);
            ctx_.collector().TryInsert(dist, candidate.addr);
            ctx_.stats().total_reranked++;
            ctx_.stats().reranked_candidates++;
        }
    }
    ctx_.stats().rerank_compute_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - compute_start).count();
    buffered_candidates_.clear();
}

uint32_t RerankConsumer::BufferedCount() const {
    return static_cast<uint32_t>(buffered_candidates_.size());
}

}  // namespace query
}  // namespace vdb
