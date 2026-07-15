#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <vector>

#include "vdb/query/rerank_consumer.h"
#include "vdb/storage/hot_record.h"

using namespace vdb;
using namespace vdb::query;

class RerankConsumerTest : public ::testing::Test {
 protected:
    static constexpr Dim kDim = 4;
    static constexpr uint32_t kVecBytes = kDim * sizeof(float);

    void SetUp() override {
        // Query vector: [1, 0, 0, 0]
        query_ = {1.0f, 0.0f, 0.0f, 0.0f};
        config_.top_k = 3;
        ctx_ = std::make_unique<SearchContext>(query_.data(), config_);
        consumer_ = std::make_unique<RerankConsumer>(*ctx_, kDim);
    }

    // Create a VEC_ONLY buffer from a float vector
    AlignedBufPtr MakeVecBuf(const float* vec) {
        auto* raw = static_cast<uint8_t*>(std::aligned_alloc(4096, 4096));
        std::memcpy(raw, vec, kVecBytes);
        return AlignedBufPtr(raw);
    }

    // Create an ALL buffer (vec + payload bytes)
    AlignedBufPtr MakeAllBuf(const float* vec,
                              const uint8_t* payload,
                              uint32_t payload_len) {
        auto* raw = static_cast<uint8_t*>(std::aligned_alloc(4096, 4096));
        std::memcpy(raw, vec, kVecBytes);
        std::memcpy(raw + kVecBytes, payload, payload_len);
        return AlignedBufPtr(raw);
    }

    AlignedBufPtr MakeInlineHotRecordBuf(
        const float* vec,
        const vdb::storage::HotPayloadDescriptor& desc,
        const uint8_t* payload = nullptr,
        uint32_t payload_len = 0) {
        auto* raw = static_cast<uint8_t*>(std::aligned_alloc(4096, 4096));
        std::memcpy(raw, vec, kVecBytes);
        vdb::storage::EncodeHotPayloadDescriptor(desc, raw + kVecBytes);
        if (payload != nullptr && payload_len > 0) {
            std::memcpy(raw + kVecBytes + sizeof(desc), payload, payload_len);
        }
        return AlignedBufPtr(raw);
    }

    void ResetInlineConsumer() {
        config_.inline_hot_record_store.enabled = true;
        config_.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        ctx_ = std::make_unique<SearchContext>(query_.data(), config_);
        consumer_ = std::make_unique<RerankConsumer>(*ctx_, kDim);
    }

    std::vector<float> query_;
    SearchConfig config_;
    std::unique_ptr<SearchContext> ctx_;
    std::unique_ptr<RerankConsumer> consumer_;
};

TEST_F(RerankConsumerTest, ConsumeVec_InsertsToCollector) {
    // vec = [2, 0, 0, 0] → L2Sqr = (2-1)^2 = 1.0
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    auto buf = MakeVecBuf(vec);
    AddressEntry addr{100, 16};

    consumer_->ConsumeVec(buf.get(), addr);
    EXPECT_EQ(consumer_->BufferedCount(), 1u);
    consumer_->ExecuteBuffered();

    EXPECT_EQ(ctx_->collector().Size(), 1u);
    EXPECT_EQ(ctx_->stats().total_reranked, 1u);
    EXPECT_EQ(ctx_->stats().rerank_vec_copy_ms, 0.0);
}

TEST_F(RerankConsumerTest, ConsumeAll_CachesPayload) {
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
    auto buf = MakeAllBuf(vec, payload, 4);
    AddressEntry addr{200, kVecBytes + 4};

    consumer_->ConsumeAll(buf.get(), addr, addr.size);
    EXPECT_EQ(consumer_->BufferedCount(), 1u);
    consumer_->ExecuteBuffered();

    EXPECT_EQ(ctx_->collector().Size(), 1u);
    EXPECT_TRUE(consumer_->HasPayload(200));

    auto cached = consumer_->TakePayload(200);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached[0], 0xAA);
    EXPECT_EQ(cached[3], 0xDD);
}

TEST_F(RerankConsumerTest, ConsumeAll_CachesOnlyCoveredPayloadPrefix) {
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
    auto buf = MakeAllBuf(vec, payload, 4);
    AddressEntry addr{250, kVecBytes + 4};

    consumer_->ConsumeAll(buf.get(), addr, kVecBytes + 2);

    EXPECT_TRUE(consumer_->HasPayload(250));
    EXPECT_EQ(consumer_->CachedPayloadBytes(250), 2u);
    const uint8_t* cached = consumer_->CachedPayloadData(250);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached[0], 0xAA);
    EXPECT_EQ(cached[1], 0xBB);
}

TEST_F(RerankConsumerTest, HotPayloadDescriptorValidationRejectsBadLayouts) {
    using vdb::storage::HotPayloadDescriptor;
    using vdb::storage::HotPayloadStorageType;
    using vdb::storage::ValidateHotPayloadDescriptor;

    HotPayloadDescriptor inline_desc;
    inline_desc.payload_storage_type =
        static_cast<uint8_t>(HotPayloadStorageType::kInlinePayload);
    inline_desc.inline_bytes = 4;
    inline_desc.payload_bytes = 4;
    EXPECT_TRUE(ValidateHotPayloadDescriptor(inline_desc).ok());

    HotPayloadDescriptor unknown = inline_desc;
    unknown.payload_storage_type = 99;
    EXPECT_FALSE(ValidateHotPayloadDescriptor(unknown).ok());

    HotPayloadDescriptor prefix = inline_desc;
    prefix.payload_storage_type =
        static_cast<uint8_t>(HotPayloadStorageType::kPrefixColdPointer);
    prefix.inline_bytes = 2;
    prefix.payload_bytes = 4;
    prefix.payload_offset = 128;
    EXPECT_TRUE(ValidateHotPayloadDescriptor(prefix).ok());

    HotPayloadDescriptor bad_prefix = prefix;
    bad_prefix.inline_bytes = bad_prefix.payload_bytes;
    EXPECT_FALSE(ValidateHotPayloadDescriptor(bad_prefix).ok());

    HotPayloadDescriptor bad_inline = inline_desc;
    bad_inline.payload_offset = 128;
    EXPECT_FALSE(ValidateHotPayloadDescriptor(bad_inline).ok());
}

TEST_F(RerankConsumerTest, ConsumeAll_InlinePayloadCachesLogicalPayloadOnly) {
    using vdb::storage::HotPayloadDescriptor;
    using vdb::storage::HotPayloadStorageType;

    ResetInlineConsumer();
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};
    HotPayloadDescriptor desc;
    desc.payload_storage_type =
        static_cast<uint8_t>(HotPayloadStorageType::kInlinePayload);
    desc.inline_bytes = sizeof(payload);
    desc.payload_bytes = sizeof(payload);
    auto buf = MakeInlineHotRecordBuf(vec, desc, payload, sizeof(payload));
    AddressEntry addr{400, kVecBytes + sizeof(desc) + sizeof(payload)};

    consumer_->ConsumeAll(buf.get(), addr, addr.size);

    EXPECT_TRUE(consumer_->HasPayload(addr.offset));
    EXPECT_EQ(consumer_->CachedPayloadBytes(addr.offset), sizeof(payload));
    const uint8_t* cached = consumer_->CachedPayloadData(addr.offset);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached[0], 0x10);
    EXPECT_EQ(cached[3], 0x40);
    EXPECT_EQ(ctx_->stats().total_safein_payload_prefetched, 1u);
    EXPECT_EQ(ctx_->stats().inline_cold_payload_deferred, 0u);
    EXPECT_EQ(ctx_->stats().inline_descriptor_errors, 0u);
}

TEST_F(RerankConsumerTest, ConsumeAll_InlineColdPointerDefersPayload) {
    using vdb::storage::HotPayloadDescriptor;
    using vdb::storage::HotPayloadStorageType;

    ResetInlineConsumer();
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    HotPayloadDescriptor desc;
    desc.payload_storage_type =
        static_cast<uint8_t>(HotPayloadStorageType::kColdPointer);
    desc.inline_bytes = 0;
    desc.payload_offset = 4096;
    desc.payload_bytes = 64;
    auto buf = MakeInlineHotRecordBuf(vec, desc);
    AddressEntry addr{500, kVecBytes + sizeof(desc)};

    consumer_->ConsumeAll(buf.get(), addr, addr.size);

    EXPECT_FALSE(consumer_->HasPayload(addr.offset));
    EXPECT_EQ(ctx_->stats().inline_cold_payload_deferred, 1u);
    EXPECT_EQ(ctx_->stats().total_safein_payload_prefetched, 0u);
    EXPECT_EQ(ctx_->stats().inline_descriptor_errors, 0u);
}

TEST_F(RerankConsumerTest, ConsumeAll_PrefixColdCachesOnlyInlinePrefix) {
    using vdb::storage::HotPayloadDescriptor;
    using vdb::storage::HotPayloadStorageType;

    ResetInlineConsumer();
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    uint8_t prefix[] = {0x31, 0x32, 0x33};
    HotPayloadDescriptor desc;
    desc.payload_storage_type =
        static_cast<uint8_t>(HotPayloadStorageType::kPrefixColdPointer);
    desc.inline_bytes = sizeof(prefix);
    desc.payload_offset = 8192;
    desc.payload_bytes = 10;
    auto buf = MakeInlineHotRecordBuf(vec, desc, prefix, sizeof(prefix));
    AddressEntry addr{550, kVecBytes + sizeof(desc) + sizeof(prefix)};

    consumer_->ConsumeAll(buf.get(), addr, addr.size);

    EXPECT_TRUE(consumer_->HasPayload(addr.offset));
    EXPECT_EQ(consumer_->CachedPayloadBytes(addr.offset), sizeof(prefix));
    EXPECT_EQ(consumer_->CachedPayloadData(addr.offset)[2], 0x33);
    EXPECT_EQ(ctx_->stats().total_safein_payload_prefetched, 1u);
    EXPECT_EQ(ctx_->stats().inline_descriptor_errors, 0u);
}

TEST_F(RerankConsumerTest, ConsumeAll_InlineUnknownDescriptorDoesNotCache) {
    using vdb::storage::HotPayloadDescriptor;

    ResetInlineConsumer();
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    HotPayloadDescriptor desc;
    desc.payload_storage_type = 99;
    desc.inline_bytes = 4;
    desc.payload_bytes = 4;
    auto buf = MakeInlineHotRecordBuf(vec, desc);
    AddressEntry addr{600, kVecBytes + sizeof(desc)};

    consumer_->ConsumeAll(buf.get(), addr, addr.size);

    EXPECT_FALSE(consumer_->HasPayload(addr.offset));
    EXPECT_EQ(ctx_->stats().inline_descriptor_errors, 1u);
}

TEST_F(RerankConsumerTest, ConsumePayloadPrefix_TracksPrefixLength) {
    auto* raw = static_cast<uint8_t*>(std::aligned_alloc(4096, 4096));
    auto buf = AlignedBufPtr(raw);
    buf[0] = 0x11;
    buf[1] = 0x22;
    AddressEntry addr{275, kVecBytes + 8};

    consumer_->ConsumePayloadPrefix(buf.release(), addr, 2);

    EXPECT_TRUE(consumer_->HasPayload(275));
    EXPECT_EQ(consumer_->CachedPayloadBytes(275), 2u);
    const uint8_t* cached = consumer_->CachedPayloadData(275);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached[0], 0x11);
    EXPECT_EQ(cached[1], 0x22);
}

TEST_F(RerankConsumerTest, ConsumePayload_TransfersOwnership) {
    auto* raw = static_cast<uint8_t*>(std::aligned_alloc(4096, 4096));
    auto buf = AlignedBufPtr(raw);
    buf[0] = 0x42;
    AddressEntry addr{300, 24};

    // ConsumePayload takes ownership — caller must NOT free
    consumer_->ConsumePayload(buf.release(), addr, 8);

    EXPECT_TRUE(consumer_->HasPayload(300));
    auto taken = consumer_->TakePayload(300);
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(taken[0], 0x42);
    EXPECT_FALSE(consumer_->HasPayload(300));
}

TEST_F(RerankConsumerTest, PoolBackedPayloadReturnsBufferAfterParse) {
    BufferPool pool;
    uint8_t* raw = pool.Acquire(128);
    raw[0] = 0x5A;
    AddressEntry addr{350, 128};

    consumer_->ConsumePayload(raw, addr, 128, &pool);
    EXPECT_EQ(pool.OutstandingCount(), 1u);
    ASSERT_NE(consumer_->CachedPayloadData(addr.offset), nullptr);
    EXPECT_EQ(consumer_->CachedPayloadData(addr.offset)[0], 0x5A);

    consumer_->ReleasePayload(addr.offset);
    EXPECT_FALSE(consumer_->HasPayload(addr.offset));
    EXPECT_EQ(pool.OutstandingCount(), 0u);
    EXPECT_EQ(pool.PoolSize(), 1u);
}

TEST_F(RerankConsumerTest, ReplacingPoolBackedPayloadReleasesOldBuffer) {
    BufferPool pool;
    AddressEntry addr{360, 128};
    uint8_t* first = pool.Acquire(128);
    uint8_t* second = pool.Acquire(256);

    consumer_->ConsumePayload(first, addr, 128, &pool);
    consumer_->ConsumePayload(second, addr, 256, &pool);

    EXPECT_EQ(pool.OutstandingCount(), 1u);
    EXPECT_EQ(pool.PoolSize(), 1u);
    consumer_->ReleasePayload(addr.offset);
    EXPECT_EQ(pool.OutstandingCount(), 0u);
    EXPECT_EQ(pool.PoolSize(), 2u);
}

TEST_F(RerankConsumerTest, CleanupUnusedCache) {
    // Insert some payloads
    auto buf1 = AlignedBufPtr(static_cast<uint8_t*>(std::aligned_alloc(4096, 4096)));
    auto buf2 = AlignedBufPtr(static_cast<uint8_t*>(std::aligned_alloc(4096, 4096)));
    consumer_->CachePayload(100, std::move(buf1));
    consumer_->CachePayload(200, std::move(buf2));

    // Only offset=100 is in final results
    std::vector<CollectorEntry> results;
    results.push_back({1.0f, {100, 20}});

    consumer_->CleanupUnusedCache(results);

    EXPECT_TRUE(consumer_->HasPayload(100));
    EXPECT_FALSE(consumer_->HasPayload(200));
}

TEST_F(RerankConsumerTest, VectorSlabResetsAcrossExecuteBuffered) {
    float vec1[] = {2.0f, 0.0f, 0.0f, 0.0f};
    float vec2[] = {0.5f, 0.0f, 0.0f, 0.0f};
    auto buf1 = MakeVecBuf(vec1);
    auto buf2 = MakeVecBuf(vec2);

    consumer_->ConsumeVec(buf1.get(), AddressEntry{100, kVecBytes});
    consumer_->ConsumeVec(buf2.get(), AddressEntry{200, kVecBytes});
    EXPECT_EQ(consumer_->BufferedCount(), 2u);

    consumer_->ExecuteBuffered();
    EXPECT_EQ(consumer_->BufferedCount(), 0u);
    EXPECT_EQ(ctx_->collector().Size(), 2u);

    auto buf3 = MakeVecBuf(vec1);
    consumer_->ConsumeVec(buf3.get(), AddressEntry{300, kVecBytes});
    consumer_->ExecuteBuffered();

    EXPECT_EQ(ctx_->stats().total_reranked, 3u);
    EXPECT_EQ(ctx_->stats().rerank_vec_alloc_ms, 0.0);
    EXPECT_EQ(ctx_->stats().rerank_vec_copy_ms, 0.0);
}

TEST_F(RerankConsumerTest, CandidateOrderBreaksExactDistanceTiesAcrossLayouts) {
    config_.top_k = 1;
    ctx_ = std::make_unique<SearchContext>(query_.data(), config_);
    std::unordered_map<uint64_t, uint64_t> candidate_order = {
        {100, 1},
        {200, 0},
    };
    consumer_ = std::make_unique<RerankConsumer>(
        *ctx_, kDim, &candidate_order);

    float tied_vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    auto later = MakeVecBuf(tied_vec);
    auto earlier = MakeVecBuf(tied_vec);
    consumer_->ConsumeVec(later.get(), AddressEntry{100, kVecBytes});
    consumer_->ConsumeVec(earlier.get(), AddressEntry{200, kVecBytes});

    consumer_->ExecuteBuffered();
    const auto results = ctx_->collector().Finalize();

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().addr.offset, 200u);
}

TEST_F(RerankConsumerTest, DetailedHotpathTiming_PopulatesVectorTimingStats) {
    config_.enable_hotpath_detailed_timing = true;
    ctx_ = std::make_unique<SearchContext>(query_.data(), config_);
    consumer_ = std::make_unique<RerankConsumer>(*ctx_, kDim);

    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    auto buf = MakeVecBuf(vec);

    consumer_->ConsumeVec(buf.get(), AddressEntry{123, kVecBytes});
    consumer_->ExecuteBuffered();

    EXPECT_GE(ctx_->stats().rerank_vec_alloc_ms, 0.0);
    EXPECT_GE(ctx_->stats().rerank_vec_copy_ms, 0.0);
}
