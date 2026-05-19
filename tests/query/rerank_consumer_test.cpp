#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <vector>

#include "vdb/query/rerank_consumer.h"

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

    EXPECT_EQ(ctx_->collector().Size(), 1u);
    EXPECT_EQ(ctx_->stats().total_reranked, 1u);
}

TEST_F(RerankConsumerTest, ConsumeAll_CachesPayload) {
    float vec[] = {2.0f, 0.0f, 0.0f, 0.0f};
    uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
    auto buf = MakeAllBuf(vec, payload, 4);
    AddressEntry addr{200, kVecBytes + 4};

    consumer_->ConsumeAll(buf.get(), addr);

    EXPECT_EQ(ctx_->collector().Size(), 1u);
    EXPECT_TRUE(consumer_->HasPayload(200));

    auto cached = consumer_->TakePayload(200);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(cached[0], 0xAA);
    EXPECT_EQ(cached[3], 0xDD);
}

TEST_F(RerankConsumerTest, ConsumePayload_TransfersOwnership) {
    auto buf = std::make_unique<uint8_t[]>(8);
    buf[0] = 0x42;
    uint8_t* raw = buf.get();
    AddressEntry addr{300, 24};

    // ConsumePayload takes ownership — caller must NOT free
    consumer_->ConsumePayload(buf.release(), addr);

    EXPECT_TRUE(consumer_->HasPayload(300));
    auto taken = consumer_->TakePayload(300);
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(taken[0], 0x42);
    EXPECT_FALSE(consumer_->HasPayload(300));
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
